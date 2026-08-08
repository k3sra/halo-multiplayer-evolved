// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Unreal/Reflection.cpp
#define MPE_LOG_CATEGORY "Unreal.Reflect"

#include "Unreal/Reflection.h"

#include "Core/Log.h"
#include "Core/Text.h"
#include "Unreal/ProcessMemory.h"

#include <algorithm>
#include <format>
#include <limits>
#include <unordered_set>

namespace mpe::unreal {
namespace {

/// True when text looks like a UE field or type name.
///
/// "None" is rejected on purpose, and that single line is why this file was rewritten.
/// FName index 0 is "None", and zero is also what reading uninitialised or unrelated
/// memory as an FName produces. Accepting it let the chain walker march happily through
/// whatever happened to sit at the guessed offset, which is exactly how a class dump came
/// back as a column of "var None : at +0x0" while every other check reported success.
[[nodiscard]] bool IsPlausibleFieldName(std::string_view text) noexcept {
    if (text.size() < 2 || text.size() > 128) {
        return false;
    }
    if (text == "None") {
        return false;
    }
    for (const char c : text) {
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                             (c >= '0' && c <= '9') || c == '_';
        if (!allowed) {
            return false;
        }
    }
    // A name that is all digits is a number, not an identifier.
    return !std::all_of(text.begin(), text.end(),
                        [](char c) { return c >= '0' && c <= '9'; });
}

/// True when a name is one of UE's property classes.
///
/// Every FProperty subclass is named <Something>Property, without exception, so this is a
/// rule rather than a list that a new engine version invalidates. It is the strongest
/// signal available that a walked chain really is a property chain: garbage memory
/// produces resolvable identifiers now and then, but not ones ending in "Property".
[[nodiscard]] bool IsPropertyClassName(std::string_view text) noexcept {
    return text.size() > 8 && text.ends_with("Property");
}

/// Size in bytes of one element of a property type whose size the language fixes.
///
/// Returns zero for types whose size depends on what they contain, which are simply not
/// used as evidence. Three agreeing samples from this table identify ElementSize's slot
/// outright, where "the int32 before Offset_Internal" only ever assumed it.
[[nodiscard]] std::int32_t KnownElementSize(std::string_view type) noexcept {
    struct Entry {
        std::string_view name;
        std::int32_t     size;
    };
    static constexpr Entry kSizes[] = {
        {"BoolProperty", 1},        {"ByteProperty", 1},       {"Int8Property", 1},
        {"Int16Property", 2},       {"UInt16Property", 2},     {"IntProperty", 4},
        {"UInt32Property", 4},      {"FloatProperty", 4},      {"Int64Property", 8},
        {"UInt64Property", 8},      {"DoubleProperty", 8},     {"NameProperty", 8},
        {"ObjectProperty", 8},      {"ObjectPtrProperty", 8},  {"ClassProperty", 8},
        {"ClassPtrProperty", 8},    {"WeakObjectProperty", 8}, {"StrProperty", 16},
        {"ArrayProperty", 16},      {"InterfaceProperty", 16},
    };
    for (const Entry& entry : kSizes) {
        if (entry.name == type) {
            return entry.size;
        }
    }
    return 0;
}

/// Candidate offsets searched during detection. Pointer aligned, covering the range
/// any plausible UStruct or FField layout occupies.
constexpr std::size_t kMinCandidate = 0x18;
constexpr std::size_t kMaxCandidate = 0xA0;

/// sizeof(UObject) and sizeof(UField) on any 64 bit build. See LayoutAnchors.
constexpr std::int32_t kSizeOfUObject = 0x28;
constexpr std::int32_t kSizeOfUField  = 0x30;

/// UObject::ClassPrivate, already proven correct by the object array walker.
constexpr std::size_t kObjectClassPrivateOffset = 0x10;

} // namespace

std::string ReflectionLayout::Describe() const {
    return std::format(
        "child_properties=+0x{:X} field_name=+0x{:X} field_next=+0x{:X} field_class=+0x{:X} "
        "offset_internal=+0x{:X} element_size=+0x{:X} array_dim=+0x{:X} flags=+0x{:X} "
        "rep_index=+0x{:X} rep_notify=+0x{:X} inner=+0x{:X} properties_size=+0x{:X} "
        "super=+0x{:X} children=+0x{:X} field_object_next=+0x{:X} detected={} anchored={} "
        "chain={}",
        child_properties_offset, field_name_offset, field_next_offset, field_class_offset,
        offset_internal_offset, element_size_offset, array_dim_offset, property_flags_offset,
        rep_index_offset, rep_notify_offset, struct_property_inner_offset,
        properties_size_offset, super_struct_offset, children_offset,
        field_object_next_offset, detected, anchored, detected_chain_length);
}

std::string Reflection::ResolveNameAt(std::uintptr_t address) const {
    if (names_ == nullptr) {
        return {};
    }
    // FName is { uint32 ComparisonIndex; uint32 Number; }. Only the index identifies
    // the text.
    const auto index = memory::Read<std::uint32_t>(address);
    if (!index.has_value()) {
        return {};
    }
    const Expected<std::string> text = names_->Resolve(*index);
    return text.ok() ? text.value() : std::string{};
}

std::vector<std::uintptr_t> Reflection::WalkChain(std::uintptr_t head, std::size_t name_offset,
                                                 std::size_t next_offset,
                                                 std::size_t max_nodes) const {
    std::vector<std::uintptr_t> nodes;
    std::unordered_set<std::uintptr_t> visited;
    std::uintptr_t current = head;

    while (memory::IsPlausiblePointer(current) && nodes.size() < max_nodes) {
        if (!visited.insert(current).second) {
            break; // Cycle.
        }
        if (!memory::IsReadable(current, std::max(name_offset, next_offset) + 8)) {
            break;
        }
        const std::string name = ResolveNameAt(current + name_offset);
        if (!IsPlausibleFieldName(name)) {
            break;
        }
        nodes.push_back(current);

        const auto next = memory::ReadPointer(current + next_offset);
        if (!next.has_value() || *next == 0) {
            break;
        }
        current = *next;
    }
    return nodes;
}

ReflectionLayout Reflection::DetectLayout(const std::vector<std::uintptr_t>& candidate_structs,
                                          const LayoutAnchors& anchors) const {
    ReflectionLayout best{};

    // The class name of a UObject, for identifying UFunction nodes without the object
    // array. ClassPrivate then NamePrivate, both already proven on this build.
    const auto class_name_of = [this](std::uintptr_t object) -> std::string {
        const auto klass = memory::ReadPointer(object + kObjectClassPrivateOffset);
        if (!klass.has_value() || !memory::IsPlausiblePointer(*klass)) {
            return {};
        }
        return ResolveNameAt(*klass + kObjectNamePrivateOffset);
    };

    // ---- Phase one: the FField chain -------------------------------------------------
    //
    // Scored, not first-past-the-post. The winner is the reading under which the most
    // chain nodes declare themselves to be properties, because a node whose FFieldClass
    // is named "BoolProperty" is not a coincidence and a node whose name merely looks
    // like an identifier is.
    struct Chain {
        std::size_t                 child{0};
        std::size_t                 name{0};
        std::size_t                 next{0};
        std::size_t                 field_class{0};
        std::size_t                 typed{0};
        std::size_t                 score{0};
        std::vector<std::uintptr_t> nodes;
    };
    Chain winner;

    for (const std::uintptr_t struct_address : candidate_structs) {
        if (!memory::IsReadable(struct_address, kMaxCandidate + 16)) {
            continue;
        }

        for (std::size_t child = kMinCandidate; child <= kMaxCandidate; child += 8) {
            const auto head = memory::ReadPointer(struct_address + child);
            if (!head.has_value() || !memory::IsPlausiblePointer(*head)) {
                continue;
            }

            for (std::size_t name_off = 0x10; name_off <= 0x38; name_off += 8) {
                // A single node proves nothing; require the chain to continue.
                for (std::size_t next_off = 0x10; next_off <= 0x38; next_off += 8) {
                    if (next_off == name_off) {
                        continue;
                    }
                    const std::vector<std::uintptr_t> nodes =
                        WalkChain(*head, name_off, next_off, kMaxPropertiesPerStruct);
                    if (nodes.size() < 2) {
                        continue;
                    }

                    // Names must be distinct: a chain of identical names means we are
                    // reading the same field repeatedly through a self referencing
                    // pointer.
                    std::unordered_set<std::string> names;
                    for (const std::uintptr_t node : nodes) {
                        names.insert(ResolveNameAt(node + name_off));
                    }
                    if (names.size() < nodes.size()) {
                        continue;
                    }

                    for (const std::size_t class_off : {std::size_t{0x08}, std::size_t{0x10}}) {
                        std::size_t typed = 0;
                        for (const std::uintptr_t node : nodes) {
                            const auto klass = memory::ReadPointer(node + class_off);
                            if (!klass.has_value() || !memory::IsPlausiblePointer(*klass)) {
                                continue;
                            }
                            if (IsPropertyClassName(ResolveNameAt(*klass))) {
                                ++typed;
                            }
                        }
                        // A real chain types every node. Four in five allows for a node
                        // freed underneath us mid walk, and nothing looser.
                        if (typed * 5 < nodes.size() * 4) {
                            continue;
                        }
                        const std::size_t score = typed * 1000 + nodes.size();
                        if (score > winner.score) {
                            winner = Chain{child, name_off, next_off, class_off,
                                           typed, score, nodes};
                        }
                    }
                }
            }
        }
    }

    if (winner.nodes.empty()) {
        return best;
    }

    best.child_properties_offset = winner.child;
    best.field_name_offset       = winner.name;
    best.field_next_offset       = winner.next;
    best.field_class_offset      = winner.field_class;
    best.detected_chain_length   = winner.nodes.size();

    // Every property reachable under the winning reading, which is a far better sample
    // for the numeric slots than one struct's own fields.
    std::vector<std::uintptr_t> sample = winner.nodes;
    for (const std::uintptr_t struct_address : candidate_structs) {
        const auto head = memory::ReadPointer(struct_address + winner.child);
        if (!head.has_value()) {
            continue;
        }
        for (const std::uintptr_t node :
             WalkChain(*head, winner.name, winner.next, kMaxPropertiesPerStruct)) {
            sample.push_back(node);
        }
    }

    // ---- Phase two: Offset_Internal ---------------------------------------------------
    //
    // The int32 whose values run monotonically along one struct's chain. Direction is not
    // fixed: UE prepends as it registers, so a chain can be in declaration order or the
    // reverse of it depending on how the type was built.
    std::size_t offset_slot = 0;
    for (std::size_t slot = 0x28; slot <= 0x68 && offset_slot == 0; slot += 4) {
        std::vector<std::int32_t> values;
        values.reserve(winner.nodes.size());
        bool ok = true;
        for (const std::uintptr_t node : winner.nodes) {
            const auto value = memory::Read<std::int32_t>(node + slot);
            if (!value.has_value() || *value < 0 || *value > 0x20000) {
                ok = false;
                break;
            }
            values.push_back(*value);
        }
        if (!ok || values.size() < 2) {
            continue;
        }
        const std::unordered_set<std::int32_t> distinct(values.begin(), values.end());
        if (distinct.size() != values.size()) {
            continue;
        }
        const bool rising = std::is_sorted(values.begin(), values.end());
        const bool falling =
            std::is_sorted(values.rbegin(), values.rend());
        if (rising || falling) {
            offset_slot = slot;
        }
    }
    if (offset_slot == 0) {
        return best; // detected stays false: without an offset a property is unusable.
    }
    best.offset_internal_offset = offset_slot;

    // ---- Phase three: the rest of FProperty -------------------------------------------
    //
    // FProperty declares ArrayDim, ElementSize, PropertyFlags, RepIndex,
    // BlueprintReplicationCondition, Offset_Internal and RepNotifyFunc in that order, so
    // fixing one fixes all of them. Derived and then checked, never assumed.
    const auto derive = [offset_slot](std::ptrdiff_t delta) -> std::size_t {
        const auto value = static_cast<std::ptrdiff_t>(offset_slot) + delta;
        return value > 0 ? static_cast<std::size_t>(value) : 0;
    };

    // ElementSize, checked against the types whose size the language fixes.
    const auto score_element_size = [&](std::size_t slot,
                                        std::size_t& out_hits) -> bool {
        out_hits = 0;
        for (const std::uintptr_t node : sample) {
            const auto klass = memory::ReadPointer(node + winner.field_class);
            if (!klass.has_value() || !memory::IsPlausiblePointer(*klass)) {
                continue;
            }
            const std::int32_t expected = KnownElementSize(ResolveNameAt(*klass));
            if (expected == 0) {
                continue;
            }
            const auto value = memory::Read<std::int32_t>(node + slot);
            if (!value.has_value() || *value != expected) {
                return false; // One contradiction is enough to rule a slot out.
            }
            ++out_hits;
        }
        return out_hits >= 3;
    };

    std::size_t hits = 0;
    if (const std::size_t derived = derive(-0x14);
        derived != 0 && score_element_size(derived, hits)) {
        best.element_size_offset = derived;
    } else {
        std::size_t best_hits = 0;
        for (std::size_t slot = derive(-0x24); slot != 0 && slot < offset_slot; slot += 4) {
            std::size_t slot_hits = 0;
            if (score_element_size(slot, slot_hits) && slot_hits > best_hits) {
                best_hits                = slot_hits;
                best.element_size_offset = slot;
            }
        }
    }

    // ArrayDim and PropertyFlags sit either side of ElementSize, so they follow from a
    // measured ElementSize rather than from a second guess at where FField ends. That
    // distinction matters: FField's flags word is padded out to a pointer boundary, and
    // deriving both from Offset_Internal put each of them four bytes early.
    const std::size_t element_slot = best.element_size_offset;

    // ArrayDim: almost every property is a single element, none is zero or absurd.
    const auto plausible_array_dim = [&](std::size_t slot) {
        std::size_t ones = 0;
        std::size_t seen = 0;
        for (const std::uintptr_t node : sample) {
            const auto value = memory::Read<std::int32_t>(node + slot);
            if (!value.has_value() || *value < 1 || *value > 0x10000) {
                return false;
            }
            ++seen;
            ones += (*value == 1) ? 1 : 0;
        }
        return seen >= 3 && ones * 4 >= seen * 3;
    };
    if (element_slot >= 4 && plausible_array_dim(element_slot - 4)) {
        best.array_dim_offset = element_slot - 4;
    } else {
        for (std::size_t slot = derive(-0x24); slot != 0 && slot < offset_slot; slot += 4) {
            if (slot != element_slot && plausible_array_dim(slot)) {
                best.array_dim_offset = slot;
                break;
            }
        }
    }

    // PropertyFlags: a 64 bit mask, found by its signature rather than by its range.
    //
    // Two range tests failed here before this one. An upper bound of bit 48 rejected every
    // real property, because the header tool sets one of the three native access specifier
    // bits, and those live at bits 52 to 54. Raising it to bit 55 then caught
    // CPF_SkipSerialization instead. Chasing the ceiling is the wrong idea: the enum grows
    // upward with every engine release and a bound that is right today is wrong at the
    // next one.
    //
    // The access specifier itself is the signature. Every property the header tool emits
    // is public, protected or private and never two of those at once, so exactly one bit
    // set inside that three bit window identifies the mask and nothing else in an FProperty
    // imitates it.
    constexpr std::uint64_t kAccessMask    = 0x0070000000000000ull;
    constexpr std::uint64_t kAccessPublic  = 0x0010000000000000ull;
    constexpr std::uint64_t kAccessProtect = 0x0020000000000000ull;
    constexpr std::uint64_t kAccessPrivate = 0x0040000000000000ull;

    const auto score_flags = [&](std::size_t slot) -> std::size_t {
        if ((slot & 0x7u) != 0) {
            return 0;
        }
        std::size_t matching = 0;
        std::size_t seen     = 0;
        for (const std::uintptr_t node : sample) {
            const auto value = memory::Read<std::uint64_t>(node + slot);
            if (!value.has_value()) {
                continue;
            }
            ++seen;
            const std::uint64_t access = *value & kAccessMask;
            if (access == kAccessPublic || access == kAccessProtect ||
                access == kAccessPrivate) {
                ++matching;
            }
        }
        return (seen >= 3 && matching * 5 >= seen * 3) ? matching : 0;
    };

    std::size_t flags_score = 0;
    if (element_slot != 0) {
        flags_score = score_flags(element_slot + 4);
        if (flags_score > 0) {
            best.property_flags_offset = element_slot + 4;
        }
    }
    if (flags_score == 0) {
        // Aligned up, because the mask is eight byte aligned and stepping by eight from an
        // unaligned start visits nothing that could hold it.
        for (std::size_t slot = (derive(-0x20) + 7u) & ~std::size_t{7};
             slot != 0 && slot < offset_slot; slot += 8) {
            if (const std::size_t score = score_flags(slot); score > flags_score) {
                flags_score                = score;
                best.property_flags_offset = slot;
            }
        }
    }

    best.rep_index_offset = derive(-0x4);

    // Where FProperty ends, measured by what the subclasses put there.
    //
    // Deriving this from Offset_Internal was wrong on this build. UE 5.5 puts RepNotifyFunc
    // after the four in memory link pointers rather than before them, so the derived
    // sizeof was 0x2C past Offset_Internal when the real answer is 0x2C plus another
    // twenty bytes. The measurement needs no such assumption: an FStructProperty's first
    // extra member points at a UScriptStruct and an FObjectProperty's at a UClass, and
    // nothing else inside an FProperty does either.
    const auto class_of_object = [&](std::uintptr_t object) -> std::string {
        const auto klass = memory::ReadPointer(object + kObjectClassPrivateOffset);
        if (!klass.has_value() || !memory::IsPlausiblePointer(*klass)) {
            return {};
        }
        return ResolveNameAt(*klass + kObjectNamePrivateOffset);
    };

    std::size_t inner_hits = 0;
    for (std::size_t slot = offset_slot + 8; slot <= offset_slot + 0x60; slot += 8) {
        std::size_t hits = 0;
        for (const std::uintptr_t node : sample) {
            const auto klass = memory::ReadPointer(node + winner.field_class);
            if (!klass.has_value() || !memory::IsPlausiblePointer(*klass)) {
                continue;
            }
            const std::string type = ResolveNameAt(*klass);
            const bool wants_struct = type == "StructProperty";
            const bool wants_class  = type == "ObjectProperty" || type == "ClassProperty" ||
                                     type == "ObjectPtrProperty";
            if (!wants_struct && !wants_class) {
                continue;
            }
            const auto target = memory::ReadPointer(node + slot);
            if (!target.has_value() || !memory::IsPlausiblePointer(*target)) {
                continue;
            }
            const std::string target_class = class_of_object(*target);
            if ((wants_struct && target_class == "ScriptStruct") ||
                (wants_class && target_class == "Class")) {
                ++hits;
            }
        }
        if (hits > inner_hits) {
            inner_hits                        = hits;
            best.struct_property_inner_offset = slot;
        }
    }
    if (inner_hits == 0) {
        best.struct_property_inner_offset = derive(0x2C);
    }

    // RepNotifyFunc is the last member of FProperty, so it sits one FName before the end.
    // Checked rather than assumed: it must never look like a pointer, because the members
    // around it are exactly that and confusing the two is how this went wrong before.
    if (best.struct_property_inner_offset >= 8) {
        const std::size_t slot = best.struct_property_inner_offset - 8;
        bool              ok   = true;
        for (const std::uintptr_t node : sample) {
            const auto value = memory::ReadPointer(node + slot);
            if (!value.has_value()) {
                continue;
            }
            if (memory::IsPlausiblePointer(*value) && memory::IsReadable(*value, 0x30)) {
                ok = false;
                break;
            }
            const std::string name = ResolveNameAt(node + slot);
            if (*value != 0 && name.empty()) {
                ok = false;
                break;
            }
        }
        if (ok) {
            best.rep_notify_offset = slot;
        }
    }

    // ---- Phase four: UStruct, pinned against classes of known size --------------------
    best.super_struct_offset = kStructSuperOffset;
    if (anchors.field_class != 0 && anchors.object_class != 0) {
        for (std::size_t slot = 0x28; slot <= 0x60; slot += 8) {
            const auto from_field = memory::ReadPointer(anchors.field_class + slot);
            if (!from_field.has_value() || *from_field != anchors.object_class) {
                continue;
            }
            if (anchors.struct_class != 0) {
                const auto from_struct = memory::ReadPointer(anchors.struct_class + slot);
                if (!from_struct.has_value() || *from_struct != anchors.field_class) {
                    continue;
                }
            }
            best.super_struct_offset = slot;
            break;
        }
    }

    if (anchors.HasSizeAnchors()) {
        for (std::size_t slot = 0x30; slot <= 0x80; slot += 4) {
            const auto object_size = memory::Read<std::int32_t>(anchors.object_class + slot);
            const auto field_size  = memory::Read<std::int32_t>(anchors.field_class + slot);
            if (!object_size.has_value() || !field_size.has_value()) {
                continue;
            }
            if (*object_size == kSizeOfUObject && *field_size == kSizeOfUField) {
                best.properties_size_offset = slot;
                best.anchored               = true;
                break;
            }
        }
    }
    if (!best.anchored) {
        // No anchors available. Fall back to the bounding argument, but refuse any slot
        // that overlaps the child properties pointer, which is what produced a
        // "PropertiesSize" of 365 that was really the top half of a heap address.
        std::int32_t highest_end = 0;
        for (const std::uintptr_t node : winner.nodes) {
            if (const auto value = memory::Read<std::int32_t>(node + offset_slot);
                value.has_value()) {
                highest_end = std::max(highest_end, *value);
            }
        }
        for (std::size_t slot = 0x30; slot <= 0x80; slot += 4) {
            if (slot >= winner.child && slot < winner.child + sizeof(std::uintptr_t)) {
                continue;
            }
            const auto value = memory::Read<std::int32_t>(candidate_structs.front() + slot);
            if (value.has_value() && *value >= highest_end && *value > 0 && *value <= 0x20000) {
                best.properties_size_offset = slot;
                break;
            }
        }
    }

    // ---- Phase five: UStruct::Children, the UFunction list ---------------------------
    if (anchors.class_with_functions != 0) {
        std::size_t best_length = 0;
        for (std::size_t child = 0x28; child <= 0x70; child += 8) {
            const auto head = memory::ReadPointer(anchors.class_with_functions + child);
            if (!head.has_value() || !memory::IsPlausiblePointer(*head) ||
                class_name_of(*head) != "Function") {
                continue;
            }
            for (std::size_t next = 0x20; next <= 0x48; next += 8) {
                std::size_t                        length = 0;
                std::uintptr_t                     node   = *head;
                std::unordered_set<std::uintptr_t> visited;
                while (memory::IsPlausiblePointer(node) && visited.insert(node).second &&
                       length < kMaxPropertiesPerStruct) {
                    if (!IsPlausibleFieldName(ResolveNameAt(node + kObjectNamePrivateOffset))) {
                        break;
                    }
                    ++length;
                    const auto following = memory::ReadPointer(node + next);
                    if (!following.has_value() || *following == 0) {
                        break;
                    }
                    node = *following;
                }
                if (length > best_length) {
                    best_length                   = length;
                    best.children_offset          = child;
                    best.field_object_next_offset = next;
                }
            }
        }
    }

    best.detected = true;
    return best;
}

std::vector<std::string> Reflection::ReadFunctionNames(std::uintptr_t struct_address) const {
    std::vector<std::string> names;
    if (layout_.children_offset == 0 || layout_.field_object_next_offset == 0) {
        return names;
    }
    const auto head = memory::ReadPointer(struct_address + layout_.children_offset);
    if (!head.has_value()) {
        return names;
    }

    std::uintptr_t                     node = *head;
    std::unordered_set<std::uintptr_t> visited;
    while (memory::IsPlausiblePointer(node) && visited.insert(node).second &&
           names.size() < kMaxPropertiesPerStruct) {
        std::string name = ResolveNameAt(node + kObjectNamePrivateOffset);
        if (!IsPlausibleFieldName(name)) {
            break;
        }
        names.push_back(std::move(name));
        const auto following = memory::ReadPointer(node + layout_.field_object_next_offset);
        if (!following.has_value() || *following == 0) {
            break;
        }
        node = *following;
    }
    return names;
}

std::string Reflection::DescribeProperty(const PropertyInfo& property) const {
    std::string type = property.type_name.empty() ? std::string{"?"} : property.type_name;
    if (!property.inner_type_name.empty()) {
        type += "<" + property.inner_type_name + ">";
    }

    std::string notes;
    const auto note = [&notes](std::string_view text) {
        if (!notes.empty()) {
            notes += " ";
        }
        notes += text;
    };
    if (property.IsReplicated()) {
        note("REPLICATED");
    }
    if (!property.rep_notify.empty()) {
        note("notify=" + property.rep_notify);
    }
    if ((property.flags & kPropertyFlagConfig) != 0) {
        note("config");
    }
    if ((property.flags & kPropertyFlagTransient) != 0) {
        note("transient");
    }

    return std::format("+0x{:<5X} {:<44} {:<34} size {:<5} {}", property.offset, property.name,
                       type, property.TotalSize(), notes);
}

namespace {

/// An engine FString or FText payload read out of the process.
///
/// FString is a TArray<TCHAR>: a pointer, a count that includes the terminator, and a
/// capacity. Nothing here assumes the string is well formed, because half of them are
/// tag names produced by a cooker and the rest are whatever a player typed.
[[nodiscard]] std::string ReadEngineString(std::uintptr_t address) {
    const auto data  = memory::ReadPointer(address);
    const auto count = memory::Read<std::int32_t>(address + sizeof(std::uintptr_t));
    if (!data.has_value() || !count.has_value() || *count <= 1 || *count > 4096) {
        return {};
    }
    if (!memory::IsPlausiblePointer(*data)) {
        return {};
    }

    const auto length = static_cast<std::size_t>(*count - 1);
    std::vector<char32_t> code_points;
    code_points.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
        const auto unit = memory::Read<std::uint16_t>(*data + index * sizeof(std::uint16_t));
        if (!unit.has_value() || *unit == 0) {
            break;
        }
        // Surrogate pairs, so a character outside the basic plane arrives whole.
        if (*unit >= 0xD800 && *unit <= 0xDBFF && index + 1 < length) {
            const auto low =
                memory::Read<std::uint16_t>(*data + (index + 1) * sizeof(std::uint16_t));
            if (low.has_value() && *low >= 0xDC00 && *low <= 0xDFFF) {
                code_points.push_back(static_cast<char32_t>(
                    0x10000 + ((*unit - 0xD800) << 10) + (*low - 0xDC00)));
                ++index;
                continue;
            }
        }
        code_points.push_back(static_cast<char32_t>(*unit));
    }
    return text::EncodeUtf8(code_points);
}

} // namespace

