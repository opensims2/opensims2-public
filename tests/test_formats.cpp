#include <bit>
#include <cmath>
#include "formats/bhav.hpp"
#include "formats/cas_catalog.hpp"
#include "formats/cas_face.hpp"
#include "formats/character_write.hpp"
#include "formats/sim_character.hpp"
#include "formats/dbpf.hpp"
#include "formats/camera_config.hpp"
#include "formats/lighting_config.hpp"
#include "formats/lot.hpp"
#include "formats/lot_edit.hpp"
#include "formats/lot_model.hpp"
#include "formats/lot_diffusion.hpp"
#include "formats/lot_lamps.hpp"
#include "formats/lot_roof.hpp"
#include "formats/lot_water.hpp"
#include "formats/lot_ui_state.hpp"
#include "formats/log_once.hpp"
#include "app/wall_view.hpp"
#include "formats/image.hpp"
#include "formats/model.hpp"
#include "formats/object_shadow.hpp"
#include "formats/sim.hpp"
#include "formats/property_set.hpp"
#include "formats/resindex.hpp"
#include "formats/simantics.hpp"
#include "formats/strings.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#if defined(_WIN32)
#include <io.h> // _dup, _dup2, _fileno, _close
#else
#include <unistd.h>
#endif
#include <filesystem>
#include <fstream>
#include <map>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <zlib.h>

using namespace os2;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static bool pixelIs(const RgbaImage& img, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const uint8_t* p = &img.pixels[(size_t(y) * img.width + x) * 4];
    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

static void testDxt1() {
    // c0 = red (0xF800), c1 = blue (0x001F); row 0 uses indices 0,1,2,3.
    const std::vector<uint8_t> block = {0x00, 0xF8, 0x1F, 0x00, 0b11100100, 0, 0, 0};
    auto img = decodeTexture(TexFormat::Dxt1, 4, 4, block);
    check(pixelIs(img, 0, 0, 255, 0, 0, 255), "dxt1 c0");
    check(pixelIs(img, 1, 0, 0, 0, 255, 255), "dxt1 c1");
    check(pixelIs(img, 2, 0, 170, 0, 85, 255), "dxt1 2/3 c0 + 1/3 c1");
    check(pixelIs(img, 3, 0, 85, 0, 170, 255), "dxt1 1/3 c0 + 2/3 c1");
    check(pixelIs(img, 0, 1, 255, 0, 0, 255), "dxt1 row 1");

    // c0 <= c1 enables the transparent colour for index 3.
    const std::vector<uint8_t> punch = {0x1F, 0x00, 0x00, 0xF8, 0b11000000, 0, 0, 0};
    img = decodeTexture(TexFormat::Dxt1, 4, 4, punch);
    check(img.pixels[3 * 4 + 3] == 0, "dxt1 punch-through alpha");
}

static void testDxt3And5() {
    std::vector<uint8_t> dxt3(16, 0);
    dxt3[0] = 0xF0; // pixel 0 alpha 0, pixel 1 alpha 15
    dxt3[8] = 0xFF, dxt3[9] = 0xFF; // c0 = white
    auto img = decodeTexture(TexFormat::Dxt3, 4, 4, dxt3);
    check(pixelIs(img, 0, 0, 255, 255, 255, 0), "dxt3 alpha 0");
    check(pixelIs(img, 1, 0, 255, 255, 255, 255), "dxt3 alpha 15");

    std::vector<uint8_t> dxt5(16, 0);
    dxt5[0] = 255, dxt5[1] = 0;     // 8-step alpha ramp
    dxt5[2] = 0b010001;             // pixel 0 -> index 1 (a1 = 0), pixel 1 -> index 2
    dxt5[8] = 0xFF, dxt5[9] = 0xFF; // c0 = white
    img = decodeTexture(TexFormat::Dxt5, 4, 4, dxt5);
    check(img.pixels[3] == 0, "dxt5 index 1");
    check(img.pixels[4 + 3] == 218, "dxt5 index 2 = (6*255)/7");
    check(img.pixels[2 * 4 + 3] == 255, "dxt5 index 0");
}

static void testRaw() {
    const std::vector<uint8_t> bgra = {1, 2, 3, 4};
    check(pixelIs(decodeTexture(TexFormat::Raw32, 1, 1, bgra), 0, 0, 3, 2, 1, 4), "raw32 BGRA");
    const std::vector<uint8_t> gray = {7};
    check(pixelIs(decodeTexture(TexFormat::Raw8, 1, 1, gray), 0, 0, 7, 7, 7, 255), "raw8 gray");
    check(pixelIs(decodeTexture(TexFormat::Alpha8, 1, 1, gray), 0, 0, 255, 255, 255, 7), "alpha8");
    bool threw = false;
    try {
        decodeTexture(TexFormat::Dxt1, 8, 8, bgra);
    } catch (const FormatError&) {
        threw = true;
    }
    check(threw, "size mismatch throws");
}

// The game's height map to normal map conversion (GenerateBumpMap_L8_to_A8R8G8B8,
// base 1.0 FUN_0010379c; docs/shaders-mac.md 13.5) with the bump scale 10.
static void testHeightMapToNormalMap() {
    TextureLevels flat;
    flat.format = TexFormat::Raw8;
    flat.levels.push_back({4, 4, std::vector<uint8_t>(16, 200)});
    const TextureLevels fn = heightMapToNormalMap(flat);
    const RgbaImage f = decodeTexture(fn.format, 4, 4, fn.levels[0].data);
    // f = 1020 / 10 = 102, s = the table's 127.5 / sqrt(102^2) (1.2351 by the fast
    // inverse square root): blue 128 + round(102 s) = 254.
    check(fn.format == TexFormat::Raw32 && pixelIs(f, 1, 2, 128, 128, 254, 255), "height map: flat");
    // Heights rising 10 a texel to the right (wrapping at the edge): a = -40,
    // s = 127.5 / sqrt(64 x 25 + 102^2) (1.1508): red 128 - 46, blue 128 + 117.
    TextureLevels ramp;
    ramp.format = TexFormat::Raw8;
    std::vector<uint8_t> h;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            h.push_back(uint8_t(100 + 10 * x));
    ramp.levels.push_back({8, 8, h});
    ramp.levels.push_back({1, 1, {9}}); // every level on its own
    const TextureLevels rn = heightMapToNormalMap(ramp);
    const RgbaImage r = decodeTexture(rn.format, 8, 8, rn.levels[0].data);
    check(rn.levels.size() == 2 && pixelIs(r, 3, 5, 82, 128, 245, 255), "height map: ramp to the right");
    // At the left edge x - 1 wraps to the right edge (170): a = 2 x (170 - 110) = 120.
    check(r.pixels[(5 * 8 + 0) * 4] > 200, "height map: wraps");
    check(pixelIs(decodeTexture(rn.format, 1, 1, rn.levels[1].data), 0, 0, 128, 128, 254, 255), "height map: small level");
    // Other formats pass through.
    TextureLevels dxt;
    dxt.format = TexFormat::Dxt1;
    check(heightMapToNormalMap(dxt).format == TexFormat::Dxt1, "height map: only Raw8");
}

static void testPng() {
    RgbaImage img;
    img.width = 3;
    img.height = 2;
    for (int i = 0; i < 24; ++i)
        img.pixels.push_back(uint8_t(i * 10));
    const auto path = std::filesystem::temp_directory_path() / "os2_test.png";
    writePng(path, img);
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> png((std::istreambuf_iterator<char>(f)), {});
    f.close();
    std::filesystem::remove(path);
    check(png.size() > 8 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G', "png signature");

    // Inflate IDAT and compare with the filter-0 scanlines.
    const std::string bytes(png.begin(), png.end());
    const size_t idat = bytes.find("IDAT");
    check(idat != std::string::npos, "png has IDAT");
    if (idat == std::string::npos)
        return;
    const uint32_t len = uint32_t(png[idat - 4]) << 24 | png[idat - 3] << 16 | png[idat - 2] << 8 | png[idat - 1];
    std::vector<uint8_t> raw(2 * (1 + 3 * 4));
    uLongf rawLen = uLongf(raw.size());
    check(uncompress(raw.data(), &rawLen, &png[idat + 4], len) == Z_OK && rawLen == raw.size(), "png inflates");
    check(raw[0] == 0 && raw[1] == 0 && raw[13] == 0 && raw[14] == 120, "png scanlines");
}

static void testNameHash() {
    // Pairs taken from objects.package.
    check(nameInstanceHash("0xcd7eb376_age4_00_shpe") == 0xFF03600A, "instance hash (shpe)");
    check(nameInstanceHash("6c6b10bc-8e91-45d7-8911-b92defb69a3a_TXMT") == 0xFF07B665, "instance hash is case-insensitive");
}

static std::vector<uint8_t> strHeader(uint16_t format, uint16_t count) {
    std::vector<uint8_t> d(64, 0);
    const char name[] = "test";
    std::copy(name, name + 4, d.begin());
    d.push_back(uint8_t(format)), d.push_back(uint8_t(format >> 8));
    d.push_back(uint8_t(count)), d.push_back(uint8_t(count >> 8));
    return d;
}

static void put(std::vector<uint8_t>& d, const std::string& s) {
    d.insert(d.end(), s.begin(), s.end());
    d.push_back(0);
}

static void testStrings() {
    auto d = strHeader(0xFFFD, 5);
    for (auto [l, v] : std::initializer_list<std::pair<uint8_t, const char*>>{
             {1, "Chair"}, {1, "Table"}, {14, "Cadeira PT"}, {35, "Cadeira"}, {14, "Mesa"}}) {
        d.push_back(l);
        put(d, v);
        put(d, "");
    }
    d.insert(d.end(), 7, 0xA3); // fill padding seen in the game data
    const auto t = StringTable::parse(d);
    check(t.fileName == "test" && t.entries.size() == 5, "STR# entries");
    check(t.get(0) == "Cadeira", "STR# pt-BR");
    check(t.get(1) == "Mesa", "STR# pt-BR falls back to Portuguese");
    check(t.get(1, lang::EnglishUS) == "Table", "STR# English");
    check(t.count(14) == 2, "STR# count per language");

    std::vector<uint8_t> pascal(64, 0);
    pascal.insert(pascal.end(), {0x00, 0x02, 2, 'o', 'n', 3, 'o', 'f', 'f'});
    const auto p = StringTable::parse(pascal);
    check(p.entries.size() == 2 && p.get(1) == "off", "STR# legacy Pascal table");
}

static void testSimAntics() {
    // BCON: u16 count with a flag in bit 15.
    std::vector<uint8_t> bcon(64, 0);
    bcon.insert(bcon.end(), {0x02, 0x80, 0x05, 0x00, 0xFF, 0xFF});
    const auto c = Constants::parse(bcon);
    check(c.flags == 1 && c.values.size() == 2 && c.values[0] == 5 && c.values[1] == -1, "BCON");

    // OBJf: two u32, 'fJBO', count, (guard, action) pairs.
    std::vector<uint8_t> objf(64, 0);
    objf.insert(objf.end(), {0, 0, 0, 0, 0, 0, 0, 0, 'f', 'J', 'B', 'O', 2, 0, 0, 0, 0, 0, 0x00, 0x10, 0x01, 0x10, 0x02, 0x10});
    const auto f = ObjectFunctions::parse(objf);
    check(f.functions.size() == 2 && f.functions[0].action == 0x1000 && f.functions[1].guard == 0x1001, "OBJf");

    // GLOB: Pascal or C string, with optional fill padding.
    std::vector<uint8_t> glob(64, 0);
    glob.insert(glob.end(), {4, 'T', 'e', 's', 't', 0xA3, 0xA3});
    check(SemiGlobal::parse(glob).semiGlobalName == "Test", "GLOB");

    // FCNS: 'SNCF', version 0x4B, 0, count, (Pascal name, float, Pascal description).
    std::vector<uint8_t> fcns(64, 0);
    fcns.insert(fcns.end(), {'S', 'N', 'C', 'F', 0x4B, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 4, 'h', 'o', 'u', 'r',
                             0x00, 0x00, 0x00, 0x41, 0});
    const auto fc = FloatConstants::parse(fcns);
    check(fc.entries.size() == 1 && fc.value("hour", 7.0f) == 8.0f && fc.value("other", 7.0f) == 7.0f, "FCNS");
}

// SLOT by version, as cTSSlotDescriptor::Read has it (round n49, base 1.0
// 0xbc3f1c): x, y in sixteenths of a tile before 0x47; the proximities
// floats from 0x43, sixteenths from 0xA, whole tiles before; the resolution
// sixteenths before 0x47, a float from it; before version 9 the facing from
// the flags 0x200 (-3) and 0x400 (-1).
static void testSlotVersions() {
    auto u32 = [](std::vector<uint8_t>& d, uint32_t v) {
        for (int i = 0; i < 4; ++i)
            d.push_back(uint8_t(v >> (8 * i)));
    };
    auto f32 = [&](std::vector<uint8_t>& d, float v) { u32(d, std::bit_cast<uint32_t>(v)); };
    auto slot = [&](uint32_t version, bool ts2, float x, uint32_t flags, uint32_t prox, float proxF, uint32_t res, float resF) {
        std::vector<uint8_t> d(64, 0);
        if (ts2) {
            d.insert(d.end(), {'T', 'O', 'L', 'S'}), u32(d, version), u32(d, 0);
        } else {
            u32(d, 0), u32(d, version), d.insert(d.end(), {'T', 'O', 'L', 'S'});
        }
        u32(d, 1);
        d.push_back(3), d.push_back(0); // type
        f32(d, x), f32(d, -x), f32(d, 0.5f);
        u32(d, 1), u32(d, 0), u32(d, 0), u32(d, flags), u32(d, uint32_t(-1)); // multipliers, flags, snap
        if (version >= 0x43)
            f32(d, proxF), f32(d, proxF * 2), f32(d, proxF);
        else
            u32(d, prox), u32(d, prox * 2), u32(d, prox);
        u32(d, 0), u32(d, 0);                     // max size, +0x24
        f32(d, 2.0f), u32(d, 4);                  // gradient, height
        if (version >= 9)
            u32(d, 5);                            // facing
        if (version >= 0x47)
            f32(d, resF);
        else if (version >= 0xA)
            u32(d, res);
        if (version >= 0x40)
            u32(d, 7), u32(d, 9);
        return Slots::parse(d);
    };
    const Slots a = slot(0x46, true, 48, 0, 0, 1.0f, 4, 0);
    check(a.slots[0].offset[0] == 3.0f && a.slots[0].offset[1] == -3.0f && a.slots[0].offset[2] == 0.5f &&
              a.slots[0].minDistance == 1.0f && a.slots[0].maxDistance == 2.0f && a.slots[0].resolutionTiles == 0.25f &&
              a.slots[0].facing == 5 && a.slots[0].ts2Extra[0] == 7,
          "SLOT 0x46: x, y and the resolution in sixteenths, the proximities floats");
    const Slots b = slot(0x47, true, 0.7f, 0, 0, 0.5f, 0, 0.25f);
    check(b.slots[0].offset[0] == 0.7f && b.slots[0].minDistance == 0.5f && b.slots[0].resolutionTiles == 0.25f,
          "SLOT 0x47: x, y and the resolution in tiles");
    const Slots c = slot(0xA, false, 16, 0, 32, 0, 8, 0);
    check(c.slots[0].offset[0] == 1.0f && c.slots[0].minDistance == 2.0f && c.slots[0].maxDistance == 4.0f &&
              c.slots[0].resolutionTiles == 0.5f && c.slots[0].ts2Extra[0] == 0 && c.slots[0].ts2Extra[1] == 0xFFFFFFFFu,
          "SLOT 0xA (The Sims 1's layout): the proximities in sixteenths");
    const Slots e = slot(8, false, 0, 0x400, 2, 0, 0, 0);
    check(e.slots[0].minDistance == 2.0f && e.slots[0].facing == -1 && !(e.slots[0].flags & 0x400) &&
              e.slots[0].gradient == 0.125f,
          "SLOT 8: whole-tile proximities, the facing from flag 0x400, the gradient in sixteenths");
}

static void testInteractionTable() {
    // Version 0x48, one interaction with 16 advertisements for the first age
    // group, then 96 zero bytes and the name, as in the game's "Reaction
    // Interaction Table" (group 0x7F9FC607).
    std::vector<uint8_t> d(64, 0);
    auto u16 = [&](uint32_t v) { d.insert(d.end(), {uint8_t(v), uint8_t(v >> 8)}); };
    auto u32 = [&](uint32_t v) { u16(v & 0xFFFF), u16(v >> 16); };
    u32(0xFFFFFFFF), u32(0x48), u32(0), u16(1);
    u16(0x2019), u16(0x0109);
    u32(16);
    for (int i = 0; i < 6; ++i)
        u32(0);
    d.insert(d.end(), 2 + 2 + 4 + 4 + 4 + 4 + 4 + 2 + 4, 0);
    d.insert(d.end(), 16 * 6 + 96, 0);
    const std::string name = "Reaction Interaction Table";
    d.push_back(uint8_t(name.size()));
    d.insert(d.end(), name.begin(), name.end());
    const auto t = InteractionTable::parse(d);
    check(t.interactions.size() == 1 && t.interactions[0].action == 0x2019 && t.name == name,
          "TTAB with zero padding before the name");
}

static void testInteractionTableVersions() {
    // 0x4F..0x51 keep the 0x4E entry layout byte for byte: the AL content has
    // 276 of them (149/106/21) and all parse to the trailing name with the
    // same fields (docs/data-findings.md section 17). 0x53 keeps that layout
    // with 11 motive tables instead of 7 and its 7 AL resources close to the
    // trailing name too (section 19). 0x52 never shows up in the UC content
    // and nothing above 0x55 does, so both are refused.
    auto table = [](uint32_t version) {
        std::vector<uint8_t> d(64, 0);
        auto u16 = [&](uint32_t v) { d.insert(d.end(), {uint8_t(v), uint8_t(v >> 8)}); };
        auto u32 = [&](uint32_t v) { u16(v & 0xFFFF), u16(v >> 16); };
        u32(0x54544142), u32(version), u32(0), u16(1);
        u16(0x1002), u16(0x1003); // action, guard
        // 0x53 has 11 motive tables, the older versions 7; the first one holds
        // two advertisements.
        u32(2);
        for (uint32_t i = 1; i < (version == 0x53 ? 11u : 7u); ++i)
            u32(0);
        u16(0x0001), u16(0x8000);        // flags, flags2
        u32(7), u32(1), u32(0x40000000); // string index, attenuation code/value 2.0
        u32(50), u32(0xFFFFFFFF);        // autonomy, join index
        u16(3), u32(0), u32(0x3F000000), u32(0), u32(1); // 0x45/0x46/0x4A/0x4C fields
        u16(10), u16(2), u16(1);                          // advertisements: min, delta, personality
        u16(20), u16(3), u16(2);
        const std::string name = "Version Layout Table";
        d.push_back(uint8_t(name.size()));
        d.insert(d.end(), name.begin(), name.end());
        return d;
    };
    for (uint32_t v : {0x4E, 0x4F, 0x50, 0x51, 0x53}) {
        const auto t = InteractionTable::parse(table(v));
        const auto& e = t.interactions.at(0);
        check(t.version == v && t.interactions.size() == 1, "TTAB version from 0x4E to 0x53 accepted");
        check(e.action == 0x1002 && e.guard == 0x1003 && e.autonomy == 50 && e.joinIndex == -1 &&
                  e.uiDisplayType == 3 && e.memoryMultiplier == 0.5f && e.modelTableId == 1,
              "TTAB 0x4E..0x53 entries share the layout");
        const size_t tables = v == 0x53 ? 11 : 7;
        check(e.motives.size() == tables && e.motives[0].size() == 2 && e.motives[0][1].minimum == 20 &&
                  e.motives[0][1].delta == 3 && e.motives[0][1].personality == 2,
              "TTAB 0x4E..0x53 advertisements");
        check(t.name == "Version Layout Table", "TTAB 0x4E..0x53 trailing name");
    }
    for (uint32_t v : {0x52, 0x56}) {
        bool threw = false;
        try {
            InteractionTable::parse(table(v));
        } catch (const FormatError&) {
            threw = true;
        }
        check(threw, "TTAB 0x52 and above 0x55 are refused");
    }

    // 0x54 and 0x55 changed the entry again: 46 fixed bytes (the counts of
    // the 0x4E layout are gone) and then two arrays of advertisement groups.
    // The first group of each array is a count of advertisements; the second
    // array's group starts with a count of 4-byte items that are still
    // unknown, then its own count of advertisements.
    auto table54 = [](uint32_t version) {
        std::vector<uint8_t> d(64, 0);
        auto u16 = [&](uint32_t v) { d.insert(d.end(), {uint8_t(v), uint8_t(v >> 8)}); };
        auto u32 = [&](uint32_t v) { u16(v & 0xFFFF), u16(v >> 16); };
        auto i16 = [&](int32_t v) { u16(uint16_t(v)); };
        u32(0x54544142), u32(version), u32(0), u16(1);
        u16(0x1002), u16(0x1003);                 // action, guard
        u16(0x0001), u16(0x8000);                 // flags, flags2
        u32(7), u32(1), u32(0x40000000);          // string index, attenuation code/value 2.0
        u32(50), u32(0xFFFFFFFF);                 // autonomy, join index
        u16(3), u32(0), u32(0x3F000000), u32(0), u32(1); // ui, facial, memory, object, model
        u32(1);                                   // first array: one group
        u32(2);                                   // with two advertisements
        i16(10), i16(2), u16(1);
        i16(20), i16(3), u16(2);
        u32(1);                                   // second array: one group
        u32(3);                                   // with three 4-byte items (two unknown, skipped)
        u32(0xDEADBEEF), u32(0x12345678);
        u32(1);                                   // and one advertisement
        i16(30), i16(4), u16(3);
        const std::string name = "Version Layout Table";
        d.push_back(uint8_t(name.size()));
        d.insert(d.end(), name.begin(), name.end());
        return d;
    };
    for (uint32_t v : {0x54, 0x55}) {
        const auto t = InteractionTable::parse(table54(v));
        const auto& e = t.interactions.at(0);
        check(t.version == v && t.interactions.size() == 1, "TTAB 0x54 and 0x55 accepted");
        check(e.action == 0x1002 && e.guard == 0x1003 && e.autonomy == 50 && e.joinIndex == -1 &&
                  e.uiDisplayType == 3 && e.memoryMultiplier == 0.5f && e.modelTableId == 1,
              "TTAB 0x54..0x55 entries keep the 0x4E fields");
        check(e.motives.size() == 2 && e.motives[0].size() == 2 && e.motives[0][1].minimum == 20 &&
                  e.motives[0][1].delta == 3 && e.motives[0][1].personality == 2,
              "TTAB 0x54..0x55 first array of advertisement groups");
        check(e.motives[1].size() == 1 && e.motives[1][0].minimum == 30 && e.motives[1][0].delta == 4 &&
                  e.motives[1][0].personality == 3,
              "TTAB 0x54..0x55 second array of advertisement groups");
        check(t.name == "Version Layout Table", "TTAB 0x54..0x55 trailing name");
    }
}

static void testBhav() {
    std::vector<uint8_t> d(64, 0);
    d.insert(d.end(), {0x07, 0x80, 2, 0, 0, 1, 2, 0, 0, 0, 0, 0}); // 0x8007, 2 instructions, 1 arg, 2 locals
    auto instr = [&](uint16_t op, uint16_t t, uint16_t f) {
        d.insert(d.end(), {uint8_t(op), uint8_t(op >> 8), uint8_t(t), uint8_t(t >> 8), uint8_t(f), uint8_t(f >> 8), 0});
        d.insert(d.end(), 16, 0);
    };
    instr(0x0002, 1, 0xFFFE);      // Expression: true -> 1, false -> return false
    instr(0x2010, 0xFFFD, 0xFFFC); // semi-global call: true -> return true, false -> error
    const auto b = Behavior::parse(d);
    check(b.argCount == 1 && b.localCount == 2 && b.instructions.size() == 2, "BHAV header");
    check(b.instructions[0].onTrue.kind == Behavior::TargetKind::Instruction && b.instructions[0].onTrue.index == 1,
          "BHAV branch to instruction");
    check(b.instructions[0].onFalse.kind == Behavior::TargetKind::False, "BHAV return false");
    check(b.instructions[1].onFalse.kind == Behavior::TargetKind::Error, "BHAV error target");
    check(b.disassemble().find("call semiglobal 0x2010") != std::string::npos, "BHAV disassembly");

    d[64 + 12 + 2] = 5; // branch past the end
    bool threw = false;
    try {
        Behavior::parse(d);
    } catch (const FormatError&) {
        threw = true;
    }
    check(threw, "BHAV rejects out-of-range branches");

    // 0x8009 (Apartment Life and later) adds one header byte after the tree
    // version: 0 in 28,988 of the AL package's 29,014 trees. Same tree parses.
    std::vector<uint8_t> e(64, 0);
    e.insert(e.end(), {0x09, 0x80, 2, 0, 0, 1, 2, 0, 0x0A, 0x80, 0xFF, 0xFF, 0x00});
    auto push = [&](uint16_t op, uint16_t t, uint16_t f) {
        e.insert(e.end(), {uint8_t(op), uint8_t(op >> 8), uint8_t(t), uint8_t(t >> 8), uint8_t(f), uint8_t(f >> 8), 0});
        e.insert(e.end(), 16, 0);
    };
    push(0x0002, 1, 0xFFFE);      // Expression: true -> 1, false -> return false
    push(0x2010, 0xFFFD, 0xFFFC); // semi-global call: true -> return true, false -> error
    const auto b9 = Behavior::parse(e);
    check(b9.instructions.size() == 2 && b9.instructions[0].onTrue.index == 1 &&
              b9.instructions[1].onFalse.kind == Behavior::TargetKind::Error && (b9.treeVersion & 0xFFFF) == 0x800A,
          "BHAV 0x8009 header byte");
}

static void testCameraConfig() {
    const char* text = R"(# comment
version 1
sinclude "missing.txt"
camera "Live" Live
zoom        1 (70   40   25)
fov           (45   45   45)
pitch         (30   22   14)   (40  45  50)
orientation 1 (10 55 100)   # trailing comment
smoothing   5 8
setparam KeyboardTranslationSpeed 50
if (maxVersion() >= 3)
   setparam keyboardtranslationspeed 99
endif
if (maxVersion() >= 4)
   setparam keyboardtranslationspeed 7
endif
if (epVersion() >= 2)
   setparam keyboardtranslationspeed 8
endif
camera     "CAS" CreateASim
clip       0.2 2000
)";
    auto cams = parseCameraConfig(text);
    check(cams.size() == 2, "camera count");
    const CameraDef* live = findCamera(cams, "live");
    check(live && live->controller == "Live", "find camera by name");
    if (live) {
        check(live->defaultZoom == 1 && live->distances.size() == 3 && live->distances[1] == 40, "zoom table");
        check(live->pitchMin[2] == 14 && live->pitchMax[2] == 50, "pitch tables");
        check(live->defaultOrientation == 1 && live->yaws.size() == 3 && live->yaws[2] == 100, "orientation");
        check(live->zoomSmoothing == 5 && live->spinSmoothing == 8, "smoothing");
        check(live->param("keyboardtranslationspeed", 0) == 99, "setparam in maxVersion() >= 3, later checks skipped");
    }
    const CameraDef* cas = findCamera(cams, "CAS");
    check(cas && cas->nearClip.size() == 1 && cas->nearClip[0] == 0.2f && cas->farClip[0] == 2000, "scalar clip");
}

static void testLightingConfig() {
    const char* text = R"(# Lighting Manager settings
set outsideFloorAmbient                0.25
setb morningEvening false
lightingState day
   control -edith true
   skylight                      (1,1,1)  0.5
   light sun      (1.5, 1.2, 2)    (1, .94, .875)     2
   light fill1    (1, 1, -1)     (1,0.8,.9)    .5
   light simFill1  (1, -1.2, 1) (1, .95, .89)  .5
   modifiers insidePortals       0.5  .7          1.3    1  #### trailing comment
  projectiveShadowStrength 0.73
end
lightingState dayNoPortals : day
   light sun      (0, 0, 1)    (1, 1, 1)     1
end
)";
    auto cfg = parseLightingConfig(text);
    check(cfg.globals["outsidefloorambient"] == "0.25" && cfg.globals["morningevening"] == "false", "lighting globals");
    check(cfg.states.size() == 2, "lighting state count");
    const LightingState* day = cfg.state("DAY");
    check(day && day->skyStrength == 0.5f && day->skyColor[2] == 1, "skylight");
    check(day && day->projectiveShadowStrength == 0.73f && cfg.state("dayNoPortals")->projectiveShadowStrength == 0.73f,
          "projectiveShadowStrength, inherited");
    if (day) {
        check(day->lights.size() == 3 && day->lights[0].name == "sun" && day->lights[0].direction[1] == 1.2f &&
                  day->lights[0].color[2] == 0.875f && day->lights[0].intensity == 2,
              "light with comma lists");
        auto objects = day->objectLights();
        check(objects.size() == 2 && objects[1].name == "fill1" && objects[1].direction[2] == -1,
              "object lights skip simFill");
    }
    if (day) {
        auto it = day->modifiers.find("insideportals");
        check(it != day->modifiers.end() && it->second.practical == 0.5f && it->second.practicalFalloff == 1.3f,
              "lighting modifiers");
    }
    const auto overrides = parseLightOverrides(R"(define Four(lightName)
	lightAttribOverride &lightName intensity 1
	lightAttribOverride &lightName falloffRate 1 # comment
enddef
define Amber(lightName)
        lightAttribOverride &lightName color (1,0.9,0.76)
enddef
create Four(lightingWallValue_light)
create Amber(lightingWallValue_light)
lightAttribOverride gardenUplight coneAngle 35
)");
    {
        auto it = overrides.find("lightingwallvalue_light");
        check(it != overrides.end() && it->second.count("intensity") && it->second.at("intensity")[0] == 1 &&
                  it->second.count("color") && it->second.at("color").size() == 3 && it->second.at("color")[1] == 0.9f,
              "light overrides from macros");
        check(overrides.count("gardenuplight") && overrides.at("gardenuplight").at("coneangle")[0] == 35,
              "plain light override");
    }
    const LightingState* np = cfg.state("dayNoPortals");
    check(np && np->parent == "day" && np->skyStrength == 0.5f && np->lights.size() == 3 &&
              np->lights[0].intensity == 1 && np->lights[0].direction[2] == 1,
          "inherited state overrides a light by name");

    // A room's lights (FUN_0070daa8): an unlit room at night (unlitRoomColor
    // (0.345, 0.345, 0.42) on the four sides and the floor, nothing from the
    // ceiling), objects' tuning: ambient = the mean of the six, and the four
    // Deltas by |colour - ambient|: the ceiling and floor ones (darker than
    // the mean) come first and are negative, then two sides.
    {
        RoomLightTuning t;
        t.overall = 0.5f, t.directionalBoost = 4;
        const std::array<float, 3> u{0.345f, 0.345f, 0.42f};
        const RoomLights rl = roomLights({u, u, u, u, u, std::array<float, 3>{0, 0, 0}}, t);
        const float side = (0.345f + 0.345f) * 1.2f * 0.5f, up = (0.345f + 4 * 0.345f) * 0.25f * 0.5f,
                    down = 4 * 0.345f * 0.25f * 0.5f, mean = (4 * side + up + down) / 6;
        check(std::fabs(rl.ambient[0] - mean) < 1e-5f, "room lights: ambient is the mean");
        check(rl.deltas.size() == 4, "room lights: four Deltas");
        if (rl.deltas.size() == 4) {
            check(std::fabs(rl.deltas[0].color[0] - (down - mean) * 4) < 1e-5f && rl.deltas[0].color[0] < 0 &&
                      rl.deltas[0].towardLight[2] == 1,
                  "room lights: the ceiling's Delta is first and negative (towards the light: up)");
            check(rl.deltas[1].color[0] < 0 && rl.deltas[1].towardLight[2] == -1, "room lights: then the floor's, negative");
            check(rl.deltas[2].color[0] > 0 && rl.deltas[2].towardLight[0] < 0 && rl.deltas[2].towardLight[2] > 0,
                  "room lights: then +X travelling (from -X, sloped down), positive");
            check(rl.deltas[3].towardLight[0] > 0, "room lights: then -X");
        }
        t.type = 2; // fewer than 3: the sides only
        const RoomLights two = roomLights({u, u, u, u, u, std::array<float, 3>{0, 0, 0}}, t);
        check(two.deltas.size() == 2 && two.deltas[0].towardLight[2] > 0 && two.deltas[0].towardLight[0] < 0,
              "room lights: type 2 picks among the sides");
    }
}

