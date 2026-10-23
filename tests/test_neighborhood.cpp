// Neighborhood decoders (NGBH, SDSC, FAMI, family ties, SREL, lot info, NID)
// and Neighborhood::load on hand-built data.
#include "formats/dbpf.hpp"
#include "formats/neighborhood.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace os2;
using namespace os2::hood;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

template <class F>
static void throws(F&& f, const char* what) {
    try {
        f();
    } catch (const std::exception&) {
        return;
    }
    check(false, what);
}

struct Writer {
    std::vector<uint8_t> b;
    Writer& u8(uint8_t v) { b.push_back(v); return *this; }
    Writer& u16(uint16_t v) { return u8(uint8_t(v)).u8(uint8_t(v >> 8)); }
    Writer& u32(uint32_t v) { return u16(uint16_t(v)).u16(uint16_t(v >> 16)); }
    Writer& f32(float v) { uint32_t u; std::memcpy(&u, &v, 4); return u32(u); }
    Writer& str32(const std::string& s) { u32(uint32_t(s.size())); b.insert(b.end(), s.begin(), s.end()); return *this; }
    Writer& cstr(const std::string& s) { b.insert(b.end(), s.begin(), s.end()); return u8(0); }
    Writer& zeros(size_t n) { b.insert(b.end(), n, 0); return *this; }
};

// One token list: (guid, flags, data...).
static void tokens(Writer& w, std::initializer_list<std::pair<uint32_t, std::vector<uint16_t>>> list, uint16_t flags) {
    w.u32(uint32_t(list.size()));
    for (const auto& [guid, data] : list) {
        w.u32(guid).u16(flags).u32(uint32_t(data.size()));
        for (auto v : data)
            w.u16(v);
    }
}

static std::vector<uint8_t> ngbh(const std::string& second = "") {
    Writer w;
    w.u32(hoodtype::NGBH).u32(0x6F).u32(0).u32(0x80).u32(0x80).str32("Temperate");
    w.str32(second).zeros(28 - second.size());
    tokens(w, {}, 0);
    tokens(w, {{0x6D9E58F0, {1, 0}}}, 0);       // global
    w.u32(1).u32(11);                            // one lot
    tokens(w, {}, 0);
    tokens(w, {{0x2CD9022F, {0, 300, 300}}}, 0);
    w.u32(1).u32(1);                             // one family
    tokens(w, {{0x0D26B746, {1, 3}}}, 1);
    tokens(w, {}, 0);
    w.u32(1).u32(1);                             // one neighbor with a memory
    tokens(w, {}, 0);
    tokens(w, {{0xCC89C448, {0, 1997, 6, 16, 2}}}, 3);
    w.u8(1);
    return w.b;
}

static std::vector<uint8_t> makeSdsc(uint16_t id, uint32_t guid, int16_t age, int16_t gender, int16_t family,
                                 std::vector<uint32_t> relations) {
    Writer w;
    w.u32(0x70).u32(0x20).u32(0x20);
    for (size_t i = 0; i < kSdscPersonDataCount; ++i)
        w.u16(uint16_t(i == sdsc::Age ? age : i == sdsc::Gender ? gender : i == sdsc::FamilyNumber ? family
                       : i == sdsc::Nice ? 500 : 0));
    w.u16(id).u32(guid).u32(3).u32(uint32_t(relations.size()));
    for (auto r : relations)
        w.u32(r);
    w.u8(1);
    return w.b;
}

// SDSC 0x36 (AL): 231 person data words, trailer 0 and the BV collectibles.
static std::vector<uint8_t> makeSdscVersion(uint32_t version, size_t personData) {
    Writer w;
    w.u32(0x71).u32(version).u32(version);
    for (size_t i = 0; i < personData; ++i)
        w.u16(uint16_t(i == sdsc::Age ? lifestage::Adult : i == sdsc::Gender ? 1 : 0));
    w.u16(4).u32(0x724CD298).u32(3).u32(2).u32(5).u32(7).u8(0);
    w.u32(0x9ABCDEF0).u32(0x12345678);
    return w.b;
}

static std::vector<uint8_t> makeSdsc36() { return makeSdscVersion(0x36, 231); }

