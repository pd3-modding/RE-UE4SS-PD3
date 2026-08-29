#include <MCP/MCPServer.hpp>

#ifdef UE4SS_ENABLE_MCP

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>
#include <Mod/LuaMod.hpp>
#include <UE4SSProgram.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UnrealInitializer.hpp>

#include <mcp_bind.h>

namespace RC::MCP
{
    namespace
    {
        // ------------------------------------------------------------------------------
        // Request marshalling
        //
        // A tool call arrives on a tokio worker thread. Anything that touches live UObjects
        // has to run on the game thread, so lua_eval parks a request here and blocks until
        // the engine tick hook has run it -- or until the timeout expires, because a game
        // thread stuck in a map load must not hang the HTTP request forever.
        // ------------------------------------------------------------------------------
        struct PendingEval
        {
            std::string code{};
            std::string output{};
            bool ok{false};
            bool done{false};
            // Set when the requester gave up. The game thread skips these rather than running
            // stale code minutes later, but the shared_ptr keeps the object alive either way,
            // so a timeout can never leave the game thread writing to freed memory.
            bool abandoned{false};
        };

        std::mutex g_queue_mutex;
        std::condition_variable g_queue_cv;
        std::deque<std::shared_ptr<PendingEval>> g_queue;
        bool g_shutting_down{false};

        std::mutex g_activity_mutex;
        std::deque<ActivityRecord> g_activity;
        constexpr size_t max_activity_records = 200;

        std::mutex g_status_mutex;
        File::StringType g_status_line{STR("not started")};

        bool g_running{false};
        bool g_tick_hook_registered{false};

        auto set_status(File::StringType status) -> void
        {
            std::lock_guard guard(g_status_mutex);
            g_status_line = std::move(status);
        }

        auto push_activity(ActivityRecord::Kind kind, File::StringType text, bool ok) -> void
        {
            std::lock_guard guard(g_activity_mutex);
            g_activity.push_back(ActivityRecord{std::chrono::system_clock::now(), kind, std::move(text), ok});
            while (g_activity.size() > max_activity_records)
            {
                g_activity.pop_front();
            }
        }

        // ------------------------------------------------------------------------------
        // String marshalling
        //
        // Rust copies the contents and then calls free_string on the same struct, so these
        // two functions must stay a matched pair. See include/mcp_bind.h.
        // ------------------------------------------------------------------------------
        auto make_string(std::string_view utf8, McpString* out) -> void
        {
            if (!out)
            {
                return;
            }
            auto wide = ensure_str(std::string{utf8});
            auto* buffer = new McpChar[wide.size() + 1];
            std::memcpy(buffer, wide.data(), wide.size() * sizeof(McpChar));
            buffer[wide.size()] = 0;
            out->data = buffer;
            out->len = wide.size();
        }

        extern "C" void free_string_cb(void*, McpString* s)
        {
            if (!s || !s->data)
            {
                return;
            }
            delete[] s->data;
            s->data = nullptr;
            s->len = 0;
        }

        // ------------------------------------------------------------------------------
        // Host callbacks. Every one of these runs on a tokio worker, never the game thread.
        // ------------------------------------------------------------------------------

        extern "C" void log_cb(void*, const McpChar* msg)
        {
            if (!msg)
            {
                return;
            }
            auto text = File::StringType{msg};
            // Requested explicitly: MCP activity belongs in the main log alongside everything
            // else, not only in the GUI.
            Output::send(STR("{}\n"), text);
            push_activity(ActivityRecord::Kind::Lifecycle, text, true);
        }

        extern "C" void on_tool_call_cb(void*, const McpChar* tool, const McpChar* args_json, bool ok)
        {
            auto tool_name = tool ? File::StringType{tool} : File::StringType{STR("<unknown>")};
            auto args = args_json ? File::StringType{args_json} : File::StringType{};

            // Arguments can be a whole Lua script; keep the log readable.
            constexpr size_t max_logged_args = 240;
            if (args.size() > max_logged_args)
            {
                args.resize(max_logged_args);
                args.append(STR("..."));
            }
            std::replace(args.begin(), args.end(), STR('\n'), STR(' '));
            std::replace(args.begin(), args.end(), STR('\r'), STR(' '));

            auto text = fmt::format(STR("[MCP] tool {} {} {}"), tool_name, ok ? STR("ok") : STR("FAILED"), args);
            Output::send(STR("{}\n"), text);
            push_activity(ActivityRecord::Kind::ToolCall, std::move(text), ok);
        }

