#pragma once

#include <cstdlib>

#include "formats/bhav.hpp"
#include "formats/simantics.hpp"
#include "formats/strings.hpp"
#include "formats/xform.hpp"
#include "animation.hpp"
#include "locomotion.hpp"
#include "dialog.hpp"
#include "path_planner.hpp"
#include "tutorial.hpp"
#include "world.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace os2 {
class DbpfPackage;
struct Lot;
class ResourceIndex;
}

namespace os2::vm {

struct WantSystem; // wants.hpp

// Group of the global BHAVs/BCONs (opcodes 0x100-0xFFF, in objects.package).
constexpr uint32_t kGlobalGroup = 0x7FD46CD0;
constexpr uint32_t kTypeBhav = 0x42484156;
constexpr uint32_t kTypeBcon = 0x42434F4E;
constexpr uint32_t kTypeGlob = 0x474C4F42;
constexpr uint32_t kTypeTtab = 0x54544142;
constexpr uint32_t kTypeObjf = 0x4F424A66;
constexpr uint32_t kTypeTtas = 0x54544173;

// Group of the semi-global library a GLOB resource names: CRC24 of the
// lowercased name with the high byte 0x7F (holds for every GLOB in the game).
uint32_t semiGlobalGroup(std::string_view name);

constexpr uint32_t kTypeObjd = 0x4F424A44;
constexpr uint32_t kTypeStr = 0x53545223;
constexpr uint32_t kTypeSlot = 0x534C4F54;

// STR# with the tree names Run Tree by Name refers to ("Call Named Tree prim
// string set"), in object, semi-global and global groups.
constexpr uint16_t kNamedTreeStrings = 0x12F;
// STR# of an object group with its mesh group and material names.
constexpr uint16_t kMeshGroupStrings = 0x87;
constexpr uint16_t kMaterialStrings = 0x88;

// Sizes of the engine-defined variable sets. The base 1.0's were 0x58 (88),
// 0x36 (54) and 0xAA (170), which is also what the game's own SimAntics editor
// labels (objects.package, group 0x7FE59FD0: STR# 0x81 global labels, 0x8D
// object data, 0xC8 person data, 0x86 motives) — and what this port pinned
// until the packs' trees showed they read past them (the AL's `bhav-sweep` of
// 18/10: 460 "global N out of range" — 456 + 4 in one-tick trees — 1.882 of
// person data and 264 of object data, over 54/88/170-entry arrays). The BV-era
// data owners check 0x71 (113), 0x3A (58) and 0xCC (204) (cTSObject::InterpValue
// BV 0xb15f2c / cTSPerson::InterpValue BV 0xad56ec, docs/primitives-mac.md
// §4j), and the AL goes one past each: raising the sizes until the same sweep
// was clean gives the largest accesses of its trees — object data 113, global
// 59, person data 230 — so the arrays hold 114, 60 and 231 (the OpenTS2
// reimplementation sizes its object data and globals at 114 and 60 too). The
// AL's own compiled limits are ≥ these (no AL binary read here).
// A saved state keeps its own count per version (lotObjectDataCount /
// lotPersonDataCount), applied to these arrays.
// Temps: 8 per object (cTSObject +0x8A; InterpValue's owner 8 takes 0-7 and
// gives error 0x02 past them, Mac base 1.0 and BV; no BHAV of the game uses
// temp 8 or above).
constexpr size_t kTempCount = 8;
constexpr size_t kObjectDataCount = 114;
constexpr size_t kGlobalCount = 60;
constexpr size_t kPersonDataCount = 231;
constexpr size_t kMotiveCount = 16;
constexpr size_t kMaxStackDepth = 1000; // frames past it: "Max stack depth exceeded" (cTSTreeSim::Gosub BV 0xe1a428)
// The nodes a thread may run in one turn before it fails as an infinite loop:
// the one value for the app (LotVM), the VM's own default and every s2pkg
// command (cli/s2pkg/budget.hpp). The binary's limit: cTSTreeSim::DoNodeAction
// (BV 0xe1b18c) raises 0x44D "Too many iterations" past DAT_0277ea94 = 100000
// (docs/routing-mac.md §7.1, docs/primitives-mac.md §4g).
constexpr size_t kNodeLimitPerTurn = 100000;
// Attributes and semi-attributes have a fixed count (Mac binary, the
// cTSObject constructor BV 0xb0c7a8): the OBJD's "num attributes" (field
// 0x3A, byte 0xB4) but at least 8, and the number of the semi-global's
// attribute labels (STR# 0x100); an index past them is error 0x06. A saved
// object keeps its saved attribute count (cTSObject::Read).
constexpr size_t kMinAttributes = 8;
constexpr size_t kMaxAttributes = 256;

// Object data fields the engine maintains or reads (names as in STR# 0x8D).
// An object model's footprint tile (the CRES "footprint" data list's
// 32-byte 16x16 mask, a u16 a row, bit 0 the low x) halved to 8x8 cells of
// 1/8 tile (a cell is set when any of its four is; BV 0x6537a4), and each
// row's run of cells as a rectangle x0, y0, x1, y1 in the model's space,
// about the tile (tx, ty)'s centre, shrunk by 0.01 (BV 0x656cc0).
std::array<uint8_t, 8> halveFootprintMask(std::span<const uint8_t> mask);
std::vector<std::array<float, 4>> footprintMaskRects(const std::array<uint8_t, 8>& rows, int tx, int ty);
// GetFootprint's snapping of a world rectangle (x0, y0, x1, y1) to the 1/8
// tile grid (BV FUN_0191fa14, base FUN_00b43bfc): x0' = max(x0, floor(8 x0
// + 0.5) / 8 + 0.01), x1' = min(x1, ceil(8 x1 - 0.5) / 8 - 0.01), y alike;
// a non-empty side left under 1/16 grows to 0.105 from its low end.
std::array<float, 4> snapFootprintRect(const std::array<float, 4>& r);

namespace objdata {
constexpr int16_t Direction = 1;
constexpr int16_t ContainerId = 2;
constexpr int16_t SlotNumber = 3;
constexpr int16_t Flags = 8; // bit 0 can be stepped over, 2 allow object and person intersection, 4 allow person intersection (STR# 0x8E)
constexpr int16_t ObjectId = 11;
constexpr int16_t LeadTileObjectId = 19;
constexpr int16_t Room = 29;
constexpr int16_t HideInteraction = 50;
constexpr int16_t Category = 59;
constexpr int16_t SlotCount = 67;
constexpr int16_t SemiAttributeCount = 79;
} // namespace objdata

// Person data fields the engine reads (names as in STR# 0xC8).
namespace persondata {
constexpr int16_t NeighborId = 31;
// "NPC Type" (STR# 0xC8, the label the Lua scripts' PersonData table gives 155):
// the pool of NPCs of the AL reads it (docs/lua-scripts.md).
constexpr int16_t NpcType = 155;
// "route status": 1 while the person's route is on (base 1.0 router:
// docs/primitives-mac.md §4j); the shoo trees only shoo who has 0.
constexpr int16_t RouteStatus = 39;
constexpr int16_t PersonAge = 58;
constexpr int16_t FamilyNumber = 61;
} // namespace persondata

// Globals the engine reads or writes (names as in STR# 0x81).
namespace global {
constexpr int16_t Hour = 0;
constexpr int16_t DayOfMonth = 1;
constexpr int16_t Minute = 5;
constexpr int16_t Second = 6;
constexpr int16_t Month = 7;
constexpr int16_t Year = 8;
constexpr int16_t TimeOfDay = 4;     // 0 day, 1 dusk, 2 night, 3 dawn (STR# 0xE1), from the hour
constexpr int16_t DaysRunning = 28;
constexpr int16_t MaxDayNumber = 29;
constexpr int16_t DayOfWeek = 34;    // Days Running % 7
constexpr int16_t SelectedPersonId = 3;
constexpr int16_t CurrentFamily = 9;
constexpr int16_t Speed = 16;
constexpr int16_t Paused = 17;
constexpr int16_t HouseRadioStation = 31;
constexpr int16_t CurrentHouse = 10;
constexpr int16_t BudgetMod10000 = 13; // the current family's funds % 10000
constexpr int16_t BudgetDiv10000 = 14; // and / 10000
constexpr int16_t CurrentNeighborhoodId = 40;
constexpr int16_t LotZoning = 41;
constexpr int16_t InteractionGuid = 36; // "Current Interaction GUID": the next interaction ID (links.cpp)
} // namespace global

// Families of 0x7FD9 and above are the reserved ones ("G - Reserved Family
// IDs": NPCs, townies); a family between 1 and 0x7FD9 is a real one
// (cTSFamilyManager: MakeNewFamily gives a number below it the lowest free
// number from 1; AddToFamily copies clothes only between real families).
constexpr int16_t kReservedFamilies = 0x7FD9;

// A Generic Sims Call (or other primitive) that asks the engine for something
// outside the simulation: camera, UI, loading, lot transitions, ... It is
// recorded for the host to act on, with the state the call reads its
// arguments from (STR# 0xDC documents them in the temps).
struct EngineRequest {
    uint8_t call = 0; // Generic Sims Call index (STR# 0xDC)
    int16_t me = 0;
    int16_t stackObject = 0;
    std::array<int16_t, kTempCount> temps{};
};
// Requests the host has not taken yet; the oldest are dropped past this.
constexpr size_t kMaxEngineRequests = 1024;

// A sound the trees asked for (Play / Stop Sound Event, Stop ALL Sounds;
// vm/sound.hpp), for the host to play from `source`'s position.
struct SoundRequest {
    enum class Kind : uint8_t { Play, Stop, StopAll } kind = Kind::Play;
    int16_t source = 0;  // object the sound comes from
    std::string event;   // name from the Sound ID Table (STR# 0x4132)
    bool loop = false;
    float volume = 1;
    uint8_t flags = 0;   // operand byte 4, as is
};
constexpr size_t kMaxSoundRequests = 256;

// Object arrays (Array Operation, data owners 0x29-0x2E): as many as the
// OBJD's field 0x3B (at most 256), made empty with the object (BV 0xb0c7a8);
// another number is error 0x6C "Bad Object Array". This bounds their length.
constexpr size_t kMaxObjectArrays = 256;
constexpr size_t kMaxArrayLength = 4096;

// Relationship variables grow when written past their end; this bounds that
// (the trees use indices up to 11).
constexpr size_t kMaxRelationshipVars = 32;

// Object types (OBJD "type", STR# 0xF6).
constexpr uint16_t kObjectTypePerson = 2;
constexpr uint16_t kObjectTypeSupport = 5; // cTSObject::IsSupport (BV 0xaff018): columns
constexpr uint16_t kObjectTypeDoor = 8;
constexpr uint16_t kObjectTypeWindow = 9;
constexpr uint16_t kObjectTypeStairs = 10;
constexpr uint16_t kObjectTypeModularStairs = 11;
constexpr uint16_t kObjectTypeModularStairsPortal = 12;
constexpr uint16_t kObjectTypeVehicle = 13;

// A Sim of the neighborhood, on this lot or not, known by its neighbor ID.
// The neighborhood keeps a copy of its person data (cTSNeighbor has a person
// data array); while the Sim is on the lot, its object's person data is used.
struct Neighbor {
    uint32_t guid = 0; // of the Sim's OBJD
    int16_t house = 0; // house (lot) number; 0 = none
    std::array<int16_t, kPersonDataCount> personData{};
    // Outfit keys of the Sim's character file (category bit << 16 | part;
    // see hood_bridge.hpp), for Generic Sims Call 32.
    std::vector<uint32_t> outfits;
    std::string firstName, lastName; // for the dialogs' $ tokens
    // The SDNA property set (genetics.hpp): genes a baby inherits.
    std::map<std::string, std::string> dna;
    // A Sim born in play (Make New Character): its parents' neighbor IDs,
    // for the family tree the save writes (hood_save.hpp).
    int16_t parentA = 0, parentB = 0;
};

// Inventories (Manage Inventory) and their tokens grow on demand; the game
// adds no singular token past 0x7FFF (cTSInventory::AddSingularToken, BV
// 0x496e68); properties are bounded here.
constexpr size_t kMaxInventoryTokens = 0x7FFF;
constexpr size_t kMaxTokenProperties = 256;

// An inventory token as cTSInventoryToken keeps it: an object GUID, flags
// (eInventoryTokenFlags; bit 0 visible, 1 memory, 2 marked for deletion, 3
// shopping: cEdithLanguage::CatManageInventoryMatchFlags BV 0x4b2d24) and a
// list of properties. Singular tokens are memories, skills, wants, food
// units, ...; a counted token (gifts, outfits, games) keeps its count and
// category as properties 0 and 1 (cTSInventory::AddCountedToken BV 0x496ac4).
struct InventoryToken {
    uint32_t guid = 0;
    int16_t flags = 0;
    std::vector<int16_t> properties;
};
namespace tokenflag {
constexpr int16_t Visible = 0x01, Memory = 0x02, MarkedForDeletion = 0x04, Shopping = 0x08;
}
inline int16_t countOf(const InventoryToken& t) { return t.properties.empty() ? int16_t(0) : t.properties[0]; }
inline int16_t categoryOf(const InventoryToken& t) { return t.properties.size() < 2 ? int16_t(0) : t.properties[1]; }

// Inventories are global, of a lot ("house"), of a family or of a neighbor
// (the order of "Which Inventory(G = 0, H, F, N)" in the editor's labels,
// STR# 0xFC), each identified by an ID except the global one.
enum class InventoryKind : uint8_t { Global, Lot, Family, Neighbor };

struct Inventory {
    std::vector<InventoryToken> singular;
    std::vector<InventoryToken> counted;
};

// The token Manage Inventory reads from an inventory, with where it came from,
// so its properties can be changed and saved back. Fields as data owners
// 0x27/0x28 see them (cTSObject +0x1A0.., InterpValue BV 0xb15f2c), labelled
// in STR# 0xFC: 0 inventory kind, 1 inventory ID, 2 singular (0) or counted,
// 3 token index (from 0, as the game keeps it: TryInventoryAction BV
// 0xe26a40), 4-5 GUID low/high word, 6 flags, 7 property count, 8 property
// index, 9 property value (at that index; ours: in the game 8 and 9 are the
// pointer to the token object). Fields 4-7 are raw copies (cTSObject
// +0x1A8..+0x1AE, saved with the object; docs/save-mac.md §8): every
// Manage Inventory read of a token copies its GUID, flags and property
// count there, writing them changes nothing else, and the singular
// operation 11 gives the copy it adds the flags of field 6.
struct TempToken {
    InventoryToken token;
    bool loaded = false;
    InventoryKind kind = InventoryKind::Global;
    int16_t inventoryId = 0;
    bool counted = false;
    int16_t index = 0; // from 0
    std::array<int16_t, 4> copied{}; // fields 4-7
    int16_t propertyIndex = 0;
    // What a Manage Inventory read leaves in fields 4-7.
    void copyFields() {
        copied = {int16_t(token.guid & 0xFFFF), int16_t(token.guid >> 16), token.flags,
                  int16_t(token.properties.size())};
    }
};

struct VMError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// BHAVs and BCONs by (group, instance), read on demand from a ResourceIndex
// and/or added directly (tests). Returned pointers stay valid.
class CodeLibrary {
public:
    CodeLibrary() = default;
    explicit CodeLibrary(ResourceIndex* index) : index_(index) {}
    ResourceIndex* index() const { return index_; }