static std::vector<uint8_t> fami(uint32_t lot, int32_t funds, std::vector<uint32_t> members) {
    Writer w;
    w.u32(hoodtype::FAMI).u32(0x4E).u32(0).u32(lot).u32(1).u32(uint32_t(funds)).u32(2).u32(9);
    w.u32(uint32_t(members.size()));
    for (auto g : members)
        w.u32(g);
    w.u32(0x12345678);
    return w.b;
}

// FAMI 0x55 (BV): lot, last played lot, vacation lot, creation order, funds,
// friends, flags, members, album, neighborhood, business funds.
static std::vector<uint8_t> fami55() {
    Writer w;
    w.u32(hoodtype::FAMI).u32(0x55).u32(0).u32(124).u32(124).u32(0).u32(1).u32(4262).u32(3).u32(1);
    w.u32(2).u32(0x7518EF1F).u32(0x352C9AC9).u32(0xD518F8C7).u32(1).u32(4262);
    return w.b;
}

// FAMI 0x50 (Nightlife): the last played lot, without the business funds.
static std::vector<uint8_t> fami50() {
    Writer w;
    w.u32(hoodtype::FAMI).u32(0x50).u32(0).u32(128).u32(128).u32(14).u32(0).u32(0).u32(0);
    w.u32(0).u32(0x70D11698).u32(1);
    return w.b;
}

// NGBH 0xCB (Seasons+): the slot version words, the token flags2 / inventory
// number / unknown and the EP ready marker.
static std::vector<uint8_t> ngbhCb() {
    Writer w;
    w.u32(hoodtype::NGBH).u32(0xCB).u32(0).u32(0x80).u32(0x80).str32("Temperate").zeros(32);
    w.u32(0xBE); // global slot version, after the 16 data words
    auto item = [&](uint32_t guid, uint16_t flags, uint16_t flags2, uint32_t inv, uint16_t unknown,
                    std::vector<uint16_t> data) {
        w.u32(guid).u16(flags).u16(flags2).u32(inv).u16(unknown).u32(uint32_t(data.size()));
        for (auto v : data)
            w.u16(v);
    };
    w.u32(0);                              // global counted
    w.u32(1); item(0x6D9E58F0, 0, 0, 7, 9, {1, 0}); // global singular
    w.u32(1).u32(11).u32(0xBE);            // one lot, its slot version
    w.u32(0);                              // counted
    w.u32(1); item(0x2CD9022F, 0, 0, 8, 10, {0, 300, 300}); // singular
    w.u32(0).u32(0);                       // no families, no neighbors
    w.u8(1).u32(7);                        // flag +0xFD, EP ready marker
    return w.b;
}

static std::vector<uint8_t> ties() {
    Writer w;
    w.u32(1).u32(2);
    w.u16(1).u32(1).u32(1).u32(4).u16(2); // 1: my child is 2
    w.u16(2).u32(1).u32(1).u32(0).u16(1); // 2: my parent is 1
    return w.b;
}

static std::vector<uint8_t> srel(std::vector<int32_t> values, size_t stale = 0) {
    Writer w;
    w.u32(2).u32(uint32_t(values.size()));
    for (auto v : values)
        w.u32(uint32_t(v));
    for (size_t i = 0; i < stale; ++i)
        w.u32(0);
    return w.b;
}

static std::vector<uint8_t> lotInfo(int32_t id) {
    Writer w;
    w.u16(13).u16(6).u32(1).u32(1).u8(0).u8(4).u8(2).u32(3).str32("1 Sim Lane").str32("desc");
    w.u32(4).f32(1).f32(2).f32(3).f32(4);
    w.u32(67).u32(56).f32(324.5f).u32(uint32_t(id)).u8(2).str32("lottexture-test-01").u8(1);
    return w.b;
}

// Lot info 18 wrapping base 11 (AL): the apartment header after the version 8
// field, then the position, the texture, the business owner and the apartment
// base/sublot tail (kept in rest).
static std::vector<uint8_t> lotInfoApartment(int32_t id) {
    Writer w;
    w.u16(18).u16(11).u32(4).u32(2).u8(8).u8(4).u8(2).u32(3).str32("Apt Base").str32("desc");
    w.u32(0);    // no heights
    w.f32(0);    // road height (> 6)
    w.u32(0);    // version 8 field
    w.u8(3).u32(5000).u32(1000).u32(42).u8(1); // apartments, rents, class, override
    w.u32(67).u32(56).f32(324.5f).u32(uint32_t(id)).u8(2).str32("lottexture-test-01").u8(0);
    w.u32(0xABCD); // business owner (outer version >= 14)
    w.u32(0).zeros(9).u32(0).u32(0); // apartment base, nine bytes, no sublots, count B
    return w.b;
}

