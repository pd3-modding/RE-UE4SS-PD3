#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace RC::NativeHook
{
    // Detours an arbitrary native function by address, so a mod can intercept engine code that
    // has no reflection data and therefore cannot be reached by RegisterHook (which is
    // UFunction-only). Everything else in UE4SS that hooks native code -- RegisterLoadMapPreHook,
    // RegisterBeginPlayPreHook, and the rest -- is a hand-written binding to one known function.
    // This is the general form of that.
    //
    // CALLING CONVENTION. The detour has one fixed shape:
    //
    //     uint64_t f(uint64_t rcx, uint64_t rdx, uint64_t r8, uint64_t r9)
    //
    // which covers Win64 functions taking up to four integer/pointer arguments and returning an
    // integer or nothing. It does NOT cover float/SSE arguments, arguments passed on the stack
    // (a fifth onwards), or struct-by-value returns. Hooking a function outside that shape will
    // corrupt the call. There is no way for this layer to check that -- the caller has to know
    // the target's signature.
    //
    // SAFETY. This runs arbitrary Lua on whatever thread the target runs on, inside whatever
    // engine call path the target lives in. A target that fires every frame will marshal into
    // Lua every frame. Pick cold functions.

    // Receives the four argument registers and may overwrite them in place; the original is then
    // called with whatever is left in `args`.
    using Dispatcher = std::function<void(uint64_t args[4])>;

    struct Spec
    {
        std::string name{};
        // Hint. Used only if the bytes there match `pattern` (or if no pattern is given).
        // 0 means "no hint, scan".
        size_t rva{};
        // "48 85 C9 74 ?? 48 89 5C 24 08" -- ?? is a wildcard byte. Empty means "trust the RVA".
        // Giving both is strongly preferred: the RVA makes startup deterministic, the pattern
        // proves it is still the right function after a game update.
        std::string pattern{};
    };

    // Returns a hook id, or -1 on failure (the reason is logged). Installing over an address
    // that is already hooked returns the existing id and replaces its dispatcher.
    auto install(const Spec& spec, Dispatcher dispatcher) -> int32_t;

    auto uninstall(int32_t id) -> bool;
    auto uninstall_all() -> void;

    // RVA the hook actually resolved to, for logging back to the user so they can pin it.
    auto resolved_rva(int32_t id) -> size_t;
    // How many times the target has been called since the hook was installed. 0 means the hook
    // is in place but the code path never ran -- usually a timing mistake, not a bad address.
    auto call_count(int32_t id) -> uint64_t;

    auto max_hooks() -> size_t;
} // namespace RC::NativeHook