    const Behavior* behavior(uint32_t group, uint16_t instance);
    const Constants* constants(uint32_t group, uint16_t instance);
    const ObjectDefinition* definition(uint32_t group, uint16_t instance);
    const StringTable* strings(uint32_t group, uint16_t instance);
    // SLOT (group, instance): an object's slots, named by its OBJD "slots id".
    const Slots* slots(uint32_t group, uint16_t instance);
    // Instance of the BHAV of `group` with this file name; the first call reads
    // the name of every BHAV.
    std::optional<uint16_t> behaviorByName(uint32_t group, const std::string& name);
    // The ObjectScripts' Lua 5.0 chunks (docs/lua-scripts.md), by script name:
    // the EPs' resources (0x9012468B, the `.objLua`, and 0x9012468A, the
    // `.globalObjLua`) hold "u32 0, u32 length, the name, then the chunk". Their
    // TGI hashes the name in a way this port's nameInstanceHash does not
    // reproduce, so the first call scans the index once and maps the names.
    // luaPreludes() is the `.globalObjLua` set (Global first, then by name): what
    // the game loads before a script.
    const std::vector<uint8_t>* luaScript(const std::string& name);
    const std::vector<std::vector<uint8_t>>& luaPreludes();
    // The file names of luaPreludes(), in the same order (the log's "prelude
    // <name> failed").
    const std::vector<std::string>& luaPreludeNames();
    // OBJD (group, instance) with this GUID; the first scan reads every OBJD.
    // A GUID none has goes to the fallback once (the neighbourhood's
    // character files: every neighbor's Sim has its definition in the game,
    // loaded or not; loadLot sets it).
    std::optional<std::pair<uint32_t, uint16_t>> definitionByGuid(uint32_t guid);
    // A neighbor's definition (the same lookup; owner 0x26 and the sweep).
    std::optional<std::pair<uint32_t, uint16_t>> characterDefinition(uint32_t guid);
    using GuidFallback = std::function<std::optional<std::pair<uint32_t, uint16_t>>(CodeLibrary&, uint32_t guid)>;
    void setGuidFallback(GuidFallback f) { guidFallback_ = std::move(f); }
    // The OBJDs of type 7 with field 42 "is global sim object" & mask set, in
    // the scan's order, with their field 26 "Ignore GlobalSim field in CAS
    // Lot" (cEdithObjectModule::UpdateSimObjects, docs/game-flow-mac.md §7.1).
    struct GlobalSimObject {
        uint32_t group = 0;
        uint16_t instance = 0;
        uint32_t guid = 0;
        bool notInCas = false;
    };
    std::vector<GlobalSimObject> globalSimObjects(uint16_t mask = 1);
    // The object definitions in the order the GUID scan finds them (the
    // first OBJD of each GUID, as the rest of the index), with OBJD fields
    // 75 and 76. The Apartment Life content uses those two "unused" fields
    // as the selector category and subcategory ("Stereo - RadioStation -
    // Pop" is 117/4, "Accessory - ..." 153/0-4, "Token - Badge - ..." 121/1-
    // 11), and Set to Next 38 walks this list by index.
    struct DefinitionRef {
        uint32_t guid = 0;
        int16_t category = 0;
        int16_t subcategory = 0;
    };
    const std::vector<DefinitionRef>& definitions();
    // OBJf with the same (group, instance) as an OBJD: the object's function table.
    const ObjectFunctions* functions(uint32_t group, uint16_t instance);
    // OBJD instance of the master of a multi-tile object of `group` (the OBJD
    // with that master ID and sub index -1), read with the GUID scan.
    std::optional<uint16_t> masterDefinition(uint32_t group, uint16_t masterId);
    // OBJD instances of the parts of that multi-tile object (same master ID,
    // sub index other than -1), by instance.
    std::vector<uint16_t> parts(uint32_t group, uint16_t masterId);
    // OBJD (group, instance) with this file name ("Trash Can - Outside"); the
    // first OBJD of a name, as for GUIDs.
    std::optional<std::pair<uint32_t, uint16_t>> definitionByName(const std::string& fileName);
    // TTAB (group, instance), null if missing or unreadable, and its strings
    // (the TTAs with the same group and instance).
    const InteractionTable* interactions(uint32_t group, uint16_t instance);
    const StringTable* interactionStrings(uint32_t group, uint16_t instance);
    // Semi-global group the objects of `group` use (from their GLOB), 0 if none.
    uint32_t semiGlobalFor(uint32_t group);
    // Number of (English) labels in the group's STR# 0x100: the attribute
    // labels of an object group, the semi-attribute labels of a semi-global one.
    size_t attributeLabelCount(uint32_t group);

    void add(uint32_t group, uint16_t instance, Behavior b);
    void add(uint32_t group, uint16_t instance, Constants c);
    void add(uint32_t group, uint16_t instance, ObjectDefinition d);
    void add(uint32_t group, uint16_t instance, StringTable t);
    void add(uint32_t group, uint16_t instance, Slots s);
    void add(uint32_t group, uint16_t instance, ObjectFunctions f);
    void add(uint32_t group, uint16_t instance, InteractionTable t);
    void addInteractionStrings(uint32_t group, uint16_t instance, StringTable t);
    void setSemiGlobal(uint32_t group, uint32_t semiGroup) { semiGlobals_[group] = semiGroup; }
    void setAttributeLabelCount(uint32_t group, size_t n) { labelCounts_[group] = n; }
    // Adds the code and definitions of a package's own group (0xFFFFFFFF, as
    // in a Sim's character file) as `group`: the index cannot tell those
    // apart, since every such package uses the same group. Its OBJDs take
    // over their GUIDs. Returns the OBJD instances it added.
    std::vector<uint16_t> addPackage(DbpfPackage& package, uint32_t group);
    // Gives an OBJD added here another file name, in place (a character made
    // in memory takes the name of the file written for it: "N001_User00171 -
    // Bebê", which the lot's cPerson names). False when there is none.
    bool renameDefinition(uint32_t group, uint16_t instance, const std::string& fileName);

private:
    // A null entry records a resource known to be missing.
    template <class T>
    using Cache = std::unordered_map<uint64_t, std::unique_ptr<T>>;

