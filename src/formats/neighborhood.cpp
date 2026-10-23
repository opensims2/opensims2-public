#include "neighborhood.hpp"
#include "formats/log_once.hpp"

#include "binreader.hpp"
#include "property_set.hpp"
#include "sim.hpp"
#include "simantics.hpp"
#include "strings.hpp"

#include <algorithm>
#include <unordered_map>

namespace os2::hood {

namespace {

constexpr uint32_t kTypeObjd = 0x4F424A44, kTypeCtss = 0x43545353, kTypeStr = 0x53545223;

std::string str32(BinReader& r) {
    const uint32_t n = r.u32();
    if (n > r.remaining())
        throw FormatError("bad string length " + std::to_string(n));
    auto b = r.bytes(n);
    return std::string(reinterpret_cast<const char*>(b.data()), n);
}

uint32_t count(BinReader& r, size_t minBytesEach, const char* what) {
    const uint32_t n = r.u32();
    if (minBytesEach && n > r.remaining() / minBytesEach)
        throw FormatError(std::string(what) + ": bad count " + std::to_string(n));
    return n;
}

std::vector<InventoryToken> readTokens(BinReader& r, uint32_t version) {
    std::vector<InventoryToken> out(count(r, 10, "NGBH tokens"));
    for (auto& t : out) {
        t.guid = r.u32();
        t.flags = r.u16();
        if (version >= 0xC2) // OFB
            t.flags2 = r.u16();
        if (version >= 0xBE) // NL
            t.inventoryNumber = r.u32();
        if (version >= 0xCB) // Seasons+
            t.unknown = r.u16();
        t.data.resize(count(r, 2, "NGBH token data"));
        for (auto& v : t.data)
            v = r.u16();
    }
    return out;
}

Inventory readInventory(BinReader& r, uint32_t id, uint32_t version) {
    Inventory inv;
    inv.id = id;
    inv.counted = readTokens(r, version);
    inv.singular = readTokens(r, version);
    return inv;
}

std::vector<Inventory> readInventories(BinReader& r, uint32_t version) {
    std::vector<Inventory> out(count(r, 12, "NGBH slots"));
    for (auto& inv : out) {
        const uint32_t id = r.u32();
        const uint32_t slotVersion = version >= 0xBE ? r.u32() : 0;
        inv = readInventory(r, id, version);
        inv.version = slotVersion;
    }
    return out;
}

uint32_t propertyNumber(const std::map<std::string, std::string>& m, const std::string& key, uint32_t def) {
    auto it = m.find(key);
    if (it == m.end() || it->second.empty())
        return def;
    try {
        return uint32_t(std::stoul(it->second, nullptr, 0));
    } catch (const std::logic_error& err) { // not a number (std::stoul): the default
        os2::logOnce("neighborhood property", key + "=" + it->second, err);
        return def;
    }
}

// "N001_Lot11.package" -> 11; -1 if not a lot file name.
long lotNumber(const std::string& stem) {
    const auto p = stem.rfind("_Lot");
    if (p == std::string::npos || p + 4 >= stem.size())
        return -1;
    try {
        size_t used = 0;
        const long n = std::stol(stem.substr(p + 4), &used);
        return used == stem.size() - p - 4 ? n : -1;
    } catch (const std::logic_error&) { // not a number (std::stol): not a lot file name
        return -1;
    }
}

} // namespace

const char* lifeStageName(int16_t age) {
    switch (age) {
    case lifestage::Baby: return "baby";
    case lifestage::Toddler: return "toddler";
    case lifestage::Child: return "child";
    case lifestage::Teen: return "teen";
    case lifestage::Adult: return "adult";
    case lifestage::Elder: return "elder";
    default: return "?";
    }
}

const char* tieTypeName(TieType t) {
    switch (t) {
    case TieType::Parent: return "parent";
    case TieType::OtherParent: return "parent";
    case TieType::Spouse: return "spouse";
    case TieType::Sibling: return "sibling";
    case TieType::Child: return "child";
    }
    return "?";
}

uint32_t relationshipKeyHash(uint32_t k) {
    k += k << 12;
    k ^= k >> 22;
    k += k << 4;
    k ^= k >> 9;
    k += k << 10;
    k ^= k >> 2;
    k += k << 7;
    k ^= k >> 12;
    return k;
}

std::vector<uint32_t> relationshipTableOrder(const std::vector<uint32_t>& inserted) {
    static constexpr uint32_t kPrimes[] = {53,       97,        193,       389,       769,       1543,     3079,
                                           6151,     12289,     24593,     49157,     98317,     196613,   393241,
                                           786433,   1572869,   3145739,   6291469,   12582917,  25165843, 50331653,
                                           100663319, 201326611, 402653189, 805306457, 1610612741, 3221225473u,
                                           4294967291u};
    std::vector<std::vector<uint32_t>> buckets(kPrimes[0]); // each head first
    size_t count = 0;
    for (uint32_t key : inserted) {
        bool present = false;
        for (uint32_t k : buckets[relationshipKeyHash(key) % buckets.size()])
            present = present || k == key;
        if (present)
            continue;
        if (count + 1 > buckets.size()) { // resize(count + 1)
            uint32_t n = kPrimes[0];
            for (uint32_t p : kPrimes)
                if (p >= count + 1) {
                    n = p;
                    break;
                }
            std::vector<std::vector<uint32_t>> grown(n);
            for (auto& b : buckets)
                for (uint32_t k : b) {
                    auto& to = grown[relationshipKeyHash(k) % n];
                    to.insert(to.begin(), k);
                }
            buckets = std::move(grown);
        }
        auto& b = buckets[relationshipKeyHash(key) % buckets.size()];
        b.insert(b.begin(), key);
        ++count;
    }
    std::vector<uint32_t> out;
    for (const auto& b : buckets)
        out.insert(out.end(), b.begin(), b.end());
    return out;
}

const char* familialName(int32_t v) {
    static const char* const names[] = {"",            "child",        "parent",     "sibling", "grandchild",
                                        "grandparent", "nephew/niece", "uncle/aunt", "cousin",  "spouse"};
    return v >= 0 && v < 10 ? names[v] : "?";
}

NeighborhoodInventories NeighborhoodInventories::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    if (r.u32() != hoodtype::NGBH)
        throw FormatError("NGBH: bad magic");
    NeighborhoodInventories n;
    n.version = r.u32();
    // The versions the UC content ships (the SimWiki's NGBH page): 0x6F base,
    // 0x70 University, 0xBE Nightlife, 0xC2 OFB, 0xCB Seasons and later.
    if (n.version != 0x6F && n.version != 0x70 && n.version != 0xBE && n.version != 0xC2 && n.version != 0xCB)
        throw FormatError("NGBH: unsupported version " + hexString(n.version));
    n.zero = r.u32();
    n.tilesX = r.u32();
    n.tilesY = r.u32();
    n.regionType = str32(r);
    for (auto& d : n.data)
        d = r.i16();
    // The 16 words above absorb the global slot's ID word (0); from 0xBE the
    // slot also carries its version, right after that word.
    if (n.version >= 0xBE)
        n.globalVersion = r.u32();
    n.global = readInventory(r, 0, n.version);
    n.lots = readInventories(r, n.version);
    n.families = readInventories(r, n.version);
    n.neighbors = readInventories(r, n.version);
    n.flagFD = r.u8() != 0; // Read keeps (byte != 0)
    if (n.version >= 0x70)  // University+: the EP ready marker
        n.epReady = r.u32();
    r.expectEnd("NGBH");
    return n;
}