static void testStandardMaterial() {
    MaterialDefinition m;
    m.materialType = "StandardMaterial";
    m.properties = {{"stdMatAlphaBlendMode", "blend"},       {"stdMatAlphaMultiplier", "0.300000"},
                    {"stdMatBaseTextureEnabled", "true"},    {"stdMatBaseTextureName", "chair-shadow-alpha"},
                    {"stdMatCullMode", "cullClockwise"},     {"stdMatDiffCoef", "0,0.5,1"},
                    {"stdMatLightingEnabled", "0"},          {"stdMatAlphaTestEnabled", "1"},
                    {"stdMatAlphaRefValue", "51"},          {"stdMatSpecCoef", "0.5,0.5,0.25"},
                    {"stdMatSpecPower", "1e+002"},          {"stdMatEmissiveCoef", "0,0.1,0"},
                    {"stdMatUntexturedDiffAlpha", "0.78"}};
    auto sm = standardMaterial(m);
    check(sm.specular[2] == 0.25f && sm.specularPower == 100 && sm.emissive[1] == 0.1f && sm.untexturedAlpha == 0.78f,
          "material specular, emissive, untextured alpha");
    check(sm.baseTexture == "chair-shadow-alpha_txtr", "material base texture");
    check(sm.blend == StandardMaterial::Blend::Alpha && sm.alphaMultiplier == 0.3f, "material blend");
    check(sm.diffuse[0] == 0 && sm.diffuse[1] == 0.5f && sm.diffuse[2] == 1, "material diffuse");
    check(!sm.lighting && sm.alphaTest && sm.alphaRef == 51.0f / 255.0f, "material flags");
    check(sm.cull == StandardMaterial::Cull::Clockwise && sm.visible, "material cull");

    MaterialDefinition none;
    none.materialType = "Null";
    auto ns = standardMaterial(none);
    check(!ns.visible && ns.baseTexture.empty() && ns.blend == StandardMaterial::Blend::None && ns.lighting,
          "null material defaults");
    none.properties = {{"stdMatAlphaBlendMode", "additive"}, {"stdMatCullMode", "none"},
                       {"stdMatBaseTextureEnabled", "false"}, {"stdMatBaseTextureName", "x"}};
    ns = standardMaterial(none);
    check(ns.blend == StandardMaterial::Blend::Additive && ns.cull == StandardMaterial::Cull::None &&
              ns.baseTexture.empty(),
          "additive, no cull, texture disabled");

    MaterialDefinition add;
    add.materialType = "StandardMaterial";
    add.properties = {{"stdMatAlphaBlendMode", "additive"}};
    check(!standardMaterial(add).lighting, "additive StandardMaterial is unlit (DetermineLightingMode)");
    add.properties = {{"stdMatAlphaBlendMode", "addNoSrcAlphaScale"}};
    const auto one = standardMaterial(add);
    check(one.blend == StandardMaterial::Blend::AddNoSrcAlpha && one.lighting, "addNoSrcAlphaScale adds one + one, lit");
    MaterialDefinition water;
    water.materialType = "WaterAnimatingTextures";
    water.properties = {{"stdMatBaseTextureName", "bathtubwater-water"}, {"stdMatBaseTextureEnabled", "true"},
                        {"stdMatSpecPower", "20"}, {"stdMatSpecCoef", "0.5,0.5,0.5"}, {"stdMatAlphaBlendMode", "none"}};
    const auto wm = standardMaterial(water);
    check(wm.shading == StandardMaterial::Shading::Water && wm.blend == StandardMaterial::Blend::Alpha &&
              wm.specularPower == 0 && wm.waterSpeed == 0.3f && !wm.depthWrite,
          "WaterAnimatingTextures as LowEndWater");
    MaterialDefinition wallpaper;
    wallpaper.materialType = "Wallpaper";
    wallpaper.properties = {{"stdMatDiffCoef", "0.5,0.5,0.5,1"}};
    const auto wp = standardMaterial(wallpaper);
    check(wp.lightMapped && wp.diffuse[0] == 1, "walls are light-mapped and ignore the diffuse");
    check(wp.caustics == StandardMaterial::Caustics::None && wp.waterBump[0].empty(), "no caustics on a plain wall");

    // The pool's caustics (docs/roof-fence-water-mac.md 3.3): FloorPool's
    // floorCausticsPass, a wall's tsCausticsPass or WallpaperAndMask's own
    // test of the blue pool's paper (the lots' "_base" TXMTs have no flag).
    MaterialDefinition poolFloor;
    poolFloor.materialType = "FloorPool";
    poolFloor.properties = {{"stdMatBaseTextureName", "floor-poolroundedlipblue"}, {"floorCausticsPass", "1"}};
    check(standardMaterial(poolFloor).caustics == StandardMaterial::Caustics::Floor, "caustics: pool floor");
    poolFloor.properties.pop_back();
    check(standardMaterial(poolFloor).caustics == StandardMaterial::Caustics::None, "caustics: only with the flag");
    MaterialDefinition poolWall;
    poolWall.materialType = "Wallpaper";
    poolWall.properties = {{"stdMatBaseTextureName", "wall-poolroundedlipblue-base"}};
    check(standardMaterial(poolWall).caustics == StandardMaterial::Caustics::Wall, "caustics: the blue pool's wall");
    poolWall.properties = {{"stdMatBaseTextureName", "wall-poolsquarelipgreen-base"}};
    check(standardMaterial(poolWall).caustics == StandardMaterial::Caustics::None, "caustics: not the green pool's");
    poolWall.materialType = "WallpaperPool";
    poolWall.properties.push_back({"tsCausticsPass", "1"});
    check(standardMaterial(poolWall).caustics == StandardMaterial::Caustics::Wall, "caustics: tsCausticsPass");
    MaterialDefinition terrainWater;
    terrainWater.materialType = "TerrainWater";
    const auto ws = standardMaterial(terrainWater);
    check(ws.waterBump[0] == "water-1-bump_txtr" && ws.waterBump[1] == "water-2-bump_txtr",
          "terrain water: the second pass's bump textures");

    // standard.matShad defaults: no specular without a power; blended parts
    // go 7 layers up and stop writing depth.
    MaterialDefinition plain;
    plain.materialType = "StandardMaterial";
    plain.properties = {{"stdMatSpecCoef", "0.5,0.5,0.5"}, {"stdMatAlphaBlendMode", "blend"}, {"stdMatLayer", "1"}};
    auto ps = standardMaterial(plain);
    check(ps.specularPower == 0 && ps.layer == 15 && !ps.depthWrite && ps.envCube.empty() && ps.normalMap.empty(),
          "standard defaults and layer");
    plain.properties = {{"stdMatEnvCubeMode", "reflection"}, {"stdMatEnvCubeCoef", "0.1,0.1,0.1"},
                        {"stdMatEnvCubeTextureName", "reflectionkitchenhighcontrast-envcube"},
                        {"stdMatNormalMapTextureEnabled", "true"}, {"stdMatNormalMapTextureName", "wall-bump"},
                        {"stdMatTextureCoordAnimMode", "transform"},
                        {"stdMatTextureCoordTfAnimRotSpeed", "16.000000"},
                        {"stdMatTextureCoordTfAnimRotStartEnd", "0.000000,0.040000"},
                        {"stdMatTextureCoordTfAnimRotWaveform", "sawtooth"}};
    ps = standardMaterial(plain);
    check(ps.envCube == "reflectionkitchenhighcontrast-envcube_txtr" && ps.envCubeCoef[1] == 0.1f &&
              ps.normalMap == "wall-bump_txtr" && ps.layer == 0 && ps.depthWrite,
          "env cube and normal map");
    check(ps.texAnim == StandardMaterial::TexAnim::Transform && ps.rotation.speed == 16 &&
              ps.rotation.end[0] == 0.04f && ps.rotation.waveform == StandardMaterial::Waveform::Sawtooth &&
              ps.scale.speed == 0,
          "texture coordinate animation");
    plain.properties[1].second = "0.001,0.001,0"; // below CheckForCubeMapping's threshold
    check(standardMaterial(plain).envCube.empty(), "weak env cube is off");

    // Deprecated types (deprecated.matShad).
    MaterialDefinition phong;
    phong.materialType = "PhongTextureAlphaCubeMapReflection";
    phong.properties = {{"stdMatBaseTextureName", "glass"}, {"stdMatDiffCoef", "0.5,0.5,0.5,0.25"},
                        {"stdMatSpecCoef", "0.2,0.4,0.6"}, {"reflectivity", "0.5"},
                        {"deprecatedStdMatInvDiffuseCoeffMultiplier", "1.2"}};
    auto pm = standardMaterial(phong);
    check(pm.baseTexture == "glass_txtr" && pm.blend == StandardMaterial::Blend::Alpha && pm.alphaMultiplier == 0.25f &&
              std::abs(pm.diffuse[0] - 0.6f) < 1e-6f && std::abs(pm.envCubeCoef[2] - 0.3f) < 1e-6f &&
              pm.envCube == "reflectionCubeTemplate-envcube_txtr" && pm.layer == 7,
          "deprecated phong type");
    phong.materialType = "AdditiveTextureAlphaBlend";
    phong.properties.push_back({"blendStrength", "0.5"});
    pm = standardMaterial(phong);
    check(pm.blend == StandardMaterial::Blend::Additive && !pm.lighting && pm.alphaReplicate &&
              pm.alphaMultiplier == 0.5f && pm.envCube.empty(),
          "deprecated additive type");

    // Sim hair: blended, but alpha tested at 60 and writing depth, 3 layers
    // up; SimHair is what the outfit builder makes of a hair part's
    // SimStandardMaterials (markHairPart), not the texture's name.
    MaterialDefinition hair;
    hair.materialType = "SimStandardMaterial";
    hair.properties = {{"stdMatBaseTextureEnabled", "true"}, {"stdMatBaseTextureName", "ufhairlong-red"},
                       {"stdMatAlphaBlendMode", "blend"}, {"stdMatSpecPower", "2"}};
    auto plainHair = standardMaterial(hair);
    check(!plainHair.hair && plainHair.layer == 7 && !plainHair.depthWrite, "SimStandardMaterial outside a hair part");
    ObjectModel hairPart;
    hairPart.materials["hair"] = hair;
    MaterialDefinition scalp;
    scalp.materialType = "SimSkin";
    hairPart.materials["scalp"] = scalp;
    markHairPart(hairPart);
    check(hairPart.materials["hair"].materialType == "SimHair" && hairPart.materials["scalp"].materialType == "SimSkin",
          "hair part: SimStandardMaterial -> SimHair");
    auto hm = standardMaterial(hairPart.materials["hair"]);
    check(hm.shading == StandardMaterial::Shading::Sim && hm.layer == 3 && hm.depthWrite && hm.alphaTest &&
              hm.alphaRef == 61.0f / 255.0f && hm.specularPower == 0 && hm.hair,
          "sim hair");
    check(standardMaterial(hairPart.materials["scalp"]).hair, "hair part's scalp drawn as hair");
}

