#include <ModManagerStore/ModManagerStore.hpp>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <optional>
#include <unordered_set>

#include <DynamicOutput/DynamicOutput.hpp>
#include <GUI/GUITab.hpp>
#include <Helpers/String.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <Mod/LuaMod.hpp>
#include <UE4SSProgram.hpp>
#include <Unreal/Hooks/Hooks.hpp>

#include <imgui.h>

namespace RC::ModManagerStore
{
    namespace
    {
        // ------------------------------------------------------------------------------
        // Store state. Guarded by s_store_mutex; the tab renders on the GUI thread while
        // Lua pushes arrive from game/event threads. Lock order is always
        // LuaMod::m_thread_actions_mutex -> s_store_mutex, never the reverse, so a push
        // coming out of the drain (which holds the Lua lock) cannot deadlock the GUI.
        // ------------------------------------------------------------------------------
        std::mutex s_store_mutex{};
        std::vector<ModEntry> s_mods{};
        std::vector<PendingEdit> s_pending{};
        bool s_drain_armed{false};

        // The per-widget edit state below is touched from two threads: the tab renders on the
        // GUI thread while re-registration clears it from the Lua/event thread at mod reload.
        // Its own mutex, always the OUTER lock relative to s_store_mutex (submit() enqueues
        // under s_store_mutex while holding this one; clear_widget_state() is called WITHOUT
        // s_store_mutex held, see lua_register_descriptor) -- the reverse order would deadlock.
        std::mutex s_widget_mutex{};

        auto find_mod_locked(const std::string& mod_id) -> ModEntry*
        {
            for (auto& mod : s_mods)
            {
                if (mod.id == mod_id) return &mod;
            }
            return nullptr;
        }

        // ------------------------------------------------------------------------------
        // Lua table reading, on the raw C API. The descriptor arrives as a Lua table whose
        // fields are plain data plus functions we must skip, so every read is type-checked.
        // Each helper is stack-clean: it pushes the key, fetches, validates, pops.
        // ------------------------------------------------------------------------------

        auto lfield(lua_State* L, int idx, const char* key) -> bool
        {
            // Normalize the table index BEFORE pushing the key: the push shifts every
            // relative index by one, so an unnormalized -1 here would read the key string
            // itself. This bug silently emptied every nested read once already.
            if (idx < 0 && idx > LUA_REGISTRYINDEX)
            {
                idx = lua_gettop(L) + idx + 1;
            }
            lua_pushstring(L, key);
            lua_gettable(L, idx);
            return !lua_isnil(L, -1);
        }

        auto lfield_str(lua_State* L, int idx, const char* key, std::string& out) -> bool
        {
            if (!lfield(L, idx, key) || !lua_isstring(L, -1))
            {
                lua_pop(L, 1);
                return false;
            }
            size_t len{};
            const char* s = lua_tolstring(L, -1, &len);
            out.assign(s, len);
            lua_pop(L, 1);
            return true;
        }

        auto lfield_num(lua_State* L, int idx, const char* key, double& out) -> bool
        {
            if (!lfield(L, idx, key) || !lua_isnumber(L, -1))
            {
                lua_pop(L, 1);
                return false;
            }
            out = lua_tonumber(L, -1);
            lua_pop(L, 1);
            return true;
        }

        auto lfield_bool(lua_State* L, int idx, const char* key, bool& out) -> bool
        {
            if (!lfield(L, idx, key) || !lua_isboolean(L, -1))
            {
                lua_pop(L, 1);
                return false;
            }
            out = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            return true;
        }

        // Reads any primitive as a SettingValue; functions/tables/nil are rejected.
        auto lvalue(lua_State* L, int idx, SettingValue& out) -> bool
        {
            switch (lua_type(L, idx))
            {
            case LUA_TNUMBER:
                out = SettingValue{.kind = SettingValue::Kind::Number, .number = lua_tonumber(L, idx)};
                return true;
            case LUA_TBOOLEAN:
                out = SettingValue{.kind = SettingValue::Kind::Bool, .boolean = lua_toboolean(L, idx) != 0};
                return true;
            case LUA_TSTRING:
            {
                size_t len{};
                const char* s = lua_tolstring(L, idx, &len);
                out = SettingValue{.kind = SettingValue::Kind::String};
                out.string.assign(s, len);
                return true;
            }
            default:
                return false;
            }
        }