// The person data count of each SDSC version, from SimPE's SDescWrapper
// (RelationPosition minus its 0x0A and the 12-byte header) and measured on
// the UC neighborhoods: every record closes exactly on its neighbor ID.
size_t sdscPersonDataCount(uint32_t version) {
    switch (version) {
    case 0x20: return 170; // base game
    case 0x22: return 179; // University
    case 0x29: return 195; // Nightlife
    case 0x2A: return 199; // Open for Business
    case 0x2C: return 200; // Pets
    case 0x2D: return 201; // Castaway
    case 0x2E:            // Bon Voyage
    case 0x2F: return 204; // Bon Voyage (b)
    case 0x33: return 228; // FreeTime
    case 0x35: return 230; // Apartment Life template (E002)
    case 0x36: return 231; // Apartment Life
    default: throw FormatError("SDSC: unsupported version " + hexString(version));
    }
}

SimDescription SimDescription::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    SimDescription s;
    s.unknown0 = r.u32();
    s.version = r.u32();
    const size_t personDataCount = sdscPersonDataCount(s.version);
    s.unknown2 = r.u32();
    s.personData.resize(personDataCount);
    for (auto& v : s.personData)
        v = r.i16();
    s.neighborId = r.u16();
    s.guid = r.u32();
    s.unknown3 = r.u32();
    s.relations.resize(count(r, 4, "SDSC relations"));
    for (auto& v : s.relations)
        v = r.u32();
    s.trailer = r.u8();
    if (s.version >= 0x2E) { // Bon Voyage: the collectibles u64
        const uint32_t low = r.u32(), high = r.u32();
        s.collectibles = uint64_t(low) | (uint64_t(high) << 32);
    }
    r.expectEnd("SDSC");
    return s;
}

