// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Unreal/GameThread.h
//
// Runs work on the game thread, and calls Unreal functions.
//
// WHY THIS IS NEEDED
//
// Almost nothing in Unreal may be touched from an arbitrary thread. Reading memory from
// outside is safe enough; calling a function is not. Anything that creates a widget,
// travels to a map, or drives a session has to run where the engine runs.
//
// HOW THE GAME THREAD IS REACHED
//
// Without patching any of the game's code: arm an instruction breakpoint on a function the
// game thread calls constantly, and the exception handler then runs in that thread's
// context. Work queued from outside is picked up there and executed.
//
// This is why the approach was chosen over the usual one of overwriting a virtual table
// entry or splicing a jump into a prologue. Nothing in the game is modified, the mechanism
// is removed by clearing a register, and a mistake shows up as a breakpoint that never
// fires rather than as corrupted code. It costs a debug register while a job is pending
// and nothing at all when idle.
//
// FINDING ProcessEvent
//
// ProcessEvent is a UObject virtual, so it is reached through a virtual table rather than
// by name. Its slot is not assumed. Virtual tables from unrelated classes are compared:
// entries that are identical across classes are inherited from UObject, and among those the
// largest function is ProcessEvent. Sizes come from the executable's exception directory,
// which describes every function's exact bounds, rather than from guesswork.
//
// Each guess is then checked before use, because an address that is merely plausible has
// already produced silent wrong answers on this build more than once.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "Core/Result.h"
#include "Unreal/ObjectArray.h"
#include "Unreal/Reflection.h"

