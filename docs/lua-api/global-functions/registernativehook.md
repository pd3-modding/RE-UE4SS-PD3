# RegisterNativeHook

Detours an arbitrary native function **by address** and calls back into Lua.

This is the general form of `RegisterLoadMapPreHook`, `RegisterBeginPlayPreHook` and the ~20 hooks
like them: each of those is a hand-written binding to one known engine function, this one takes
the address as data. It reaches engine code that has no reflection data and therefore cannot be
hooked with `RegisterHook`, which is UFunction-only.

Requires `[NativeHooks] Enabled = 1` in `UE4SS-settings.ini` (the default). Nothing is hooked
until a mod calls this, so leaving the setting on costs nothing.

## Constraints — read these first

The detour has **one fixed shape**:

```cpp
uint64_t f(uint64_t rcx, uint64_t rdx, uint64_t r8, uint64_t r9)
```

That covers Win64 functions taking **up to four integer/pointer arguments** and returning an
integer or nothing. It does **not** cover float/SSE arguments, a fifth argument onwards (passed on
the stack), or a struct-by-value return. Hooking a function outside that shape corrupts the call,
and nothing in this layer can detect it — you have to know the target's signature.

The callback runs **on whatever thread the target runs on**, inside whatever engine call path the
target lives in. Hook cold functions. A target that fires every frame marshals into Lua every
frame.

At most **16** native hooks can be installed at once (PolyHook needs a distinct C function per
detour, and one cannot be emitted at runtime, so the thunks are a compile-time pool).

## Parameters

| # | Type     | Information |
|---|----------|-------------|
| 1 | table    | Spec, see below |
| 2 | function | The callback |

### Spec

| Key       | Type    | Information |
|-----------|---------|-------------|
| `Name`    | string  | Required. Used only in log output. |
| `RVA`     | integer | Optional. A **hint**: used only if the bytes there still match `Pattern`. |
| `Pattern` | string  | Optional. `"48 85 C9 74 ?? 48 89"` — `??` is a wildcard byte. |

Give **both**. The RVA makes startup deterministic; the pattern proves it is still the right
function after a game update. If the RVA no longer matches, the image is scanned and the resolved
RVA is logged so you can re-pin it. An address that matches neither is refused rather than guessed
at — detouring the wrong function is silent corruption.

Registering a second hook on an address that is already hooked adds your callback to the existing
detour rather than creating a new one.

## Callback Parameters

| # | Type    | Information |
|---|---------|-------------|
| 1-4 | integer | The four Win64 integer argument registers (RCX, RDX, R8, R9) |

## Callback Return Value

| # | Type    | Information |
|---|---------|-------------|
| 1-4 | integer or nil | Replacement arguments. `nil` leaves that argument alone. |

Replacements chain: what one callback returns is what the next callback on the same address sees,
and what the original function is finally called with.

## Return Value

| # | Type    | Information |
|---|---------|-------------|
| 1 | integer | Hook id, or `-1` on failure (the reason is logged) |

## Related functions

| Function | Information |
|---|---|
| `UnregisterNativeHook(id)` | Removes this mod's callbacks, and the detour once none are left. Call from `RegisterModUnload`. |
| `GetNativeHookCallCount(id)` | How many times the target has been called. `0` means the detour is in but that code path never ran — usually a timing mistake, not a bad address. |
| `NativeToProperty(address)` | Wraps a raw address as an `FProperty` so a hook receiving one can read `GetFullName()`. Returns `nil` for a null address. **Cannot** check that the address really is an `FProperty`. |
| `NativeAlloc(size)` / `NativeFree(address)` | Raw allocation, zeroed. Yours to free; not tracked per mod. |
| `NativeCopy(dst, src, size)` | `memcpy`. |
| `NativeReadI32` / `NativeWriteI32` / `NativeReadU64` / `NativeWriteU64` | Raw memory access. |

Every one of the raw-memory functions will crash the process on a bad address, and `pcall` will
not catch it.

## Example

Rewriting an argument in a copy of the struct the caller passed, rather than in place — because
the caller may reuse that struct for the calls that follow:

```lua
local PARAMS_SIZE = 0x18
local scratch = NativeAlloc(PARAMS_SIZE)

local id = RegisterNativeHook({
    Name    = "RegisterReplicatedLifetimeProperty",
    RVA     = 0x038B1828,
    Pattern = "48 85 C9 74 ?? 48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 8B F2",
}, function(property_address, out_props, params_address)
    if property_address == 0 or params_address == 0 then return end

    local property = NativeToProperty(property_address)
    if not property then return end

    -- "BoolProperty /Script/Starbreeze.SBZCharacter:bIsAlive"
    if not property:GetFullName():match(":bIsAlive$") then return end

    NativeCopy(scratch, params_address, PARAMS_SIZE)
    NativeWriteI32(scratch, 0)          -- ELifetimeCondition COND_None
    return nil, nil, scratch            -- replace argument 3 only
end)

RegisterModUnload(function()
    UnregisterNativeHook(id)
    NativeFree(scratch)
end)
```
