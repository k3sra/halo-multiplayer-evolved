// SPDX-License-Identifier: MIT
// ForgeEvolved: Update/UpdateCheck.h
//
// Asks GitHub what the newest published release is.
//
// WHY THIS EXISTS
//
// A mod that talks to other players over the network is only useful when everybody is
// running a build that agrees on the protocol. Telling a player their copy is out of date
// is the cheapest way to prevent a session that fails for reasons nobody can see, so the
// lobby reports it and the loader can act on it.
//
// Deliberately small: one HTTPS GET against the public releases endpoint, parsed with the
// project's own JSON reader. No third party dependency, no token, nothing written to disk.
#pragma once

#include <string>

#include "Core/Result.h"

namespace fe::update {

/// A published release, as GitHub describes it.
struct ReleaseInfo {
    /// The tag with any leading v removed, so it compares against the built in version.
    std::string version;
    /// Direct download for the release asset, empty when the release publishes none.
    std::string download_url;
    std::string asset_name;
    long long   asset_bytes{0};
};

/// Fetches the newest release for a repository, for example "k3sra/halo-multiplayer-evolved".
///
/// Blocking: performs network I/O and must not be called from the game thread. Fails rather
/// than throwing, and a failure is not an error worth surfacing to the player: being unable
/// to reach GitHub means the version is simply unknown, not that anything is wrong.
[[nodiscard]] Expected<ReleaseInfo> FetchLatestRelease(std::string_view repository);

/// Compares two dotted version strings.
///
/// Returns true when candidate is newer than current. Compares numerically component by
/// component, so 0.10.0 is correctly newer than 0.9.0, which a string comparison would get
/// backwards.
[[nodiscard]] bool IsNewer(std::string_view candidate, std::string_view current);

} // namespace fe::update
