// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Unreal/LobbyUI.h
//
// Builds the multiplayer lobby as real UMG, laid out on a canvas.
//
// WHY THIS EXISTS
//
// The first lobby was a list of menu buttons in the main menu's own container. That
// container is a plain vertical stack with no scrolling, so anything beyond a handful of
// rows ran off the bottom of the screen, and a stack of rows cannot express tabs, side by
// side team columns, player cards or a table of servers.
//
// A real layout needs widgets that have to be created rather than reused: canvases, boxes,
// borders and text blocks. Those are made with SpawnObject, which is NewObject exposed to
// Blueprint, and positioned through the canvas slot each one gets when it is parented.
//
// Everything here is ordinary engine UI. The widgets are the engine's own, the layout is
// the engine's own canvas arithmetic, and the screen is drawn and scaled by the game.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "Core/Result.h"
#include "Unreal/ObjectArray.h"

namespace mpe::unreal {

/// A scenario a match can be played on.
///
/// Named, with the scenario code kept in brackets. The code is what the engine is actually
/// handed, so showing it alongside the name means a button can always be checked against
/// what it loads rather than trusted on the strength of its label.
struct LobbyMap {
    const char* label;
    const char* scenario;
};

/// The maps offered on the HOST tab, in the order they are listed.
///
/// These are Halo's own scenario codes, unchanged since the original: a30 is Halo, b30 the
/// Silent Cartographer, b40 Assault on the Control Room, c20 the Library.
inline constexpr LobbyMap kLobbyMaps[] = {
    {"HALO (A30)", "a30"},
    {"SILENT CARTOGRAPHER (B30)", "b30"},
    {"CONTROL ROOM (B40)", "b40"},
    {"THE LIBRARY (C20)", "c20"},
};

/// One row in the server browser.
struct ServerEntry {
    std::string name;
    std::string mode;
    std::string map;
    int         players{0};
    int         capacity{10};
    int         ping{0};
    /// What the session is doing, as the host published it: "LOBBY" while people are
    /// gathering, "IN GAME" once a match is running. Empty when the host has not said.
    ///
    /// Worth a column of its own because the two are entirely different invitations. A
    /// lobby is somewhere to go now; a match in progress is somewhere to go if you do not
    /// mind arriving late, and until this was shown the row gave no way to tell.
    std::string status;
};

/// Everything the lobby draws.
struct LobbyView {
    bool                     browsing{false}; ///< False shows HOST, true shows BROWSE.
    std::string              mode{"CAPTURE THE FLAG"};
    std::string              map{"BLOOD GULCH"};
    std::string              server_name;
    int                      game_time_minutes{15};
    bool                     friendly_fire{true};
    int                      respawn_seconds{10};
    std::vector<std::string> red;   ///< Names on red, in slot order.
    std::vector<std::string> blue;  ///< Names on blue.
    std::string              host_name;
    std::vector<ServerEntry> servers;
    int                      selected_server{0};
};

/// What pressing a button on the lobby is meant to do.
enum class LobbyAction {
    None,
    ShowHost,
    ShowBrowse,
    SelectCaptureTheFlag,
    SelectSlayer,
    StartMatch,
    JoinMatch,
    Back,
    /// An empty player slot. Pressing one invites somebody into that side.
    InviteRed,
    InviteBlue,
    /// Server browser filters. Each is a toggle against the live listing.
    FilterModeAny,
    FilterModeCaptureTheFlag,
    FilterModeSlayer,
    FilterSlotsAny,
    FilterSlotsOpen,
    FilterSlotsFull,
    FilterPingAny,
    FilterPingUnder50,
    FilterPingUnder100,
    /// A row in the server table. The row index is carried on the control.
    SelectServer,
    /// A map on the HOST tab. The index into kLobbyMaps is carried on the control.
    SelectMap,
    /// A row in the invite list. The row index is carried on the control.
    SelectFriend,
    /// Asks Steam for the lobby list again and redraws the table.
    RefreshServers,
    /// Dismisses the invite list without inviting anybody.
    CloseInvite,
    /// Pages the invite list, for a friends list longer than one screen.
    FriendsPrevious,
    FriendsNext,
};

/// One row in the invite list.
struct LobbyFriend {
    std::string name;
    /// True when Steam reports them in this game, which is who can accept immediately.
    bool in_game{false};
    /// True once an invite has been sent to them from this lobby.
    bool invited{false};
};

/// How many invite rows the panel draws. A longer friends list is paged.
inline constexpr int kFriendRows = 8;

/// How the server browser is filtered.
struct ServerFilter {
    /// Empty means any mode.
    std::string mode;
    /// 0 any, 1 only servers with room, 2 only full servers.
    int slots{0};
    /// 0 any, otherwise the highest acceptable ping in milliseconds.
    int max_ping{0};