static void testPropertySet() {
    const auto p = parsePropertySet(R"(<?xml version="1.0"?>
<cGZPropertySetString>
   <AnyUint32 key="guid" type="0xeb61e4f7">3435290776</AnyUint32>
   <AnyString key="name" type="0x0b8bea18">&quot;Soft Rock&quot; Faux &amp; Stone</AnyString>
   <AnyString key="material" type="0x0b8bea18">floor_loftcoralsandstone</AnyString>
</cGZPropertySetString>)");
    check(p.size() == 3 && p.at("guid") == "3435290776", "property set guid");
    check(p.at("name") == "\"Soft Rock\" Faux & Stone", "property set entities");
    check(p.at("material") == "floor_loftcoralsandstone", "property set material");
}

static void testLotModel() {
    // 2x1 lot, one level of elevations at 0 and a second at 3; one floor tile
    // with only its north and east quarters, and one wall along x = 1.
    Lot lot;
    lot.width = 2, lot.height = 1, lot.levelCount = 2;
    lot.elevations.assign(3 * 2 * 2, 0.0f);
    for (size_t i = 6; i < 12; ++i)
        lot.elevations[i] = 3.0f;
    lot.floors.assign(2 * 1 * 2, {});
    lot.floors[0] = {0, 1, 1, 0};
    lot.floorPatterns[1] = "tile";
    lot.wallPatterns[1] = "paper";
    Lot::Wall w;
    w.a = {1, 0, 0}, w.b = {1, 1, 0};
    w.type = Lot::Normal;
    w.patterns = {1, 0xFFFF};
    lot.walls.push_back(w);
    check(lot.elevation(1, 2, 1) == 3.0f && lot.elevation(0, 5, 5) == 0.0f, "lot elevation lookup and clamp");

    const auto empty = std::filesystem::temp_directory_path() / "os2_test_empty_res";
    std::filesystem::create_directories(empty);
    ResourceIndex index(empty);
    const PatternCatalog patterns(index, empty);
    const ObjectModel m = buildLotModel(lot, index, patterns);
    const ObjectModel::Part* floor = nullptr;
    const ObjectModel::Part* wall = nullptr;
    for (const auto& part : m.parts) {
        if (part.node == "floor:tile")
            floor = &part;
        if (part.node == "wall:paper")
            wall = &part;
    }
    check(floor && floor->mesh.triangles.size() == 6, "two floor quarters");
    if (floor) {
        bool upAndCcw = true, northEast = true;
        for (size_t t = 0; t < floor->mesh.triangles.size(); t += 3) {
            const auto& a = floor->mesh.positions[floor->mesh.triangles[t]];
            const auto& b = floor->mesh.positions[floor->mesh.triangles[t + 1]];
            const auto& c = floor->mesh.positions[floor->mesh.triangles[t + 2]];
            const float cross = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
            upAndCcw = upAndCcw && cross > 0 && floor->mesh.normals[floor->mesh.triangles[t]][2] > 0.99f;
            northEast = northEast && a[0] + b[0] + c[0] >= 1.5f - 1e-4f && a[1] + b[1] + c[1] >= 1.5f - 1e-4f;
        }
        check(upAndCcw, "floor triangles face up, counter-clockwise");
        check(northEast, "floor quarters are the north and east ones");
    }
    // Pattern 0 is the right side of a -> b: +X for a wall running +Y; the
    // face is two quads (start, middle, end columns) and, the wall's start
    // meeting no other wall, a cap there (the end's cap belongs to the left
    // face, which has no covering here).
    check(wall && wall->mesh.triangles.size() == 18 && wall->mesh.normals[0][0] == 1.0f, "wall right side faces +X");
    if (wall) {
        float top = 0;
        for (const auto& p : wall->mesh.positions)
            top = std::max(top, p[2]);
        // The node's height offset, -0.01 (cWallGeometryNode, Mac BV 0x74ab40).
        check(std::abs(top - 2.99f) < 1e-5f, "wall reaches the next level (less the node's offset)");
    }
    check(wall && wall->mesh.uvs2.size() == wall->mesh.positions.size(), "walls get light-map coordinates");
    check(floor && floor->mesh.uvs2.size() == floor->mesh.positions.size(), "floors get light-map coordinates");

    // Every lot (not only one with a pool) gets earth sides down to
    // lotSkirtBottom, 1.5 under its lowest terrain point, so what lies under
    // the ground (deck supports, buried stairs) never shows past its edges.
    {
        Lot hill = lot;
        hill.terrainBase = "lttest";
        hill.elevations[0] = -0.25f; // grid point (0, 0) of level 0
        check(lotSkirtBottom(hill) == -0.25f - kSkirtDepth, "skirt bottom under the lowest terrain point");
        const ObjectModel hm = buildLotModel(hill, index, patterns);
        float sideBottom = 1e9f;
        size_t sideTris = 0;
        for (const auto& part : hm.parts)
            if (part.material == "terrain:lttest")
                for (size_t t = 0; t < part.mesh.triangles.size(); t += 3) {
                    const auto& n = part.mesh.normals[part.mesh.triangles[t]];
                    if (std::abs(n[2]) > 0.01f)
                        continue;
                    ++sideTris;
                    for (int k = 0; k < 3; ++k)
                        sideBottom = std::min(sideBottom, part.mesh.positions[part.mesh.triangles[t + k]][2]);
                }
        check(sideTris == 2 * 2 * (2 + 1) && sideBottom == lotSkirtBottom(hill), "a lot without a pool gets its skirt");
    }

    // Wall views: walls down cut the wall to its stub, a level below the
    // wall's hides it, and the light-map atlas is the same for every view.
    auto wallTop = [&](const ObjectModel& model) {
        float top = -1;
        for (const auto& part : model.parts)
            if (part.node == "wall:paper")
                for (const auto& p : part.mesh.positions)
                    top = std::max(top, p[2]);
        return top;
    };
    LotLightMapLayout upLayout, downLayout, hiddenLayout;
    buildLotModel(lot, index, patterns, {}, &upLayout);
    LotViewOptions down;
    down.walls = LotViewOptions::Walls::Down;
    LotModelCache cache;
    const ObjectModel dm = buildLotModel(lot, index, patterns, down, &downLayout, &cache);
    check(wallTop(dm) > 0 && wallTop(dm) < 0.5f, "walls down: a low stub");
    check(cache.holes.has_value(), "the cache keeps the wall holes");
    down.level = 0;
    check(wallTop(buildLotModel(lot, index, patterns, down, nullptr, &cache)) < 0.5f, "walls down on the wall's level");
    down.level = 1; // looking at the level above: this level's walls stay up
    check(std::abs(wallTop(buildLotModel(lot, index, patterns, down, nullptr, &cache)) - 2.99f) < 1e-5f,
          "lower levels keep their walls up");
    down.level = -1;
    check(wallTop(buildLotModel(lot, index, patterns, down, &hiddenLayout, &cache)) < 0, "levels above the view are hidden");
    check(upLayout.walls.size() == downLayout.walls.size() && downLayout.walls.size() == hiddenLayout.walls.size() &&
              !upLayout.walls.empty() && upLayout.walls[0].h == downLayout.walls[0].h &&
              upLayout.walls[0].top == hiddenLayout.walls[0].top && upLayout.height == downLayout.height,
          "the light-map atlas does not depend on the view");
    check(lotTopLevel(lot) == 0, "top level: the highest with walls or floors");
    {
        Lot gabled = lot;
        Lot::Wall g = gabled.walls[0];
        g.a.level = g.b.level = 1;
        g.type = Lot::Attic;
        gabled.walls.push_back(g);
        check(lotTopLevel(gabled) == 0, "top level: not the gables (attic walls) over the roof's floor");
    }

    // WallView: the model is rebuilt for another mode, roof or level (what
    // the cutaway cuts is compared apart: lotCutawayWalls).
    WallView wv;
    check(wv.walls() == LotViewOptions::Walls::Cutaway, "lots start in cutaway");
    const LotViewOptions a = wv.options({1, 0.2f}, 0), b = wv.options({1, -0.4f}, 0);
    check(WallView::sameCut(a, b), "the camera alone does not change the model's mode");
    check(!WallView::sameCut(a, wv.options({1, 0.2f}, 1)), "another level rebuilds");
    check(wv.mode() == 1 && !wv.roof() && !wv.options({1, 0}, 0).roof, "cutaway shows no roof");
    wv.cycle(1);
    check(wv.mode() == 2 && wv.walls() == LotViewOptions::Walls::Up && !wv.roof(), "Home: cutaway to up");
    wv.cycle(1);
    check(wv.mode() == 3 && wv.walls() == LotViewOptions::Walls::Up && wv.roof() && wv.options({1, 0}, 0).roof,
          "Home: up to up with the roof (wall mode 3)");
    check(!WallView::sameCut(wv.options({1, 0}, 0), [&] {
              WallView up;
              up.setMode(2);
              return up.options({1, 0}, 0);
          }()),
          "the roof mode rebuilds");
    wv.cycle(1);
    check(wv.walls() == LotViewOptions::Walls::Down, "Home wraps to walls down");
    wv.cycle(-1);
    check(wv.mode() == 3 && WallView::sameCut(wv.options({1, 0}, 0), wv.options({0, 1}, 0)),
          "End: down to the roof; walls up ignore the camera");

    // Cutaway (cWallCutawayManager, docs/ui-live-mac.md 8c): inside walls
    // always cut, outside walls only with their outside face to the camera,
    // walls alone in the yard and fences never.
    {
        Lot c;
        c.width = 4, c.height = 1, c.minLevel = 0, c.levelCount = 1;
        c.elevations.assign(5 * 2, 0.0f);
        c.floors.assign(4, {});
        c.rooms.assign(4, {});
        c.rooms[0] = {1, 1, 1, 1}; // tile (0, 0): room 1
        c.rooms[1] = {2, 2, 2, 2}; // tile (1, 0): room 2; tiles 2 and 3 outside
        c.wallPatterns[1] = "paper";
        auto add = [&](float x, uint32_t type) {
            Lot::Wall w;
            w.a = {x, 0, 0}, w.b = {x, 1, 0};
            w.type = type;
            w.patterns = {1, 1};
            c.walls.push_back(w);
        };
        add(1, Lot::Normal);    // between rooms 1 and 2: inside
        add(2, Lot::Normal);    // room 2 | yard: outside, its outside face +X
        add(3, Lot::Normal);    // yard | yard: alone
        add(0, Lot::Foundation); // a style without cutaway
        auto cutAt = [&](std::array<float, 2> look, std::optional<std::array<float, 3>> simPoint = std::nullopt,
                         std::optional<std::array<float, 3>> camera = std::nullopt) {
            LotViewOptions v;
            v.walls = LotViewOptions::Walls::Cutaway;
            v.level = 0;
            v.viewDirection = look;
            v.simPoint = simPoint;
            v.camera = camera;
            std::vector<float> fromSet;
            const auto set = lotCutawayWalls(c, v);
            for (size_t i = 0; i < set.size(); ++i)
                if (set[i])
                    fromSet.push_back(c.walls[i].a.x);
            std::sort(fromSet.begin(), fromSet.end());
            LotModelCache k;
            buildLotModel(c, index, patterns, v, nullptr, &k);
            std::vector<float> xs;
            for (const auto& w : k.cut)
                xs.push_back(w[0]);
            std::sort(xs.begin(), xs.end());
            check(xs == fromSet, "the model cuts what lotCutawayWalls says");
            return xs;
        };
        check(cutAt({-1, 0.2f}) == std::vector<float>{1, 2}, "cutaway from +X: the inside wall and the outside wall facing the camera");
        check(cutAt({1, 0.2f}) == std::vector<float>{1}, "cutaway from -X: the inside wall only (the outside wall's face looks away)");
        check(cutAt({0.0f, 1}) == std::vector<float>{1}, "an inside wall is cut even edge-on");
        // The camera's position (KeepOnlyFrontFacingOutsideWalls, docs/ui-live-mac.md
        // 8d): the outside wall at x = 2 is cut while the camera is past x = 2,
        // wherever it looks.
        check(cutAt({1, 0}, std::nullopt, std::array<float, 3>{2.5f, -20, 15}) == std::vector<float>{1, 2},
              "a camera on the outside of the wall's line cuts it, even looking the other way");
        check(cutAt({-1, 0}, std::nullopt, std::array<float, 3>{1.5f, -20, 15}) == std::vector<float>{1},
              "a camera on the inside of the wall's line does not cut it");
        check(cutAt({-1, 0}, std::nullopt, std::array<float, 3>{2.0f, -20, 15}) == std::vector<float>{1},
              "a camera on the wall's line does not cut it (the game's test is strict)");
        // cutawayForSelectedSim (WallsBetweenPointSetAndPoint, docs/ui-live-mac.md
        // 8d): a Sim at x = 3.5 seen from a camera at +X, 1.5 up per tile
        // away: the ray stops 3.0 up, 2 tiles on; nothing on its way here.
        check(cutAt({-1, 0}, std::array<float, 3>{0.5f, 0.5f, 0}, std::array<float, 3>{20.5f, 0.5f, 30}) ==
                  std::vector<float>{1, 2},
              "a Sim with no wall toward the camera cuts nothing more");
        // From x = 0.5 toward a camera at +X lower down (3 up per 20 tiles):
        // the ray reaches 6 tiles on, past the yard wall at x = 3.
        check(cutAt({1, 0}, std::array<float, 3>{0.5f, 0.5f, 0}, std::array<float, 3>{40.5f, 0.5f, 20}) ==
                  std::vector<float>{1, 2, 3},
              "the walls between the selected Sim and the camera are cut");
        // A Sim in the yard at x = 3.5 seen from -X: the outside wall at
        // x = 2, its outside face away from the camera, is cut for the Sim;
        // the yard wall at x = 3 by the Sim is not (the walk starts a tile
        // toward the camera, as the game's).
        check(cutAt({1, 0}, std::array<float, 3>{3.5f, 0.5f, 0}, std::array<float, 3>{-40, 0.5f, 20}) ==
                  std::vector<float>{1, 2},
              "a Sim behind the house: the back wall between it and the camera is cut");
        // Cinematics (Expose, docs/ui-live-mac.md 8d): no front-facing rule;
        // a remembered room's walls facing the camera and what blocks them.
        auto exposed = [&](std::array<float, 3> camera, std::vector<uint16_t> rooms,
                           std::vector<std::array<float, 3>> outside = {}) {
            LotViewOptions v;
            v.walls = LotViewOptions::Walls::Cutaway;
            v.level = 0;
            v.camera = camera;
            v.cinematics = true;
            v.exposeRooms = std::move(rooms);
            v.exposeOutside = std::move(outside);
            v.simPoint = std::array<float, 3>{3.5f, 0.5f, 0}; // no selected Sim in a scene
            std::vector<float> xs;
            const auto set = lotCutawayWalls(c, v);
            for (size_t i = 0; i < set.size(); ++i)
                if (set[i])
                    xs.push_back(c.walls[i].a.x);
            std::sort(xs.begin(), xs.end());
            return xs;
        };
        check(exposed({40, 0.5f, 20}, {}).empty(), "a scene with no target cuts nothing (not even inside walls)");
        check(exposed({40, 0.5f, 20}, {2}) == std::vector<float>{2, 3},
              "room 2 seen from +X: its wall facing the camera, and the yard wall blocking that wall's ends");
        check(exposed({-40, 0.5f, 20}, {2}) == std::vector<float>{1},
              "room 2 seen from -X: its wall to room 1 (the room's other face toward the camera)");
        check(exposed({-40, 0.5f, 20}, {}, {{3.5f, 0.5f, 0}}) == std::vector<float>{1, 2},
              "a target in the yard: the walls blocking the near half circle around it");
        // A camera high above: the ray reaches 3.0 up within the Sim's own
        // tile plus a little: only the next tile is marked.
        check(cutAt({1, 0}, std::array<float, 3>{0.5f, 0.5f, 0}, std::array<float, 3>{1.6f, 0.5f, 3}) ==
                  std::vector<float>{1},
              "the end tile of the Sim's ray is not marked: the wall at x = 1 is cut as an inside wall only");
    }
    {
        // The query's steps: the Sim's tile, one back on an axis the camera
        // is on the negative side of, the end tile left out, and an edge only
        // with both tiles across it marked (or at the lot's edge).
        Lot q;
        q.width = 8, q.height = 8, q.minLevel = 0, q.levelCount = 1;
        q.elevations.assign(9 * 9, 0.0f);
        auto has = [](const std::vector<std::array<int, 4>>& v, std::array<int, 4> e) {
            return std::find(v.begin(), v.end(), e) != v.end();
        };
        // +X: tiles 3, 4, 5 (the end, 6, is not), edges x = 4 and 5.
        auto plus = lotWallsBetweenPointsAndCamera(q, {{3.5f, 2.5f, 0}}, {9.5f, 2.5f, 6});
        check(has(plus, {4, 2, 4, 3}) && has(plus, {5, 2, 5, 3}) && !has(plus, {3, 2, 3, 3}) &&
                  !has(plus, {6, 2, 6, 3}) && has(plus, {5, 2, 6, 3}) && !has(plus, {6, 2, 7, 3}),
              "toward +X: the edges past the Sim's tile, the end tile left out");
        // -X: from tile 2 (one back), tiles 2 and 1 (the end, 0, is not):
        // only the edge x = 2; the one at x = 3 by the Sim is not.
        auto minus = lotWallsBetweenPointsAndCamera(q, {{3.5f, 2.5f, 0}}, {-2.5f, 2.5f, 6});
        check(has(minus, {2, 2, 2, 3}) && !has(minus, {3, 2, 3, 3}) && !has(minus, {1, 2, 1, 3}),
              "toward -X: the walk starts a tile back, so the Sim's own edge is left out (as the game)");
        // Below 3.0 the ray goes all the way to the camera.
        auto low = lotWallsBetweenPointsAndCamera(q, {{0.5f, 0.5f, 0}}, {6.5f, 0.5f, 2});
        check(has(low, {5, 0, 5, 1}) && !has(low, {6, 0, 6, 1}), "a camera less than 3.0 up: the ray reaches it");
        check(lotRoomAt(q, 0, 0.5f, 0.5f) == 0, "no room map: outdoors");
    }
    check(WallView::visible(lot, 0, 0.5f, 0.5f, 1.0f) && !WallView::visible(lot, 0, 0.5f, 0.5f, 3.0f) &&
              WallView::visible(lot, 1, 0.5f, 0.5f, 3.0f),
          "objects above the shown level are hidden");
}