std::string Reflection::ReadValueText(std::uintptr_t instance_address,
                                      const PropertyInfo& property, int depth) const {
    const std::uintptr_t at = instance_address + static_cast<std::uintptr_t>(property.offset);
    const std::string&   type = property.type_name;

    const auto integer = [at](auto tag) -> std::string {
        using T = decltype(tag);
        const auto value = memory::Read<T>(at);
        return value.has_value() ? std::format("{}", *value) : std::string{"<unreadable>"};
    };

    if (type == "BoolProperty") {
        const auto value = memory::Read<std::uint8_t>(at);
        if (!value.has_value()) {
            return "<unreadable>";
        }
        return *value != 0 ? "true" : "false";
    }
    if (type == "IntProperty") {
        return integer(std::int32_t{});
    }
    if (type == "Int64Property") {
        return integer(std::int64_t{});
    }
    if (type == "Int16Property") {
        return integer(std::int16_t{});
    }
    if (type == "Int8Property" || type == "ByteProperty") {
        return integer(std::uint8_t{});
    }
    if (type == "UInt16Property") {
        return integer(std::uint16_t{});
    }
    if (type == "UInt32Property") {
        return integer(std::uint32_t{});
    }
    if (type == "UInt64Property") {
        return integer(std::uint64_t{});
    }
    if (type == "FloatProperty") {
        const auto value = memory::Read<float>(at);
        return value.has_value() ? std::format("{}", *value) : std::string{"<unreadable>"};
    }
    if (type == "DoubleProperty") {
        const auto value = memory::Read<double>(at);
        return value.has_value() ? std::format("{}", *value) : std::string{"<unreadable>"};
    }
    if (type == "EnumProperty") {
        // The underlying integer is what is stored; naming the value needs the UEnum's
        // own name table, which is a separate walk and rarely worth it here.
        const std::int32_t width = property.element_size > 0 ? property.element_size : 1;
        std::uint64_t      value = 0;
        if (!memory::GuardedRead(at, &value, static_cast<std::size_t>(std::min(width, 8)))) {
            return "<unreadable>";
        }
        return std::format("{}", value);
    }
    if (type == "NameProperty") {
        const std::string name = ResolveNameAt(at);
        return name.empty() ? "None" : name;
    }
    if (type == "StrProperty") {
        const std::string value = ReadEngineString(at);
        return value.empty() ? "\"\"" : "\"" + value + "\"";
    }
    if (type == "TextProperty") {
        return "<FText>";
    }
    if (type == "ObjectProperty" || type == "ObjectPtrProperty" || type == "ClassProperty" ||
        type == "ClassPtrProperty" || type == "WeakObjectProperty") {
        const auto pointer = memory::ReadPointer(at);
        if (!pointer.has_value() || *pointer == 0) {
            return "null";
        }
        if (!memory::IsPlausiblePointer(*pointer)) {
            return std::format("0x{:X}", *pointer);
        }
        const std::string name = ResolveNameAt(*pointer + kObjectNamePrivateOffset);
        return name.empty() ? std::format("0x{:X}", *pointer)
                            : std::format("{} (0x{:X})", name, *pointer);
    }
    if (type == "StructProperty") {
        if (depth <= 0 || property.inner_address == 0) {
            return std::format("<{}>", property.inner_type_name.empty()
                                           ? std::string{"struct"}
                                           : property.inner_type_name);
        }
        std::string body;
        for (const PropertyInfo& field : ReadAllProperties(property.inner_address)) {
            if (!body.empty()) {
                body += ", ";
            }
            body += field.name + "=" + ReadValueText(at, field, depth - 1);
            if (body.size() > 600) {
                body += ", ...";
                break;
            }
        }
        return "{" + body + "}";
    }
    if (type == "ArrayProperty") {
        const auto data  = memory::ReadPointer(at);
        const auto count = memory::Read<std::int32_t>(at + sizeof(std::uintptr_t));
        if (!count.has_value() || *count < 0 || *count > 0x100000) {
            return "<bad array>";
        }
        if (*count == 0) {
            return "[]";
        }
        if (!data.has_value() || !memory::IsPlausiblePointer(*data)) {
            return std::format("[{} element(s), unreadable]", *count);
        }
        if (depth <= 0 || property.inner_address == 0) {
            return std::format("[{} x {}]", *count,
                               property.inner_type_name.empty() ? std::string{"?"}
                                                                : property.inner_type_name);
        }

        Expected<PropertyInfo> inner = ReadProperty(property.inner_address);
        if (!inner.ok() || inner.value().element_size <= 0) {
            return std::format("[{} element(s)]", *count);
        }
        PropertyInfo element = inner.value();
        // The inner property describes the element type; inside the array the elements
        // start at zero rather than at the inner property's own offset.
        const std::int32_t stride = element.element_size;
        element.offset            = 0;

        std::string body;
        const std::int32_t shown = std::min(*count, 24);
        for (std::int32_t index = 0; index < shown; ++index) {
            if (!body.empty()) {
                body += ", ";
            }
            body += ReadValueText(*data + static_cast<std::uintptr_t>(index) *
                                              static_cast<std::uintptr_t>(stride),
                                  element, depth - 1);
            if (body.size() > 900) {
                body += ", ...";
                break;
            }
        }
        if (shown < *count) {
            body += std::format(", ... {} more", *count - shown);
        }
        return "[" + body + "]";
    }

    return std::format("<{}>", type.empty() ? std::string{"?"} : type);
}

