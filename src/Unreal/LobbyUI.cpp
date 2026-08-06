// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Unreal/LobbyUI.cpp
#define MPE_LOG_CATEGORY "Unreal.LobbyUI"

#include "Unreal/LobbyUI.h"

#include "Core/Log.h"
#include "Unreal/GameThread.h"
#include "Unreal/ProcessMemory.h"

// For MultiByteToWideChar: Steam hands back UTF-8 and the engine's text wants UTF-16.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <unordered_map>
#include <unordered_set>

namespace mpe::unreal {
namespace {

/// The design is laid out against this size and scaled by the canvas, so it holds its
/// proportions on any resolution rather than needing per resolution numbers.
constexpr float kDesignWidth  = 1920.0F;
constexpr float kDesignHeight = 1080.0F;

/// FVector2D as the engine actually lays it out.
///
/// Double precision, not float. Unreal 5 widened the core vector types, so FVector2D is
/// sixteen bytes. Passing a pair of floats to a function that takes one is not a rounding
/// difference, it is a parameter block half the expected size: the engine reads the four
/// bytes of x and the four of y as the first double, and reads whatever follows on the
/// stack as the second.
///
/// This is what made every earlier lobby build without a single error and draw nothing.
/// The widgets were created, parented and given slots correctly, then placed at a garbage
/// position with a garbage size, which put them off screen or collapsed them to nothing.
struct Vector2 {
    double x{0};
    double y{0};
};

struct LinearColour {
    float r{0};
    float g{0};
    float b{0};
    float a{1};
};

/// The palette from the reference: dark slate panels with a cyan accent.
constexpr LinearColour kPanel      = {0.043F, 0.063F, 0.078F, 0.92F};
constexpr LinearColour kPanelLight = {0.086F, 0.114F, 0.133F, 0.95F};
constexpr LinearColour kAccent     = {0.294F, 0.780F, 0.886F, 1.0F};
constexpr LinearColour kAccentDim  = {0.294F, 0.780F, 0.886F, 0.25F};
constexpr LinearColour kRed        = {0.898F, 0.286F, 0.286F, 1.0F};
constexpr LinearColour kBlue       = {0.361F, 0.643F, 0.898F, 1.0F};
constexpr LinearColour kText       = {0.902F, 0.933F, 0.949F, 1.0F};
constexpr LinearColour kTextDim    = {0.600F, 0.655F, 0.690F, 1.0F};
constexpr LinearColour kSlot       = {0.180F, 0.216F, 0.239F, 0.45F};
/// The plus on an empty slot. Deliberately dim: an empty slot is an absence, and it should
/// read as quieter than a card with somebody in it rather than competing with one.
constexpr LinearColour kSlotMark   = {0.294F, 0.780F, 0.886F, 0.35F};
constexpr LinearColour kGood       = {0.400F, 0.850F, 0.450F, 1.0F};
constexpr LinearColour kBad        = {0.910F, 0.380F, 0.350F, 1.0F};
constexpr LinearColour kWarn       = {0.960F, 0.760F, 0.300F, 1.0F};
/// Barely there. The status panel reports on the session rather than being part of the
/// layout, so it should read over the scene instead of punching a hole in it.
constexpr LinearColour kStatusPanel = {0.020F, 0.040F, 0.055F, 0.45F};

/// The bars drawn behind the two mode buttons. Selection is shown by making one visible
/// and the other not, so choosing a mode costs a visibility change rather than a rebuild.
std::uintptr_t g_mode_marker[2] = {0, 0};

/// The same idea for the map list: one marker per map, only the chosen one visible.
std::uintptr_t g_map_marker[4] = {0, 0, 0, 0};

/// The server name field, so its contents can be read back when hosting.
std::uintptr_t g_server_name_field = 0;

/// ESlateVisibility values used throughout.
constexpr std::uint8_t kVisibleValue          = 0;
constexpr std::uint8_t kCollapsedValue        = 1;
constexpr std::uint8_t kHitTestInvisible      = 3;
/// EStretch::Fill. Stretches a widget to its whole box rather than fitting it centred.
constexpr std::uint8_t kStretchFill           = 1;
constexpr std::uint8_t kSelfHitTestInvisibleValue = 4;

/// The server table, built once and then only rewritten.
///
/// Rebuilding the browser to apply a filter would mean creating the whole screen again,
/// which is exactly the cost this design exists to avoid. The rows are permanent and their
/// text is replaced, so filtering is a few string writes.
struct ServerRowWidgets {
    std::uintptr_t highlight{0};
    std::uintptr_t button{0};
    std::uintptr_t name{0};
    std::uintptr_t mode{0};
    std::uintptr_t map{0};
    std::uintptr_t players{0};
    std::uintptr_t ping{0};
    /// LOBBY or IN GAME, under the server's name. Small on purpose: it qualifies the row
    /// rather than competing with it.
    std::uintptr_t status{0};
};
constexpr std::size_t kServerRows = 8;
ServerRowWidgets g_server_row[kServerRows];

/// The details panel's five value lines, and the filter markers.
std::uintptr_t g_detail_line[5]   = {0, 0, 0, 0, 0};
std::uintptr_t g_filter_mode[3]   = {0, 0, 0};
std::uintptr_t g_filter_slots[3]  = {0, 0, 0};
std::uintptr_t g_filter_ping[3]   = {0, 0, 0};
std::uintptr_t g_empty_notice     = 0;

/// The status panel's lines, top right of the screen.
///
/// Five now rather than three, at a smaller size in a wider box. Three lines of nineteen
/// point in a box three hundred and ten points wide could not hold a sentence: a session
/// line reading "CONNECTING TO HOST  STALLED" ran off the right of the screen, because a
/// text block given more than it can draw simply keeps going.
constexpr std::size_t kStatusLines = 5;
std::uintptr_t g_status_line[kStatusLines] = {0, 0, 0, 0, 0};

/// Turns UTF-8 into the wide string the engine's text conversion expects.
///
/// Every one of these used to widen by casting each byte to a wchar_t. That is correct for
/// ASCII and wrong for everything else: a Steam persona name is UTF-8, so a name with any
/// character outside ASCII arrived as one garbage glyph per byte. A player called Nessie
/// with decorated brackets around the name rendered as "£T? Nessie £T?" on the team card,
/// which looks like the mod corrupting somebody's name, because it was.
///
/// Falls back to the old byte for byte widening only if the conversion fails outright,
/// which keeps a name on screen rather than blanking the card.
[[nodiscard]] std::wstring WidenUtf8(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (needed > 0) {
        std::wstring wide(static_cast<std::size_t>(needed), wchar_t{});
        if (::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                  wide.data(), needed) == needed) {
            return wide;
        }
    }
    std::wstring fallback;
    fallback.reserve(text.size());
    for (const char character : text) {
        fallback.push_back(static_cast<wchar_t>(static_cast<unsigned char>(character)));
    }
    return fallback;
}

/// Green under a hundred, amber to a hundred and fifty, red past it.
///
/// The thresholds are what a player can feel rather than anything measured: under a
/// hundred plays like a local game, past a hundred and fifty the shots stop landing where
/// they were aimed. Shared with the server browser so a number means the same thing in
/// both places.
[[nodiscard]] LinearColour PingColour(int ping_ms) {
    if (ping_ms < 0) {
        return kTextDim;
    }
    if (ping_ms <= 100) {
        return kGood;
    }
    if (ping_ms <= 150) {
        return kWarn;
    }
    return kBad;
}

/// The overlay that carries the status and notice panels.
///
/// Its own viewport widget, above the lobby's and never hidden, so the panel is on the
/// main menu as well as over the lobby.
std::uintptr_t g_status_host    = 0;
std::uintptr_t g_status_scaler  = 0;
std::uintptr_t g_status_root    = 0;

/// The notice panel, top left. Empty and collapsed unless there is something to say.
///
/// Separate from the status line rather than another entry on it, because the things that
/// belong here are sentences. A staged update the player has to restart for, and a session
/// error explaining why a join failed, are both useless abbreviated, and the status line
/// gives a line about three hundred points to work with.
///
/// One title line and two detail lines, so a message of any ordinary length wraps rather
/// than being cut off at whatever word happens to land on the edge.
std::uintptr_t g_notice_panel     = 0;
std::uintptr_t g_notice_rule      = 0;
std::uintptr_t g_notice_title     = 0;
std::uintptr_t g_notice_detail[2] = {0, 0};

/// One team slot's widgets, so a player joining rewrites a card instead of the screen.
///
/// Every part of the card is made whether or not the slot is occupied, and the parts that
/// do not apply are collapsed. Building both states up front is what lets somebody
/// arriving in the lobby appear on it without touching the widget tree.
struct SlotWidgets {
    std::uintptr_t backing{0};   ///< The card itself; its colour marks occupied or empty.
    std::uintptr_t strip{0};     ///< The team coloured bar along the top of a filled card.
    std::uintptr_t name{0};      ///< Who is in it.
    std::uintptr_t role{0};      ///< Owner or Player.
    std::uintptr_t plus{0};      ///< The invite glyph on an empty card.
    std::uintptr_t button{0};    ///< The whole card, pressable while empty.
};
constexpr int kTeamSlots = 5;
/// [0] blue, [1] red, matching the order the columns are drawn in.
SlotWidgets    g_slot[2][kTeamSlots];
/// The "n/5" beside each team title, rewritten as people come and go.
std::uintptr_t g_team_heading[2] = {0, 0};

/// One row of the invite list.
struct FriendRowWidgets {
    std::uintptr_t highlight{0}; ///< Marks a row an invite has already gone to.
    std::uintptr_t button{0};
    std::uintptr_t name{0};
    std::uintptr_t status{0};
};
FriendRowWidgets g_friend_row[kFriendRows];

/// The invite list's own widgets. Built with the lobby and kept collapsed.
std::uintptr_t g_invite_panel  = 0; ///< The canvas holding all of it.
std::uintptr_t g_friend_empty  = 0; ///< Shown when there is nobody to list.
std::uintptr_t g_friend_page   = 0; ///< The page indicator between the paging buttons.
bool           g_invite_open   = false;

/// Builds widgets and places them on a canvas.
///
/// Every call here is an engine call on the game thread, so the builder keeps them to the
/// minimum: create, parent, position, and set the one or two properties that matter.
class Builder {
public:
    explicit Builder(const LobbyUIContext& context) noexcept : context_(context) {}

    [[nodiscard]] const LobbyUIContext& Context() const noexcept { return context_; }

    /// Creates a widget of a class.
    [[nodiscard]] std::uintptr_t Spawn(std::uintptr_t widget_class) const {
        struct Parameters {
            std::uintptr_t object_class;
            std::uintptr_t outer;
            std::uintptr_t return_value;
        };
        Parameters parameters{};
        parameters.object_class = widget_class;
        parameters.outer        = context_.outer;
        if (!CallFunction(context_.gameplay_statics, context_.spawn_object, &parameters).ok()) {
            return 0;
        }
        return parameters.return_value;
    }

    /// Parents a widget to a canvas and places it.
    ///
    /// Anchors are left at the default top left, and the position and size are given in
    /// design space; the canvas scales the whole thing to the real viewport.
    [[nodiscard]] bool Place(std::uintptr_t canvas, std::uintptr_t widget, float x, float y,
                             float width, float height) const {
        struct AddParameters {
            std::uintptr_t content;
            std::uintptr_t return_value;
        };
        AddParameters add{};
        add.content = widget;
        if (!CallFunction(canvas, context_.add_to_canvas, &add).ok() ||
            add.return_value == 0) {
            return false;
        }
        const std::uintptr_t slot = add.return_value;

        struct VectorParameters {
            Vector2 value;
        };
        VectorParameters position{};
        position.value = {x, y};
        (void)CallFunction(slot, context_.set_position, &position);

        VectorParameters size{};
        size.value = {width, height};
        (void)CallFunction(slot, context_.set_size, &size);
        return true;
    }

    /// A filled rectangle, used for panels, bars and slots.
    [[nodiscard]] std::uintptr_t Panel(std::uintptr_t canvas, float x, float y, float width,
                                       float height, LinearColour colour) const {
        const std::uintptr_t border = Spawn(context_.border_class);
        if (border == 0) {
            return 0;
        }
        // Border's brush colour lives in its style rather than behind a setter that takes a
        // plain colour, so it is written directly. BrushColor sits at the start of the
        // background brush, which is the first property on the class.
        SetBorderColour(border, colour);
        if (!Place(canvas, border, x, y, width, height)) {
            return 0;
        }
        return border;
    }

    /// A line of text.
    [[nodiscard]] std::uintptr_t Text(std::uintptr_t canvas, float x, float y, float width,
                                      float height, std::string_view value,
                                      LinearColour colour, float size = 22.0F) const {
        const std::uintptr_t block = Spawn(context_.text_class);
        if (block == 0) {
            return 0;
        }
        SetText(block, value);
        SetTextAppearance(block, colour, size);
        if (!Place(canvas, block, x, y, width, height)) {
            return 0;
        }
        return block;
    }

