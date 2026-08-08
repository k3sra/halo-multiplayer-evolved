// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Unreal/Reflection.h
//
// Walks UE5 UStruct property chains to recover field names, types and offsets.
//
// WHY THIS IS THE THIRD KEYSTONE
//
// NamePool gave identity. ObjectArray gave instances. Neither tells us where a field
// lives inside a struct, and without an offset a located struct is just an address.
//
// The structs that matter were located in the running game, all eight in one pass:
//
//   /Script/BlamGlue.BlamGameEngineBaseVariantStorage
//   /Script/BlamGlue.BlamGameEngineSocialOptions
//   /Script/BlamGlue.BlamGameEnginePlayerTraits
//   /Script/BlamGlue.BlamGameEngineCampaignVariantStorage
//   /Script/BlamEngine.BlamScenarioGameOptions
//
// Finding bFriendlyFireEnabled's offset inside the social options is the concrete goal.
//
// OFFSETS ARE DETECTED, NOT ASSUMED
//
// The documented UE5 layout puts ChildProperties at 0x40 and PropertiesSize at 0x48.
// On this build that is wrong: reading BlamScenarioGameOptions that way reported size 0
// and zero fields, while its name resolved correctly, proving the UObject offsets are
// right and the UStruct ones are not.
//
// So the constants below are starting guesses only, and DetectLayout establishes the
// real values empirically. Two independent signals make detection reliable:
//
//   A property chain's nodes have names that resolve to plausible identifiers, and a
//   chain two or more nodes long with distinct resolvable names is not coincidence.
//
//   A struct's field offsets increase across that chain. Scanning for the int32 slot
//   whose values are distinct, in range, and non decreasing identifies Offset_Internal
//   without knowing the engine version.
//
// This is the same approach that located the Blam tables: anchor on content the engine
// itself depends on, then validate before trusting.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Result.h"
#include "Unreal/NamePool.h"

namespace mpe::unreal {

/// Starting guesses, the documented UE5 layout. Replaced at runtime by DetectLayout.
inline constexpr std::size_t kStructSuperOffset            = 0x30;
inline constexpr std::size_t kStructChildrenOffset         = 0x38;
inline constexpr std::size_t kStructChildPropertiesOffset  = 0x40;
inline constexpr std::size_t kStructPropertiesSizeOffset   = 0x48;

inline constexpr std::size_t kFFieldClassOffset        = 0x08;
inline constexpr std::size_t kFFieldNextOffset         = 0x20;
inline constexpr std::size_t kFFieldNameOffset         = 0x28;
inline constexpr std::size_t kFPropertyElementSize     = 0x3C;
inline constexpr std::size_t kFPropertyOffsetInternal  = 0x4C;

/// UObject::NamePrivate. Confirmed correct on this build, since struct names resolve.
inline constexpr std::size_t kObjectNamePrivateOffset = 0x18;

/// Guard rails.
inline constexpr std::size_t kMaxPropertiesPerStruct = 4096;
inline constexpr std::size_t kMaxInheritanceDepth    = 64;

/// EPropertyFlags bits this reader cares about. The full set is large; these are the
/// ones that decide whether a field crosses the wire.
inline constexpr std::uint64_t kPropertyFlagNet         = 0x0000000000000020ull;
inline constexpr std::uint64_t kPropertyFlagRepNotify   = 0x0000000100000000ull;
inline constexpr std::uint64_t kPropertyFlagTransient   = 0x0000000000002000ull;
inline constexpr std::uint64_t kPropertyFlagConfig      = 0x0000000000004000ull;
inline constexpr std::uint64_t kPropertyFlagEditorOnly  = 0x0000000000080000ull;
inline constexpr std::uint64_t kPropertyFlagBlueprintVisible = 0x0000000000000004ull;

/// One reflected field.
struct PropertyInfo {
    std::string    name;
    std::string    type_name;
    /// The type a container, object or struct property points at, once resolved.
    std::string    inner_type_name;
    /// RepNotifyFunc, the handler called on a client when the value arrives. Empty
    /// when the property is not replicated with a notify.
    std::string    rep_notify;
    std::uintptr_t address{0};
    /// FStructProperty::Struct, FObjectPropertyBase::PropertyClass or
    /// FArrayProperty::Inner, whichever applies. Zero when the type has no payload.
    std::uintptr_t inner_address{0};
    std::uint64_t  flags{0};
    std::int32_t   offset{0};
    std::int32_t   element_size{0};
    std::int32_t   array_dim{1};
    std::uint16_t  rep_index{0};