    [[nodiscard]] bool Accepts(const ServerEntry& entry) const {
        if (!mode.empty() && entry.mode != mode) {
            return false;
        }
        if (slots == 1 && entry.players >= entry.capacity) {
            return false;
        }
        if (slots == 2 && entry.players < entry.capacity) {
            return false;
        }
        if (max_ping > 0 && entry.ping > max_ping) {
            return false;
        }
        return true;
    }
};

/// A button the lobby built, and what it means.
///
/// Returned rather than handled here, because acting on a press needs the lobby state, the
/// Steam session and the campaign launch path, none of which belong to drawing.
struct LobbyControl {
    std::uintptr_t widget{0};
    LobbyAction    action{LobbyAction::None};
    /// Which row, for the server table. Unused by every other action.
    int index{0};
};

/// Resolved once, so building a screen makes no object array scans.
struct LobbyUIContext {
    std::uintptr_t spawn_object{0};
    std::uintptr_t gameplay_statics{0};
    std::uintptr_t add_to_canvas{0};
    /// Generic panel add, used for the one attach where the parent is the game's own root
    /// and its type is not known to be a canvas.
    std::uintptr_t add_child{0};
    /// Overlay slot alignment. A canvas has no size of its own, so without telling the
    /// overlay to stretch it the lobby is built correctly and drawn at zero by zero.
    std::uintptr_t set_horizontal_alignment{0};
    std::uintptr_t set_vertical_alignment{0};
    std::uintptr_t remove_from_parent{0};
    /// Used to fold the main menu away while the lobby is up, and to bring it back. This
    /// is what makes the lobby a screen rather than a panel sitting on top of the menu.
    std::uintptr_t set_visibility{0};
    /// Reads a widget's visibility so folding the menu away can put it back exactly as it
    /// was. Restoring it to Visible instead is what left the main menu drawn and dead.
    std::uintptr_t get_visibility{0};
    /// Walking the menu root's children, so the widgets to fold away are the ones actually
    /// parented there rather than a list of offsets guessed from a header.
    std::uintptr_t get_children_count{0};
    std::uintptr_t get_child_at{0};
    /// Measured after building. A screen that reports zero by zero has a layout fault; one
    /// that reports its real size and still shows nothing has a mounting fault. Logging it
    /// turns "it built and drew nothing" into a question with an answer.
    std::uintptr_t get_desired_size{0};
    std::uintptr_t set_position{0};
    std::uintptr_t set_size{0};
    std::uintptr_t set_text{0};
    std::uintptr_t set_color_and_opacity{0};
    std::uintptr_t convert_function{0};
    std::uintptr_t text_library{0};

    /// Focus. Folding the menu away hides it but does not stop it: the frontend keeps
    /// handling input and plays its hover sounds for buttons that are no longer drawn.
    /// Pointing input at the lobby is what actually makes it a separate screen.
    std::uintptr_t get_player_controller{0};
    std::uintptr_t set_input_mode_ui{0};
    std::uintptr_t set_keyboard_focus{0};
    std::uintptr_t widget_library{0};

    std::uintptr_t canvas_class{0};
    std::uintptr_t text_class{0};
    std::uintptr_t border_class{0};

    /// Scales the design to the viewport.
    ///
    /// The layout is authored against a fixed 1920x1080 and a size box holds it at exactly
    /// that. On a larger display that put the whole lobby in the top left corner at one to
    /// one, with the game still visible around it. A scale box set to fit resizes the
    /// design to whatever the viewport is, so the same numbers hold at any resolution.
    std::uintptr_t scalebox_class{0};
    std::uintptr_t set_stretch{0};

    /// Hosting the lobby in the viewport rather than inside the main menu's widget tree.
    ///
    /// Parenting into the menu meant inheriting geometry this code does not control: the
    /// menu's root hands out an area smaller than the design, so a scale box told to fit
    /// shrank the whole screen to about half and pinned it to the top left. It is also why
    /// the menu kept handling input underneath. A widget added to the viewport gets the
    /// whole viewport, and owes the menu nothing.
    std::uintptr_t create_widget{0};    ///< UWidgetBlueprintLibrary::Create
    std::uintptr_t add_to_viewport{0};  ///< UUserWidget::AddToViewport
    std::uintptr_t host_class{0};       ///< A concrete UUserWidget class to host with.
    /// The frontend's own button. Using it rather than a coloured rectangle is what makes
    /// the lobby's buttons the game's buttons: its typeface, its hover and press states,
    /// its sounds and its focus behaviour, none of which can be reproduced by drawing.
    std::uintptr_t button_class{0};
    /// The frontend's panel backing, used instead of a flat coloured border.
    std::uintptr_t backer_class{0};
    std::uintptr_t layout_library{0};
    std::uintptr_t get_viewport_size{0};

