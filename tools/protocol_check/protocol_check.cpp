// SPDX-License-Identifier: MIT
// MultiplayerEvolved: tools/protocol_check/protocol_check.cpp
//
// Checks the wire protocol rules that cost a two player test.
//
// WHY THIS EXISTS
//
// The first time two players ever reached each other, the client disconnected the host
// over a roster message that arrived a fraction of a second early, then waited out the
// handshake timeout and reported that the host had never replied. Nothing about that was
// visible without two machines, two people and a log from each.
//
// Every rule it broke is a pure function over an enum. They can be checked in a second on
// one machine, and from now on they are.
#include <cstdio>
#include <string>
#include <vector>

#include "Core/ByteStream.h"
#include "Core/Text.h"
#include "Net/PacketProtocol.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("  ok    %s\n", what.c_str());
        return;
    }
    std::printf("  FAIL  %s\n", what.c_str());
    ++g_failures;
}

} // namespace

int main() {
    using namespace mpe;
    using namespace mpe::net;

    std::printf("handshake ordering\n");

    // The bug itself. A host sends HandshakeAccept on the control channel and the roster
    // and settings on the lobby channel, which are separate lanes with no ordering
    // between them, so either can arrive first.
    Check(IsMessageAcceptable(MessageType::RosterUpdate, PeerRole::Client,
                              ProtocolPhase::Handshaking),
          "a client accepts RosterUpdate while handshaking");
    Check(IsMessageAcceptable(MessageType::MatchSettingsSync, PeerRole::Client,
                              ProtocolPhase::Handshaking),
          "a client accepts MatchSettingsSync while handshaking");
    Check(IsMessageAcceptable(MessageType::HandshakeAccept, PeerRole::Client,
                              ProtocolPhase::Handshaking),
          "a client still accepts HandshakeAccept while handshaking");

    // The roster keeps arriving for the whole session, so it must stay acceptable.
    for (const ProtocolPhase phase : {ProtocolPhase::InLobby, ProtocolPhase::DistributingMap,
                                      ProtocolPhase::Loading, ProtocolPhase::InMatch}) {
        Check(IsMessageAcceptable(MessageType::RosterUpdate, PeerRole::Client, phase),
              "a client accepts RosterUpdate in a later phase");
    }

    // Role separation is the part that genuinely protects anything, so it must survive
    // the relaxation above.
    Check(!IsMessageAcceptable(MessageType::HandshakeRequest, PeerRole::Client,
                               ProtocolPhase::InLobby),
          "a client never accepts a client authored HandshakeRequest");
    Check(!IsMessageAcceptable(MessageType::RosterUpdate, PeerRole::Host,
                               ProtocolPhase::InLobby),
          "a host never accepts a host authored RosterUpdate");

    std::printf("handshake accept round trip\n");

    // The host's phase has to survive the wire, or a late joiner lands in a lobby the
    // host left minutes ago.
    for (const ProtocolPhase phase : {ProtocolPhase::InLobby, ProtocolPhase::Loading,
                                      ProtocolPhase::InMatch}) {
        std::vector<std::byte> buffer;
        ByteWriter             writer(buffer);
        HandshakeAcceptBody    sent;
        sent.assigned_slot  = 3;
        sent.assigned_team  = 1;
        sent.host_tick_rate = 60;
        sent.host_phase     = static_cast<std::uint8_t>(phase);
        sent.Write(writer);

        ByteReader reader(buffer);
        const auto read = HandshakeAcceptBody::Read(reader);
        Check(read.ok() && read.value().host_phase == sent.host_phase &&
                  read.value().assigned_slot == sent.assigned_slot &&
                  read.value().assigned_team == sent.assigned_team,
              "HandshakeAccept carries the host phase intact");
    }

    // A body without the phase must read as a lobby rather than as malformed, so the
    // field can be added without the reader becoming brittle about length.
    {
        std::vector<std::byte> buffer;
        ByteWriter             writer(buffer);
        writer.WriteU8(2);
        writer.WriteU8(0);
        writer.WriteU32(60);

        ByteReader reader(buffer);
        const auto read = HandshakeAcceptBody::Read(reader);
        Check(read.ok() &&
                  read.value().host_phase == static_cast<std::uint8_t>(ProtocolPhase::InLobby),
              "a HandshakeAccept without a phase reads as a lobby");
    }

    std::printf("text that came from somebody else\n");
    {
        using namespace mpe::text;

        // The mangling itself. A Steam persona name is UTF-8, and widening it one byte at a
        // time turned every multi byte character into a run of garbage glyphs. These are the
        // decorative brackets that surrounded a real player's name when it drew wrongly.
        const std::string decorated = "\xEA\xA7\x81 Nessie \xEA\xA7\x82";
        Check(CountCodePoints(decorated) == 10,
              "a decorated name is ten code points, not fourteen bytes");
        Check(WidenUtf8(decorated).size() == 10,
              "widening yields one wide character per code point");
        Check(WidenUtf8("plain").size() == 5, "ASCII widens unchanged");

        // Past the basic plane, which is where every emoji lives. One code point, two wide
        // characters, because UTF-16 needs a surrogate pair for it.
        const std::string emoji = "\xF0\x9F\x8E\xAE";
        Check(CountCodePoints(emoji) == 1, "an emoji is one code point");
        Check(WidenUtf8(emoji).size() == 2, "an emoji widens to a surrogate pair");

        // Round tripping, which is what proves the decoder and the encoder agree.
        Check(EncodeUtf8(DecodeUtf8(decorated)) == decorated,
              "decoding and re-encoding a name gives it back unchanged");

        // Cutting by code points rather than bytes. A byte wise cut at four would land
        // inside the first character and leave half of it behind.
        Check(Truncate(decorated, 2) == "\xEA\xA7\x81 ",
              "truncation lands on a code point boundary");
        Check(CountCodePoints(Ellipsise("abcdefghij", 6)) == 6,
              "an ellipsis fits inside the limit rather than being added past it");
        Check(Ellipsise("abcdefghij", 6) == "abc...", "the ellipsis replaces what was cut");
        Check(Ellipsise("short", 12) == "short", "nothing under the limit is touched");

        // What the frontend's font can draw. Latin, Greek and Cyrillic; not emoji, and not
        // the decorative brackets that started this.
        Check(IsDrawable(U'A') && IsDrawable(U'\u00E9') && IsDrawable(U'\u0416'),
              "Latin, accented Latin and Cyrillic are drawable");
        Check(!IsDrawable(U'\U0001F3AE') && !IsDrawable(U'\u0007'),
              "an emoji and a control character are not");

        // The whole point: the name a player recognises, out of what Steam handed over.
        Check(CleanDisplayName(decorated, 13) == "Nessie",
              "a decorated name cleans to the name inside it");
        Check(CleanDisplayName("  spaced   out  ", 20) == "spaced out",
              "runs of space left by dropping are collapsed");
        Check(CleanDisplayName("\xF0\x9F\x8E\xAE", 13) == "PLAYER",
              "a name with nothing drawable in it falls back rather than drawing boxes");
        Check(CountCodePoints(CleanDisplayName("Averyveryverylongpersonaname", 13)) == 13,
              "a long name is cut to the width of the card it goes on");
        Check(CleanDisplayName("", 13) == "PLAYER", "an empty name falls back too");

        // A truncated multi byte sequence is what a byte wise cut leaves behind. It has to
        // decode to something rather than derailing the rest of the string.
        Check(CountCodePoints("\xEA\xA7") == 2,
              "a half written character decodes without eating what follows");
    }

    std::printf("versioning\n");
    Check(kProtocolVersion >= 2,
          "the protocol version was bumped past the build that hangs up on a roster");

    std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
