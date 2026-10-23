#pragma once

// Neighborhood data: the shipped neighborhoods (UserData/Neighborhoods/N001
// Pleasantview, N002 Strangetown, N003 Veronaville, Tutorial) and their
// character and lot packages. Plain data, no VM dependency: the VM can later
// copy what it needs (see TODO.md, M6).
//
// Layout of a neighborhood folder:
//   <P>_Neighborhood.package  NGBH, SDSC, FAMI, family ties, SREL, lot infos,
//                             SDNA, family wardrobes (BINX/3IDR), names (STR#/CTSS)
//   Characters/<P>_User<NNNNN>.package  one per Sim with a character object:
//                             OBJD (GUID), CTSS (names), appearance (0xAC598EAC + 3IDR)
//   Lots/<P>_Lot<N>.package   the lot itself (decoded by lot.cpp)

#include "dbpf.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace os2 {

namespace hoodtype {
constexpr uint32_t NGBH = 0x4E474248;          // neighborhood: inventories and memories
constexpr uint32_t SDSC = 0xAACE2EFB;          // Sim description (instance = neighbor ID)
constexpr uint32_t FAMI = 0x46414D49;          // family (instance = family number)
constexpr uint32_t FamilyTies = 0x8C870743;    // family tree
constexpr uint32_t SREL = 0xCC364C2A;          // relationship (instance = from << 16 | to)
constexpr uint32_t LotInfo = 0x0BF999E7;       // lot description ("LTXT"; instance = lot ID)
constexpr uint32_t NeighborhoodId = 0xAC8A7A2E; // NID: prefix and ID
constexpr uint32_t SDNA = 0xEBFEE33F;          // Sim DNA (XML property set; instance = neighbor ID)
constexpr uint32_t Wardrobe = 0x6C4F359D;      // family clothing bin (CPF; instance = family number)
constexpr uint32_t BINX = 0x0C560F39;          // bin entry (CPF) with a 3IDR of the same instance
constexpr uint32_t ReferenceList = 0xAC506764; // 3IDR
constexpr uint32_t SimAppearance = 0xAC598EAC; // character's outfits (CPF) with a 3IDR
constexpr uint32_t WantsFears = 0xCD95548E;    // SWAF (not decoded)
constexpr uint32_t FamilyHistory = 0x46414D68; // FAMh (not decoded)
} // namespace hoodtype

// Kept apart from the VM's and the lot's types of the same name (Lot,
// Neighbor, Inventory, ...).
namespace hood {

// ---------------------------------------------------------------------------
// NGBH: inventories of the whole neighborhood, as cTSNeighborhood::Write/
// Read (BV 0x4dc43c/0x4e0d38; docs/save-mac.md §8) do them. Version 0x6F
// (base game): 'NGBH' u32 magic, u32 version, u32 0, the neighborhood's
// size in tiles (+0x34, +0x38: cTSNeighborhood::TileCountX/TileCountY, set
// by SetSize; 128 x 128 from the constructor and in every shipped one),
// u32-length region type ("Temperate", "Desert"), the 16 i16 neighborhood
// data (+0x14, data owner 0x22; N003's hold the bytes of "Temperate"), the
// global inventory, then the lot, family and neighbor inventories (u32
// count, then u32 ID + inventory each; std::maps by ID, so in ascending
// order) and one final byte (+0xFD: a bool, read as byte != 0, that only the
// constructor (0), Read and Write touch in the base game and in BV; 1 in the
// shipped neighborhoods, 0 in the tutorial). docs/save-mac.md §8, §9.4.
// The later versions (the SimWiki's NGBH page) change three things, all
// measured over the UC neighborhoods: 0x70 (Uni) adds a u32 EP ready marker
// after that byte; 0xBE (NL) adds a version word to every slot (the global
// one included) and a u32 number to every token; 0xC2 (OFB) adds a u16 after
// the token flags; 0xCB (Seasons and later) adds a u16 after that number.
// An inventory (cTSInventory::Read/Write, BV 0x497040/0x4949a0) is the
// counted tokens then the singular ones (u32 count, then tokens); a token
// is u32 GUID, u16 flags, u32 count and that many u16 values. The singular
// list holds the memories (flags 3, the subject Sim's GUID in the data).
struct InventoryToken {
    uint32_t guid = 0;
    uint16_t flags = 0;
    std::vector<uint16_t> data;
    uint16_t flags2 = 0;          // >= 0xC2
    uint32_t inventoryNumber = 0; // >= 0xBE
    uint16_t unknown = 0;         // >= 0xCB
};

struct Inventory {
    uint32_t id = 0; // lot ID, family number or neighbor ID (0 for the global one)
    std::vector<InventoryToken> counted;  // first list
    std::vector<InventoryToken> singular; // second list (memories)
    uint32_t version = 0; // >= 0xBE: the slot's own version word
};

struct NeighborhoodInventories {
    uint32_t version = 0;
    uint32_t zero = 0;                    // written 0
    uint32_t tilesX = 128, tilesY = 128;  // +0x34/+0x38 (TileCountX/Y)
    std::string regionType = "Temperate";
    std::array<int16_t, 16> data{}; // the neighborhood data (data owner 0x22)
    uint32_t globalVersion = 0;     // >= 0xBE: the global slot's version word
    Inventory global;
    std::vector<Inventory> lots, families, neighbors;
    bool flagFD = false; // +0xFD, kept and unused
    uint32_t epReady = 0; // >= 0x70: the EP ready marker after the flag