    ResourceIndex* index_ = nullptr;
    GuidFallback guidFallback_;
    std::set<uint32_t> guidFallbackTried_;
    Cache<Behavior> behaviors_;
    Cache<Constants> constants_;
    Cache<ObjectDefinition> definitions_;
    Cache<StringTable> strings_;
    Cache<Slots> slots_;
    Cache<ObjectFunctions> functions_;
    Cache<InteractionTable> interactions_;
    Cache<StringTable> interactionStrings_;
    std::vector<std::pair<GlobalSimObject, uint16_t>> globalSims_; // with field 42
    std::map<std::pair<uint32_t, uint16_t>, uint16_t> masters_;
    std::map<std::pair<uint32_t, uint16_t>, std::vector<uint16_t>> parts_;
    std::unordered_map<std::string, std::pair<uint32_t, uint16_t>> byFileName_;
    std::map<uint32_t, std::vector<uint16_t>> packages_; // addPackage
    void indexDefinition(uint32_t group, uint16_t instance, const ObjectDefinition& d);
    void scanDefinitions();
    std::unordered_map<uint32_t, std::unordered_map<std::string, uint16_t>> byName_;
    bool namesScanned_ = false;
    void scanLuaScripts();
    bool luaScanned_ = false;
    std::unordered_map<std::string, std::vector<uint8_t>> luaScripts_;
    std::vector<std::vector<uint8_t>> luaPreludeChunks_;
    std::vector<std::string> luaPreludeNames_;
    std::unordered_map<uint32_t, uint32_t> semiGlobals_;
    std::unordered_map<uint32_t, size_t> labelCounts_;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint16_t>> byGuid_;
    std::vector<DefinitionRef> definitionList_; // first GUID scan order
    bool guidsScanned_ = false;
};

using ObjectId = int16_t;

// An interaction in a Sim's queue: pushed by Push Interaction or picked
// from the pie menu (interactions.hpp has the queue logic). The entry is
// resolved from the TTAB of the object whose interaction it is when it is
// queued.
struct QueuedInteraction {
    ObjectId object = 0;      // whose interaction (TTAB) it is (the callee; also the stack object and code owner)
    uint16_t interaction = 0; // TTA index of the TTAB entry: the number Push Interaction and the pie menu use
    int16_t priority = 0;     // kPriority* (interactions.hpp)
    uint8_t flags = 0;        // Push Interaction's flags byte (0 from the pie menu)
    ObjectId pushedBy = 0;    // the object whose thread pushed it (the Sim itself from the pie menu)
    // The interaction's target (cTSInteraction +0x10, GetTargetObject): the
    // object, unless Push Interaction's flag 0x01 named the object in a local
    // (also the icon); the other Sim of a Sim-to-Sim animation (0 = the object).
    ObjectId target = 0;
    std::array<uint8_t, 16> operands{}; // Push Interaction's whole operand, for what is not understood yet
    // From the TTAB entry.
    uint16_t action = 0, guard = 0; // trees, resolved against the object's code
    uint32_t ttabFlags = 0;         // flags | flags2 << 16
    std::string name;               // TTAs string (Brazilian Portuguese), "Sub.../Item" for submenus
    std::array<int16_t, 4> args{};  // the action tree's parameters
    uint16_t uid = 0;               // unique across Sims (links.cpp; global 36), for cancelling a specific entry; saved at +0x40
    uint16_t link = 0;              // the interaction it is linked to (Push Interaction flag 0x40), or 0
    // Two bits of the binary's cTSInteraction +0x54 (saved at +0x54;
    // docs/primitives-mac.md §4g "Rodada n31"): `autonomous` (0x400,
    // vtable 0xbc/0xc0), which SetCurrentAction copies into person data 166,
    // set for the Find Best Interaction's entry (Gosub Found Action, Push
    // Interaction byte 14 bit 0x02) and inherited from the pushing Sim's
    // current interaction otherwise; `testAutonomous` (0x800, vtable
    // 0xc4/0xc8), the check tree's parameter 0 when Idle for Input takes the
    // entry from the queue, set only for the Find Best Interaction's entry.
    bool autonomous = false;
    bool testAutonomous = false;
    // +0x54 bit 0x4000 (vtable 0x90/0x8c), Push Interaction byte 14 bit
    // 0x10: when it ends, the Idle for Input that ran it does not start a
    // "continue as current" entry after it (no base-game tree sets it).
    bool noContinuation = false;
    // Cancelled by the user or by something more important queued behind it:
    // Idle for Input in its action tree returns at once (FreeSO's NotifyIdle).
    bool cancelled = false;
};
constexpr size_t kMaxQueuedInteractions = 64;

// A pie menu entry a check tree adds (Add/Change the Action String), with
// the stack object at that moment: the action tree gets it as parameter 0.
// As the Mac binary makes it (cEdithObjectTestSim::sMakeNewMenuItem), the
// parameters are {stack object, temp 0} and, for autonomy, the check tree
// ads at that moment (Bon Voyage can also pass temps 4 and 5).
struct ActionString {
    std::string name;
    ObjectId stackObject = 0;
    std::array<int16_t, 4> params{};
    std::array<int16_t, kMotiveCount> adMin{}, adRange{}, adPersonality{};
};

// A motive change Set Motive Change (0x1D) put on a Sim: the motive moves
// by `perTick` each tick until it reaches `limit` (motives.hpp).
struct MotiveChange {
    int16_t motive = 0;
    float perTick = 0;
    float limit = 0;
};

// State only Sims have.
// cITSPerson::cMemoryInfo (0x1C bytes): a visible memory (memories.hpp).
struct MemoryInfo {
    uint32_t guid = 0;      // the memory object
    uint32_t subject = 0;   // property 6 << 16 | property 5
    int32_t kind = 0;       // property 0
    int32_t current = 0;    // property 10
    int32_t initial = 0;    // property 7
    int32_t final = 0;      // property 8
    int32_t aspiration = 0; // property 11
    bool operator==(const MemoryInfo&) const = default;
};

struct Person {
    std::array<int16_t, kPersonDataCount> data{};
    std::array<int16_t, kMotiveCount> motives{};
    // The motive engine (motives.cpp): the changes running, and the motives
    // with their fractions (the game keeps them as floats) as last written;
    // the old set (cTSMotives +0x48: the current one as the last 60-tick
    // update left it, saved in the PERS).
    std::vector<MotiveChange> motiveChanges;
    std::array<float, kMotiveCount> exactMotives{};
    std::array<int16_t, kMotiveCount> writtenMotives{};
    bool exactMotivesValid = false;
    std::array<float, kMotiveCount> oldMotives{};
    bool oldMotivesValid = false;
    // The last Find Best Interaction's choice, for Gosub Found Action
    // (autonomy.cpp); its object and entry are also in person data 53 and
    // object data 20.
    std::array<int16_t, 4> foundActionArgs{};
    std::string foundActionName; // a dynamic interaction's own name, or empty
    // What autonomy did lately (autonomy.cpp): the interactions Gosub Found
    // Action ran, newest last, and whether their action tree returned false.
    struct AutonomyMemory {
        ObjectId object = 0;
        uint16_t interaction = 0;
        uint64_t tick = 0;
        bool failed = false;
    };
    std::vector<AutonomyMemory> autonomyMemory;
    // The memories as last read (cTSPerson +0x8F4, memories.hpp), whether
    // they were, and the tick of the last 60-tick update of the motives and
    // memories (+0x2E4, motives.cpp).
    std::vector<MemoryInfo> memories;
    bool memoriesRead = false;
    uint64_t memoriesTick = 0;
    // The object records (cTSPerson +0x60c, cObjectRecord; records.cpp,
    // docs/primitives-mac.md §4j): one for each tree of an object that a
    // GosubObjectTree pushed on the person's tree stack (VM::call: the
    // action and sub-action trees, Run Tree by Name, Run Functional Tree, the
    // router's trees), with the object, the stack's depth with that frame
    // and Run Functional Tree's "change icon" flag. Each adds 1 to the
    // object's data 62 "use count" while its frame is on the stack. `thread`
    // is the Thread::serial that pushed it (a one-tick tree runs on the
    // same stack in the game, on a thread of its own here); not saved.
    struct ObjectRecord {
        ObjectId object = 0;
        int16_t depth = 0;
        bool flag = false;
        uint64_t thread = 0;
    };
    std::vector<ObjectRecord> objectRecords;
    // The number of records when the current action (+0x646) and sub-action
    // (+0x644) began (SetCurrentAction, SetCurrentSubAction): the action
    // ends when its record goes (one fewer), the sub-action when the count
    // is back to it. Saved in the PERS.
    uint16_t recordsAtAction = 1, recordsAtSubAction = 0;
    // The GUID the empty interaction got when it became the current action
    // (SetCurrentAction gives one to an interaction without: the action
    // ended, the reset), which person data 109 then holds. Not saved.
    uint16_t emptyActionGuid = 0;
};

// Where an object is in the lot, as the lot's saved state has it: the tile
// position (tile centres at .5), the level (1 = the ground) and the
// direction in object data 1. Objects out of the world (controllers, ones
// in a slot or an inventory) are at (-1, -1).
struct Position {
    float x = -1, y = -1;
    int level = 0;
    // Set by Build mode for what it puts off the floor (the modular stairs'
    // steps, docs/buy-build-mac.md §7.12): the height of the object in the
    // lot (tiles, as the elevations) and the saved state's "elevation";
    // kNoHeight leaves both to the floor (and the state as it was read).
    static constexpr float kNoHeight = -1000.0f;
    float height = kNoHeight;
    float elevation = 0;
    bool inWorld() const { return x >= 0 && y >= 0; }
    bool hasHeight() const { return height > kNoHeight; }
};

// A timer On Timer (0x7A) set on an object (cTSObject::cObjectTimerDetail,
// set by cTSObject::AddTimer BV 0xb05448, saved by WriteTimers; timer.cpp,
// docs/save-mac.md §8): when `ticks` runs out the event tree runs on the
// object with the setter as stack object; a repeating timer whose tree
// returned true starts over with `period`, any other is removed.
struct ObjectTimer {
    int16_t id = 0;     // from the simulator's GUID counter (global 36, as the interactions' IDs)
    uint16_t tree = 0;  // call opcode, resolved against the object's code
    int32_t ticks = 0;  // left
    int32_t period = 0;
    uint32_t flags = 0; // bit 0 repeat, bit 1 "reset timer ticks" of the last Modify
    ObjectId object = 0; // the object whose thread set it: the event tree's stack object
    std::array<int16_t, 8> params{};
    bool repeats() const { return flags & 1; }
};
constexpr size_t kMaxTimersPerObject = 64;

struct Object {
    ObjectId id = 0;
    // Which object of that ID this is (VM::createObject counts them): IDs
    // are reused, and a thread of a removed object must not go on as the
    // next object given its ID.
    uint64_t serial = 0;
    // Its place in the object module's list (cEdithObjectModule; the order
    // the lot's ObjM saves): Set to Next walks the objects in this order.
    // A loaded object takes its place in the saved list; one made in play
    // goes to the front (VM::createObject; inferred from the saved lists,
    // which start with the newest IDs).
    int64_t listOrder = 0;
    // Its place in the vectors of people and of each definition's instances
    // (Set to Next 1 and 4/18 walk those): a loaded object at its place in
    // the saved list, one made in play after all (appended; inferred).
    int64_t appendOrder = 0;
    uint32_t group = 0;     // private BHAVs/BCONs (the group of the object's OBJD)
    uint32_t semiGroup = 0; // semi-global BHAVs/BCONs, 0 if none
    const ObjectDefinition* definition = nullptr; // shared by every instance; may be null (tests)
    uint16_t definitionInstance = 0;              // of the OBJD (and of its OBJf)
    // Private to each instance: attributes are declared by the object, semi-attributes
    // by its semi-global library (both labelled in STR# 0x100 of their group).
    std::vector<int16_t> attributes;
    std::vector<int16_t> semiAttributes;
    std::array<int16_t, kObjectDataCount> data{};
    // Kept next to `data`: the scans over every object read them (cache lines).
    Position position;
    // The room last told to the object's "room changed" entry point
    // (cTSObject +0x1CC, -1 from the constructor, never saved), and the
    // portal's two rooms last told (+0x2B4/+0x2B6): vm/rooms.cpp onRoomChanged.
    int16_t notifiedRoom = -1;
    std::array<int16_t, 2> portalRooms{-1, -1};
    // On Timer's timers, in the order they were set.
    std::vector<ObjectTimer> timers;
    std::optional<Person> person;
    // Object arrays, each with an iterator index (data owner 0x29; saved
    // after the arrays, cTSObject +0x194: -1 in every array of the game's
    // lots never walked, so -1 before a tree sets it).
    struct Array {
        std::vector<int16_t> values;
        int16_t iterator = -1;
    };
    std::vector<Array> arrays;
    Array& array(int idx); // error 0x6C past the object's arrays
    // Materials set by Change Material: mesh group name ("" = every group) -> material name.
    std::map<std::string, std::string> materials;
    // The design choice the Design tool gave it (subset -> material, the
    // cObject's list, formats/design_catalog.hpp); none: the saved one.
    std::optional<std::map<std::string, std::string>> design;
    // Data owners 0x27 (mine) and 0x28 (the stack object's).
    TempToken tempToken;
    // Interactions queued for this object (a Sim, usually), in the order they
    // will run; `activeInteraction` is the index of the one running (-1: none).
    std::vector<QueuedInteraction> queue;
    int activeInteraction = -1;
    // Nested interactions (TTAB flag 0x8000) wait in the sub-queue until an
    // Idle for Input in "Handle Sub Queue Interactions" mode runs them as the
    // current sub-action (queue.cpp).
    std::vector<QueuedInteraction> subQueue;
    std::optional<QueuedInteraction> subAction;
    // Notify the Stack Object out of Idle: its next Idle for Input returns at once.
    bool idleInterrupt = false;
    // The route the object is walking (Go To Routing Slot, Go To Relative
    // Position), while it walks: the points still ahead (the next first; tile
    // centres, then the goal point) on its level, the facing at the end (-1:
    // as it arrives) and the object it walks to. The instruction that walks
    // it keeps yielding until the last point is reached; it is kept here for
    // the renderer to animate the walk.
    //
    // On stairs the route holds the climb's phases: points where the walker
    // is put (the animation's anchor, `z` high) to play a stair animation
    // for `ticks` ticks, as the game snaps a Sim into each step in turn
    // (Stair, world.hpp); `climb` is the phase playing.
    struct Route {
        struct Point {
            float x = 0, y = 0;
            int level = 0;
            // A climb phase (ticks > 0).
            int ticks = 0;
            float z = 0;
            int direction = -1;
            std::string animation; // empty with ticks > 0: a wait (for a door to open)
            ObjectId door = 0;     // the door whose portal this point goes through
            // The portal tree (OBJf 15 of `portalOwner`) runs on the walker's
            // thread here, with `portal` as its stack object (routing_walk.cpp);
            // `viaPortal` points are the engine's own way through, skipped
            // when the tree did it.
            ObjectId portal = 0, portalOwner = 0;
            std::array<int16_t, 10> portalParams{};
            bool viaPortal = false;
            // A turn in place (ticks > 0): 45-degree steps, counter-clockwise
            // positive; the animation played mirrored (routing_plan.cpp turnPoint).
            int turnSteps = 0;
            bool mirror = false;
        };
        std::vector<Point> points;
        int level = 0; // of the goal
        int facing = -1;
        ObjectId target = 0;
        // For planning again when the way is blocked (routing_walk.cpp): the goal
        // points (x, y, cost) with their facings, the objects whose
        // footprints do not count, how long the walker has been blocked, how
        // often it planned again, and whether it stands waiting (a door, a
        // Sim in the way: no walk animation).
        std::vector<std::array<float, 3>> goals;
        std::vector<int> facings;
        std::vector<int16_t> multi; // the goals' multi slot indices (person data 38), when any
        // The route's person data (UpdatePersonDataFields): 37 its routing
        // slot (WorkOutSlotIndex: the slot's number among the target's
        // routing slots, -1 none; the binary adds 1000 for a global slot,
        // not done: routing_walk.cpp walkStep) and 38 the current goal's
        // multi slot index (-1 without one).
        int16_t slotIndex = -1, goalMulti = -1;
        std::vector<ObjectId> chairs; // the goals' chairs (sitting goals), when any
        ObjectId chair = 0;           // the planned goal's chair: the post-route tree's parameter 0
        std::vector<ObjectId> ignore;
        int blockedTicks = 0, replans = 0;
        // Unused since round n30 (the routers' places below), kept in the
        // saved thread state.
        float blockerX = 0, blockerY = 0;
        bool waiting = false;
        bool failureTrees = true; // "CT - Route Failure Feedback" when it fails
        ObjectId blocker = 0;     // the Sim in the way
        // The blocker the try counter (replans) counts for: the route's own
        // memory (DoBlockedGoalStillAvailable's +0x3e), kept when the way is
        // free again, so blocks by the same Sim go on counting.
        ObjectId lastBlocker = 0;
        // DoBlockedGoalStillAvailable (routing_walk.cpp): the walker steps aside
        // itself on a way of its own (TryGetSelfOutOfWay, once a route), then
        // plans its route again; a pause ("CT - Route Blocked Animation")
        // or "CT - Route Blocked" tree runs; the pauses so far.
        bool aside = false, asideTried = false, pausing = false, blockedTree = false;
        bool blockedTreeRan = false; // "CT - Route Blocked" has run for this route (RouteBlockedTreeHasRun)
        // The Sim the step-aside route gives way to (the binary sets it as
        // that route's blocker, route vtable 0x140, when TryGetSelfOutOfWay
        // pushes it).
        ObjectId asideFrom = 0;
        // The Sim this route asked to move (AskBlockerToMove accepted; route
        // vtable 0x188) and the uid of that move-out-of-way interaction
        // (0x18c); cleared when the asked Sim's route in it ends.
        ObjectId asked = 0;
        uint16_t askedUid = 0;
        // Where each router stood at the last plan (InitRoute: cTSRoute
        // +0x134, Get/SetRouterLastKnownLocation), the walker's own place
        // noted again at each count of DoBlockedGoalStillAvailable.
        std::vector<std::pair<ObjectId, std::array<float, 2>>> places;
        int pauses = 0;
        bool inPortal = false;    // a portal tree runs (routing_walk.cpp)
        // The goal check / along route trees (routing_walk.cpp): which one runs
        // (1 goal check, 2 along route), the tick of the last check and
        // how many path points were walked by then.
        int check = 0;
        uint64_t lastCheck = 0;
        // How many times the way was planned (the route's path changing,
        // cTSRoute +0xd8), and what DoAlongRouteCheckTest (BV 0x470fe4)
        // noted at the last check: that count (+0x50), the walker's room
        // (+0x54), its level (+0x4c, -999 for a new route) and how far it had
        // walked along its locomotion path (+0x48).
        uint32_t plans = 0, checkPlans = 0;
        int checkRoom = -1, checkLevel = -999;
        float checkWalked = 0;
        // Blocked by `blocker` (DoBlocked, routing_walk.cpp): kBlockChecked,
        // the goal check has run and said go on (DoBlockedWaitingForGoalCheckTree
        // -> DoBlockedGoalStillAvailable), so the next step of the walk is the
        // try counter, not a new step test; kBlockPending, DoBlocked waits for
        // the next tick (this tick's goal check was spent). Saved as a byte.
        static constexpr uint8_t kBlockChecked = 1, kBlockPending = 2;
        uint8_t blockCheckDone = 0;
        struct Climb {
            std::string animation; // empty between phases
            uint32_t serial = 0;   // bumped as each phase starts
            float z = 0;
            int ticksLeft = 0;
            int turnSteps = 0; // a turn in place (not a stair: on the floor)
            bool mirror = false;
        } climb;
        bool endTurned = false; // the turn to the final facing was added
        // Kept under a route a callback tree started (Object::outerRoutes):
        // where the walker stood then, and whether it is current again.
        float keptX = 0, keptY = 0;
        int keptLevel = 0;
        bool resumed = false;
        // The locomotion along the floor (routing_loco.cpp locoStep, after
        // cLocomotable::FollowOneStep, docs/routing-mac.md §6.1): a start
        // ("turnAndGo", or a step on a short way), the walk cycle, a stop
        // ("turnAndStop") at the end. A frame moves along the route (the
        // start and the stop over their clip's distance in its length, the
        // walk at the gait's speed) and the walker stands where the clip's
        // root is, offset from it.
        struct Loco {
            enum Kind : uint8_t { None, Start, Walk, Stop };
            Kind kind = None;
            bool moving = false;     // walking on (no start when it goes on)
            std::string animation;   // the clip playing
            uint32_t serial = 0;     // bumped as each clip starts
            bool mirror = false;
            // walkShort's weight in the walk, the start and the stop (the
            // stride blend, blendGaits/strideWeight).
            float strideWeight = 0;
            // The planned stop's short weight (fitStride's v; the binary's
            // fit can give the stop its own weight).
            float stopWeight = 0;
            // How early the next request starts, overlapping the one ending
            // (FollowOneStep: at the old one's EndTicks less 5 ms when the
            // way's beginning, measured up to 0.75, is at most 0.25 long,
            // else 100 ms; docs/anim-mac.md §8.3).
            float overlap = 0.1f;
            bool sidestep = false;   // stepping without turning (CreateStepCycle)
            int tick = 0, ticks = 0; // in a start or a stop
            // VM ticks the phase began before its first tick; -1 for a
            // route's first phase: the request starts now and FollowOneStep
            // updates it to now in that tick (0 ms played), so it stands
            // at the clip's start that tick (docs/anim-mac.md §8.2-8.3).
            float lead = 0;
            // The planned stop's foot (fitStride: the "R" clip), and a short
            // way (start and stop, no cycles between: the stop follows the
            // start at once).
            bool stopOtherFoot = false, shortWay = false;
            // Seconds the drawn clips cross over as this phase begins: the
            // request's fade in for its first clip (233.3 ms), then the
            // request's BlendDistance (+0xec) at the walk's speed between its
            // clips (cLocoCycle::AnimUpdate, cosine; docs/anim-mac.md §8.4).
            float fade = 0.2333f;
            bool crossover = false; // the fade is one of the request's cosine crossovers
            // Halting for a blocker (StopLocomotion with the stop clip): the
            // walk's way still to go to the end of its half cycle before
            // the stop clip, and that clip's foot.
            float haltWalk = 0;
            bool haltOtherFoot = false;
            float haltRoom = 0; // how far its frame may still go (inferred)
            // The request (BeginFollow's): VM ticks it has played and the
            // length of its way (FollowOneStep hands it off as its fade out
            // starts: docs/anim-mac.md §8.3).
            int requestTicks = 0;
            float requestLength = 0;
            float distance = 0;      // its way along the route
            float speed = 0;         // the walk's, tiles per tick
            float fx = 0, fy = 0;    // the frame
            float heading = 0;       // which way the frame faces (radians, 0 = +Y)
            float turn = 0;          // how far the clip turns the body (radians, + counter-clockwise)
            float endTurn = 0;       // where a stop or a step leaves the body, off the frame's way
            float cycle = 0;         // walk cycle portion (0-1)
            float cycleStart = 0;    // the portion the walk clip started at
            float endX = 0, endY = 0; // a stop's last point
            // What plays, blended by weight (the first is `animation`): a
            // start's or a stop's clip and its short one blended by stride,
            // a sidestep's clips, or the weighed walk cycles
            // (docs/routing-mac.md §6.1).
            std::vector<LocoLayer> layers;
            // The frame on the route's curve: `segment` of it, `arc` along
            // that segment, `taken` of the route points it was built
            // through passed.
            std::shared_ptr<const LocoCurve> curve;
            size_t segment = 0, taken = 0;
            float arc = 0;
            bool onCurve = false;
            // Stopping where it is because the way is blocked (the walk
            // interrupted: cActiveAnimLoco::StopLocomotion), by `halted`.
            bool halting = false, haltDone = false;
            ObjectId halted = 0;
        } loco;
        // The instruction walking it: its thread's stack depth, tree and node.
        size_t depth = 0;
        const Behavior* tree = nullptr;
        uint16_t node = 0;
    };
    std::optional<Route> route;
    // The routes of outer routing instructions while a tree they called routes
    // again (cTSRoutingObjectAggregate's route stack, +0x14..+0x24; ProcessGoto
    // works on its top, vtable 0x68): a goal check tree that refreshes a
    // social's way runs "Route - Dynamic w/Lead in" again from inside the
    // walk's goal check. When the inner route ends the outer one is current
    // again (routing_walk.cpp). Not saved (a save resumes the inner one).
    std::vector<Route> outerRoutes;
    // The routing aggregate's goal checks this tick (cTSRoutingObjectAggregate
    // +0x3c, zeroed by its OnPreSimulatorTick, BV 0x56ee34; DoWalking and
    // DoBlocked count it): one goal check a tick (routing_walk.cpp), counted
    // per turn of the walker's thread (Thread::turn). Not saved (a tick's own).
    uint64_t goalCheckTurn = ~uint64_t(0);
    int goalChecks = 0;
    // The walker's "Routing - Eye Attractor" (routing_walk.cpp): the object the
    // along route tree's parameter 5 names, kept on the way ahead.
    ObjectId eyeAttractor = 0;
    // Bumped when a walk ends as its last clip starts to fade out
    // (FollowOneStep's hand-off): the clip plays on where it was in the
    // world while the next one blends in (draw only, not saved).
    uint32_t locoHandOff = 0;
    // A routing slot 0 the engine gave the object (the synthetic slot of a
    // move-out-of-way destination, routing_loco.cpp), instead of its SLOT's.
    std::optional<Slots::Slot> syntheticSlot;
    // A destination the engine made for a Sim (go_here.hpp): flushed once no
    // Sim has it queued, unless held (a pie menu is open on it).
    bool managedDestination = false, destinationHeld = false;
    // What the animation primitives play on it (animation.hpp).
    AnimationState animation;
    // Where the head looks (Look At, view.cpp): the target object, from which
    // VM tick and for how many (0: until changed).
    struct LookAt {
        ObjectId target = 0;
        uint64_t since = 0;
        int32_t ticks = 0;
        // The turn rates in degrees a second (try_look_at, BV 0x6a19f8:
        // byte 4 x 2 or temp 1 towards, byte 5 x 2 or temp 1/2 away; the
        // choreo request takes them x pi/180 as radians a second).
        int16_t towards = 0, away = 0;
        // The request's HoldTicks (the duration in ms; -1: until the next
        // Look At), and byte 0's 0x04 "including Spine" and 0x02 "no early
        // exit" (cChoreoRequestLookAt vtable 0x154 and 0x148, read by
        // cActiveAnimLookAt::AnimUpdate as 0x178 and 0x16c; docs/anim-mac.md
        // §12.3). Which Look At this is: a new one starts a new turn.
        int32_t durationMs = -1;
        bool spine = false, noEarlyExit = false;
        uint32_t serial = 0;
        // The look point (byte 14, index byte 8): 0 the target's box at
        // three quarters of its height; 1 targeting, 2 routing, 3
        // containment = the target's SLOT entries of type 4, 3 and 0 (the
        // content's "CT - Teach - Look Object" trees; view.cpp).
        uint8_t slotKind = 0, slotIndex = 0;
    };
    std::optional<LookAt> lookAt;
    uint32_t lookAtSerial = 0; // Look Ats so far (LookAt::serial)
    // The rate the head turns back at, degrees a second: the last Look At's
    // (STOP's own included: try_look_at sets it on the looks it stops).
    int16_t lookAwayRate = 0;
    // Look At requests this tick (the game allows 25 a Sim a simulator turn).
    uint64_t lookAtTick = 0;
    int lookAtRequests = 0;
    // Reach/Put in progress (reach.cpp): the instruction running it (stack
    // depth, tree, node), the reach animation and its ticks, the tick of its
    // event 0 (when the object changes hands), what moves where, and the
    // grasp/carry overlays it adds on a pick up.
    struct Reach {
        size_t depth = 0;
        const Behavior* tree = nullptr;
        uint16_t node = 0;
        uint8_t mode = 0; // 0 pick up, 1 drop onto an object, 2 drop onto the floor
        std::string animation;
        int tick = 0, transferAt = 0, end = 0;
        bool transferred = false;
        ObjectId object = 0, target = 0;
        int hand = 0, slot = -1;
        TilePos floor{};
        std::vector<std::string> overlays;
    };
    std::optional<Reach> reach;
    // The grasp/carry overlays of what a Sim holds, taken off when it puts it down.
    std::vector<std::string> carryOverlays;
    // The model's bones as last posed (bone name, local transform): the
    // transform nodes the lot saved for it (nTSSG::cObject), then where each
    // Animate Object animation left them (VM::holdObjectPose). Its bone
    // slots are found in this pose, under what plays on it now.
    std::vector<std::pair<std::string, Xform>> pose;
    // Lights as Change Light set them (dialog.hpp).
    ObjectLights lights;

