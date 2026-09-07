#include <GameConsoleForwarder.hpp>

#include <Helpers/String.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/FStrProperty.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FMemory.hpp>
#include <Unreal/FOutputDevice.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <algorithm>
#include <mutex>
#include <vector>

using namespace RC;

namespace
{
    // Lines waiting for the game thread. Devices receive from any thread (the loader runs on
    // several); only the EngineTick drain touches Unreal, and only from the game thread.
    std::mutex s_queue_mutex{};
    std::vector<File::StringType> s_queue{};
    // Bound on queued lines: a silent loader flood before the console exists must not grow
    // without limit. Oldest lines are dropped, matching what the console's own scrollback
    // (MaxScrollbackSize, BaseInput.ini) will do to them anyway.
    constexpr size_t MAX_QUEUED_LINES = 512;

    // Resolved lazily on the game thread and used only there -- no cross-thread UObject reads.
    Unreal::UObject* s_viewport_client = nullptr;
    Unreal::UObject* s_kismet_system_cdo = nullptr;
    Unreal::UFunction* s_exec_command_func = nullptr;

    bool s_drain_failure_logged = false;

    auto severity_prefix(int32_t color) -> const File::CharType*
    {
        switch (color)
        {
        case Color::Red: return STR("ERROR: ");
        case Color::Yellow: return STR("WARNING: ");
        default: return STR("");
        }
    }
}

auto Output::GameConsoleDevice::has_optional_arg() const -> bool
{
    return true;
}

auto Output::GameConsoleDevice::receive(File::StringViewType fmt) const -> void
{
    receive_with_optional_arg(fmt, Color::NoColor);
}

auto Output::GameConsoleDevice::receive_with_optional_arg(File::StringViewType fmt, int32_t optional_arg) const -> void
{
    auto text = File::StringType{fmt};
    if (text.ends_with(STR('\n')))
    {
        text.pop_back();
    }

    // One queued line per newline, matching what the GUI console's device does -- a line that
    // survives to the exec payload must not contain a newline or '|', which
    // UPlayer::ConsoleCommand treats as command separators.
    size_t start = 0;
    while (start <= text.size())
    {
        auto newline = text.find(STR('\n'), start);
        auto line = newline == File::StringType::npos ? text.substr(start) : text.substr(start, newline - start);
        if (!line.empty())
        {
            auto clean = line;
            std::replace(clean.begin(), clean.end(), STR('|'), STR('/'));
            std::replace(clean.begin(), clean.end(), STR('\r'), STR(' '));
            std::lock_guard lock{s_queue_mutex};
            if (s_queue.size() >= MAX_QUEUED_LINES)
            {
                s_queue.erase(s_queue.begin());
            }
            File::StringType forwarded{STR("[UE4SS] ")};
            forwarded += severity_prefix(optional_arg);
            forwarded += clean;
            s_queue.push_back(std::move(forwarded));
        }
        if (newline == File::StringType::npos) break;
        start = newline + 1;
    }
}

namespace RC::GameConsoleForwarder
{
    // The "ue4ss.echo" pipe: a pre-callback on the engine's ProcessConsoleExec that turns a
    // queued line's exec back into console output. Runs before the Lua command dispatch
    // (registered in on_program_start, ahead of every mod), and writes through `ar`, the
    // FConsoleOutputDevice(ViewportConsole) the exec caller built -- the same device
    // `out:Log` uses from Lua command handlers.
    static auto intercept_console_exec(
            Unreal::Hook::TCallbackIterationData<bool>& info,
            Unreal::UObject* context,
            const File::CharType* cmd,
            Unreal::FOutputDevice& ar,
            Unreal::UObject* executor) -> void
    {
        (void)context;
        (void)executor;
        if (!cmd) return;

        File::StringType line{ToCharTypePtr(cmd)};
        constexpr File::CharType prefix[] = STR("ue4ss.echo ");
        if (line.compare(0, sizeof(prefix) / sizeof(File::CharType) - 1, prefix) != 0) return;

        auto payload = line.substr(sizeof(prefix) / sizeof(File::CharType) - 1);
        if (!payload.empty())
        {
            ar.Log(ToCharTypePtr(payload.c_str()));
        }
        // Suppress the engine's own handling of the line: without this, an unrecognized
        // command would append its own "Command not recognized" line after our payload.
        info.TrySetReturnValue(true);
    }