    static NeighborhoodInventories parse(std::span<const uint8_t> data);
    std::vector<uint8_t> write() const; // neighborhood_write.cpp: parse's inverse

    // cTSNeighborhood::SanifyInventories (base 0xbdbd5c, docs/save-mac.md
    // §9.3), run before every NGBH write: the lot, family and neighbor
    // inventories whose lot, family or neighbor is not in the given sets go,
    // each one in them without an inventory gets an empty one
    // (GetInventoryBy*ID, CreateNewNeighborInventories), and the three lists
    // end in ascending ID order (the game's maps). Not done (inferred): the
    // game also empties the inventory of a neighbor whose definition is a
    // template (OBJD type 0x10), which the neighborhood file does not say.
    void sanify(const std::vector<uint32_t>& lotIds, const std::vector<uint32_t>& familyIds,
                const std::vector<uint32_t>& neighborIds);
};

// ---------------------------------------------------------------------------
// SDSC: one neighbor. Version 0x20 (base game):
//   u32 (always 0x70), u32 version, u32 (0x1C-0x20), the person data (170 i16
//   in 0x20; the count grows with the EPs: 179 (0x22), 195 (0x29), 199
//   (0x2A), 200 (0x2C), 201 (0x2D), 204 (0x2E/0x2F), 228 (0x33), 230 (0x35,
//   the E002 template), 231 (0x36);
//   labels in objects.package STR# 0x7FE59FD0/0xC8, the same array the VM
//   keeps), u16 neighbor ID, u32 GUID (the character's OBJD), u32 (always 3),
//   u32 count + count x u32 neighbor IDs this Sim has a relationship with
//   (an SREL from this Sim to each), u8 (1 for family members, 2 for the
//   rest; confirmar o que foi inferido) and, from 0x2E on, the u64 of the BV
//   collectibles (SimPE's SdscVoyage::UnserializeMem). The person data and
//   the relation offset of each version are in SimPE's SDescWrapper
//   (RelationPosition) and were measured over the UC neighborhoods.
constexpr size_t kSdscPersonDataCount = 170;

namespace sdsc { // person data fields (STR# 0xC8)
constexpr size_t Nice = 2, Active = 3, Playful = 5, Outgoing = 6, Neat = 7, CurrentOutfit = 8;
constexpr size_t Cleaning = 9, Cooking = 10, Charisma = 11, Mechanical = 12, Creativity = 15, Body = 17,
                 Logic = 18;
constexpr size_t NeighborId = 31, PersonType = 32, Aspiration = 46, SchoolGrade = 56, JobLevel = 57,
                 Age = 58, SkinColor = 60, FamilyNumber = 61, JobPerformance = 63, Gender = 65,
                 Zodiac = 70, AgeDaysLeft = 91, InterestFirst = 124, InterestLast = 141;
} // namespace sdsc

// Person Age values in SDSC (life sections, as in SimPE's LifeSections).
namespace lifestage {
constexpr int16_t Baby = 0x01, Toddler = 0x02, Child = 0x03, Teen = 0x10, Adult = 0x13, Elder = 0x33;
}
const char* lifeStageName(int16_t age); // "baby", ..., "elder", or "?"

struct SimDescription {
    uint32_t unknown0 = 0, version = 0, unknown2 = 0;
    // 170 entries in 0x20 and more with the EPs (see above); kept sized by
    // the version so a record round-trips.
    std::vector<int16_t> personData = std::vector<int16_t>(kSdscPersonDataCount);
    uint16_t neighborId = 0;
    uint32_t guid = 0;
    uint32_t unknown3 = 0;
    std::vector<uint32_t> relations;
    uint8_t trailer = 0;
    uint64_t collectibles = 0; // version >= 0x2E (BV): SimPE's SdscVoyage

