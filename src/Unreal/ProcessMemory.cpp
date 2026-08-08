// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Unreal/ProcessMemory.cpp
#include "Unreal/ProcessMemory.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstring>

namespace mpe::unreal::memory {
namespace {

/// Cached bounds of one memory region, per thread.
///
/// Per thread rather than shared, so no lock is needed on the hot path.
///
/// The cache is an optimization, never a safety mechanism. It reports what was true
/// when the region was last queried, and the game changes its address space constantly
/// while loading. GuardedRead is what makes a stale answer harmless.
struct RegionCache {
    std::uintptr_t begin{0};
    std::uintptr_t end{0};
    bool           readable{false};
    bool           writable{false};
    bool           valid{false};
};

thread_local RegionCache g_cache;
thread_local CacheStats  g_stats;

constexpr DWORD kReadableProtect = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                   PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                   PAGE_EXECUTE_WRITECOPY;

constexpr DWORD kWritableProtect = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
                                   PAGE_EXECUTE_WRITECOPY;

/// Resolves the region containing address into the cache.
[[nodiscard]] bool ResolveRegion(std::uintptr_t address) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    ++g_stats.queries;
    if (::VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == 0) {
        g_cache.valid = false;
        return false;
    }

    g_cache.begin = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    g_cache.end   = g_cache.begin + info.RegionSize;
    g_cache.valid = true;

    // A guard page counts as unreadable: touching it raises an exception even though
    // the protection bits otherwise look fine.
    const bool committed = (info.State == MEM_COMMIT);
    const bool guarded   = (info.Protect & PAGE_GUARD) != 0;
    const bool no_access = (info.Protect & PAGE_NOACCESS) != 0;

    g_cache.readable = committed && !guarded && !no_access &&
                       (info.Protect & kReadableProtect) != 0;
    g_cache.writable = committed && !guarded && !no_access &&
                       (info.Protect & kWritableProtect) != 0;
    return true;
}

/// Shared implementation for the readable and writable queries.
[[nodiscard]] bool CheckRange(std::uintptr_t address, std::size_t size, bool need_write) noexcept {
    if (address == 0 || size == 0) {
        return false;
    }
    // Overflow guard: a huge size would otherwise wrap and pass the bounds test.
    if (address + size < address) {
        return false;
    }

    // Fast path: entirely inside the cached region.
    if (g_cache.valid && address >= g_cache.begin && address + size <= g_cache.end) {
        ++g_stats.hits;
        return need_write ? g_cache.writable : g_cache.readable;
    }

    if (!ResolveRegion(address)) {
        return false;
    }
    if (!(need_write ? g_cache.writable : g_cache.readable)) {
        return false;
    }

    // The range may straddle a region boundary. Adjacent regions can have different
    // protection, so each one has to be confirmed rather than assumed.
    if (address + size <= g_cache.end) {
        return true;
    }

    std::uintptr_t cursor    = g_cache.end;
    const std::uintptr_t end = address + size;
    while (cursor < end) {
        if (!ResolveRegion(cursor)) {
            return false;
        }
        if (!(need_write ? g_cache.writable : g_cache.readable)) {
            return false;
        }
        if (g_cache.end <= cursor) {
            return false; // No forward progress; refuse rather than spin.
        }
        cursor = g_cache.end;
    }
    return true;
}

} // namespace

// GuardedRead and GuardedWrite deliberately contain no C++ objects with destructors.
// MSVC forbids __try in a function that requires unwinding, and keeping these two
// functions trivially simple is what allows the handler to exist at all.

bool GuardedRead(std::uintptr_t address, void* destination, std::size_t size) noexcept {
    if (address == 0 || destination == nullptr || size == 0) {
        return false;
    }
    __try {
        std::memcpy(destination, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // The page went away or was reprotected between the advisory check and here.
        // That is expected while the game is loading, and it is exactly the case that
        // used to crash the process.
        return false;
    }
}

bool GuardedWrite(std::uintptr_t address, const void* source, std::size_t size) noexcept {
    if (address == 0 || source == nullptr || size == 0) {
        return false;
    }
    __try {
        std::memcpy(reinterpret_cast<void*>(address), source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/// True when the address is inside a committed, executable page.
///
/// Used to find where a virtual table ends. The entries are function pointers, so the first
/// value that is not code is the first value that is not an entry. Asking the operating
/// system what a page is for is the only way to tell without knowing the class.
bool IsExecutableAddress(std::uintptr_t address) noexcept {
    if (!IsPlausiblePointer(address)) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (::VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == 0) {
        return false;
    }
    if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0 ||
        (info.Protect & PAGE_NOACCESS) != 0) {
        return false;
    }
    constexpr DWORD kExecutable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                  PAGE_EXECUTE_WRITECOPY;
    return (info.Protect & kExecutable) != 0;
}

bool IsReadable(std::uintptr_t address, std::size_t size) noexcept {
    return CheckRange(address, size, false);
}

bool IsWritable(std::uintptr_t address, std::size_t size) noexcept {
    return CheckRange(address, size, true);
}

bool WriteBytes(std::uintptr_t address, const void* source, std::size_t size) noexcept {
    if (address == 0 || source == nullptr || size == 0) {
        return false;
    }
    // Must be committed memory before touching protection. VirtualProtect on a reserved
    // or free region fails, and on a guard page it would arm a fault.
    if (!IsReadable(address, size)) {
        return false;
    }

    DWORD previous = 0;
    if (::VirtualProtect(reinterpret_cast<LPVOID>(address), size, PAGE_READWRITE, &previous) ==
        FALSE) {
        return false;
    }

    const bool stored = GuardedWrite(address, source, size);

    DWORD ignored = 0;
    // Restoring can fail without the write having failed, so its result is not
    // propagated. The cache is invalidated either way because protection changed.
    (void)::VirtualProtect(reinterpret_cast<LPVOID>(address), size, previous, &ignored);
    InvalidateCache();
    return stored;
}

void InvalidateCache() noexcept {
    g_cache = RegionCache{};
}

CacheStats Stats() noexcept {
    return g_stats;
}

void ResetStats() noexcept {
    g_stats = CacheStats{};
}

} // namespace mpe::unreal::memory