static void testLotLightMap() {
    // 4x4 lot, one level: a 2x2 room with floor (room 2) in the corner,
    // the rest terrain (room 1, outdoors since it has no floor).
    Lot lot;
    lot.width = 4, lot.height = 4, lot.levelCount = 1;
    lot.elevations.assign(5 * 5, 0.0f);
    lot.floors.assign(16, {});
    lot.rooms.assign(16, {1, 1, 1, 1});
    for (uint32_t x = 0; x < 2; ++x)
        for (uint32_t y = 0; y < 2; ++y) {
            lot.floors[x * 4 + y] = {1, 1, 1, 1};
            lot.rooms[x * 4 + y] = {2, 2, 2, 2};
        }
    LotLightMapLayout layout;
    layout.init(lot);
    check(layout.outdoor(0, 1) && !layout.outdoor(0, 2) && layout.outdoor(0, 0), "rooms without floor are outdoors");
    check(layout.roomAt(0, 0.5f, 0.5f) == 2 && layout.roomAt(0, 3.5f, 3.5f) == 1, "light-map room lookup");
    const auto t = layout.floorTexel(0, 1.5f, 2.0f);
    check(t[0] == 12 && t[1] == 16, "floor page texels: 8 per tile");
    LotLightMapLayout::WallFace face;
    face.origin = {2, 0}, face.along = {0, 1}, face.normal = {-1, 0}, face.length = 2, face.base = 0, face.top = 3;
    face.room = 2;
    const size_t cell = layout.addWall(face);
    check(layout.walls[cell].w == 18 && layout.walls[cell].h == 26 && layout.walls[cell].y == 32, "wall cell below the floor page");
    const auto wt = layout.wallTexel(cell, 1, 3);
    check(wt[0] == 9 && wt[1] == 33, "wall texel: one-texel border, row 0 at the top");

    const auto cfg = parseLightingConfig(R"(
lightingState day
   skylight (1,1,1) 0.5
   light sun (0, 0, 1) (1, 1, 1) 1
   diffParam insideAmbientColor (1, 1, 1)
   diffParam insideFloorAmbient 0.1
   diffParam floorThreshold 0.5
   modifiers insideObjects 1 1 1 1
   modifiers outsideObjects 1 1 1 1
end
)");
    const LightingState& day = *cfg.state("day");
    auto texel = [](const RgbaImage& im, uint32_t x, uint32_t y) { return im.pixels[(size_t(y) * im.width + x) * 4] / 255.0f; };
    // Outdoors: half of skylight 0.5 + (sun - skylight) x N.L = 0.5.
    LotLightMap dark = computeLotLightMap(layout, day, {});
    check(std::fabs(texel(dark.image, 28, 28) - 0.5f) < 0.01f, "outdoor floor texel is half the outdoor light");
    check(std::fabs(texel(dark.image, 4, 4) - 0.1f) < 0.01f, "indoor floor texel is the inside ambient");
    {
        // The environment's light under a point (formats/lot_lamps, round
        // n42): the floor texel x 2 as 0.3 r + 0.59 g + 0.11 b; VM level 1 is
        // the lot's level 0; off the pages 0; nothing without a light map.
        const auto out = floorIlluminationAt(layout, nullptr, &dark, 1, 1, 3.5f, 3.5f);
        const auto in = floorIlluminationAt(layout, nullptr, &dark, 1, 1, 0.5f, 0.5f);
        check(out && std::fabs(*out - 2 * texel(dark.image, 28, 28)) < 1e-4f, "floor illuminance outdoors: texel x 2");
        check(in && std::fabs(*in - 2 * texel(dark.image, 4, 4)) < 1e-4f, "floor illuminance indoors: texel x 2");
        check(floorIlluminationAt(layout, nullptr, &dark, 1, 1, 5.0f, 1.0f) == 0.0f, "floor illuminance off the lot: 0");
        check(floorIlluminationAt(layout, nullptr, &dark, 1, 2, 1.0f, 1.0f) == 0.0f, "floor illuminance off the levels: 0");
        check(!floorIlluminationAt(layout, nullptr, nullptr, 1, 1, 1.0f, 1.0f), "no light map, no illuminance");
        // Crossfading two maps: halfway between them.
        LotLightMap bright = dark;
        for (size_t i = 0; i < bright.image.pixels.size(); i += 4)
            bright.image.pixels[i] = bright.image.pixels[i + 1] = bright.image.pixels[i + 2] = 255;
        const auto mid = floorIlluminationAt(layout, &dark, &bright, 0.5f, 1, 0.5f, 0.5f);
        check(mid && std::fabs(*mid - (*in + 2.0f) * 0.5f) < 1e-3f, "floor illuminance crossfades the maps");
        // The light-map lights: lamps that are off are left out; the cache
        // key has the state and each lit lamp's object, light and level.
        LotLamp on, off;
        on.id = 7, on.light = "lamp", on.level = 50, on.diffusionColor = {1, 0.5f, 0};
        off.id = 8, off.light = "lamp", off.level = 0;
        std::string key;
        const auto lights = lightMapLights({on, off}, "night", &key);
        check(lights.size() == 1 && lights[0].object == 7 && std::fabs(lights[0].color[1] - 0.25f) < 1e-6f,
              "light-map lights: the lit lamps at their level");
        check(key == "night|7:lamp:50", "light-map cache key");
        check(std::string(lightingStateOfHour(7)) == "day" && std::string(lightingStateOfHour(19)) == "night" &&
                  std::string(lightingStateOfHour(6)) == "night",
              "lighting state of the hour");
    }
    {
        // The sun's height-map shadow: a 3-high block on tile (3, 1) with
        // the sun low in +X shades the tile west of it (its top is far over
        // the ground there: the sun's weight is gone) and not tile (2, 3).
        LotLightMapLayout shaded = layout;
        shaded.occluders.assign(size_t(32 * 32), -1e9f);
        for (int j = 8; j < 16; ++j)
            for (int i = 24; i < 32; ++i)
                shaded.occluders[size_t(j * 32 + i)] = 3;
        const auto sunCfg = parseLightingConfig(R"(
lightingState day
   skylight (1,1,1) 0.5
   light sun (1, 0, 1) (1, 1, 1) 1
end
)");
        const LotLightMap sm = computeLotLightMap(shaded, *sunCfg.state("day"), {});
        check(texel(sm.image, 20, 12) + 0.1f < texel(sm.image, 20, 28), "sun height-map shadow beside a block");
        LightMapGlobals off;
        off.sunShadows = false;
        const LotLightMap nm = computeLotLightMap(shaded, *sunCfg.state("day"), {}, off);
        check(std::fabs(texel(nm.image, 20, 12) - texel(nm.image, 20, 28)) < 0.02f, "no sun shadows with the option off");
        // FindHeightMapShadows: a wall along x = 2, 3 high, with the sun in
        // +X is drawn on its west side (column 15), the front dropping by
        // the sun's slope (1 per tile) away from it; nothing on the sun side.
        LotLightMapLayout walled = layout;
        walled.occluders.assign(size_t(32 * 32), -1e9f);
        walled.sunWalls.push_back({{2, 0}, {2, 4}, 3, 3});
        const auto tops = sunShadowTops(walled, {1, 0, 1});
        check(tops.size() == 32 * 32 && std::fabs(tops[10 * 32 + 15] - 3) < 1e-3f && std::fabs(tops[10 * 32 + 7] - 2) < 1e-3f &&
                  std::fabs(tops[10 * 32 + 16]) < 1e-3f,
              "height-map shadow tops behind a wall");
        check(sunShadowTops(walled, {0, 0, 1})[10 * 32 + 7] == -1000.0f, "no height-map shadow with the sun overhead");
        // LM_sunHM (cLightingManager vtable 0x80): 1 - d x zone under the
        // shadow's top, 1 in the light and off the map.
        LotLightMap hm;
        hm.sunTops = tops, hm.sunTopsWidth = 32;
        check(std::fabs(hm.sunKept(15.5f / 8, 10.5f / 8, 2.5f, 1) - 0.5f) < 1e-5f && hm.sunKept(15.5f / 8, 10.5f / 8, 0, 1) == 0 &&
                  hm.sunKept(15.5f / 8, 10.5f / 8, 3.5f, 1) == 1 && hm.sunKept(-1, 1, 0, 1) == 1,
              "LM_sunHM: the sun x max(0, 1 - d x zone) under the height map's shadow");
        // An outdoor wall face towards the sun (+X, at x = 1) with a 3-high
        // block a tile east of it: its foot is in the block's shadow, its top not.
        LotLightMapLayout faced = shaded;
        LotLightMapLayout::WallFace out;
        out.origin = {1, 0}, out.along = {0, 1}, out.normal = {1, 0}, out.length = 2, out.base = 0, out.top = 3;
        out.room = 0;
        const size_t oc = faced.addWall(out);
        for (int j = 0; j < 16; ++j)
            for (int i = 16; i < 24; ++i)
                faced.occluders[size_t(j * 32 + i)] = 3;
        const LotLightMap fm = computeLotLightMap(faced, *sunCfg.state("day"), {});
        const auto& fc = faced.walls[oc];
        check(texel(fm.image, fc.x + 8, fc.y + fc.h - 2) + 0.1f < texel(fm.image, fc.x + 8, fc.y + 1), "outdoor wall in the sun's height-map shadow");
    }
    // A lamp in the room: seeded at colour x floor scale, falling by 1 + k per half tile.
    LightMapLight lamp;
    lamp.position = {0.25f, 0.25f, 2};
    lamp.color = {1, 1, 1};
    lamp.floorScale = 0.8f;
    LotLightMap lit = computeLotLightMap(layout, day, {lamp});
    check(std::fabs(texel(lit.image, 1, 1) - 0.8f) < 0.02f, "lamp seeds its sample");
    const float far = texel(lit.image, 14, 14), farther = texel(lit.image, 30, 30);
    check(far < 0.8f && far > 0.1f, "lamp light spreads through the room");
    check(std::fabs(farther - 0.5f) < 0.01f, "lamp light stays in its room");
    check(lit.rooms.count({0, 2}) && lit.rooms.at({0, 2})[4][0] > dark.rooms.at({0, 2})[4][0], "room irradiance from the light map");
    {
        // LookupFloorDiffusion (the optimized pass's floor for the walls):
        // 2 x the sample half under the point, less the outside ambient
        // outdoors.
        DiffusionParams dp;
        dp.insideFloor = {0.1f, 0.1f, 0.1f}, dp.outsideFloor = {0.25f, 0.25f, 0.25f}, dp.floorThreshold = 0.5f;
        DiffusionSource src;
        src.position = {0.25f, 0.25f}, src.rect = {-0.25f, -0.25f, 0.75f, 0.75f}, src.room = 2, src.rgb = {0.8f, 0.8f, 0.8f};
        const auto grids = diffuseFloors(layout, {src}, dp);
        check(std::fabs(grids[0].lookup(0.2f, 0.3f)[0] - 2 * grids[0].at(0, 0).h[0].c[0]) < 1e-6f &&
                  std::fabs(grids[0].lookup(0.2f, 0.3f)[0] - 1.6f) < 1e-5f,
              "LookupFloorDiffusion: 2 x the sample under the point");
        check(std::fabs(grids[0].lookup(3.6f, 3.6f)[0]) < 1e-6f, "LookupFloorDiffusion: less the outside ambient outdoors");
    }
    // An outdoor lamp: a pool over the direct light (the outside ambient
    // 0.25 only shapes the falloff and is taken away again).
    LightMapLight garden;
    garden.position = {3.25f, 3.25f, 1};
    garden.color = {1, 1, 1};
    garden.floorScale = 1;
    LotLightMap pool = computeLotLightMap(layout, day, {garden});
    check(std::fabs(texel(pool.image, 26, 26) - 1.0f) < 0.02f, "outdoor lamp adds to the direct light");
    check(std::fabs(texel(pool.image, 26, 2) - 0.5f) < 0.03f, "outdoor lamp pool fades to the direct light");
    // The floor's colour tints the light coming up (roomSaturation 0.5).
    LotLightMapLayout red = layout;
    red.materialColors.push_back({1, 0, 0});
    for (auto& f : red.floorMaterials)
        f = {1, 1, 1, 1};
    LotLightMap tinted = computeLotLightMap(red, day, {lamp});
    check(tinted.rooms.at({0, 2})[4][1] < lit.rooms.at({0, 2})[4][1] * 0.6f &&
              std::fabs(tinted.rooms.at({0, 2})[4][0] - lit.rooms.at({0, 2})[4][0]) < 1e-4f,
          "floor colour in the room irradiance");
    // Unlit rooms (FindUnlitRooms / BrightenUnlitRooms): the last
    // unlitRoomColor of the state; a room whose mean irradiance peaks at m
    // under t gets colour x (1 - m/t) x 0.5 on its texels and colour x
    // (1 - m/t) on five of its six irradiance colours; lit rooms nothing.
    {
        const auto unlitCfg = parseLightingConfig(R"(
lightingState day
   skylight (1,1,1) 0.5
   light sun (0, 0, 1) (1, 1, 1) 1
   diffParam insideAmbientColor (1, 1, 1)
   diffParam insideFloorAmbient 0.1
   diffParam floorThreshold 0.5
   modifiers insideObjects 1 1 1 1
   unlitRoomColor (1, 0, 0) -threshold .9
   unlitRoomColor (.4, .4, .4) -threshold .5
end
)");
        const LotLightMap u = computeLotLightMap(layout, *unlitCfg.state("day"), {});
        const auto& before = dark.rooms.at({0, 2});
        const auto& after = u.rooms.at({0, 2});
        float m = 0;
        for (int c = 0; c < 3; ++c) {
            float s = 0;
            for (const auto& v : before)
                s += v[size_t(c)];
            m = std::max(m, s / 6);
        }
        const float f = 1 - m / 0.5f;
        check(std::fabs(after[0][1] - before[0][1] - 0.4f * f) < 1e-4f && std::fabs(after[5][1] - before[5][1]) < 1e-6f,
              "unlit room: five irradiance colours brightened, the sixth not");
        check(std::fabs(texel(u.image, 4, 4) - (0.1f + 0.2f * f)) < 0.01f, "unlit room: floor texels + colour x f x 0.5");
        check(std::fabs(texel(u.image, 28, 28) - 0.5f) < 0.01f, "unlit room: outdoors untouched");
        LightMapLight bright = lamp;
        bright.color = {20, 20, 20};
        const LotLightMap litRoom = computeLotLightMap(layout, *unlitCfg.state("day"), {bright});
        const LotLightMap litPlain = computeLotLightMap(layout, day, {bright});
        check(std::fabs(texel(litRoom.image, 14, 14) - texel(litPlain.image, 14, 14)) < 0.01f, "a lit room is not brightened");
    }
    // The floor diffusion (cFloorColorGrid) stops at light-blocking walls,
    // not at fences or room borders; the walls (cWallDiffusionManager) are
    // lit on the side facing the lamp and keep 0.5 x their colour.
    {
        Lot walled;
        walled.width = 4, walled.height = 2, walled.levelCount = 1;
        walled.elevations.assign(5 * 3, 0.0f);
        walled.floors.assign(8, {1, 1, 1, 1});
        walled.rooms.assign(8, {2, 2, 2, 2});
        for (int y = 0; y < 2; ++y) {
            Lot::Wall w;
            w.a = {2, float(y), 0}, w.b = {2, float(y + 1), 0}, w.type = Lot::Normal;
            walled.walls.push_back(w);
            Lot::Wall f = w;
            f.a = {1, float(y), 0}, f.b = {1, float(y + 1), 0}, f.type = 0x12345678; // a fence
            walled.walls.push_back(f);
        }
        LotLightMapLayout wl;
        wl.init(walled);
        check(wl.lightWalls.size() == 2, "fences do not block light");
        LotLightMapLayout::WallFace lampSide, backSide;
        lampSide.origin = {2, 0}, lampSide.along = {0, 1}, lampSide.normal = {-1, 0}, lampSide.length = 1, lampSide.top = 3;
        lampSide.room = 2, lampSide.wallA = {2, 0}, lampSide.wallB = {2, 1};
        backSide = lampSide;
        backSide.normal = {1, 0};
        const size_t front = wl.addWall(lampSide), back = wl.addWall(backSide);
        LotLightMapLayout::WallFace fenceSide = lampSide;
        fenceSide.origin = {1, 0}, fenceSide.wallA = {1, 0}, fenceSide.wallB = {1, 1};
        const size_t fence = wl.addWall(fenceSide);
        LightMapLight bulb;
        bulb.position = {0.5f, 0.5f, 1.5f};
        bulb.color = {1, 1, 1};
        const LotLightMap m = computeLotLightMap(wl, day, {bulb});
        check(texel(m.image, 12, 4) > 0.2f, "floor light passes a fence");
        check(std::fabs(texel(m.image, 20, 4) - 0.1f) < 0.01f, "floor light stops at a wall");
        const auto& fc = wl.walls[front];
        const auto& bc = wl.walls[back];
        check(texel(m.image, fc.x + 4, fc.y + 12) > texel(m.image, bc.x + 4, bc.y + 12) + 0.1f, "wall lit on the lamp's side only");
        const auto& nc = wl.walls[fence];
        check(texel(m.image, nc.x + 4, nc.y + 12) == 0 && texel(m.image, fc.x + 4, fc.y + 12) > 0,
              "SetupWalls: a fence gets no wall diffusion (only light-blocking and pool walls)");
    }
    {
        // Portals (docs/lighting-mac.md 11.4): a 4 x 2 lot, a room with
        // floor west of a wall at x = 2, terrain east; the sun low in +X.
        Lot pl;
        pl.width = 4, pl.height = 2, pl.levelCount = 1;
        pl.elevations.assign(5 * 3, 0.0f);
        pl.floors.assign(8, {});
        pl.rooms.assign(8, {1, 1, 1, 1});
        for (uint32_t x = 0; x < 2; ++x)
            for (uint32_t y = 0; y < 2; ++y)
                pl.floors[x * 2 + y] = {1, 1, 1, 1}, pl.rooms[x * 2 + y] = {2, 2, 2, 2};
        for (int y = 0; y < 2; ++y) {
            Lot::Wall w;
            w.a = {2, float(y), 0}, w.b = {2, float(y + 1), 0}, w.type = Lot::Normal;
            pl.walls.push_back(w);
        }
        LotLightMapLayout ly;
        ly.init(pl);
        const auto pcfg = parseLightingConfig(R"(
set portalSuppressionRadius 4
set portalSuppressionAmount .2
lightingState day
   skylight (1,1,1) 0.5
   light sun (1, 0, 1) (1, 1, 1) 1
   diffParam insideAmbientColor (1, 1, 1)
   diffParam insideFloorAmbient 0.1
   diffParam floorThreshold 0.5
   modifiers insidePortals 1 1 1 1
   modifiers insidePortalSpills 1 0 1 1
end
)");
        const LightingState& pday = *pcfg.state("day");
        const LightMapGlobals pg = LightMapGlobals::from(pcfg);
        check(pg.doorOrArchDirectMultiplier == 0 && pg.portalSuppressionRadius == 4 && pg.portalCornerSuppressionAmount == 0.2f,
              "portal globals: doorOrArchDirectMultiplier 0 by default, the corner amount is the amount");
        // A window's light pointing west into the room, from the window's
        // tile: east of the wall (outdoors) or west of it (in the room).
        auto window = [](float x, float y, int object) {
            LightMapLight w;
            w.position = {x, y, 1.5f};
            w.color = {1, 1, 1};
            w.direction = {-0.85f, 0, -0.53f};
            w.portal = true, w.object = object, w.name = "window_light";
            return w;
        };
        const LotLightMap outTile = computeLotLightMap(ly, pday, {window(2.45f, 0.5f, 1)}, pg);
        const LotLightMap inTile = computeLotLightMap(ly, pday, {window(1.55f, 0.5f, 1)}, pg);
        // Texel (12, 4): the tile west of the wall, where the light enters.
        check(texel(inTile.image, 12, 4) > texel(outTile.image, 12, 4) + 0.2f,
              "a portal's diffusion source is in the room at its light: a window put from outside seeds outdoors");
        check(outTile.rooms.count({0, 2}) && outTile.rooms.at({0, 2})[1][0] > 0.3f,
              "the portal still lights the room's irradiance (travelling -X)");
        LightMapLight doorLight = window(1.55f, 0.5f, 1);
        doorLight.doorOrArch = true;
        const LotLightMap door = computeLotLightMap(ly, pday, {doorLight}, pg);
        check(texel(door.image, 12, 4) + 0.2f < texel(inTile.image, 12, 4),
              "a door's or arch's direct light is x doorOrArchDirectMultiplier (0)");
        check(std::fabs(door.rooms.at({0, 2})[1][0] - inTile.rooms.at({0, 2})[1][0]) < 1e-5f,
              "a door's portal colour in the room's irradiance is kept");
        // Practical lights: the direct one (the light itself) and the spill
        // in front of a plane spillPlaneOffset into the room.
        bool direct = false, spill = false;
        for (const auto& p : inTile.portalLights) {
            direct = direct || (!p.spill && p.room == 2 && p.scale[0] > 0.5f && p.name == "window_light");
            spill = spill || (p.spill && std::fabs(p.planeNormal[0] + 1) < 1e-5f && std::fabs(p.planePoint[0] - 1.9f) < 1e-5f);
        }
        check(direct && spill, "a window's direct and spill practical lights");
        // Two windows side by side suppress each other: 1 - (1 - 1/4) x 0.2.
        const LotLightMap two = computeLotLightMap(ly, pday, {window(1.55f, 0.5f, 1), window(1.55f, 1.5f, 2)}, pg);
        float d1 = 0;
        for (const auto& p : two.portalLights)
            if (!p.spill && p.object == 1)
                d1 = p.scale[0];
        float d0 = 0;
        for (const auto& p : inTile.portalLights)
            if (!p.spill)
                d0 = p.scale[0];
        check(d0 > 0 && std::fabs(d1 / d0 - 0.85f) < 1e-4f, "FindPortalSuppression: windows a tile apart");
    }
}