    /// One of the game's own menu buttons, placed on the canvas.
    ///
    /// Created through the same path the main menu uses for its entries, so it arrives
    /// fully styled: the frontend's typeface, its hover and pressed states, its sounds and
    /// its focus handling. A border with text on it can look similar in a screenshot and is
    /// none of those things, and in particular it cannot be clicked.
    ///
    ///   the button's label is an FText at +0x15B0
    /// stretch is an EStretch: ScaleToFit keeps the button's proportions and centres it,
    /// which means its clickable area is only the shrunken art, not the box. Fill stretches
    /// it to the whole box instead, so the entire area responds. Anything showing a label
    /// wants ScaleToFit; anything that is purely a hit target wants Fill.
    [[nodiscard]] std::uintptr_t Button(std::uintptr_t canvas, float x, float y, float width,
                                        float height, std::string_view label,
                                        std::uint8_t stretch = 2) const {
        constexpr std::uintptr_t kLabelOffset = 0x15B0;
        constexpr std::size_t    kTextSize    = 0x10;

        if (context_.button_class == 0 || context_.create_widget == 0 ||
            context_.widget_library == 0) {
            return 0;
        }
        struct CreateParameters {
            std::uintptr_t world_context;
            std::uintptr_t widget_type;
            std::uintptr_t owning_player;
            std::uintptr_t return_value;
        };
        CreateParameters create{};
        create.world_context = context_.outer;
        create.widget_type   = context_.button_class;
        if (!CallFunction(context_.widget_library, context_.create_widget, &create).ok() ||
            create.return_value == 0) {
            return 0;
        }

        if (context_.convert_function != 0 && context_.text_library != 0) {
            std::wstring wide = WidenUtf8(label);
            wide.push_back(L'\0');
            struct ConvertParameters {
                struct {
                    wchar_t*     data;
                    std::int32_t count;
                    std::int32_t capacity;
                } input;
                std::uint8_t result[kTextSize];
            };
            ConvertParameters convert{};
            convert.input.data     = wide.data();
            convert.input.count    = static_cast<std::int32_t>(wide.size());
            convert.input.capacity = convert.input.count;
            if (CallFunction(context_.text_library, context_.convert_function, &convert)
                    .ok()) {
                (void)memory::GuardedWrite(create.return_value + kLabelOffset, convert.result,
                                           kTextSize);
            }
        }

        // Scaled to the box rather than dropped into it.
        //
        // The frontend's button is authored for the main menu's list and renders at that
        // size wherever it is put: placed straight into a canvas slot it does not shrink,
        // it clips. That is why the two mode buttons came out as a pair of bracket marks
        // with no label between them, and why START MATCH ran off the screen reading
        // "T MATCH". A scale box gives the button all the room it wants and then fits the
        // result to the space the layout actually has.
        const std::uintptr_t fitted = Spawn(context_.scalebox_class);
        if (fitted == 0) {
            return 0;
        }
        struct AddParameters {
            std::uintptr_t content;
            std::uintptr_t return_value;
        };
        AddParameters inside{};
        inside.content = create.return_value;
        if (!CallFunction(fitted, context_.add_child, &inside).ok() ||
            inside.return_value == 0) {
            return 0;
        }
        struct StretchParameters {
            std::uint8_t stretch;
        };
        StretchParameters mode{stretch};
        (void)CallFunction(fitted, context_.set_stretch, &mode);

        if (!Place(canvas, fitted, x, y, width, height)) {
            return 0;
        }
        return create.return_value;
    }

    /// A heading or a value drawn with the frontend's own button art.
    ///
    /// The same widget the lobby's buttons are made from, made non interactive: it is
    /// visible and it draws exactly as the game draws its menu text, but it does not answer
    /// the mouse, so it neither highlights nor swallows clicks meant for anything behind
    /// it. This is what makes a heading match the buttons instead of merely sitting near
    /// them in a similar colour.
    [[nodiscard]] std::uintptr_t Label(std::uintptr_t canvas, float x, float y, float width,
                                       float height, std::string_view text) const {
        const std::uintptr_t label = Button(canvas, x, y, width, height, text);
        if (label == 0) {
            return 0;
        }
        SetVisibilityOf(label, kHitTestInvisible);
        return label;
    }

    /// Replaces the text on a label or button built earlier.
    void Relabel(std::uintptr_t button, std::string_view text) const {
        constexpr std::uintptr_t kLabelOffset = 0x15B0;
        constexpr std::size_t    kTextSize    = 0x10;
        if (button == 0 || context_.convert_function == 0 || context_.text_library == 0) {
            return;
        }
        std::wstring wide = WidenUtf8(text);
        wide.push_back(L'\0');
        struct ConvertParameters {
            struct {
                wchar_t*     data;
                std::int32_t count;
                std::int32_t capacity;
            } input;
            std::uint8_t result[kTextSize];
        };
        ConvertParameters convert{};
        convert.input.data     = wide.data();
        convert.input.count    = static_cast<std::int32_t>(wide.size());
        convert.input.capacity = convert.input.count;
        if (CallFunction(context_.text_library, context_.convert_function, &convert).ok()) {
            (void)memory::GuardedWrite(button + kLabelOffset, convert.result, kTextSize);
        }
    }

    /// Changes the text of a block that is already on screen.
    ///
    /// Writing the Text property works only while the widget is being created, because the
    /// value is read once when Slate builds the widget and cached there afterwards. Setting
    /// it later stores a string in the object that nothing ever looks at again, which is why
    /// the status panel drew its background and its rule and no text at all.
    ///
    /// Calling the engine's own setter is what pushes the value through to Slate. It is
    /// resolved against TextBlock specifically, because SetText exists on several unrelated
    /// classes and calling the wrong one is a crash rather than a wrong result.
    void SetTextLive(std::uintptr_t block, std::string_view value) const {
        SetTextLiveOn(block, context_.set_text, value);
    }

    /// The same, against a setter the caller names.
    ///
    /// A text block and an editable box both take an FText and both cache it, but they are
    /// unrelated classes with their own SetText, and calling one on the other is a crash
    /// rather than a wrong result.
    void SetTextLiveOn(std::uintptr_t block, std::uintptr_t setter,
                       std::string_view value) const {
        if (block == 0 || setter == 0 || context_.convert_function == 0 ||
            context_.text_library == 0) {
            return;
        }
        std::wstring wide = WidenUtf8(value);
        wide.push_back(L'\0');

        struct ConvertParameters {
            struct {
                wchar_t*     data;
                std::int32_t count;
                std::int32_t capacity;
            } input;
            std::uint8_t result[0x10];
        };
        ConvertParameters convert{};
        convert.input.data     = wide.data();
        convert.input.count    = static_cast<std::int32_t>(wide.size());
        convert.input.capacity = convert.input.count;
        if (!CallFunction(context_.text_library, context_.convert_function, &convert).ok()) {
            return;
        }

        struct TextParameters {
            std::uint8_t text[0x10];
        };
        TextParameters parameters{};
        std::memcpy(parameters.text, convert.result, sizeof(parameters.text));
        (void)CallFunction(block, setter, &parameters);
    }

    /// Changes the colour of a block that is already on screen, for the same reason.
    ///
    /// The buffer is larger than FSlateColor needs, which is safe: the engine copies only as
    /// many bytes as the function declares, and a buffer that is too small is the dangerous
    /// direction.
    void SetColourLive(std::uintptr_t block, LinearColour colour) const {
        if (block == 0 || context_.set_color_and_opacity == 0) {
            return;
        }
        struct ColourParameters {
            LinearColour colour;
            std::uint8_t rule;
            std::uint8_t padding[7];
        };
        ColourParameters parameters{};
        parameters.colour = colour;
        parameters.rule   = 0; // UseColor_Specified
        (void)CallFunction(block, context_.set_color_and_opacity, &parameters);
    }

    void SetVisibilityOf(std::uintptr_t widget, std::uint8_t visibility) const {
        if (context_.set_visibility == 0 || widget == 0) {
            return;
        }
        struct Parameters {
            std::uint8_t visibility;
        };
        Parameters parameters{visibility};
        (void)CallFunction(widget, context_.set_visibility, &parameters);
    }

    /// A field the player can type into.
    ///
    ///   EditableTextBox +0x2E8 Text (FText, written directly)
    ///
    /// Written rather than passed to SetText, for the same reason the text block's caption
    /// is: SetText exists on several unrelated classes and calling the wrong one is a
    /// crash, not a wrong result.
    [[nodiscard]] std::uintptr_t Field(std::uintptr_t canvas, float x, float y, float width,
                                       float height, std::string_view initial) const {
        if (context_.editable_class == 0) {
            return 0;
        }
        const std::uintptr_t field = Spawn(context_.editable_class);
        if (field == 0) {
            return 0;
        }
        if (context_.has_font && context_.editable_font_offset != 0) {
            (void)memory::GuardedWrite(field + context_.editable_font_offset,
                                       context_.font_template.data(),
                                       context_.font_template.size());
            const float size = 20.0F;
            (void)memory::GuardedWrite(field + context_.editable_font_offset + 0x48, &size,
                                       sizeof(size));
        }
        if (context_.editable_colour_offset != 0) {
            constexpr std::uint8_t kUseSpecified = 0;
            (void)memory::GuardedWrite(field + context_.editable_colour_offset, &kText,
                                       sizeof(kText));
            (void)memory::GuardedWrite(field + context_.editable_colour_offset + 0x10,
                                       &kUseSpecified, sizeof(kUseSpecified));
        }
        if (context_.editable_hint_offset != 0) {
            SetFieldTextAt(field, context_.editable_hint_offset, "ENTER SERVER NAME");
        }
        if (!initial.empty()) {
            SetFieldText(field, initial);
        }
        if (!Place(canvas, field, x, y, width, height)) {
            return 0;
        }
        return field;
    }

    void SetFieldText(std::uintptr_t field, std::string_view value) const {
        SetFieldTextAt(field, 0x2E8, value);
    }

    void SetFieldTextAt(std::uintptr_t field, std::uintptr_t offset,
                        std::string_view value) const {
        const std::uintptr_t kFieldText = offset;
        std::wstring wide = WidenUtf8(value);
        wide.push_back(L'\0');
        struct ConvertParameters {
            struct {
                wchar_t*     data;
                std::int32_t count;
                std::int32_t capacity;
            } input;
            std::uint8_t result[0x10];
        };
        ConvertParameters convert{};
        convert.input.data     = wide.data();
        convert.input.count    = static_cast<std::int32_t>(wide.size());
        convert.input.capacity = convert.input.count;
        if (CallFunction(context_.text_library, context_.convert_function, &convert).ok()) {
            (void)memory::GuardedWrite(field + kFieldText, convert.result, sizeof(convert.result));
        }
    }

    /// The game's own panel backing, filling a rectangle.
    ///
    /// Falls back to a plain filled border when the frontend's backer is not available, so
    /// a panel is always drawn rather than a hole appearing in the layout.
    [[nodiscard]] std::uintptr_t Backer(std::uintptr_t canvas, float x, float y, float width,
                                        float height, LinearColour fallback) const {
        if (context_.backer_class == 0 || context_.create_widget == 0 ||
            context_.widget_library == 0) {
            return Panel(canvas, x, y, width, height, fallback);
        }
        struct CreateParameters {
            std::uintptr_t world_context;
            std::uintptr_t widget_type;
            std::uintptr_t owning_player;
            std::uintptr_t return_value;
        };
        CreateParameters create{};
        create.world_context = context_.outer;
        create.widget_type   = context_.backer_class;
        if (!CallFunction(context_.widget_library, context_.create_widget, &create).ok() ||
            create.return_value == 0) {
            return Panel(canvas, x, y, width, height, fallback);
        }
        if (!Place(canvas, create.return_value, x, y, width, height)) {
            return 0;
        }
        return create.return_value;
    }

    /// A nested canvas, so a section can be positioned as a unit.
    [[nodiscard]] std::uintptr_t Canvas(std::uintptr_t parent, float x, float y, float width,
                                        float height) const {
        const std::uintptr_t canvas = Spawn(context_.canvas_class);
        if (canvas == 0 || !Place(parent, canvas, x, y, width, height)) {
            return 0;
        }
        return canvas;
    }

    void SetText(std::uintptr_t block, std::string_view value) const {
        // The FText itself still comes from the engine, because it owns shared text data
        // that cannot be built by hand. Where it goes is different: it is written straight
        // into the text block's Text property rather than passed to SetText.
        //
        // SetText exists on several unrelated classes, and calling the wrong one on a text
        // block faulted reading 0xffffffffffffffff. Writing the property has no such
        // ambiguity, and the offset comes from the class's own reflection.
        constexpr std::uintptr_t kTextOffset = 0x188;

        std::wstring wide = WidenUtf8(value);

        struct FStringLayout {
            wchar_t*     data;
            std::int32_t count;
            std::int32_t capacity;
        };
        struct ConvertParameters {
            FStringLayout input;
            std::uint8_t  result[0x10];
        };
        ConvertParameters convert{};
        convert.input.data     = wide.data();
        convert.input.count    = static_cast<std::int32_t>(wide.size() + 1);
        convert.input.capacity = convert.input.count;
        if (!CallFunction(context_.text_library, context_.convert_function, &convert).ok()) {
            return;
        }
        (void)memory::GuardedWrite(block + kTextOffset, convert.result, sizeof(convert.result));
    }