std::vector<std::string> Reflection::DumpInstance(std::uintptr_t struct_address,
                                                  std::uintptr_t instance_address,
                                                  int            depth) const {
    std::vector<std::string> lines;
    for (const PropertyInfo& property : ReadAllProperties(struct_address)) {
        lines.push_back(std::format("+0x{:<5X} {:<44} {:<22} = {}", property.offset, property.name,
                                    property.inner_type_name.empty()
                                        ? property.type_name
                                        : property.type_name + "<" + property.inner_type_name + ">",
                                    ReadValueText(instance_address, property, depth)));
    }
    return lines;
}

Expected<PropertyInfo> Reflection::ReadProperty(std::uintptr_t property_address) const {
    const std::size_t needed =
        std::max({layout_.field_name_offset, layout_.offset_internal_offset,
                  layout_.element_size_offset, layout_.field_class_offset}) + 8;
    if (!memory::IsPlausiblePointer(property_address) ||
        !memory::IsReadable(property_address, needed)) {
        return Error{ErrorCode::NotFound,
                     std::format("property at 0x{:X} is unreadable", property_address)};
    }

    PropertyInfo info;
    info.address = property_address;
    info.name    = ResolveNameAt(property_address + layout_.field_name_offset);
    if (!IsPlausibleFieldName(info.name)) {
        return Error{ErrorCode::ValidationFailed,
                     std::format("property at 0x{:X} has an implausible name", property_address)};
    }

    if (const auto field_class = memory::ReadPointer(property_address + layout_.field_class_offset);
        field_class.has_value() && memory::IsPlausiblePointer(*field_class)) {
        // FFieldClass::Name is its first member. For a UProperty style layout this
        // instead reads the UClass name, which is equally useful as a type label.
        info.type_name = ResolveNameAt(*field_class);
        if (info.type_name.empty()) {
            info.type_name = ResolveNameAt(*field_class + kObjectNamePrivateOffset);
        }
    }

    const auto offset       = memory::Read<std::int32_t>(property_address +
                                                        layout_.offset_internal_offset);
    const auto element_size = memory::Read<std::int32_t>(property_address +
                                                        layout_.element_size_offset);
    if (!offset.has_value()) {
        return Error{ErrorCode::NotFound,
                     std::format("property '{}' offset is unreadable", info.name)};
    }
    if (*offset < 0 || *offset > 0x20000) {
        return Error{ErrorCode::ValidationFailed,
                     std::format("property '{}' has an implausible offset {}", info.name,
                                 *offset)};
    }

    info.offset = *offset;
    if (element_size.has_value() && *element_size >= 0 && *element_size <= 0x20000) {
        info.element_size = *element_size;
    }

    info.array_dim = 1;
    if (layout_.array_dim_offset != 0) {
        if (const auto dim = memory::Read<std::int32_t>(property_address +
                                                       layout_.array_dim_offset);
            dim.has_value() && *dim >= 1 && *dim <= 0x10000) {
            info.array_dim = *dim;
        }
    }
    if (layout_.property_flags_offset != 0) {
        if (const auto flags = memory::Read<std::uint64_t>(property_address +
                                                          layout_.property_flags_offset);
            flags.has_value()) {
            info.flags = *flags;
        }
    }
    if (layout_.rep_index_offset != 0) {
        if (const auto index = memory::Read<std::uint16_t>(property_address +
                                                          layout_.rep_index_offset);
            index.has_value()) {
            info.rep_index = *index;
        }
    }
    if (layout_.rep_notify_offset != 0) {
        // Resolves to "None" for the overwhelming majority, which IsPlausibleFieldName
        // rejects, so an empty string here means exactly "no notify".
        std::string notify = ResolveNameAt(property_address + layout_.rep_notify_offset);
        if (IsPlausibleFieldName(notify)) {
            info.rep_notify = std::move(notify);
        }
    }

    // What the property points at. A wall of "ObjectProperty" says nothing; the same wall
    // annotated with BlamNetworkPlayerStateComponent is a map of the session.
    //
    // Searched over the first few members past FProperty rather than read from a fixed one.
    // FStructProperty leads with its UScriptStruct, but FArrayProperty on this build puts
    // its element flags first and the inner property after them, so a single offset that
    // is right for one is a null read for the other.
    if (layout_.struct_property_inner_offset != 0) {
        // FEnumProperty leads with its underlying integer property, not with the UEnum, so
        // it belongs with the containers rather than with the object types.
        const bool is_field = info.type_name == "ArrayProperty" ||
                              info.type_name == "SetProperty" ||
                              info.type_name == "OptionalProperty" ||
                              info.type_name == "EnumProperty";
        const bool is_object = info.type_name == "StructProperty" ||
                               info.type_name == "ObjectProperty" ||
                               info.type_name == "ObjectPtrProperty" ||
                               info.type_name == "ClassProperty" ||
                               info.type_name == "ClassPtrProperty" ||
                               info.type_name == "WeakObjectProperty" ||
                               info.type_name == "SoftObjectProperty" ||
                               info.type_name == "InterfaceProperty" ||
                               info.type_name == "ByteProperty";

        for (std::size_t step = 0; step <= 0x10 && info.inner_address == 0; step += 8) {
            const auto inner =
                memory::ReadPointer(property_address + layout_.struct_property_inner_offset + step);
            if (!inner.has_value() || !memory::IsPlausiblePointer(*inner)) {
                continue;
            }

            std::string resolved;
            if (is_field) {
                // An inner FProperty, accepted only when it says it is one.
                const auto klass = memory::ReadPointer(*inner + layout_.field_class_offset);
                if (!klass.has_value() || !memory::IsPlausiblePointer(*klass)) {
                    continue;
                }
                const std::string element = ResolveNameAt(*klass);
                if (!IsPropertyClassName(element)) {
                    continue;
                }
                resolved = element;
            } else if (is_object) {
                // A UObject, accepted only when its own class names a reflected type.
                const auto klass = memory::ReadPointer(*inner + kObjectClassPrivateOffset);
                if (!klass.has_value() || !memory::IsPlausiblePointer(*klass)) {
                    continue;
                }
                const std::string kind = ResolveNameAt(*klass + kObjectNamePrivateOffset);
                if (kind != "ScriptStruct" && kind != "Class" && kind != "Enum") {
                    continue;
                }
                resolved = ResolveNameAt(*inner + kObjectNamePrivateOffset);
            } else {
                break;
            }

            if (IsPlausibleFieldName(resolved)) {
                info.inner_address   = *inner;
                info.inner_type_name = std::move(resolved);
            }
        }
    }
    return info;
}