    static SimDescription parse(std::span<const uint8_t> data);
    std::vector<uint8_t> write() const;

    int16_t age() const { return personData[sdsc::Age]; }
    bool female() const { return personData[sdsc::Gender] == 1; }
};

// ---------------------------------------------------------------------------
// FAMI: one family. Version 0x4E (base game):
//   'FAMI' u32 magic, u32 version, u32 (0), u32 lot ID (0 = no house), u32
//   (the family number for most families, larger for some, 0xFFFFFFFF in
//   family 0: perhaps cTSNeighborhood's "Sim group ID"), i32 funds, u32
//   friends, u32 flags (1 or 9: SimPE names bit 0 "has phone", bit 3 "has
//   computer"; confirmar o que foi inferido), u32 count + count x u32 member
//   GUIDs, u32 album GUID.
// The name (#0) and description (#1) are in the STR# whose instance is the
// family number. Families 0x7FFE/0x7FFF hold the townies and service NPCs
// (their STR# hold an unrelated surname).
// From 0x4F on a u32 neighborhood ID follows the album; from 0x50 on a u32
// lot (the last one the family was played on, the Best of Business 0x50 record
// has it) follows the lot; 0x51 adds a i32 business funds at the end and 0x55
// (BV) adds the u32 vacation lot after that lot. The SimWiki's FAMI page gives
// the chain 78/79/81/84/85; the measured gates above differ only in giving the
// last-played lot to 0x50 (data-findings.md section 20), where the 21 E001
// families (0x55) and the B001 pair (0x50/0x51) close byte for byte.
struct FamilyRecord {
    uint32_t version = 0, unknown = 0;
    uint32_t lot = 0;
    uint32_t lastPlayedLot = 0; // >= 0x51
    uint32_t vacationLot = 0;   // >= 0x55
    uint32_t simGroupId = 0;
    int32_t funds = 0;
    uint32_t friends = 0;
    uint32_t flags = 0;
    std::vector<uint32_t> members; // GUIDs
    uint32_t albumGuid = 0;
    uint32_t neighborhood = 0;  // >= 0x4F
    int32_t businessFunds = 0;  // >= 0x51

    static FamilyRecord parse(std::span<const uint8_t> data);
    std::vector<uint8_t> write() const;
};

// ---------------------------------------------------------------------------
// Family ties (0x8C870743): u32 version (1), u32 count, then per Sim u16
// neighbor ID, u32 (always 1), u32 count + count x (u32 type, u16 neighbor ID).
// Types (the edges come back in the shipped data: a child's parents list the
// child, spouses list each other): 0 and 1 my parent is, 2 I'm married to, 3
// my sibling is, 4 my child is. SimPE calls 0 "my mother is" and 1 "my father
// is", but across the four neighborhoods 0 points at 88 males and 50 females
// (the Goths list Mortimer as 0), so they stay two parent slots.
enum class TieType : uint32_t { Parent = 0, OtherParent = 1, Spouse = 2, Sibling = 3, Child = 4 };
const char* tieTypeName(TieType t);

struct FamilyTie {
    TieType type{};
    uint16_t other = 0;
};

struct FamilyTies {
    struct Sim {
        uint16_t neighborId = 0;
        uint32_t unknown = 0;
        std::vector<FamilyTie> ties;
    };
    uint32_t version = 0;
    std::vector<Sim> sims;

