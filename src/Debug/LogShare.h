// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Debug/LogShare.h
//
// Sends this machine's log to a collector, while the mod is being tested.
//
// WHY IT EXISTS
//
// Every fault worth fixing so far has needed two machines at once, and the person who can
// read the logs is not sitting at either of them. Collecting them by hand after every
// attempt is slow enough that it does not happen, which means the runs that matter most,
// the ones with three or four people in them, are the runs nobody has a record of.
//
// WHAT IT WILL NOT DO
//
// It sends nothing unless the install has been told where to send it. The destination comes
// from a file the tester has in their mod folder, not from anything compiled in, for two
// reasons: an address baked into a distributed binary is an address anybody who downloads
// the mod can post to, and an install that was never set up for testing should behave
// exactly as if this code were not here.
//
// It is also not quiet about it. While sharing is on, the status panel says so on the
// machine doing the sharing, because somebody running a mod should not have to read its
// source to find out it is sending their log somewhere.
//
// It carries no credentials. There is nothing in the binary worth extracting.
//
// THIS IS TEMPORARY
//
// It exists for the testing period and is meant to be deleted, not configured away. When
// the mod stops being a thing under test, this file, its build entry and the status line go
// with it.
#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "Core/Result.h"

namespace mpe::debugshare {

/// Reads the destination from the mod folder. Empty when this install shares nothing.
///
/// The file is MultiplayerEvolved/report.url and holds one https address. Anything else in
/// it, including an http address, is refused: a log crossing the network in the clear is
/// not something to arrange by accident.
[[nodiscard]] std::string ConfiguredEndpoint(const std::filesystem::path& data_directory);

/// True while this install is set up to share, for the status panel to report.
[[nodiscard]] bool SharingEnabled();

/// Starts the background sender. Does nothing when no endpoint is configured.
///
/// Sending happens on its own thread and never on the mod tick, because an upload waits on
/// a network and the tick counts down countdowns.
/// describe is asked for the machine's name on every report rather than once at startup.
/// Sharing begins before Steam is signed in, so a label captured then reads PLAYER (0) on
/// every machine, which defeats the only job a label has.
void Start(const std::filesystem::path& data_directory, std::function<std::string()> describe);

/// Queues the current log to be sent, with a reason recorded alongside it.
///
/// Coalesced: a burst of reasons produces one upload, because the log is cumulative and the
/// last send contains everything the earlier ones would have.
void Queue(std::string reason);

/// Stops the sender and waits for an upload in flight.
void Stop();

} // namespace mpe::debugshare