    /// Writes a text block's colour and size.
    ///
    /// Two fields matter, not one. FSlateColor carries the colour and a rule saying whether
    /// to use it: the rule defaults to taking the foreground colour instead, so writing only
    /// the colour changes nothing. The font size is set explicitly because a size of zero
    /// draws nothing at all.
    ///
    ///   TextBlock +0x1A8 ColorAndOpacity  ( +0x00 SpecifiedColor, +0x10 ColorUseRule )
    ///   TextBlock +0x1D0 Font             ( +0x48 Size )
    void SetTextAppearance(std::uintptr_t block, LinearColour colour, float size) const {
        constexpr std::uintptr_t kColourOffset = 0x1A8;
        constexpr std::uintptr_t kRuleOffset   = 0x1A8 + 0x10;
        constexpr std::uintptr_t kFontSize     = 0x1D0 + 0x48;
        constexpr std::uint8_t   kUseSpecified = 0;

        (void)memory::GuardedWrite(block + kColourOffset, &colour, sizeof(colour));
        (void)memory::GuardedWrite(block + kRuleOffset, &kUseSpecified, sizeof(kUseSpecified));

        // The game's font first, then the size, in that order: the template carries the
        // size the block it was taken from happened to use, so writing it afterwards is
        // what keeps each line the size this layout asked for.
        if (context_.has_font) {
            (void)memory::GuardedWrite(block + 0x1D0, context_.font_template.data(),
                                       context_.font_template.size());
        }
        (void)memory::GuardedWrite(block + kFontSize, &size, sizeof(size));
    }

    /// Makes a border draw a solid rectangle in a colour.
    ///
    /// Colour alone is not enough. A newly created border's background brush has DrawAs set
    /// to nothing, so it renders no pixels whatever colour it is told to be. That is why an
    /// entire lobby could be built, attached and sized correctly and still show only the
    /// backdrop: every panel was present and drawing nothing.
    ///
    ///   Border +0x1C0 Background (FSlateBrush)
    ///     brush +0x08 TintColor ( +0x00 SpecifiedColor, +0x10 ColorUseRule )
    ///     brush +0x1C DrawAs
    ///   Border +0x280 BrushColor
    void SetBorderColour(std::uintptr_t border, LinearColour colour) const {
        if (border == 0) {
            return;
        }
        constexpr std::uintptr_t kBackground   = 0x1C0;
        constexpr std::uintptr_t kTintColour   = kBackground + 0x08;
        constexpr std::uintptr_t kTintRule     = kBackground + 0x08 + 0x10;
        constexpr std::uintptr_t kDrawAs       = kBackground + 0x1C;
        constexpr std::uintptr_t kBrushColour  = 0x280;
        constexpr std::uint8_t   kDrawAsBox    = 1; ///< A filled rectangle.
        constexpr std::uint8_t   kUseSpecified = 0;

        (void)memory::GuardedWrite(border + kDrawAs, &kDrawAsBox, sizeof(kDrawAsBox));
        (void)memory::GuardedWrite(border + kTintColour, &colour, sizeof(colour));
        (void)memory::GuardedWrite(border + kTintRule, &kUseSpecified, sizeof(kUseSpecified));
        (void)memory::GuardedWrite(border + kBrushColour, &colour, sizeof(colour));
    }

private:
    const LobbyUIContext& context_;
};

/// Builds one player card with both of its states, and records the pieces.
///
/// Occupied and empty are not separate cards. A slot fills when somebody joins and empties
/// when they leave, and both happen while the lobby is on screen, so a card that could only
/// be drawn one way would need the screen rebuilt underneath the player every time the
/// roster changed. Everything is created here and SetLobbyRoster decides what shows.
void BuildPlayerCard(const Builder& builder, std::uintptr_t canvas, SlotWidgets& out,
                     float x, float y, LinearColour team, std::vector<LobbyControl>& controls,
                     LobbyAction invite, int index) {
    constexpr float kCardWidth  = 132.0F;
    constexpr float kCardHeight = 168.0F;

    out.backing = builder.Panel(canvas, x, y, kCardWidth, kCardHeight, kSlot);

    // An empty slot is a real button, so pressing it opens the invite list rather than
    // merely drawing a plus sign that does nothing.
    //
    // A button is scaled to fit its box, and the frontend's button art is wide, so a card
    // this narrow crushes it to about a fifth of its size and the label with it. Drawing
    // the plus as text and putting the button behind it, non interactive, gives a glyph as
    // large as the card allows while the whole card stays pressable.
    out.button = builder.Button(canvas, x, y, kCardWidth, kCardHeight, " ", kStretchFill);
    controls.push_back({out.button, invite, index});

    out.plus = builder.Text(canvas, x + kCardWidth * 0.30F, y + kCardHeight * 0.28F,
                            kCardWidth, 80.0F, "+", kSlotMark, 72.0F);
    builder.SetVisibilityOf(out.plus, kHitTestInvisible);

    // The filled state: a team coloured strip so the side is readable at a glance rather
    // than only from the column heading, then the name and the role.
    out.strip = builder.Panel(canvas, x, y, kCardWidth, 4.0F, team);
    out.name  = builder.Text(canvas, x + 8.0F, y + kCardHeight - 46.0F, kCardWidth - 16.0F,
                             22.0F, "", kText);
    out.role  = builder.Text(canvas, x + 8.0F, y + kCardHeight - 24.0F, kCardWidth - 16.0F,
                             18.0F, "Player", kTextDim);
    builder.SetVisibilityOf(out.strip, kCollapsedValue);
    builder.SetVisibilityOf(out.name, kCollapsedValue);
    builder.SetVisibilityOf(out.role, kCollapsedValue);
}

/// The HOST tab.
void DrawHostTab(const Builder& builder, std::uintptr_t canvas, const LobbyView& view,
                 std::vector<LobbyControl>& controls) {
    // Left: mode selection.
    (void)builder.Label(canvas, 56.0F, 180.0F, 400.0F, 72.0F, "GAME MODE SELECTION");
    const std::array<const char*, 2> modes = {"CAPTURE THE FLAG", "SLAYER"};
    float                            mode_y = 252.0F;
    for (std::size_t index = 0; index < modes.size(); ++index) {
        const char* mode = modes[index];
        // Behind the button, so the frontend's own button art still reads normally and the
        // bar only marks which one is chosen.
        g_mode_marker[index] = builder.Panel(canvas, 52.0F, mode_y - 3.0F, 376.0F, 72.0F,
                                             kAccentDim);
        controls.push_back(
            {builder.Button(canvas, 60.0F, mode_y, 360.0F, 66.0F, mode),
             std::string_view(mode) == "SLAYER" ? LobbyAction::SelectSlayer
                                                : LobbyAction::SelectCaptureTheFlag});
        mode_y += 82.0F;
    }


    // Left, below the modes: the map.
    (void)builder.Label(canvas, 56.0F, 424.0F, 400.0F, 72.0F, "MAP SELECTION");
    for (std::size_t index = 0; index < std::size(kLobbyMaps); ++index) {
        const float y = 490.0F + static_cast<float>(index) * 62.0F;
        g_map_marker[index] =
            builder.Panel(canvas, 56.0F, y - 3.0F, 308.0F, 60.0F, kAccentDim);
        controls.push_back({builder.Button(canvas, 60.0F, y, 300.0F, 54.0F,
                                           kLobbyMaps[index].label),
                            LobbyAction::SelectMap, static_cast<int>(index)});
    }

    // Middle: the two team columns, five slots each in two rows.
    struct Column {
        const char*                     title;
        LinearColour                    colour;
        const std::vector<std::string>* players;
        float                           x;
    };
    const std::array<Column, 2> columns = {
        Column{"BLUE TEAM", kBlue, &view.blue, 470.0F},
        Column{"RED TEAM", kRed, &view.red, 940.0F},
    };

    for (std::size_t side = 0; side < columns.size(); ++side) {
        const Column& column = columns[side];
        // The title keeps the frontend's own heading art; only the count changes, so only
        // the count is a text block that can be rewritten in place.
        (void)builder.Label(canvas, column.x - 4.0F, 180.0F, 400.0F, 72.0F, column.title);
        g_team_heading[side] = builder.Text(canvas, column.x + 300.0F, 202.0F, 120.0F, 30.0F,
                                            "0/5", kAccent, 21.0F);
        for (int slot = 0; slot < kTeamSlots; ++slot) {
            const float x = column.x + static_cast<float>(slot % 3) * 144.0F;
            const float y = 250.0F + static_cast<float>(slot / 3) * 180.0F;
            BuildPlayerCard(builder, canvas, g_slot[side][slot], x, y, column.colour, controls,
                            side == 0 ? LobbyAction::InviteBlue : LobbyAction::InviteRed, slot);
        }
    }
    SetLobbyRoster(builder.Context(), view.blue, view.red, view.host_name);

    // Right: settings and server name.
    (void)builder.Backer(canvas, 1440.0F, 250.0F, 440.0F, 190.0F, kPanelLight);
    (void)builder.Label(canvas, 1444.0F, 172.0F, 440.0F, 74.0F, "LOBBY SETTINGS");
    (void)builder.Label(canvas, 1456.0F, 262.0F, 408.0F, 54.0F,
                        std::format("GAME TIME: {}min", view.game_time_minutes));
    (void)builder.Label(canvas, 1456.0F, 320.0F, 408.0F, 54.0F,
                        std::format("FRIENDLY FIRE: {}", view.friendly_fire ? "ON" : "OFF"));
    (void)builder.Label(canvas, 1456.0F, 378.0F, 408.0F, 54.0F,
                        std::format("RESPAWN TIME: {}s", view.respawn_seconds));

    (void)builder.Panel(canvas, 1452.0F, 498.0F, 416.0F, 68.0F, kPanelLight);
    (void)builder.Text(canvas, 1456.0F, 462.0F, 400.0F, 28.0F, "SERVER NAME", kAccent,
                       18.0F);
    g_server_name_field = builder.Field(canvas, 1468.0F, 512.0F, 384.0F, 40.0F,
                                        view.server_name);

    // Bottom right action.
    controls.push_back({builder.Button(canvas, 1380.0F, 916.0F, 500.0F, 104.0F, "START MATCH"),
                        LobbyAction::StartMatch});
    controls.push_back({builder.Button(canvas, 60.0F, 946.0F, 500.0F, 104.0F, "BACK"),
                        LobbyAction::Back, 0});
}

/// The BROWSE tab.
///
/// Everything here is built once. Filtering and refreshing rewrite the rows in place
/// through SetLobbyServers rather than building the screen again, so the browser stays as
/// instant as the rest of the lobby no matter how often the filters are changed.
void DrawBrowseTab(const Builder& builder, std::uintptr_t canvas, const LobbyView& view,
                   std::vector<LobbyControl>& controls) {
    // Left: filters. Each option is a real button, so the filter is a control rather than
    // a picture of one, with a marker behind the active choice.
    (void)builder.Label(canvas, 56.0F, 176.0F, 380.0F, 72.0F, "SERVER FILTERS");

    struct FilterRow {
        const char*                      caption;
        float                            y;
        std::array<const char*, 3>       options;
        std::array<LobbyAction, 3>       actions;
        std::uintptr_t*                  markers;
    };
    const std::array<FilterRow, 3> rows = {
        FilterRow{"MODE", 232.0F, {"ANY", "CTF", "SLAYER"},
                  {LobbyAction::FilterModeAny, LobbyAction::FilterModeCaptureTheFlag,
                   LobbyAction::FilterModeSlayer},
                  g_filter_mode},
        FilterRow{"SLOTS", 468.0F, {"ANY", "OPEN", "FULL"},
                  {LobbyAction::FilterSlotsAny, LobbyAction::FilterSlotsOpen,
                   LobbyAction::FilterSlotsFull},
                  g_filter_slots},
        FilterRow{"PING", 704.0F, {"ANY", "<50ms", "<100ms"},
                  {LobbyAction::FilterPingAny, LobbyAction::FilterPingUnder50,
                   LobbyAction::FilterPingUnder100},
                  g_filter_ping},
    };

    // Stacked, not in a row.
    //
    // Three options side by side left each about a hundred wide, and a button scaled into
    // a box that narrow renders its label unreadably small. Down the column each one gets
    // the full width, so the text is as large as the panel allows.
    for (const FilterRow& row : rows) {
        // Ruled, not merely larger.
        //
        // Size alone did not separate a heading from the buttons under it. A line above and
        // below makes the grouping structural: everything between two rules belongs to the
        // heading at the top of them.
        (void)builder.Panel(canvas, 56.0F, row.y - 8.0F, 308.0F, 2.0F, kAccent);
        (void)builder.Text(canvas, 64.0F, row.y + 10.0F, 300.0F, 30.0F, row.caption,
                           kAccent, 20.0F);
        (void)builder.Panel(canvas, 56.0F, row.y + 52.0F, 308.0F, 2.0F, kAccentDim);
        for (std::size_t option = 0; option < row.options.size(); ++option) {
            const float y = row.y + 66.0F + static_cast<float>(option) * 52.0F;
            row.markers[option] =
                builder.Panel(canvas, 56.0F, y - 3.0F, 308.0F, 52.0F, kAccentDim);
            controls.push_back({builder.Button(canvas, 60.0F, y, 300.0F, 46.0F,
                                               row.options[option]),
                                row.actions[option], 0});
        }
    }

    // Middle: the table. Headings in the frontend's own art, then eight permanent rows.
    const std::array<const char*, 5> headings = {"SERVER", "MODE", "MAP", "PLAYERS", "PING"};
    // Spaced so the widest heading fits its own column. PLAYERS is the long one, so the
    // gap after it is the one that matters; at thirty point it ran straight into PING.
    const std::array<float, 5> columns = {400.0F, 800.0F, 980.0F, 1120.0F, 1290.0F};
    const std::array<float, 5> widths  = {390.0F, 170.0F, 130.0F, 160.0F, 110.0F};

    // Text, not the button art.
    //
    // A label is a button scaled to fit its box, and a column is under two hundred wide, so
    // whatever size is asked for it comes out at about a third of it. A heading is not a
    // button, so it is drawn as text in the game's own font, where the size is the size.
    for (std::size_t index = 0; index < headings.size(); ++index) {
        (void)builder.Text(canvas, columns[index], 206.0F, widths[index], 26.0F,
                           headings[index], kAccent, 17.0F);
    }
    (void)builder.Panel(canvas, 390.0F, 244.0F, 1000.0F, 2.0F, kAccentDim);

    float row_y = 258.0F;
    for (std::size_t index = 0; index < kServerRows; ++index) {
        ServerRowWidgets& row = g_server_row[index];
        row.highlight = builder.Panel(canvas, 390.0F, row_y, 1000.0F, 62.0F, kAccentDim);

        // The pressable area goes down before the text, so the text draws over it rather
        // than being hidden behind it, and the text is made non interactive so the click
        // still reaches the row underneath. The whole row is the target: a server is
        // chosen by clicking it, not by a separate control that would need explaining.
        row.button = builder.Button(canvas, 390.0F, row_y, 1000.0F, 62.0F, " ",
                                    kStretchFill);
        controls.push_back({row.button, LobbyAction::SelectServer, static_cast<int>(index)});

        row.name    = builder.Text(canvas, columns[0] + 10.0F, row_y + 18.0F, widths[0],
                                   26.0F, "", kText, 24.0F);
        row.mode    = builder.Text(canvas, columns[1], row_y + 18.0F, widths[1], 26.0F, "",
                                   kTextDim, 22.0F);
        row.map     = builder.Text(canvas, columns[2], row_y + 18.0F, widths[2], 26.0F, "",
                                   kTextDim, 22.0F);
        row.players = builder.Text(canvas, columns[3], row_y + 18.0F, widths[3], 26.0F, "",
                                   kTextDim, 22.0F);
        row.ping    = builder.Text(canvas, columns[4], row_y + 18.0F, widths[4], 26.0F, "",
                                   kTextDim, 22.0F);

        // Under the name rather than in a column of its own. The table is already as wide
        // as the design allows, and this qualifies the server rather than being another
        // field of it: a row that says IN GAME is the same server, differently joinable.
        row.status  = builder.Text(canvas, columns[0] + 10.0F, row_y + 42.0F, widths[0],
                                   20.0F, "", kTextDim, 15.0F);
        for (const std::uintptr_t block : {row.name, row.mode, row.map, row.players,
                                           row.ping}) {
            builder.SetVisibilityOf(block, kHitTestInvisible);
        }
        row_y += 70.0F;
    }

    g_empty_notice = builder.Text(canvas, 400.0F, 300.0F, 900.0F, 40.0F,
                                  "No games found. Host one, or refresh.", kTextDim, 26.0F);

    // Right: details of the highlighted server.
    (void)builder.Backer(canvas, 1440.0F, 250.0F, 440.0F, 330.0F, kPanelLight);
    (void)builder.Label(canvas, 1444.0F, 172.0F, 440.0F, 74.0F, "SERVER DETAILS");
    for (std::size_t line = 0; line < std::size(g_detail_line); ++line) {
        g_detail_line[line] =
            builder.Text(canvas, 1462.0F, 266.0F + static_cast<float>(line) * 40.0F, 400.0F,
                         30.0F, "", kTextDim, 24.0F);
    }

    // A search runs on a timer while this tab is open, but a player who has just been told
    // to host a game wants to look again now rather than wait out the interval, and a list
    // that only updates on its own gives them no way to tell waiting from broken.
    controls.push_back({builder.Button(canvas, 1380.0F, 812.0F, 500.0F, 88.0F, "REFRESH"),
                        LobbyAction::RefreshServers, 0});
    controls.push_back({builder.Button(canvas, 1380.0F, 916.0F, 500.0F, 104.0F, "JOIN MATCH"),
                        LobbyAction::JoinMatch, 0});
    controls.push_back({builder.Button(canvas, 60.0F, 946.0F, 500.0F, 104.0F, "BACK"),
                        LobbyAction::Back, 0});

    SetLobbyServers(builder.Context(), view.servers, view.selected_server);
}

/// The invite list, drawn over the lobby.
///
/// WHY THE MOD DRAWS THIS RATHER THAN USING WHAT ALREADY EXISTS
///
/// There are two invite screens in reach and neither one works here. Steam's own dialog is
/// drawn by the in-game overlay, which is enabled in this process and does not render over
/// this title, so opening it succeeds and nothing appears. The game's invite screen sends
/// its invitations through PlayFab to a co-op fireteam, which is a different system from
/// the Steam lobby this session is, and pointing it at a Steam lobby id is not something it
/// has a way to express.
///
/// So the list is built from the friends Steam reports and each row invites one person to
/// this lobby directly, which needs no overlay and no fireteam.
void DrawInvitePanel(const Builder& builder, std::uintptr_t canvas,
                     std::vector<LobbyControl>& controls) {
    // Full bleed, and deliberately opaque enough to read against. This also swallows every
    // click that misses a row, so the lobby underneath cannot be operated by accident while
    // a modal choice is open.
    (void)builder.Panel(canvas, 0.0F, 0.0F, kDesignWidth, kDesignHeight, {0, 0, 0, 0.78F});

    (void)builder.Backer(canvas, 580.0F, 150.0F, 760.0F, 840.0F, kPanel);
    (void)builder.Panel(canvas, 604.0F, 228.0F, 712.0F, 2.0F, kAccent);
    (void)builder.Text(canvas, 604.0F, 182.0F, 712.0F, 40.0F, "INVITE TO THIS SESSION",
                       kAccent, 26.0F);
    (void)builder.Text(canvas, 604.0F, 236.0F, 712.0F, 26.0F,
                       "Anyone here joins this multiplayer session, not a fireteam.",
                       kTextDim, 17.0F);

    float row_y = 278.0F;
    for (int index = 0; index < kFriendRows; ++index) {
        FriendRowWidgets& row = g_friend_row[index];
        row.highlight = builder.Panel(canvas, 600.0F, row_y, 720.0F, 52.0F, kAccentDim);
        row.button    = builder.Button(canvas, 600.0F, row_y, 720.0F, 52.0F, " ",
                                       kStretchFill);
        row.name      = builder.Text(canvas, 618.0F, row_y + 12.0F, 500.0F, 30.0F, "", kText,
                                     22.0F);
        row.status    = builder.Text(canvas, 1130.0F, row_y + 14.0F, 180.0F, 28.0F, "",
                                     kTextDim, 18.0F);
        builder.SetVisibilityOf(row.name, kHitTestInvisible);
        builder.SetVisibilityOf(row.status, kHitTestInvisible);
        builder.SetVisibilityOf(row.highlight, kCollapsedValue);
        builder.SetVisibilityOf(row.button, kCollapsedValue);
        controls.push_back({row.button, LobbyAction::SelectFriend, index});
        row_y += 58.0F;
    }

    g_friend_empty = builder.Text(canvas, 604.0F, 420.0F, 712.0F, 40.0F,
                                  "Nobody on your Steam friends list is online.", kTextDim,
                                  22.0F);
    builder.SetVisibilityOf(g_friend_empty, kHitTestInvisible);

    controls.push_back({builder.Button(canvas, 600.0F, 866.0F, 210.0F, 62.0F, "PREV"),
                        LobbyAction::FriendsPrevious, 0});
    controls.push_back({builder.Button(canvas, 1110.0F, 866.0F, 210.0F, 62.0F, "NEXT"),
                        LobbyAction::FriendsNext, 0});
    g_friend_page = builder.Text(canvas, 830.0F, 880.0F, 260.0F, 30.0F, "", kTextDim, 19.0F);
    builder.SetVisibilityOf(g_friend_page, kHitTestInvisible);

    controls.push_back({builder.Button(canvas, 600.0F, 916.0F, 720.0F, 74.0F, "CLOSE"),
                        LobbyAction::CloseInvite, 0});
}

} // namespace