    uint32_t guid() const { return definition ? definition->guid() : 0; }
};

// Multi-tile objects: the parts point at the lead tile's object (object data
// "lead tile object ID"); the lead itself may hold 0 (inferred).
inline ObjectId multiTileGroup(const Object& x) {
    const ObjectId lead = x.data[objdata::LeadTileObjectId];
    return lead ? lead : x.id;
}

struct Frame {
    const Behavior* behavior = nullptr;
    uint32_t group = 0;  // where the BHAV was found
    uint16_t instance = 0;
    ObjectId codeOwner = 0; // object whose private/semi-global code and constants are in scope
    uint16_t pc = 0;
    // Starts as the caller's; changing it does not change the caller's (subroutines
    // that look an object up return its ID in a temp).
    ObjectId stackObject = 0;
    std::vector<int16_t> params;
    std::vector<int16_t> locals;
    // Root of a queued interaction's action tree (pushed by Idle for Input):
    // when it returns, the interaction ends and the Idle for Input that ran
    // it runs again instead of branching (interactions.hpp).
    bool interaction = false;
    // An interaction run by Gosub Found Action (autonomy.cpp): it ends the
    // same way, but the Gosub Found Action instruction branches on its result.
    bool foundAction = false;
    // A sub-queue interaction's action tree: when it returns, the sub-action
    // ends and the Idle for Input that ran it branches on its result.
    bool subInteraction = false;
    // When this frame returns, the caller's instruction branches on this
    // instead of the returned value (a primitive that ran a tree for its
    // own ends: the route failure feedback; routing_walk.cpp).
    std::optional<bool> callerResult;
    // When this frame returns, the caller's instruction runs again (with
    // the returned value in Thread::rerunResult): a primitive that runs a
    // tree in the middle of its work (the routing's waiting animations).
    bool rerunCaller = false;
    // A route of this frame's instruction failed with this result and its
    // failure feedback tree runs (routing_walk.cpp); -1: none.
    int16_t pendingRouteResult = -1;
    // The walker's pre-route tree ran for this frame's routing instruction,
    // which keeps the routing slot it read before (the tree may change the
    // temps it came from).
    bool preRouteRan = false;
    int16_t routeSlot = -1;
    // The walker's post-route tree runs for this frame's routing instruction
    // (1 after an arrival, 2 after a failure; routing_walk.cpp).
    uint8_t postRoute = 0;
    // A route callback tree (pre/post-route, goal check, along route,
    // portal) the routing pushed: routes inside it are not base routes.
    bool routeCallback = false;
    // A routing instruction resumed from a save in the middle of its walk
    // (resumeRoutes): it plans the way again from where the walker stands,
    // to the same goals, without the pre-route tree (the game's route,
    // cTSRoute::LoadFromStream BV 0x4736bc, keeps no way either: it makes
    // its goal list again, CreateBestGoalPointList).
    bool routeResume = false;
    // The running primitive's own state across ticks (the stack element's
    // primitive state in the game, cITSTreeStackElem vtable 0x48/0x4C);
    // cleared when the frame moves to another node.
    int32_t primitiveState = 0;
};

// Temps belong to the thread, so subroutines can return values in them;
// parameters, locals and the stack object belong to each frame.
struct Thread {
    // Killed: the object running the thread was removed.
    enum class Status { Running, Finished, Failed, Killed };

