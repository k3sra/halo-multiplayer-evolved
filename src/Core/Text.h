// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Core/Text.h
//
// UTF-8 handling for text that came from somebody else.
//
// WHY THIS EXISTS
//
// Every string this mod draws that a player did not type themselves is UTF-8 from Steam: a
// persona name, a server name, a friend's name. Steam names are the least well behaved
// strings in the process. They carry emoji, decorative brackets, zero width joiners,
// right-to-left marks, and scripts the frontend's font has never heard of, and they are the
// one thing on the screen a player recognises instantly when it is wrong.
//
// Two separate faults came out of not having this.
//
// Widening one byte at a time. Correct for ASCII and wrong for everything else, because it
// turns each byte of a multi byte sequence into its own character. A name with any
// character outside ASCII arrived as a run of garbage glyphs, one per byte, which looks
// exactly like the mod corrupting somebody's name, because it was.
//
// Cutting by bytes. A name clipped to fit a column was clipped mid sequence, leaving a
// partial code point at the end that decodes to nothing and draws as a replacement box.
//
// Both are fixed by working in code points rather than bytes, everywhere, which is what
// this is. It is deliberately free of any platform call so the test harness can run it:
// MultiByteToWideChar would do the widening, but it cannot be linked into a check that runs
// without the game.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mpe::text {

/// Decodes UTF-8 into code points. Invalid bytes become U+FFFD rather than being dropped.
///
/// Kept as its own step because everything below wants code points, and decoding once is
/// both faster and the only way the results can agree with each other.
[[nodiscard]] std::vector<char32_t> DecodeUtf8(std::string_view text);

/// Encodes code points back to UTF-8.
[[nodiscard]] std::string EncodeUtf8(const std::vector<char32_t>& code_points);

/// UTF-8 to the UTF-16 the engine's text conversion expects.
///
/// Surrogate pairs are produced for anything past the basic plane, which is what makes an
/// emoji arrive as one character rather than as an invalid half of one.
[[nodiscard]] std::wstring WidenUtf8(std::string_view text);

/// How many code points a UTF-8 string holds. Not its length in bytes.
[[nodiscard]] std::size_t CountCodePoints(std::string_view text);

/// Cuts to a code point boundary. Never leaves half a character behind.
[[nodiscard]] std::string Truncate(std::string_view text, std::size_t max_code_points);

/// The same, with a trailing ellipsis when anything was cut.
///
/// The ellipsis is three full stops rather than U+2026, because a single glyph is one more
/// thing the font has to have and this is used precisely where the font's coverage is in
/// question.
[[nodiscard]] std::string Ellipsise(std::string_view text, std::size_t max_code_points);

/// True for a code point the frontend's font can be expected to draw.
///
/// The game's menus are set in a Latin display face. It covers Latin, Greek and Cyrillic
/// and nothing else, so a name in any other script draws as a row of empty boxes whatever
/// this code does with it.
[[nodiscard]] bool IsDrawable(char32_t code_point);

/// A name from Steam, made safe to draw and short enough to fit.
///
/// Anything the font cannot draw is dropped rather than turned into a box, runs of spaces
/// left behind by the dropping are collapsed, and the result is cut to a code point
/// boundary with an ellipsis if it is still too long.
///
/// A name that is entirely undrawable becomes "PLAYER". That is a real loss for somebody
/// whose name is written in a script the game does not ship a font for, and it is still the
/// better of the two available answers: the alternative is a slot showing five identical
/// empty rectangles, which names nobody and looks like a fault.
[[nodiscard]] std::string CleanDisplayName(std::string_view raw,
                                           std::size_t      max_code_points);

} // namespace mpe::text