namespace {
/// The lobby currently on screen, so it can be replaced or closed.
std::uintptr_t g_open_lobby_root = 0;
/// The user widget carrying it, which is what the viewport actually holds.
std::uintptr_t g_open_lobby_widget = 0;

/// The host user widget in the viewport. Removing this takes the whole lobby with it.
std::uintptr_t g_open_host_widget = 0;

/// The two tab canvases. Both exist at once; visibility decides which is on screen.
std::uintptr_t g_host_tab   = 0;
std::uintptr_t g_browse_tab = 0;


/// Creates a user widget, gives it a canvas as its content, and shows it.
///
/// The canvas is installed as the widget's tree root before the widget is added, because
/// adding it is what builds the underlying Slate representation: set the root afterwards
/// and the widget is already built around whatever it had before.
///
///   UserWidget +0x268 WidgetTree, WidgetTree +0x30 RootWidget
/// Attaches a correctly sized canvas into the game's own menu.
///
/// Native throughout: the canvas becomes a child of the main menu's existing root, so the
/// game draws, scales and owns it like any other part of that screen. No separate window,
/// no borrowed developer widget, and nothing added to the viewport on its own.
///
/// Two things are done explicitly because leaving them to defaults is what produced a
/// screen that built cleanly and drew nothing:
///
///   A canvas panel reports no desired size. Placed in an overlay it is given zero unless
///   the slot stretches it, so it is wrapped in a size box with a real width and height.
///   The size box's override flags share one bitfield at +0x1B0; without those bits the
///   values at +0x190 and +0x194 are ignored.
///
///   The overlay slot's alignment is written directly at +0x50 and +0x51 rather than
///   through setters. Those setters exist on several slot types, and one resolved against
///   the wrong class fails silently, leaving the alignment at its default.
/// ESlateVisibility. Collapsed takes the widget out of layout as well as out of the draw,
/// which is what a screen change should do; Hidden would leave it occupying its space.
constexpr std::uint8_t kVisible   = 0;
constexpr std::uint8_t kCollapsed = 1;
constexpr std::uint8_t kHidden    = 2;
/// Drawn, but does not answer the mouse itself; its children still do.
///
/// The tab canvases cover the whole screen and are added after the HOST and BROWSE buttons,
/// so they sit on top of them. Hit testable, a canvas swallows the click before it reaches
/// the button underneath, which left exactly those two buttons dead while every button
/// inside a tab kept working.
constexpr std::uint8_t kSelfHitTestInvisible = 4;

/// The menu widgets folded away while the lobby is up, so they can be brought back.
/// A widget folded away, and what its visibility was before it was.
struct FoldedWidget {
    std::uintptr_t widget{0};
    std::uint8_t   previous{kSelfHitTestInvisible};
};
std::vector<FoldedWidget> g_folded;

void SetWidgetVisibility(const LobbyUIContext& context, std::uintptr_t widget,
                         std::uint8_t visibility) {
    if (context.set_visibility == 0 || widget == 0) {
        return;
    }
    struct Parameters {
        std::uint8_t visibility;
    };
    Parameters parameters{visibility};
    (void)CallFunction(widget, context.set_visibility, &parameters);
}

/// What a widget's visibility is right now.
///
/// Returns SelfHitTestInvisible when it cannot be read, because that is the safe direction
/// to be wrong in: a container that does not hit test itself still lets everything under it
/// be clicked, while one restored as Visible swallows every click that lands on it.
[[nodiscard]] std::uint8_t ReadWidgetVisibility(const LobbyUIContext& context,
                                                std::uintptr_t       widget) {
    if (context.get_visibility == 0 || widget == 0) {
        return kSelfHitTestInvisible;
    }
    struct Parameters {
        std::uint8_t return_value;
    };
    Parameters parameters{kSelfHitTestInvisible};
    if (!CallFunction(widget, context.get_visibility, &parameters).ok()) {
        return kSelfHitTestInvisible;
    }
    return parameters.return_value;
}

/// Folds the main menu away, leaving the lobby as the only thing on the root.
///
/// This is what makes the entry a screen rather than a panel drawn over the menu. Without
/// it the campaign list, the fireteam panel and the last played card stay on screen behind
/// the lobby, which is not a multiplayer screen, it is the main menu with something on top
/// of it.
///
/// The children are read from the root rather than taken from a list of offsets, so this
/// stays correct if the menu is laid out differently than expected.
void FoldMenuAway(const LobbyUIContext& context) {
    // The whole menu widget, not the children of its root.
    //
    // Collapsing the children hid everything that was drawn and still left the menu itself
    // in the viewport, alive and handling input: moving the mouse went on playing its hover
    // sounds for buttons that were no longer on screen, and two of its panels were not
    // under that root at all so they stayed visible. The lobby is hosted in the viewport
    // now rather than inside the menu, so the menu can be collapsed whole, which takes its
    // drawing, its layout and its input with it in one call.
    if (context.outer == 0) {
        return;
    }

    // Read before collapsing, in that order, or every widget records Collapsed as the
    // state to go back to and the menu never comes back at all.
    const auto fold = [&](std::uintptr_t widget) {
        std::uint8_t previous = ReadWidgetVisibility(context, widget);

        // A hidden state is never restored, whatever was read.
        //
        // Reading Collapsed or Hidden here means either this ran twice or the read
        // failed in a way that looked like a value, and honouring it would leave the
        // player at an empty screen with no menu and no way back. The visible states are
        // the only sane answers, so anything else becomes the safest of them.
        if (previous == kCollapsed || previous == kHidden) {
            MPE_LOG_WARN("widget 0x{:X} was already hidden ({}) when the menu was folded; it "
                        "will be restored as visible rather than left hidden",
                        widget, previous);
            previous = kSelfHitTestInvisible;
        }

        g_folded.push_back(FoldedWidget{widget, previous});
        SetWidgetVisibility(context, widget, kCollapsed);
    };

    fold(context.outer);
    for (const std::uintptr_t widget : context.also_fold) {
        fold(widget);
    }
    MPE_LOG_INFO("main menu 0x{:X} collapsed whole (visibility was {}), with {} widget(s) "
                "beside it",
                context.outer, g_folded.front().previous, context.also_fold.size());
}

/// Points input at the lobby instead of the menu underneath it.
///
/// Folding the menu away hides it, and a collapsed widget cannot be hit tested, but the
/// frontend keeps handling input at the controller level: moving the mouse still played the
/// menu's hover sounds for buttons that were no longer on screen. Taking focus is what
/// makes the lobby a screen the player is actually on rather than a picture over one.
/// Passing this as the widget hands focus back to the game viewport rather than to any
/// particular widget. See FocusLobby for why that is the only way to give the menu back.
constexpr std::uintptr_t kFocusTheViewport = 0;

void FocusLobby(const LobbyUIContext& context, std::uintptr_t widget) {
    if (context.set_input_mode_ui == 0 || context.get_player_controller == 0 ||
        context.widget_library == 0) {
        MPE_LOG_WARN("input cannot be given to the lobby: focus functions were not resolved");
        return;
    }

    struct ControllerParameters {
        std::uintptr_t world_context;
        std::int32_t   player_index;
        std::uintptr_t return_value;
    };
    ControllerParameters controller{};
    controller.world_context = context.outer;
    controller.player_index  = 0;
    if (!CallFunction(context.gameplay_statics, context.get_player_controller, &controller)
             .ok() ||
        controller.return_value == 0) {
        MPE_LOG_WARN("no player controller to take input from");
        return;
    }

    //   SetInputMode_UIOnlyEx(PlayerController, WidgetToFocus, MouseLockMode, bFlushInput)
    // The flush matters: without it the press that opened the lobby is still in the queue
    // and arrives at the new screen.
    struct InputModeParameters {
        std::uintptr_t player_controller;
        std::uintptr_t widget_to_focus;
        std::uint8_t   mouse_lock_mode;
        bool           flush_input;
    };
    InputModeParameters mode{};
    mode.player_controller = controller.return_value;
    mode.widget_to_focus   = widget;
    mode.mouse_lock_mode   = 0; // DoNotLock
    mode.flush_input       = true;
    (void)CallFunction(context.widget_library, context.set_input_mode_ui, &mode);

    // Keyboard focus is only forced when there is a widget to force it onto. Doing it to
    // the menu is what left it dead: see below.
    if (widget != kFocusTheViewport && context.set_keyboard_focus != 0) {
        (void)CallFunction(widget, context.set_keyboard_focus, nullptr);
    }
    MPE_LOG_INFO("input focus moved to {} (controller 0x{:X})",
                widget == kFocusTheViewport ? "the game viewport" : "the lobby",
                controller.return_value);
}

void UnfoldMenu(const LobbyUIContext& context) {
    // Put back exactly what was there, not Visible.
    //
    // Restoring everything as Visible is what left the main menu on screen and completely
    // dead, with no button on it answering a click and no way out but closing the game.
    //
    // ESlateVisibility::Visible means the widget hit tests itself, so a container restored
    // that way swallows every click that lands on it before any of its children see one.
    // Menu containers are authored SelfHitTestInvisible precisely so their buttons get the
    // input, and folding the menu away silently overwrote that. The widget was drawn, it
    // looked right, and it ate everything.
    for (const FoldedWidget& folded : g_folded) {
        SetWidgetVisibility(context, folded.widget, folded.previous);
    }
    if (!g_folded.empty()) {
        MPE_LOG_INFO("restored {} main menu widget(s), the menu root to visibility {}",
                    g_folded.size(), g_folded.front().previous);
    }
    g_folded.clear();
}

/// Builds a design sized canvas, scales it to the viewport and adds it as a widget.
///
/// z_order decides what draws over what. The lobby sits at 1000; anything meant to stay
/// visible while the lobby is hidden, and to sit above it while it is not, goes higher.
///
/// out_host receives the user widget the viewport holds, which is the handle for removing
/// it again or changing its visibility. out_scaler receives the scale box, which is what
/// reports the laid out size.
[[nodiscard]] std::uintptr_t CreateHostedCanvasAt(const LobbyUIContext& context,
                                                  std::int32_t          z_order,
                                                  std::uintptr_t&       out_host,
                                                  std::uintptr_t&       out_scaler) {
    constexpr std::uintptr_t kWidthOverride   = 0x190;
    constexpr std::uintptr_t kHeightOverride  = 0x194;
    constexpr std::uintptr_t kOverrideFlags   = 0x1B0;
    constexpr std::uint8_t   kWidthAndHeight  = 0x03; // bOverride_Width | bOverride_Height

    struct SpawnParameters {
        std::uintptr_t object_class;
        std::uintptr_t outer;
        std::uintptr_t return_value;
    };
    const auto spawn = [&](std::uintptr_t widget_class) -> std::uintptr_t {
        SpawnParameters parameters{};
        parameters.object_class = widget_class;
        parameters.outer        = context.outer;
        if (!CallFunction(context.gameplay_statics, context.spawn_object, &parameters).ok()) {
            return 0;
        }
        return parameters.return_value;
    };

    const std::uintptr_t frame = spawn(context.sizebox_class);
    if (frame == 0) {
        MPE_LOG_WARN("could not create the sizing frame");
        return 0;
    }
    const float width  = kDesignWidth;
    const float height = kDesignHeight;
    (void)memory::GuardedWrite(frame + kWidthOverride, &width, sizeof(width));
    (void)memory::GuardedWrite(frame + kHeightOverride, &height, sizeof(height));
    (void)memory::GuardedWrite(frame + kOverrideFlags, &kWidthAndHeight,
                               sizeof(kWidthAndHeight));

    const std::uintptr_t canvas = spawn(context.canvas_class);
    if (canvas == 0) {
        MPE_LOG_WARN("could not create the lobby canvas");
        return 0;
    }

    // Canvas inside the frame.
    struct AddParameters {
        std::uintptr_t content;
        std::uintptr_t return_value;
    };
    AddParameters inner{};
    inner.content = canvas;
    if (context.add_child == 0 ||
        !CallFunction(frame, context.add_child, &inner).ok() || inner.return_value == 0) {
        MPE_LOG_WARN("the sizing frame did not accept the canvas");
        return 0;
    }

    // Frame into a scale box, so the fixed design resizes to the viewport.
    //
    // Without this the size box holds the lobby at exactly 1920 by 1080 regardless of the
    // display, which on anything larger drew the whole screen in the top left corner at one
    // to one with the game still visible around it.
    const std::uintptr_t scaler = spawn(context.scalebox_class);
    if (scaler == 0) {
        MPE_LOG_WARN("could not create the scaling box");
        return 0;
    }
    AddParameters scaled{};
    scaled.content = frame;
    if (!CallFunction(scaler, context.add_child, &scaled).ok() || scaled.return_value == 0) {
        MPE_LOG_WARN("the scaling box did not accept the lobby frame");
        return 0;
    }
    // EStretch::ScaleToFit keeps the design's proportions and fits it to the viewport.
    struct StretchParameters {
        std::uint8_t stretch;
    };
    StretchParameters stretch{2};
    (void)CallFunction(scaler, context.set_stretch, &stretch);

    // The scale box goes into the viewport inside a host widget, not into the menu.
    //
    // A user widget is the only thing the viewport takes, and its Slate is built when it is
    // added rather than when it is created, so its tree root can be swapped for the lobby
    // in between. That ordering is the whole trick: set the root afterwards and the widget
    // has already been built around whatever it had before.
    //
    //   UserWidget +0x268 WidgetTree, WidgetTree +0x30 RootWidget
    struct CreateParameters {
        std::uintptr_t world_context;
        std::uintptr_t widget_type;
        std::uintptr_t owning_player;
        std::uintptr_t return_value;
    };
    CreateParameters created{};
    created.world_context = context.outer;
    created.widget_type   = context.host_class;
    if (context.create_widget == 0 || context.host_class == 0 ||
        context.add_to_viewport == 0 ||
        !CallFunction(context.widget_library, context.create_widget, &created).ok() ||
        created.return_value == 0) {
        MPE_LOG_WARN("could not create the widget that carries the lobby into the viewport");
        return 0;
    }

    std::uintptr_t host_tree = 0;
    if (!memory::GuardedRead(created.return_value + 0x268, &host_tree, sizeof(host_tree)) ||
        host_tree == 0 ||
        !memory::GuardedWrite(host_tree + 0x30, &scaler, sizeof(scaler))) {
        MPE_LOG_WARN("the host widget 0x{:X} would not take the lobby as its root",
                    created.return_value);
        return 0;
    }

    // Above the menu, so nothing of the frontend can draw over it.
    struct ViewportParameters {
        std::int32_t z_order;
    };
    ViewportParameters viewport{z_order};
    if (!CallFunction(created.return_value, context.add_to_viewport, &viewport).ok()) {
        MPE_LOG_WARN("the lobby host was not accepted by the viewport");
        return 0;
    }
    out_host   = created.return_value;
    out_scaler = scaler;

    // What the viewport is actually handing out, which is the number that decides whether
    // the design scales up or down. Guessing at this is what produced a half sized screen.
    struct SizeParameters {
        std::uintptr_t world_context;
        double         x;
        double         y;
    };
    SizeParameters viewport_size{};
    viewport_size.world_context = context.outer;
    if (context.get_viewport_size != 0 && context.layout_library != 0) {
        (void)CallFunction(context.layout_library, context.get_viewport_size, &viewport_size);
    }

    MPE_LOG_INFO("lobby hosted in the viewport: host 0x{:X}, scaler 0x{:X}, frame 0x{:X} "
                "({}x{} design), canvas 0x{:X}; viewport is {:.0f}x{:.0f}",
                created.return_value, scaler, frame, static_cast<int>(width),
                static_cast<int>(height), canvas, viewport_size.x, viewport_size.y);

    // Neither folded nor focused here. The lobby is built ahead of time and kept hidden,
    // so the menu must stay exactly as it is until the screen is actually shown.
    return canvas;
}

} // namespace

