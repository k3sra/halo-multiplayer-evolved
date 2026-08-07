// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Unreal/LoadingLines.h
//
// The line under the loading screen's title.
//
// WHY THESE AND NOT THE REAL ONES
//
// Every line here is written for this mod. None of it is dialogue from any Halo game, and
// none of it should be replaced with dialogue from one: the games' scripts are somebody
// else's work, and a hundred quotes lifted out of them is a hundred quotes lifted out of
// them however affectionately it is meant. What they have in common with the originals is
// the register, which is the part worth having anyway: grunts who would rather be anywhere
// else, marines who have stopped being surprised, and elites who take everything far too
// seriously.
//
// WHY THEY REPLACED THE EXPLANATION
//
// The line used to describe the step being waited on, which was worth having when a step
// could hang and say nothing. Steps report themselves now: the title names the stage, the
// bar carries the fraction, the clock carries the time, and anything actually wrong takes
// this line over. Explaining a thing the screen already shows is just words to read.
#pragma once

#include <cstdint>
#include <string_view>

namespace mpe::unreal {

/// A line for the loading screen, chosen by an index that need not be in range.
[[nodiscard]] std::string_view LoadingLine(std::uint32_t index) noexcept;

/// How many there are.
[[nodiscard]] std::size_t LoadingLineCount() noexcept;

} // namespace mpe::unreal