        // The default a schema seeds when neither the registering state nor this mirror has a
        // value for a setting.
        auto default_value(const SettingSchema& setting) -> SettingValue
        {
            if (setting.type == "bool")
            {
                return SettingValue{.kind = SettingValue::Kind::Bool, .boolean = setting.default_bool};
            }
            if (setting.type == "str" || setting.type == "text" || setting.type == "enum")
            {
                return SettingValue{.kind = SettingValue::Kind::String, .string = setting.default_string};
            }
            return SettingValue{.kind = SettingValue::Kind::Number, .number = setting.default_number};
        }

        // ------------------------------------------------------------------------------
        // Lua chunk generation for the drain. Each edit becomes a tiny guarded script run
        // in every started Lua mod state; states whose registry does not know the
        // descriptor no-op via the schema check.
        // ------------------------------------------------------------------------------

        auto lua_quote(const std::string& s) -> std::string
        {
            std::string out;
            out.reserve(s.size() + 2);
            out += '"';
            for (char c : s)
            {
                switch (c)
                {
                case '\\': out += "\\\\"; break;
                case '"': out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\0': out += "\\0"; break;
                default: out += c;
                }
            }
            out += '"';
            return out;
        }

        auto lua_number(double v) -> std::string
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.17g", v);
            return buf;
        }

        auto value_literal(const SettingValue& v) -> std::string
        {
            switch (v.kind)
            {
            case SettingValue::Kind::Number: return lua_number(v.number);
            case SettingValue::Kind::Bool: return v.boolean ? "true" : "false";
            case SettingValue::Kind::String: return lua_quote(v.string);
            }
            return "nil";
        }

        auto chunk_for(const PendingEdit& edit) -> std::string
        {
            switch (edit.kind)
            {
            case PendingEdit::Kind::SetValue:
                return fmt::format(
                        "local __ok, MM = pcall(require, 'PD3ModManager')\n"
                        "if __ok and type(MM) == 'table' and MM.schema_of and MM.schema_of({0}) then\n"
                        "    MM.set_value({0}, {1}, {2}, {{ source = 'debuggui' }})\n"
                        "end\n",
                        lua_quote(edit.mod_id),
                        lua_quote(edit.setting_id),
                        value_literal(edit.value));
            case PendingEdit::Kind::ApplyMod:
                // Fail closed: without a host assertion, requires_host descriptors are skipped
                // -- exactly MM.apply's own contract when ctx is missing.
                return fmt::format(
                        "local __ok, MM = pcall(require, 'PD3ModManager')\n"
                        "if __ok and type(MM) == 'table' and MM.schema_of and MM.schema_of({0}) then\n"
                        "    local __core_ok, Core = pcall(require, 'PD3Core')\n"
                        "    local __ctx = nil\n"
                        "    if __core_ok and type(Core) == 'table' and Core.is_host then\n"
                        "        local __host_ok, __host = pcall(Core.is_host)\n"
                        "        __ctx = {{ is_host = __host_ok and __host or false }}\n"
                        "    end\n"
                        "    MM.apply({0}, __ctx)\n"
                        "end\n",
                        lua_quote(edit.mod_id));
            case PendingEdit::Kind::RevertAll:
                return "local __ok, MM = pcall(require, 'PD3ModManager')\n"
                       "if __ok and type(MM) == 'table' and MM.revert_all then MM.revert_all() end\n";
            }
            return {};
        }

        // Runs one chunk in one Lua state. Caller holds LuaMod::m_thread_actions_mutex.
        auto run_chunk(lua_State* L, const std::string& code, std::string_view mod_name) -> void
        {
            const int stack_base = lua_gettop(L);
            const int error_handler_index = LuaMadeSimple::push_pcall_error_handler(L);

            if (luaL_loadstring(L, code.c_str()) != LUA_OK)
            {
                const char* message = lua_tostring(L, -1);
                Output::send<LogLevel::Error>(STR("[ModManagerStore] chunk load failed in '{}': {}\n"), ensure_str(mod_name),
                                              ensure_str(message ? message : "unknown error"));
                lua_settop(L, stack_base);
                return;
            }

            if (lua_pcall(L, 0, 0, error_handler_index) != LUA_OK)
            {
                const char* message = lua_tostring(L, -1);
                Output::send<LogLevel::Error>(STR("[ModManagerStore] edit failed in '{}': {}\n"), ensure_str(mod_name),
                                              ensure_str(message ? message : "unknown error"));
            }

            lua_settop(L, stack_base);
        }