// With the game's data (OS2_DATA): character files our CAS saved before the
// face fixes. A face of another tone than the body's (the stray "afface-s3"
// CASFace.package's afface_s1_txmt names, round 16) takes the body's tone; a
// "<p>face-s<n>" the game does not have (round 12) goes through the face
// material; a face of the body's tone, or of an unknown tone, stays.
static void testFaceSkinMigration() {
    const char* data = std::getenv("OS2_DATA");
    if (!data || !std::filesystem::exists(std::filesystem::path(data) / "TSData/Res"))
        return;
    ResourceIndex index(data);
    check(migrateFaceSkin(index, "afface-s3", 1) == "afface-s1", "stray afface-s3 face of a tone-1 Sim -> afface-s1");
    check(migrateFaceSkin(index, "afface-s1", 3) == "afface-s3", "tone-3 Sim with a tone-1 face -> afface-s3");
    check(migrateFaceSkin(index, "afface-s1", 1) == "afface-s1", "a face of the body's tone stays");
    check(migrateFaceSkin(index, "afface-s3", 0) == "afface-s3", "unknown body tone: the face stays");
    check(migrateFaceSkin(index, "afface-s5", 5) == "afface-s5", "the alien tone stays");
    check(migrateFaceSkin(index, "tfface-s2", 2) == "afface-s2", "tfface-s2 (no such texture) -> afface-s2");
    check(migrateFaceSkin(index, "tfface-s2", 1) == "afface-s1", "tfface-s2 of a tone-1 teen -> afface-s1");
    check(migrateFaceSkin(index, "amhairshort", 1) == "amhairshort", "not a face skin: unchanged");

    // The CAS archetype grid leaves the hidden heads out (flag 0x1, "hidden"
    // in PrintSkinEntryFlagsInfo): the alien and "eelf" ones.
    const cas::CasCatalog catalog = cas::CasCatalog::load(index);
    for (uint32_t a : {cas::age::Child, cas::age::Teen, cas::age::Adult, cas::age::Elder})
        for (uint32_t g : {cas::gender::Female, cas::gender::Male}) {
            bool alienAll = false, alienGrid = false, base = false;
            for (const SimPart* e : catalog.archetypesFor(a, g))
                alienAll = alienAll || e->name().find("alien") != std::string::npos;
            for (const SimPart* e : catalog.archetypesFor(a, g, cas::flag::Hidden)) {
                alienGrid = alienGrid || e->name().find("alien") != std::string::npos ||
                            e->name().find("eelf") != std::string::npos;
                base = base || e->name().find("archbase") != std::string::npos;
            }
            check(alienAll, "the catalog has the alien head of every age");
            check(!alienGrid, "the CAS grid has no alien/eelf head");
            check(base, "the CAS grid keeps the base head");
        }
}

// The CAS randomize button (RandomizeForHumans, docs/cas-mac.md round n10),
// with the game's data: normal fitness, every clothed category dressed
// (a whole outfit, or a top with a bottom), only parts neither hidden nor
// "no_random" (the gestante falls back to the one "default" maternity
// outfit), the same hair family everywhere and no hat where the category
// takes none; with both kinds drawn over a few seeds.
static void testCasRandomize() {
    const char* data = std::getenv("OS2_DATA");
    if (!data || !std::filesystem::exists(std::filesystem::path(data) / "TSData/Res"))
        return;
    ResourceIndex index(data);
    const cas::CasCatalog catalog = cas::CasCatalog::load(index);
    auto byName = [&](const std::string& n) -> const SimPart* {
        for (const auto& p : catalog.parts)
            if (p.name() == n)
                return &p;
        return nullptr;
    };
    bool sawBody = false, sawTopBottom = false, allOpen = true, maternity = true, dressed = true, hats = true,
         sameHair = true;
    for (uint32_t seed = 1; seed <= 12; ++seed) {
        cas::CharacterSpec s;
        s.age = cas::age::Adult;
        s.gender = seed % 2 ? cas::gender::Female : cas::gender::Male;
        s.fitness = 2;
        cas::CasRandom rng(seed * 2654435761u);
        cas::randomizeLook(catalog, s, rng);
        check(s.fitness == 0, "randomize: the body back to normal (SetFitnessLevel(500))");
        std::string hairFamily;
        for (uint32_t cat : cas::outfitCategories(s.age, s.gender)) {
            if (cat == cas::category::Naked || cat == cas::category::TryOn || !s.outfits.count(cat))
                continue;
            const auto& o = s.outfits.at(cat);
            const bool body = o.count(cas::part::Body), top = o.count(cas::part::Top), bottom = o.count(cas::part::Bottom);
            dressed = dressed && (body ? !top && !bottom : top && bottom);
            if (cat == cas::category::Everyday) {
                sawBody = sawBody || body;
                sawTopBottom = sawTopBottom || (top && bottom);
            }
            for (const auto& [bit, name] : o) {
                const SimPart* p = byName(name);
                if (!p)
                    continue;
                if (bit == cas::part::Hair) {
                    if (hairFamily.empty())
                        hairFamily = cas::family(*p);
                    sameHair = sameHair && cas::family(*p) == hairFamily;
                    if (cas::categoryFlags(cat) & 1)
                        hats = hats && !(p->number("flags") & cas::flag::Hat);
                    continue;
                }
                if (cat == cas::category::Maternity)
                    maternity = maternity && name == "afbodymaternityshirtpants_navyrust";
                else
                    allOpen = allOpen && !(p->number("flags") & (cas::flag::Hidden | cas::flag::NoRandom));
            }
        }
    }
    check(dressed, "randomize: each clothed category wears a whole outfit or a top and a bottom");
    check(sawBody && sawTopBottom, "randomize: both whole outfits and tops with bottoms over the seeds");
    check(allOpen, "randomize: no hidden or no_random clothes");
    check(maternity, "randomize: the maternity outfit is the default one (navyrust, flags 13)");
    check(sameHair, "randomize: one hair family in every category");
    check(hats, "randomize: no hat in swimwear, pajamas, underwear, athletic");
}

// The CAS's generator (FUN_00171674, Mac BV; docs/cas-mac.md round n51):
// s = s * 1103515245 + 12345 and a draw below n = n x bits 16..47 / 2^32.
static void testCasRandom() {
    cas::CasRandom r(12345);
    const uint32_t want[5] = {4, 99, 2, 3, 900}, ns[5] = {100, 100, 6, 4, 1000};
    bool ok = true;
    for (int i = 0; i < 5; ++i)
        ok = ok && r.below(ns[i]) == want[i];
    check(ok, "CasRandom: the game's draws from seed 12345");
    check(r.state() == 0xC21F1C8Au, "CasRandom: the state after five draws");
    check(cas::CasRandom(1).below(0) == 0, "CasRandom: a draw below 0 is 0");
}

// A region taken from another archetype moves the face (the mask weight is
// the colour's top byte, BuildRegionMask: with the old largest-RGB rule the
// nose mask weighed almost nothing), only where its mask is, and never the
// "hairneck" vertices (BlendArchetype), not even for a whole archetype.
static void testFaceRegionMasks() {
    const char* data = std::getenv("OS2_DATA");
    if (!data || !std::filesystem::exists(std::filesystem::path(data) / "TSData/Res"))
        return;
    ResourceIndex index(data);
    const cas::CasCatalog catalog = cas::CasCatalog::load(index);
    cas::FaceBaker baker(index, catalog);
    auto fam = [&](const char* name) {
        for (const auto& e : catalog.archetypes)
            if (e.name() == name)
                return cas::family(e);
        return std::string();
    };
    cas::FaceSpec base;
    base.archetype = fam("amarchbase_base");
    cas::FaceSpec nose = base;
    nose.regions["nose"] = fam("amarcheafr_eafr");
    const auto a = baker.bake(base, cas::age::Adult, cas::gender::Male);
    const auto b = baker.bake(nose, cas::age::Adult, cas::gender::Male);
    size_t moved = 0;
    float most = 0;
    for (size_t i = 0; i < a.displacements.size() && i < b.displacements.size(); ++i) {
        float d = 0;
        for (int c = 0; c < 3; ++c)
            d += std::fabs(a.displacements[i][c] - b.displacements[i][c]);
        moved += d > 1e-4f;
        most = std::max(most, d);
    }
    check(moved >= 30 && most > 0.005f, "a nose region from another archetype moves the nose");
    check(moved < a.displacements.size() / 2, "the nose region leaves most of the face alone");
    cas::FaceSpec whole;
    whole.archetype = fam("amarcheafr_eafr");
    const auto w = baker.bake(whole, cas::age::Adult, cas::gender::Male);
    check(w.moved > 0 && w.moved < w.vertices, "a whole archetype leaves the hairneck vertices in place");
}

// BlendFaces (Mac BV 0x80748c, docs/cas-mac.md round n21): the blend data
// of FaceInfo.package, and a child's face from the neutral face, its
// regions taken from the parent the script names, the heights kept in the
// limits. With OS2_DATA, two archetypes' faces.
static void testBlendFaces() {
    {
        std::vector<uint8_t> b;
        auto u32 = [&](uint32_t v) {
            for (int i = 0; i < 4; ++i)
                b.push_back(uint8_t(v >> (8 * i)));
        };
        auto str = [&](const std::string& t) {
            u32(uint32_t(t.size()));
            b.insert(b.end(), t.begin(), t.end());
        };
        u32(1);
        u32(1);
        for (const char* t : {"brow", "eyes", "eyesUp", "eyesDown"})
            str(t);
        u32(1);
        u32(3);
        u32(1);
        str("brow");
        str("eyes");
        u32(std::bit_cast<uint32_t>(0.023f));
        u32(std::bit_cast<uint32_t>(0.028f));
        const cas::FaceBlendData d = cas::FaceBlendData::parse(b);
        check(d.steps.size() == 1 && d.steps[0].a == "brow" && d.steps[0].down == "eyesDown" && d.limits.size() == 1,
              "face blend data: steps and limits");
        check(d.limitsFor(3, 1, "Brow", "eyes") == std::pair<float, float>{0.023f, 0.028f} &&
                  d.limitsFor(3, 0, "brow", "eyes") == std::pair<float, float>{-9999.0f, 9999.0f},
              "face blend limits by age, gender and pair (+-9999 without one)");
    }
    const char* data = std::getenv("OS2_DATA");
    if (!data || !std::filesystem::exists(std::filesystem::path(data) / "TSData/Res"))
        return;
    ResourceIndex index(data);
    if (auto loc = index.find(Tgi{cas::FaceBlendData::kType, 0xFFFFFFFF, 1, 0})) {
        const cas::FaceBlendData d = cas::FaceBlendData::parse(index.read(*loc));
        check(d.steps.size() == 8 && d.limits.size() == 70 && d.steps[1].a == "brow" && d.steps[1].up == "eyesUp",
              "FaceInfo.package: 8 blend steps and 70 limits");
    } else {
        check(false, "FaceInfo.package's FACE resource");
    }
    const cas::CasCatalog catalog = cas::CasCatalog::load(index);
    cas::FaceBaker baker(index, catalog);
    auto fam = [&](const char* name) {
        for (const auto& e : catalog.archetypes)
            if (e.name() == name)
                return cas::family(e);
        return std::string();
    };
    cas::FaceSpec sa, sb;
    sa.archetype = fam("amarchcara_cara");
    sb.archetype = fam("amarcheafr_eafr");
    const cas::FaceStructure fa = baker.structureOf(sa, cas::gender::Male);
    const cas::FaceStructure fb = baker.structureOf(sb, cas::gender::Male);
    const uint32_t key = cas::FaceStructure::key(cas::age::Adult, cas::gender::Male);
    std::vector<std::pair<std::string, int>> allA, allB;
    for (const char* r : {"nose", "brow", "mouth", "cheek", "eyes", "cheekbones", "jaw", "ears"}) {
        allA.emplace_back(r, 0);
        allB.emplace_back(r, 1);
    }
    const cas::FaceStructure ca = baker.descendant(fa, fb, allA), cb = baker.descendant(fa, fb, allB);
    const auto* va = ca.find(cas::age::Adult, cas::gender::Male);
    const auto* vb = cb.find(cas::age::Adult, cas::gender::Male);
    const auto& pa = fa.data.at(key);
    check(va && vb && va->size() == pa.size(), "a child's face has the parents' adult key");
    if (!va || !vb)
        return;
    size_t likeA = 0, neutral = 0, differ = 0;
    for (size_t k = 0; k < va->size(); ++k) {
        float dA = 0, dN = 0, dAB = 0;
        for (int c = 0; c < 3; ++c) {
            dA += std::fabs((*va)[k].delta[c] - pa[k].delta[c]);
            dN += std::fabs((*va)[k].delta[c]);
            dAB += std::fabs((*va)[k].delta[c] - (*vb)[k].delta[c]);
        }
        likeA += dA < 1e-4f;
        neutral += dN == 0;
        differ += dAB > 1e-4f;
    }
    check(likeA > va->size() / 3, "every region from the first parent: much of the face is that parent's");
    check(neutral > 0, "corners outside every region mask keep the neutral face");
    check(differ > va->size() / 3, "the second parent's regions give another face");
}

// The face's composited layers as GetMaterialLayersToBeComposited lays
// them (Mac BV 0x80d5e8, docs/cas-mac.md round n21): one overlay per bin
// (two costume makeups share bin 20: the later one stays), stably sorted by
// layer; cafOverlay<i> lists every choice in order, "<family>," as the game
// writes them, and reading the character back gets them all again.
static void testFaceOverlayLayers() {
    const char* data = std::getenv("OS2_DATA");
    if (!data || !std::filesystem::exists(std::filesystem::path(data) / "TSData/Res"))
        return;
    ResourceIndex index(data);
    const cas::CasCatalog catalog = cas::CasCatalog::load(index);
    cas::FaceBaker baker(index, catalog);
    auto fam = [&](const char* name) {
        for (const auto& e : catalog.textureOverlays)
            if (e.name() == name)
                return cas::family(e);
        return std::string();
    };
    cas::CharacterSpec s;
    s.age = cas::age::Adult;
    s.gender = cas::gender::Female;
    s.hairTone = 1;
    s.eyes = fam("uuface_eye_green");
    s.overlays = {fam("uuface_browbushy_black"), fam("uuface_lip_classicred"), fam("uuface_mime"), fam("uuface_tiger")};
    cas::completeSpec(catalog, s, 7);
    const auto dir = std::filesystem::temp_directory_path() / "os2_test_face_layers";
    std::filesystem::create_directories(dir);
    const auto file = dir / "face_layers.package";
    cas::writeCharacter(index, catalog, baker, s, "Face Layers", file, false);
    DbpfPackage pkg(file);
    std::vector<std::string> textures;
    int cafCount = -1;
    std::string caf0;
    for (const auto& e : pkg.entries()) {
        if (e.tgi.type != 0x49596978 || e.tgi.group != 0xFFFFFFFF)
            continue;
        const auto bytes = pkg.read(e);
        Rcol rc(bytes);
        const auto* b = rc.find("cMaterialDefinition");
        if (!b)
            continue;
        const auto md = MaterialDefinition::parse(b->body);
        if (md.property("cafNumOverlays").empty())
            continue;
        const int n = std::atoi(md.property("numTexturesToComposite").c_str());
        for (int i = 0; i < n; ++i)
            textures.push_back(md.property("baseTexture" + std::to_string(i)));
        cafCount = std::atoi(md.property("cafNumOverlays").c_str());
        caf0 = md.property("cafOverlay0");
        break;
    }
    check(textures.size() == 5, "face layers: the skin, the eyes, the lipstick, one costume makeup, the brows");
    if (textures.size() == 5) {
        check(textures[1].find("eye-green") != std::string::npos && textures[2].find("lip-classicred") != std::string::npos,
              "face layers: layer 0 in choice order (the eyes, then the lipstick)");
        check(textures[3].find("tiger") != std::string::npos, "face layers: the later costume makeup of bin 20 stays");
        check(textures[4].find("browbushy") != std::string::npos, "face layers: the brows (layer 80) on top");
    }
    check(cafCount == 5 && caf0 == s.eyes + ",", "cafOverlay: every choice, the eyes first, \"<family>,\"");
    const cas::CharacterSpec back = cas::readCharacterSpec(pkg, 0xFFFFFFFF, catalog);
    check(back.eyes == s.eyes && back.overlays.size() == 4, "reading the character back gets its overlays again");
    std::filesystem::remove_all(dir);
}

