#include "lot.hpp"

#include "binreader.hpp"
#include "dbpf.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace os2 {

namespace {

constexpr uint32_t kType2DArray = 0x6B943B43, kType3DArray = 0x2A51171B, kTypeStringMap = 0xCAC4FC40,
                   kTypeWallGraph = 0x0A284D0B, kTypeWallLayer = 0x8A84D7B0, kTypeLotTexture = 0x4B58975B,
                   kTypeObject = 0xFA1C39F7, kTypeFencePosts = 0xAB4BA572, kTypeRoof = 0xAB9406AA,
                   kTypePool = 0x0C900FDB;
constexpr uint32_t kArrayFloors = 0x00, kArrayElevation = 0x01, kArrayLocked = 0x03, kArrayDeckFoundations = 0x14, kArrayRooms = 0x09, kArrayRoomsLoadBearing = 0x0A, kArrayRoomsLight = 0x0B, kArrayFloorVisibility = 0x15, kArrayFloorRotations = 0x20, kSmapWalls = 0x0D, kSmapFloors = 0x0E,
                   kWallLayerPartitions = 0x04, kPaintMaskFirst = 0x5CBC,
                   kFencePostLayer = 0x11, kWaterLevels = 0x3B76;
constexpr size_t kFileNameSize = 64;

// Every lot resource: 64-byte file name, block id, version.
BinReader blockReader(const std::vector<uint8_t>& bytes, uint32_t blockId, uint32_t* version = nullptr) {
    BinReader r(bytes);
    r.skip(kFileNameSize);
    const uint32_t id = r.u32();
    if (id != blockId)
        throw FormatError("block id " + hexString(id) + ", expected " + hexString(blockId));
    const uint32_t v = r.u32();
    if (version)
        *version = v;
    return r;
}

void expectName(BinReader& r, const char* name) {
    const std::string n = r.str7();
    if (n != name)
        throw FormatError("block name \"" + n + "\", expected \"" + name + "\"");
}

struct Array3 {
    uint32_t x = 0, y = 0, z = 0;
    std::span<const uint8_t> data;
};

Array3 readArray3(BinReader& r, size_t elementSize) {
    expectName(r, "c3DArray");
    Array3 a;
    a.x = r.u32(), a.y = r.u32(), a.z = r.u32();
    a.data = r.bytes(size_t(a.x) * a.y * a.z * elementSize);
    return a;
}

std::map<uint16_t, std::string> readStringMap(BinReader& r) {
    expectName(r, "cStringMap");
    std::map<uint16_t, std::string> out;
    for (uint32_t n = r.u32(); n; --n) {
        std::string s = r.str7();
        const uint16_t ref = r.u16();
        r.u32(); // usage count
        out[ref] = std::move(s);
    }
    return out;
}

std::optional<std::vector<uint8_t>> readResource(DbpfPackage& pkg, uint32_t type, uint32_t instance) {
    for (const auto& e : pkg.entries())
        if (e.tgi.type == type && e.tgi.instance == instance)
            return pkg.read(e);
    return std::nullopt;
}

// cObject, possibly wrapped by subclass blocks (cAnimatable, cLocomotable,
// cPerson: id, version, name, outermost first) whose own data follows it.
Lot::Object readObject(const std::vector<uint8_t>& bytes) {
    BinReader r(bytes);
    r.skip(kFileNameSize);
    Lot::Object o;
    uint32_t version = 0;
    for (int depth = 0;; ++depth) {
        if (depth == 8)
            throw FormatError("no cObject block");
        r.u32();
        version = r.u32();
        const std::string cls = r.str7();
        if (cls == "cPerson")
            o.person = true;
        if (cls == "cObject")
            break;
    }
    o.model = r.str7();
    // Models with their material choices; a Sim lists one per body part, an
    // object at most one (the same name again).
    for (uint32_t n = r.u32(); n; --n) {
        r.str7();
        if (version >= 0x11)
            r.u32(); // 12 for objects, 10 for Sims
        for (uint32_t m = r.u32(); m; --m) {
            std::string material = r.str7();
            o.materials[r.str7()] = std::move(material);
        }
    }
    for (auto& v : o.position)
        v = r.f32();
    for (auto& v : o.rotation)
        v = r.f32();
    // The scene graph's transform nodes as saved (nTSSG::cObject::
    // LoadFromStream, BV 0x65fae8, sets each node found by name): first the
    // object's own node, named after its OBJD ("Chair - Dining - Club_objt",
    // at the object's position again), then its model's bones as last posed
    // (a dining chair pushed in under the table keeps the "slot_chairdining_
    // trans" of its push-in animation's last frame). Versions before 0x0F
    // add an f32 to each. Then the subclass data (cAnimatable's queue).
    const uint32_t nodes = r.u32();
    for (uint32_t i = 0; i < nodes; ++i) {
        std::string name = r.str7();
        Xform x;
        for (auto& v : x.t)
            v = r.f32();
        x.q.x = r.f32(), x.q.y = r.f32(), x.q.z = r.f32(), x.q.w = r.f32();
        if (version < 0x0F)
            r.f32();
        if (i == 0)
            o.definition = std::move(name);
        else
            o.nodes.emplace_back(std::move(name), x);
    }
    return o;
}

} // namespace