Result ProbeLobbyUI(const LobbyUIContext& context) {
    if (!context.Complete()) {
        return Result::Fail(ErrorCode::InvalidState, "the lobby UI context is incomplete");
    }
    RemoveLobbyUI(context);

    const std::uintptr_t canvas =
        CreateHostedCanvasAt(context, 1000, g_open_host_widget, g_open_lobby_widget);
    if (canvas == 0) {
        return Result::Fail(ErrorCode::InvalidState, "could not host a canvas");
    }
    g_open_lobby_root = canvas;

    const Builder builder(context);
    // Deliberately garish and large: the point is to be impossible to miss if it draws.
    if (builder.Panel(canvas, 200.0F, 200.0F, 900.0F, 500.0F, {1.0F, 0.0F, 0.0F, 1.0F}) == 0) {
        return Result::Fail(ErrorCode::InvalidState, "the probe rectangle was not created");
    }
    (void)builder.Text(canvas, 240.0F, 240.0F, 800.0F, 60.0F, "FORGE EVOLVED UI PROBE",
                       {1.0F, 1.0F, 1.0F, 1.0F}, 40.0F);
    MPE_LOG_INFO("probe drawn: a red rectangle with white text should now be on screen");
    return Result::Success();
}

void RemoveLobbyUI(const LobbyUIContext& context) {
    // The viewport holds the hosting widget, so that is what has to be removed. Removing
    // the canvas inside it would leave an empty widget still on screen.
    // The host is what the viewport holds, so removing it takes the scale box, the frame,
    // the canvas and everything on it away in one call.
    if (g_open_host_widget != 0 && context.remove_from_parent != 0) {
        (void)CallFunction(g_open_host_widget, context.remove_from_parent, nullptr);
    }
    g_open_host_widget  = 0;
    g_open_lobby_widget = 0;
    g_open_lobby_root   = 0;
    g_host_tab          = 0;
    g_browse_tab        = 0;
    g_invite_panel      = 0;
    g_invite_open       = false;
    g_friend_empty      = 0;
    g_friend_page       = 0;
    for (FriendRowWidgets& row : g_friend_row) {
        row = FriendRowWidgets{};
    }
    for (auto& side : g_slot) {
        for (SlotWidgets& card : side) {
            card = SlotWidgets{};
        }
    }
    g_team_heading[0] = 0;
    g_team_heading[1] = 0;

    // The menu comes back with the lobby's departure, not on a separate call, so there is
    // no path that closes the lobby and leaves the player looking at an empty screen.
    UnfoldMenu(context);
}