// The naked outfit (shower, bath, getting changed) keeps the Sim's hair and
// face: SetupShapesPrivate takes the naked category's hair and face from
// swimwear, never the bald "<p>hairbald_skin" its list names (playtest 35:
// the tutorial's adult man lost his hair in the shower). With OS2_DATA, the
// tutorial's man and its exterminator (ball cap: the swimwear hair is the
// hat-less one).
static void testNakedOutfitKeepsHair() {
    const Tgi bald{0, 0, 1}, swimHair{0, 0, 2}, nakedTop{0, 0, 3}, face{0, 0, 4}, everydayHair{0, 0, 5};
    Character ch;
    ch.outfits[(0x080u << 16) | 1] = {bald};
    ch.outfits[(0x080u << 16) | 4] = {nakedTop};
    ch.outfits[(0x008u << 16) | 1] = {swimHair};
    ch.outfits[(0x001u << 16) | 1] = {everydayHair};
    ch.outfits[(0x001u << 16) | 2] = {face};
    auto naked = ch.wear(0x080);
    check(naked.at(1).instance == 2, "naked: the swimwear hair, not the bald one");
    check(naked.at(4).instance == 3, "naked: its own top");
    check(naked.count(2) && naked.at(2).instance == 4, "naked: the everyday face");
    ch.outfits.erase((0x008u << 16) | 1);
    check(ch.wear(0x080).at(1).instance == 5, "naked without swimwear hair: the everyday hair");
    check(ch.wear(0x001).at(1).instance == 5, "everyday: its own hair");

    const char* data = std::getenv("OS2_DATA");
    if (!data || !std::filesystem::exists(std::filesystem::path(data) / "TSData/Res"))
        return;
    ResourceIndex index(data);
    const auto dir = std::filesystem::path(data) / "TSData/Res/UserData/Neighborhoods/Tutorial/Characters";
    for (const char* file : {"Tutorial_User00000.package", "Tutorial_User00003.package"}) {
        if (!std::filesystem::exists(dir / file))
            continue;
        DbpfPackage pkg(dir / file);
        const Character c = readCharacter(pkg, index);
        const auto w = c.wear(0x080);
        const std::string what = std::string(file) + ": ";
        check(w.count(1) && c.outfit(0x080).count(1) && w.at(1).instance != c.outfit(0x080).at(1).instance,
              (what + "the naked outfit does not wear its bald hair").c_str());
        check(w.count(1) && c.outfit(0x008).count(1) && w.at(1).instance == c.outfit(0x008).at(1).instance,
              (what + "the naked outfit wears the swimwear hair").c_str());
        check(w.count(4) && w.at(4).instance == c.outfit(0x080).at(4).instance, (what + "the naked top stays").c_str());
        check(w.count(2), (what + "the naked outfit keeps the face").c_str());
    }
}

// With the game's data (OS2_DATA): N001_Lot82's front arch (door 455, "Door -
// 2 Tile - French Empty Arch" at (30.5, 38.5) facing -y, its +x along -x:
// tiles x 30..31 ("0") and 29..30 ("1e") of the wall y = 39) is cut open up
// to the edge the two tiles share and keeps the wall at its outer edges. The
// wall masks' u ran the other way: open at the outer edges with a strip of
// wall in the middle, which looked like two leaves swung open.
// Walls as cWallGeometryNode builds them (docs/walls-floors-mac.md §2-3):
// mitred corners, caps on free ends, junction polygons, the tops' world
// (x, y) texture with no light map, cut walls showing the foot of the
// paper, ramps toward a cut end, half walls 1.025 high.
static void testWallGeometry() {
    const auto empty = std::filesystem::temp_directory_path() / "os2_test_empty_res";
    std::filesystem::create_directories(empty);
    ResourceIndex index(empty);
    const PatternCatalog patterns(index, empty);
    Lot lot;
    lot.width = 3, lot.height = 3, lot.levelCount = 2;
    lot.elevations.assign(2 * 4 * 4, 0.0f);
    for (size_t i = 16; i < 32; ++i)
        lot.elevations[i] = 3.0f;
    lot.floors.assign(2 * 3 * 3, {});
    lot.wallPatterns[1] = "paper";
    auto add = [&](Lot& l, float ax, float ay, float bx, float by, uint32_t type = Lot::Normal) {
        Lot::Wall w;
        w.a = {ax, ay, 0}, w.b = {bx, by, 0};
        w.type = type;
        w.patterns = {1, 1};
        l.walls.push_back(w);
    };
    add(lot, 1, 1, 2, 1); // east
    add(lot, 1, 1, 1, 2); // north: an L at (1, 1)
    constexpr float h = 0.075f;
    auto part = [](const ObjectModel& m, const std::string& node) -> const ObjectModel::Part* {
        for (const auto& p : m.parts)
            if (p.node == node)
                return &p;
        return nullptr;
    };
    auto near = [](float a, float b) { return std::abs(a - b) < 1e-4f; };
    {
        const ObjectModel m = buildLotModel(lot, index, patterns);
        const auto* wall = part(m, "wall:paper");
        // The east wall's south face reaches the outer corner (1 - h, 1 - h);
        // its north face starts at the inner one (1 + h, 1 + h).
        float southMin = 9, northMin = 9;
        size_t caps = 0;
        for (size_t i = 0; wall && i < wall->mesh.positions.size(); ++i) {
            const auto& p = wall->mesh.positions[i];
            const auto& n = wall->mesh.normals[i];
            if (near(n[1], -1) && near(p[1], 1 - h))
                southMin = std::min(southMin, p[0]);
            if (near(n[1], 1) && near(p[1], 1 + h))
                northMin = std::min(northMin, p[0]);
            if ((near(n[0], 1) && near(p[0], 2)) || (near(n[1], 1) && near(p[1], 2)))
                ++caps;
        }
        check(near(southMin, 1 - h) && near(northMin, 1 + h), "walls mitre at an L corner");
        check(caps == 8, "a cap on each free end, none at the corner");
        const auto* top = part(m, "walltop:wall_top 2x");
        bool worldUv = top && top->mesh.uvs2.empty() && !top->mesh.positions.empty();
        for (size_t i = 0; top && i < top->mesh.positions.size(); ++i)
            worldUv = worldUv && top->mesh.uvs[i][0] == top->mesh.positions[i][0] &&
                      top->mesh.uvs[i][1] == top->mesh.positions[i][1] && top->mesh.normals[i][2] == 1.0f;
        check(worldUv, "wall tops: texture by world (x, y), normal up, no light map");
        auto mat = m.materials.find("wall_top 2x");
        check(mat != m.materials.end() && mat->second.property("stdMatDiffCoef") == "2,2,2,1" &&
                  mat->second.property("stdMatLightingEnabled") == "false",
              "wall tops: 2 x the texture, unlit");
        // The wallpaper: u 0, 0.5, 1 along each face, v 0 at the top of a
        // full wall and 1 at its floor.
        bool paper = wall != nullptr;
        for (size_t i = 0; wall && i < wall->mesh.positions.size(); ++i) {
            const auto& p = wall->mesh.positions[i];
            const auto& n = wall->mesh.normals[i];
            if (!near(n[1], -1)) // the east wall's south face
                continue;
            const float u = wall->mesh.uvs[i][0], v = wall->mesh.uvs[i][1];
            paper = paper && near(u, std::clamp(p[0] - 1, 0.0f, 1.0f) < 0.25f ? 0.0f : p[0] > 1.75f ? 1.0f : 0.5f) &&
                    near(v, (2.99f - p[2]) / 3.0f);
        }
        check(paper, "wallpaper: u 0/0.5/1 per wall, v a story from the top");
    }
    {
        // Floors (SetTileQuadrantSubset, WorldLocationsForPatternRotation):
        // a quarter's texture coordinates are (y, x) of its corners, turned
        // by the saved rotation 2 / 4 / 8 (the quarter + 3 / 2 / 1); any
        // other value leaves them.
        auto northUvs = [&](uint8_t rotation) {
            Lot f = lot;
            f.walls.clear();
            f.floors[0] = {0, 1, 0, 0}; // tile (0, 0): the north quarter
            f.floorPatterns[1] = "tile";
            f.floorRotations.assign(f.floors.size(), uint16_t(0x1111)); // 3ARY 0x20: a nibble a quarter
            f.floorRotations[0] = uint16_t(0x1101 | rotation << 4);
            const ObjectModel m = buildLotModel(f, index, patterns);
            std::vector<std::array<float, 2>> uvs;
            if (const auto* fl = part(m, "floor:tile"))
                for (uint32_t t : fl->mesh.triangles)
                    uvs.push_back(fl->mesh.uvs[t]);
            return uvs;
        };
        const auto plain = northUvs(1);
        // North quarter: (1, 1), (0, 1), centre -> uv (y, x).
        check(plain.size() == 3 && plain[0] == std::array<float, 2>{1, 1} && plain[1] == std::array<float, 2>{1, 0} &&
                  plain[2] == std::array<float, 2>{0.5f, 0.5f},
              "floor quarter: texture by (y, x) of its corners");
        // Rotation 2: the west quarter's corners (0, 1), (0, 0).
        const auto turned = northUvs(2);
        check(turned.size() == 3 && turned[0] == std::array<float, 2>{1, 0} && turned[1] == std::array<float, 2>{0, 0},
              "floor rotation 2 takes the quarter before");
        check(northUvs(3) == plain && northUvs(8) != plain, "floor rotations other than 2, 4, 8 leave the texture");
        // The visibility layer (3ARY 0x15): a hidden quarter is not drawn.
        Lot f = lot;
        f.walls.clear();
        f.floors[0] = {0, 1, 0, 0};
        f.floorPatterns[1] = "tile";
        f.floorVisibility.assign(f.floors.size(), {1, 1, 1, 1});
        f.floorVisibility[0][1] = 0;
        check(!part(buildLotModel(f, index, patterns), "floor:tile"), "a hidden floor quarter is not drawn");
    }
    {
        // A T: the junction polygon over (1, 1).
        Lot t = lot;
        add(t, 1, 1, 0, 1); // west
        const ObjectModel m = buildLotModel(t, index, patterns);
        const auto* top = part(m, "walltop:wall_top 2x");
        bool junction = false;
        for (size_t k = 0; top && k < top->mesh.triangles.size(); k += 3) {
            bool all = true;
            for (int j = 0; j < 3; ++j) {
                const auto& p = top->mesh.positions[top->mesh.triangles[k + size_t(j)]];
                all = all && std::abs(p[0] - 1) < 0.11f && std::abs(p[1] - 1) < 0.11f && near(p[2], 2.99f);
            }
            junction = junction || all;
        }
        check(junction, "three walls: a polygon over their junction");
    }
    {
        // Walls down: the face shows the foot of its paper (v 1 at the
        // floor, 1 - 0.2 / 3 at the cut top).
        LotViewOptions down;
        down.walls = LotViewOptions::Walls::Down;
        const ObjectModel m = buildLotModel(lot, index, patterns, down);
        const auto* wall = part(m, "wall:paper");
        float vMin = 9, vMax = -9, zMax = -9;
        for (size_t i = 0; wall && i < wall->mesh.positions.size(); ++i) {
            if (std::abs(wall->mesh.normals[i][2]) > 0.5f)
                continue;
            vMin = std::min(vMin, wall->mesh.uvs[i][1]);
            vMax = std::max(vMax, wall->mesh.uvs[i][1]);
            zMax = std::max(zMax, wall->mesh.positions[i][2]);
        }
        check(near(vMax, 1) && near(vMin, 1 - 0.2f / 3) && near(zMax, 0.19f), "walls down show the foot of the paper");
    }
    {
        // Cutaway by end points: the inside wall at x = 1 (rooms 1 | 2) is
        // cut; the outside wall along y = 1 from its top end (1, 1) keeps
        // its far end up and ramps down from the middle.
        Lot c;
        c.width = 3, c.height = 1, c.levelCount = 1;
        c.elevations.assign(4 * 2, 0.0f);
        c.floors.assign(3, {});
        c.rooms.assign(3, {});
        c.rooms[0] = {1, 1, 1, 1};
        c.rooms[1] = {2, 2, 2, 2};
        c.wallPatterns[1] = "paper";
        add(c, 1, 0, 1, 1);
        add(c, 1, 1, 2, 1);
        LotViewOptions v;
        v.walls = LotViewOptions::Walls::Cutaway;
        v.level = 0;
        v.viewDirection = {0, 1}; // looking +y: the y = 1 wall's outside face looks away
        const auto set = lotCutawayWalls(c, v);
        check(set.size() == 2 && set[0] && !set[1], "cutaway: the inside wall only");
        LotModelCache k;
        const ObjectModel m = buildLotModel(c, index, patterns, v, nullptr, &k);
        const auto* wall = part(m, "wall:paper");
        float midTop = -9, farTop = -9;
        for (size_t i = 0; wall && i < wall->mesh.positions.size(); ++i) {
            const auto& p = wall->mesh.positions[i];
            if (std::abs(std::abs(wall->mesh.normals[i][1]) - 1) > 1e-3f)
                continue;
            if (near(p[0], 1.5f))
                midTop = std::max(midTop, p[2]);
            if (std::abs(p[0] - 2) < 0.1f)
                farTop = std::max(farTop, p[2]);
        }
        check(near(midTop, 0.19f) && near(farTop, 2.99f), "a wall touching a cut wall's end ramps down to it");
        check(k.cut.size() == 1 && k.cut[0][0] == 1 && k.cut[0][2] == 1, "only walls down at both ends hide what hangs on them");
    }
    {
        // A half wall (a catalog fence with ishalfwall: PartitionType 3):
        // 1.025 over its floor. The same covered fence of a plain fence style
        // is no wall (AddWallImpl takes walls and half walls only).
        Lot f = lot;
        f.walls.clear();
        add(f, 0, 0, 1, 0, 0x1234ABCD);
        check(!part(buildLotModel(f, index, patterns), "wall:paper"),
              "half walls: a covered fence that is not a half wall style draws no wall");
        PatternCatalog halfWalls = patterns;
        halfWalls.setFence(0x1234ABCD, {"rail", "", "post", true});
        const ObjectModel m = buildLotModel(f, index, halfWalls);
        const auto* wall = part(m, "wall:paper");
        float top = -9;
        for (size_t i = 0; wall && i < wall->mesh.positions.size(); ++i)
            top = std::max(top, wall->mesh.positions[i][2]);
        check(near(top, 1.025f - 0.01f), "half walls: 1.025 over the floor");
    }
    {
        // A wall with neither tile beside it on the lot: no geometry
        // (AddWallImpl, cWorldDB slot 0xf8).
        Lot f = lot;
        f.walls.clear();
        add(f, 3, 0, 3, 1); // the lot's east edge: the west tile is on the lot
        add(f, 4, 0, 4, 1); // a tile past it: neither
        const ObjectModel m = buildLotModel(f, index, patterns);
        const auto* wall = part(m, "wall:paper");
        float maxX = -9;
        for (size_t i = 0; wall && i < wall->mesh.positions.size(); ++i)
            maxX = std::max(maxX, wall->mesh.positions[i][0]);
        check(wall && maxX < 3.2f, "walls: one with no tile beside it on the lot is not drawn");
    }
}

// The floors' thickness (cFloorThicknessGeometryNode, docs/walls-floors-mac.md
// §6.1): on levels 1 and up, a 0.15 strip under each edge round the level's
// floor quarters (the edges two quarters share cancel), facing out, u along
// the edge and v down; none on the ground.
static void testFloorThickness() {
    const auto empty = std::filesystem::temp_directory_path() / "os2_test_empty_res";
    std::filesystem::create_directories(empty);
    ResourceIndex index(empty);
    const PatternCatalog patterns(index, empty);
    Lot lot;
    lot.width = 3, lot.height = 3, lot.levelCount = 2;
    lot.elevations.assign(2 * 4 * 4, 0.0f);
    for (size_t i = 16; i < 32; ++i)
        lot.elevations[i] = 3.0f;
    lot.floors.assign(2 * 3 * 3, {});
    lot.floorPatterns[1] = "tile";
    auto at = [&](int level, int x, int y) -> std::array<uint16_t, 4>& { return lot.floors[size_t((level * 3 + x) * 3 + y)]; };
    at(0, 0, 0) = {1, 1, 1, 1}; // the ground: no thickness
    at(1, 1, 1) = {1, 1, 1, 1}; // a whole tile up: its four sides
    at(1, 2, 1) = {1, 0, 0, 1}; // the half tile east of it (west and south quarters): the diagonal and its south side
    const ObjectModel m = buildLotModel(lot, index, patterns);
    const ObjectModel::Part* t = nullptr;
    for (const auto& p : m.parts)
        if (p.material == "FloorThickness")
            t = &p;
    check(t && m.materials.count("FloorThickness") &&
              m.materials.at("FloorThickness").property("stdMatBaseTextureName") == "floor-edge",
          "floor thickness: a FloorThickness part with the floor-edge texture");
    if (!t)
        return;
    // Quads: tile (1, 1) south, west, north (its east side is shared), the
    // half tile's south side and the two halves of its diagonal (from the
    // tile's centre: the quarters' own edges).
    check(t->mesh.positions.size() == 6 * 4 && t->mesh.triangles.size() == 6 * 6,
          "floor thickness: one quad per edge round the level's floor, none between quarters or on the ground");
    auto inFloor = [](float x, float y) {
        return (x > 1 && x < 2 && y > 1 && y < 2) || (x > 2 && y > 1 && (x - 2) + (y - 1) < 1);
    };
    bool depth = true, outward = true, uvs = true, diagonal = false, wound = true;
    const auto& p = t->mesh.positions;
    for (size_t q = 0; q < p.size(); q += 4) {
        const auto& n = t->mesh.normals[q];
        depth = depth && std::abs(p[q][2] - 3) < 1e-4f && std::abs(p[q + 2][2] - 2.85f) < 1e-4f;
        // Past the edge's middle along the normal: off the floor; before it: on it.
        const float mx = (p[q][0] + p[q + 1][0]) / 2, my = (p[q][1] + p[q + 1][1]) / 2;
        outward = outward && !inFloor(mx + n[0] * 0.1f, my + n[1] * 0.1f) && inFloor(mx - n[0] * 0.1f, my - n[1] * 0.1f);
        const auto& uv = t->mesh.uvs;
        uvs = uvs && uv[q] == std::array<float, 2>{0, 0} && uv[q + 1] == std::array<float, 2>{1, 0} &&
              uv[q + 2] == std::array<float, 2>{0, 1} && uv[q + 3] == std::array<float, 2>{1, 1};
        diagonal = diagonal || (std::abs(std::abs(n[0]) - std::sqrt(0.5f)) < 1e-3f && std::abs(std::abs(n[1]) - std::sqrt(0.5f)) < 1e-3f);
    }
    // Front faces (counter-clockwise: the cross of the triangle's edges) face the normal.
    const auto& tri = t->mesh.triangles;
    for (size_t i = 0; i < tri.size(); i += 3) {
        const auto &a = p[tri[i]], &b = p[tri[i + 1]], &c = p[tri[i + 2]];
        const float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        const float cx = uy * vz - uz * vy, cy = uz * vx - ux * vz;
        const auto& n = t->mesh.normals[tri[i]];
        wound = wound && cx * n[0] + cy * n[1] > 0;
    }
    check(depth, "floor thickness: from the floor down 0.15");
    check(outward, "floor thickness: each strip faces out of the floor");
    check(uvs, "floor thickness: u 0 to 1 along the edge, v 0 at the top to 1 below");
    check(diagonal, "floor thickness: a half tile's diagonal gets its strips");
    check(wound, "floor thickness: wound to face out");
}

