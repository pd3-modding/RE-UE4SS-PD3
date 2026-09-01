#include <NativeHook.hpp>

#include <array>
#include <bit>
#include <cctype>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>
#include <Helpers/UETargetModules.hpp>
#include <SigScanner/SinglePassSigScanner.hpp>
#include <UE4SSProgram.hpp>

#include <polyhook2/Detour/x64Detour.hpp>

namespace RC::NativeHook
{
    namespace
    {
        using ThunkFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t);

        // PolyHook needs a distinct C function per detour and we cannot emit one at runtime, so
        // the thunks are a compile-time pool. Raising this is free; each unused entry is one
        // never-called function.
        constexpr size_t k_max_hooks = 16;

        struct Entry
        {
            bool in_use{};
            std::string name{};
            uint8_t* target{};
            size_t rva{};
            uint64_t trampoline{};
            uint64_t calls{};
            std::unique_ptr<PLH::x64Detour> detour{};
            Dispatcher dispatcher{};
        };

        std::mutex g_mutex{};
        std::array<Entry, k_max_hooks> g_entries{};

        auto dispatch(size_t index, uint64_t a, uint64_t b, uint64_t c, uint64_t d) -> uint64_t
        {
            uint64_t args[4]{a, b, c, d};
            uint64_t trampoline{};
            Dispatcher dispatcher{};

            {
                std::scoped_lock lock{g_mutex};
                auto& entry = g_entries[index];
                if (!entry.in_use)
                {
                    return 0;
                }
                ++entry.calls;
                trampoline = entry.trampoline;
                // Copy the dispatcher so the Lua call happens outside the lock: a callback that
                // registers or removes a hook would otherwise deadlock on this mutex.
                dispatcher = entry.dispatcher;
            }

            if (dispatcher)
            {
                dispatcher(args);
            }

            if (!trampoline)
            {
                return 0;
            }
            return PLH::FnCast(trampoline, ThunkFn{})(args[0], args[1], args[2], args[3]);
        }

        template <size_t Index>
        auto thunk(uint64_t a, uint64_t b, uint64_t c, uint64_t d) -> uint64_t
        {
            return dispatch(Index, a, b, c, d);
        }

        template <size_t... Indices>
        constexpr auto make_thunks(std::index_sequence<Indices...>) -> std::array<ThunkFn, sizeof...(Indices)>
        {
            return {&thunk<Indices>...};
        }

        constexpr auto g_thunks = make_thunks(std::make_index_sequence<k_max_hooks>{});

        struct ParsedPattern
        {
            std::vector<uint8_t> bytes{};
            std::vector<bool> is_wildcard{};

            auto valid() const -> bool
            {
                return !bytes.empty();
            }
        };

        // "48 85 C9 74 ?? 48" -> bytes + wildcard mask. Tolerates '?' and '??'.
        auto parse_pattern(const std::string& text) -> ParsedPattern
        {
            ParsedPattern out{};
            size_t i{};
            while (i < text.size())
            {
                if (std::isspace(static_cast<unsigned char>(text[i])))
                {
                    ++i;
                    continue;
                }
                if (text[i] == '?')
                {
                    out.bytes.push_back(0);
                    out.is_wildcard.push_back(true);
                    while (i < text.size() && text[i] == '?')
                    {
                        ++i;
                    }
                    continue;
                }
                if (i + 1 >= text.size())
                {
                    return {};
                }
                const auto hex = text.substr(i, 2);
                try
                {
                    out.bytes.push_back(static_cast<uint8_t>(std::stoul(hex, nullptr, 16)));
                }
                catch (...)
                {
                    return {};
                }
                out.is_wildcard.push_back(false);
                i += 2;
            }
            return out;
        }

        auto matches(const ParsedPattern& pattern, const uint8_t* at) -> bool
        {
            for (size_t i = 0; i < pattern.bytes.size(); ++i)
            {
                if (!pattern.is_wildcard[i] && at[i] != pattern.bytes[i])
                {
                    return false;
                }
            }
            return true;
        }