float Lot::elevation(int level, int x, int y) const {
    const int w = int(width) + 1, h = int(height) + 1;
    if (!hasLevel(level) || elevations.empty())
        return 3.0f * float(level);
    x = std::clamp(x, 0, w - 1), y = std::clamp(y, 0, h - 1);
    return elevations[(size_t(level - minLevel) * w + size_t(x)) * h + size_t(y)];
}

float Lot::elevationAt(int level, float x, float y) const {
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    const float fx = x - float(ix), fy = y - float(iy);
    const float a = elevation(level, ix, iy) * (1 - fx) + elevation(level, ix + 1, iy) * fx;
    const float b = elevation(level, ix, iy + 1) * (1 - fx) + elevation(level, ix + 1, iy + 1) * fx;
    return a * (1 - fy) + b * fy;
}

float Lot::waterLevel(int x, int y) const {
    const int w = int(width) + 1, h = int(height) + 1;
    if (water.size() != size_t(w) * size_t(h) || x < 0 || y < 0 || x >= w || y >= h)
        return -std::numeric_limits<float>::infinity();
    return water[size_t(y) * size_t(w) + size_t(x)];
}

const std::array<uint16_t, 4>& Lot::floor(int level, uint32_t x, uint32_t y) const {
    static const std::array<uint16_t, 4> none{};
    if (!hasLevel(level) || x >= width || y >= height || floors.empty())
        return none;
    return floors[(size_t(level - minLevel) * width + x) * height + y];
}

