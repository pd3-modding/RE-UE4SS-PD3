#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>

#include <Common.hpp>
#include <File/File.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <Mod/Mod.hpp>
#include <SettingsManager.hpp>

#include <String/StringType.hpp>

#include <Unreal/NameTypes.hpp>

namespace RC
{
    class UE4SSProgram;

    namespace Unreal
    {
        class UClass;
    }

    RC_UE4SS_API auto get_mod_ref(const LuaMadeSimple::Lua& lua) -> class LuaMod*;

    class LuaMod : public Mod
    {
      private:
        std::filesystem::path m_scripts_path;
        LuaMadeSimple::Lua& m_lua;

      public:
        LuaMadeSimple::Lua* m_hook_lua = nullptr;
        LuaMadeSimple::Lua* m_main_lua{};
        LuaMadeSimple::Lua* m_async_lua{};
        // Set as the very first thing uninstall() does. The action drains check it per action
        // and skip any action owned by this mod, because they run their swapped-out lists
        // lock-free where uninstall's erase cannot reach them (see SimpleLuaAction.mod).
        // SHARED, not a plain member: the LuaMod object is deleted when its uninstall
        // completes, and an action still holding a raw pointer into it would read FREED
        // memory in the drain (garbage false -> the dead action runs on the closed state --
        // the 2026-09-06 11:31 crash, luaH_getint inside add_metamethods's __index lambda).
        std::shared_ptr<std::atomic<bool>> m_unload_started{std::make_shared<std::atomic<bool>>(false)};

      public:
        enum class ActionType
        {
            Immediate,
            Delayed,
            Loop
        };

        struct SimpleLuaAction
        {
            const LuaMadeSimple::Lua* lua;
            int32_t lua_action_function_ref{};
            int32_t lua_action_thread_ref{};
            // OWNING MOD'S UNLOAD TOKEN. The drain runs its swapped-out list lock-free, so
            // when a reload uninstalls mods mid-list it cannot see these from uninstall's
            // erase; the drain skips any action whose token is set instead of touching a
            // closed Lua state. Shared so it stays readable after the LuaMod is deleted.
            std::shared_ptr<std::atomic<bool>> unload_token{};
        };

        // Status of a delayed action (mirrors UE's ETimerStatus)
        enum class DelayedActionStatus : uint8_t
        {
            Pending,        // Created but not yet started
            Active,         // Running, waiting for delay to expire
            Paused,         // Timer paused, will resume when unpaused
            Executing,      // Currently executing callback
            PendingRemoval  // Marked for removal, will be cleaned up
        };

        struct DelayedGameThreadAction
        {
            const LuaMadeSimple::Lua* lua;
            int32_t lua_action_function_ref{};
            int32_t lua_action_thread_ref{};
            GameThreadExecutionMethod method{GameThreadExecutionMethod::EngineTick};
            DelayedActionStatus status{DelayedActionStatus::Active};
            std::chrono::steady_clock::time_point execute_at{};  // Absolute time when action should execute
            int64_t time_remaining_ms{0};  // Time remaining when paused (milliseconds)
            int64_t frames_remaining{0};  // Countdown for frame-based delays
            int64_t delay_ms{0};  // Original delay in milliseconds (for loop/reset)
            int64_t delay_frames{0};  // Original delay in frames (0 means use time-based delay)
            int64_t handle{0};  // Unique handle for this action
            bool is_retriggerable{false};  // If true, can be reset by calling with same handle
            bool is_looping{false};  // If true, re-schedule after each execution
            bool pause_after_execution{false};  // Pause immediately after current callback returns
            std::shared_ptr<std::atomic<bool>> unload_token{};  // Owning mod's token -- see SimpleLuaAction.unload_token
        };

        static inline int64_t m_next_delayed_action_handle{1};