std::vector<PropertyInfo> Reflection::ReadProperties(std::uintptr_t struct_address) const {
    std::vector<PropertyInfo> properties;
    if (!memory::IsPlausiblePointer(struct_address) ||
        !memory::IsReadable(struct_address, layout_.child_properties_offset + 8)) {
        return properties;
    }

    const auto head = memory::ReadPointer(struct_address + layout_.child_properties_offset);
    if (!head.has_value()) {
        return properties;
    }

    const std::vector<std::uintptr_t> nodes =
        WalkChain(*head, layout_.field_name_offset, layout_.field_next_offset,
                  kMaxPropertiesPerStruct);

    properties.reserve(nodes.size());
    for (const std::uintptr_t node : nodes) {
        Expected<PropertyInfo> property = ReadProperty(node);
        if (property.ok()) {
            properties.push_back(std::move(property).value());
        }
    }
    return properties;
}

Expected<StructInfo> Reflection::ReadStruct(std::uintptr_t struct_address) const {
    if (!memory::IsPlausiblePointer(struct_address) ||
        !memory::IsReadable(struct_address, layout_.properties_size_offset + 8)) {
        return Error{ErrorCode::NotFound,
                     std::format("struct at 0x{:X} is unreadable", struct_address)};
    }

    StructInfo info;
    info.address = struct_address;
    info.name    = ResolveNameAt(struct_address + kObjectNamePrivateOffset);

    if (const auto super = memory::ReadPointer(struct_address + layout_.super_struct_offset);
        super.has_value() && memory::IsPlausiblePointer(*super)) {
        info.super_address = *super;
    }
    if (const auto size = memory::Read<std::int32_t>(struct_address +
                                                    layout_.properties_size_offset);
        size.has_value()) {
        info.properties_size = *size;
    }

    info.properties = ReadProperties(struct_address);
    return info;
}