        extern "C" bool lua_eval_cb(void*, const McpChar* code, McpString* out)
        {
            auto request = std::make_shared<PendingEval>();
            request->code = code ? to_string(File::StringType{code}) : std::string{};

            // Clamp: a zero or negative ini value would otherwise mean "give up instantly".
            const auto timeout = std::chrono::milliseconds{std::max<int64_t>(1000, UE4SSProgram::settings_manager.MCP.GameThreadTimeoutMs)};

            std::unique_lock lock(g_queue_mutex);
            if (g_shutting_down)
            {
                lock.unlock();
                make_string("MCP server is shutting down", out);
                return false;
            }
            g_queue.push_back(request);

            if (!g_queue_cv.wait_for(lock, timeout, [&] {
                    return request->done || g_shutting_down;
                }))
            {
                request->abandoned = true;
                lock.unlock();
                make_string(fmt::format("Timed out after {}ms waiting for the game thread. The game may be loading, "
                                        "paused at a breakpoint, or not ticking yet.",
                                        timeout.count()),
                            out);
                return false;
            }

            if (!request->done)
            {
                request->abandoned = true;
                lock.unlock();
                make_string("MCP server stopped before the request could run", out);
                return false;
            }

            auto output = std::move(request->output);
            const bool ok = request->ok;
            lock.unlock();

            make_string(output, out);
            return ok;
        }

        // Deliberately does NOT go through the game thread: this is the tool you reach for
        // when the game thread is wedged, so it must still answer then. Everything it reports
        // is loader state, not UObject state -- ask lua_eval for anything about the world.
        extern "C" bool game_status_cb(void*, McpString* out)
        {
            auto& program = UE4SSProgram::get_program();

            std::string mods_json{};
            bool first = true;
            for (const auto& [mod_name, is_enabled] : program.get_mods_txt_entries())
            {
                if (!first)
                {
                    mods_json.append(",");
                }
                first = false;
                mods_json.append(fmt::format(R"({{"name":"{}","enabled":{}}})", mod_name, is_enabled ? "true" : "false"));
            }

            size_t pending{};
            {
                std::lock_guard guard(g_queue_mutex);
                pending = g_queue.size();
            }

            auto json = fmt::format(R"({{"ue4ss_version":"{}.{}.{}","configuration":"{}","git_sha":"{}",)"
                                    R"("unreal_initialized":{},"program_started":{},"pending_tool_requests":{},)"
                                    R"("game_thread_timeout_ms":{},"mods":[{}]}})",
                                    UE4SS_LIB_VERSION_MAJOR,
                                    UE4SS_LIB_VERSION_MINOR,
                                    UE4SS_LIB_VERSION_HOTFIX,
                                    UE4SS_CONFIGURATION,
                                    UE4SS_LIB_BUILD_GITSHA,
                                    Unreal::UnrealInitializer::StaticStorage::bIsInitialized ? "true" : "false",
                                    program.is_program_started() ? "true" : "false",
                                    pending,
                                    UE4SSProgram::settings_manager.MCP.GameThreadTimeoutMs,
                                    mods_json);

            make_string(json, out);
            return true;
        }

        extern "C" bool log_tail_cb(void*, uint32_t lines, const McpChar* filter, McpString* out)
        {
            const auto log_path = std::filesystem::path{UE4SSProgram::get_program().get_working_directory()} / STR("UE4SS.log");

            std::ifstream file{log_path};
            if (!file.is_open())
            {
                make_string(fmt::format("Could not open '{}' for reading.", log_path.string()), out);
                return false;
            }

            const auto needle = filter ? to_string(File::StringType{filter}) : std::string{};
            const size_t wanted = lines == 0 ? 50 : lines;

            // Keep only the tail: the log grows without bound across a session and there is no
            // reason to hold all of it in memory to return the last few dozen lines.
            std::deque<std::string> tail;
            std::string line;
            while (std::getline(file, line))
            {
                if (!needle.empty() && line.find(needle) == std::string::npos)
                {
                    continue;
                }
                tail.push_back(line);
                if (tail.size() > wanted)
                {
                    tail.pop_front();
                }
            }

            std::string result;
            for (const auto& entry : tail)
            {
                result.append(entry).append("\n");
            }
            if (result.empty())
            {
                result = needle.empty() ? "<log is empty>" : fmt::format("<no lines matching '{}'>", needle);
            }

            make_string(result, out);
            return true;
        }

        auto make_host() -> McpHost
        {
            McpHost host{};
            host.ctx = nullptr;
            host.log = &log_cb;
            host.lua_eval = &lua_eval_cb;
            host.game_status = &game_status_cb;
            host.log_tail = &log_tail_cb;
            host.on_tool_call = &on_tool_call_cb;
            host.free_string = &free_string_cb;
            return host;
        }
    } // namespace

    auto MCPServer::is_compiled_in() -> bool
    {
        return true;
    }