namespace mpe::unreal {

/// What detection found, for logging and for the self test.
struct CallLayout {
    std::uintptr_t process_event{0};      ///< Absolute address of UObject::ProcessEvent.
    std::uintptr_t process_event_rva{0};  ///< Its offset in the module.
    std::size_t    process_event_size{0}; ///< Function length, from the exception directory.
    int            vtable_slot{-1};       ///< Which slot it occupied.
    std::uintptr_t dispatch_anchor{0};    ///< Address the game thread breakpoint uses.
};

/// Works out where ProcessEvent lives. Safe to call repeatedly.
[[nodiscard]] Result DetectCallLayout(const ObjectArray& objects, CallLayout& out_layout);

/// Runs a job on the game thread and waits for it to finish.
///
/// Returns a failure if the game thread never reaches the anchor within the timeout, which
/// is what happens when the game is paused, loading, or has not created a world yet.
///
/// A job may capture the caller's locals by reference. That is only sound because this
/// never returns while a job that has already started could still write to them: a deadline
/// that passes before anybody claimed the job cancels it, and one that passes after keeps
/// waiting. Calling it from the game thread runs the job inline rather than queueing it,
/// which is what stops work reached from a widget event deadlocking on its own frame.
[[nodiscard]] Result RunOnGameThread(const std::function<void()>& job,
                                     unsigned timeout_milliseconds = 5000);

/// Queues a job and returns immediately.
///
/// For work whose result nobody is waiting for, where blocking is the actual problem rather
/// than an acceptable cost. Beginning a match is the case this exists for: it runs inside
/// the lobby tick, and waiting there stops the tick, the keepalives and every screen update
/// for as long as the load takes.
///
/// The job must own everything it touches. Nothing is waiting on it, so it may run after the
/// calling frame has gone, and a reference capture would be a reference to nothing.
///
/// Fails when there is no pump, because without one nothing would ever run it.
[[nodiscard]] Result PostToGameThread(std::function<void()> job);

/// True when the calling thread is the one the pump last fired on.
[[nodiscard]] bool OnGameThread();

/// Cancels everything queued and stops any waiter, for unload.
void ShutdownGameThreadDispatch();

/// Finds a UFunction by name, optionally requiring a particular owning class.
[[nodiscard]] std::uintptr_t FindFunction(const ObjectArray& objects, std::string_view name,
                                          std::string_view owner_class = {});

/// Calls a function on an object with a caller supplied parameter block.
///
/// The block must match what the function expects. Unreal writes return values back into
/// it, so a caller reads results from the same buffer after the call returns.
///
/// Must be called from the game thread; use RunOnGameThread to get there.
[[nodiscard]] Result CallFunction(std::uintptr_t object, std::uintptr_t function,
                                  void* parameters);

/// Executes a console command through a player controller.
///
/// ConsoleCommand is an ordinary UFunction, so this needs no console to exist. That
/// matters here because constructing the console crashes this build.
[[nodiscard]] Result ExecuteConsoleCommand(const ObjectArray& objects,
                                           std::string_view command);

/// Travels the local player to a URL.
///
/// This is the whole of multiplayer in one call. A URL of "<map path>?listen" starts
/// hosting on that level; a URL that is an address joins whoever is hosting there. It is
/// used instead of a console command because this build does not expose ConsoleCommand as
/// a reflected function, while ClientTravel is fully described.
///
/// Must run on the game thread.
[[nodiscard]] Result Travel(const ObjectArray& objects, std::string_view url,
                            std::uint8_t travel_type = 0 /* TRAVEL_Absolute */);

/// Starts a mission through the game's own campaign flow.
///
/// This is the route the game itself uses, and it is why ClientTravel is not. Travelling
/// with Unreal's own machinery completes the call and then faults during the transition,
/// because the Blam simulation is never set up for the new world. BeginCampaign is the
/// subsystem entry point that does that setup, and it is fully reflected.
///
/// Must run on the game thread.
///
/// campaign_asset names a BlamCampaignDataAsset such as DA_FirstPlayableCampaign. Passing
/// the asset is what BeginCampaign on its own lacks: without an active campaign there is
/// nothing to begin, and it returns false rather than starting anything.
[[nodiscard]] Result BeginCampaign(const ObjectArray& objects, const Reflection& reflection,
                                   std::string_view scenario,
                                   std::string_view campaign_asset,
                                   bool             friendly_fire,
                                   int              difficulty = -1);

/// Creates a widget of the named class and puts it on screen.
///
/// This is real in game UI rather than an overlay: the widget is constructed by the engine
/// through UWidgetBlueprintLibrary::Create and added to the viewport, so it is drawn,
/// styled and input handled by the game itself.
///
/// Must run on the game thread.
[[nodiscard]] Result ShowWidget(const ObjectArray& objects, std::string_view widget_class,
                                std::uintptr_t& out_widget);

/// Turns a long lived widget into a game thread pump.
///
/// Jobs then run on the next event that widget receives, which for anything on screen is
/// the next frame. This replaces arming and disarming a hardware breakpoint across every
/// thread for each job: that worked, but suspending nearly two hundred threads twice per
/// call took seconds, which is far too slow for a menu that should react at once.
///
/// The widget's virtual table is copied and only that object is pointed at the copy, so
/// nothing else in the game is affected and the original pointer restores it.
[[nodiscard]] Result InstallGameThreadPump(std::uintptr_t widget);

/// Removes the pump and restores the widget.
void RemoveGameThreadPump();

/// True while jobs can run without a breakpoint.
[[nodiscard]] bool GameThreadPumpActive();

/// How many events the pump has seen since the process started.
///
/// Installed is not the same as working. The pump lives on one object's event path and
/// objects do not live forever: loading a scenario destroys the front end, which is exactly
/// where the pump was, and from that moment nothing queued can run. A count that stops
/// moving is the only way to notice, because a destroyed object cannot report anything.
[[nodiscard]] std::uint64_t PumpEventCount();

/// The object currently carrying the pump, or zero.
[[nodiscard]] std::uintptr_t GameThreadPumpHost();

/// Drops the pump handles without writing to them.
///
/// For a host that has been destroyed rather than released: its address is no longer the
/// address of anything, so restoring the original virtual table would be a write into
/// whatever now occupies it.
void ForgetGameThreadPump();

/// Starts reporting events on one widget.
///
/// Clicks arrive through OnButtonBaseClicked, a multicast delegate, and the runtime
/// selected state is not reflected, so neither binding nor polling is available from here.
/// Instead the widget's virtual table is copied, the ProcessEvent slot in the copy is
/// replaced, and only this one object is pointed at the copy.
///
/// Nothing in the game is modified: other widgets of the same class keep the original
/// table, and the object is restored by writing the original pointer back. Every event the
/// widget receives then passes through, and the click is recognised by name.
[[nodiscard]] Result WatchWidgetEvents(std::uintptr_t widget);

/// True once, when a watched widget has been clicked since the last call.
[[nodiscard]] bool ConsumeWidgetClick();

/// Watches another widget of the same class as the established watch.
///
/// The lobby needs every button on it live at once rather than one at a time. Widgets of
/// one class share a virtual table, so they can share the single patched copy: this only
/// records the address and repoints the object, and refuses a widget of any other class
/// because pointing it at this table would call the wrong function for every slot in it.
[[nodiscard]] Result AlsoWatchWidget(std::uintptr_t widget);

/// Which widget was clicked, cleared by reading it. Zero when nothing is pending.
[[nodiscard]] std::uintptr_t ConsumeClickedWidget();

/// Drops every extra watched widget.
///
/// Called before rebuilding a screen, because the widgets it watched are about to be
/// destroyed and a freed address can be reused by an unrelated object.
void ForgetExtraWatchedWidgets();

/// Every distinct event seen on the watched widget so far.
[[nodiscard]] std::vector<std::uintptr_t> SeenWidgetEvents();

/// The most recent event, so an action can be tied to what just happened.
[[nodiscard]] std::uintptr_t LastWidgetEvent();

/// Declares which event counts as a click.
void SetWidgetClickEvent(std::uintptr_t function);

/// Puts the widget's original virtual table back.
void StopWatchingWidgetEvents();

/// Everything AddMainMenuButton needs, resolved ahead of time.
///
/// Looking these up means scanning the object array, which is far too slow to do while the
/// game thread is blocked waiting: doing it inline froze the game for several seconds. The
/// lookups happen on the caller's thread, and only the function calls run on the game
/// thread, which takes the visible cost to nothing.
struct MenuButtonPlan {
    std::uintptr_t menu{0};
    std::uintptr_t container{0};
    std::uintptr_t button_class{0};
    std::uintptr_t controller{0};
    std::uintptr_t create_function{0};
    std::uintptr_t widget_library{0};
    std::uintptr_t add_child_function{0};
    std::uintptr_t remove_child_function{0};
    std::uintptr_t convert_function{0};
    std::uintptr_t text_library{0};
    std::uintptr_t existing[10]{};
    std::size_t    existing_count{0};