    /// A real text field for the server name.
    ///
    /// A label cannot be typed into, so the name had nowhere to come from. An editable text
    /// box is the engine's own input widget; its contents are read back through GetText and
    /// converted with Conv_TextToString, because an FText owns shared string data that
    /// cannot be picked apart by hand.
    std::uintptr_t editable_class{0};
    std::uintptr_t get_editable_text{0};
    std::uintptr_t set_editable_text{0};
    std::uintptr_t text_to_string{0};
    /// Where the field keeps its font and text colour, found by reflection rather than
    /// guessed. Zero when it could not be located, in which case styling is skipped rather
    /// than written to an offset that might be something else entirely.
    std::uintptr_t editable_font_offset{0};
    std::uintptr_t editable_colour_offset{0};
    /// Where the field keeps its placeholder, so an empty box says what it is for.
    std::uintptr_t editable_hint_offset{0};

    /// The game's own font, copied from a text block the game itself built.
    ///
    /// A text block created from scratch gets the engine default, which is Roboto, and no
    /// amount of colour or size makes that look like this game's menus. Rather than name a
    /// font asset and hope the path is right, the whole FSlateFontInfo up to Size is taken
    /// from a live game text block and stamped onto every block the lobby makes, leaving
    /// only the size to be set per line. That carries the typeface, the material and the
    /// outline settings across without knowing what any of them are.
    std::array<std::uint8_t, 0x48> font_template{};
    bool                           has_font{false};

    /// Widgets that are on screen with the menu but are not part of it.
    ///
    /// Collapsing the menu takes everything under it away, and the fireteam panel stayed
    /// visible through that, so it is not under it: it belongs to the layout the frontend
    /// puts around the menu. Anything in that position has to be folded by name.
    std::vector<std::uintptr_t> also_fold;

    std::uintptr_t outer{0};       ///< Owner for created widgets.
    std::uintptr_t root_canvas{0}; ///< The game's root panel, kept for diagnostics.
    std::string    root_class;     ///< What that root actually is.

    /// Gives the lobby a real size. A canvas panel has none of its own, so without this it
    /// is laid out at zero by zero and draws nothing.
    std::uintptr_t sizebox_class{0};

    /// True once the parts that never change have been found.
    [[nodiscard]] bool StaticsComplete() const {
        // Everything the lobby cannot open without. A handle missing from this list is a
        // handle whose absence is only discovered when a player presses the button, which
        // is how a resolver bug turned into a menu entry that silently did nothing.
        return spawn_object && gameplay_statics && add_to_canvas && add_child &&
               set_position && set_size && convert_function && text_library && canvas_class &&
               text_class && border_class && sizebox_class && scalebox_class &&
               set_stretch && create_widget && add_to_viewport && host_class &&
               button_class && widget_library;
    }