    ObjectId me = 0;
    uint64_t meSerial = 0; // Object::serial of `me` when the thread started (0: not checked)
    // Unique per thread (VM::start, lot_resume): which thread pushed an
    // object record (Person::ObjectRecord::thread).
    uint64_t serial = 0;
    std::array<int16_t, kTempCount> temps{};
    std::vector<Frame> stack;
    // What the tree a primitive ran returned, for the primitive running
    // again (Frame::rerunCaller); it reads and clears it.
    std::optional<bool> rerunResult;

    Status status = Status::Running;
    bool result = false;   // return value of the root tree once Finished
    std::string error;     // once Failed: the message and the stack at the failure
    size_t instructions = 0; // total executed
    // Which VM::run call runs it now (VM::treeRuns as it started): one turn
    // of its object a tick (the routing aggregate's goal checks a tick,
    // routing_walk.cpp countGoalCheck). Not saved.
    uint64_t turn = 0;
    // Set when a queued interaction's action tree returned to the Idle for
    // Input that ran it (1, or 2 when it said "no continuation"), which runs
    // again at once (queue.cpp).
    uint8_t interactionReturned = 0;
    // Set while a check tree runs for the pie menu: Add/Change the Action
    // String adds entries here instead of renaming the running interaction.
    std::vector<ActionString>* actionStrings = nullptr;
    // The motive ads of the TTAB entry whose check tree runs for Find Best
    // Interaction, by motive: data owners 0x1D "check tree ad min", 0x0C
    // "check tree ad range" and 0x1C "check tree ad personality var" (autonomy.cpp).
    std::array<int16_t, kMotiveCount> adMin{}, adRange{}, adPersonality{};
};

// Outcome of a primitive. Yield keeps the thread on the same instruction,
// which runs again on the next call to VM::run. Call: the primitive pushed a
// tree (VM::call); the instruction branches on its result when it returns.
enum class Exit : uint8_t { True, False, Error, Yield, Call };

class VM;

// The 0x7E's Lua 5.0 state (vm/lua_scripts.cpp): one per VM, built at the
// first script.
class LuaRuntime;

// What a primitive sees while it runs.
struct Context {
    VM& vm;
    Thread& thread;
    Frame& frame;
    const Behavior::Instruction& instruction;

    Object& me() const;
    Object& stackObject() const; // throws VMError if there is none

    // Variable access by data owner ("scope") and data value, as in the operands
    // (owner names in STR# 0x84 of the editor labels).
    int16_t get(uint8_t owner, int16_t data) const;
    void set(uint8_t owner, int16_t data, int16_t value) const;
};

using Primitive = std::function<Exit(Context&)>;

class VM {
public:
    explicit VM(CodeLibrary& code);
    // Defined in vm/lua_scripts.cpp, where LuaRuntime is complete.
    ~VM();
    VM(const VM&) = delete; // byId_ points into objects_

    CodeLibrary& code() { return code_; }

    // The 0x7E's Lua 5.0 state: made at the first script, kept for the VM.
    LuaRuntime& lua();

    // Objects get the lowest free id from 1 up, or `id` (a lot's saved
    // objects keep theirs; throws if it is taken). Attributes start with
    // `attributeCount` entries, semi-attributes with one per label of the
    // semi-global library. Object data "object id" and "lead tile object ID"
    // are the new id (a lot's saved single-tile objects lead themselves).
    Object& createObject(uint32_t group, size_t attributeCount = 0, ObjectId id = 0);
    // An instance of the OBJD (group, instance): attributes sized by the larger of
    // its "num attributes" and the group's attribute labels; Sims get a Person.
    // A multi-tile master (master ID set, sub index -1) is not an object
    // itself: each of its parts is created, they all lead to the first one
    // (sub index 0, else the lowest), and that one is returned.
    Object& instantiate(uint32_t group, uint16_t definitionInstance);
    // One instance of exactly that OBJD (a part stays a single part).
    Object& instantiatePart(uint32_t group, uint16_t definitionInstance, ObjectId id = 0);
    // An instance of the object with this GUID, or null if no OBJD has it.
    Object* instantiateGuid(uint32_t guid);
    Object* object(ObjectId id);
    // --- World ---------------------------------------------------------------
    // The lot's walls, floors and rooms (vm::loadLot builds it); without one
    // (single trees, tests) positions mean nothing to the primitives that
    // need them, which then fail with the reason.
    std::optional<World> world;
    // The lot's geometry for the placement tests that read heights and
    // floor quarters (TestPlacementCommon's IsFlat and floor test,
    // vm/placement.cpp): whoever keeps the lot for the VM's lifetime sets it
    // (the app's LotSimulation, s2pkg's lot commands); null skips those tests.
    const Lot* placementLot = nullptr;
    // The path planner of `world` (path_planner.hpp), built on first use
    // (routing_plan.cpp); reset it after changing the world.
    std::shared_ptr<PathPlanner> planner;
    // Walking speed in tiles per tick of walkers without a walk animation
    // (Sims walk at their walk animation's speed, animation.hpp): 2 m/s, the
    // normal adult walk, with a tile taken as a metre and a tick as 1/30 s
    // (30 ticks a game minute, a game minute a second at normal speed; inferred).
    float walkTilesPerTick = 2.0f / 30.0f;
    // Lengths of the animations the animation primitives play (animation.cpp).
    AnimationLibrary animations;
    // The gaits and locomotion clips already looked up (locomotion.cpp), by
    // what they depend on.
    struct LocoCache {
        std::unordered_map<std::string, Gait> gaits;
        std::unordered_map<std::string, std::optional<LocoPick>> clips;
    } locoCache;
    // The tile an object stands on (its position rounded down), whether or
    // not it is in the world.
    static TilePos tileOf(const Object& o) {
        return {int(std::floor(o.position.x)), int(std::floor(o.position.y)), o.position.level};
    }
    // Whether an object goes into the path planner as an obstacle, as
    // cTSObject::AddThisToPathPlanner decides (routing.cpp; docs/routing-mac.md
    // §2): in the world and not in a slot, not a Sim, door, window or stair,
    // not "attached to ceiling", and without "allow (object and) person
    // intersection". It then blocks by its routing footprint.
    static bool blocksRouting(const Object& o);
    // The rectangles (x0, y0, x1, y1, world tiles) `o` puts in the path
    // planner: its model's footprint for its tile, the model's box cut to
    // the tile when the model has no footprint list, or the OBJD box
    // (routing.cpp, cTSObject::AddThisToPathPlanner).
    // They are GetFootprint's (BV 0x100 FUN_00af9e40, base 0xe8 FUN_00b43bfc;
    // docs/routing-mac.md §9.5): each rectangle snapped to the 1/8 grid
    // (snapFootprintRect); worldFootprint is the same unsnapped.
    std::vector<std::array<float, 4>> routingFootprint(const Object& o);
    std::vector<std::array<float, 4>> worldFootprint(const Object& o);
    // A person's GetFootprint at (x, y): its box (0.2 tile, person data 73
    // as TSObjectWorldSpaceFootprint has it; a plain one without a person)
    // there, snapped.
    static std::array<float, 4> personFootprintAt(const Object* person, float x, float y);
    // TSObjectIndexedWorldSpaceFootprint(o, its place, its facing, index,
    // true) (BV 0x8a8c64): the model's footprint list named `index` (vtable
    // 0x2cc), every tile of it, where `o` stands (turned with its lead as
    // routingFootprint's); nothing when the model has no such list (the
    // binary's false: no test). The doors' 0 and 2 (DoesFootprintIntersectDoor).
    std::optional<std::vector<std::array<float, 4>>> indexedFootprint(const Object& o, int index);
    // cTSObject::ConformsToCeiling (vtable 0x1f0 -> AggregateConformsToCeiling,
    // BV 0xafdd94: the first part of its scene graph with interface
    // 0xaae90adb, nTSSG::cObject::AttachedToCeiling 0x6542f0): its model has
    // a transform node named "post_trans" (the columns that reach the
    // ceiling). Read once per OBJD.
    bool conformsToCeiling(const Object& o);
    // A tile is in the world, has a floor and nothing that blocks stands on it.
    bool tileFree(TilePos t, std::span<const ObjectId> ignore = {});
    // A person's footprint box (0.2 tile) at (x, y) on `level` meets no
    // blocking object's routing footprint (the stair rule and the floor as
    // tileFree; TSObjectCanSetPosition's object test for a Sim).
    bool pointFree(float x, float y, int level, std::span<const ObjectId> ignore = {});
    // cTSPerson::OnCleanup's "out of the world and back in a free place
    // near" (vtable 0x2d8, FUN_00af87c8): out of its slot, then the first
    // tile centre of a square spiral where it fits, its room first
    // (routing_walk.cpp). False: no place, left where it was.
    bool placeNearFree(Object& o);
    // The portals objects make of their portal-pair routing slots (the pool
    // ladder), into World::objectPortals (routing_plan.cpp). Called when the lot
    // loads and when build or buy mode changes it.
    void refreshObjectPortals();
    // Objects placed on a tile (in the world and not in a slot), by ID. The
    // tile index (tiles_) is built on the first query and kept by setPosition
    // and removeObject; a direct write to Object::position leaves it out of
    // date, so every move goes through setPosition.
    std::vector<ObjectId> objectsOnTile(TilePos t) const;
    // Sets an object's position and keeps the tile index up to date (the
    // routing's moveTo, the placement, the teleports and the containers).
    void setPosition(Object& o, Position p);
    // objectsOnTile of each of the (2r + 1)^2 tiles round `at`, in one walk
    // over the objects: [(dx + r) * (2r + 1) + dy + r].
    std::vector<std::vector<ObjectId>> objectsAround(TilePos at, int r) const;
    // How many times VM::run ran a tree: lists of objects taken before are
    // out of date once a tree may have moved something.
    uint64_t treeRuns = 0;
    // The tile's objects as above, each followed by what is in its slots (by
    // slot number, depth first): the walk of the binary's tile iterator
    // (Set to Next 8; FUN_00e1e484 in the Mac BV).
    std::vector<ObjectId> objectsOnTileWithContents(TilePos t) const;

