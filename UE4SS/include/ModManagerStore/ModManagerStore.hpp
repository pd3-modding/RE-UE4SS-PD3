#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <Common.hpp>

namespace RC
{
    class CppUserModBase;
    namespace LuaMadeSimple
    {
        class Lua;
    }

    namespace ModManagerStore
    {
        // A native mirror of the settings registry that PD3ModManager builds in Lua, plus a
        // debug GUI tab ("Mod Manager") that renders and edits it.
        //
        // WHY THIS EXISTS
        // ---------------
        // The in-game MODS screen is UMG and dies with the game's front end. The store is fed
        // by the same Lua registry (shared/PD3ModManager), so the debug GUI can render the exact
        // descriptors and values the menu would, even when the game's UI is broken.
        //
        // OWNERSHIP (stage 1)
        // -------------------
        // config.lua on disk stays the authority and Lua keeps owning values; the store is a
        // mirror. Lua pushes every schema at MM.register time and every value at change time;
        // edits made in the tab are queued and drained on the game thread by running
        // MM.set_value / MM.apply in the Lua states, whose change notification echoes the new
        // value back here. Nothing here reads or writes a UObject, and nothing here persists
        // to disk.
        //
        // THREADING
        // ---------
        // The tab renders on the GUI thread; Lua pushes arrive from game/event threads. All
        // store access takes the store mutex, which is always the INNER lock: the drain holds
        // LuaMod::m_thread_actions_mutex and takes the store lock only to swap the queue, and
        // a push arriving out of that swap takes the store lock without any Lua lock held.

        // A setting value as the store holds it: only what a schema allows.
        struct SettingValue
        {
            enum class Kind : uint8_t
            {
                Number,
                Bool,
                String,
            };

            Kind kind{Kind::Number};
            double number{};
            bool boolean{};
            std::string string{};

            auto operator==(const SettingValue& other) const -> bool
            {
                if (kind != other.kind) return false;
                switch (kind)
                {
                case Kind::Number: return number == other.number;
                case Kind::Bool: return boolean == other.boolean;
                case Kind::String: return string == other.string;
                }
                return false;
            }
        };

        // One setting's schema, exactly what a descriptor declares. Functions
        // (on_value_change) and anything else it carries are skipped at the Lua boundary.
        struct SettingSchema
        {
            std::string id;
            std::string label; // display label, defaulted from id
            std::string type;  // "num" | "bool" | "enum" | "str" | "text"
            double default_number{};
            bool default_bool{};
            std::string default_string{};
            double min{};
            double max{};
            double step{}; // 0 = free; the Lua side still quantizes
            bool has_min{};
            bool has_max{};
            std::vector<std::string> choices; // enum only, in declared order
        };

        // A registered descriptor, mirrored from Lua. Registration order of settings is kept.
        struct ModEntry
        {
            std::string id;
            std::string label;
            std::string category{"General"};
            std::vector<SettingSchema> settings;
            std::unordered_map<std::string, SettingValue> values;
        };

        // An edit made in the debug GUI, waiting for the game-thread drain to hand it to Lua.
        struct PendingEdit
        {
            enum class Kind : uint8_t
            {
                SetValue,
                ApplyMod,
                RevertAll,
            };

            Kind kind{Kind::SetValue};
            std::string mod_id;
            std::string setting_id;
            SettingValue value;
        };

        // Registers the Lua-facing functions on all states, adds the "Mod Manager" tab to the
        // debug GUI and arms the game-thread drain (engine tick hook, like the MCP server's).
        // Call once from UE4SSProgram::init, after share_lua_functions.
        RC_UE4SS_API auto start() -> void;

        // Render function for the debug GUI tab (see DebuggingGUI::add_tab). Runs on the GUI
        // thread; takes the store mutex per frame.
        RC_UE4SS_API auto render_tab(CppUserModBase* owner) -> void;

        // RegisterNativeDescriptor({ id, label, category, settings = {...}, values = {...} })
        // Mirrors one descriptor into the store. Called from Lua at MM.register time.
        RC_UE4SS_API auto lua_register_descriptor(const LuaMadeSimple::Lua& lua) -> int;

        // PushNativeValue(mod_id, setting_id, value) -- mirrors a live value change into the
        // store. Called from Lua inside MM's change notification.
        RC_UE4SS_API auto lua_push_value(const LuaMadeSimple::Lua& lua) -> int;

        // Queue an edit made in the debug GUI. Stores the value optimistically on the tab side;
        // the drain hands it to Lua, whose change notification echoes it back into the store.
        RC_UE4SS_API auto enqueue_edit(PendingEdit edit) -> void;

        // The game-thread drain. Runs every pending edit through each started Lua mod state,
        // under the same lock script_hook holds. Registered on the engine tick hook.
        RC_UE4SS_API auto process_queue() -> void;
    } // namespace ModManagerStore
} // namespace RC