        // ------------------------------------------------------------------------------
        // Tab rendering
        // ------------------------------------------------------------------------------

        // What a widget shows while its echo from Lua has not landed yet. Keyed
        // "mod\x01setting"; cleared once the store's value matches, so a dropped edit
        // cannot wedge the widget forever.
        auto ui_edits() -> std::unordered_map<std::string, SettingValue>&
        {
            static std::unordered_map<std::string, SettingValue> s_ui_edits{};
            return s_ui_edits;
        }

        // ------------------------------------------------------------------------------
        // Per-widget edit state. A control is seeded from the store ONLY when it was not
        // active last frame; while it is held, the widget owns its value. Re-seeding while
        // active is what broke both editable kinds here: for InputText it is an external
        // buffer modification mid-edit (the user's typed text was clobbered by the next
        // frame's seed -- the "text fields never update" report), and for a slider it snaps
        // the handle back to the stored value under the drag. Committing once on
        // deactivation is also the contract the UMG screen follows (defer + flush at
        // capture end): one MM.set_value per gesture instead of one per drag tick, so a
        // mod's on_value_change does not run every tick of a drag.
        // ------------------------------------------------------------------------------
        auto widget_scratch() -> std::unordered_map<std::string, SettingValue>&
        {
            static std::unordered_map<std::string, SettingValue> s{};
            return s;
        }

        auto widget_held() -> std::unordered_set<std::string>&
        {
            static std::unordered_set<std::string> s{};
            return s;
        }

        // InputText edits its buffer in place across frames, so each str/text widget needs a
        // persistent buffer of its own. The single static buffer this replaced was seeded by
        // every text row in turn, so one row's in-flight edit was overwritten by the next
        // row's seed before its own widget call ran again.
        auto widget_buffers() -> std::unordered_map<std::string, std::vector<char>>&
        {
            static std::unordered_map<std::string, std::vector<char>> s{};
            return s;
        }

        auto clear_widget_state() -> void
        {
            // WITHOUT s_store_mutex held (see lua_register_descriptor): the lock order this
            // enforces is s_widget_mutex -> s_store_mutex, never the reverse.
            std::lock_guard guard{s_widget_mutex};
            widget_scratch().clear();
            widget_held().clear();
            widget_buffers().clear();
            ui_edits().clear();
        }

        auto widget_key(const std::string& mod_id, const std::string& setting_id) -> std::string
        {
            return mod_id + '\x01' + setting_id;
        }