Lot Lot::load(DbpfPackage& pkg) {
    Lot lot;

    // The empty lot package (the 96-byte stub with no resources): the game
    // loads it as a lot with nothing in it, so it is not a load error.
    if (pkg.entries().empty())
        throw EmptyLotPackage("the lot package has no resources (the empty lot stub)");

    // Wall graph first: it holds the lowest level. Of its instances (walls
    // flagged for light blocking, load bearing, ...) the largest lists all of them.
    struct Graph {
        std::map<uint32_t, WallVertex> vertices;
        std::vector<std::array<uint32_t, 3>> walls; // id, vertex a, vertex b
    };
    std::vector<Graph> graphs;
    for (const auto& e : pkg.entries()) {
        if (e.tgi.type != kTypeWallGraph)
            continue;
        try {
            const auto bytes = pkg.read(e);
            WallGraph wg;
            wg.instance = e.tgi.instance;
            BinReader r = blockReader(bytes, kTypeWallGraph, &wg.version);
            expectName(r, "cWallGraph");
            for (auto& h : wg.header)
                h = r.i32();
            const int minLevel = wg.header[2];
            Graph g;
            for (uint32_t n = r.u32(); n; --n) {
                WallGraph::Vertex v;
                v.id = r.u32();
                v.x = r.f32(), v.y = r.f32(), v.level = r.i32();
                wg.vertices.push_back(v);
                g.vertices[v.id] = {v.x, v.y, v.level};
            }
            for (uint32_t n = r.u32(); n; --n)
                wg.rooms.push_back(r.i32());
            for (uint32_t n = r.u32(); n; --n) {
                WallGraph::Line l;
                l.id = r.u32(), l.a = r.u32(), l.roomRight = r.i32(), l.b = r.u32(), l.roomLeft = r.i32();
                wg.lines.push_back(l);
                g.walls.push_back({l.id, l.a, l.b});
            }
            lot.minLevel = minLevel;
            lot.wallGraphs.push_back(std::move(wg));
            graphs.push_back(std::move(g));
        } catch (const std::exception& ex) {
            lot.warnings.push_back("wall graph " + hexString(e.tgi.instance) + ": " + ex.what());
        }
    }
    std::sort(lot.wallGraphs.begin(), lot.wallGraphs.end(),
              [](const WallGraph& a, const WallGraph& b) { return a.instance < b.instance; });

    if (auto bytes = readResource(pkg, kType3DArray, kArrayElevation)) {
        BinReader r = blockReader(*bytes, kType3DArray);
        const Array3 a = readArray3(r, 4);
        if (a.x < 2 || a.y < 2)
            throw FormatError("elevation array " + std::to_string(a.x) + "x" + std::to_string(a.y));
        lot.width = a.x - 1, lot.height = a.y - 1, lot.levelCount = int(a.z);
        lot.elevations.resize(size_t(a.x) * a.y * a.z);
        for (size_t i = 0; i < lot.elevations.size(); ++i)
            lot.elevations[i] = std::bit_cast<float>(le32(&a.data[i * 4]));
    } else {
        throw FormatError("no elevation array (3ARY 0x01): not a lot package?");
    }

    if (auto bytes = readResource(pkg, kType3DArray, kArrayFloors)) {
        BinReader r = blockReader(*bytes, kType3DArray);
        const Array3 a = readArray3(r, 8);
        if (a.x != lot.width || a.y != lot.height || int(a.z) != lot.levelCount) {
            lot.warnings.push_back("floor array size does not match the elevations");
        } else {
            lot.floors.resize(size_t(a.x) * a.y * a.z);
            for (size_t i = 0; i < lot.floors.size(); ++i)
                for (int q = 0; q < 4; ++q)
                    lot.floors[i][size_t(q)] = le16(&a.data[i * 8 + size_t(q) * 2]);
        }
    }

    if (auto bytes = readResource(pkg, kType3DArray, kArrayFloorVisibility)) {
        BinReader r = blockReader(*bytes, kType3DArray);
        const Array3 a = readArray3(r, 4);
        if (a.x == lot.width && a.y == lot.height && int(a.z) == lot.levelCount) {
            lot.floorVisibility.resize(size_t(a.x) * a.y * a.z);
            for (size_t i = 0; i < lot.floorVisibility.size(); ++i)
                for (int q = 0; q < 4; ++q)
                    lot.floorVisibility[i][size_t(q)] = a.data[i * 4 + size_t(q)];
        }
    }
    if (auto bytes = readResource(pkg, kType3DArray, kArrayFloorRotations)) {
        BinReader r = blockReader(*bytes, kType3DArray);
        const Array3 a = readArray3(r, 2);
        if (a.x == lot.width && a.y == lot.height && int(a.z) == lot.levelCount) {
            lot.floorRotations.resize(size_t(a.x) * a.y * a.z);
            for (size_t i = 0; i < lot.floorRotations.size(); ++i)
                lot.floorRotations[i] = uint16_t(a.data[2 * i] | a.data[2 * i + 1] << 8);
        }
    }

    if (auto bytes = readResource(pkg, kType3DArray, kArrayLocked)) {
        BinReader r = blockReader(*bytes, kType3DArray);
        const Array3 a = readArray3(r, 1);
        if (a.x == lot.width && a.y == lot.height && int(a.z) == lot.levelCount)
            lot.locked.assign(a.data.begin(), a.data.begin() + long(size_t(a.x) * a.y * a.z));
        else
            lot.warnings.push_back("locked tile array size does not match the elevations");
    }

    if (auto bytes = readResource(pkg, kType3DArray, kArrayDeckFoundations)) {
        BinReader r = blockReader(*bytes, kType3DArray);
        const Array3 a = readArray3(r, 16);
        if (a.x == lot.width && a.y == lot.height && int(a.z) <= lot.levelCount) {
            lot.deckFoundationBase = lot.minLevel;
            lot.deckFoundationStyles.resize(size_t(a.x) * a.y * a.z);
            for (size_t i = 0; i < lot.deckFoundationStyles.size(); ++i)
                for (int q = 0; q < 4; ++q)
                    lot.deckFoundationStyles[i][size_t(q)] = int32_t(le32(&a.data[i * 16 + size_t(q) * 4]));
        } else {
            lot.warnings.push_back("deck/foundation marker array size does not match the elevations");
        }
    }

    // Rooms: same layout as the floors (checked against the "room" object
    // data of the saved objects: 411 of 418 in-world objects of N001_Lot11).
    if (auto bytes = readResource(pkg, kType3DArray, kArrayRooms)) {
        BinReader r = blockReader(*bytes, kType3DArray);
        const Array3 a = readArray3(r, 8);
        if (a.x != lot.width || a.y != lot.height || int(a.z) != lot.levelCount) {
            lot.warnings.push_back("room array size does not match the elevations");
        } else {
            lot.rooms.resize(size_t(a.x) * a.y * a.z);
            for (size_t i = 0; i < lot.rooms.size(); ++i)
                for (int q = 0; q < 4; ++q)
                    lot.rooms[i][size_t(q)] = le16(&a.data[i * 8 + size_t(q) * 2]);
        }
    }
    for (uint32_t instance : {kArrayRoomsLoadBearing, kArrayRoomsLight})
        if (auto bytes = readResource(pkg, kType3DArray, instance)) {
            BinReader r = blockReader(*bytes, kType3DArray);
            const Array3 a = readArray3(r, 8);
            if (a.x == lot.width && a.y == lot.height && int(a.z) == lot.levelCount) {
                auto& out = lot.roomLayers[instance];
                out.resize(size_t(a.x) * a.y * a.z);
                for (size_t i = 0; i < out.size(); ++i)
                    for (int q = 0; q < 4; ++q)
                        out[i][size_t(q)] = le16(&a.data[i * 8 + size_t(q) * 2]);
            }
        }

    for (auto [instance, out] : {std::pair{kSmapFloors, &lot.floorPatterns}, std::pair{kSmapWalls, &lot.wallPatterns}})
        if (auto bytes = readResource(pkg, kTypeStringMap, instance)) {
            BinReader r = blockReader(*bytes, kTypeStringMap);
            *out = readStringMap(r);
        }

    if (!graphs.empty()) {
        std::map<uint32_t, std::pair<uint32_t, std::array<uint16_t, 2>>> layer; // id -> type, coverings
        if (auto bytes = readResource(pkg, kTypeWallLayer, kWallLayerPartitions)) {
            BinReader r = blockReader(*bytes, kTypeWallLayer);
            expectName(r, "cWallLayer");
            for (uint32_t n = r.u32(); n; --n) {
                const uint32_t id = r.u32(), type = r.u32();
                const uint16_t right = r.u16(), left = r.u16();
                layer[id] = {type, {right, left}};
            }
        }
        // The graph of every partition: of the instances with the most
        // lines (0x05 and 0x18 both list all of them), the one whose line IDs
        // the wall layer names. On some lots (N002_Lot10, N001_Lot32) 0x05
        // keeps older IDs for some lines and only 0x18 matches the layer:
        // taking 0x05 left 213 of N002_Lot10's walls without type or
        // coverings (drawn as nothing) and their coverings on stale entries.
        auto matches = [&](const Graph& g) {
            return size_t(std::count_if(g.walls.begin(), g.walls.end(),
                                        [&](const std::array<uint32_t, 3>& w) { return layer.count(w[0]) != 0; }));
        };
        const Graph* graph = &graphs.front();
        for (const auto& g : graphs)
            if (g.walls.size() > graph->walls.size() ||
                (g.walls.size() == graph->walls.size() && matches(g) > matches(*graph)))
                graph = &g;
        size_t missing = 0;
        for (const auto& [id, a, b] : graph->walls) {
            auto va = graph->vertices.find(a), vb = graph->vertices.find(b);
            if (va == graph->vertices.end() || vb == graph->vertices.end()) {
                ++missing;
                continue;
            }
            Wall w;
            w.id = id;
            w.a = va->second, w.b = vb->second;
            if (auto it = layer.find(id); it != layer.end())
                w.type = it->second.first, w.patterns = it->second.second;
            else
                w.layered = false;
            lot.walls.push_back(w);
        }
        if (missing)
            lot.warnings.push_back(std::to_string(missing) + " walls with unknown vertices");
        for (const auto& [id, entry] : layer)
            if (std::none_of(lot.walls.begin(), lot.walls.end(), [&](const Wall& w) { return w.id == id; }))
                lot.staleWallLayer[id] = {entry.first, entry.second};
    }

    if (auto bytes = readResource(pkg, kTypeLotTexture, 0)) {
        BinReader r = blockReader(*bytes, kTypeLotTexture);
        r.skip(8); // width, height in tiles
        lot.terrainBase = r.str7();
        const uint32_t n = r.u32();
        for (uint32_t i = 0; i < n; ++i) {
            TerrainPaint p;
            p.texture = r.str7();
            if (auto mask = readResource(pkg, kType2DArray, kPaintMaskFirst + i)) {
                BinReader m = blockReader(*mask, kType2DArray);
                expectName(m, "c2DArray");
                const uint32_t w = m.u32(), h = m.u32();
                if (lot.maskWidth && (w != lot.maskWidth || h != lot.maskHeight)) {
                    lot.warnings.push_back("paint mask " + p.texture + " has a different size");
                    continue;
                }
                lot.maskWidth = w, lot.maskHeight = h;
                const auto values = m.bytes(size_t(w) * h);
                p.mask.assign(values.begin(), values.end());
            } else {
                lot.warnings.push_back("no mask for terrain paint " + p.texture);
                continue;
            }
            lot.paints.push_back(std::move(p));
        }
    }

    if (auto bytes = readResource(pkg, kTypeFencePosts, kFencePostLayer)) {
        try {
            BinReader r = blockReader(*bytes, kTypeFencePosts);
            expectName(r, "cFencePostLayer");
            for (uint32_t n = r.u32(); n; --n) {
                FencePost p;
                p.level = r.i32();
                p.x = r.f32(), p.y = r.f32();
                p.fence = r.u32();
                lot.fencePosts.push_back(p);
            }
        } catch (const std::exception& ex) {
            lot.warnings.push_back(std::string("fence posts: ") + ex.what());
        }
    }

    // Layouts from OpenTS2 (RoofCodec, PoolCodec): no block name.
    if (auto bytes = readResource(pkg, kTypeRoof, 0)) {
        try {
            uint32_t version = 0;
            BinReader r = blockReader(*bytes, kTypeRoof, &version);
            for (uint32_t n = r.u32(); n; --n) {
                Roof f;
                f.id = r.u32();
                f.x0 = r.f32(), f.y0 = r.f32(), f.level = r.i32();
                f.x1 = r.f32(), f.y1 = r.f32(), f.levelTo = r.i32();
                f.type = r.u32();
                f.pattern = r.u32();
                if (version >= 2)
                    f.angle = r.f32();
                if (version >= 3)
                    f.style = r.u32();
                lot.roofs.push_back(f);
            }
        } catch (const std::exception& ex) {
            lot.warnings.push_back(std::string("roofs: ") + ex.what());
        }
    }
    if (auto bytes = readResource(pkg, kTypePool, 0)) {
        try {
            BinReader r = blockReader(*bytes, kTypePool);
            for (uint32_t n = r.u32(); n; --n) {
                SwimmingPool p;
                p.id = r.u32();
                p.x = r.f32(), p.y = r.f32();
                p.level = r.i32();
                p.width = r.u32(), p.height = r.u32();
                p.unknown1 = r.u32();
                p.depth = r.f32();
                p.unknown2 = r.u32();
                lot.pools.push_back(p);
            }
        } catch (const std::exception& ex) {
            lot.warnings.push_back(std::string("pools: ") + ex.what());
        }
    }
    if (auto bytes = readResource(pkg, kType2DArray, kWaterLevels)) {
        try {
            BinReader m = blockReader(*bytes, kType2DArray);
            expectName(m, "c2DArray");
            const uint32_t w = m.u32(), h = m.u32();
            if (w != lot.width + 1 || h != lot.height + 1)
                throw FormatError("size " + std::to_string(w) + "x" + std::to_string(h));
            const auto values = m.bytes(size_t(w) * h * 4);
            lot.water.resize(size_t(w) * h);
            for (size_t i = 0; i < lot.water.size(); ++i)
                lot.water[i] = std::bit_cast<float>(le32(&values[i * 4]));
        } catch (const std::exception& ex) {
            lot.warnings.push_back(std::string("water levels: ") + ex.what());
        }
    }

    for (const auto& e : pkg.entries()) {
        if (e.tgi.type != kTypeObject)
            continue;
        try {
            Object o = readObject(pkg.read(e));
            o.instance = e.tgi.instance;
            lot.objects.push_back(std::move(o));
        } catch (const std::exception& ex) {
            lot.warnings.push_back("object " + hexString(e.tgi.instance) + ": " + ex.what());
        }
    }
    std::sort(lot.objects.begin(), lot.objects.end(),
              [](const Object& a, const Object& b) { return a.instance < b.instance; });
    return lot;
}

} // namespace os2