    static FamilyTies parse(std::span<const uint8_t> data);
};

// ---------------------------------------------------------------------------
// SREL: what one Sim thinks of another (instance = from << 16 | to).
// u32 version (always 2; cTSRelationshipEntry::ResourceRead, BV 0xac6be0,
// reads no other: the entry stays empty), u32 count, then i32 values: count
// of them, in 124 of the 18594 shipped SRELs followed by stale ones the game
// ignores (kept in `stale`; docs/relationships-mac.md §2). Labels in the character STR#
// 0x101 "Rel. Labels": 0 STR (daily), 1 UI flags, 2 LTR (lifetime), 3
// familial relation. Flag bits in SimPE's naming: 0 crush, 1 love, 2 engaged,
// 3 married, 4 friend, 5 best friend, 14 family, 15 known (confirmar o que
// foi inferido). The familial relation is what the other Sim is to this one,
// checked against the family tree of the four shipped neighborhoods
// (docs/data-findings.md): 1 = child (252 of 301 SRELs with that value), 2 =
// parent (254/296), 3 = sibling (98/188), 4 = grandchild (256/297), 5 =
// grandparent (256/288), 6 = nephew/niece (61/82), 7 = uncle/aunt (61/81), 8 =
// cousin (36/42), 9 = spouse (185/196); the rest have no path in the tree
// (Sims whose parents it does not hold). Sims2.exe's strings ("Parent",
// "Sibling", "GrandParent", "GrandChild", "Uncle/Aunt", "Nephew/Niece",
// "Cousin", "Spouse") name the other side: what this Sim is to the other.
namespace relvar {
constexpr size_t Daily = 0, Flags = 1, Lifetime = 2, Familial = 3;
}
const char* familialName(int32_t v); // "child", "parent", ..., "" for 0, "?" if unknown

// The order cTSRelationshipTable walks (and writes) its keys: a gcc
// hash_map with cTSRelationshipTable::GUIDHash (Mac BV 0x19aaef0, Thomas
// Wang's 32-bit integer hash) over 53 buckets, growing to the next of gcc's
// primes (97, 193, ...) when the count passes them; bucket by bucket, each
// bucket newest first (an insert goes to the head; a resize moves each old
// bucket's nodes, head first, to the heads of the new ones). `inserted` is
// the keys in the order they were added (a table reads its keys in the
// order it wrote them). docs/relationships-mac.md §1.
uint32_t relationshipKeyHash(uint32_t key);
std::vector<uint32_t> relationshipTableOrder(const std::vector<uint32_t>& inserted);
struct Relationship {
    uint16_t from = 0, to = 0;
    uint32_t version = 2;
    std::vector<int32_t> values;
    std::vector<int32_t> stale; // bytes after the values: the game ignores them