        auto render_setting(const ModEntry& mod, const SettingSchema& setting, const SettingValue& stored) -> void
        {
            // The widget state is shared with the event thread (clear at re-registration, the
            // drain's echo), so the whole read-modify-write of one widget's row runs under
            // s_widget_mutex. Held per widget, not per frame -- the stores are tiny.
            std::lock_guard widget_guard{s_widget_mutex};
            auto& edits = ui_edits();
            const std::string key = widget_key(mod.id, setting.id);

            const SettingValue* display = &stored;
            if (auto it = edits.find(key); it != edits.end())
            {
                if (it->second == stored)
                {
                    edits.erase(it); // echo landed
                }
                else
                {
                    display = &it->second;
                }
            }

            const auto submit = [&](SettingValue v) {
                edits[key] = v;
                enqueue_edit(PendingEdit{.kind = PendingEdit::Kind::SetValue,
                                         .mod_id = mod.id,
                                         .setting_id = setting.id,
                                         .value = v});
            };

            ImGui::PushID(setting.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(setting.label.c_str());
            ImGui::TableSetColumnIndex(1);

            switch (setting.type == "num" ? 0 : setting.type == "bool" ? 1 :
                    setting.type == "enum" ? 2 : 3)
            {
            case 0: // num
            {
                if (setting.has_min && setting.has_max)
                {
                    // The drag's in-flight value, tracked per widget: the slider is seeded from
                    // the store only when it is not held, and submitted ONCE, on release. A drag
                    // that ends where it started submits nothing.
                    const bool held = widget_held().count(key) != 0;
                    auto& scratch = widget_scratch();
                    auto held_it = scratch.find(key);
                    float v = (held && held_it != scratch.end()
                               && held_it->second.kind == SettingValue::Kind::Number)
                                  ? static_cast<float>(held_it->second.number)
                                  : static_cast<float>(display->number);
                    if (ImGui::SliderFloat("##v", &v, static_cast<float>(setting.min),
                                           static_cast<float>(setting.max)))
                    {
                        widget_scratch()[key] =
                            SettingValue{.kind = SettingValue::Kind::Number, .number = v};
                    }
                    if (ImGui::IsItemActive())
                    {
                        widget_held().insert(key);
                    }
                    else
                    {
                        widget_held().erase(key);
                        auto committed = scratch.find(key);
                        if (ImGui::IsItemDeactivatedAfterEdit() && committed != scratch.end())
                        {
                            submit(committed->second);
                            scratch.erase(committed);
                        }
                    }
                }
                else
                {
                    double v = display->number;
                    if (ImGui::InputDouble("##v", &v, 0.0, 0.0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue))
                    {
                        submit(SettingValue{.kind = SettingValue::Kind::Number, .number = v});
                    }
                }
                break;
            }
            case 1: // bool
            {
                bool v = display->boolean;
                if (ImGui::Checkbox("##v", &v))
                {
                    submit(SettingValue{.kind = SettingValue::Kind::Bool, .boolean = v});
                }
                break;
            }
            case 2: // enum
            {
                if (ImGui::BeginCombo("##v", display->string.c_str()))
                {
                    for (const auto& choice : setting.choices)
                    {
                        if (ImGui::Selectable(choice.c_str(), choice == display->string))
                        {
                            submit(SettingValue{.kind = SettingValue::Kind::String, .string = choice});
                        }
                    }
                    ImGui::EndCombo();
                }
                break;
            }
            default: // str / text
            {
                // A per-widget buffer, seeded from the store only when this widget is not held.
                // While it is held, the buffer IS the edit in progress; overwriting it per frame
                // from the store reverts every keystroke on the frame after it lands. Committed
                // on deactivation, and dropped with the commit so the next inactive frame
                // re-seeds from whatever the echo landed.
                static constexpr size_t TEXT_BUF = 4096;
                const bool held = widget_held().count(key) != 0;
                auto buf_it = widget_buffers().find(key);
                if (buf_it == widget_buffers().end())
                {
                    buf_it = widget_buffers().emplace(key, std::vector<char>(TEXT_BUF, '\0')).first;
                }
                auto& buf = buf_it->second;
                if (!held)
                {
                    const size_t copy_len = std::min(display->string.size(), buf.size() - 1);
                    if (copy_len > 0) memcpy(buf.data(), display->string.data(), copy_len);
                    buf[copy_len] = '\0';
                }

                const bool multiline = setting.type == "text";
                if (multiline)
                {
                    ImGui::InputTextMultiline("##v", buf.data(), buf.size(), ImVec2(0, 80));
                }
                else
                {
                    ImGui::InputText("##v", buf.data(), buf.size(), ImGuiInputTextFlags_EnterReturnsTrue);
                }

                if (ImGui::IsItemActive())
                {
                    widget_held().insert(key);
                }
                else
                {
                    widget_held().erase(key);
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        submit(SettingValue{.kind = SettingValue::Kind::String,
                                            .string = std::string{buf.data()}});
                        widget_buffers().erase(key);
                    }
                }
                break;
            }
            }

            ImGui::PopID();
        }