FamilyRecord FamilyRecord::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    if (r.u32() != hoodtype::FAMI)
        throw FormatError("FAMI: bad magic");
    FamilyRecord f;
    f.version = r.u32();
    // 0x4E base, 0x4F University, 0x50 Nightlife, 0x51 OFB, 0x54 Pets,
    // 0x55 BV; the fields below come from the SimWiki's FAMI page.
    if (f.version < 0x4E || f.version > 0x55)
        throw FormatError("FAMI: unsupported version " + hexString(f.version));
    f.unknown = r.u32();
    f.lot = r.u32();
    if (f.version >= 0x50) // Nightlife: the last lot the family played on
        f.lastPlayedLot = r.u32();
    if (f.version >= 0x55)
        f.vacationLot = r.u32();
    f.simGroupId = r.u32();
    f.funds = r.i32();
    f.friends = r.u32();
    f.flags = r.u32();
    f.members.resize(count(r, 4, "FAMI members"));
    for (auto& g : f.members)
        g = r.u32();
    f.albumGuid = r.u32();
    if (f.version >= 0x4F)
        f.neighborhood = r.u32();
    if (f.version >= 0x51)
        f.businessFunds = r.i32();
    r.expectEnd("FAMI");
    return f;
}

FamilyTies FamilyTies::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    FamilyTies t;
    t.version = r.u32();
    if (t.version != 1)
        throw FormatError("family ties: unsupported version " + std::to_string(t.version));
    t.sims.resize(count(r, 10, "family ties"));
    for (auto& s : t.sims) {
        s.neighborId = r.u16();
        s.unknown = r.u32();
        s.ties.resize(count(r, 6, "family ties of a Sim"));
        for (auto& tie : s.ties) {
            const uint32_t type = r.u32();
            if (type > uint32_t(TieType::Child))
                throw FormatError("family ties: unknown type " + std::to_string(type));
            tie.type = TieType(type);
            tie.other = r.u16();
        }
    }
    r.expectEnd("family ties");
    return t;
}

Relationship Relationship::parse(std::span<const uint8_t> data, uint32_t instance) {
    BinReader r(data);
    Relationship rel;
    rel.from = uint16_t(instance >> 16);
    rel.to = uint16_t(instance);
    rel.version = r.u32();
    const uint32_t n = r.u32();
    if (r.remaining() % 4 || n > r.remaining() / 4)
        throw FormatError("SREL: " + std::to_string(n) + " values in " + std::to_string(r.remaining()) + " bytes");
    rel.values.resize(n);
    for (auto& v : rel.values)
        v = r.i32();
    rel.stale.resize(r.remaining() / 4);
    for (auto& v : rel.stale)
        v = r.i32();
    return rel;
}

