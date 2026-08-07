// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Unreal/LoadingLines.cpp
#include "Unreal/LoadingLines.h"

#include <array>

namespace mpe::unreal {
namespace {

/// Written for this mod, in the register of the originals rather than out of them.
///
/// Kept in one array so the count is whatever is in it and nothing has to be updated in two
/// places when a line is added.
constexpr std::array kLines = {
    // Grunts, who would rather be anywhere else.
    std::string_view{"The grunts have been told this is a training exercise."},
    std::string_view{"A grunt has volunteered. A grunt has been volunteered."},
    std::string_view{"Someone gave the grunts grenades again."},
    std::string_view{"The grunts are asleep. Nobody wants to be the one to wake them."},
    std::string_view{"A grunt is asking whether anyone checked the map first."},
    std::string_view{"The grunts have formed a committee about the respawn timer."},
    std::string_view{"Two grunts are arguing over who gets the good plasma pistol."},
    std::string_view{"A grunt has read the rules of engagement and has notes."},
    std::string_view{"The grunts would like it on record that they were against this."},
    std::string_view{"One grunt is pretending to be unconscious. It is working."},
    std::string_view{"The grunts have been issued helmets. Morale is unchanged."},
    std::string_view{"A grunt has asked to be transferred to a quieter ring."},
    std::string_view{"The grunts are counting. They keep getting different answers."},
    std::string_view{"A grunt has found the horn. Nobody has found the grunt."},
    std::string_view{"The grunts are practising running away in formation."},
    std::string_view{"A grunt has been promoted. Nobody has explained to what."},
    std::string_view{"The grunts have hidden the ammunition somewhere safe from themselves."},
    std::string_view{"A grunt is writing home. It is mostly complaints about the food."},
    std::string_view{"The grunts have been reminded which end of the rifle is which."},
    std::string_view{"A grunt has queried the plan. There was no plan."},
    std::string_view{"The grunts are on a break. They have been on a break for some time."},
    std::string_view{"A grunt has volunteered to go first, having misheard the question."},
    std::string_view{"The grunts have agreed to attack in whichever order feels safest."},
    std::string_view{"A grunt is asking whether the shields come in his size."},
    std::string_view{"The grunts have painted their armour. Command has questions."},

    // Marines, who have stopped being surprised by any of it.
    std::string_view{"The marines have seen worse. The marines say that a lot."},
    std::string_view{"Somebody left the warthog running. Again."},
    std::string_view{"A marine has claimed the turret. Nobody is arguing."},
    std::string_view{"The marines are checking their ammunition for the fourth time."},
    std::string_view{"A marine has asked how bad it is. Nobody has answered."},
    std::string_view{"The marines have taken bets on how long the vehicles last."},
    std::string_view{"Somebody is explaining the objective to somebody who is not listening."},
    std::string_view{"A marine has volunteered to drive. Everyone else has volunteered to walk."},
    std::string_view{"The marines have decided the plan is fine and are not discussing it further."},
    std::string_view{"A marine is checking whether the radio still works. It does not."},
    std::string_view{"The marines have found the ammunition. It is the wrong ammunition."},
    std::string_view{"Somebody has reversed the warthog into the only tree on the map."},
    std::string_view{"A marine has been told to hold this position and is holding it."},
    std::string_view{"The marines are quietly hoping this is the last ring."},
    std::string_view{"A marine has spotted something. The marine would rather not have."},
    std::string_view{"The marines have agreed that whoever laughs first drives."},
    std::string_view{"Somebody has counted the grenades and rounded down out of pessimism."},
    std::string_view{"A marine is explaining the flag rules to a marine who wrote them."},
    std::string_view{"The marines have finished briefing. Nobody took notes."},
    std::string_view{"A marine has asked for air support and been offered encouragement."},
    std::string_view{"The marines are pretending the shield recharge sound is not comforting."},
    std::string_view{"Somebody has taken the good rifle and will not admit it."},
    std::string_view{"A marine has been assigned to the passenger seat and is at peace with it."},
    std::string_view{"The marines have agreed on a rally point nobody can find."},
    std::string_view{"A marine has asked what the plan is if the plan does not work."},

    // Elites, who take all of this extremely seriously.
    std::string_view{"An elite is sharpening something that does not need sharpening."},
    std::string_view{"The elites consider this a matter of honour. The elites consider everything a matter of honour."},
    std::string_view{"An elite has declared the map unworthy and is loading it anyway."},
    std::string_view{"The elites have delivered a speech. It is still going."},
    std::string_view{"An elite is insisting the duel be conducted properly."},
    std::string_view{"The elites have taken issue with the respawn rules on principle."},
    std::string_view{"An elite has sworn an oath about a game of capture the flag."},
    std::string_view{"The elites are arranging themselves impressively."},
    std::string_view{"An elite has demanded a worthier opponent and been given a grunt."},
    std::string_view{"The elites are debating whether the sword counts as a formality."},
    std::string_view{"An elite has forgiven somebody dramatically and at length."},
    std::string_view{"The elites have agreed the flag is a symbol and must be carried nobly."},
    std::string_view{"An elite is standing on a hill for reasons of composition."},
    std::string_view{"The elites have decided the loser will speak of this to no one."},
    std::string_view{"An elite has taken offence at the scoreboard."},
    std::string_view{"The elites are observing a moment of silence for the vehicles."},
    std::string_view{"An elite is explaining honour to somebody holding a rocket launcher."},
    std::string_view{"The elites have refused the shortcut on principle."},
    std::string_view{"An elite has named their weapon. The weapon has not been consulted."},
    std::string_view{"The elites are waiting for a dramatic moment to arrive."},

    // The ring, the rest of it, and the machinery.
    std::string_view{"The ring has been asked to hold still for a moment."},
    std::string_view{"Somewhere, a door is opening that has not opened in a very long time."},
    std::string_view{"The monitor would like you to know this is all within parameters."},
    std::string_view{"Something ancient is being switched on by someone recent."},
    std::string_view{"The installation is running diagnostics it does not intend to share."},
    std::string_view{"A very old machine is being asked a very simple question."},
    std::string_view{"The forerunners left instructions. Nobody left a translation."},
    std::string_view{"Somewhere a containment protocol is being described as a formality."},
    std::string_view{"The ring is beautiful from here. It is beautiful from everywhere."},
    std::string_view{"A terminal is displaying a warning in a language nobody reads."},
    std::string_view{"The architecture is holding. The architecture is always holding."},
    std::string_view{"Something enormous is being moved very slightly."},
    std::string_view{"A light has come on that has been off for a hundred thousand years."},
    std::string_view{"The gravity lift is thinking about it."},
    std::string_view{"An ancient system has been asked to run a game of slayer."},
    std::string_view{"The control room has been located. Nobody is admitting how."},
    std::string_view{"Somewhere a countdown is being started for reassuring reasons."},
    std::string_view{"The installation has been asked to be reasonable."},
    std::string_view{"A pelican is inbound and is not going to wait."},
    std::string_view{"The snow on this ring falls in exactly the wrong direction."},

    // The mod, and the business of getting two people into the same place.
    std::string_view{"Waking up the relay. It sleeps like a grunt."},
    std::string_view{"Steam has been asked politely."},
    std::string_view{"Negotiating with the network. The network is winning."},
    std::string_view{"Rounding up everybody who said they were ready."},
    std::string_view{"Checking that everyone brought the same map."},
    std::string_view{"Handing out teams. Nobody will be happy."},
    std::string_view{"Counting players. There are more than there were."},
    std::string_view{"Somebody is still finding their helmet."},
    std::string_view{"Synchronising watches, which matters more than it sounds."},
    std::string_view{"Making sure everybody starts at the same moment."},
    std::string_view{"Putting the flags back where they belong."},
    std::string_view{"Reminding the scoreboard what game this is."},
    std::string_view{"Assembling a fireteam out of whoever answered."},
    std::string_view{"Warming up the shaders. They take their time."},
    std::string_view{"Loading a map that has been waiting twenty years for this."},
};

} // namespace

std::string_view LoadingLine(std::uint32_t index) noexcept {
    return kLines[index % kLines.size()];
}

std::size_t LoadingLineCount() noexcept {
    return kLines.size();
}

} // namespace mpe::unreal