        auto render_mod(const ModEntry& mod) -> void
        {
            ImGui::PushID(mod.id.c_str());

            // Vertical section, like the in-game MODS screen: a ruled heading with the mod
            // label and its Apply at the right edge, then the settings in a two-column grid.
            // ImGui's grid mechanism IS the table API: column 0 auto-fits to the widest label
            // (WidthFixed under SizingFixedFit) and column 1 stretches to fill the rest, so
            // the control column aligns across rows without any manual width math.
            const float apply_x = ImGui::GetContentRegionAvail().x - 60.0f;
            ImGui::SeparatorText(mod.label.c_str());
            ImGui::SameLine(apply_x);
            if (ImGui::SmallButton("Apply"))
            {
                enqueue_edit(PendingEdit{.kind = PendingEdit::Kind::ApplyMod, .mod_id = mod.id});
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Run this mod's apply() on the game thread.\n"
                                  "requires_host descriptors are skipped without host authority.");
            }

            if (ImGui::BeginTable("##rows", 2, ImGuiTableFlags_SizingFixedFit))
            {
                ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("##control", ImGuiTableColumnFlags_WidthStretch);

                for (const auto& setting : mod.settings)
                {
                    auto it = mod.values.find(setting.id);
                    const SettingValue stored = it != mod.values.end() ? it->second : SettingValue{};
                    render_setting(mod, setting, stored);
                }
                ImGui::EndTable();
            }
            ImGui::Dummy(ImVec2(0.0f, 8.0f));