std::uintptr_t OpenLobbyFrame() { return g_open_lobby_widget; }

void MeasureLobby(const LobbyUIContext& context) {
    if (context.get_desired_size == 0 || g_open_lobby_widget == 0) {
        return;
    }
    // FVector2D is double precision in UE5, so this is two doubles rather than two floats.
    struct SizeParameters {
        double x;
        double y;
    };
    SizeParameters frame_size{};
    SizeParameters canvas_size{};
    (void)CallFunction(g_open_lobby_widget, context.get_desired_size, &frame_size);
    if (g_open_lobby_root != 0) {
        (void)CallFunction(g_open_lobby_root, context.get_desired_size, &canvas_size);
    }
    // Read a frame after building, because desired size is whatever the last layout pass
    // cached and a widget created this frame has not been through one yet.
    MPE_LOG_INFO("lobby measured: frame {:.0f}x{:.0f}, canvas {:.0f}x{:.0f}", frame_size.x,
                frame_size.y, canvas_size.x, canvas_size.y);
}

Result ResolveLobbyStatics(const ObjectArray& objects, LobbyUIContext& out_context) {
    LobbyUIContext context;

    // The game's own widget classes, listed once.
    //
    // The lobby is meant to be built from the game's widgets rather than from engine
    // primitives dressed to look like them, and that needs to start from what actually
    // exists in this build rather than from names guessed off a wiki.
    std::vector<std::string> game_widgets;

    /// Candidates for the font, and the game widgets they might belong to.
    std::vector<ObjectInfo>          text_blocks;
    std::unordered_set<std::uintptr_t> game_widget_instances;

    /// Everything needed to resolve a function by name and owning class, gathered in the
    /// one pass this function already makes.
    ///
    /// Each owner qualified lookup used to be its own scan of fifty thousand objects, and
    /// there are nine of them. That was fifteen seconds, and because the menu entry is only
    /// added once this has finished, it was fifteen seconds during which the main menu sat
    /// there without a MULTIPLAYER entry on it. A function's outer is the class that
    /// declares it, so keeping the classes by address and the functions by outer turns all
    /// nine lookups into map lookups over data already in hand.
    struct FunctionRecord {
        std::string    name;
        std::uintptr_t outer{0};
        std::uintptr_t address{0};
    };
    std::vector<FunctionRecord>                     functions;
    std::unordered_map<std::uintptr_t, std::string> class_names;

    // One pass over the object array. Doing a pass per lookup is what made the first lobby
    // stall the game noticeably.
    objects.ForEach([&](const ObjectInfo& object) {
        const bool is_default = object.name.rfind("Default__", 0) == 0;
        const bool is_class   = object.class_name.find("Class") != std::string::npos;

        if (object.class_name == "Function") {
            functions.emplace_back(FunctionRecord{object.name, object.outer_address,
                                                  object.address});
            // Only the unambiguous names are taken here. SetText, SetPosition, SetSize and
            // SetColorAndOpacity each exist on several unrelated classes, and calling one
            // class's function on another object is not a wrong result, it is a crash: an
            // earlier version picked whichever matched first and faulted reading
            // 0xffffffffffffffff inside the text block. Those are resolved by owner below.
            if (!context.spawn_object && object.name == "SpawnObject") {
                context.spawn_object = object.address;
            } else if (!context.convert_function && object.name == "Conv_StringToText") {
                context.convert_function = object.address;
            }
        } else if (is_default) {
            if (object.name == "Default__GameplayStatics") {
                context.gameplay_statics = object.address;
            } else if (object.name == "Default__KismetTextLibrary") {
                context.text_library = object.address;
            } else if (object.name == "Default__WidgetBlueprintLibrary") {
                context.widget_library = object.address;
            } else if (object.name == "Default__WidgetLayoutLibrary") {
                context.layout_library = object.address;
            }
        } else if (object.class_name == "TextBlock") {
            // Kept for a second pass rather than taken here. Any text block will hand over
            // a font, but most of them hand over the engine default: taking the first one
            // found produced a lobby in Roboto, which is exactly what it looked like. Only
            // a block belonging to one of the game's own widgets carries the menu typeface,
            // and whether it does cannot be told without knowing what owns it.
            text_blocks.push_back(object);
        } else if (!is_class && object.name.rfind("WBP_", 0) == 0) {
            // Instances only. Without the class check this arm also catches the classes,
            // which are named the same way, and they then never reach the arm below that
            // resolves them: the host widget and the button both came back null and the
            // lobby failed to open at all.
            game_widget_instances.insert(object.address);
        } else if (is_class) {
            class_names.emplace(object.address, object.name);
            if (object.name.rfind("WBP_", 0) == 0 || object.name.rfind("W_", 0) == 0) {
                game_widgets.push_back(object.name);
            }
            if (object.name == "CanvasPanel") {
                context.canvas_class = object.address;
            } else if (object.name == "TextBlock") {
                context.text_class = object.address;
            } else if (object.name == "Border") {
                context.border_class = object.address;
            } else if (object.name == "SizeBox") {
                context.sizebox_class = object.address;
            } else if (object.name == "ScaleBox") {
                context.scalebox_class = object.address;
            } else if (object.name == "EditableText") {
                // EditableText, not EditableTextBox.
                //
                // The box variant ships a white background brush and a grey text style,
                // which is why the field looked like it belonged to a different program.
                // The plain variant draws only the text and the caret, so the panel behind
                // it is ours and the whole thing matches the rest of the screen.
                context.editable_class = object.address;
            } else if (object.name == "WBP_MeteoriteStandaloneButtonDefault_C") {
                context.button_class = object.address;
            } else if (object.name == "WBP_FrontendMenuBacker_C") {
                // The frontend's own panel backing. A border filled with a colour picked by
                // eye is the last part of the lobby that is not the game's, and this is
                // what the game puts behind its own menu panels.
                context.backer_class = object.address;
            } else if (object.name == "WBP_FadeOverlay_C") {
                // Only ever used as an empty shell to carry the lobby into the viewport:
                // its own tree is replaced before it is shown. A full screen overlay is
                // the right shape for that and has no layout of its own worth keeping.
                context.host_class = object.address;
            }
        }
        return true;
    });

    // Resolved against the class that declares them, so each is the one that belongs to the
    // object it will be called on.
    //
    //   SetText              on TextBlock, not EditableText or RichTextBlock
    //   SetColorAndOpacity   on TextBlock
    //   SetPosition/SetSize  on CanvasPanelSlot, whose SetSize takes a Vector2D; the
    //                        HorizontalBox slot's SetSize takes a SlateChildSize instead
    //   AddChildToCanvas     on CanvasPanel
    //   AddChild             on PanelWidget, which is what makes it work for any panel
    // Owner qualified, because the names are not unique. SetText exists on four unrelated
    // classes and SetPosition on five, and calling one class's function on another object
    // is not a wrong answer, it is a crash: an earlier version took whichever matched first
    // and faulted reading 0xffffffffffffffff inside a text block.
    const auto find = [&](std::string_view name, std::string_view owner) -> std::uintptr_t {
        for (const FunctionRecord& function : functions) {
            if (function.name != name) {
                continue;
            }
            const auto owner_name = class_names.find(function.outer);
            if (owner_name != class_names.end() && owner_name->second == owner) {
                return function.address;
            }
        }
        return 0;
    };

    context.set_text              = find("SetText", "TextBlock");
    context.get_visibility        = find("GetVisibility", "Widget");
    context.set_color_and_opacity = find("SetColorAndOpacity", "TextBlock");
    context.set_position          = find("SetPosition", "CanvasPanelSlot");
    context.set_size              = find("SetSize", "CanvasPanelSlot");
    context.add_to_canvas         = find("AddChildToCanvas", "CanvasPanel");
    context.add_child             = find("AddChild", "PanelWidget");
    context.set_horizontal_alignment = find("SetHorizontalAlignment", "OverlaySlot");
    context.set_vertical_alignment   = find("SetVerticalAlignment", "OverlaySlot");
    context.remove_from_parent    = find("RemoveFromParent", "Widget");
    context.set_visibility        = find("SetVisibility", "Widget");
    context.get_children_count    = find("GetChildrenCount", "PanelWidget");
    context.get_child_at          = find("GetChildAt", "PanelWidget");
    context.get_desired_size      = find("GetDesiredSize", "Widget");
    context.set_stretch           = find("SetStretch", "ScaleBox");
    context.set_keyboard_focus    = find("SetKeyboardFocus", "Widget");
    context.get_player_controller = find("GetPlayerController", "GameplayStatics");
    context.set_input_mode_ui  = find("SetInputMode_UIOnlyEx", "WidgetBlueprintLibrary");
    context.create_widget      = find("Create", "WidgetBlueprintLibrary");
    context.add_to_viewport    = find("AddToViewport", "UserWidget");
    context.get_viewport_size  = find("GetViewportSize", "WidgetLayoutLibrary");
    context.get_editable_text  = find("GetText", "EditableText");
    // Needed to put text back into the field after the screen exists. Writing the property
    // only works while the widget is being created, the same as every other live change.
    context.set_editable_text  = find("SetText", "EditableText");
    context.text_to_string     = find("Conv_TextToString", "KismetTextLibrary");

    // The font the game's own text is set in, chosen by which one most of it uses.
    //
    // Deciding by ownership was tried first and found nothing at all: it walked from a text
    // block to its outer's outer expecting the user widget, and that chain does not hold
    // here, so all forty nine candidates were rejected and the lobby stayed in Roboto.
    //
    // Counting needs no assumption about how widgets are parented. A frontend has dozens of
    // text blocks and they are nearly all set in its UI font, while the handful the engine
    // itself creates are not, so the most used font asset is the game's by a wide margin.
    //   TextBlock +0x1D0 Font, FSlateFontInfo +0x00 FontObject
    std::unordered_map<std::uintptr_t, int>            font_uses;
    std::unordered_map<std::uintptr_t, std::uintptr_t> font_example;
    for (const ObjectInfo& block : text_blocks) {
        std::uintptr_t font_object = 0;
        if (!memory::GuardedRead(block.address + 0x1D0, &font_object, sizeof(font_object)) ||
            font_object == 0) {
            continue;
        }
        ++font_uses[font_object];
        font_example.try_emplace(font_object, block.address);
    }

    std::uintptr_t best_font = 0;
    int            best_uses = 0;
    for (const auto& [font_object, uses] : font_uses) {
        if (uses > best_uses) {
            best_uses = uses;
            best_font = font_object;
        }
    }
    if (best_font != 0 &&
        memory::GuardedRead(font_example[best_font] + 0x1D0, context.font_template.data(),
                            context.font_template.size())) {
        context.has_font = true;
        MPE_LOG_INFO("font asset 0x{:X} taken from text block 0x{:X}; {} of {} block(s) use "
                    "it, out of {} distinct font(s)",
                    best_font, font_example[best_font], best_uses, text_blocks.size(),
                    font_uses.size());
    } else {
        MPE_LOG_WARN("no text block carried a font; the lobby will use the engine default "
                    "typeface ({} block(s) considered)", text_blocks.size());
    }
    (void)game_widget_instances;

    std::sort(game_widgets.begin(), game_widgets.end());
    MPE_LOG_INFO("the game ships {} widget class(es):", game_widgets.size());
    for (const std::string& widget : game_widgets) {
        MPE_LOG_INFO("  {}", widget);
    }

    if (!context.StaticsComplete()) {
        return Result::Fail(
            ErrorCode::InvalidState,
            std::format("lobby UI incomplete: spawn={} statics={} canvasAdd={} addChild={} "
                        "pos={} size={} conv={} textlib={} canvasClass={} textClass={} "
                        "borderClass={} sizeboxClass={} scaleboxClass={} stretch={} "
                        "create={} addToViewport={} hostClass={} buttonClass={} "
                        "widgetLib={}",
                        context.spawn_object != 0, context.gameplay_statics != 0,
                        context.add_to_canvas != 0, context.add_child != 0,
                        context.set_position != 0, context.set_size != 0,
                        context.convert_function != 0, context.text_library != 0,
                        context.canvas_class != 0, context.text_class != 0,
                        context.border_class != 0, context.sizebox_class != 0,
                        context.scalebox_class != 0, context.set_stretch != 0,
                        context.create_widget != 0, context.add_to_viewport != 0,
                        context.host_class != 0, context.button_class != 0,
                        context.widget_library != 0));
    }

    out_context = context;
    return Result::Success();
}

Result BindLobbyMenu(std::uintptr_t menu, LobbyUIContext& context) {
    if (menu == 0) {
        return Result::Fail(ErrorCode::InvalidState, "no live main menu to attach to");
    }

    // The lobby is parented into the main menu's own root, which means the engine owns its
    // lifetime, scaling and draw order rather than this code having to.
    //
    //   UserWidget +0x268 WidgetTree, WidgetTree +0x30 RootWidget
    std::uintptr_t tree = 0;
    if (!memory::GuardedRead(menu + 0x268, &tree, sizeof(tree)) || tree == 0) {
        return Result::Fail(ErrorCode::InvalidState, "the menu has no widget tree");
    }
    std::uintptr_t root = 0;
    if (!memory::GuardedRead(tree + 0x30, &root, sizeof(root)) || root == 0) {
        return Result::Fail(ErrorCode::InvalidState, "the widget tree has no root");
    }

    context.outer       = menu;
    context.root_canvas = root;
    return Result::Success();
}