    [[nodiscard]] bool Complete() const {
        return StaticsComplete() && outer && root_canvas;
    }
};

/// Finds the parts that do not change: functions and widget classes.
///
/// This is the expensive half, around a dozen passes over fifty thousand objects, and it
/// is why opening the lobby used to take fifteen seconds. It depends on nothing that a
/// level load or a menu rebuild can invalidate, so it is resolved once and kept.
///
/// Any thread; performs no engine calls.
[[nodiscard]] Result ResolveLobbyStatics(const ObjectArray& objects,
                                         LobbyUIContext& out_context);

/// Points a resolved context at a specific live main menu.
///
/// Cheap by construction: two guarded reads, no scanning. The menu address comes from the
/// caller, which already knows which instance is live because it is the one the menu entry
/// was successfully added to.
///
/// Any thread; performs no engine calls.
[[nodiscard]] Result BindLobbyMenu(std::uintptr_t menu, LobbyUIContext& context);

/// Finds everything the builder needs. Any thread; performs no engine calls.
///
/// Equivalent to ResolveLobbyStatics followed by BindLobbyMenu against the first live
/// menu found. Kept for callers that have no menu address of their own.
[[nodiscard]] Result ResolveLobbyUI(const ObjectArray& objects, LobbyUIContext& out_context);

/// Builds the lobby and puts it on screen. Must run on the game thread.
///
/// Any previously built lobby is removed first, so reopening the screen replaces it rather
/// than stacking another copy on top of it.
///
/// Returns the root panel it created, so it can be removed again.
[[nodiscard]] Result BuildLobbyUI(const LobbyUIContext& context, const LobbyView& view,
                                  std::uintptr_t&             out_root,
                                  std::vector<LobbyControl>& out_controls);

/// Shows or hides the whole lobby without rebuilding any of it.
///
/// Building the screen means creating around a hundred widgets on the game thread, and the
/// frontend's button duplicates a widget tree apiece. Doing that for every press is what
/// made switching tabs slow, and because a rebuild takes the old screen down first, it also
/// let the menu and the fireteam back for the moment in between, which is where their hover
/// sounds were coming back from. The lobby is built once and then only shown and hidden.
///
/// Must run on the game thread.
void ShowLobbyUI(const LobbyUIContext& context, bool visible);

/// Switches between the HOST and BROWSE tabs. Both are built; only one is visible.
///
/// Must run on the game thread.
void SetLobbyTab(const LobbyUIContext& context, bool browsing);

/// Shows or hides everything only a host may use: the mode and map choices, and START
/// MATCH.
///
/// A client cannot change any of it, so it is taken off the screen rather than left inert.
/// Must run on the game thread.
void SetLobbyHostControls(const LobbyUIContext& context, bool is_host);

/// Marks which game mode is selected, without rebuilding the screen.
///
/// Must run on the game thread.
void SetLobbyMode(const LobbyUIContext& context, bool slayer);

/// Marks the chosen map. Must run on the game thread.
void SetLobbyMap(const LobbyUIContext& context, int map_index);

/// What the status panel reports, top right of the lobby.
struct LobbyStatus {
    /// Signed in to Steam and able to reach its services.
    bool online{false};
    /// The session line: whether a lobby exists and can be invited to.
    std::string session{"OFFLINE"};
    /// True when the session is up and someone can actually be invited to it.
    bool invitable{false};
    /// The version line, including whether an update is waiting.
    std::string version;
    bool        update_available{false};
    /// True once a new build has been downloaded and is waiting to be swapped in.
    ///
    /// This is not the same as an update being available. A mapped DLL cannot replace
    /// itself, so the new build sits beside the running one until the next start, and the
    /// only thing that finishes the update is the player closing the game and opening it
    /// again. Told plainly, because a player who does not restart keeps running the old
    /// build while the screen says the update succeeded.
    bool        restart_required{false};
    /// The version that is staged and will be in use after a restart.
    std::string staged_version;

    /// Round trip to whoever this machine is connected to, in milliseconds.
    ///
    /// Negative when there is nobody to measure against, which is most of the time: a
    /// lobby with one person in it has no round trip, and a number invented for that case
    /// would be a number the player could act on wrongly.
    int ping_ms{-1};

    /// Who is hosting, when this machine is not. Empty otherwise.
    std::string host_name;

    /// What the session is playing, for the panel's own summary.
    std::string mode;
    std::string map;