    // Where a walker may stand to use a routing slot of `target`: goal
    // tiles with their costs, the exact point and facing of each (facing -1:
    // any). See routingGoals in routing.cpp for how the SLOT fields are read.
    struct RouteGoals {
        int level = 0;
        std::vector<PathGoal> goals;
        std::vector<std::array<float, 2>> points;
        std::vector<int> facings;
        bool onTarget = false; // the goal is on the target's own tile (its parts do not block it)
        std::vector<int16_t> multi; // each goal's "multi slot index" (its bone in a bone slot), when any
        // Each goal's chair (cTSRouteGoal +0x10): the chair a sitting goal
        // sits in, 0 for a standing goal (docs/seat-mac.md §A).
        std::vector<ObjectId> chairs;
        // With `evaluate`: the route result of the last candidate turned down
        // (EvalTileForGoal, routing.cpp), for when none is left; 0 if none was.
        int16_t refused = 0;
    };
    RouteGoals routingGoals(const Slots::Slot& slot, const Object& target, bool evaluate = false, ObjectId router = 0);
    // The model bones a routing slot names, in the object's model space, each
    // with its facing relative to the object's direction (slot_bones.cpp);
    // none without the game's resources. slotBone: the first one.
    struct SlotBone {
        float x = 0, y = 0, z = 0;
        int facing = 0;
        int16_t index = 0; // in the slot's list: the goal's "multi slot index" (person data 38)
    };
    std::vector<SlotBone> slotBones(const Slots::Slot& slot, const Object& target);
    std::optional<std::array<float, 3>> slotBone(const Slots::Slot& slot, const Object& target);
    // The object's model as posed now (local transforms of its skeleton's
    // bones; empty without one): the rest pose with Object::pose, under the
    // Animate Object animations playing on it at their time (held at their
    // last frame). holdObjectPose keeps that pose in Object::pose, for an
    // animation about to be stopped or replaced (the game's objects stay
    // where the last frame left them).
    std::vector<Xform> objectPose(const Object& target);
    // Where the object's skeleton rests its root ("root_trans", +Y of the
    // model, tiles); 0 without a skeleton.
    float rootRestY(const Object& target);
    void holdObjectPose(Object& target);

    // Threads of a removed object are killed the next time they run.
    void removeObject(ObjectId id) {
        // A person's object records go with it, each taking 1 off its
        // object's data 62 (cTSPerson::OnCleanup in the kill queue, flag
        // 0x1000: no cascade; records.cpp). A visitor leaving left 1 on the
        // visitor controller each time.
        if (auto it = objects_.find(id); it != objects_.end() && it->second.person) {
            for (const auto& r : it->second.person->objectRecords)
                if (auto x = objects_.find(r.object); x != objects_.end() && r.object != id)
                    x->second.data[62] = int16_t(x->second.data[62] - 1);
            it->second.person->objectRecords.clear();
        }
        if (auto it = objects_.find(id); it != objects_.end()) {
            unindexObject(it->second);
            objectList_.erase(it->second.listOrder);
            if (const uint32_t g = it->second.guid(); g)
                if (auto b = byGuid_.find(g); b != byGuid_.end())
                    if (auto at = std::lower_bound(b->second.begin(), b->second.end(), id);
                        at != b->second.end() && *at == id)
                        b->second.erase(at);
        }
        objects_.erase(id);
        if (size_t(uint16_t(id)) < byId_.size())
            byId_[uint16_t(id)] = nullptr;
        contained_.drop(id);
        timed_.drop(id);
        destinations_.drop(id);
        ++tableGeneration_;
    }
    // Remove Object Instance queues objects to die at the end of the tick (kill.hpp).
    std::vector<ObjectId> killQueue;
    std::set<ObjectId> dyingObjects;
    // The objects whose room (object data 29) was written since the last
    // notifyRoomChanges (rooms.cpp): that pass used to scan every object every
    // tick (0.36 s of a 43,200-tick run). The port's room writers and
    // Context::set call noteRoomChanged; createObject and the lot's
    // restoreState mark the new and loaded ones (the scan notified those too,
    // their notified room still being the constructor's). OS2_CHECK_ROOM_DIRTY=1
    // compares the set with the full scan.
    void noteRoomChanged(ObjectId id) { roomDirty_.insert(id); }
    const std::set<ObjectId>& roomDirty() const { return roomDirty_; }
    void clearRoomDirty() { roomDirty_.clear(); }
    std::set<ObjectId> roomDirty_;
    // Objects in the middle of their Cleanup (cTSObject flag 0x40000,
    // cTSPerson 0x800): a cascade of resets does not come back to them.
    std::set<ObjectId> cleaningUp;
    // The next Thread::serial.
    uint64_t threadSerials = 0;
    const std::map<ObjectId, Object>& objects() const { return objects_; }
    // The same objects in the same (ID) order, as a table indexed by ID with
    // null holes: scanning it is cheaper than walking the map. Objects must
    // not be created or removed while a loop over it runs with a reference.
    const std::vector<Object*>& objectTable() const { return byId_; }
    // The objects in the object module's list order (Object::listOrder).
    const std::map<int64_t, ObjectId>& objectList() const { return objectList_; }
    // Puts `o` at `order` in that list (the lot loader: the saved order).
    void setListOrder(Object& o, int64_t order) {
        objectList_.erase(o.listOrder);
        o.listOrder = order;
        o.appendOrder = order;
        objectList_[order] = o.id;
    }
    // The IDs of the people (objects with person data), in ID order, as of
    // this tick and the objects there are (the walkers' collision tests
    // look at them every step).
    const std::vector<ObjectId>& people() const {
        if (peopleTick_ != ticks || peopleGeneration_ != tableGeneration_) {
            people_.clear();
            for (const Object* o : byId_)
                if (o && o->person)
                    people_.push_back(o->id);
            peopleTick_ = ticks, peopleGeneration_ = tableGeneration_;
        }
        return people_;
    }
    // The objects of a definition's GUID (Object::guid), in ID order: an
    // object's definition is set as it is made (instantiatePart) and never
    // changes; removeObject takes it out. Set to Next 4 and 18.
    const std::vector<ObjectId>& objectsOfGuid(uint32_t guid) const {
        checkContained();
        static const std::vector<ObjectId> none;
        const auto it = byGuid_.find(guid);
        return it == byGuid_.end() ? none : it->second;
    }
    // `for (const auto& [id, o] : vm.eachObject())`: the same walk as over
    // objects() (ID order, an object made during the loop is met when its ID
    // is later, one removed is skipped) through the table, which is much
    // cheaper than the map's nodes; safe against creation and removal.
    struct EachObject {
        const VM* vm;
        struct It {
            const VM* vm;
            size_t i;
            void skip() {
                while (i < vm->byId_.size() && !vm->byId_[i])
                    ++i;
            }
            std::pair<ObjectId, const Object&> operator*() const { return {vm->byId_[i]->id, *vm->byId_[i]}; }
            It& operator++() {
                ++i;
                skip();
                return *this;
            }
            bool operator!=(const It& o) const { return i != o.i; }
        };
        It begin() const {
            It it{vm, 0};
            it.skip();
            return it;
        }
        It end() const { return {vm, vm->byId_.size()}; }
    };
    EachObject eachObject() const { return {this}; }

    // Containment: an object is in slot "slot number" (object data 3) of the
    // object "container id" (object data 2); outside of any slot those are 0
    // and -1, as in the lot's saved state. The object in that slot, or 0.
    // Data owners 0x10/0x14/0x30/0x31 read slots this way.
    ObjectId containedIn(ObjectId container, int16_t slot) const;
    // Every write of object data 2 goes through setContainer, or is followed
    // by noteContainer (a lot's saved data, SimAntics' writes), so that the
    // objects in a container are known without a walk over all of them.
    void setContainer(Object& o, ObjectId container, int16_t slot) {
        o.data[objdata::ContainerId] = container;
        o.data[objdata::SlotNumber] = slot;
        noteContainer(o);
    }
    void noteContainer(const Object& o) {
        if (o.data[objdata::ContainerId] != 0 && !contained_.has(o.id))
            contained_.add(o.id);
    }
    // The objects whose object data 2 may not be 0, in ID order: all those
    // whose is not, and maybe some taken out of their container since
    // (followContainers drops them).
    const std::vector<ObjectId>& containedObjects() const { return contained_.ids; }
    // Likewise the objects with timers (every timer added is followed by
    // noteTimers; handleTimers drops those without any) and the engine's
    // destinations (Object::syntheticSlot or managedDestination, set before
    // noteDestination; they go with removeObject).
    void noteTimers(const Object& o) {
        if (!o.timers.empty() && !timed_.has(o.id))
            timed_.add(o.id);
    }
    void noteDestination(const Object& o) {
        if ((o.syntheticSlot || o.managedDestination) && !destinations_.has(o.id))
            destinations_.add(o.id);
    }
    const std::vector<ObjectId>& destinationObjects() const { return destinations_.ids; }

    // --- Scheduler -----------------------------------------------------------
    // Each object has one thread of its own. Its Init (OBJf function 0) runs
    // to completion when the object is initialized, as a check tree: sleeps
    // count down without waiting for ticks (as FreeSO runs entry points).
    // Then its Main (function 1) runs a turn every tick, in object ID order,
    // until it yields; a Main that returns starts again on the next tick
    // (as in FreeSO).