    [[nodiscard]] std::int32_t TotalSize() const noexcept {
        return element_size * (array_dim > 0 ? array_dim : 1);
    }
    [[nodiscard]] bool IsBool() const noexcept { return type_name == "BoolProperty"; }
    /// True when the engine replicates this field of its own accord.
    [[nodiscard]] bool IsReplicated() const noexcept { return (flags & kPropertyFlagNet) != 0; }
};

/// One reflected struct or class.
struct StructInfo {
    std::string    name;
    std::uintptr_t address{0};
    std::uintptr_t super_address{0};
    std::int32_t   properties_size{0};
    std::vector<PropertyInfo> properties;
};

/// The set of offsets this reader uses. Discovered at runtime.
struct ReflectionLayout {
    std::size_t child_properties_offset{kStructChildPropertiesOffset};
    std::size_t field_name_offset{kFFieldNameOffset};
    std::size_t field_next_offset{kFFieldNextOffset};
    std::size_t field_class_offset{kFFieldClassOffset};
    std::size_t offset_internal_offset{kFPropertyOffsetInternal};
    std::size_t element_size_offset{kFPropertyElementSize};
    std::size_t properties_size_offset{kStructPropertiesSizeOffset};
    std::size_t super_struct_offset{kStructSuperOffset};

    /// UStruct::Children, the UObject chain that holds a class's UFunctions.
    std::size_t children_offset{0};
    /// UField::Next, which links that chain. Distinct from FField::Next above.
    std::size_t field_object_next_offset{0};

    std::size_t array_dim_offset{0};
    std::size_t property_flags_offset{0};
    std::size_t rep_index_offset{0};
    std::size_t rep_notify_offset{0};

    /// FStructProperty::Struct, the inner UScriptStruct a StructProperty refers to.
    /// Detected by looking for a pointer to a ScriptStruct we already located.
    ///
    /// FObjectPropertyBase::PropertyClass and FArrayProperty::Inner are the first member
    /// past FProperty as well, so one offset serves all three.
    std::size_t struct_property_inner_offset{0};
    bool        struct_property_inner_detected{false};

    bool        detected{false};
    /// True when the offsets were pinned against classes whose size the engine fixes,
    /// rather than inferred from what looked plausible.
    bool        anchored{false};
    std::size_t detected_chain_length{0};

    [[nodiscard]] std::string Describe() const;
};

/// Classes whose layout the engine itself fixes, used to pin offsets to certainty.
///
/// WHY THESE THREE
///
/// Every heuristic here answers "does this look like a property chain", and a heuristic
/// that only has to look right has already been wrong once on this build: it settled on
/// an int32 slot for PropertiesSize that was really the top half of the pointer beside
/// it, which read 365 on one run and 650 on the next because that is where the heap
/// happened to be.
///
/// UObject, UField and UStruct do not need a heuristic. UObject is 0x28 bytes and UField
/// is 0x30 on any 64 bit build, UField derives from UObject and UStruct from UField, and
/// all three are registered as ordinary UClass objects. So the slot holding PropertiesSize
/// is the one reading 0x28 on the first and 0x30 on the second, and the slot holding
/// SuperStruct is the one where the second points at the first. Two constraints on one
/// offset is proof, not a guess.
struct LayoutAnchors {
    std::uintptr_t object_class{0};        ///< UClass "Object". PropertiesSize is 0x28.
    std::uintptr_t field_class{0};         ///< UClass "Field".  PropertiesSize is 0x30.
    std::uintptr_t struct_class{0};        ///< UClass "Struct". Super is Field.
    std::uintptr_t class_with_functions{0};///< Any UClass known to own UFunctions.
    std::uint32_t  function_name_index{0}; ///< FName index of "Function".

    [[nodiscard]] bool HasSizeAnchors() const noexcept {
        return object_class != 0 && field_class != 0;
    }
};

/// Where a struct type is embedded inside an owning class or struct.
///
/// This is what makes a live instance findable: a ScriptStruct has no presence in the
/// object array, so the only way to reach one is through the object that contains it.
struct StructUsage {
    std::uintptr_t owner_address{0};   ///< The owning UClass or UScriptStruct.
    std::string    owner_name;
    std::string    owner_class_name;   ///< "Class", "ScriptStruct", ...
    std::string    property_name;      ///< The field name inside the owner.
    std::int32_t   property_offset{0}; ///< Byte offset of the embedded struct.
};

/// Reads UE5 reflection metadata.
///
/// Holds a reference to the name pool, which must outlive it.
class Reflection {
public:
    explicit Reflection(const NamePool& names) noexcept : names_(&names) {}

    [[nodiscard]] const ReflectionLayout& Layout() const noexcept { return layout_; }
    void SetLayout(const ReflectionLayout& layout) { layout_ = layout; }

    /// Determines the real offsets by inspecting structs known to have fields.
    ///
    /// Pass several candidates: a struct with no fields of its own cannot reveal the
    /// chain layout, so the one that yields the strongest chain wins. Supplying anchors
    /// turns the guesswork parts, SuperStruct and PropertiesSize, into measurements.
    [[nodiscard]] ReflectionLayout DetectLayout(
        const std::vector<std::uintptr_t>& candidate_structs,
        const LayoutAnchors&               anchors = {}) const;