static std::vector<uint8_t> strings(std::initializer_list<std::string> values) {
    Writer w;
    w.zeros(64).u16(0xFFFD).u16(uint16_t(values.size()));
    for (const auto& v : values)
        w.u8(1).cstr(v).cstr("");
    return w.b;
}

static std::vector<uint8_t> objd(uint32_t guid) {
    Writer w;
    std::string name = "N999_User00000 - Test";
    name.resize(64, '\0');
    w.b.insert(w.b.end(), name.begin(), name.end());
    w.u32(0x8B);
    for (size_t off = 0x44; off < 0x60; off += 2)
        w.u16(off == 0x5C ? uint16_t(guid) : off == 0x5E ? uint16_t(guid >> 16) : 0);
    w.str32("Test");
    return w.b;
}

static void testDecoders() {
    {
        const auto n = NeighborhoodInventories::parse(ngbh());
        check(n.version == 0x6F && n.regionType == "Temperate" && n.data == std::array<int16_t, 16>{}, "NGBH header");
        check(n.global.singular.size() == 1 && n.global.singular[0].guid == 0x6D9E58F0, "NGBH global inventory");
        check(n.lots.size() == 1 && n.lots[0].id == 11 && n.lots[0].singular[0].data.size() == 3, "NGBH lot");
        check(n.families.size() == 1 && n.families[0].counted.size() == 1 && n.families[0].counted[0].flags == 1,
              "NGBH family counted token");
        check(n.neighbors.size() == 1 && n.neighbors[0].singular[0].flags == 3 &&
                  n.neighbors[0].singular[0].data[1] == 1997,
              "NGBH memory");
        check(n.flagFD, "NGBH flag +0xFD");
        // The 32 bytes after the region type are the 16 neighborhood data
        // (cTSNeighborhood +0x14, data owner 0x22), whatever they hold.
        const auto t = NeighborhoodInventories::parse(ngbh("Temperate"));
        check(t.data[0] == 9 && t.data[1] == 0 && t.write() == ngbh("Temperate"), "NGBH neighborhood data");
        auto cut = ngbh();
        cut.pop_back();
        throws([&] { NeighborhoodInventories::parse(cut); }, "NGBH truncated");
        // 0xCB (Seasons+): slot version words, the three extra token fields and
        // the EP ready marker; the record round-trips.
        const auto cb = NeighborhoodInventories::parse(ngbhCb());
        check(cb.version == 0xCB && cb.globalVersion == 0xBE && cb.epReady == 7, "NGBH 0xCB header");
        check(cb.global.singular[0].flags2 == 0 && cb.global.singular[0].inventoryNumber == 7 &&
                  cb.global.singular[0].unknown == 9,
              "NGBH 0xCB token fields");
        check(cb.lots.size() == 1 && cb.lots[0].version == 0xBE && cb.lots[0].id == 11 &&
                  cb.lots[0].singular[0].data[1] == 300,
              "NGBH 0xCB slot");
        check(cb.write() == ngbhCb(), "NGBH 0xCB round-trip");
    }
    {
        const auto s = SimDescription::parse(makeSdsc(7, 0xCDD72499, lifestage::Adult, 1, 8, {10, 48}));
        check(s.neighborId == 7 && s.guid == 0xCDD72499 && s.age() == lifestage::Adult && s.female(), "SDSC fields");
        check(s.personData[sdsc::Nice] == 500 && s.personData[sdsc::FamilyNumber] == 8, "SDSC person data");
        check(s.relations == std::vector<uint32_t>{10, 48} && s.trailer == 1 && s.unknown3 == 3, "SDSC relations");
        check(std::string(lifeStageName(s.age())) == "adult", "life stage name");
        auto v = makeSdsc(7, 1, 0, 0, 0, {});
        v[4] = 0x22;
        throws([&] { SimDescription::parse(v); }, "SDSC other version");
        // 0x36 (AL): 231 person data words and the BV collectibles tail.
        const auto al = SimDescription::parse(makeSdsc36());
        check(al.version == 0x36 && al.personData.size() == 231 && al.age() == lifestage::Adult && al.female(),
              "SDSC 0x36 person data");
        check(al.neighborId == 4 && al.guid == 0x724CD298 && al.relations == std::vector<uint32_t>({5, 7}) &&
                  al.trailer == 0 && al.collectibles == 0x123456789ABCDEF0ull,
              "SDSC 0x36 tail");
        check(al.write() == makeSdsc36(), "SDSC 0x36 round-trip");
        // 0x35 (the E002 template): 230 words, same BV tail.
        const auto tpl = SimDescription::parse(makeSdscVersion(0x35, 230));
        check(tpl.version == 0x35 && tpl.personData.size() == 230 && tpl.collectibles == 0x123456789ABCDEF0ull &&
                  tpl.write() == makeSdscVersion(0x35, 230),
              "SDSC 0x35");
    }
    {
        const auto f = FamilyRecord::parse(fami(11, 511632, {0x4DAE6810, 0x0DAE6972}));
        check(f.lot == 11 && f.funds == 511632 && f.flags == 9 && f.friends == 2, "FAMI fields");
        check(f.members.size() == 2 && f.members[1] == 0x0DAE6972 && f.albumGuid == 0x12345678, "FAMI members");
        auto extra = fami(0, 0, {});
        extra.push_back(0);
        throws([&] { FamilyRecord::parse(extra); }, "FAMI trailing bytes");
        // 0x55 (BV): last played lot, vacation lot, neighborhood and the
        // business funds.
        const auto bv = FamilyRecord::parse(fami55());
        check(bv.version == 0x55 && bv.lot == 124 && bv.lastPlayedLot == 124 && bv.vacationLot == 0,
              "FAMI 0x55 lots");
        check(bv.neighborhood == 1 && bv.businessFunds == 4262 && bv.members.size() == 2 &&
                  bv.members[0] == 0x7518EF1F && bv.albumGuid == 0xD518F8C7,
              "FAMI 0x55 fields");
        check(bv.write() == fami55(), "FAMI 0x55 round-trip");
        // 0x50 (Nightlife): last played lot, no business funds, neighborhood.
        const auto nl = FamilyRecord::parse(fami50());
        check(nl.version == 0x50 && nl.lot == 128 && nl.lastPlayedLot == 128 && nl.albumGuid == 0x70D11698 &&
                  nl.neighborhood == 1 &&
                  nl.write() == fami50(),
              "FAMI 0x50");
    }
    {
        const auto t = FamilyTies::parse(ties());
        check(t.sims.size() == 2 && t.sims[0].ties[0].type == TieType::Child && t.sims[0].ties[0].other == 2,
              "family ties");
        check(t.sims[1].ties[0].type == TieType::Parent, "family tie parent");
    }
    {
        const auto r = Relationship::parse(srel({45, 0xC000, 19, 1}, 3), 0x0001000F);
        check(r.from == 1 && r.to == 15 && r.values.size() == 4 && r.stale.size() == 3, "SREL key and values");
        check(r.value(relvar::Lifetime) == 19 && r.value(relvar::Familial) == 1 && r.value(9) == 0, "SREL value");
        check(std::string(familialName(1)) == "child" && std::string(familialName(9)) == "spouse" &&
                  std::string(familialName(4)) == "grandchild" && std::string(familialName(7)) == "uncle/aunt",
              "familial names");
        auto odd = srel({1});
        odd.push_back(0);
        throws([&] { Relationship::parse(odd, 0); }, "SREL odd size");
    }
    {
        const auto l = LotInfo::parse(lotInfo(11));
        check(l.version == 13 && l.baseVersion == 6 && l.width == 1 && l.depth == 1 && l.roads == 4, "lot info base");
        check(l.name == "1 Sim Lane" && l.heights.size() == 4 && l.heights[3] == 4.0f, "lot info name and heights");
        check(l.x == 67 && l.y == 56 && l.lotId == 11 && l.frontEdge == 2 && l.skirtTexture == "lottexture-test-01" &&
                  l.rest.size() == 1,
              "lot info position");
        // Outer 18 wrapping base 11 (AL): the apartment header, then the
        // apartment base/sublot tail in rest.
        const auto apt = LotInfo::parse(lotInfoApartment(127));
        check(apt.version == 18 && apt.baseVersion == 11 && apt.type == 8 && apt.apartments == 3 &&
                  apt.rentHigh == 5000 && apt.rentLow == 1000 && apt.lotClass == 42 && apt.lotClassOverride == 1,
              "lot info 11 apartment header");
        check(apt.lotId == 127 && apt.rest.size() == 26, "lot info 11 tail");
        check(apt.write() == lotInfoApartment(127), "lot info 11 round-trip");
    }
    {
        Writer w;
        w.u32(3).str32("N001").u32(1);
        const auto n = NeighborhoodInfo::parse(w.b);
        check(n.prefix == "N001" && n.id == 1, "NID");
        // Version 0x0A (Seasons+): type, sub-prefix and the later fields.
        w.b.clear();
        w.u32(0x0A).str32("E001").u32(1).u32(1).str32("").u32(0).u32(0x10).u32(0x10).u32(0).u8(1).u8(2).u8(3).u8(0);
        const auto e = NeighborhoodInfo::parse(w.b);
        check(e.version == 0x0A && e.prefix == "E001" && e.type == 1 && e.subPrefix.empty() && e.downtown == 0 &&
                  e.unknown7 == 0x10 && e.unknown8 == 0x10 && e.stealth == 0 &&
                  e.unknownA == std::array<uint8_t, 4>{1, 2, 3, 0},
              "NID 0x0A");
    }
}

