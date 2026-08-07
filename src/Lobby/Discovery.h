// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Lobby/Discovery.h
//
// The decisions the server browser and the invite list make, with no platform underneath.
//
// WHY THIS EXISTS
//
// These rules were written inline, inside functions that also call Steam. That made them
// untestable for a bad reason: not because they depend on Steam, but because they were
// standing next to something that does. Two of them have already been wrong in ways nobody
// could see.
//
// The browser discarded every lobby whose marker key was empty, and nothing had ever
// written a key by that name, so it could not return a row under any circumstance. The
// invite list indexes a paged friends list by arithmetic that has no visible symptom when
// it is wrong: it invites the wrong person.
//
// Neither needs Steam to be checked. What needs Steam is fetching the lobbies and sending
// the invitation, which is one call each; everything either of them decides is here, and
// is covered by tools/session_check.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mpe::lobby {

/// A lobby as the platform describes it, before this mod decides what to do with it.
struct RawListing {
    std::uint64_t id{0};
    /// The host's platform id as text. Present on every lobby this mod created.
    std::string host_id;
    /// The marker key's value, which is the protocol version this lobby speaks.
    std::string marker;
};

/// Why a lobby is not shown, or Listed when it is.
enum class ListingVerdict : std::uint8_t {
    Listed = 0,
    /// This machine's own session. Joining it would tear down the transport it is
    /// listening on and leave a roster with one player who is both host and client.
    OwnSession,
    /// Not one of ours. The game opens Steam lobbies for its own co-op fireteams and they
    /// arrive in the same search results.
    NotOurs,
    /// Ours, but speaking a protocol this build cannot complete a handshake with. Better
    /// not listed than listed and unjoinable for a reason the player cannot see.
    WrongProtocol,
};

[[nodiscard]] std::string_view ToString(ListingVerdict verdict) noexcept;

/// Decides whether one lobby belongs in the server browser.
///
/// expected_marker is this build's protocol version as text. A lobby with no host id is
/// not ours whatever else it carries, because that key is written by this mod before
/// anybody is told the lobby exists.
[[nodiscard]] ListingVerdict JudgeListing(const RawListing& listing,
                                          std::uint64_t     current_lobby,
                                          std::string_view  expected_marker) noexcept;

/// Turns the phase a host publishes into the two answers a player is choosing between.
///
/// A lobby is somewhere to go now; a match in progress is somewhere to go if arriving late
/// is acceptable. Anything the host has not published yet reads as a lobby, because a
/// session that has only just been created is one.
[[nodiscard]] std::string_view SessionStatusFromPhase(std::string_view phase) noexcept;

/// Where a row on a page of the invite list lands in the friends list.
///
/// Returns npos when the row is past the end, which happens on the last page of a list
/// that does not divide evenly. Getting this wrong invites somebody other than the person
/// whose name was pressed, and nothing on screen would say so.
[[nodiscard]] std::size_t FriendIndexFor(int page, int row, std::size_t friend_count,
                                         int rows_per_page) noexcept;

/// How many pages a friends list needs. Always at least one, so an empty list still has a
/// page to be looking at.
[[nodiscard]] int FriendPageCount(std::size_t friend_count, int rows_per_page) noexcept;

/// The page reached by stepping direction pages from page, wrapping at both ends.
///
/// Wraps rather than stopping, so neither paging button is ever a control that looks
/// pressable and does nothing.
[[nodiscard]] int StepFriendPage(int page, int direction, std::size_t friend_count,
                                 int rows_per_page) noexcept;

} // namespace mpe::lobby