namespace {
void parseBaseLotInfo(BinReader& r, LotInfo& l, bool str7Names = false) {
    l.baseVersion = r.u16();
    l.width = r.u32();
    l.depth = r.u32();
    l.type = r.u8();
    l.roads = r.u8();
    l.creationFrontEdge = r.u8();
    if (l.baseVersion >= 5)
        l.flags = r.u32();
    if (l.baseVersion > 5) {
        l.name = str7Names ? r.str7() : str32(r);
        l.description = str7Names ? r.str7() : str32(r);
    }
    if (l.baseVersion > 3) {
        l.heights.resize(count(r, 4, "lot heights"));
        for (auto& h : l.heights)
            h = r.f32();
    }
    if (l.baseVersion > 6)
        l.roadHeight = r.f32();
    if (l.baseVersion > 7)
        l.baseUnknown = r.u32();
    if (l.baseVersion > 11)
        throw FormatError("lot info: unsupported base version " + std::to_string(l.baseVersion));
    if (l.baseVersion == 11) { // Apartment Life
        l.apartments = r.u8();
        l.rentHigh = r.u32();
        l.rentLow = r.u32();
        l.lotClass = r.u32();
        l.lotClassOverride = r.u8();
    }
}
} // namespace

LotInfo LotInfo::parseBase(std::span<const uint8_t> data) {
    BinReader r(data);
    r.skip(64);
    LotInfo l;
    parseBaseLotInfo(r, l, true);
    return l;
}

LotInfo LotInfo::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    LotInfo l;
    l.version = r.u16();
    if (l.version < 12)
        throw FormatError("lot info: unsupported version " + std::to_string(l.version));
    parseBaseLotInfo(r, l);
    l.x = r.u32();
    l.y = r.u32();
    l.heightOffset = r.f32();
    l.lotId = r.i32();
    l.frontEdge = r.u8();
    l.skirtTexture = str32(r);
    auto rest = r.bytes(r.remaining());
    l.rest.assign(rest.begin(), rest.end());
    return l;
}

NeighborhoodInfo NeighborhoodInfo::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    NeighborhoodInfo n;
    n.version = r.u32();
    if (n.version < 3 || n.version > 0x0A)
        throw FormatError("NID: unsupported version " + std::to_string(n.version));
    n.prefix = str32(r);
    n.id = r.u32();
    if (n.version >= 5) {
        n.type = r.u32();
        n.subPrefix = str32(r);
    }
    if (n.version >= 7) {
        n.downtown = r.u32();
        n.unknown7 = r.u32();
    }
    if (n.version >= 8)
        n.unknown8 = r.u32();
    if (n.version >= 9)
        n.stealth = r.u32();
    if (n.version >= 0x0A)
        for (auto& b : n.unknownA)
            b = r.u8();
    r.expectEnd("NID");
    return n;
}