    auto MCPServer::start() -> void
    {
        auto& settings = UE4SSProgram::settings_manager.MCP;
        if (!settings.Enabled)
        {
            set_status(STR("disabled in UE4SS-settings.ini"));
            return;
        }

        if (g_running)
        {
            return;
        }

        if (settings.Port <= 0 || settings.Port > 65535)
        {
            Output::send<LogLevel::Error>(STR("[MCP] invalid port {}; not starting\n"), settings.Port);
            set_status(STR("invalid port"));
            return;
        }

        // lua_eval is drained from the engine tick. Without that hook the queue is never
        // serviced and every call would sit there until it timed out, so say so plainly now
        // rather than letting it look like the server is broken.
        if (!UE4SSProgram::settings_manager.Hooks.HookEngineTick)
        {
            Output::send<LogLevel::Warning>(STR("[MCP] Hooks.HookEngineTick is disabled; lua_eval will time out. ")
                                            STR("Enable it in UE4SS-settings.ini to use the MCP server.\n"));
        }
        else if (!g_tick_hook_registered)
        {
            Unreal::Hook::RegisterEngineTickPreCallback(
                    [](Unreal::Hook::TCallbackIterationData<void>&, Unreal::UEngine*, float, bool) -> void {
                        MCPServer::process_game_thread_queue();
                    },
                    {false, false, STR("UE4SS"), STR("MCPServer")});
            g_tick_hook_registered = true;
        }

        {
            std::lock_guard guard(g_queue_mutex);
            g_shutting_down = false;
        }

        auto host = make_host();
        McpConfig config{};
        config.bind_address = settings.BindAddress.c_str();
        config.port = static_cast<uint16_t>(settings.Port);

        if (!mcp_start(&config, &host))
        {
            set_status(STR("failed to start (see log)"));
            return;
        }

        g_running = true;
        set_status(fmt::format(STR("listening on http://{}:{}/mcp"), settings.BindAddress, settings.Port));
    }

    auto MCPServer::stop() -> void
    {
        if (!g_running)
        {
            return;
        }

        // Release anyone blocked on the game thread before tearing the runtime down, so
        // mcp_stop is not waiting on requests that can never complete.
        {
            std::lock_guard guard(g_queue_mutex);
            g_shutting_down = true;
            for (auto& request : g_queue)
            {
                request->abandoned = true;
            }
            g_queue.clear();
        }
        g_queue_cv.notify_all();

        mcp_stop();
        g_running = false;
        set_status(STR("stopped"));
    }

    auto MCPServer::is_running() -> bool
    {
        return g_running && mcp_is_running();
    }

    auto MCPServer::process_game_thread_queue() -> void
    {
        std::deque<std::shared_ptr<PendingEval>> batch;
        {
            std::lock_guard guard(g_queue_mutex);
            if (g_queue.empty())
            {
                return;
            }
            batch.swap(g_queue);
        }

        for (auto& request : batch)
        {
            // The requester timed out and is no longer waiting. Running its code now, possibly
            // long after it was asked for, is worse than dropping it.
            {
                std::lock_guard guard(g_queue_mutex);
                if (request->abandoned)
                {
                    continue;
                }
            }

            std::string output;
            bool ok = false;
            try
            {
                ok = LuaMod::mcp_eval(request->code, output);
            }
            catch (std::exception& e)
            {
                output = fmt::format("Uncaught exception while evaluating: {}", e.what());
                ok = false;
            }

            {
                std::lock_guard guard(g_queue_mutex);
                request->output = std::move(output);
                request->ok = ok;
                request->done = true;
            }
        }

        g_queue_cv.notify_all();
    }

    auto MCPServer::recent_activity() -> std::vector<ActivityRecord>
    {
        std::lock_guard guard(g_activity_mutex);
        return std::vector<ActivityRecord>{g_activity.begin(), g_activity.end()};
    }

    auto MCPServer::clear_activity() -> void
    {
        std::lock_guard guard(g_activity_mutex);
        g_activity.clear();
    }

    auto MCPServer::status_line() -> File::StringType
    {
        std::lock_guard guard(g_status_mutex);
        return g_status_line;
    }
} // namespace RC::MCP

#else // UE4SS_ENABLE_MCP

namespace RC::MCP
{
    // Stubs so callers never need to be compiled conditionally.
    auto MCPServer::is_compiled_in() -> bool
    {
        return false;
    }
    auto MCPServer::start() -> void
    {
    }
    auto MCPServer::stop() -> void
    {
    }
    auto MCPServer::is_running() -> bool
    {
        return false;
    }
    auto MCPServer::process_game_thread_queue() -> void
    {
    }
    auto MCPServer::recent_activity() -> std::vector<ActivityRecord>
    {
        return {};
    }
    auto MCPServer::clear_activity() -> void
    {
    }
    auto MCPServer::status_line() -> File::StringType
    {
        return STR("not built with MCP support (configure with -DUE4SS_ENABLE_MCP=ON)");
    }
} // namespace RC::MCP

#endif // UE4SS_ENABLE_MCP