std::vector<PropertyInfo> Reflection::ReadAllProperties(std::uintptr_t struct_address) const {
    std::vector<std::vector<PropertyInfo>> levels;
    std::uintptr_t current = struct_address;
    std::unordered_set<std::uintptr_t> visited;

    for (std::size_t depth = 0; depth < kMaxInheritanceDepth && current != 0; ++depth) {
        if (!visited.insert(current).second) {
            break;
        }
        levels.push_back(ReadProperties(current));

        const auto super = memory::ReadPointer(current + layout_.super_struct_offset);
        if (!super.has_value() || !memory::IsPlausiblePointer(*super)) {
            break;
        }
        current = *super;
    }

    // Base first, matching memory order.
    std::vector<PropertyInfo> all;
    for (auto it = levels.rbegin(); it != levels.rend(); ++it) {
        all.insert(all.end(), it->begin(), it->end());
    }
    return all;
}

Expected<PropertyInfo> Reflection::FindProperty(std::uintptr_t struct_address,
                                                std::string_view name) const {
    const std::vector<PropertyInfo> properties = ReadAllProperties(struct_address);
    const auto it = std::find_if(properties.begin(), properties.end(),
                                 [name](const PropertyInfo& p) { return p.name == name; });
    if (it == properties.end()) {
        return Error{ErrorCode::NotFound,
                     std::format("no property named '{}' in the struct at 0x{:X} ({} searched)",
                                 name, struct_address, properties.size())};
    }
    return *it;
}