Result ResolveLobbyUI(const ObjectArray& objects, LobbyUIContext& out_context) {
    LobbyUIContext context;
    if (const Result statics = ResolveLobbyStatics(objects, context); !statics.ok()) {
        return statics;
    }

    std::uintptr_t menu = 0;
    objects.ForEach([&](const ObjectInfo& object) {
        if (object.name.rfind("Default__", 0) == 0 || object.class_name != "WBP_MainMenu_C") {
            return true;
        }
        menu = object.address;
        return false;
    });
    if (const Result bound = BindLobbyMenu(menu, context); !bound.ok()) {
        return bound;
    }

    // Naming the root makes a refusal explainable rather than a guess about its type. Only
    // done on this path, which is not the one a click takes.
    objects.ForEach([&](const ObjectInfo& object) {
        if (object.address != context.root_canvas) {
            return true;
        }
        context.root_class = object.class_name;
        return false;
    });
    MPE_LOG_INFO("menu root is 0x{:X} ({})", context.root_canvas,
                context.root_class.empty() ? "unknown class" : context.root_class);

    out_context = context;
    return Result::Success();
}

Result BuildLobbyUI(const LobbyUIContext& context, const LobbyView& view,
                    std::uintptr_t& out_root, std::vector<LobbyControl>& out_controls) {
    out_controls.clear();
    if (!context.Complete()) {
        return Result::Fail(ErrorCode::InvalidState, "the lobby UI context is incomplete");
    }

    const Builder builder(context);

    // Replace rather than stack. Clicking the entry twice previously built a second lobby
    // on top of the first, which then had to be closed twice.
    RemoveLobbyUI(context);

    // Hosted in a user widget and added to the viewport.
    //
    // Parenting into the menu's own widget tree built the whole screen correctly, reported
    // success, and drew nothing. AddToViewport is the path already proven to put a widget
    // on screen in this game, so the lobby is shown the way the engine shows any screen.
    const std::uintptr_t root =
        CreateHostedCanvasAt(context, 1000, g_open_host_widget, g_open_lobby_widget);
    if (root == 0) {
        return Result::Fail(ErrorCode::InvalidState,
                            "could not host the lobby canvas in a viewport widget");
    }

    // Backdrop and frame.
    (void)builder.Panel(root, 0.0F, 0.0F, kDesignWidth, kDesignHeight, {0, 0, 0, 0.72F});
    (void)builder.Backer(root, 40.0F, 140.0F, 1840.0F, 795.0F, kPanel);

    // Title.
    (void)builder.Backer(root, 620.0F, 40.0F, 680.0F, 80.0F, kPanelLight);
    (void)builder.Label(root, 640.0F, 36.0F, 640.0F, 88.0F, "MULTIPLAYER LOBBY");

    // The status panel is not built here.
    //
    // It reports on the mod, not on this screen, so it lives in its own overlay that is
    // added to the viewport above the lobby and is never hidden. That is what puts it on
    // the main menu as well, and it is why hiding the lobby no longer takes it away.

    // The notice panel is built with the status overlay, for the same reason.

    // Tabs, as real buttons so they can be pressed rather than only looked at.
    out_controls.push_back({builder.Button(root, 520.0F, 122.0F, 440.0F, 66.0F, "HOST"),
                            LobbyAction::ShowHost});
    out_controls.push_back({builder.Button(root, 968.0F, 122.0F, 440.0F, 66.0F, "BROWSE"),
                            LobbyAction::ShowBrowse});

    // Both tabs are built, each on its own canvas covering the same area, and switching
    // between them sets one visible and the other collapsed. Rebuilding to change tab meant
    // creating the whole screen again, which was slow enough to see and briefly restored
    // the menu underneath along with its sounds.
    g_host_tab = builder.Canvas(root, 0.0F, 0.0F, kDesignWidth, kDesignHeight);
    if (g_host_tab != 0) {
        DrawHostTab(builder, g_host_tab, view, out_controls);
    }
    g_browse_tab = builder.Canvas(root, 0.0F, 0.0F, kDesignWidth, kDesignHeight);
    if (g_browse_tab != 0) {
        DrawBrowseTab(builder, g_browse_tab, view, out_controls);
    }

    // Last, so it is on top of both tabs: a canvas draws its children in the order they
    // were added, and a modal that the lobby can be clicked through is not a modal.
    g_invite_panel = builder.Canvas(root, 0.0F, 0.0F, kDesignWidth, kDesignHeight);
    if (g_invite_panel != 0) {
        DrawInvitePanel(builder, g_invite_panel, out_controls);
    }
    ShowInvitePanel(context, false);

    SetLobbyTab(context, view.browsing);
    SetLobbyMode(context, view.mode == "SLAYER");
    {
        int chosen = 0;
        for (std::size_t index = 0; index < std::size(kLobbyMaps); ++index) {
            if (view.map == kLobbyMaps[index].scenario) {
                chosen = static_cast<int>(index);
            }
        }
        SetLobbyMap(context, chosen);
    }

    out_root          = root;
    g_open_lobby_root = root;
    MPE_LOG_INFO("lobby UI built: host tab 0x{:X}, browse tab 0x{:X}, {} control(s)",
                g_host_tab, g_browse_tab, out_controls.size());
    return Result::Success();
}

void ShowLobbyUI(const LobbyUIContext& context, bool visible) {
    if (g_open_host_widget == 0) {
        return;
    }
    SetWidgetVisibility(context, g_open_host_widget, visible ? kVisible : kCollapsed);
    if (visible) {
        FoldMenuAway(context);
        FocusLobby(context, g_open_host_widget);

        // The pump has to move with the screen.
        //
        // Queued game thread work runs from a widget's own event path, and the widget it
        // was installed on is the main menu, which this has just collapsed. A collapsed
        // widget receives no events, so the pump stopped firing the moment the lobby
        // opened and every job fell back to the slow path: that is the delay on changing a
        // filter, a mode or a map. The lobby is what is alive now, so the lobby carries it.
        if (const Result pump = InstallGameThreadPump(g_open_host_widget); !pump.ok()) {
            MPE_LOG_WARN("the lobby could not take over the game thread pump: {}",
                        pump.message());
        }
        return;
    }

    // A modal does not survive the screen it was opened over.
    ShowInvitePanel(context, false);

    // Handed back before the lobby goes away, for the same reason in reverse.
    if (context.outer != 0) {
        (void)InstallGameThreadPump(context.outer);
    }

    UnfoldMenu(context);

    // Input has to go back with the menu, and it has to go back to the viewport.
    //
    // Opening the lobby points the player controller's UI focus at the lobby widget. Simply
    // hiding that widget leaves the focus pointing at something collapsed, so the menu comes
    // back on screen and answers nothing.
    //
    // Handing focus to the menu widget instead was no better, and this is the part that
    // took a while to see. The menu is a UserWidget, and a UserWidget's own root is not
    // focusable: the buttons under it are. Naming it as the focus target of a UI-only
    // input mode therefore parks focus on something that accepts nothing, and since a
    // UI-only mode routes everything through focus, the whole frontend stops answering
    // and the game has to be closed from the desktop.
    //
    // Naming no widget at all is what the engine wants here. A UI-only input mode with no
    // focus target focuses the game viewport, which is the state the menu was in before
    // the lobby ever opened, and every widget under it hit tests normally again.
    FocusLobby(context, kFocusTheViewport);
}

void ShowInvitePanel(const LobbyUIContext& context, bool visible) {
    if (g_invite_panel == 0) {
        return;
    }
    // SelfHitTestInvisible on the canvas, not Visible: the canvas covers the whole design
    // and would otherwise be the thing every click lands on. Its children, including the
    // dimming panel behind the card, still hit test normally.
    SetWidgetVisibility(context, g_invite_panel,
                        visible ? kSelfHitTestInvisible : kCollapsedValue);
    g_invite_open = visible;
}

bool InvitePanelIsOpen() {
    return g_invite_open;
}

void SetLobbyFriends(const LobbyUIContext& context, const std::vector<LobbyFriend>& friends,
                     int page) {
    const Builder builder(context);

    const int total = static_cast<int>(friends.size());
    const int pages = (total + kFriendRows - 1) / kFriendRows;
    const int shown = (page < 0) ? 0 : page;
    const int first = shown * kFriendRows;

    for (int index = 0; index < kFriendRows; ++index) {
        FriendRowWidgets& row  = g_friend_row[index];
        const int         from = first + index;
        if (from >= total) {
            builder.SetVisibilityOf(row.button, kCollapsedValue);
            builder.SetVisibilityOf(row.highlight, kCollapsedValue);
            builder.SetTextLive(row.name, "");
            builder.SetTextLive(row.status, "");
            continue;
        }

        const LobbyFriend& entry = friends[static_cast<std::size_t>(from)];
        builder.SetVisibilityOf(row.button, kVisibleValue);
        builder.SetVisibilityOf(row.highlight, entry.invited ? kHitTestInvisible
                                                             : kCollapsedValue);
        builder.SetTextLive(row.name, entry.name);
        if (entry.invited) {
            builder.SetTextLive(row.status, "INVITED");
            builder.SetColourLive(row.status, kAccent);
        } else if (entry.in_game) {
            builder.SetTextLive(row.status, "IN GAME");
            builder.SetColourLive(row.status, kGood);
        } else {
            builder.SetTextLive(row.status, "ONLINE");
            builder.SetColourLive(row.status, kTextDim);
        }
    }

    builder.SetVisibilityOf(g_friend_empty, total == 0 ? kHitTestInvisible : kCollapsedValue);
    builder.SetTextLive(g_friend_page,
                        pages > 1 ? std::format("PAGE {} OF {}", shown + 1, pages)
                                  : std::string{});
}

void SetLobbyRoster(const LobbyUIContext& context, const std::vector<std::string>& blue,
                    const std::vector<std::string>& red, const std::string& host_name) {
    const Builder                     builder(context);
    const std::vector<std::string>*   sides[2] = {&blue, &red};

    for (std::size_t side = 0; side < 2; ++side) {
        const std::vector<std::string>& players = *sides[side];
        for (int slot = 0; slot < kTeamSlots; ++slot) {
            SlotWidgets& card = g_slot[side][slot];
            const bool   occupied = slot < static_cast<int>(players.size());

            // The button stays hit testable only while the slot is empty, so a card with
            // somebody in it cannot be pressed to invite somebody else into it.
            builder.SetVisibilityOf(card.button, occupied ? kCollapsedValue : kVisibleValue);
            builder.SetVisibilityOf(card.plus, occupied ? kCollapsedValue : kHitTestInvisible);
            builder.SetVisibilityOf(card.strip, occupied ? kHitTestInvisible : kCollapsedValue);
            builder.SetVisibilityOf(card.name, occupied ? kHitTestInvisible : kCollapsedValue);
            builder.SetVisibilityOf(card.role, occupied ? kHitTestInvisible : kCollapsedValue);
            builder.SetBorderColour(card.backing, occupied ? kAccentDim : kSlot);

            if (!occupied) {
                continue;
            }
            const std::string& name = players[static_cast<std::size_t>(slot)];
            builder.SetTextLive(card.name, name);
            builder.SetTextLive(card.role, name == host_name ? "Owner" : "Player");
        }
        builder.SetTextLive(g_team_heading[side],
                            std::format("{}/{}", players.size(), kTeamSlots));
    }
}

void SetLobbyTab(const LobbyUIContext& context, bool browsing) {
    SetWidgetVisibility(context, g_host_tab,
                        browsing ? kCollapsed : kSelfHitTestInvisible);
    SetWidgetVisibility(context, g_browse_tab,
                        browsing ? kSelfHitTestInvisible : kCollapsed);
    MPE_LOG_INFO("lobby tab is now {} (host 0x{:X}, browse 0x{:X}, setter 0x{:X})",
                browsing ? "BROWSE" : "HOST", g_host_tab, g_browse_tab,
                context.set_visibility);
}

void SetLobbyMode(const LobbyUIContext& context, bool slayer) {
    SetWidgetVisibility(context, g_mode_marker[0], slayer ? kCollapsed : kVisible);
    SetWidgetVisibility(context, g_mode_marker[1], slayer ? kVisible : kCollapsed);
}

void SetLobbyMap(const LobbyUIContext& context, int map_index) {
    for (int index = 0; index < 4; ++index) {
        SetWidgetVisibility(context, g_map_marker[index],
                            index == map_index ? kVisible : kCollapsed);
    }
}