    static Relationship parse(std::span<const uint8_t> data, uint32_t instance);
    // As cTSRelationshipEntry::ResourceWrite (BV 0xac68ec): u32 2, u32
    // count, the values (`stale` is not written, as the game drops it).
    std::vector<uint8_t> write() const;
    int32_t value(size_t i) const { return i < values.size() ? values[i] : 0; }
};

// ---------------------------------------------------------------------------
// Lot info (0x0BF999E7, "LTXT"; instance = lot ID). Version 13 wrapping a
// base lot info (version 6), as read by OpenTS2's LotInfoCodec: u16 version;
// base: u16 version, u32 width, u32 depth, u8 type, u8 roads along edges, u8
// creation front edge, u32 flags (>= 5), u32-length name and description
// (> 5), u32 count + count x f32 heights (> 3), f32 road height (> 6), u32
// (> 7); then u32 x, u32 y, f32 height offset, i32 lot ID, u8 front edge,
// u32-length skirt texture name and the rest (one byte in every shipped lot).
// Apartment Life wraps base version 11 (outer version 18), which adds, right
// after the version 8 field, the apartment header (u8 number of apartments,
// u32 rent range high, u32 rent range low, u32 lot class value and the u8
// override flag; the SimWiki's DESC page). The apartment base/sublot tail
// after the business owner stays in `rest` for now; the 56 E001 records close
// with it byte for byte (data-findings.md section 20).
struct LotInfo {
    uint16_t version = 0, baseVersion = 0;
    uint32_t width = 0, depth = 0;
    uint8_t type = 0, roads = 0, creationFrontEdge = 0;
    uint32_t flags = 0;
    std::string name, description;
    std::vector<float> heights;
    float roadHeight = 0;
    uint32_t baseUnknown = 0;
    uint8_t apartments = 0;       // base version 11
    uint32_t rentHigh = 0;        // base version 11
    uint32_t rentLow = 0;         // base version 11
    uint32_t lotClass = 0;        // base version 11
    uint8_t lotClassOverride = 0; // base version 11
    uint32_t x = 0, y = 0;
    float heightOffset = 0;
    int32_t lotId = 0;
    uint8_t frontEdge = 0;
    std::string skirtTexture;
    std::vector<uint8_t> rest;

    // Base lot info flags (cITSBaseLotInfo::eLotFlag, docs/neighborhood-mac.md
    // §6.1): 1 the lot has been saved (cTSSaveLotController::SaveLot sets it),
    // 4 set up the family's first day on the next load (SaveLot clears it),
    // 8 its family was evicted (EvictFamily sets it, SaveLot clears it).
    static constexpr uint32_t kFlagSaved = 1, kFlagFirstDay = 4, kFlagEvicted = 8;
    // 0x80 a beach lot (cTSUserToolPlaceNewLot::PlaceLot sets it; no water check, IsOKToPlaceBeachLot).
    static constexpr uint32_t kFlagBeach = 0x80;

    static LotInfo parse(std::span<const uint8_t> data);
    // A base lot info alone (cTSBaseLotInfo: the 0x6C589723 record of a lot
    // file, after its 64-byte name): as the version 13 wrapper holds it, but
    // with 7-bit-length names.
    static LotInfo parseBase(std::span<const uint8_t> data);
    std::vector<uint8_t> write() const;     // neighborhood_write.cpp: parse's inverse
    std::vector<uint8_t> writeBase(bool str7Names = false) const; // the base part (true: as a lot file's 0x6C589723)
    // cTSLotInfo::GetCurrentSize (BV 0x48326c): the base size, swapped when
    // the lot was turned an odd number of quarters from how it was created
    // ((front edge - creation front edge) odd).
    bool turned() const { return ((int(frontEdge) - int(creationFrontEdge)) & 1) != 0; }
    uint32_t currentWidth() const { return turned() ? depth : width; }
    uint32_t currentDepth() const { return turned() ? width : depth; }
};

// NID (0xAC8A7A2E): u32 version, u32-length prefix ("N001"), u32 ID, and from
// version 5 on u32 type and u32-length sub-prefix; from 7 on a u32 "is
// downtown" and a u32 unknown; from 8 on another u32; from 9 on the u32
// stealth flags; from 0x0A on four bytes (1, 2, 3, 0 in E001). E001's version
// 10 closes exactly with the SimWiki's layout (data-findings.md section 20).
struct NeighborhoodInfo {
    uint32_t version = 0;
    std::string prefix;
    uint32_t id = 0;
    uint32_t type = 0;
    std::string subPrefix;
    uint32_t downtown = 0;  // >= 7
    uint32_t unknown7 = 0;  // >= 7
    uint32_t unknown8 = 0;  // >= 8
    uint32_t stealth = 0;   // >= 9
    std::array<uint8_t, 4> unknownA{}; // >= 0x0A

