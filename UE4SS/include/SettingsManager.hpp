#pragma once

#include <cstdint>
#include <filesystem>

#include <Common.hpp>
#include <File/File.hpp>
#include <GUI/GUI.hpp>
#include <Input/KeyDef.hpp>
#include <Unreal/UnrealInitializer.hpp>

namespace RC
{
    // Method for executing callbacks in game thread
    enum class GameThreadExecutionMethod
    {
        ProcessEvent, // Use ProcessEvent hook
        EngineTick    // Use Engine Tick hook (once per frame)
    };

    class RC_UE4SS_API SettingsManager
    {
      public:
        struct SectionOverrides
        {
            File::StringType ModsFolderPath{};
            File::StringType ControllingModsTxt{};
        } Overrides;

        struct SectionGeneral
        {
            bool EnableHotReloadSystem{};
            Input::Key HotReloadKey{Input::Key::R};
            bool EnableAutoReloadingLuaMods{};
            bool UseCache{true};
            bool InvalidateCacheIfDLLDiffers{true};
            bool EnableDebugKeyBindings{false};
            int64_t SecondsToScanBeforeGivingUp{30};
            bool UseUObjectArrayCache{true};
            StringType InputSource{STR("Default")};
            bool DoEarlyScan{false};
            bool SearchByAddress{false};
            GameThreadExecutionMethod DefaultExecuteInGameThreadMethod{GameThreadExecutionMethod::EngineTick};
            Unreal::UnrealInitializer::FNameToStringMethod DefaultFNameToStringMethod{Unreal::UnrealInitializer::FNameToStringMethod::Scan};
        } General;

        struct SectionEngineVersionOverride
        {
            int64_t MajorVersion{-1};
            int64_t MinorVersion{-1};
            bool DebugBuild{false};
        } EngineVersionOverride;

        struct SectionObjectDumper
        {
            bool LoadAllAssetsBeforeDumpingObjects{};
            bool UseModuleOffsets{};
        } ObjectDumper;

        struct SectionCXXHeaderGenerator
        {
            bool DumpOffsetsAndSizes{};
            bool KeepMemoryLayout{};
            bool LoadAllAssetsBeforeGeneratingCXXHeaders{};
        } CXXHeaderGenerator;

        struct SectionUHTHeaderGenerator
        {
            bool IgnoreAllCoreEngineModules{};
            bool IgnoreEngineAndCoreUObject{true};
            bool MakeAllFunctionsBlueprintCallable{};
            bool MakeAllPropertyBlueprintsReadWrite{};
            bool MakeEnumClassesBlueprintType{};
            bool MakeAllConfigsEngineConfig{};
        } UHTHeaderGenerator;

        struct SDKGenerator
        {
            StringType OutputPath{};
        } SDKGenerator;

        struct SectionDebug
        {
            bool SimpleConsoleEnabled{true};
            bool DebugConsoleEnabled{true};
            bool DebugConsoleVisible{true};
            float DebugGUIFontScaling{1.0};
            bool DebugGUIUseMonospace{false};
            GUI::GfxBackend GraphicsAPI{GUI::GfxBackend::GLFW3_OpenGL3};
            GUI::RenderMode RenderMode{GUI::RenderMode::ExternalThread};
            Input::Key ToggleGUIKey{Input::Key::O};
            bool ForwardLogToGameConsole{false};
        } Debug;

        struct SectionCrashDump
        {
            bool EnableDumping{true};
            bool FullMemoryDump{false};
        } CrashDump;

        struct SectionThreads
        {
            int64_t SigScannerNumThreads{8};
            int64_t SigScannerMultithreadingModuleSizeThreshold{16777216};
        } Threads;

        struct SectionMemory
        {
            int64_t MaxMemoryUsageDuringAssetLoading{85};
        } Memory;

        struct SectionHooks
        {
            bool HookProcessInternal{true};
            bool HookProcessLocalScriptFunction{true};
            bool HookInitGameState{true};
            bool HookLoadMap{true};
            bool HookCallFunctionByNameWithArguments{true};
            bool HookBeginPlay{true};
            bool HookEndPlay{true};
            bool HookLocalPlayerExec{true};
            bool HookAActorTick{true};
            bool HookEngineTick{true};
            Unreal::UnrealInitializer::FunctionResolveMethod EngineTickResolveMethod{Unreal::UnrealInitializer::FunctionResolveMethod::Scan};
            bool HookGameViewportClientTick{true};
            bool HookUObjectProcessEvent{true};
            bool HookProcessConsoleExec{true};
            bool HookUStructLink{true};
            int64_t FExecVTableOffsetInLocalPlayer{0x28};
        } Hooks;

        // In-process MCP server. Off by default and loopback-only on purpose: it executes
        // arbitrary Lua inside the game, so it is a development tool, not something to leave
        // running during normal play. See deps/first/mcp_bind.
        struct SectionMCP
        {
            bool Enabled{false};
            File::StringType BindAddress{STR("127.0.0.1")};
            int64_t Port{8787};
            // How long a tool call waits for the game thread before giving up. A wedged game
            // thread (mid-map-load, or crashed) must not hang the HTTP request forever.
            int64_t GameThreadTimeoutMs{10000};
        } MCP;

        // Lua-installable detours on arbitrary native addresses (RegisterNativeHook). Nothing is
        // hooked until a mod asks for it, so leaving this enabled costs nothing -- but it is the
        // switch that turns off every such hook at once if one of them is destabilising the game.
        // See UE4SS/include/NativeHook.hpp.
        struct SectionNativeHooks
        {
            bool Enabled{true};
        } NativeHooks;

        struct ExperimentalFeatures
        {
        } Experimental;

      public:
        SettingsManager() = default;

      public:
        auto deserialize(std::filesystem::path& file_name) -> void;
    };
} // namespace RC