        struct AsyncAction
        {
            // TODO: Use LuaMadeSimple instead of lua_State*
            // Not doing it now because the copy constructor gets implicitly deleted which is needed for erase & remove_if
            // lua_State* lua_state;
            int32_t lua_action_function_ref{};
            ActionType type{};
            std::chrono::time_point<std::chrono::steady_clock> created_at{};
            int64_t delay{};
        };
        std::vector<AsyncAction> m_pending_actions{};
        std::vector<AsyncAction> m_delayed_actions{};

        struct SharedLuaVariable
        {
            struct UserdataContainer
            {
                void* userdata;
            };
            int lua_type{LUA_TNIL};
            void* value{};
            bool is_integer{}; // Is true if lua_isinteger returned true when this variable was shared.
        };
        struct LuaCallbackData
        {
            struct RegistryIndex
            {
                int32_t lua_index{};
                int32_t identifier{};
            };
            const LuaMadeSimple::Lua* lua;
            Unreal::UClass* instance_of_class;
            std::vector<std::pair<const LuaMadeSimple::Lua*, RegistryIndex>> registry_indexes;
            bool scheduled_for_removal{};
        };
        struct LuaCancellableCallbackData
        {
            uint64_t callback_id{};
            const LuaMadeSimple::Lua* lua;
            Unreal::FName instance_class_name{};
            Unreal::FName instance_class_outer_name{};
            int32_t lua_callback_function_ref{};
            int32_t lua_callback_thread_ref{};
        };
        // Pending NotifyOnNewObject callback to be executed on game thread
        // StaticConstructObject can be called from any thread (loading threads, etc.)
        // but Lua is NOT thread-safe, so we must defer Lua calls to the game thread
        struct PendingNotifyOnNewObjectCallback
        {
            uint64_t callback_id{}; // Stable ID for matching callback safely
            int32_t object_index{}; // UObject internal index
            int32_t object_serial{}; // Serial number for validity checks
        };
        struct FunctionHookData
        {
            std::vector<Unreal::FName> names{};
            LuaCallbackData callback_data{};
        };
        static inline std::vector<LuaCancellableCallbackData> m_static_construct_object_lua_callbacks;
        static inline uint64_t m_next_static_construct_callback_id{1};
        static inline std::vector<LuaCallbackData> m_process_console_exec_pre_callbacks;
        static inline std::vector<LuaCallbackData> m_process_console_exec_post_callbacks;
        static inline std::vector<LuaCallbackData> m_call_function_by_name_with_arguments_pre_callbacks;
        static inline std::vector<LuaCallbackData> m_call_function_by_name_with_arguments_post_callbacks;
        static inline std::vector<LuaCallbackData> m_local_player_exec_pre_callbacks;
        static inline std::vector<LuaCallbackData> m_local_player_exec_post_callbacks;
        static inline std::unordered_map<File::StringType, LuaCallbackData> m_global_command_lua_callbacks;
        static inline std::unordered_map<File::StringType, LuaCallbackData> m_custom_command_lua_pre_callbacks;
        static inline std::vector<SimpleLuaAction> m_game_thread_actions{};
        static inline std::vector<SimpleLuaAction> m_engine_tick_actions{};
        static inline std::vector<DelayedGameThreadAction> m_delayed_game_thread_actions{};
        // Pending queues for actions registered during iteration (prevents iterator invalidation)
        static inline std::vector<SimpleLuaAction> m_pending_game_thread_actions{};
        static inline std::vector<SimpleLuaAction> m_pending_engine_tick_actions{};
        static inline std::vector<DelayedGameThreadAction> m_pending_delayed_game_thread_actions{};
        // Pending NotifyOnNewObject callbacks to be processed on game thread
        static inline std::vector<PendingNotifyOnNewObjectCallback> m_pending_notify_on_new_object_callbacks{};
        // DEPTH of in-flight game-thread action drains (engine tick + ProcessEvent), not a
        // plain flag: a UFunction call inside an action re-enters ProcessEvent and nests a
        // second drain inside the first, and a flag would have the nested teardown clear it
        // while the outer drain is still executing Lua. Atomic because uninstall() waits on
        // it from the unload thread while the game thread decrements it outside the mutex
        // (the drains only hold m_thread_actions_mutex around the entry increment; the
        // actions themselves run lock-free).
        static inline std::atomic<uint32_t> m_is_processing_actions{};
        // NON-ZERO while mods are being installed or uninstalled on the UE4SS EVENT-LOOP thread.
        // While it is raised the GAME thread must not enter ANY mod's Lua state.
        //
        // WHY. queue_reinstall_mods() runs uninstall_mods() and then start_lua_mods() on the event
        // loop, and start_lua_mods() executes every mod's main.lua THERE. The game thread carries
        // on ticking the whole time, so the instant a starting mod registers a hook or queues an
        // ExecuteInGameThread action, the game thread can call into that same lua_State while the
        // event-loop thread is still running main.lua inside it. Two OS threads, one lua_State,
        // whose stack, GC and string table are all unsynchronised.
        //
        // That is the 2026-09-11 09:50 crash: an engine-tick drain (engine_tick_hook ->
        // process_simple_actions -> call_function) faulted in lua_rawget reading a table pointer
        // of 0x2f2765eb481, ~200ms after "All mods re-installed", with no unload warning in the
        // log because nothing was unloading any more -- the mods were STARTING.
        //
        // The existing guards do not cover this. The unload token and m_hook_callbacks_in_flight
        // both answer "is this mod going away", and a mod coming UP is not. m_thread_actions_mutex
        // is held across uninstall's erase and lua_close but NOT across start_lua_mods.
        //
        // A COUNTER, NOT A MUTEX, deliberately. Holding m_thread_actions_mutex across mod startup
        // would block the async loading threads, which take that same mutex at every
        // StaticConstructObject -- a mod that force-loads an asset from main.lua would deadlock
        // the loader exactly as UECC-7357EC did. This flag makes the game thread SKIP Lua and
        // carry on; queued actions simply stay queued (the drains return before swapping the list
        // out, so nothing is dropped) and hooks no-op for the few hundred milliseconds a reload
        // takes. A counter rather than a bool because queue_reinstall_mods() raises it and then
        // calls start_lua_mods(), which raises it again.
        static inline std::atomic<uint32_t> m_mods_transitioning{};