std::vector<PropertyInfo> Reflection::FindPropertiesContaining(std::uintptr_t struct_address,
                                                               std::string_view fragment) const {
    std::vector<PropertyInfo> results;
    if (fragment.empty()) {
        return results;
    }
    for (const PropertyInfo& property : ReadAllProperties(struct_address)) {
        if (property.name.find(fragment) != std::string::npos) {
            results.push_back(property);
        }
    }
    return results;
}

Result Reflection::VerifyLayout(std::uintptr_t struct_address, std::string& out_report) const {
    Expected<StructInfo> info = ReadStruct(struct_address);
    if (!info.ok()) {
        out_report = std::format("could not read the struct at 0x{:X}: {}", struct_address,
                                 info.message());
        return Result::Fail(ErrorCode::SymbolValidationFailed, out_report);
    }

    const StructInfo& s = info.value();
    std::size_t inside  = 0;
    std::size_t outside = 0;
    for (const PropertyInfo& property : s.properties) {
        if (property.offset + property.TotalSize() <= s.properties_size) {
            ++inside;
        } else {
            ++outside;
        }
    }

    out_report = std::format("struct '{}' at 0x{:X}: size {}, {} field(s), {} in bounds, {} out",
                             s.name, s.address, s.properties_size, s.properties.size(), inside,
                             outside);

    if (s.name.empty()) {
        return Result::Fail(ErrorCode::SymbolValidationFailed,
                            out_report + "; no resolvable name");
    }
    if (s.properties.empty()) {
        return Result::Fail(ErrorCode::SymbolValidationFailed,
                            out_report + "; no readable properties");
    }
    if (outside > 0) {
        return Result::Fail(ErrorCode::SymbolValidationFailed,
                            out_report + "; fields lie outside their own struct");
    }
    return Result::Success();
}

