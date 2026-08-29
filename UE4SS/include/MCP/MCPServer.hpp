#pragma once

#include <chrono>
#include <string>
#include <vector>

#include <Common.hpp>
#include <File/File.hpp>

namespace RC::MCP
{
    // One line of MCP activity, kept in a small ring buffer for the GUI.
    struct ActivityRecord
    {
        enum class Kind
        {
            // Server lifecycle and client connect/disconnect, mirrored from the Rust side's
            // log callback.
            Lifecycle,
            // A tool invocation.
            ToolCall,
        };

        std::chrono::system_clock::time_point time{};
        Kind kind{Kind::Lifecycle};
        File::StringType text{};
        // Only meaningful for ToolCall.
        bool ok{true};
    };

    // The in-process MCP server.
    //
    // The protocol, HTTP transport and tool schemas live in the Rust crate at
    // deps/first/mcp_bind (a separate repository, vendored as a submodule). This class is the
    // host half: it owns the settings, implements the callbacks that crate needs, and marshals
    // anything touching game state onto the game thread.
    //
    // Everything here is a no-op unless UE4SS was built with UE4SS_ENABLE_MCP *and* the server
    // is enabled in UE4SS-settings.ini, so callers do not need to guard their calls.
    class RC_UE4SS_API MCPServer
    {
      public:
        // True if this build has MCP support compiled in at all.
        static auto is_compiled_in() -> bool;

        // Start the server if enabled in settings. Safe to call when disabled or already
        // running.
        static auto start() -> void;
        static auto stop() -> void;
        static auto is_running() -> bool;

        // Drain queued tool requests. MUST be called on the game thread -- it runs Lua that
        // touches live UObjects. Wired to the engine tick hook by start().
        static auto process_game_thread_queue() -> void;

        // Snapshot of recent activity, oldest first, for the GUI.
        static auto recent_activity() -> std::vector<ActivityRecord>;
        static auto clear_activity() -> void;

        // One-line human-readable state, e.g. "listening on 127.0.0.1:8787" or "disabled".
        static auto status_line() -> File::StringType;
    };
} // namespace RC::MCP