    /// Names of the UFunctions a class owns, read from the class itself.
    ///
    /// The alternative is a pass over the whole object array per class looking for
    /// functions whose Outer matches, which is fifty thousand reads and took two seconds
    /// each time. UStruct::Children is the list the engine keeps for exactly this.
    [[nodiscard]] std::vector<std::string> ReadFunctionNames(std::uintptr_t struct_address) const;

    /// One property rendered for the log, with its type, flags and replication.
    [[nodiscard]] std::string DescribeProperty(const PropertyInfo& property) const;

    /// Renders one field's live value as text.
    ///
    /// Offsets alone answer where a setting lives; this answers what it currently says,
    /// which is the difference between knowing a tag asset has a mode list and knowing
    /// which modes are in it. Structs and arrays recurse until depth runs out, so a
    /// bounded amount of work happens however deeply nested the type is.
    [[nodiscard]] std::string ReadValueText(std::uintptr_t      instance_address,
                                            const PropertyInfo& property,
                                            int                 depth = 2) const;

    /// Every field of a live instance, one line each, ready for the log.
    [[nodiscard]] std::vector<std::string> DumpInstance(std::uintptr_t struct_address,
                                                        std::uintptr_t instance_address,
                                                        int            depth = 2) const;

    [[nodiscard]] Expected<StructInfo> ReadStruct(std::uintptr_t struct_address) const;
    [[nodiscard]] std::vector<PropertyInfo> ReadProperties(std::uintptr_t struct_address) const;
    [[nodiscard]] std::vector<PropertyInfo> ReadAllProperties(std::uintptr_t struct_address) const;

    [[nodiscard]] Expected<PropertyInfo> FindProperty(std::uintptr_t struct_address,
                                                      std::string_view name) const;
    [[nodiscard]] std::vector<PropertyInfo> FindPropertiesContaining(
        std::uintptr_t struct_address, std::string_view fragment) const;

    /// Confirms the active layout produces self consistent results.
    [[nodiscard]] Result VerifyLayout(std::uintptr_t struct_address,
                                     std::string& out_report) const;

    /// Annotated hex dump of a struct header, for when detection itself fails.
    [[nodiscard]] std::string ProbeStructLayout(std::uintptr_t struct_address,
                                               std::size_t bytes = 0x90) const;

    /// Determines FStructProperty::Struct's offset.
    ///
    /// Detected rather than assumed, using the same trick as everything else here: we
    /// already know several ScriptStruct addresses, so the correct offset is the one
    /// where a StructProperty's pointer lands on one of them.
    ///
    /// known_script_structs maps address to name, and owner_structs are structs whose
    /// StructProperty fields should point into that set.
    [[nodiscard]] std::size_t DetectStructPropertyInnerOffset(
        const std::vector<std::uintptr_t>& owner_structs,
        const std::vector<std::uintptr_t>& known_script_structs) const;

    /// Reads the inner UScriptStruct a StructProperty refers to, or zero.
    [[nodiscard]] std::uintptr_t ResolveStructPropertyInner(
        std::uintptr_t property_address) const;

    /// Refines properties_size_offset using several structs at once.
    ///
    /// A single struct cannot distinguish the real size field from an unrelated constant
    /// that happens to be large enough. Requiring the value to bound each struct's own
    /// fields, and to differ between structs, does.
    [[nodiscard]] std::size_t DetectPropertiesSizeOffset(
        const std::vector<std::uintptr_t>& structs) const;

    /// Sets one boolean field on a live instance.
    ///
    /// address is the instance base, not the ScriptStruct. Writes a single byte, because
    /// that is what a BoolProperty of size 1 occupies.
    [[nodiscard]] Result WriteBoolField(std::uintptr_t instance_address,
                                       const PropertyInfo& property, bool value) const;

    /// Reads one boolean field from a live instance.
    [[nodiscard]] Expected<bool> ReadBoolField(std::uintptr_t instance_address,
                                              const PropertyInfo& property) const;

    /// Resolves an FName stored at an address.
    [[nodiscard]] std::string ResolveNameAt(std::uintptr_t address) const;

private:
    [[nodiscard]] Expected<PropertyInfo> ReadProperty(std::uintptr_t property_address) const;

    /// Walks a candidate chain, returning the node addresses whose names resolve.
    [[nodiscard]] std::vector<std::uintptr_t> WalkChain(std::uintptr_t head,
                                                       std::size_t name_offset,
                                                       std::size_t next_offset,
                                                       std::size_t max_nodes) const;

    const NamePool*  names_{nullptr};
    ReflectionLayout layout_{};
};

} // namespace mpe::unreal