    // Action tree of an object's OBJf function, 0 if it has none.
    uint16_t functionTree(const Object& o, size_t function);
    // Runs an OBJf function of `id` to completion in a thread of its own and
    // returns that thread (Finished, Failed, Killed; Running if it was still
    // going after `maxTurns` yields), or nothing if the object has no such function.
    // `args` are its parameters (the engine's RunTree passes some: the
    // stairs' "Set Model by Type", entry 47, takes the railing model).
    // `stackObject`: the tree's stack object (cTSObject::RunTree's short;
    // OnRoomChanged passes the object itself).
    std::optional<Thread> runFunction(ObjectId id, size_t function, size_t maxTurns = 1000,
                                      std::span<const int16_t> args = {}, ObjectId stackObject = 0);
    // Starts the object's Main in its thread; false if it has none.
    bool startMain(ObjectId id);
    void resetAfterError(ObjectId id, Thread& t);
    // The same reset, not after an error. `cascade`: a person's reset also
    // resets the objects of its records and their other users (unless the
    // person, or the object whose removal resets it, is dying; records.cpp).
    void resetObject(ObjectId id, Thread& t, bool cascade = true);
    // A loaded lot's objects run their load entry (OBJf 2, lot_resume.hpp);
    // off only for the save checks (s2pkg lot-save --check), which compare
    // what the save carries, not what the game's load trees do after.
    bool runLoadEntries = true;
    std::map<ObjectId, Thread> threads; // the Main threads
    // The threads of the objects removed while the tick runs (deleteGroup):
    // they are dropped at the end of it, so that the tick can walk the map
    // without looking every thread up again. The ID and the thread's serial:
    // the ID goes to a new object in the meantime, and that one's thread is
    // not the one to drop.
    std::vector<std::pair<ObjectId, uint64_t>> deadThreads;
    // The tick is running (a nested one must not drop the dead threads: the
    // iterators of the outer tick would dangle).
    bool ticking = false;
    // Instructions a thread may run in one turn before it fails as an
    // infinite loop: the game's "Too many iterations" (0x44D) after 100000
    // nodes a tick (cTSTreeSim::DoNodeAction BV 0xe1b18c; 300000 when the
    // tree simulator's flag 0x33 is set, not understood).
    size_t instructionsPerTurn = kNodeLimitPerTurn;
    // Create New Object Instance initializes the new object (runs its Init
    // and, with the scheduler running, starts its Main), as FreeSO does; off
    // for single trees (bhav-sweep), where nothing else would run.
    bool initCreatedObjects = false;
    // Create New Object Instance tests the new object's place in the world
    // (TSObjectCanSetPosition); OS2_NO_CREATE_PLACEMENT=1 turns it off to
    // compare.
    bool checkCreatedPlacement = std::getenv("OS2_NO_CREATE_PLACEMENT") == nullptr;
    // Doors and stairs run their portal trees (OBJf entry 15: TEST while
    // planning, the action on arrival; routing_plan.cpp, routing_walk.cpp); off: the engine opens
    // the way itself (waits at doors, climbs the stairs with its own phases).
    bool portalTrees = true;
    // An object whose Main fails is reset, as the binary does outside the
    // debugger (cTSObject::Error flags it, HandleError resets it: the tree
    // sim starts its Main again; the game writes an ObjectError log): its
    // queue goes and its Main starts over. On (the game) unless a test or
    // tool wants failed Mains kept. What each reset was, for the host:
    bool resetOnError = true;
    struct ErrorReset {
        size_t count = 0;
        std::string first, last; // the failures' messages with their stacks
        uint64_t lastTick = 0;
    };
    std::map<ObjectId, ErrorReset> errorResets;
    // The global sim objects the lot load made (vm::updateSimObjects): a
    // reload makes them again, so lot-save --check does not compare them.
    std::set<ObjectId> globalsMadeAtLoad;
    // Init threads of objects created while running (initCreatedObjects), for the host to look at.
    std::vector<Thread> createdInits;
    // Runs the Init of `id` and of the other parts of its multi-tile object
    // that are not in `existing` (the IDs before it was created), then starts
    // their Mains. Objects created by Inits that create objects nest; past a
    // few levels that is an error.
    void initialize(ObjectId id, const std::vector<ObjectId>& existing);

    // Game time, kept in the globals Hour, Minute, Second, Day of Month,
    // Month and Year. A minute is `ticksPerMinute` ticks (30, as in FreeSO's
    // clock for The Sims 1; not confirmed for The Sims 2).
    struct Clock {
        int ticksPerMinute = 30;
        // Real seconds to ticks at normal speed, for animation and routing
        // durations. The game runs a tick per 42 ms of its scaled clock (about
        // 23.8 a second, docs/ui-live-mac.md §1.2); 30 here by default (tests,
        // s2pkg), the app sets the game's rate.
        double ticksPerSecond = 30;
        int tick = 0; // within the minute
        int minute = 0, hour = 8, day = 1, month = 1, year = 1;
    } clock;
    void setClockGlobals(); // also Time Of Day (global 4) from the hour
    uint64_t ticks = 0;
    // The simulator's tick counter (cTSSimulator +0xa0): read from the lot's
    // SIMI (lot_resume.cpp applySavedSimulator) and written back to it,
    // passed to every person's OnSimulatorTick (its 60-tick motive step,
    // motives.cpp) and counted up after the objects have run (SimulateOneTick,
    // Mac BV 0x49a814; docs/motives-mac.md §8.5).
    uint64_t simulatorTicks = 0;
    // The tick (+ 1) flushMoveOutOfWayDestinations last ran in (once a tick;
    // it was a static map by VM address, which a later VM can reuse).
    uint64_t moveOutOfWayFlushedAt = 0;
    // The animation request counter (the game's DAT_02779c34): each Animate
    // Sim takes the next for its request ID; saved in the Sims' animation
    // queues.
    uint32_t animationCounter = 0;
    // One tick: the clock, the objects' timers, then a turn of every Main
    // thread, then contained objects move to their (outermost) container's position.
    void tick();
    // Counts down every object's timers and runs the event trees of the
    // ones that are due (timer.cpp).
    void handleTimers();
    // Event trees of timers that failed (the latest; for the host to look at).
    std::deque<Thread> failedTimerEvents;
    void followContainers();

    std::array<int16_t, kGlobalCount> globals{};

    // Relationship variables, as the game's cTSRelationshipTable keeps them
    // (docs/relationships-mac.md §1): each neighbor has a table, keyed by
    // neighbor ID (Sim to Sim: {true, from} -> {true, to}); each object,
    // Sims included, has another whose key is a raw number, an object ID or,
    // for ownership (Relationship's flag 0x08), a neighbor ID, both in the
    // same key space ({false, object} -> {false, key}: the second is never
    // `neighbor` there). The vector is the entry, as long as the game's.
    struct RelationshipKey {
        bool neighbor = false;
        int16_t id = 0;
        auto operator<=>(const RelationshipKey&) const = default;
    };
    using RelationshipPair = std::pair<RelationshipKey, RelationshipKey>;
    // The entry `key` of object `from`'s own table.
    static RelationshipPair objectRelationship(int16_t from, int16_t key) {
        return {RelationshipKey{false, from}, RelationshipKey{false, key}};
    }
    std::map<RelationshipPair, std::vector<int16_t>> relationships;
    // Key of an object (its neighbor ID if it is a Sim that has one).
    RelationshipKey relationshipKey(ObjectId id);

    // The neighborhood's families (funds, house, flags; members are the
    // neighbors whose person data "family number" is the family), lots
    // (zoning, position) and "neighborhood data" (data owner 0x22, labelled in
    // STR# 0xF9: level complete, tutorial progress, tutorial house number,
    // hide arrow). vm::loadNeighborhood (hood_bridge.hpp) fills them in.
    struct Family {
        int32_t funds = 0;
        int16_t house = 0;
        uint32_t flags = 0; // FAMI flags: bit 0 telephone, bit 3 computer (inferred)
        int16_t friends = 0;
        std::string name; // the family's name (the neighborhood's FAMI string; $Family)
        // The family's clothing bin (cTSFamily::GetClothingList: the GZPS
        // keys of its BINX entries, formats/neighborhood.hpp), what Plan
        // Outfit offers; the clothing rack adds to it (hood_save writes the
        // new ones).
        std::vector<Tgi> wardrobe;
    };
    std::map<int16_t, Family> families;
    // What cTSFamily::AddMemberClothing puts in a family's bin for a Sim
    // joining it (the GZPS of its outfits' tops, bottoms and whole outfits,
    // cas::familyBinClothing), and whether an item of another family's bin
    // fits the Sim (the binary compares three masks of the Sim's 0xAC598EAC
    // resource with the item's: inferred age, gender and category). The
    // host sets them (it has the CAS catalog); without them the bins are
    // left as they are.
    std::function<std::vector<Tgi>(int16_t neighbor)> memberClothing;
    std::function<bool(int16_t neighbor, const Tgi& item)> clothingFits;
    // The game's message 0xEC1AAC03 (evict the family from its lot,
    // cTSGameStateController::EvictFamily): the hood bridge refunds the
    // lot's price and records the lot (evictedLots, for the save); without
    // it the family only loses its house.
    std::function<void(int16_t family)> evictFamily;
    std::set<int16_t> evictedLots;
    // Families Generic Sims Call 4 removed (RemoveFamily: the last member
    // left), for the neighborhood save.
    std::set<int16_t> removedFamilies;
    struct LotRecord {
        int16_t zoning = 0; // 0 residential, 1 community
        float x = 0, y = 0; // neighborhood position
        std::string name;   // the lot info's name (the phone book's neighbor rows)
    };
    std::map<int16_t, LotRecord> lots;
    // The neighborhood data (data owner 0x22, labels in STR# 0xF9), the
    // NGBH's 16 values (cTSNeighborhood +0x14).
    std::array<int16_t, 16> neighborhoodData{};

    // Neighbors by neighbor ID. Without a neighborhood they are only
    // the ones added here (tests, stand-ins).
    std::map<int16_t, Neighbor> neighbors;

    // The family tree by neighbor ID ("Family Relationship Array"), built by
    // Generic Sims Call 21-23 and searched by Set to Next 21-23 and 31. The
    // neighborhood's family ties (loadNeighborhood) fill it in first.
    // As cTSFamilyRelationshipArray keeps them (BV 0x5442e8): siblings are
    // ties of their own (type 3), made when a child joins a parent with
    // other children, not worked out from the parents.
    struct FamilyTies {
        std::set<int16_t> parents, children, spouses, siblings;
    };
    // cTSFamilyRelationshipArray::AddRelationship / RemoveRelationship
    // (Generic Sims Call 21-23; vm/primitives.cpp).
    bool addChildTie(int16_t child, int16_t parent);
    void addSpouseTie(int16_t a, int16_t b);
    void removeTies(int16_t a, int16_t b);
    std::map<int16_t, FamilyTies> familyTree;

    // Type attributes (data owners 0x24/0x25): shared by every object of a
    // type, keyed by its definition's proxy GUID or GUID (vm.cpp).
    std::map<uint32_t, std::vector<int16_t>> typeAttributes;

    // Rooms of the lot by room ID, with the values labelled in STR# 0xDB
    // (0 ambient light 0-100, 1 outside, 2 level, 3 area, 4 pool, 5 number of
    // windows), read by data owner 0x17 with the room in temp 0. A lot fills
    // in the ones it can tell (vm::loadLot: level and area); reading a value
    // that is not known is an error.
    static constexpr size_t kRoomValueCount = 6;
    struct Room {
        std::array<int16_t, kRoomValueCount> values{};
        uint8_t known = 0x3F; // bit per value
    };
    std::map<int16_t, Room> rooms;
    // The environment score manager's walls and floors (UpdateWallScores,
    // UpdateFloorScores; vm/environment.hpp): their sums by environment
    // room, made with the rooms (setLotRooms). `environmentKnown` once a
    // lot gave them: only then does the 60-tick motive step set the
    // environment (13).
    struct EnvironmentArchitecture {
        float wallNice = 0, wallCrap = 0, wallCount = 0;
        float floorNice = 0, floorCrap = 0, floorArea = 0;
    };
    std::map<int32_t, EnvironmentArchitecture> environmentArchitecture;
    bool environmentKnown = false;
    // The light the environment score reads under a Sim (the floor light
    // map's colour there as an illuminance, cLightingManager's
    // LocationIlluminationStrength; docs/motives-mac.md §8.4), given by
    // the host that has the light maps (the app's LotLighting): the VM's
    // level (1 = the ground) and (x, y) in tiles. Unset or nothing: the
    // light is not known and the score leaves its term out (headless runs).
    std::function<std::optional<float>(int level, float x, float y)> floorIllumination;
    Neighbor* neighbor(int16_t id) {
        auto it = neighbors.find(id);
        return it == neighbors.end() ? nullptr : &it->second;
    }
    // The Sim on the lot with this neighbor ID, or null.
    Object* neighborSim(int16_t id);
    // The neighbor of a Sim made from its definition: the one with this
    // OBJD GUID, or a new one (the next ID) when the neighborhood has none;
    // 0 without a neighborhood (inferred, Create New Object Instance).
    int16_t neighborForGuid(uint32_t guid);