// Fences as cFenceGeometryNode places them (docs/roof-fence-water-mac.md
// §2.1): rails turned and sheared to the floor at the far end, scale 1; the
// catalog's default fence for a style it lacks; the diagonal model on a
// diagonal edge and no rail there without one; the +0.75 of the raising
// deck/foundation style (marker 4: both ends for a rail, the point for a
// post); posts not turned; arch end parts only where no arch goes on.
static void testFencePieces() {
    auto catalogAt = [](const char* name, bool diagonal) {
        const auto res = std::filesystem::temp_directory_path() / name;
        std::filesystem::create_directories(res / "Catalog/Scripts");
        std::ofstream f(res / "Catalog/Scripts/fences.txt");
        f << "version 1\nfence 3\nrailModel \"other\"\npostModel \"otherPost\"\n"
          << "fence 2\nrailModel \"rail\"\n" << (diagonal ? "diagRailModel \"diag\"\n" : "")
          << "postModel \"post\"\nnotInCatalog\ndefault\n";
        std::ofstream a(res / "Catalog/Scripts/fencearches.txt");
        a << "version 1\nfenceArch 26\narchCenterModel \"center\"\narchEastModel \"east\"\n"
          << "archWestModel \"west\"\narchPostModel \"apost\"\n";
        return res;
    };
    auto near = [](float a, float b) { return std::abs(a - b) < 1e-4f; };
    const auto res = catalogAt("os2_test_fence_res", true);
    ResourceIndex index(res);
    const PatternCatalog patterns(index, res);
    check(patterns.fenceOrDefault(0x12345678) && patterns.fenceOrDefault(0x12345678)->rail == "rail",
          "fences: a style with no catalog fence takes the default fence (fences.txt's default)");

    constexpr uint32_t kStyle = 0x12345678; // no fence of its own
    Lot lot;
    lot.width = 4, lot.height = 4, lot.levelCount = 1;
    lot.elevations.assign(5 * 5, 0.0f);
    lot.elevations[2 * 5 + 1] = 0.5f; // grid point (2, 1)
    lot.deckFoundationStyles.assign(4 * 4, {-1, -1, -1, -1});
    lot.deckFoundationStyles[2 * 4 + 3][3] = 4; // tile (2, 3), south quarter: touches (2, 3) and (3, 3)
    auto add = [&](float ax, float ay, float bx, float by) {
        Lot::Wall w;
        w.a = {ax, ay, 0}, w.b = {bx, by, 0};
        w.type = kStyle;
        lot.walls.push_back(w);
    };
    add(1, 1, 2, 1); // sloped: (2, 1) is 0.5 up
    add(1, 1, 2, 2); // diagonal
    add(2, 3, 3, 3); // both ends on the raising style
    add(3, 3, 4, 3); // one end only
    lot.fencePosts = {{0, 3, 3, kStyle}, {0, 4, 3, kStyle}, {0, 1, 2, 26}};
    Lot::WallGraph arches;
    arches.instance = 0x13;
    lot.wallGraphs.push_back(arches);
    addFenceArch(lot, 0, 0, 0, 1, 0);
    addFenceArch(lot, 0, 1, 0, 2, 0); // in line with the first
    addFenceArch(lot, 0, 0, 4, 0, 3); // alone, going -y

    const auto pieces = lotFencePieces(lot, patterns);
    auto find = [&](const std::string& model, float x, float y) -> const LotFencePiece* {
        for (const auto& p : pieces)
            if (p.model == model && std::abs(p.transform[12] - x) < 1e-4f && std::abs(p.transform[13] - y) < 1e-4f)
                return &p;
        return nullptr;
    };
    auto count = [&](const std::string& model) {
        return std::count_if(pieces.begin(), pieces.end(), [&](const LotFencePiece& p) { return p.model == model; });
    };
    const auto* slope = find("rail", 1, 1);
    check(slope && near(slope->transform[0], 1) && near(slope->transform[1], 0) && near(slope->transform[2], 0.5f) &&
              near(slope->transform[4], 0) && near(slope->transform[5], 1) && near(slope->transform[14], 0),
          "fences: a rail is sheared up to the floor at its far end, scale 1");
    const auto* diag = find("diag", 1, 1);
    const float r = std::sqrt(0.5f);
    check(diag && near(diag->transform[0], r) && near(diag->transform[1], r) && near(diag->transform[2], 0) &&
              near(diag->transform[4], -r) && near(diag->transform[5], r),
          "fences: a diagonal edge takes the diagonal rail, turned 45 degrees, not stretched");
    const auto* raisedRail = find("rail", 2, 3);
    const auto* oneEnd = find("rail", 3, 3);
    check(raisedRail && near(raisedRail->transform[14], 0.75f) && oneEnd && near(oneEnd->transform[14], 0),
          "fences: a rail is raised 0.75 when both ends touch deck/foundation style 4, not one");
    const auto* post = find("post", 3, 3);
    const auto* plainPost = find("post", 4, 3);
    check(post && near(post->transform[14], 0.75f) && plainPost && near(plainPost->transform[14], 0) &&
              near(post->transform[0], 1) && near(post->transform[1], 0),
          "fences: a post on a point touching style 4 is raised 0.75, not turned");
    check(!find("post", 1, 2) && !find("otherPost", 1, 2), "fences: an arch-style post is left to the arch");
    check(count("center") == 3 && count("apost") == 6, "fences: every arch has its centre and two posts");
    check(find("west", 0, 0) && !find("west", 1, 0) && find("east", 2, 0) && !find("east", 1, 0),
          "fences: arches in a line keep only the outer end parts");
    const auto* turned = find("west", 0, 4);
    check(turned && near(turned->transform[0], 0) && near(turned->transform[1], -1) && find("east", 0, 3),
          "fences: a lone arch has both end parts, turned with it");
    const auto* archPost = find("apost", 0, 4);
    check(archPost && near(archPost->transform[0], 1) && near(archPost->transform[1], 0),
          "fences: arch posts are not turned");

    // Without a diagonal model the diagonal edge draws nothing (no straight
    // rail in its place).
    const auto res2 = catalogAt("os2_test_fence_res_nodiag", false);
    ResourceIndex index2(res2);
    const PatternCatalog plain(index2, res2);
    const auto noDiag = lotFencePieces(lot, plain);
    const bool straightOnDiagonal = std::any_of(noDiag.begin(), noDiag.end(), [&](const LotFencePiece& p) {
        return p.model == "rail" && std::abs(p.transform[0] - p.transform[1]) < 1e-4f;
    });
    check(!straightOnDiagonal, "fences: no straight rail on a diagonal edge without a diagonal model");

    // A half wall (a catalog fence with ishalfwall, PartitionType 3) on the
    // raising style: wall node 5, raised 0.75 (the base catalog has none).
    // Its rails and posts are flagged for the walls-down mode, and its post
    // is raised only where a half wall of the style ends (FUN_00a7098c).
    PatternCatalog halfWalls = patterns;
    halfWalls.setFence(kStyle, {"hrail", "", "hpost", true});
    lot.wallPatterns[1] = "paper";
    Lot half;
    half = lot;
    half.walls.clear();
    half.fencePosts = {{0, 3, 3, kStyle}};
    {
        // No half wall ends at (3, 3) yet: its post stays down.
        const auto pieces = lotFencePieces(half, halfWalls);
        auto lone = std::find_if(pieces.begin(), pieces.end(), [](const LotFencePiece& p) { return p.model == "hpost"; });
        check(lone != pieces.end() && lone->halfWall && near(lone->transform[14], 0),
              "half walls: a post on style 4 with no half wall of its style there is not raised");
    }
    Lot::Wall hw;
    hw.a = {2, 3, 0}, hw.b = {3, 3, 0};
    hw.type = kStyle;
    hw.patterns = {1, 1};
    half.walls.push_back(hw);
    {
        const auto hp = lotFencePieces(half, halfWalls);
        auto at = [&](const std::string& model, float x) -> const LotFencePiece* {
            for (const auto& p : hp)
                if (p.model == model && std::abs(p.transform[12] - x) < 1e-4f)
                    return &p;
            return nullptr;
        };
        const auto *rail = at("hrail", 2), *post = at("hpost", 3);
        check(rail && rail->halfWall && post && post->halfWall && near(post->transform[14], 0.75f),
              "half walls: rails and posts flagged for the walls-down mode; the post raised where a half wall ends");
    }
    const ObjectModel m = buildLotModel(half, index, halfWalls);
    float bottom = 1e9f, top = -1e9f;
    for (const auto& part : m.parts)
        if (part.node == "wall:paper")
            for (const auto& v : part.mesh.positions)
                bottom = std::min(bottom, v[2]), top = std::max(top, v[2]);
    check(near(bottom, 0.75f) && near(top, 0.75f + 1.025f), "half walls: node 5 raised 0.75 on deck/foundation style 4");
}

// With the game's data: N001_Lot68's deck/foundation markers (3ARY 0x14):
// the pool's 3 under the ground (level -1), the foundation's 0 and the
// deck's 1 under the raised floor (level 0).
static void testDeckFoundationMarkers() {
    const char* data = std::getenv("OS2_DATA");
    if (!data)
        return;
    const std::filesystem::path lotPath =
        std::filesystem::path(data) / "TSData/Res/UserData/Neighborhoods/N001/Lots/N001_Lot68.package";
    if (!std::filesystem::exists(lotPath))
        return;
    DbpfPackage pkg(lotPath);
    const Lot lot = Lot::load(pkg);
    std::map<std::pair<int, int32_t>, int> counts;
    for (int level = lot.minLevel; level < lot.minLevel + lot.levelCount; ++level)
        for (int x = 0; x < int(lot.width); ++x)
            for (int y = 0; y < int(lot.height); ++y)
                for (int q = 0; q < 4; ++q)
                    if (const int32_t s = lot.deckFoundationStyle(level, x, y, q); s != -1)
                        ++counts[{level, s}];
    check(counts.size() == 3 && counts[{-1, 3}] == 160 && counts[{0, 0}] == 728 && counts[{0, 1}] == 36,
          "lot 68: the deck/foundation markers (3ARY 0x14): pool 3 on level -1, foundation 0 and deck 1 on level 0");
}

static void testTwoTileArchMask() {
    const char* data = std::getenv("OS2_DATA");
    if (!data)
        return;
    const std::filesystem::path lotPath =
        std::filesystem::path(data) / "TSData/Res/UserData/Neighborhoods/N001/Lots/N001_Lot82.package";
    if (!std::filesystem::exists(lotPath))
        return;
    ResourceIndex index(data);
    const PatternCatalog patterns(index, data);
    DbpfPackage pkg(lotPath);
    const Lot lot = Lot::load(pkg);
    const ObjectModel m = buildLotModel(lot, index, patterns);
    // The mask values at a point of the masked wall parts lying on y = 39
    // within [x0, x1] on level 1 (z 0.75 .. 3.75): both faces, each with its
    // own mask (the s face's mirrored), and the wall top.
    auto values = [&](float x0, float x1, float x, float z) {
        std::vector<int> out;
        for (const auto& part : m.parts) {
            if (part.mask < 0 || size_t(part.mask) >= m.masks.size() || !m.masks[size_t(part.mask)].planar)
                continue;
            const auto& p = part.mesh.positions;
            if (p.empty())
                continue;
            bool on = true;
            for (const auto& v : p)
                on = on && std::abs(std::abs(v[1] - 39.0f) - 0.0f) < 0.2f && v[0] > x0 - 0.2f && v[0] < x1 + 0.2f &&
                     v[2] > 0.7f && v[2] < 3.8f;
            if (!on)
                continue;
            const auto& k = m.masks[size_t(part.mask)];
            const float u = k.planeU[0] * x + k.planeU[1] * 39.0f + k.planeU[2] * z + k.planeU[3];
            const float v = k.planeV[0] * x + k.planeV[1] * 39.0f + k.planeV[2] * z + k.planeV[3];
            const uint32_t tx = std::min(k.width - 1, uint32_t(std::clamp(u, 0.0f, 1.0f) * float(k.width)));
            const uint32_t ty = std::min(k.height - 1, uint32_t(std::clamp(v, 0.0f, 1.0f) * float(k.height)));
            out.push_back(k.values[size_t(ty) * k.width + tx]);
        }
        return out;
    };
    const float z = 0.75f + 1.5f; // half way up the door
    const auto inner0 = values(30, 31, 30.05f, z), outer0 = values(30, 31, 30.95f, z);
    const auto inner1 = values(29, 30, 29.95f, z), outer1 = values(29, 30, 29.05f, z);
    auto all = [](const std::vector<int>& v, auto test) { return !v.empty() && std::all_of(v.begin(), v.end(), test); };
    check(inner0.size() >= 2 && inner1.size() >= 2, "lot 82: both faces of the front arch's walls are masked");
    check(all(inner0, [](int v) { return v < 128; }) && all(inner1, [](int v) { return v < 128; }),
          "lot 82: the two-tile arch is open where its tiles meet, on both faces");
    check(all(outer0, [](int v) { return v > 128; }) && all(outer1, [](int v) { return v > 128; }),
          "lot 82: the two-tile arch keeps the wall at its outer edges, on both faces");
    // Walls down: the wall tops take face A's mask too (SetCutoutMaterialImpl
    // gives it to the top), at v = 1 - 0.2 / 3: open over the arch.
    LotViewOptions down;
    down.walls = LotViewOptions::Walls::Down;
    const ObjectModel md = buildLotModel(lot, index, patterns, down);
    int topValue = -1;
    for (const auto& part : md.parts) {
        if (part.node.rfind("walltop:", 0) != 0 || part.mask < 0)
            continue;
        bool on = !part.mesh.positions.empty();
        for (const auto& v : part.mesh.positions)
            on = on && std::abs(v[1] - 39.0f) < 0.2f && v[0] > 29.8f && v[0] < 31.2f && v[2] < 1.0f;
        if (!on)
            continue;
        const auto& k = md.masks[size_t(part.mask)];
        const float x = 30.05f, zt = 0.75f + 0.2f - 0.01f;
        const float u = k.planeU[0] * x + k.planeU[1] * 39.0f + k.planeU[2] * zt + k.planeU[3];
        const float v = k.planeV[0] * x + k.planeV[1] * 39.0f + k.planeV[2] * zt + k.planeV[3];
        const uint32_t tx = std::min(k.width - 1, uint32_t(std::clamp(u, 0.0f, 1.0f) * float(k.width)));
        const uint32_t ty = std::min(k.height - 1, uint32_t(std::clamp(v, 0.0f, 1.0f) * float(k.height)));
        topValue = k.values[size_t(ty) * k.width + tx];
    }
    check(topValue >= 0 && topValue < 128, "lot 82: with the walls down the wall top is open over the arch");
}

// With the game's data: N002_Lot31's sawtooth roofs (shed roofs on level 1,
// each rising to x = 23, 27, ...) have a row of windows in the attic walls
// between them. Those walls are cut for the windows (they were not: only
// normal walls were, so the glass hid behind the wall) and their face over
// the next, lower roof is lit as outside (the room map puts the attic there:
// it was drawn dark).
static void testSawtoothAtticWindows() {
    const char* data = std::getenv("OS2_DATA");
    if (!data)
        return;
    const std::filesystem::path lotPath =
        std::filesystem::path(data) / "TSData/Res/UserData/Neighborhoods/N002/Lots/N002_Lot31.package";
    if (!std::filesystem::exists(lotPath))
        return;
    ResourceIndex index(data);
    const PatternCatalog patterns(index, data);
    DbpfPackage pkg(lotPath);
    const Lot lot = Lot::load(pkg);
    LotLightMapLayout layout;
    const ObjectModel m = buildLotModel(lot, index, patterns, {}, &layout);
    // Wall faces on x = 23, level 1: no wall triangle covers the middle of
    // the window at (23, 29.5..30.5, 4) (a box cut or a mask).
    bool covered = false, masked = false;
    for (const auto& part : m.parts) {
        if (part.node.rfind("wall:", 0) != 0)
            continue;
        const auto& p = part.mesh.positions;
        const auto& t = part.mesh.triangles;
        for (size_t i = 0; i + 2 < t.size(); i += 3) {
            const auto &a = p[t[i]], &b = p[t[i + 1]], &c = p[t[i + 2]];
            if (std::abs(a[0] - 23) > 0.15f || std::abs(b[0] - 23) > 0.15f || std::abs(c[0] - 23) > 0.15f)
                continue;
            // In the (y, z) plane: is (30, 4) inside the triangle?
            auto side = [](const auto& u, const auto& v, float y, float z) {
                return (v[1] - u[1]) * (z - u[2]) - (v[2] - u[2]) * (y - u[1]);
            };
            const float d1 = side(a, b, 30, 4), d2 = side(b, c, 30, 4), d3 = side(c, a, 30, 4);
            const bool inside = (d1 >= 0 && d2 >= 0 && d3 >= 0) || (d1 <= 0 && d2 <= 0 && d3 <= 0);
            if (inside)
                (part.mask >= 0 ? masked : covered) = true;
        }
    }
    check(!covered || masked, "lot 31: the sawtooth's attic wall is cut for its window");
    bool outside = false, found = false;
    for (const auto& w : layout.walls)
        if (w.level == 1 && w.normal[0] > 0.9f && std::abs(w.origin[0] - 23) < 0.2f && w.origin[1] >= 29 &&
            w.origin[1] <= 37) {
            found = true;
            outside = outside || w.room == 0;
        }
    check(found && outside, "lot 31: the attic wall's face over the lower roof is lit as outside");
}