static void testLoad() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "os2_test_hood" / "N999";
    fs::remove_all(dir.parent_path());
    fs::create_directories(dir / "Characters");
    fs::create_directories(dir / "Lots");

    constexpr uint32_t G = 0xFFFFFFFF;
    DbpfWriter hood;
    hood.add({hoodtype::NGBH, G, 0, 0}, ngbh(), true);
    hood.add({hoodtype::SDSC, G, 1, 0}, makeSdsc(1, 0x1111, lifestage::Adult, 0, 1, {2}));
    hood.add({hoodtype::SDSC, G, 2, 0}, makeSdsc(2, 0x2222, lifestage::Child, 1, 1, {1}));
    hood.add({hoodtype::SDSC, G, 3, 0}, makeSdsc(3, 0x3333, lifestage::Elder, 0, 0, {}));
    hood.add({hoodtype::FAMI, G, 1, 0}, fami(11, 1000, {0x1111, 0x2222, 0x9999}));
    hood.add({hoodtype::FamilyTies, G, 1, 0}, ties());
    hood.add({hoodtype::SREL, G, 0x00010002, 0}, srel({50, 0xC000, 30, 1}));
    hood.add({hoodtype::SREL, G, 0x00020001, 0}, srel({40, 0xC000, 20, 2}));
    hood.add({hoodtype::LotInfo, G, 11, 0}, lotInfo(11));
    hood.add({0x53545223, G, 1, 0}, strings({"Goth", "A family"}));
    hood.add({0x53545223, G, 0x800B, 0}, strings({"165 Sim Lane", ""}));
    hood.add({0x43545353, G, 1, 0}, strings({"Testview", "A test"}));
    hood.save(dir / "N999_Neighborhood.package");

    DbpfWriter chr;
    chr.add({0x4F424A44, G, 0x80, 0}, objd(0x1111));
    chr.add({0x43545353, G, 0x7D0, 0}, strings({"Mortimer", "Fond of insects", "Goth"}));
    chr.save(dir / "Characters" / "N999_User00000.package");
    DbpfWriter lot;
    lot.add({0x53545223, G, 1, 0}, strings({"x"}));
    lot.save(dir / "Lots" / "N999_Lot11.package");

    const auto h = Neighborhood::load(dir);
    check(h.errors.empty(), "load: no errors");
    for (const auto& e : h.errors)
        std::fprintf(stderr, "  %s\n", e.c_str());
    check(h.prefix == "N999" && h.name == "Testview", "load: prefix and name");
    check(h.neighbors.size() == 3 && h.families.size() == 1 && h.lots.size() == 1, "load: counts");
    const auto* mortimer = h.neighbor(1);
    check(mortimer && mortimer->name() == "Mortimer Goth" && mortimer->character == 0, "load: character name");
    check(mortimer && mortimer->family == 1 && h.neighbor(3)->family == 0, "load: family by FAMI");
    check(h.neighborByGuid(0x2222) == h.neighbor(2), "load: by GUID");
    const auto* f = h.family(1);
    check(f && f->name == "Goth" && f->members == std::vector<uint16_t>{1, 2}, "load: family name and members");
    const auto* l = h.lot(11);
    check(l && l->name == "165 Sim Lane" && l->families == std::vector<uint32_t>{1} && !l->package.empty(),
          "load: lot name, family and file");
    check(h.relationship(1, 2) && h.relationship(1, 2)->value(relvar::Daily) == 50 && !h.relationship(1, 3),
          "load: relationships");
    check(h.familyTree.size() == 2 && h.familyTree[0].from == 1 && h.familyTree[0].type == TieType::Child,
          "load: family tree");
    check(h.neighborInventory(1) && h.neighborInventory(1)->singular.size() == 1 && h.lotInventory(11) &&
              h.familyInventory(1) && !h.neighborInventory(2),
          "load: inventories");
    check(h.decodeCounts.at(hoodtype::SDSC).decoded == 3, "load: decode counts");
    check(findNeighborhoods(dir.parent_path()) == std::vector<fs::path>{dir}, "findNeighborhoods");
    fs::remove_all(dir.parent_path());
}