Character Character::load(const std::filesystem::path& package) {
    DbpfPackage pkg(package);
    Character c;
    c.path = package;
    const DbpfEntry* objd = nullptr;
    const DbpfEntry* ctss = nullptr;
    const DbpfEntry* look = nullptr;
    for (const auto& e : pkg.entries()) {
        if (e.tgi.type == kTypeObjd && !objd)
            objd = &e;
        else if (e.tgi.type == kTypeCtss && !ctss)
            ctss = &e;
        else if (e.tgi.type == hoodtype::SimAppearance && !look)
            look = &e;
    }
    if (!objd)
        throw FormatError("character: no OBJD");
    const auto def = ObjectDefinition::parse(pkg.read(*objd));
    c.guid = def.guid();
    c.objectName = def.fileName;
    if (ctss) {
        const auto t = StringTable::parse(pkg.read(*ctss));
        c.firstName = t.get(0);
        c.description = t.get(1);
        c.lastName = t.get(2);
    }
    if (look) {
        c.appearance = readPropertySet(pkg.read(*look));
        const Tgi refTgi{hoodtype::ReferenceList, look->tgi.group, look->tgi.instance, look->tgi.instanceHi};
        if (const auto* ref = pkg.find(refTgi))
            c.references = parseReferenceList(pkg.read(*ref));
        auto at = [&](uint32_t i) -> std::optional<Tgi> {
            if (i < c.references.size())
                return c.references[i];
            return std::nullopt;
        };
        const uint32_t lists = propertyNumber(c.appearance, "listcnt", 0);
        for (uint32_t n = 0; n < lists; ++n) {
            Outfit o;
            const auto ln = std::to_string(n);
            o.key = propertyNumber(c.appearance, "lk" + ln, 0);
            const uint32_t parts = propertyNumber(c.appearance, "ls" + ln, 0);
            for (uint32_t i = 0; i < parts; ++i) {
                const uint32_t idx = propertyNumber(c.appearance, "le" + ln + "_" + std::to_string(i), ~0u);
                if (auto t = at(idx))
                    o.parts.push_back(*t);
                else
                    throw FormatError("character: outfit part index " + std::to_string(idx) + " outside the 3IDR");
            }
            c.outfits.push_back(std::move(o));
        }
        if (c.appearance.count("skeletonkeyidx"))
            c.skeleton = at(propertyNumber(c.appearance, "skeletonkeyidx", ~0u));
    }
    return c;
}

std::string Neighbor::name() const {
    if (firstName.empty() && lastName.empty())
        return "#" + std::to_string(id);
    if (lastName.empty())
        return firstName;
    return firstName + " " + lastName;
}

namespace {

template <class T, class Id>
const T* byId(const std::vector<T>& v, Id id) {
    auto it = std::lower_bound(v.begin(), v.end(), id, [](const T& a, Id b) { return a.id < b; });
    return it != v.end() && it->id == id ? &*it : nullptr;
}

const Inventory* findInventory(const std::vector<Inventory>& v, uint32_t id) {
    for (const auto& inv : v)
        if (inv.id == id)
            return &inv;
    return nullptr;
}

} // namespace

const Neighbor* Neighborhood::neighbor(uint16_t id) const { return byId(neighbors, id); }
const Family* Neighborhood::family(uint32_t id) const { return byId(families, id); }
const Lot* Neighborhood::lot(uint32_t id) const { return byId(lots, id); }

const Neighbor* Neighborhood::neighborByGuid(uint32_t guid) const {
    for (const auto& n : neighbors)
        if (n.description.guid == guid)
            return &n;
    return nullptr;
}

const Relationship* Neighborhood::relationship(uint16_t from, uint16_t to) const {
    for (const auto& r : relationships)
        if (r.from == from && r.to == to)
            return &r;
    return nullptr;
}

const Inventory* Neighborhood::neighborInventory(uint16_t id) const { return findInventory(inventories.neighbors, id); }
const Inventory* Neighborhood::familyInventory(uint32_t id) const { return findInventory(inventories.families, id); }
const Inventory* Neighborhood::lotInventory(uint32_t id) const { return findInventory(inventories.lots, id); }

