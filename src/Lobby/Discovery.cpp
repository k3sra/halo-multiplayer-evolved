// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Lobby/Discovery.cpp
#include "Lobby/Discovery.h"

namespace mpe::lobby {

std::string_view ToString(ListingVerdict verdict) noexcept {
    switch (verdict) {
        case ListingVerdict::Listed:        return "listed";
        case ListingVerdict::OwnSession:    return "own session";
        case ListingVerdict::NotOurs:       return "not ours";
        case ListingVerdict::WrongProtocol: return "wrong protocol";
    }
    return "unknown";
}

ListingVerdict JudgeListing(const RawListing& listing, std::uint64_t current_lobby,
                            std::string_view expected_marker) noexcept {
    // Own session first, because it is ours and it does carry the marker: testing the
    // marker first would list it and then have to take it out again.
    if (listing.id != 0 && listing.id == current_lobby) {
        return ListingVerdict::OwnSession;
    }
    if (listing.host_id.empty()) {
        return ListingVerdict::NotOurs;
    }
    // An empty expected marker means the caller has not been told what to look for, which
    // is not the lobby's fault; anything carrying a host id is accepted rather than the
    // browser going silently empty.
    if (!expected_marker.empty() && listing.marker != expected_marker) {
        return ListingVerdict::WrongProtocol;
    }
    return ListingVerdict::Listed;
}

std::string_view SessionStatusFromPhase(std::string_view phase) noexcept {
    if (phase == "in_match" || phase == "loading" || phase == "countdown" ||
        phase == "post_match") {
        return "IN GAME";
    }
    return "LOBBY";
}

int FriendPageCount(std::size_t friend_count, int rows_per_page) noexcept {
    if (rows_per_page <= 0) {
        return 1;
    }
    const std::size_t rows  = static_cast<std::size_t>(rows_per_page);
    const std::size_t pages = (friend_count + rows - 1) / rows;
    return pages == 0 ? 1 : static_cast<int>(pages);
}

std::size_t FriendIndexFor(int page, int row, std::size_t friend_count,
                           int rows_per_page) noexcept {
    if (page < 0 || row < 0 || rows_per_page <= 0 || row >= rows_per_page) {
        return static_cast<std::size_t>(-1);
    }
    const std::size_t index = static_cast<std::size_t>(page) *
                                  static_cast<std::size_t>(rows_per_page) +
                              static_cast<std::size_t>(row);
    return index < friend_count ? index : static_cast<std::size_t>(-1);
}

int StepFriendPage(int page, int direction, std::size_t friend_count,
                   int rows_per_page) noexcept {
    const int pages = FriendPageCount(friend_count, rows_per_page);
    if (pages <= 1) {
        return 0;
    }
    // Modulo of a negative is negative in C++, so the page count is added before the
    // remainder is taken. Without that, paging back from the first page lands on a
    // negative page and every row reads as empty.
    const int stepped = ((page + direction) % pages + pages) % pages;
    return stepped;
}

} // namespace mpe::lobby