    static auto ensure_targets_resolved() -> Unreal::UObject*
    {
        // The viewport client is a session-lifetime object; resolve once and re-use. The
        // console is re-read per drain: it is created by ConsoleEnablerMod some time after
        // the viewport exists, and re-reads are two pointer reads.
        if (!s_viewport_client)
        {
            s_viewport_client = Unreal::UObjectGlobals::FindFirstOf(STR("GameViewportClient"));
        }
        if (!s_viewport_client) return nullptr;

        auto* console_prop = s_viewport_client->GetClassPrivate()->FindProperty(Unreal::FName{STR("ViewportConsole"), Unreal::FNAME_Find});
        if (!console_prop) return nullptr;
        auto* console = *reinterpret_cast<Unreal::UObject**>(
                reinterpret_cast<Unreal::uint8*>(s_viewport_client) + console_prop->GetOffset_Internal());
        if (!console) return nullptr;

        if (!s_kismet_system_cdo)
        {
            s_kismet_system_cdo = Unreal::UObjectGlobals::StaticFindObject(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        }
        if (!s_kismet_system_cdo) return nullptr;
        if (!s_exec_command_func)
        {
            s_exec_command_func = s_kismet_system_cdo->GetFunctionByName(STR("ExecuteConsoleCommand"));
        }
        return s_exec_command_func ? console : nullptr;
    }

    static auto drain() -> void
    {
        std::vector<File::StringType> batch{};
        {
            std::lock_guard lock{s_queue_mutex};
            batch.swap(s_queue);
        }
        if (batch.empty()) return;

        Unreal::UObject* console = nullptr;
        try
        {
            console = ensure_targets_resolved();
        }
        catch (std::exception&)
        {
            console = nullptr;
        }
        if (!console || !s_kismet_system_cdo || !s_exec_command_func) return;

        // The exec falls back to GLog when the console is missing (then nothing is visible),
        // so the console itself is the gate -- lines received before it exists are dropped
        // here, not retried. Forwarding is a live surface, not a log sink; UE4SS.log holds
        // everything regardless.
        // Note on the route: ExecuteConsoleCommand(world) -> PlayerController:ConsoleCommand
        // needs a world and a player controller in it. Without them the engine's GEngine->Exec
        // fallback is reached and the payload is silently dropped -- same as the console
        // staying closed, so no retry bookkeeping either.

        auto* func = s_exec_command_func;
        for (const auto& line : batch)
        {
            try
            {
                std::vector<Unreal::uint8> frame(static_cast<size_t>(func->GetPropertiesSize()), Unreal::uint8{});
                auto* context_prop = func->FindProperty(Unreal::FName{STR("WorldContextObject"), Unreal::FNAME_Find});
                auto* command_prop = func->FindProperty(Unreal::FName{STR("Command"), Unreal::FNAME_Find});
                auto* player_prop = func->FindProperty(Unreal::FName{STR("SpecificPlayer"), Unreal::FNAME_Find});
                if (!context_prop || !command_prop || !player_prop) return;

                *reinterpret_cast<Unreal::UObject**>(frame.data() + context_prop->GetOffset_Internal()) = s_viewport_client;
                // FStrProperty params live IN the frame as FString objects (FFrame::
                // StepExplicitProperty copies the value out of the slot; UE4SS's own
                // push_strproperty writes it the same way) -- not as pointers.
                *reinterpret_cast<Unreal::FString*>(frame.data() + command_prop->GetOffset_Internal()) = Unreal::FString{line};
                // SpecificPlayer stays null: the route resolves the world's first player
                // controller itself, so this works in the front end too.
                *reinterpret_cast<Unreal::UObject**>(frame.data() + player_prop->GetOffset_Internal()) = nullptr;

                s_kismet_system_cdo->ProcessEvent(func, frame.data());
            }
            catch (std::exception& e)
            {
                // Never take the game thread down over a debug feature. The [UE4SS] prefix
                // keeps this from re-entering the device (it is a direct log-file write).
                if (!s_drain_failure_logged)
                {
                    s_drain_failure_logged = true;
                    Output::send(STR("[UE4SS] console forward drain failed once: {}\n"), ensure_str(e.what()));
                }
            }
        }
    }

    auto initialize() -> void
    {
        Unreal::Hook::RegisterProcessConsoleExecGlobalPreCallback(
                intercept_console_exec, {false, false, STR("UE4SS"), STR("GameConsoleForwarderPipe")});

        // Drain cadence is the engine tick the GUI render already rides (HookEngineTick): no
        // polling of game state, only delivering what was queued since the last tick. With
        // HookEngineTick = 0 nothing drains; that combination is documented in the ini.
        Unreal::Hook::RegisterEngineTickPostCallback(
                [](Unreal::Hook::TCallbackIterationData<void>&, Unreal::UEngine*, float, bool) { drain(); },
                {false, false, STR("UE4SS"), STR("GameConsoleForwarderDrain")});
    }
} // namespace RC::GameConsoleForwarder