// The writers give back the bytes the parsers read.
static void testWriters() {
    for (const auto& second : {std::string(), std::string("Temperate")}) {
        const auto b = ngbh(second);
        check(NeighborhoodInventories::parse(b).write() == b, "NGBH writes back as read");
    }
    const auto d = makeSdsc(7, 0xCAFE, lifestage::Adult, 1, 3, {1, 2});
    check(SimDescription::parse(d).write() == d, "SDSC writes back as read");
    const auto f = fami(11, 20000, {0xCAFE, 0xBEEF});
    check(FamilyRecord::parse(f).write() == f, "FAMI writes back as read");
    // cTSRelationshipEntry::ResourceWrite: version, count, values; the
    // stale ones after them are not written.
    check(Relationship::parse(srel({10, 0xC000, -5, 9}, 2), 0x00010002).write() == srel({10, 0xC000, -5, 9}),
          "SREL writes back as the game does, without the stale values");
    FamilyRecord rec = FamilyRecord::parse(f);
    rec.funds = 12345;
    check(FamilyRecord::parse(rec.write()).funds == 12345, "FAMI funds written");

    // cTSNeighborhood::SanifyInventories (docs/save-mac.md §9.3): the
    // inventories of lots, families and neighbors gone go, every one left
    // has one, by ID.
    NeighborhoodInventories n = NeighborhoodInventories::parse(ngbh());
    check(n.tilesX == 128 && n.tilesY == 128, "NGBH tiles");
    n.lots = {Inventory{9, {}, {}}, Inventory{2, {InventoryToken{0x1234, 1, {5}}}, {}}, Inventory{40, {}, {}}};
    n.sanify({2, 3, 9}, {0, 1}, {});
    check(n.lots.size() == 3 && n.lots[0].id == 2 && n.lots[1].id == 3 && n.lots[2].id == 9,
          "lot inventories: one per lot, by ID, the gone one dropped");
    check(n.lots[0].counted.size() == 1 && n.lots[0].counted[0].guid == 0x1234 && n.lots[1].counted.empty(),
          "the kept inventory keeps its tokens, the new one is empty");
    check(n.families.size() == 2 && n.families[0].id == 0 && n.families[1].id == 1, "family inventories: one each");
    check(n.neighbors.empty(), "neighbor inventory of a neighbor gone dropped");
    check(NeighborhoodInventories::parse(n.write()).lots.size() == 3, "sanified NGBH reads back");
}

int main() {
    testDecoders();
    testWriters();
    testLoad();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("neighborhood: all tests passed\n");
    return failures ? 1 : 0;
}