        // RAII for m_mods_transitioning, so an exception out of mod startup cannot leave the game
        // thread permanently locked out of Lua.
        struct ScopedModTransition
        {
            ScopedModTransition()
            {
                LuaMod::m_mods_transitioning.fetch_add(1, std::memory_order_acq_rel);
            }
            ~ScopedModTransition()
            {
                LuaMod::m_mods_transitioning.fetch_sub(1, std::memory_order_acq_rel);
                LuaMod::m_mods_transitioning.notify_all();
            }
            ScopedModTransition(const ScopedModTransition&) = delete;
            ScopedModTransition& operator=(const ScopedModTransition&) = delete;
        };

        // True when the game thread must keep out of every mod's Lua state right now.
        static auto mods_are_transitioning() -> bool
        {
            return LuaMod::m_mods_transitioning.load(std::memory_order_acquire) != 0;
        }

        static inline GameThreadExecutionMethod m_default_game_thread_method{GameThreadExecutionMethod::EngineTick};
        // This is storage that persists through hot-reloads.
        static inline std::unordered_map<std::string, SharedLuaVariable> m_shared_lua_variables{};
        static inline std::vector<FunctionHookData> m_custom_event_callbacks{};
        static inline std::vector<LuaCallbackData> m_load_map_pre_callbacks{};
        static inline std::vector<LuaCallbackData> m_load_map_post_callbacks{};
        static inline std::vector<LuaCallbackData> m_init_game_state_pre_callbacks{};
        static inline std::vector<LuaCallbackData> m_init_game_state_post_callbacks{};
        static inline std::vector<LuaCallbackData> m_begin_play_pre_callbacks{};
        static inline std::vector<LuaCallbackData> m_begin_play_post_callbacks{};
        static inline std::vector<LuaCallbackData> m_end_play_pre_callbacks{};
        static inline std::vector<LuaCallbackData> m_end_play_post_callbacks{};
        static inline std::vector<FunctionHookData> m_script_hook_callbacks{};
        // RegisterModUnload handlers. Fired from uninstall() while the state is still alive, so
        // a mod can undo what it wrote before the record of it dies with the state.
        static inline std::vector<LuaCallbackData> m_mod_unload_callbacks{};
        static inline bool m_is_currently_executing_game_action{};
        static inline std::recursive_mutex m_thread_actions_mutex{};