std::vector<std::filesystem::path> findNeighborhoods(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (!e.is_directory())
            continue;
        const auto name = e.path().filename().string();
        if (std::filesystem::exists(e.path() / (name + "_Neighborhood.package")))
            out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

Neighborhood Neighborhood::load(const std::filesystem::path& path, bool loadCharacters) {
    namespace fs = std::filesystem;
    Neighborhood h;
    fs::path dir = path;
    if (fs::is_directory(path)) {
        h.prefix = path.filename().string();
        h.package = path / (h.prefix + "_Neighborhood.package");
    } else {
        h.package = path;
        dir = path.parent_path();
        const auto stem = path.stem().string();
        const auto p = stem.find("_Neighborhood");
        h.prefix = p == std::string::npos ? stem : stem.substr(0, p);
    }

    DbpfPackage pkg(h.package);
    auto attempt = [&](uint32_t type, const char* what, auto&& fn) {
        auto& c = h.decodeCounts[type];
        try {
            fn();
            ++c.decoded;
        } catch (const std::exception& e) {
            ++c.failed;
            const std::string msg = std::string(what) + ": " + e.what();
            os2::logOnce("neighborhood", what, e); // (once a kind of record; the app does not print h.errors)
            if (c.firstError.empty())
                c.firstError = msg;
            h.errors.push_back(msg);
        }
    };

    std::unordered_map<uint32_t, StringTable> strings;
    std::unordered_map<uint32_t, std::map<std::string, std::string>> dna;
    std::unordered_map<uint32_t, uint32_t> binFamily; // BINX/3IDR instance -> wardrobe family
    std::vector<const DbpfEntry*> binEntries;
    std::unordered_map<uint32_t, const DbpfEntry*> refLists;
    std::vector<FamilyRecord> famiRecords;
    std::vector<uint32_t> famiIds;
    std::optional<FamilyTies> ties;

    for (const auto& e : pkg.entries()) {
        const uint32_t inst = e.tgi.instance;
        const std::string where = hexString(inst);
        switch (e.tgi.type) {
        case hoodtype::NGBH:
            attempt(e.tgi.type, "NGBH", [&] { h.inventories = NeighborhoodInventories::parse(pkg.read(e)); });
            break;
        case hoodtype::SDSC:
            attempt(e.tgi.type, ("SDSC " + where).c_str(), [&] {
                Neighbor n;
                n.description = SimDescription::parse(pkg.read(e));
                n.id = n.description.neighborId;
                if (n.id != uint16_t(inst))
                    throw FormatError("neighbor ID " + std::to_string(n.id) + " differs from the instance");
                h.neighbors.push_back(std::move(n));
            });
            break;
        case hoodtype::FAMI:
            attempt(e.tgi.type, ("FAMI " + where).c_str(), [&] {
                famiRecords.push_back(FamilyRecord::parse(pkg.read(e)));
                famiIds.push_back(inst);
            });
            break;
        case hoodtype::FamilyTies:
            attempt(e.tgi.type, "family ties", [&] { ties = FamilyTies::parse(pkg.read(e)); });
            break;
        case hoodtype::SREL:
            attempt(e.tgi.type, ("SREL " + where).c_str(),
                    [&] { h.relationships.push_back(Relationship::parse(pkg.read(e), inst)); });
            break;
        case hoodtype::LotInfo:
            attempt(e.tgi.type, ("lot info " + where).c_str(), [&] {
                Lot l;
                l.id = inst;
                l.info = LotInfo::parse(pkg.read(e));
                if (l.info.lotId != int32_t(inst))
                    throw FormatError("lot ID " + std::to_string(l.info.lotId) + " differs from the instance");
                h.lots.push_back(std::move(l));
            });
            break;
        case hoodtype::NeighborhoodId:
            attempt(e.tgi.type, "NID", [&] { h.info = NeighborhoodInfo::parse(pkg.read(e)); });
            break;
        case hoodtype::SDNA:
            attempt(e.tgi.type, ("SDNA " + where).c_str(), [&] { dna[inst] = readPropertySet(pkg.read(e)); });
            break;
        case hoodtype::BINX:
            binEntries.push_back(&e);
            break;
        case hoodtype::ReferenceList:
            refLists[inst] = &e;
            break;
        case kTypeStr:
            attempt(e.tgi.type, ("STR# " + where).c_str(), [&] { strings[inst] = StringTable::parse(pkg.read(e)); });
            break;
        case kTypeCtss:
            attempt(e.tgi.type, "CTSS", [&] {
                const auto t = StringTable::parse(pkg.read(e));
                h.name = t.get(0);
                h.description = t.get(1);
            });
            break;
        default:
            break;
        }
    }

    // Family wardrobes: each BINX names its bin (a 0x6C4F359D whose instance
    // is the family number) and the GZPS through its 3IDR.
    std::map<uint32_t, std::vector<Tgi>> wardrobes;
    for (const auto* e : binEntries) {
        attempt(hoodtype::BINX, ("BINX " + hexString(e->tgi.instance)).c_str(), [&] {
            const auto props = readPropertySet(pkg.read(*e));
            auto it = refLists.find(e->tgi.instance);
            if (it == refLists.end())
                throw FormatError("no 3IDR");
            const auto refs = parseReferenceList(pkg.read(*it->second));
            const uint32_t bin = propertyNumber(props, "binidx", ~0u), obj = propertyNumber(props, "objectidx", ~0u);
            if (bin >= refs.size() || obj >= refs.size() || refs[bin].type != hoodtype::Wardrobe)
                throw FormatError("bin/object index outside the 3IDR");
            wardrobes[refs[bin].instance].push_back(refs[obj]);
        });
    }

    // Names and descriptions from the STR# tables.
    auto text = [&](uint32_t inst, size_t i) {
        auto it = strings.find(inst);
        return it == strings.end() ? std::string() : it->second.get(i);
    };

    std::sort(h.neighbors.begin(), h.neighbors.end(), [](auto& a, auto& b) { return a.id < b.id; });
    std::unordered_map<uint32_t, size_t> byGuid;
    for (size_t i = 0; i < h.neighbors.size(); ++i) {
        auto& n = h.neighbors[i];
        byGuid[n.description.guid] = i;
        n.family = uint32_t(uint16_t(n.description.personData[sdsc::FamilyNumber]));
        if (auto it = dna.find(n.id); it != dna.end())
            n.dna = it->second;
    }

    for (size_t i = 0; i < famiRecords.size(); ++i) {
        Family f;
        f.id = famiIds[i];
        f.record = std::move(famiRecords[i]);
        if (f.id < 0x7FFE) {
            f.name = text(f.id, 0);
            f.description = text(f.id, 1);
        }
        for (uint32_t g : f.record.members)
            if (auto it = byGuid.find(g); it != byGuid.end()) {
                f.members.push_back(h.neighbors[it->second].id);
                h.neighbors[it->second].family = f.id;
            }
        if (auto it = wardrobes.find(f.id); it != wardrobes.end())
            f.wardrobe = it->second;
        h.families.push_back(std::move(f));
    }
    std::sort(h.families.begin(), h.families.end(), [](auto& a, auto& b) { return a.id < b.id; });

    std::sort(h.lots.begin(), h.lots.end(), [](auto& a, auto& b) { return a.id < b.id; });
    std::map<long, fs::path> lotFiles;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir / "Lots", ec))
        if (e.path().extension() == ".package")
            if (long n = lotNumber(e.path().stem().string()); n >= 0)
                lotFiles[n] = e.path();
    for (auto& l : h.lots) {
        l.name = text(0x8000 | l.id, 0);
        l.description = text(0x8000 | l.id, 1);
        if (l.name.empty())
            l.name = l.info.name;
        if (l.description.empty())
            l.description = l.info.description;
        if (auto it = lotFiles.find(long(l.id)); it != lotFiles.end())
            l.package = it->second;
        for (const auto& f : h.families)
            if (f.record.lot == l.id)
                l.families.push_back(f.id);
    }

    std::sort(h.relationships.begin(), h.relationships.end(),
              [](auto& a, auto& b) { return std::pair(a.from, a.to) < std::pair(b.from, b.to); });

    if (ties)
        for (const auto& s : ties->sims)
            for (const auto& t : s.ties)
                h.familyTree.push_back({s.neighborId, t.other, t.type});

    if (loadCharacters) {
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(dir / "Characters", ec))
            if (e.path().extension() == ".package")
                files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const auto& f : files)
            attempt(0, ("character " + f.filename().string()).c_str(), [&] {
                h.characters.push_back(Character::load(f));
                const auto& c = h.characters.back();
                if (auto it = byGuid.find(c.guid); it != byGuid.end()) {
                    auto& n = h.neighbors[it->second];
                    n.character = int(h.characters.size() - 1);
                    n.firstName = c.firstName;
                    n.lastName = c.lastName;
                }
            });
    }
    return h;
}

} // namespace os2::hood