    /// A message that does not fit on the status line, shown top left instead.
    ///
    /// The status panel gives each line about three hundred points, which is enough for
    /// a phase and a version and nothing else. A session error is a whole sentence, and
    /// putting one there cut it off mid word: "ERROR: the host did" told a player less
    /// than nothing. Anything worth reading in full goes here, where it has the width to
    /// be read and the height to wrap.
    std::string notice_title;
    std::string notice_detail;
};

/// Rewrites the status panel in place. Must run on the game thread.
void SetLobbyStatus(const LobbyUIContext& context, const LobbyStatus& status);

/// Builds the status overlay, which is separate from the lobby and outlives it.
///
/// It reports on the mod rather than on whichever screen is showing, so it belongs to
/// neither: a player on the main menu wants to know they are signed in, that a session is
/// up, and whether the build is current, without opening anything. It is hosted in its own
/// viewport widget above the lobby's, so hiding the lobby does not take it away.
///
/// Safe to call repeatedly; it builds once. Must run on the game thread.
[[nodiscard]] Result BuildStatusOverlay(const LobbyUIContext& context);

/// True once the status overlay is on screen.
[[nodiscard]] bool StatusOverlayIsBuilt();

/// The widget the viewport holds the overlay in, so a caller can check it still exists.
[[nodiscard]] std::uintptr_t StatusOverlayWidget();

/// Drops every handle to the overlay without touching them.
///
/// For the case where the objects have been collected rather than removed: the addresses
/// are no longer addresses of anything, so the only correct action is to stop believing
/// in them and build again.
void ForgetStatusOverlay();

/// Counts how many times the lobby has been built.
///
/// Anything that decides what to draw by comparing against what it drew last time needs
/// this, because a rebuilt screen has different widgets and the comparison alone cannot
/// tell. A guest kept the host's controls this way.
[[nodiscard]] std::uint32_t LobbyBuildId();

/// Rewrites the server table and the details panel in place.
///
/// The rows are built once and then only have their text replaced, so filtering or a
/// refresh costs a handful of string writes rather than rebuilding the screen. Rows beyond
/// the end of the list are collapsed, so a short list does not leave stale ones behind.
///
/// Must run on the game thread.
void SetLobbyServers(const LobbyUIContext& context, const std::vector<ServerEntry>& servers,
                     int selected);

/// Marks which filter options are active.
///
/// Must run on the game thread.
void SetLobbyFilters(const LobbyUIContext& context, const ServerFilter& filter);

/// Shows or hides the invite list over the lobby.
///
/// Drawn as part of the lobby and kept collapsed, for the same reason the tabs are: this
/// has to appear the instant a slot is pressed, and creating a panel's worth of widgets on
/// the game thread is not instant. Must run on the game thread.
void ShowInvitePanel(const LobbyUIContext& context, bool visible);

/// True while the invite list is on screen.
[[nodiscard]] bool InvitePanelIsOpen();

/// Rewrites the invite list in place.
///
/// page is which block of kFriendRows to show, so a friends list longer than the panel is
/// reachable rather than truncated. Must run on the game thread.
void SetLobbyFriends(const LobbyUIContext& context, const std::vector<LobbyFriend>& friends,
                     int page);

/// Rewrites the team slots in place with who is actually in the session.
///
/// The cards are built once and only their text and visibility change, so a player joining
/// costs a handful of writes rather than rebuilding the screen. Names are the Steam persona
/// names the lobby reports, so a slot shows who is in it rather than a number.
///
/// Must run on the game thread.
void SetLobbyRoster(const LobbyUIContext& context, const std::vector<std::string>& blue,
                    const std::vector<std::string>& red, const std::string& host_name);

/// The longest a server name may be.
///
/// A name is advertised in the lobby's Steam metadata and drawn in one table column, so an
/// unbounded one is both a wide row nothing else lines up with and a larger payload on
/// every lobby search anyone runs.
inline constexpr std::size_t kMaxServerNameLength = 64;

/// Reads what the player typed as the server name.
///
/// Empty when the field is untouched, so a caller can fall back to a default rather than
/// advertising a blank name.
///
/// Anything past kMaxServerNameLength is cut off and written back to the field, so the
/// limit is visible in the box rather than being applied silently somewhere the player
/// cannot see. Must run on the game thread.
[[nodiscard]] std::string ReadServerName(const LobbyUIContext& context);

/// Puts a name into the server name field, for restoring a saved one.
///
/// Must run on the game thread.
void WriteServerName(const LobbyUIContext& context, std::string_view name);

/// True once the lobby has been built and can simply be shown.
[[nodiscard]] bool LobbyIsBuilt();

/// Removes the lobby from the screen and brings the main menu back. Game thread.
void RemoveLobbyUI(const LobbyUIContext& context);

/// The widget currently hosting the lobby, or zero when it is closed.
[[nodiscard]] std::uintptr_t OpenLobbyFrame();

/// Logs the laid out size of the open lobby.
///
/// Desired size is whatever the last layout pass cached, so this is only meaningful a
/// frame or more after building. Zero by zero means the lobby is mounted but the layout
/// gave it no room, which is a different fault from one that never mounted at all, and the
/// two are indistinguishable from a screen that simply looks empty.
///
/// Must run on the game thread.
void MeasureLobby(const LobbyUIContext& context);

/// Puts a single bright rectangle on screen and nothing else.
///
/// A deliberately minimal test. When a full screen builds without error and shows nothing,
/// the useful question is not which panel is wrong but whether a widget this code creates
/// can be drawn at all. One rectangle answers that: if it appears, the pipeline works and
/// the fault is in layout or colour; if it does not, nothing built this way will ever show
/// and the attachment is what needs changing.
///
/// Must run on the game thread.
[[nodiscard]] Result ProbeLobbyUI(const LobbyUIContext& context);

} // namespace mpe::unreal