      private:
        std::jthread m_async_thread;
        std::thread::id m_main_thread_id{};
        bool m_processing_events{};
        bool m_pause_events_processing{};
        bool m_is_process_event_hooked{};
        static inline bool m_is_engine_tick_hooked{};
        std::mutex m_actions_lock{};

      public:
        // Hook callbacks currently executing Lua IN THIS MOD'S STATE.
        //
        // scheduled_for_removal stops a hook callback from ENTERING a Lua state, but says
        // nothing about one already inside it, and there was no way to wait for that. So
        // uninstall() could set the flag, unregister, and then run fire_on_mod_unload()'s Lua in
        // a state the game thread was still executing -- two OS threads in one lua_State, whose
        // stack, GC and string table are all unsynchronised. The result is a stack slot that
        // should hold a closure holding something else; observed twice on 2026-09-04 as an AV in
        // luaV_execute and in funcnamefromcall, each dereferencing a Proto that was really a
        // string. The window is wide because a callback may call FindAllOf, which walks the whole
        // UObject array and takes tens of milliseconds.
        //
        // PER MOD, not global. Every Lua mod has its own lua_State, so a callback running in
        // another mod's state cannot corrupt this one and there is nothing to wait for -- but the
        // first version of this counter was a single static, and uninstall_mods() unloads mods one
        // at a time, so ONE callback that never finished made all 19 mods burn the full deadline:
        // a 38-second reload (2026-09-04). Per-mod, the wait is bounded by the mod that actually
        // has a callback inside it, and the timeout line names it.
        //
        // Incremented BEFORE the removal flag is tested (see lua_unreal_script_function_hook_pre)
        // so a drain can never observe zero while a callback is on its way in.
        std::atomic<int32_t> m_hook_callbacks_in_flight{0};

        LuaMod(UE4SSProgram&, StringType&& mod_name, StringType&& mod_path);
        ~LuaMod() override = default;

      private:
        auto start_async_thread() -> void
        {
            m_async_thread = std::jthread{&Mod::update_async, this};
        }

      private:
        static auto ensure_engine_tick_hooked() -> void;
        static auto ensure_process_event_hooked(LuaMod* mod) -> void;

        static auto custom_module_searcher(lua_State* L) -> int;
        auto setup_custom_module_loader(const LuaMadeSimple::Lua* lua_state) -> void;
        auto load_and_execute_script(const std::filesystem::path& script_path) -> bool;
        auto setup_lua_require_paths(const LuaMadeSimple::Lua& lua) const -> void;
        auto setup_lua_global_functions(const LuaMadeSimple::Lua& lua) const -> void;
        // Takes the state rather than using m_lua, so it can be installed on ANY state -- the
        // MCP executor included. Static because nothing in it needs the mod: every body already
        // resolves its owner from the state's ModRef global at call time, and the ones that can
        // work without an owner now say so explicitly (see get_hook_context).
        RC_UE4SS_API static auto setup_lua_global_functions_main_state_only(const LuaMadeSimple::Lua& lua) -> void;
        auto setup_lua_classes(const LuaMadeSimple::Lua& lua) const -> void;
        auto fire_on_lua_start_for_cpp_mods() -> void;
        auto fire_on_lua_stop_for_cpp_mods() -> void;
        auto fire_on_mod_unload() -> void;

