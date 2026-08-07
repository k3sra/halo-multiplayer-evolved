// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Lobby/Discovery.cpp
#include "Lobby/Discovery.h"

#include <cstdint>

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

PingBand BandForPing(int ping_milliseconds) noexcept {
    if (ping_milliseconds < 0) {
        return PingBand::Unknown;
    }
    if (ping_milliseconds <= 100) {
        return PingBand::Good;
    }
    if (ping_milliseconds <= 150) {
        return PingBand::Fair;
    }
    return PingBand::Poor;
}

FaultRecovery JudgeFault(std::int64_t elapsed_milliseconds,
                         std::int64_t linger_milliseconds) noexcept {
    FaultRecovery recovery;
    if (linger_milliseconds <= 0 || elapsed_milliseconds >= linger_milliseconds) {
        recovery.recover_now = true;
        return recovery;
    }

    // Rounded up, so a countdown starting at ten seconds shows ten rather than nine, and
    // reaches one rather than zero before it acts. A visible zero that lingers reads as a
    // countdown that has stalled.
    const std::int64_t left = linger_milliseconds - elapsed_milliseconds;
    recovery.seconds_remaining = static_cast<int>((left + 999) / 1000);
    return recovery;
}

LoadingBand BandForLoadingStep(LoadingStep step) noexcept {
    // Contiguous and ascending, which is the whole point. A bar that restarts at every step
    // tells a player the wait got longer; one that carries on tells them how much of the
    // whole thing is left, which is what they are asking.
    //
    // Loading the map is the one step marked unmeasurable. The engine's campaign entry
    // point commits to the load and returns without reporting a fraction, so there is
    // nothing to divide. Saying so here is what lets the screen show motion instead of a
    // number, rather than each caller inventing its own answer to the same question.
    switch (step) {
        case LoadingStep::JoiningLobby:      return {0, 30, true};
        case LoadingStep::StartingSession:   return {30, 55, true};
        case LoadingStep::LoadingMap:        return {55, 75, false};
        case LoadingStep::WaitingForPlayers: return {75, 100, true};
        case LoadingStep::None:
        default:                             return {0, 0, true};
    }
}

int LoadingPercent(LoadingStep step, int done, int total) noexcept {
    const LoadingBand band = BandForLoadingStep(step);
    if (!band.measurable || total <= 0) {
        return band.begin;
    }
    const int clamped = done < 0 ? 0 : (done > total ? total : done);
    return band.begin + ((band.end - band.begin) * clamped) / total;
}

} // namespace mpe::lobby