    static NeighborhoodInfo parse(std::span<const uint8_t> data);
};

// ---------------------------------------------------------------------------
// A character file (Characters/<P>_User<NNNNN>.package).
// Appearance: the 0xAC598EAC property set lists outfits as `listcnt` lists,
// list N with key `lk<N>` (outfit category << 16 | part; the category is a
// single bit like the GZPS "category", 1 = everyday, and the part a GZPS
// "outfit" bit: 1 hair, 2 face, 4 top, 8 body, 16 bottom), `ls<N>` parts and
// `le<N>_<i>` the 3IDR index of each part's GZPS; `skeletonkeyidx` is the
// skeleton's CRES.
struct Outfit {
    uint32_t key = 0;
    std::vector<Tgi> parts; // GZPS
    uint16_t category() const { return uint16_t(key >> 16); }
    uint16_t part() const { return uint16_t(key); }
};

struct Character {
    std::filesystem::path path;
    uint32_t guid = 0;
    std::string objectName; // OBJD file name ("N001_User00000 - Mortimer")
    std::string firstName, lastName, description;
    std::map<std::string, std::string> appearance; // 0xAC598EAC (empty for NPCs without one)
    std::vector<Tgi> references;                   // its 3IDR
    std::vector<Outfit> outfits;
    std::optional<Tgi> skeleton;

    static Character load(const std::filesystem::path& package);
};

// ---------------------------------------------------------------------------
// The whole neighborhood.

struct Neighbor {
    uint16_t id = 0;
    SimDescription description;
    int character = -1; // index in Neighborhood::characters, -1 if none
    std::string firstName, lastName;
    uint32_t family = 0; // FAMI holding this GUID (else person data 61); 0 = none
    std::map<std::string, std::string> dna; // SDNA (0xEBFEE33F)

    std::string name() const;
};

struct Family {
    uint32_t id = 0;
    FamilyRecord record;
    std::string name, description;
    std::vector<uint16_t> members; // neighbor IDs (GUIDs without an SDSC are dropped)
    std::vector<Tgi> wardrobe;     // GZPS in the family's clothing bin
};

struct Lot {
    uint32_t id = 0;
    LotInfo info;
    std::string name, description; // STR# 0x8000 | ID, else the lot info's
    std::filesystem::path package;  // empty if the lot file is missing
    std::vector<uint32_t> families; // families living here
};

struct FamilyTreeEdge {
    uint16_t from = 0, to = 0;
    TieType type{};
};

struct DecodeCount {
    size_t decoded = 0, failed = 0;
    std::string firstError;
};

struct Neighborhood {
    std::filesystem::path package;
    std::string prefix;            // "N001"
    std::string name, description; // CTSS 1 (empty in the tutorial)
    std::optional<NeighborhoodInfo> info;
    NeighborhoodInventories inventories;
    std::vector<Neighbor> neighbors; // by ID
    std::vector<Family> families;    // by ID
    std::vector<Lot> lots;           // by ID
    std::vector<Relationship> relationships;
    std::vector<FamilyTreeEdge> familyTree;
    std::vector<Character> characters;
    std::map<uint32_t, DecodeCount> decodeCounts; // by resource type; characters under 0
    std::vector<std::string> errors;

    // `path`: the neighborhood folder or its _Neighborhood.package.
    static Neighborhood load(const std::filesystem::path& path, bool loadCharacters = true);

    const Neighbor* neighbor(uint16_t id) const;
    const Neighbor* neighborByGuid(uint32_t guid) const;
    const Family* family(uint32_t id) const;
    const Lot* lot(uint32_t id) const;
    const Relationship* relationship(uint16_t from, uint16_t to) const;
    const Inventory* neighborInventory(uint16_t id) const;
    const Inventory* familyInventory(uint32_t id) const;
    const Inventory* lotInventory(uint32_t id) const;
};

// Every neighborhood folder under UserData/Neighborhoods (those with a
// <name>_Neighborhood.package), sorted by name.
std::vector<std::filesystem::path> findNeighborhoods(const std::filesystem::path& neighborhoodsDir);

} // namespace hood
} // namespace os2