void SetLobbyServers(const LobbyUIContext& context, const std::vector<ServerEntry>& servers,
                     int selected) {
    const Builder builder(context);
    for (std::size_t index = 0; index < kServerRows; ++index) {
        ServerRowWidgets& row  = g_server_row[index];
        const bool        used = index < servers.size();

        builder.SetVisibilityOf(row.button, used ? kVisibleValue : kCollapsedValue);
        for (const std::uintptr_t block : {row.name, row.mode, row.map, row.players,
                                           row.ping, row.status}) {
            builder.SetVisibilityOf(block, used ? kHitTestInvisible : kCollapsedValue);
        }
        builder.SetVisibilityOf(
            row.highlight,
            used && static_cast<int>(index) == selected ? kSelfHitTestInvisibleValue
                                                        : kCollapsedValue);
        if (!used) {
            continue;
        }

        const ServerEntry& entry = servers[index];
        builder.SetTextLive(row.name, entry.name);
        builder.SetTextLive(row.mode, entry.mode);
        builder.SetTextLive(row.map, entry.map);
        builder.SetTextLive(row.players,
                            std::format("{}/{}", entry.players, entry.capacity));

        // The same thresholds and the same colours as the status panel, so a number means
        // one thing wherever a player reads it.
        builder.SetTextLive(row.ping, std::format("{}ms", entry.ping));
        builder.SetColourLive(row.ping, PingColour(entry.ping));

        builder.SetTextLive(row.status, entry.status);
        builder.SetColourLive(row.status, entry.status == "IN GAME" ? kWarn : kGood);
    }

    builder.SetVisibilityOf(g_empty_notice,
                            servers.empty() ? kVisibleValue : kCollapsedValue);

    // The details panel follows the selection, and is blanked rather than left showing a
    // server that is no longer in the list.
    const bool have_selection =
        !servers.empty() && selected >= 0 && selected < static_cast<int>(servers.size());
    const std::array<std::string, 5> lines =
        have_selection
            ? std::array<std::string, 5>{
                  std::format("Server: {}", servers[static_cast<std::size_t>(selected)].name),
                  std::format("Mode: {}", servers[static_cast<std::size_t>(selected)].mode),
                  std::format("Map: {}", servers[static_cast<std::size_t>(selected)].map),
                  std::format("Players: {}/{}",
                              servers[static_cast<std::size_t>(selected)].players,
                              servers[static_cast<std::size_t>(selected)].capacity),
                  std::format("Ping: {}ms",
                              servers[static_cast<std::size_t>(selected)].ping)}
            : std::array<std::string, 5>{"No server selected", "", "", "", ""};
    for (std::size_t line = 0; line < lines.size(); ++line) {
        builder.SetTextLive(g_detail_line[line], lines[line]);
        builder.SetVisibilityOf(g_detail_line[line], kHitTestInvisible);
    }
}

bool StatusOverlayIsBuilt() {
    return g_status_host != 0 && g_status_root != 0;
}

Result BuildStatusOverlay(const LobbyUIContext& context) {
    if (StatusOverlayIsBuilt()) {
        return Result::Success();
    }
    if (!context.Complete()) {
        return Result::Fail(ErrorCode::InvalidState, "the lobby UI context is incomplete");
    }

    const Builder builder(context);

    // Above the lobby, and never hidden.
    //
    // The lobby sits at z order 1000 and is collapsed whenever it is closed. The status
    // panel describes the mod rather than that screen, so it gets its own host one step
    // higher: it draws over the lobby when the lobby is open and stays on the main menu
    // when it is not.
    const std::uintptr_t root =
        CreateHostedCanvasAt(context, 1001, g_status_host, g_status_scaler);
    if (root == 0) {
        return Result::Fail(ErrorCode::InvalidState, "could not host the status overlay");
    }

    // The canvas covers the design and must not eat clicks meant for what is underneath.
    SetWidgetVisibility(context, g_status_host, kSelfHitTestInvisible);

    // Wider and quieter than it was.
    //
    // The old box was 336 points wide with 19 point text, which a session line like
    // "CONNECTING TO HOST  STALLED" overran, running off the right of the screen. It
    // reaches further left now and the text is smaller, so a long line has somewhere to go
    // and the panel reads as an instrument rather than a headline.
    constexpr float kPanelX = 1396.0F;
    constexpr float kPanelW = 484.0F;
    constexpr float kPanelH = 186.0F;
    constexpr float kTextX  = 1414.0F;
    constexpr float kTextW  = 452.0F;

    const std::uintptr_t status_panel =
        builder.Panel(root, kPanelX, 20.0F, kPanelW, kPanelH, kStatusPanel);
    // A rule down the left edge, because a translucent panel over a starfield has no edge
    // of its own to read against and simply disappears.
    const std::uintptr_t status_rule =
        builder.Panel(root, kPanelX, 20.0F, 3.0F, kPanelH, kAccent);

    // The mod's own name, so a player who has forgotten what put this here can tell.
    const std::uintptr_t status_title = builder.Text(
        root, kTextX, 28.0F, kTextW, 26.0F, "MULTIPLAYER EVOLVED", kAccent, 18.0F);
    const std::uintptr_t status_divider =
        builder.Panel(root, kTextX, 56.0F, kTextW - 8.0F, 2.0F, kAccentDim);

    for (std::size_t line = 0; line < kStatusLines; ++line) {
        g_status_line[line] =
            builder.Text(root, kTextX, 68.0F + static_cast<float>(line) * 24.0F, kTextW,
                         22.0F, "", kText, 15.0F);
    }

    // The notice panel, opposite the status panel and part of the same overlay, so a
    // staged update or a failed session is readable from the main menu too. Built
    // collapsed; SetLobbyStatus decides whether there is anything to say.
    g_notice_panel = builder.Panel(root, 44.0F, 20.0F, 560.0F, 122.0F, kStatusPanel);
    g_notice_rule  = builder.Panel(root, 44.0F, 20.0F, 3.0F, 122.0F, kWarn);
    g_notice_title = builder.Text(root, 62.0F, 30.0F, 530.0F, 32.0F, "", kWarn, 19.0F);
    for (std::size_t line = 0; line < std::size(g_notice_detail); ++line) {
        g_notice_detail[line] =
            builder.Text(root, 62.0F, 66.0F + static_cast<float>(line) * 26.0F, 530.0F, 26.0F,
                         "", kText, 16.0F);
    }
    for (const std::uintptr_t widget : {g_notice_panel, g_notice_rule, g_notice_title,
                                        g_notice_detail[0], g_notice_detail[1]}) {
        builder.SetVisibilityOf(widget, kCollapsedValue);
    }

    // Nothing in this overlay is clickable, so nothing in it may absorb a click.
    //
    // A Border is Visible by default, which means it hit tests itself. Two panels sitting
    // in the top corners of a full screen canvas would quietly eat every press that landed
    // on them, and this overlay is above the lobby, so it would be eating the lobby's.
    // That exact mistake has killed the main menu twice.
    for (const std::uintptr_t widget : {status_panel, status_rule, status_title,
                                        status_divider}) {
        builder.SetVisibilityOf(widget, kHitTestInvisible);
    }

    g_status_root = root;
    MPE_LOG_INFO("status overlay built at 0x{:X}; it stays on screen with the menu as well "
                "as the lobby",
                g_status_host);
    return Result::Success();
}

void SetLobbyStatus(const LobbyUIContext& context, const LobbyStatus& status) {
    const Builder builder(context);

    static bool s_reported = false;
    if (!s_reported) {
        s_reported = true;
        MPE_LOG_INFO("status panel first write: net={}, session '{}', '{}'", status.online,
                    status.session, status.version);
    }

    // One line per fact, in the order a player asks the questions: am I online, is there a
    // session, who is running it, how far away are they, is this build current.
    struct Line {
        std::string  text;
        LinearColour colour;
    };
    std::array<Line, kStatusLines> lines{};

    lines[0] = {status.online ? "NET: ONLINE" : "NET: OFFLINE", status.online ? kGood : kBad};
    lines[1] = {std::format("SESSION: {}", status.session),
                status.invitable ? kGood : kText};

    // What is being played, when there is anything to play. Empty rather than a placeholder
    // when the session has not settled on one, because a map name that is not the map is
    // worse than no map name.
    if (!status.mode.empty()) {
        lines[2] = {status.map.empty() ? status.mode
                                       : std::format("{}  {}", status.mode, status.map),
                    kTextDim};
    }

    // The host's name when this machine is not the host, and the ping to them. Neither is
    // shown alone in an empty lobby: there is no round trip to a session of one, and a
    // number invented for that case is one a player could act on wrongly.
    if (status.ping_ms >= 0) {
        lines[3] = {status.host_name.empty()
                        ? std::format("PING: {} ms", status.ping_ms)
                        : std::format("{}  {} ms", status.host_name, status.ping_ms),
                    PingColour(status.ping_ms)};
    } else if (!status.host_name.empty()) {
        lines[3] = {status.host_name, kTextDim};
    }

    lines[4] = {status.version, status.update_available ? kWarn : kTextDim};

    for (std::size_t index = 0; index < kStatusLines; ++index) {
        builder.SetTextLive(g_status_line[index], lines[index].text);
        builder.SetColourLive(g_status_line[index], lines[index].colour);
        // An empty line is collapsed rather than left blank, so the panel is only as tall
        // as it has something to say.
        builder.SetVisibilityOf(g_status_line[index],
                                lines[index].text.empty() ? kCollapsedValue
                                                          : kHitTestInvisible);
    }

    // The notice panel. Shown only when there is something worth a whole sentence.
    const bool notice = !status.notice_title.empty();
    if (notice) {
        builder.SetTextLive(g_notice_title, status.notice_title);

        // Wrapped here rather than by the text block.
        //
        // A block given more text than fits simply stops drawing at the edge, which is
        // how "ERROR: the host did" reached a player as the entire explanation of a
        // failed join. Breaking on spaces at a width the panel can hold means a long
        // message loses nothing.
        constexpr std::size_t kPerLine = 60;
        std::string_view      rest     = status.notice_detail;
        for (std::size_t line = 0; line < std::size(g_notice_detail); ++line) {
            std::string_view take = rest.substr(0, std::min(rest.size(), kPerLine));
            if (take.size() == kPerLine && rest.size() > kPerLine) {
                if (const std::size_t space = take.find_last_of(' ');
                    space != std::string_view::npos) {
                    take = take.substr(0, space);
                }
            }
            builder.SetTextLive(g_notice_detail[line], take);
            rest.remove_prefix(take.size());
            while (!rest.empty() && rest.front() == ' ') {
                rest.remove_prefix(1);
            }
        }
    }

    const std::uint8_t notice_visibility = notice ? kHitTestInvisible : kCollapsedValue;
    for (const std::uintptr_t widget : {g_notice_panel, g_notice_rule, g_notice_title,
                                        g_notice_detail[0], g_notice_detail[1]}) {
        builder.SetVisibilityOf(widget, notice_visibility);
    }
}

void SetLobbyFilters(const LobbyUIContext& context, const ServerFilter& filter) {
    const Builder builder(context);

    const int mode_choice = filter.mode.empty() ? 0 : (filter.mode == "SLAYER" ? 2 : 1);
    const int ping_choice = filter.max_ping == 0 ? 0 : (filter.max_ping <= 50 ? 1 : 2);
    const std::array<std::pair<std::uintptr_t*, int>, 3> groups = {
        std::pair{g_filter_mode, mode_choice},
        std::pair{g_filter_slots, filter.slots},
        std::pair{g_filter_ping, ping_choice},
    };
    for (const auto& [markers, choice] : groups) {
        for (int option = 0; option < 3; ++option) {
            builder.SetVisibilityOf(markers[option], option == choice
                                                         ? kSelfHitTestInvisibleValue
                                                         : kCollapsedValue);
        }
    }
}

std::string ReadServerName(const LobbyUIContext& context) {
    if (g_server_name_field == 0 || context.get_editable_text == 0 ||
        context.text_to_string == 0 || context.text_library == 0) {
        return {};
    }

    struct TextParameters {
        std::uint8_t text[0x10];
    };
    TextParameters current{};
    if (!CallFunction(g_server_name_field, context.get_editable_text, &current).ok()) {
        return {};
    }

    // An FText owns shared string data, so it is converted rather than read: the engine
    // hands back an FString whose buffer can then be walked.
    struct StringParameters {
        std::uint8_t text[0x10];
        struct {
            wchar_t*     data;
            std::int32_t count;
            std::int32_t capacity;
        } result;
    };
    StringParameters converted{};
    std::memcpy(converted.text, current.text, sizeof(converted.text));
    if (!CallFunction(context.text_library, context.text_to_string, &converted).ok() ||
        converted.result.data == nullptr || converted.result.count <= 0) {
        return {};
    }

    std::string name;
    name.reserve(static_cast<std::size_t>(converted.result.count));
    for (std::int32_t index = 0; index < converted.result.count; ++index) {
        wchar_t character = 0;
        if (!memory::GuardedRead(reinterpret_cast<std::uintptr_t>(converted.result.data) +
                                     static_cast<std::uintptr_t>(index) * sizeof(wchar_t),
                                 &character, sizeof(character)) ||
            character == L'\0') {
            break;
        }
        name.push_back(static_cast<char>(character));
    }

    // Cut here, and put the cut version back in the box.
    //
    // Applying the limit only where the name is used would let somebody type a hundred
    // characters, see all of them, and then find the game advertised under something
    // shorter that they never chose. Correcting the field is the only version of this that
    // is honest about what the name actually is.
    if (name.size() > kMaxServerNameLength) {
        name.resize(kMaxServerNameLength);
        WriteServerName(context, name);
    }
    return name;
}

void WriteServerName(const LobbyUIContext& context, std::string_view name) {
    if (g_server_name_field == 0) {
        return;
    }
    const Builder builder(context);
    if (context.set_editable_text == 0) {
        // No setter resolved: the property write still works before the field has been
        // built, which covers restoring a saved name at build time.
        builder.SetFieldText(g_server_name_field, name);
        return;
    }
    builder.SetTextLiveOn(g_server_name_field, context.set_editable_text, name);
}

bool LobbyIsBuilt() { return g_open_host_widget != 0 && g_open_lobby_root != 0; }

} // namespace mpe::unreal