    /// Anything else currently parented that must also be cleared, such as an entry this
    /// mod added on a previous pass. Without it the added entry survives the rebuild and
    /// appears a second time.
    void AlsoRemove(std::uintptr_t widget) {
        if (widget != 0 && existing_count < std::size(existing)) {
            existing[existing_count++] = widget;
        }
    }
};

/// One row of a built menu.
///
/// The button widget carries a label, a second description line and four bracket flags, so
/// a row can be a heading, a setting with its current value, or a player slot, without
/// needing widget types that would have to be constructed from scratch.
struct MenuRow {
    std::string label;
    std::string description;
    bool        heading{false};  ///< Framed with brackets and no description.
    bool        selectable{true};
};

/// Replaces the menu's entries with rows, and reports the widgets made.
[[nodiscard]] Result BuildMenuRows(const MenuButtonPlan& plan,
                                   const std::vector<MenuRow>& rows,
                                   std::vector<std::uintptr_t>& out_buttons);

/// Replaces the menu's entries with a list of labels, and reports the widgets made.
///
/// The lobby is built from the same button class the shipped menu uses, in the menu's own
/// container, so it inherits the game's look, spacing, focus and navigation rather than
/// imitating them.
///
/// Must run on the game thread. Resolve the plan first.
[[nodiscard]] Result BuildMenuList(const MenuButtonPlan& plan,
                                   const std::vector<std::string>& labels,
                                   std::vector<std::uintptr_t>& out_buttons);

/// Resolves the plan. Safe to call from any thread; does no engine calls.
/// Works out everything needed to add one entry to the main menu.
///
/// known_menu is the live WBP_MainMenu_C, which the caller has usually just found. Pass it
/// and the object array is not scanned at all: the parts that never change are resolved on
/// the first call and kept, so a menu appearing costs a few guarded reads rather than a
/// pass over fifty thousand objects at the exact moment the player is waiting to see the
/// entry. Pass zero to have the menu located as well.
[[nodiscard]] Result ResolveMenuButtonPlan(const ObjectArray& objects,
                                           std::uintptr_t known_menu, MenuButtonPlan& out_plan);

/// Resolves the parts of the plan that do not depend on a menu, ahead of there being one.
///
/// Everything except the live menu is fixed for the process: the button class, the two
/// libraries, the four functions and the player controller. Finding them costs a pass over
/// the object array, and doing it when the menu appears spends that pass at the one moment
/// the player is watching an empty menu waiting for the entry.
///
/// Call it repeatedly while the game loads. It returns false until the classes it needs
/// exist, and does nothing once it has succeeded.
[[nodiscard]] bool WarmMenuButtonPlan(const ObjectArray& objects);

/// Supplies the menu independent pieces from a pass the caller has already made.
///
/// The caller walks the object array to find the menu, and the same walk sees the button
/// class, the libraries, the functions and the controller. Handing them over means
/// ResolveMenuButtonPlan makes no pass of its own, so adding the entry costs one walk
/// rather than two, back to back, at the moment the player is looking at a menu that has
/// no entry on it.
///
/// Ignored unless every required piece is present, so a partial pass cannot poison the
/// cache: the resolver simply falls back to scanning for itself.
void SeedMenuButtonPlan(const MenuButtonPlan& pieces);

/// Applies a resolved plan. Must run on the game thread.
[[nodiscard]] Result ApplyMenuButtonPlan(const MenuButtonPlan& plan, std::string_view label,
                                         std::uintptr_t& out_button);

/// Adds a button to the game's real main menu.
///
/// Not an overlay and not a separate window: a widget of the same class the menu's own
/// entries use is created, labelled, and parented into the menu's button container, so it
/// is laid out, styled, focused and navigated exactly like the shipped entries.
[[nodiscard]] Result AddMainMenuButton(const ObjectArray& objects, std::string_view label,
                                       std::uintptr_t& out_button);

/// Removes a widget previously created by ShowWidget.
[[nodiscard]] Result HideWidget(const ObjectArray& objects, std::uintptr_t widget);

/// Calls a function that takes no parameters on a live instance of a class.
///
/// Covers most of the lobby surface, which is deliberately simple: BeginAllowInvites and
/// EndAllowInvites take nothing at all.
[[nodiscard]] Result CallSimple(const ObjectArray& objects, std::string_view class_name,
                                std::string_view function_name);

/// Calls a function whose only parameter is an int return value, and reports it.
[[nodiscard]] Result CallReturningInt(const ObjectArray& objects, std::string_view class_name,
                                      std::string_view function_name, int& out_value);

} // namespace mpe::unreal