    // Inventories by kind and ID (0 for the global one): the neighborhood's
    // (loadNeighborhood), and others created on demand.
    std::map<std::pair<InventoryKind, int16_t>, Inventory> inventories;
    Inventory& inventory(InventoryKind kind, int16_t id) {
        return inventories[{kind, kind == InventoryKind::Global ? int16_t(0) : id}];
    }

    std::mt19937 random{0x5132};

    // Wants, fears and aspiration (wants.hpp); null without want data.
    std::shared_ptr<WantSystem> wants;

    // Engine requests not yet taken by the host (oldest first), and the
    // switches Generic Sims Call flips: build/buy buttons and game state
    // transitions (saving, quitting, lot transitions).
    std::deque<EngineRequest> engineRequests;
    void request(EngineRequest r) {
        if (engineRequests.size() >= kMaxEngineRequests)
            engineRequests.pop_front();
        engineRequests.push_back(r);
    }
    bool buildBuyEnabled = true;
    bool gameStateTransitionsEnabled = true;

    // Sounds not yet taken by the host (oldest first), and the loops each
    // object has playing (a loop is not started twice).
    std::deque<SoundRequest> soundRequests;
    std::map<int16_t, std::set<std::string>> loopingSounds;
    std::map<int, size_t> missingSounds; // sound IDs with no name, and how often
    void requestSound(SoundRequest r) {
        if (soundRequests.size() >= kMaxSoundRequests)
            soundRequests.pop_front();
        soundRequests.push_back(std::move(r));
    }

    // Dialogs (dialog.hpp): the blocking ones waiting for an answer, by the
    // object whose thread waits; the text notifications shown (latest last);
    // the answer given at once to every blocking dialog (-1: wait for the
    // host; headless hosts set a button).
    std::map<ObjectId, Dialog> dialogs;
    std::deque<Dialog> notifications;
    // The text of the last text-entry dialog (type 3) answered with OK:
    // TryDialog hands GetEnteredText to the simulator (cTSObject + 0x48,
    // vtable 0x68). Nothing in the base game's trees puts a type 3 up, and
    // what reads it back was not found (The Sims 1 named the next Make New
    // Character with it; the BV TryMakeNewCharacter does not).
    std::string enteredText;
    // The tutorial's progress window and UI Effects (tutorial.hpp).
    TutorialState tutorial;
    int autoDialogAnswer = -1;
    // Dialogs time out as the game's (autoTimeOutDialogs in dialog.hpp); on
    // game time in VM::tick when `dialogClockIsTicks` (headless), else the
    // host calls it with real time.
    bool dialogTimeouts = true;
    bool dialogClockIsTicks = true;
    // The simulator's values (SIMI +0xA4 lot, +0xA8 lot objects, +0xAC
    // family objects), as cTSLot::ComputeAndStoreLotData leaves them
    // (placement.hpp computeLotData); `known` once read or computed.
    struct LotValues {
        int32_t lot = 0, lotObjects = 0, familyObjects = 0;
        bool known = false;
    } lotValues;
    // The Railing Tool's price (BuildCatalog::railingPrice, product
    // 0x2C7BF690), what each side of a staircase's railing is worth
    // (placement.hpp railingCost); 0 until a catalog sets it.
    int railingPrice = 0;
    // Ours, not the binary's: Find Best Interaction leaves out what the Sim
    // ran autonomously and failed in the last 9999 ticks (autonomy.cpp). Off
    // by default: the binary's autonomy memory changes nothing in the base game.
    bool autonomySkipsFailures = false;
    uint32_t nextDialogId = 1;

    void setPrimitive(uint16_t opcode, Primitive p) { primitives_[opcode] = std::move(p); }

    // A thread for `me` that starts in tree `treeId` (a call opcode: global,
    // private or semi-global) with this stack object. The tree and its calls
    // resolve against the code of `codeOwner` (0: me); interactions run an
    // object's tree in the Sim's thread.
    Thread start(ObjectId me, uint16_t treeId, std::span<const int16_t> args = {}, ObjectId stackObject = 0,
                 ObjectId codeOwner = 0);
    // Runs until the thread yields, finishes or fails. Exceeding the budget
    // fails the thread (an infinite loop, as far as the game is concerned).
    void run(Thread& thread, size_t maxInstructions = kNodeLimitPerTurn);

    // Called before each instruction executes.
    std::function<void(const Thread&, const Frame&, const Behavior::Instruction&)> trace;

    // Opcodes and data owners that were needed but are not implemented.
    std::map<uint16_t, size_t> missingPrimitives;
    std::map<uint8_t, size_t> missingOwners;

    // Pushes tree `treeId` of `codeOwner`'s code as a subroutine of the
    // thread: cTSObject::GosubObjectTree, which for a person adds an object
    // record (records.cpp; `changeIcon`: Run Functional Tree's flag 0x01).
    void call(Thread& t, ObjectId codeOwner, uint16_t treeId, std::array<int16_t, 4> args, bool changeIcon = false);

    // Resolves a call opcode for code owned by `owner`: {group, BHAV} or {0, null}.
    std::pair<uint32_t, const Behavior*> resolveTree(const Object& owner, uint16_t treeId);
    const Constants& resolveConstants(const Object& owner, uint16_t table);
    // A BCON by where it is (0 private 0x1000+, 1 semi-global 0x2000+, 2 global
    // 0x100+) and its number there.
    const Constants& resolveConstants(const Object& owner, int scope, uint16_t table);

private:
    CodeLibrary& code_;
    std::map<ObjectId, Object> objects_;
    // id -> object (map nodes never move); kept by createObject/removeObject,
    // the only places that add or remove entries of objects_.
    std::vector<Object*> byId_;
    uint64_t tableGeneration_ = 0;
    // Tile -> the objects whose position is on it (every object in the world,
    // in a slot or not; the queries filter by container). Built by the first
    // objectsOnTile/objectsAround and kept by setPosition and removeObject:
    // the two scans over all the objects were 35.799 + 3.414 queries x ~1.300
    // objects in 3.600 ticks of N001_Lot11 (with the AL data). A direct write
    // to Object::position leaves it out of date (OS2_CHECK_TILE_INDEX=1
    // compares every query with the full scan).
    struct TileHash {
        size_t operator()(TilePos t) const {
            return (size_t(uint32_t(t.x)) * 73856093u) ^ (size_t(uint32_t(t.y)) * 19349663u) ^
                   (size_t(uint32_t(t.level)) * 83492791u);
        }
    };
    mutable std::unordered_map<TilePos, std::vector<ObjectId>, TileHash> tiles_;
    mutable bool tilesBuilt_ = false;
    void ensureTileIndex() const;
    void indexObject(const Object& o) const;
    void unindexTile(TilePos t, ObjectId id) const;
    void unindexObject(const Object& o) const;
    // IDs in ID order, with a flag per ID: containedObjects(), the objects
    // with timers, destinationObjects().
    struct IdList {
        std::vector<ObjectId> ids;
        std::vector<uint8_t> in;
        bool has(ObjectId id) const { return uint16_t(id) < in.size() && in[uint16_t(id)]; }
        void add(ObjectId id);
        void drop(ObjectId id);
        // Keeps the IDs `keep` says yes to (in order), and their flags.
        template <class F> void filter(F keep) {
            size_t kept = 0;
            for (const ObjectId id : ids)
                if (keep(id))
                    ids[kept++] = id;
                else
                    in[uint16_t(id)] = 0;
            ids.resize(kept);
        }
    };
    IdList contained_, timed_, destinations_;

public:
    // OS2_CHECK_CONTAINED=1: aborts unless the lists hold every object in a
    // container, with timers, made as a destination, by GUID.
    void checkContained() const;

private:
    uint64_t objectSerial_ = 0;
    std::map<int64_t, ObjectId> objectList_; // Object::listOrder -> ID
    int64_t listFront_ = 0;                  // the next made object's listOrder is below this
    mutable std::vector<ObjectId> people_;
    mutable uint64_t peopleTick_ = ~uint64_t(0), peopleGeneration_ = ~uint64_t(0);
    std::unordered_map<uint32_t, std::vector<ObjectId>> byGuid_;
    std::unordered_map<uint16_t, Primitive> primitives_;
    LuaRuntime* lua_ = nullptr; // owned; deleted by ~VM (vm/lua_scripts.cpp)

    int initDepth_ = 0;
    void push(Thread& t, ObjectId codeOwner, uint16_t treeId, std::span<const int16_t> args);
    // Follows a branch; returns false when the thread finished or failed.
    bool branch(Thread& t, Behavior::Target target);
    void fail(Thread& t, const std::string& message);
};

// Global 4 "Time Of Day" for an hour (0 day, 1 dusk, 2 night, 3 dawn).
int16_t timeOfDay(int hour);
// Registers the primitives implemented so far.
void registerPrimitives(VM& vm);
// The primitives that place and move objects in the world (routing_walk.cpp).
void registerRoutingPrimitives(VM& vm);
// A thread resumed from a save (lot_resume.hpp): the frame state of a
// routing instruction (Go To Routing Slot, Go To Relative Position) that was
// walking, or waiting on one of its route trees, is not saved (the game's
// stack elements keep none): it is made again from the stack. A routing
// instruction at the top was walking: it plans again from where the walker
// stands (Frame::routeResume). One under a frame waits on the tree it ran:
// the pre-route tree (its result then goes on as it would), the failure
// feedback (the failure, its parameter 0, goes on to the post-route tree),
// the post-route tree (after an arrival with result 0, else a failure), or
// a tree of the walk (goal check, along route, portal, route blocked: when
// it returns, the way is planned again). Returns how many it resumed.
size_t resumeRoutes(VM& vm, Thread& t);
// The path planner's A* noise as the next plan starts from it (1 for a
// planner not built yet), and that state given to the planner of the world
// as it is now (a lot resumed from the VM's own record, thread_state.hpp).
uint32_t plannerNoise(VM& vm);
void restorePlannerNoise(VM& vm, uint32_t noise);
// The primitives that need the neighborhood (hood_bridge.cpp).
void registerNeighborhoodPrimitives(VM& vm);
// Get Distance To and Get Direction To (measure.cpp).
void registerMeasurePrimitives(VM& vm);
// Set to Next 24: the next object (by ID, after `current`) that `me` sees
// within `distance` tiles (view.cpp).
std::optional<ObjectId> nextVisibleObject(VM& vm, const Object& me, ObjectId current, int16_t distance);
// On Timer (timer.cpp).
void registerTimerPrimitives(VM& vm);
// Look At (view.cpp).
void registerViewPrimitives(VM& vm);
// Find Best Object for Function, Run Functional Tree and Reach/Put (reach.cpp).
void registerReachPrimitives(VM& vm);
// The motive engine's tick, Set Motive Change and Find 5 Worst Motives (motives.cpp).
void tickMotives(VM& vm);
void tickWants(VM& vm); // wants.cpp
void registerMotivePrimitives(VM& vm);
// Find Best Interaction and Gosub Found Action (autonomy.cpp).
void registerAutonomyPrimitives(VM& vm);
// Called when an interaction Gosub Found Action ran returns `result`.
void foundActionReturned(VM& vm, Thread& t, bool result);
// The rooms a Sim in `room` can walk to (cTSPerson::GetAccessibleRooms,
// routing_plan.cpp; PathPlanner::accessibleRooms); null without a world or when
// the room has no free floor (nothing is filtered then).
const std::vector<int16_t>* accessibleRooms(VM& vm, int16_t room);

} // namespace os2::vm