      public:
        auto start_mod() -> void override;
        auto uninstall() -> void override;

        auto prepare_mod(const LuaMadeSimple::Lua& lua) -> void;

        RC_UE4SS_API auto lua() const -> const LuaMadeSimple::Lua&;
        RC_UE4SS_API auto main_lua() const -> const LuaMadeSimple::Lua*;
        RC_UE4SS_API auto async_lua() const -> const LuaMadeSimple::Lua*;
        RC_UE4SS_API auto get_lua_state() const -> lua_State*;

        // Is this callback one of MINE? Pointer comparison against the four states a mod owns,
        // and THE ONLY ownership test a teardown path may use: it cannot enter a lua_State, so
        // it cannot raise a Lua error, so it cannot abort the process. See the definition.
        [[nodiscard]] RC_UE4SS_API auto owns_lua_state(const LuaMadeSimple::Lua* state) const -> bool;

        RC_UE4SS_API auto get_scripts_path() const -> const std::filesystem::path& { return m_scripts_path; }

        RC_UE4SS_API auto actions_lock() -> void
        {
            m_actions_lock.lock();
        }
        RC_UE4SS_API auto actions_unlock() -> void
        {
            m_actions_lock.unlock();
        }

        [[nodiscard]] RC_UE4SS_API auto get_async_thread_id() const -> std::thread::id
        {
            return m_async_thread.get_id();
        }

        [[nodiscard]] RC_UE4SS_API auto get_main_thread_id() const -> std::thread::id
        {
            return m_main_thread_id;
        }

      public:
        // Called once when the program is starting, after mods are setup but before any mods have been started
        auto static on_program_start() -> void;

        auto static global_uninstall() -> void;

        // Async update
        // Used when the main update function would block other mods from executing their scripts
        auto update_async() -> void override;

        auto process_delayed_actions() -> void;
        auto clear_delayed_actions() -> void;

      public:
        static auto get_object_names(const Unreal::UObject*) -> std::vector<Unreal::FName>;
        static auto find_function_hook_data(std::vector<FunctionHookData>&, Unreal::FName) -> FunctionHookData*;
        static auto find_function_hook_data(std::vector<FunctionHookData>&, const Unreal::UObject*) -> FunctionHookData*;
        static auto find_function_hook_data(std::vector<FunctionHookData>&, const std::vector<Unreal::FName>&) -> FunctionHookData*;
        static auto remove_function_hook_data(std::vector<FunctionHookData>&, StringViewType) -> void;
        static auto remove_function_hook_data(std::vector<FunctionHookData>&, Unreal::FName) -> void;
        static auto remove_function_hook_data(std::vector<FunctionHookData>&, const Unreal::UObject*) -> void;
        static auto remove_function_hook_data(std::vector<FunctionHookData>&, const std::vector<Unreal::FName>&) -> void;

      public:
        // Evaluate `code` in a Lua state dedicated to the MCP server, capturing anything it
        // prints plus the values it returns into `output`. Returns false on a load or runtime
        // error, in which case `output` holds the error and traceback.
        //
        // The state is created on first use and persists, so globals set by one call are still
        // there for the next -- it behaves like a REPL session rather than a fresh sandbox.
        //
        // MUST be called on the game thread: the code it runs can touch live UObjects.
        RC_UE4SS_API static auto mcp_eval(std::string_view code, std::string& output) -> bool;
    };

    struct LuaStatics
    {
        // Lua instance connected to the in-game console.
        static LuaMadeSimple::Lua* console_executor;
        static bool console_executor_enabled;
    };
} // namespace RC