        // The RVA is a hint, never a promise: it is accepted only if the bytes there still match
        // the pattern. Detouring the wrong address is silent corruption, so an unverifiable
        // target is refused rather than guessed at.
        auto resolve(const Spec& spec, const uint8_t* base, size_t size) -> uint8_t*
        {
            const auto pattern = parse_pattern(spec.pattern);

            if (!pattern.valid())
            {
                if (spec.rva == 0 || spec.rva >= size)
                {
                    Output::send<LogLevel::Error>(STR("[NativeHook] '{}': no usable pattern and no RVA in range\n"), ensure_str(spec.name));
                    return nullptr;
                }
                Output::send<LogLevel::Warning>(STR("[NativeHook] '{}': no pattern given, trusting RVA {:#x} unverified\n"),
                                                ensure_str(spec.name),
                                                spec.rva);
                return const_cast<uint8_t*>(base + spec.rva);
            }

            if (spec.rva != 0 && spec.rva + pattern.bytes.size() < size && matches(pattern, base + spec.rva))
            {
                return const_cast<uint8_t*>(base + spec.rva);
            }

            if (spec.rva != 0)
            {
                Output::send<LogLevel::Warning>(STR("[NativeHook] '{}': RVA {:#x} no longer matches its pattern; scanning\n"),
                                                ensure_str(spec.name),
                                                spec.rva);
            }

            const uint8_t* found{};
            size_t hits{};
            for (size_t i = 0; i + pattern.bytes.size() < size; ++i)
            {
                if (matches(pattern, base + i))
                {
                    ++hits;
                    if (!found)
                    {
                        found = base + i;
                    }
                    if (hits > 1)
                    {
                        break;
                    }
                }
            }

            if (hits != 1)
            {
                Output::send<LogLevel::Error>(STR("[NativeHook] '{}': pattern matched {} times, need exactly 1 -- not installing\n"),
                                              ensure_str(spec.name),
                                              hits);
                return nullptr;
            }

            Output::send(STR("[NativeHook] '{}': resolved by scan to RVA {:#x} -- pin it to make startup deterministic\n"),
                         ensure_str(spec.name),
                         static_cast<size_t>(found - base));
            return const_cast<uint8_t*>(found);
        }
    } // namespace

    auto install(const Spec& spec, Dispatcher dispatcher) -> int32_t
    {
        if (!UE4SSProgram::settings_manager.NativeHooks.Enabled)
        {
            Output::send<LogLevel::Warning>(STR("[NativeHook] '{}' requested but [NativeHooks] Enabled = 0; ignoring\n"), ensure_str(spec.name));
            return -1;
        }

        const auto& module_info = SigScannerStaticData::m_modules_info.array[static_cast<size_t>(ScanTarget::MainExe)];
        const auto* base = static_cast<const uint8_t*>(module_info.lpBaseOfDll);
        if (!base || module_info.SizeOfImage == 0)
        {
            Output::send<LogLevel::Error>(STR("[NativeHook] main module not resolved; cannot install '{}'\n"), ensure_str(spec.name));
            return -1;
        }

        auto* target = resolve(spec, base, module_info.SizeOfImage);
        if (!target)
        {
            return -1;
        }

        std::scoped_lock lock{g_mutex};

        for (size_t i = 0; i < g_entries.size(); ++i)
        {
            if (g_entries[i].in_use && g_entries[i].target == target)
            {
                g_entries[i].dispatcher = std::move(dispatcher);
                return static_cast<int32_t>(i);
            }
        }

        size_t slot{k_max_hooks};
        for (size_t i = 0; i < g_entries.size(); ++i)
        {
            if (!g_entries[i].in_use)
            {
                slot = i;
                break;
            }
        }
        if (slot == k_max_hooks)
        {
            Output::send<LogLevel::Error>(STR("[NativeHook] out of hook slots ({}); cannot install '{}'\n"), k_max_hooks, ensure_str(spec.name));
            return -1;
        }

        auto& entry = g_entries[slot];
        entry.name = spec.name;
        entry.target = target;
        entry.rva = static_cast<size_t>(target - base);
        entry.calls = 0;
        entry.dispatcher = std::move(dispatcher);
        entry.detour = std::make_unique<PLH::x64Detour>(std::bit_cast<uint64_t>(target), std::bit_cast<uint64_t>(g_thunks[slot]), &entry.trampoline);

        if (!entry.detour->hook())
        {
            Output::send<LogLevel::Error>(STR("[NativeHook] '{}': PolyHook could not detour RVA {:#x} (prologue too short?)\n"),
                                          ensure_str(spec.name),
                                          entry.rva);
            entry = Entry{};
            return -1;
        }

        entry.in_use = true;
        Output::send(STR("[NativeHook] '{}' installed at RVA {:#x} (slot {})\n"), ensure_str(spec.name), entry.rva, slot);
        return static_cast<int32_t>(slot);
    }

    auto uninstall(int32_t id) -> bool
    {
        std::scoped_lock lock{g_mutex};
        if (id < 0 || static_cast<size_t>(id) >= g_entries.size() || !g_entries[id].in_use)
        {
            return false;
        }
        auto& entry = g_entries[id];
        if (entry.detour)
        {
            entry.detour->unHook();
        }
        Output::send(STR("[NativeHook] '{}' removed (called {} times)\n"), ensure_str(entry.name), entry.calls);
        entry = Entry{};
        return true;
    }

    auto uninstall_all() -> void
    {
        for (size_t i = 0; i < g_entries.size(); ++i)
        {
            uninstall(static_cast<int32_t>(i));
        }
    }

    auto resolved_rva(int32_t id) -> size_t
    {
        std::scoped_lock lock{g_mutex};
        if (id < 0 || static_cast<size_t>(id) >= g_entries.size() || !g_entries[id].in_use)
        {
            return 0;
        }
        return g_entries[id].rva;
    }

    auto call_count(int32_t id) -> uint64_t
    {
        std::scoped_lock lock{g_mutex};
        if (id < 0 || static_cast<size_t>(id) >= g_entries.size() || !g_entries[id].in_use)
        {
            return 0;
        }
        return g_entries[id].calls;
    }

    auto max_hooks() -> size_t
    {
        return k_max_hooks;
    }
} // namespace RC::NativeHook