// A level shown with the one below (a raised ground floor on foundations)
// leaves out the next story of a building on the ground: its floor exactly a
// story over the level below at a tile's four corners (decks and foundation
// tops sit at their own height whatever the ground). With the game's data:
// N002_Lot31's sawtooth building has attic walls on level 1, 3 up, while the
// lot's other buildings stand on foundations (level 1 at 0.75); the camera
// on the ground showed those walls cut down as long beams across the room.
static void testNextStoryOverGround() {
    Lot s;
    s.width = 3, s.height = 1, s.minLevel = 0, s.levelCount = 2;
    s.elevations.assign(2 * 4 * 2, 0.0f);
    s.floors.assign(2 * 3, {});
    for (int x = 0; x <= 3; ++x)
        for (int y = 0; y <= 1; ++y)
            s.elevations[size_t((4 + x) * 2 + y)] = x <= 1 ? 0.75f : 3.0f;
    check(!storyOverGround(s, 1, 0, 0) && !storyOverGround(s, 1, 1, 0) && storyOverGround(s, 1, 2, 0),
          "a story over the ground: the level's floor 3 over the one below at the tile's four corners");
    s.floors[size_t(3 + 2)] = {1, 1, 1, 1};
    s.floors[size_t(3 + 0)] = {1, 1, 1, 1};
    check(!WallView::visible(s, 1, 2.5f, 0.5f, 3.0f, 0) && WallView::visible(s, 1, 2.5f, 0.5f, 3.0f) &&
              WallView::visible(s, 1, 0.5f, 0.5f, 0.75f, 0),
          "shown with the level below: what stands on the next story's floor is hidden, not on the foundation's");

    const char* data = std::getenv("OS2_DATA");
    if (!data)
        return;
    const std::filesystem::path lotPath =
        std::filesystem::path(data) / "TSData/Res/UserData/Neighborhoods/N002/Lots/N002_Lot31.package";
    if (!std::filesystem::exists(lotPath))
        return;
    ResourceIndex index(data);
    const PatternCatalog patterns(index, data);
    DbpfPackage pkg(lotPath);
    const Lot lot = Lot::load(pkg);
    WallView view;
    view.study(lot);
    check(view.shownLevel(0) == 1 && view.lowestCut(1) == 0, "lot 31: the ground is shown with level 1 (foundations)");
    LotModelCache cache;
    const ObjectModel m = buildLotModel(lot, index, patterns, view.options({0.6f, 0.8f}, 1), nullptr, &cache);
    // Wall triangles on the sawtooth's attic lines x = 27 and 31 (level 1,
    // from z 3 up), across the building (y 28..38); and the level-1 walls
    // of the buildings on foundations (z under 3.75 elsewhere).
    bool sawtooth = false, foundation = false;
    for (const auto& part : m.parts) {
        if (part.node.rfind("wall:", 0) != 0)
            continue;
        const auto& p = part.mesh.positions;
        for (const auto& v : p) {
            const bool onLine = std::abs(v[0] - 27) < 0.15f || std::abs(v[0] - 31) < 0.15f;
            if (onLine && v[1] > 28.5f && v[1] < 37.5f && v[2] > 3.01f)
                sawtooth = true;
            if (v[0] < 18 && v[2] > 1 && v[2] < 3.7f)
                foundation = true;
        }
    }
    check(!sawtooth, "lot 31: the sawtooth's attic walls are not drawn with the ground floor");
    check(foundation, "lot 31: the buildings on foundations keep their floor's walls");
}

// os2::logOnce (round 24, docs/game-flow-mac.md §5): an error a caller recovers from is
// on stderr once per (where, key), at most kLogOnceKeys keys per `where`,
// then one "not shown" line.
//
// dup/dup2/fileno/close are _dup/... on Windows (<io.h>).
#if defined(_WIN32)
static int dupFd(int fd) { return _dup(fd); }
static int dup2Fd(int fd, int target) { return _dup2(fd, target); }
static int fileNo(FILE* f) { return _fileno(f); }
static void closeFd(int fd) { _close(fd); }
#else
static int dupFd(int fd) { return dup(fd); }
static int dup2Fd(int fd, int target) { return dup2(fd, target); }
static int fileNo(FILE* f) { return fileno(f); }
static void closeFd(int fd) { close(fd); }
#endif

static void testLogOnce() {
    std::fflush(stderr);
    const int saved = dupFd(2);
    FILE* tmp = std::tmpfile();
    if (saved < 0 || !tmp) {
        check(false, "logOnce: stderr redirect");
        return;
    }
    dup2Fd(fileNo(tmp), 2);
    const std::runtime_error e("unreadable");
    for (int frame = 0; frame < 3; ++frame) // a block that runs every frame
        os2::logOnce("test site", "key A", e);
    os2::logOnce("test site", "key B", e);
    for (int k = 0; k < os2::kLogOnceKeys + 5; ++k)
        os2::logOnce("test flood", std::to_string(k), "bad");
    std::fflush(stderr);
    dup2Fd(saved, 2);
    closeFd(saved);
    std::rewind(tmp);
    std::string out;
    for (int c; (c = std::fgetc(tmp)) != EOF;)
        out += char(c);
    std::fclose(tmp);
    auto count = [&](const std::string& needle) {
        size_t n = 0;
        for (size_t at = out.find(needle); at != std::string::npos; at = out.find(needle, at + 1))
            ++n;
        return n;
    };
    check(count("opensims2: test site (key A): unreadable\n") == 1, "logOnce: once per key");
    check(count("opensims2: test site (key B): unreadable\n") == 1, "logOnce: another key");
    check(count("opensims2: test flood (") == size_t(os2::kLogOnceKeys), "logOnce: keys per site capped");
    check(count("opensims2: test flood: more errors like these are not shown\n") == 1, "logOnce: one line past the cap");
}

// Terrain water (docs/roof-fence-water-mac.md 3): Terrain.ini's curve and
// the lake bed / beach paints of cWaterGeometryBuilder::PaintUnderWaterTerrain.
static void testLotWater() {
    const LotWaterParams p = LotWaterParams::parse("[WaterRepresentationParameters]\nWaterToTerrainSurfaceOffset=.5\n"
                                                   "[WaterRenderParameters]\nAlphaVsDepthCurve= .25,62,  10,110\n"
                                                   ";AlphaVsDepthCurve= 0,128,  0.5,255\nShallowPaintDepthMin=0\n"
                                                   "DeepPaintDepthMin=0.5\nShallowPaintDepthMax=5\nDeepPaintDepthMax=2\n"
                                                   "BeachPaintStartDepth=-0.35\nBeachPaintPeakDepth=-0.1\n"
                                                   "BeachPaintEndDepth=0.3\nMaxBeachWidth=1.5\nBeachIntensityFactor=3.2\n");
    check(p.alphaVsDepth.size() == 2 && p.alpha(0.1f) == 62 && p.alpha(20) == 110, "water alpha curve ends");
    check(std::abs(p.alpha(1) - (62 + 0.75f * 48 / 9.75f)) < 1e-3f, "water alpha curve between");
    check(p.deepMax == 2 && p.beachEnd == 0.3f && p.beachIntensity == 3.2f, "water paint parameters");
    check(LotWaterParams::parse("").alpha(4) == 255 && LotWaterParams::parse("").deepMin == 0.4f, "water defaults");

    // 2x1 lot sloping down to x = 2 (0, -1, -3) under a water level of 0.
    Lot lot;
    lot.width = 2, lot.height = 1, lot.levelCount = 1;
    lot.elevations = {0, 0, -1, -1, -3, -3};
    lot.water.assign(6, 0.0f);
    const LotWaterPaints w = lotWaterPaints(lot, p, 5, 3, 0.5f);
    check(w.shallow.size() == 15 && w.beach[0] == 191 && w.shallow[0] == 0 && w.deep[0] == 0, "water paint at the shore");
    check(w.shallow[1] == 255 && w.deep[1] == 0, "shallow lake bed peaks at DeepPaintDepthMin");
    check(w.shallow[2] == 227 && w.deep[2] == 84, "lake bed at depth 1");
    check(w.shallow[4] == 114 && w.deep[4] == 255 && w.beach[4] == 0, "lake bed at depth 3");
    // Dry land 0.2 above the water next to it: beach scaled by the distance.
    lot.elevations = {0.2f, 0.2f, -1, -1, -3, -3};
    const LotWaterPaints d = lotWaterPaints(lot, p, 5, 3, 0.5f);
    const float b = (0.15f) * 255 / 0.25f * (1 - 1 / 1.5f) * 3.2f;
    check(d.beach[0] == uint8_t(int(b)), "beach above the water fades with the distance to it");
}

// The game's roofs (docs/roof-fence-water-mac.md §1.5): two triangles per
// half tile, the atlas cell of each from its vertices (plain 60, the ridge's
// 4, the hip lines' 2/0x42...), the triangles under another roof hidden.
static void testGameRoofs() {
    Lot lot;
    lot.width = 16, lot.height = 16, lot.levelCount = 2;
    lot.elevations.assign(17 * 17 * 2, 0.0f);
    for (size_t i = 17 * 17; i < lot.elevations.size(); ++i)
        lot.elevations[i] = 3.0f;
    auto roof = [](uint32_t id, uint32_t type, float x0, float y0, float x1, float y1) {
        Lot::Roof r;
        r.id = id, r.type = type, r.level = 1, r.levelTo = 1, r.angle = 0.5f;
        r.x0 = x0, r.y0 = y0, r.x1 = x1, r.y1 = y1;
        return r;
    };
    // The cell (row, column) of the 5 x 5 atlas a top triangle samples.
    auto cell = [](const MeshGroup& g, size_t t) {
        float u = 0, v = 0;
        for (int k = 0; k < 3; ++k)
            u += g.uvs[g.triangles[t + size_t(k)]][0], v += g.uvs[g.triangles[t + size_t(k)]][1];
        return std::pair{int(v / 3 / 0.19921875f), int(u / 3 / 0.19921875f)};
    };
    {
        Lot one = lot;
        one.roofs = {roof(1, Lot::LongGable, 2, 2, 8, 5)};
        const LotRoofs roofs(one);
        const LotRoofs::Meshes m = roofs.build();
        // 6 x 3 tiles with the eaves: 15 x 9 grid points, 14 x 8 cells.
        check(m.top.triangles.size() == 3 * 2 * 14 * 8 && m.under.triangles.size() == m.top.triangles.size(),
              "game gable: two triangles per half-tile cell, top and underside");
        check(std::abs(roofs.heightAt(5, 3.5f, 1, -1) - 4.58f) < 1e-4f && std::abs(roofs.heightAt(5, 2, 1, -1) - 3.08f) < 1e-4f,
              "game gable: ridge 0.5 per notch over the eave tip at -0.42, the wall line at +0.08");
        int plain = 0, ridge = 0, other = 0;
        for (size_t t = 0; t < m.top.triangles.size(); t += 3) {
            bool onRidge = false;
            for (int k = 0; k < 3; ++k)
                onRidge |= m.top.positions[m.top.triangles[t + size_t(k)]][2] > 4.57f;
            const auto [row, col] = cell(m.top, t);
            if (onRidge ? (row == 0 && col == 2) : (row == 3 && col == 0))
                ++(onRidge ? ridge : plain);
            else
                ++other;
        }
        check(other == 0 && ridge == 2 * 14 * 2 && plain > 0,
              "game gable: the ridge's triangles take cell 4, the rest cell 60");
        check(!m.edges.triangles.empty() && m.edges.triangles.size() == m.trim.triangles.size(),
              "game gable: the upper and lower strips round the border");
    }
    {
        Lot one = lot;
        one.roofs = {roof(1, Lot::Hip, 2, 2, 8, 6)};
        const LotRoofs::Meshes m = LotRoofs(one).build();
        bool hipCell = false;
        for (size_t t = 0; t < m.top.triangles.size(); t += 3) {
            const auto [row, col] = cell(m.top, t);
            hipCell |= (row == 0 && col == 1) || (row == 3 && col == 3); // bases 2 and 0x42
        }
        check(hipCell, "game hip: the hip lines' triangles take their own cells");
    }
    {
        // Two gables crossing: each loses the triangles under the other.
        Lot two = lot;
        two.roofs = {roof(1, Lot::LongGable, 2, 6, 12, 9), roof(2, Lot::LongGable, 6, 2, 9, 12)};
        Lot a = lot, b = lot;
        a.roofs = {two.roofs[0]};
        b.roofs = {two.roofs[1]};
        const size_t alone = LotRoofs(a).build().top.triangles.size() + LotRoofs(b).build().top.triangles.size();
        const size_t both = LotRoofs(two).build().top.triangles.size();
        check(both < alone && both > alone / 2, "game roofs: a roof's triangles under another roof are hidden");
    }
}

// The lot's saved UI state (cTSMainToolbar::Write/Read, resource
// 0x8B0C79D6): version 2 as the base game's lots (paused, floor, wall mode
// and a fifth u32), version 0 paused with one more u32; written back as
// read. With OS2_DATA: N001_Lot11's (running, floor 1, cutaway).
static void testLotUiState() {
    os2::LotUiState s;
    s.paused = true, s.level = 2, s.wallMode = 3;
    const auto bytes = os2::writeLotUiState(s);
    const auto back = os2::parseLotUiState(bytes);
    check(bytes.size() == 64 + 20 && back && back->version == 2 && back->paused && back->level == 2 && back->wallMode == 3,
          "lot UI state: written and read back");
    std::vector<uint8_t> v0(64, 0);
    for (uint32_t w : {0u, 0u, 7u, 1u, 2u})
        for (int k = 0; k < 4; ++k)
            v0.push_back(uint8_t(w >> (8 * k)));
    const auto old = os2::parseLotUiState(v0);
    check(old && old->paused && old->level == 1 && old->wallMode == 2, "lot UI state: version 0 is paused, one u32 more");
    check(!os2::parseLotUiState(std::vector<uint8_t>(70, 0)), "lot UI state: too short");
    if (const char* data = std::getenv("OS2_DATA")) {
        os2::DbpfPackage pkg(std::filesystem::path(data) / "TSData/Res/UserData/Neighborhoods/N001/Lots/N001_Lot11.package");
        const auto ui = os2::readLotUiState(pkg);
        check(ui && ui->version == 2 && !ui->paused && ui->level == 1 && ui->wallMode == 1,
              "lot UI state: N001_Lot11 running, on the ground, cutaway");
    }
}

// nTSSG::cShadow's GUOB planes (docs/lighting-mac.md 12): the subsets, the
// tile rectangle round an object, the sides by its direction, what is on.
static void testObjectShadow() {
    check(guobIndex("groundshadow") == kGuobGround && guobIndex("WallShadow") == kGuobWall &&
              guobIndex("northwallshadow") == kGuobNorth && guobIndex("southwallshadow") == kGuobSouth &&
              guobIndex("eastwallshadow") == kGuobEast && guobIndex("westwallshadow") == kGuobWest &&
              guobIndex("shadow") == -1,
          "GUOB subset names in kGUOBSubsetNames' order");
    ObjectModel m;
    auto part = [&](const char* subset, const char* material, std::array<float, 3> a, std::array<float, 3> b) {
        ObjectModel::Part p;
        p.mesh.name = subset;
        p.material = material;
        p.mesh.positions = {a, b};
        m.parts.push_back(p);
    };
    part("body", "wood", {-0.4f, -0.45f, 0}, {0.4f, 0.45f, 0.9f});
    part("groundshadow", "dresserShadow", {-0.5f, -0.5f, 0.01f}, {0.5f, 0.5f, 0.01f});
    part("northwallshadow", "dresserWallShadow", {-0.5f, 0.49f, 0}, {0.5f, 0.49f, 1});
    part("southwallshadow", "GUOBOff", {-0.5f, -0.49f, 0}, {0.5f, -0.49f, 1});
    const GuobModelInfo info = guobModelInfo(m);
    check(info.present == ((1 << kGuobGround) | (1 << kGuobNorth)), "GUOBs present: a GUOBOff saved is none");
    check(info.hasBounds && info.lo[0] == -0.5f && info.hi[1] == 0.5f && info.hi[2] == 1.0f,
          "the model's box takes every part, the shadow planes too");

    // Placed in tile (3, 5) facing direction 0 (turned 90 degrees).
    Xform at{quatFromEulerXYZ(0, 0, 1.5707963f), {3.5f, 5.5f, 0}};
    std::array<float, 3> lo, hi;
    guobWorldBox(at, info.lo, info.hi, lo, hi);
    const GuobTileRect r = guobTileRect(lo, hi);
    check(r.x0 == 3 && r.x1 == 4 && r.y0 == 5 && r.y1 == 6, "a one-tile object's tile rectangle");
    check(guobTileRect({2.9f, 5.0f, 0}, {4.1f, 6.0f, 0}).x0 == 2 && guobTileRect({2.9f, 5.0f, 0}, {4.1f, 6.0f, 0}).x1 == 5 &&
              guobTileRect({2.9f, 5.0f, 0}, {4.1f, 6.0f, 0}).y0 == 5 && guobTileRect({2.9f, 5.0f, 0}, {4.1f, 6.0f, 0}).y1 == 6,
          "the rectangle is floor / ceil of the box, exact edges kept");

    // Walls on the low-y edge (3,5)-(4,5) and the high-x edge (4,5)-(4,6).
    GuobWorld w;
    w.wallMode = 2;
    w.wallExists = [](int x0, int y0, int x1, int y1, int level) {
        auto edge = [&](int ax, int ay, int bx, int by) {
            return (x0 == ax && y0 == ay && x1 == bx && y1 == by) || (x0 == bx && y0 == by && x1 == ax && y1 == ay);
        };
        return level == 0 && (edge(3, 5, 4, 5) || edge(4, 5, 4, 6));
    };
    auto sides = [&](int dir) { return guobWallSides(r, dir, 0, true, w); };
    constexpr uint8_t N = 1 << kGuobNorth, S = 1 << kGuobSouth, E = 1 << kGuobEast, W = 1 << kGuobWest;
    check(sides(0) == (S | W), "direction 0: north is the low-x side, west the low-y side");
    check(sides(2) == (S | E), "direction 2 turns the sides");
    check(sides(4) == (N | E), "direction 4 turns the sides");
    check(sides(6) == (N | W), "direction 6 turns the sides");
    check(guobWallSides(r, 0, 1, true, w) == 0, "the walls of another level do not count");
    check(guobWallSides(r, 0, 0, false, w) == 0, "a level not shown has no wall planes");
    GuobWorld roof = w;
    roof.wallMode = 3;
    check(guobWallSides(r, 0, 0, true, roof) == 0, "the base game's wall planes need wall mode 2, not 3");

    const uint8_t all = kAllGuobs;
    check(guobsOn(all, true, S | W, w) == ((1 << kGuobGround) | (1 << kGuobWall) | S | W), "walls up: ground, wall and the sides");
    GuobWorld cut = w;
    cut.wallMode = 1;
    check(guobsOn(all, true, 0, cut) == ((1 << kGuobGround) | (1 << kGuobWall)), "cutaway: the ground and wallshadow planes");
    GuobWorld down = w;
    down.wallMode = 0;
    check(guobsOn(all, true, 0, down) == (1 << kGuobGround), "walls down: the ground plane only");
    check(guobsOn(all, false, S, w) == 0, "a hidden shadow has none on");
    check(guobsOn(info.present, true, S | N, w) == ((1 << kGuobGround) | N), "only the planes the model has");
}

// The empty lot package: the 96-byte stub the game writes for a lot with
// nothing in it (N002_Lot32 in the base game's N002). The game loads it as a
// lot; ours says so apart from the load errors.
static void testEmptyLotPackage() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "os2_test_empty_lot";
    fs::remove_all(dir);
    fs::create_directories(dir);
    DbpfWriter w;
    w.save(dir / "empty.package");
    DbpfPackage pkg(dir / "empty.package");
    check(pkg.entries().empty(), "empty lot package: the stub has no resources");
    bool empty = false;
    try {
        Lot::load(pkg);
    } catch (const EmptyLotPackage&) {
        empty = true;
    } catch (const std::exception&) {
    }
    check(empty, "empty lot package: not a load error");
    fs::remove_all(dir);
}

int main() {
    testObjectShadow();
    testLogOnce();
    testLotUiState();
    testDxt1();
    testHeightMapToNormalMap();
    testDxt3And5();
    testRaw();
    testPng();
    testNameHash();
    testStrings();
    testSimAntics();
    testBhav();
    testInteractionTable();
    testSlotVersions();
    testInteractionTableVersions();
    testCameraConfig();
    testLightingConfig();
    testStandardMaterial();
    testPropertySet();
    testLotModel();
    testWallGeometry();
    testFloorThickness();
    testFencePieces();
    testDeckFoundationMarkers();
    testEmptyLotPackage();
    testLotLightMap();
    testLotWater();
    testGameRoofs();
    testTwoTileArchMask();
    testSawtoothAtticWindows();
    testNextStoryOverGround();
    testFaceSkinMigration();
    testCasRandomize();
    testCasRandom();
    testFaceRegionMasks();
    testBlendFaces();
    testFaceOverlayLayers();
    testNakedOutfitKeepsHair();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("all format tests passed\n");
    return failures ? 1 : 0;
}
