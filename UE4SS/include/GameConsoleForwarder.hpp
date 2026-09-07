#pragma once

#include <DynamicOutput/Common.hpp>
#include <DynamicOutput/Macros.hpp>
#include <DynamicOutput/OutputDevice.hpp>

namespace RC
{
    namespace Output
    {
        // Forward every UE4SS output line into the game's own console (UConsole, the one
        // ConsoleEnablerMod constructs). Opt-in via UE4SS.ini [Debug] ForwardLogToGameConsole.
        //
        // WHY A DEVICE. Output::send iterates the default devices with text + a severity color
        // (LogLevel::Warning = Yellow, Error = Red), so a device here sees everything the
        // loader, C++ mods and Lua `print` emit -- the internal messages that never pass
        // through Lua. The write path reuses the console exec route: a queued line is drained
        // on the game thread by calling KismetSystemLibrary:ExecuteConsoleCommand, whose exec
        // lands in UPlayer::ConsoleCommand's FConsoleOutputDevice(ViewportConsole); the
        // "ue4ss.echo" pipe (a ProcessConsoleExec pre-callback in this module) writes the
        // payload through ar.Log -> UConsole::OutputText, which buffers in scrollback even
        // while the console is closed. No vtable or member-offset RE anywhere.
        //
        // The stock UE console draws all scrollback in ONE color (ConsoleSettings->InputColor),
        // so severity travels as a text prefix (ERROR: / WARNING:), not as a color.
        class GameConsoleDevice : public OutputDevice
        {
          public:
            ~GameConsoleDevice() override = default;

          public:
            auto has_optional_arg() const -> bool override;
            auto receive(File::StringViewType fmt) const -> void override;
            auto receive_with_optional_arg(File::StringViewType fmt, int32_t optional_arg = 0) const -> void override;
        };
    } // namespace Output

    namespace GameConsoleForwarder
    {
        // Registers the "ue4ss.echo" pipe hook and the EngineTick drain. Call once from
        // UE4SSProgram::on_program_start, after the Unreal hooks are set up; the caller
        // gates on [Debug] ForwardLogToGameConsole.
        auto initialize() -> void;
    }
} // namespace RC