std::size_t Reflection::DetectStructPropertyInnerOffset(
    const std::vector<std::uintptr_t>& owner_structs,
    const std::vector<std::uintptr_t>& known_script_structs) const {
    if (known_script_structs.empty()) {
        return 0;
    }
    const std::unordered_set<std::uintptr_t> known(known_script_structs.begin(),
                                                   known_script_structs.end());

    // Tally how often each candidate offset lands on a known ScriptStruct. The correct
    // offset hits on every StructProperty; a coincidence hits once.
    std::vector<std::pair<std::size_t, std::size_t>> scores;
    for (std::size_t candidate = 0x28; candidate <= 0x80; candidate += 8) {
        std::size_t hits = 0;
        for (const std::uintptr_t owner : owner_structs) {
            for (const PropertyInfo& property : ReadProperties(owner)) {
                if (property.type_name != "StructProperty") {
                    continue;
                }
                const auto inner = memory::ReadPointer(property.address + candidate);
                if (inner.has_value() && known.contains(*inner)) {
                    ++hits;
                }
            }
        }
        if (hits > 0) {
            scores.emplace_back(candidate, hits);
        }
    }

    if (scores.empty()) {
        return 0;
    }
    const auto best = std::max_element(scores.begin(), scores.end(),
                                       [](const auto& a, const auto& b) {
                                           return a.second < b.second;
                                       });
    return best->first;
}

