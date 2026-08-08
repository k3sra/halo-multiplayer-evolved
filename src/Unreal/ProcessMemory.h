// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Unreal/ProcessMemory.h
//
// Safe reads of arbitrary process memory, with a region cache.
//
// WHY THIS EXISTS
//
// Locating UE globals means dereferencing candidate pointers by the hundreds of
// thousands. Two constraints collide:
//
//   Correctness. A candidate pointer is usually garbage. Dereferencing it blindly
//   faults, and a fault on the game's own thread is a crash the player sees. Every
//   read has to be checked first.
//
//   Speed. The first implementation called VirtualQuery for every check, which made
//   one scan of the executable's 8 MB .data take 18 seconds. That is measured, not
//   estimated: "inspected 487347 candidate slot(s)" in 18 s.
//
// The fix is that VirtualQuery already returns the bounds of the whole region
// containing an address, so one call answers every subsequent question about that
// region. Caching the last resolved region collapses a scan of adjacent addresses to
// a single query, because a linear scan stays inside one region for thousands of
// consecutive reads.
//
// The cache is per thread. Region protection can change under us, but only in ways
// that make a previously readable page unreadable, which the structured exception
// handler in ReadStruct catches. That combination is what makes this both fast and
// safe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>

namespace mpe::unreal::memory {

/// Copies memory under a structured exception handler, returning false if the read
/// faults.
///
/// THIS IS THE ACTUAL SAFETY GUARANTEE, and its absence was a real bug.
///
/// IsReadable is only ever advisory. The game allocates and frees constantly while it
/// loads, so between a check and the read that follows it, a page can be freed or
/// reprotected. The region cache below widened that window enormously, from one
/// VirtualQuery per read to one per region. The result was access violations inside the
/// game process during startup, which present as the game dying on its splash screen.
///
/// An earlier version of this header claimed a structured exception handler protected
/// these reads. It did not exist. It does now, and it is what makes scanning a live,
/// actively allocating process safe rather than merely usually safe.
[[nodiscard]] bool GuardedRead(std::uintptr_t address, void* destination,
                               std::size_t size) noexcept;

/// Guarded store. Same rationale as GuardedRead.
[[nodiscard]] bool GuardedWrite(std::uintptr_t address, const void* source,
                                std::size_t size) noexcept;

/// Cheap advisory filter: true when size bytes at address were committed and readable
/// at the moment of asking.
///
/// Never treat a true result as a guarantee. Use it to skip obviously bad candidates
/// quickly, then let GuardedRead be the thing that cannot crash.
[[nodiscard]] bool IsReadable(std::uintptr_t address, std::size_t size) noexcept;

/// True when size bytes at address are committed and writable.
[[nodiscard]] bool IsWritable(std::uintptr_t address, std::size_t size) noexcept;

/// Clears the per thread region cache.
///
/// Call after anything that could change page protection, such as the game loading a
/// level, so a stale region is never trusted.
void InvalidateCache() noexcept;

/// Statistics, for confirming the cache is actually working.
struct CacheStats {
    std::uint64_t queries{0};   ///< VirtualQuery calls actually made.
    std::uint64_t hits{0};      ///< Checks answered from the cache.
};

[[nodiscard]] CacheStats Stats() noexcept;
void ResetStats() noexcept;

/// Reads a trivially copyable value, returning nullopt when the source is unreadable.
///
/// Checked and then copied. The check is not merely advisory: it is the difference
/// between a failed lookup and a crash in the middle of the game's frame.
template <typename T>
[[nodiscard]] std::optional<T> Read(std::uintptr_t address) noexcept {
    static_assert(std::is_trivially_copyable_v<T>, "Read requires a trivially copyable type");
    // The advisory check first, because it rejects most garbage candidates without
    // entering a try block. GuardedRead is what actually prevents a crash.
    if (!IsReadable(address, sizeof(T))) {
        return std::nullopt;
    }
    T value{};
    if (!GuardedRead(address, &value, sizeof(T))) {
        return std::nullopt;
    }
    return value;
}

/// Reads a pointer.
[[nodiscard]] inline std::optional<std::uintptr_t> ReadPointer(std::uintptr_t address) noexcept {
    return Read<std::uintptr_t>(address);
}

/// Writes bytes, adjusting page protection for the duration of the store.
///
/// Fails rather than crashing when the target is not writable. Protection is restored
/// afterwards: leaving a page more permissive than the engine expects is the kind of
/// difference that turns into a mysterious crash an hour later.
[[nodiscard]] bool WriteBytes(std::uintptr_t address, const void* source,
                              std::size_t size) noexcept;

/// Writes a trivially copyable value.
template <typename T>
[[nodiscard]] bool Write(std::uintptr_t address, const T& value) noexcept {
    static_assert(std::is_trivially_copyable_v<T>, "Write requires a trivially copyable type");
    return WriteBytes(address, &value, sizeof(T));
}

/// True when the address looks like it could be a heap or module pointer at all.
///
/// A cheap pre-filter applied before the VirtualQuery path. On Win64 user space ends
/// well below 0x8000'0000'0000, and anything in the first 64 KB is the reserved null
/// region. Rejecting those two cases discards the overwhelming majority of garbage
/// candidates without a syscall.
/// True when the address is inside a committed, executable page.
///
/// Used to find where a virtual table ends: its entries are function pointers, so the first
/// value that is not code is the first value that is not an entry.
[[nodiscard]] bool IsExecutableAddress(std::uintptr_t address) noexcept;

[[nodiscard]] constexpr bool IsPlausiblePointer(std::uintptr_t address) noexcept {
    return address >= 0x10000u && address < 0x7FFFFFFFFFFFu && (address & 0x7u) == 0u;
}

} // namespace mpe::unreal::memory