            ImGui::PopID();
        }
    } // namespace

    // ------------------------------------------------------------------------------
    // Public API
    // ------------------------------------------------------------------------------

    auto start() -> void
    {
        auto& program = UE4SSProgram::get_program();

        program.get_debugging_ui().add_tab(
                std::make_shared<GUI::GUITab>(STR("Mod Manager"), &render_tab));

        if (UE4SSProgram::settings_manager.Hooks.HookEngineTick)
        {
            Unreal::Hook::RegisterEngineTickPreCallback(
                    [](Unreal::Hook::TCallbackIterationData<void>&, Unreal::UEngine*, float, bool) -> void {
                        process_queue();
                    },
                    {false, false, STR("UE4SS"), STR("ModManagerStore")});
            {
                std::lock_guard guard{s_store_mutex};
                s_drain_armed = true;
            }
        }
        else
        {
            // Same caveat the MCP server gives: without the tick hook nothing services the
            // queue, so edits would silently pile up. Say so instead of looking broken.
            Output::send<LogLevel::Warning>(STR("[ModManagerStore] Hooks.HookEngineTick is disabled; "
                                                "edits in the Mod Manager tab will not reach Lua.\n"));
        }

        Output::send(STR("[ModManagerStore] ready\n"));
    }

    auto lua_register_descriptor(const LuaMadeSimple::Lua& lua) -> int
    {
        if (!lua.is_table(1))
        {
            lua.throw_error("RegisterNativeDescriptor: expected a descriptor table");
        }

        lua_State* L = lua.get_lua_state();

        ModEntry entry{};
        if (!lfield_str(L, 1, "id", entry.id) || entry.id.empty())
        {
            lua.throw_error("RegisterNativeDescriptor: descriptor has no id");
        }
        lfield_str(L, 1, "label", entry.label);
        lfield_str(L, 1, "category", entry.category);
        if (entry.label.empty()) entry.label = entry.id;

        std::unordered_map<std::string, SettingValue> incoming_values{};

        // settings = array of { id, type, label, default, min, max, step, choices }
        //
        // Every lfield here MUST be followed by a pop on ALL paths: a bare lfield leaves the
        // fetched value (nil when absent) on the stack, and one stray value per iteration
        // drifts the loop indices until lua_next(L, -2) lands on a key string -- which is an
        // AV in luaH_next, not a Lua error (api_check is compiled out). The previous build
        // hid this because its index bug made the id/type reads fail before the choices path
        // was ever reached; fixing one bug unlocked the other.
        if (lfield(L, 1, "settings") && lua_istable(L, -1))
        {
            lua_pushnil(L);
            while (lua_next(L, -2) != 0)
            {
                if (lua_istable(L, -1))
                {
                    SettingSchema schema{};
                    if (lfield_str(L, -1, "id", schema.id) && !schema.id.empty() && lfield_str(L, -1, "type", schema.type))
                    {
                        if (!lfield_str(L, -1, "label", schema.label) || schema.label.empty())
                        {
                            schema.label = schema.id;
                        }
                        double d{};
                        if (lfield_num(L, -1, "default", d)) schema.default_number = d;
                        lfield_bool(L, -1, "default", schema.default_bool);
                        lfield_str(L, -1, "default", schema.default_string);
                        double min{}, max{}, step{};
                        if (lfield_num(L, -1, "min", min))
                        {
                            schema.min = min;
                            schema.has_min = true;
                        }
                        if (lfield_num(L, -1, "max", max))
                        {
                            schema.max = max;
                            schema.has_max = true;
                        }
                        lfield_num(L, -1, "step", schema.step);

                        if (lfield(L, -1, "choices") && lua_istable(L, -1))
                        {
                            lua_pushnil(L);
                            while (lua_next(L, -2) != 0)
                            {
                                if (lua_isstring(L, -1))
                                {
                                    size_t len{};
                                    const char* s = lua_tolstring(L, -1, &len);
                                    schema.choices.emplace_back(s, len);
                                }
                                lua_pop(L, 1);
                            }
                            lua_pop(L, 1); // the choices table
                        }
                        else
                        {
                            lua_pop(L, 1); // absent/non-table choices value lfield left behind
                        }
                        entry.settings.push_back(std::move(schema));
                    }
                }
                lua_pop(L, 1); // the setting table
            }
            lua_pop(L, 1); // the settings table
        }
        else
        {
            lua_pop(L, 1); // absent/non-table settings value lfield left behind
        }

        // values = { setting_id = value }
        if (lfield(L, 1, "values") && lua_istable(L, -1))
        {
            lua_pushnil(L);
            while (lua_next(L, -2) != 0)
            {
                if (lua_isstring(L, -2))
                {
                    SettingValue v{};
                    size_t len{};
                    const char* k = lua_tolstring(L, -2, &len);
                    if (lvalue(L, -1, v))
                    {
                        incoming_values[std::string{k, len}] = v;
                    }
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 1); // the values table
        }
        else
        {
            lua_pop(L, 1); // absent/non-table values value lfield left behind
        }

        std::lock_guard guard{s_store_mutex};

        // Replace-or-create, keeping registration order stable. Value precedence: what the
        // registering state holds now, then what this mirror had, then the schema default.
        bool replaced = false;
        for (auto& existing : s_mods)
        {
            if (existing.id != entry.id) continue;

            std::unordered_map<std::string, SettingValue> previous = std::move(existing.values);
            existing = std::move(entry);
            for (const auto& setting : existing.settings)
            {
                if (auto in = incoming_values.find(setting.id); in != incoming_values.end())
                {
                    existing.values[setting.id] = in->second;
                }
                else if (auto prev = previous.find(setting.id); prev != previous.end())
                {
                    existing.values[setting.id] = prev->second;
                }
                else
                {
                    existing.values[setting.id] = default_value(setting);
                }
            }
            replaced = true;
            break;
        }

        if (!replaced)
        {
            ModEntry& stored = s_mods.emplace_back(std::move(entry));
            for (const auto& setting : stored.settings)
            {
                if (auto in = incoming_values.find(setting.id); in != incoming_values.end())
                {
                    stored.values[setting.id] = in->second;
                }
                else
                {
                    stored.values[setting.id] = default_value(setting);
                }
            }
        }

        // Deployment diagnostics: an empty count here means the Lua bridge sent nothing or
        // the table walk failed, and the tab renders a bare collapsible.
        for (const auto& mod : s_mods)
        {
            if (mod.id == entry.id)
            {
                Output::send(STR("[ModManagerStore] mirrored '{}': {} settings, {} values\n"),
                             ensure_str(mod.id), mod.settings.size(), mod.values.size());
            }
        }

        // A re-registered descriptor is a mod reload: the tab's per-widget edit state belongs
        // to the render it replaced, so drop it. Deliberately OUTSIDE s_store_mutex -- this
        // takes s_widget_mutex, and the store lock is the INNER one everywhere. The next
        // render re-seeds from the fresh values above.
        clear_widget_state();

        return 0;
    }

    auto lua_push_value(const LuaMadeSimple::Lua& lua) -> int
    {
        lua_State* L = lua.get_lua_state();

        SettingValue v{};
        if (!lvalue(L, 3, v))
        {
            lua.throw_error("PushNativeValue: value must be a number, boolean or string");
        }
        const char* setting_id = luaL_checkstring(L, 2);
        const char* mod_id = luaL_checkstring(L, 1);

        {
            std::lock_guard guard{s_store_mutex};
            if (auto* mod = find_mod_locked(mod_id))
            {
                mod->values[setting_id] = v;
            }
            // No schema yet: drop. Register pushes the full value set when the descriptor
            // arrives, so nothing is lost.
        }

        return 0;
    }

    auto render_tab(CppUserModBase* owner) -> void
    {
        // A copy per frame: the store is tiny (dozens of settings), and rendering against the
        // live container under one lock across the whole frame would stall Lua pushes.
        std::vector<ModEntry> mods;
        bool drain_armed;
        {
            std::lock_guard guard{s_store_mutex};
            mods = s_mods;
            drain_armed = s_drain_armed;
        }

        if (!drain_armed)
        {
            ImGui::TextColored(ImVec4{1.0f, 0.6f, 0.2f, 1.0f}, "%s",
                               "HookEngineTick is off -- edits will not reach Lua (enable Hooks.HookEngineTick).");
            ImGui::Separator();
        }

        if (mods.empty())
        {
            ImGui::TextDisabled("%s", "No settings registered. Descriptors mirror in from PD3ModManager at mod load.");
            return;
        }

        if (ImGui::Button("Revert all writes"))
        {
            enqueue_edit(PendingEdit{.kind = PendingEdit::Kind::RevertAll});
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Same operation as the MODS screen's REVERT button:\n"
                              "each mod's revert() plus every journaled write, in every Lua state.");
        }
        ImGui::Separator();

        // Left-hand category tabs, right-hand content -- the same shape as the in-game MODS
        // screen (MM.categories: categories collect mods that contribute to them, in
        // first-registration order so the list is stable).
        static int selected_category = 0;

        std::vector<std::string> categories{};
        for (const auto& mod : mods)
        {
            bool seen = false;
            for (const auto& c : categories) seen = seen || c == mod.category;
            if (!seen) categories.push_back(mod.category);
        }
        if (categories.empty()) return;
        if (selected_category < 0 || selected_category >= static_cast<int>(categories.size()))
        {
            selected_category = 0; // the selected category went away (mod reload); fall back
        }

        ImGui::BeginChild("##categories", ImVec2(180, 0), ImGuiChildFlags_ResizeX);
        for (int i = 0; i < static_cast<int>(categories.size()); ++i)
        {
            const std::string label = fmt::format("{}##cat{}", categories[i], i);
            if (ImGui::Selectable(label.c_str(), i == selected_category))
            {
                selected_category = i;
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##category_content");
        const std::string& category = categories[selected_category];
        for (const auto& m : mods)
        {
            if (m.category == category)
            {
                render_mod(m);
            }
        }
        ImGui::EndChild();
    }

    auto enqueue_edit(PendingEdit edit) -> void
    {
        std::lock_guard guard{s_store_mutex};
        s_pending.push_back(std::move(edit));
    }

    auto process_queue() -> void
    {
        std::vector<PendingEdit> batch;
        {
            std::lock_guard guard{s_store_mutex};
            if (s_pending.empty())
            {
                return;
            }
            batch.swap(s_pending);
        }

        for (const auto& edit : batch)
        {
            const std::string code = chunk_for(edit);
            for (const auto& mod : UE4SSProgram::get_program().m_mods)
            {
                auto* lua_mod = dynamic_cast<LuaMod*>(mod.get());
                if (!lua_mod || !mod->is_started())
                {
                    continue;
                }

                // The same lock script_hook holds: this runs on the game thread inside engine
                // tick, so hook callbacks are not concurrent with it, but the event loop's
                // timers could touch a state at the same moment without it.
                std::lock_guard<std::recursive_mutex> lua_guard{LuaMod::m_thread_actions_mutex};
                run_chunk(lua_mod->get_lua_state(), code, to_string(mod->get_name()));
            }
        }
    }
} // namespace RC::ModManagerStore