std::uintptr_t Reflection::ResolveStructPropertyInner(std::uintptr_t property_address) const {
    if (!layout_.struct_property_inner_detected) {
        return 0;
    }
    const auto inner = memory::ReadPointer(property_address +
                                          layout_.struct_property_inner_offset);
    if (!inner.has_value() || !memory::IsPlausiblePointer(*inner)) {
        return 0;
    }
    return *inner;
}

std::size_t Reflection::DetectPropertiesSizeOffset(
    const std::vector<std::uintptr_t>& structs) const {
    // Each struct's size must be at least as large as the end of its last field.
    struct Bound {
        std::uintptr_t address{0};
        std::int32_t   minimum{0};
    };
    std::vector<Bound> bounds;
    for (const std::uintptr_t address : structs) {
        std::int32_t highest = 0;
        for (const PropertyInfo& property : ReadProperties(address)) {
            highest = std::max(highest, property.offset + std::max(property.TotalSize(), 1));
        }
        if (highest > 0) {
            bounds.push_back({address, highest});
        }
    }
    if (bounds.size() < 2) {
        return 0;
    }

    std::size_t best_offset = 0;
    std::int32_t best_slack  = std::numeric_limits<std::int32_t>::max();

    for (std::size_t candidate = 0x30; candidate <= 0x70; candidate += 4) {
        std::int32_t total_slack = 0;
        bool         ok          = true;
        std::unordered_set<std::int32_t> distinct;

        for (const Bound& bound : bounds) {
            const auto value = memory::Read<std::int32_t>(bound.address + candidate);
            if (!value.has_value() || *value < bound.minimum || *value > 0x20000) {
                ok = false;
                break;
            }
            total_slack += (*value - bound.minimum);
            distinct.insert(*value);
        }
        // A single shared value across every struct is a constant, not a size. This is
        // the check that was missing: properties_size read 387 for all eight structs.
        if (!ok || distinct.size() < 2) {
            continue;
        }
        if (total_slack < best_slack) {
            best_slack  = total_slack;
            best_offset = candidate;
        }
    }
    return best_offset;
}

Expected<bool> Reflection::ReadBoolField(std::uintptr_t instance_address,
                                        const PropertyInfo& property) const {
    if (!property.IsBool()) {
        return Error{ErrorCode::InvalidArgument,
                     std::format("'{}' is a {}, not a BoolProperty", property.name,
                                 property.type_name)};
    }
    const std::uintptr_t address = instance_address + static_cast<std::uintptr_t>(property.offset);
    const auto value = memory::Read<std::uint8_t>(address);
    if (!value.has_value()) {
        return Error{ErrorCode::NotFound,
                     std::format("'{}' at 0x{:X} is unreadable", property.name, address)};
    }
    return *value != 0;
}

Result Reflection::WriteBoolField(std::uintptr_t instance_address, const PropertyInfo& property,
                                 bool value) const {
    if (!property.IsBool()) {
        return Result::Fail(ErrorCode::InvalidArgument,
                            std::format("'{}' is a {}, not a BoolProperty", property.name,
                                        property.type_name));
    }
    const std::uintptr_t address = instance_address + static_cast<std::uintptr_t>(property.offset);
    const std::uint8_t   byte    = value ? 1u : 0u;
    if (!memory::Write(address, byte)) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("could not write '{}' at 0x{:X}", property.name,
                                        address));
    }

    // Read back. A write that does not stick is worse than a refused write, because the
    // caller would believe the setting took effect.
    const auto confirmed = memory::Read<std::uint8_t>(address);
    if (!confirmed.has_value() || (*confirmed != 0) != value) {
        return Result::Fail(ErrorCode::SymbolValidationFailed,
                            std::format("wrote {} to '{}' at 0x{:X} but read back {}", byte,
                                        property.name, address,
                                        confirmed.has_value() ? *confirmed : 0xFFu));
    }
    return Result::Success();
}

std::string Reflection::ProbeStructLayout(std::uintptr_t struct_address,
                                         std::size_t bytes) const {
    std::string out = std::format("annotated dump of 0x{:X}:\n", struct_address);

    for (std::size_t offset = 0; offset + 8 <= bytes; offset += 8) {
        const auto value = memory::Read<std::uintptr_t>(struct_address + offset);
        if (!value.has_value()) {
            out += std::format("  +0x{:02X}  <unreadable>\n", offset);
            continue;
        }

        std::string note;
        // As two int32s, which is how UE packs sizes and FNames.
        const auto low  = static_cast<std::int32_t>(*value & 0xFFFFFFFFu);
        const auto high = static_cast<std::int32_t>(*value >> 32);
        note += std::format(" i32=({}, {})", low, high);

        if (memory::IsPlausiblePointer(*value) && memory::IsReadable(*value, 0x40)) {
            note += " ptr:readable";
            // If it points at something whose FName resolves, say so and what to.
            for (const std::size_t name_off : {std::size_t{0x10}, std::size_t{0x18},
                                               std::size_t{0x20}, std::size_t{0x28}}) {
                const std::string name = ResolveNameAt(*value + name_off);
                if (IsPlausibleFieldName(name)) {
                    note += std::format(" name@+0x{:X}=\"{}\"", name_off, name);
                }
            }
        }
        // The value read as an FName index directly.
        if (const std::string self = ResolveNameAt(struct_address + offset);
            IsPlausibleFieldName(self)) {
            note += std::format(" asFName=\"{}\"", self);
        }

        out += std::format("  +0x{:02X}  0x{:016X}{}\n", offset, *value, note);
    }
    return out;
}

} // namespace mpe::unreal

