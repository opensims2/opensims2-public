// s2pkg: inspect and extract The Sims 2 .package (DBPF) files.
#include "anim_cmds.hpp"
#include "budget.hpp"
#include "prim_cmds.hpp"
#include "effects_cmds.hpp"
#include "shadow_cmds.hpp"
#include "script_cmds.hpp"
#include "simlook_cmds.hpp"
#include "material_cmds.hpp"
#include "audio_cmds.hpp"
#include "movie_cmds.hpp"
#include "ui_cmds.hpp"
#include "buy_cmds.hpp"
#include "build_cmds.hpp"
#include "cas_cmds.hpp"
#include "hood_cmds.hpp"
#include "hood_play_cmds.hpp"
#include "pie_cmds.hpp"
#include "tutorial_cmds.hpp"
#include "autonomy_cmds.hpp"
#include "wants_cmds.hpp"
#include "route_cmds.hpp"
#include "state_cmds.hpp"
#include "save_cmds.hpp"
#include "vm/lot_resume.hpp"
#include "describe.hpp"
#include "formats/dbpf.hpp"
#include "formats/gmdc.hpp"
#include "formats/image.hpp"
#include "formats/lot.hpp"
#include "formats/lot_objects.hpp"
#include "formats/lot_model.hpp"
#include "formats/model.hpp"
#include "formats/simantics.hpp"
#include "formats/resindex.hpp"
#include "formats/neighborhood.hpp"
#include "vm/hood_bridge.hpp"
#include "vm/lot.hpp"
#include "vm/lot_light.hpp"
#include "vm/vm.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <memory>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

using namespace os2;

namespace {

std::string typeLabel(uint32_t t) {
    char hex[16];
    std::snprintf(hex, sizeof hex, "0x%08X", t);
    if (auto n = typeName(t); !n.empty())
        return std::string(n);
    // Many Sims 1-era types are big-endian FourCCs ("SPR2", "DGRP", "BMP_").
    std::string cc;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const char c = char((t >> shift) & 0xFF);
        if (c < 0x20 || c > 0x7E)
            return hex;
        cc += c;
    }
    return cc;
}

std::string tgiString(const Tgi& k) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%08X-%08X-%08X-%08X", k.type, k.group, k.instanceHi, k.instance);
    return buf;
}

int cmdLs(DbpfPackage& pkg) {
    for (const auto& e : pkg.entries()) {
        std::printf("%-6s %s  off=%-10u size=%-9u", typeLabel(e.tgi.type).c_str(), tgiString(e.tgi).c_str(),
                    e.offset, e.size);
        if (e.rawSize)
            std::printf(" raw=%u", *e.rawSize);
        std::printf("\n");
    }
    return 0;
}

int cmdStats(DbpfPackage& pkg) {
    struct Acc { size_t count = 0; uint64_t bytes = 0; size_t compressed = 0; };
    std::map<uint32_t, Acc> byType;
    for (const auto& e : pkg.entries()) {
        auto& a = byType[e.tgi.type];
        ++a.count;
        a.bytes += e.size;
        a.compressed += e.compressed();
    }
    const auto& h = pkg.header();
    std::printf("DBPF %u.%u  index %u.%u  entries=%u\n", h.major, h.minor, h.indexMajor,
                h.indexMinor ? h.indexMinor - 1 : 0, h.indexCount);
    std::printf("%-12s %8s %12s %10s\n", "type", "count", "bytes", "qfs");
    for (const auto& [t, a] : byType)
        std::printf("%-12s %8zu %12llu %10zu\n", typeLabel(t).c_str(), a.count, (unsigned long long)a.bytes,
                    a.compressed);
    return 0;
}

int cmdVerify(DbpfPackage& pkg) {
    size_t ok = 0, failed = 0;
    for (const auto& e : pkg.entries()) {
        try {
            pkg.read(e);
            ++ok;
        } catch (const std::exception& ex) {
            ++failed;
            std::fprintf(stderr, "%s: %s\n", tgiString(e.tgi).c_str(), ex.what());
        }
    }
    std::printf("%s: %zu ok, %zu failed\n", pkg.path().string().c_str(), ok, failed);
    return failed ? 1 : 0;
}

// Rewrite a package. Default keeps the stored bytes; "recompress" reruns our
// QFS encoder on the resources that were compressed, "decompress" stores all raw.
int cmdRepack(DbpfPackage& pkg, const std::filesystem::path& out, const std::string& mode) {
    if (!mode.empty() && mode != "--recompress" && mode != "--decompress")
        throw std::runtime_error("unknown repack option '" + mode + "'");
    DbpfWriter w;
    for (const auto& e : pkg.entries()) {
        if (e.tgi.type == type::DIR)
            continue;
        if (mode.empty())
            w.addStored(e.tgi, pkg.readRaw(e), e.rawSize);
        else
            w.add(e.tgi, pkg.read(e), mode == "--recompress" && e.compressed());
    }
    pkg.closeFile();
    w.save(out);
    DbpfPackage written(out);
    size_t compressed = 0;
    for (const auto& e : written.entries())
        compressed += e.compressed();
    std::printf("%s: %zu resources (%zu compressed), %llu bytes\n", out.string().c_str(), w.size(), compressed,
                (unsigned long long)std::filesystem::file_size(out));
    return 0;
}

// Same TGIs with the same decompressed bytes (DIR excluded, order ignored).
int cmdCompare(DbpfPackage& a, DbpfPackage& b) {
    size_t same = 0, differ = 0, missing = 0;
    auto count = [](DbpfPackage& p) {
        size_t n = 0;
        for (const auto& e : p.entries())
            n += e.tgi.type != type::DIR;
        return n;
    };
    for (const auto& e : a.entries()) {
        if (e.tgi.type == type::DIR)
            continue;
        const auto* o = b.find(e.tgi);
        if (!o) {
            ++missing;
            std::fprintf(stderr, "%s: missing\n", tgiString(e.tgi).c_str());
        } else if (a.read(e) != b.read(*o)) {
            ++differ;
            std::fprintf(stderr, "%s: differs\n", tgiString(e.tgi).c_str());
        } else {
            ++same;
        }
    }
    const size_t extra = count(b) > same + differ ? count(b) - same - differ : 0;
    std::printf("%zu same, %zu differ, %zu missing, %zu extra\n", same, differ, missing, extra);
    return differ || missing || extra ? 1 : 0;
}

Tgi parseTgi(const std::string& s) {
    Tgi k;
    if (std::sscanf(s.c_str(), "%x-%x-%x-%x", &k.type, &k.group, &k.instanceHi, &k.instance) != 4)
        throw std::runtime_error("bad TGI '" + s + "' (expected TTTTTTTT-GGGGGGGG-HHHHHHHH-IIIIIIII)");
    return k;
}

int cmdDump(DbpfPackage& pkg, const Tgi& tgi) {
    const auto* e = pkg.find(tgi);
    if (!e)
        throw std::runtime_error("resource not found");
    std::fputs(describeResource(tgi.type, pkg.read(*e)).c_str(), stdout);
    return 0;
}

int cmdDecodeAll(DbpfPackage& pkg) {
    std::map<std::string, std::pair<size_t, size_t>> byType; // ok, failed
    for (const auto& e : pkg.entries()) {
        if (!hasDecoder(e.tgi.type))
            continue;
        auto& acc = byType[typeLabel(e.tgi.type)];
        try {
            describeResource(e.tgi.type, pkg.read(e));
            ++acc.first;
        } catch (const std::exception& ex) {
            ++acc.second;
            std::fprintf(stderr, "%s %s: %s\n", typeLabel(e.tgi.type).c_str(), tgiString(e.tgi).c_str(), ex.what());
        }
    }
    size_t failed = 0;
    for (const auto& [t, acc] : byType) {
        std::printf("%-10s %8zu ok %6zu failed  %s\n", t.c_str(), acc.first, acc.second,
                    pkg.path().filename().string().c_str());
        failed += acc.second;
    }
    return failed ? 1 : 0;
}

// The TSData/Res directory a package lives under, used to resolve cross-package references.
std::filesystem::path resRootOf(const std::filesystem::path& pkg) {
    for (auto p = std::filesystem::absolute(pkg).parent_path(); p.has_relative_path(); p = p.parent_path())
        if (p.filename() == "Res")
            return p;
    return {};
}

int cmdTexture(DbpfPackage& pkg, const Tgi& tgi, const std::filesystem::path& out) {
    const auto* e = pkg.find(tgi);
    if (!e)
        throw std::runtime_error("resource not found");
    const auto data = pkg.read(*e);
    std::unique_ptr<ResourceIndex> index;
    RgbaImage img;
    try {
        img = loadTexture(data, nullptr);
    } catch (const FormatError&) {
        const auto root = resRootOf(pkg.path());
        if (root.empty())
            throw;
        index = std::make_unique<ResourceIndex>(root);
        img = loadTexture(data, index.get());
    }
    writePng(out, img);
    std::printf("%ux%u -> %s\n", img.width, img.height, out.string().c_str());
    return 0;
}

int cmdMesh(DbpfPackage& pkg, const Tgi& tgi, const std::filesystem::path& out) {
    const auto* e = pkg.find(tgi);
    if (!e)
        throw std::runtime_error("resource not found");
    const auto data = pkg.read(*e);
    Rcol rc(data);
    const auto* block = rc.find("cGeometryDataContainer");
    if (!block)
        throw std::runtime_error("not a GMDC");
    const auto meshes = buildMeshes(GeometryData::parse(block->body));
    std::ofstream f(out);
    if (!f)
        throw std::runtime_error("cannot write " + out.string());
    ObjWriter obj{f};
    size_t tris = 0;
    for (const auto& m : meshes) {
        obj.write(m, "");
        tris += m.triangles.size() / 3;
    }
    std::printf("%zu groups, %zu vertices, %zu triangles -> %s\n", meshes.size(), obj.vBase, tris,
                out.string().c_str());
    return 0;
}

int cmdObject(const std::filesystem::path& root, const std::string& query, const std::filesystem::path& outDir) {
    ResourceIndex index(root);
    std::string match;
    const auto cres = resolveObjectCres(index, query, &match);
    std::printf("%s\n", match.c_str());
    const auto model = loadObjectModel(index, cres);
    std::filesystem::create_directories(outDir);
    const std::string base = model.name.substr(0, model.name.size() - 5); // without "_cres"

    // Materials and their base textures.
    std::ofstream mtl(outDir / (base + ".mtl"));
    size_t textures = 0;
    for (const auto& [mname, m] : model.materials) {
        auto diffuse = m.property("stdMatDiffCoef");
        for (auto& c : diffuse)
            if (c == ',')
                c = ' ';
        mtl << "newmtl " << mname << "\nKd " << (diffuse.empty() ? "1 1 1" : diffuse) << "\n";
        const auto blend = m.property("stdMatAlphaBlendMode");
        const bool alpha = (!blend.empty() && blend != "none") || m.property("stdMatAlphaTestEnabled") == "true";
        if (alpha && !m.property("stdMatAlphaMultiplier").empty())
            mtl << "d " << m.property("stdMatAlphaMultiplier") << "\n";
        const auto tex = baseTextureName(m);
        if (tex.empty())
            continue;
        auto loc = index.findByName(tex, 0x1C4A276C);
        if (!loc) {
            std::fprintf(stderr, "warning: TXTR not found: %s\n", tex.c_str());
            continue;
        }
        const auto png = tex + ".png";
        writePng(outDir / png, loadTexture(index.read(*loc), &index));
        ++textures;
        mtl << "map_Kd " << png << "\n";
        if (alpha)
            mtl << "map_d " << png << "\n";
    }

    std::ofstream obj(outDir / (base + ".obj"));
    obj << "# " << model.name << " exported by s2pkg\nmtllib " << base << ".mtl\n";
    ObjWriter writer{obj};
    size_t tris = 0;
    for (const auto& part : model.parts) {
        writer.write(part.mesh, part.material);
        tris += part.mesh.triangles.size() / 3;
    }
    for (const auto& w : model.warnings)
        std::fprintf(stderr, "warning: %s\n", w.c_str());
    std::printf("%s: %zu parts, %zu vertices, %zu triangles, %zu materials, %zu textures -> %s\n", model.name.c_str(),
                model.parts.size(), writer.vBase, tris, model.materials.size(), textures,
                (outDir / (base + ".obj")).string().c_str());
    return 0;
}

int cmdFind(const std::string& root, const std::string& name, const std::string& type) {
    ResourceIndex idx(root);
    std::printf("%zu packages, %zu resources, %zu duplicate TGIs\n", idx.packageCount(), idx.resourceCount(),
                idx.duplicateCount());
    const uint32_t t = uint32_t(std::stoul(type, nullptr, 16));
    std::printf("hash: instance=0x%08X instanceHi=0x%08X\n", nameInstanceHash(name), nameInstanceHiHash(name));
    auto loc = idx.findByName(name, t);
    if (!loc) {
        std::printf("not found\n");
        return 1;
    }
    std::printf("%s in %s\n", tgiString(loc->entry->tgi).c_str(), loc->package->path().string().c_str());
    return 0;
}

int cmdExtract(DbpfPackage& pkg, const std::filesystem::path& outDir) {
    std::filesystem::create_directories(outDir);
    for (const auto& e : pkg.entries()) {
        auto data = pkg.read(e);
        auto name = typeLabel(e.tgi.type) + "_" + tgiString(e.tgi) + ".bin";
        std::ofstream(outDir / name, std::ios::binary).write(reinterpret_cast<const char*>(data.data()),
                                                             std::streamsize(data.size()));
    }
    std::printf("extracted %zu resources to %s\n", pkg.entries().size(), outDir.string().c_str());
    return 0;
}

// Builds the terrain/floor/wall model of each lot and reports what it holds
// and what could not be resolved (patterns, TXMTs, textures).
int cmdLot(const std::filesystem::path& root, const std::vector<std::filesystem::path>& lots) {
    ResourceIndex index(root);
    const PatternCatalog patterns(index, root);
    std::printf("%zu catalog patterns\n", patterns.catalogSize());
    int failed = 0;
    for (const auto& path : lots) {
        try {
            DbpfPackage pkg(path);
            const Lot lot = Lot::load(pkg);
            const ObjectModel model = buildLotModel(lot, index, patterns);
            size_t tris = 0, missingTextures = 0;
            for (const auto& part : model.parts)
                tris += part.mesh.triangles.size() / 3;
            for (const auto& [name, m] : model.materials) {
                const auto tex = baseTextureName(m);
                if (!tex.empty() && !index.findByName(tex, 0x1C4A276C)) {
                    std::printf("  missing texture %s (%s)\n", tex.c_str(), name.c_str());
                    ++missingTextures;
                }
            }
            std::printf("%s: %ux%u, levels %d..%d, %zu walls, %zu floor / %zu wall patterns, %zu paints, "
                        "%zu parts, %zu triangles, %zu materials\n",
                        path.filename().string().c_str(), lot.width, lot.height, lot.minLevel,
                        lot.minLevel + lot.levelCount - 1, lot.walls.size(), lot.floorPatterns.size(),
                        lot.wallPatterns.size(), lot.paints.size(), model.parts.size(), tris, model.materials.size());
            size_t placed = 0, people = 0;
            for (const auto& o : lot.objects) {
                people += o.person;
                placed += !o.person && !o.model.empty() && o.model != "nullModel" && o.model != "noModel" &&
                          (o.position[0] != 0 || o.position[1] != 0);
            }
            // (A masked face is a part of its own; a masked wall top too.)
            auto masked = [&](const char* node) {
                return size_t(std::count_if(model.parts.begin(), model.parts.end(), [&](const ObjectModel::Part& p) {
                    return p.mask >= 0 && p.node.rfind(node, 0) == 0;
                }));
            };
            std::printf("  %zu objects: %zu with a model in the world, %zu Sims; %zu wall faces and %zu wall tops with a door or window mask\n",
                        lot.objects.size(), placed, people, masked("wall:"), masked("walltop:"));
            // OS2_LOT_OBJECTS=<part of a name>: those objects, where they stand and their heading.
            if (const char* filter = std::getenv("OS2_LOT_OBJECTS"))
                for (const auto& o : lot.objects)
                    if (o.definition.find(filter) != std::string::npos)
                        std::printf("    %u %s at (%.4g, %.4g, %.4g) yaw %.0f\n", o.instance, o.definition.c_str(),
                                    o.position[0], o.position[1], o.position[2],
                                    2 * std::atan2(o.rotation[2], o.rotation[3]) * 180 / 3.14159265);
            // OS2_LOT_NODES=1 with it: the bones the lot saved for each (cObject's transform nodes).
            if (const char* filter = std::getenv("OS2_LOT_OBJECTS"); filter && std::getenv("OS2_LOT_NODES"))
                for (const auto& o : lot.objects)
                    if (o.definition.find(filter) != std::string::npos)
                        for (const auto& [name, x] : o.nodes)
                            std::printf("      %u %s t (%.4g, %.4g, %.4g) q (%.4g, %.4g, %.4g, %.4g)\n", o.instance,
                                        name.c_str(), x.t[0], x.t[1], x.t[2], x.q.x, x.q.y, x.q.z, x.q.w);
            size_t wet = 0;
            for (uint32_t x = 0; x <= lot.width; ++x)
                for (uint32_t y = 0; y <= lot.height; ++y)
                    wet += lot.waterLevel(int(x), int(y)) > lot.elevation(0, int(x), int(y));
            std::printf("  %zu roofs, %zu fence posts, %zu pools, %zu grid points under water\n", lot.roofs.size(),
                        lot.fencePosts.size(), lot.pools.size(), wet);
            for (const auto& r : lot.roofs)
                std::printf("    roof %u: type %u (%g, %g)-(%g, %g) level %d, angle %g, pattern 0x%08X, style %u\n", r.id, r.type,
                            r.x0, r.y0, r.x1, r.y1, r.level, r.angle, r.pattern, r.style);
            for (const auto& p : lot.pools)
                std::printf("    pool %u: (%g, %g) %ux%u level %d, depth %g\n", p.id, p.x, p.y, p.width, p.height,
                            p.level, p.depth);
            for (const auto& w : lot.warnings)
                std::printf("  %s\n", w.c_str());
            for (const auto& w : model.warnings)
                std::printf("  %s\n", w.c_str());
        } catch (const EmptyLotPackage& e) {
            // The empty lot package (the 96-byte stub): the game loads it as a
            // lot with nothing in it, so it is not an error.
            std::printf("%s: %s\n", path.filename().string().c_str(), e.what());
        } catch (const std::exception& e) {
            std::printf("%s: error: %s\n", path.filename().string().c_str(), e.what());
            ++failed;
        }
    }
    return failed ? 1 : 0;
}

// The walls of a lot inside a rectangle (x0,y0,x1,y1, all if none): ends,
// level, type, patterns and the floor heights at their ends.
int cmdLotWalls(const std::filesystem::path& lotPath, const std::string& box) {
    DbpfPackage pkg(lotPath);
    const Lot lot = Lot::load(pkg);
    float b[4] = {-1e9f, -1e9f, 1e9f, 1e9f};
    if (!box.empty() && std::sscanf(box.c_str(), "%f,%f,%f,%f", &b[0], &b[1], &b[2], &b[3]) != 4)
        throw std::runtime_error("lot-walls: expected x0,y0,x1,y1");
    std::printf("levels %d..%d, %zu walls (%zu without a layer entry), %zu stale layer entries, graphs:",
                lot.minLevel, lot.minLevel + lot.levelCount - 1, lot.walls.size(),
                size_t(std::count_if(lot.walls.begin(), lot.walls.end(), [](const Lot::Wall& w) { return !w.layered; })),
                lot.staleWallLayer.size());
    for (const auto& g : lot.wallGraphs)
        std::printf(" 0x%X (%zu lines)", g.instance, g.lines.size());
    std::printf("\n");
    for (const auto& w : lot.walls) {
        if (std::max(w.a.x, w.b.x) < b[0] || std::min(w.a.x, w.b.x) > b[2] || std::max(w.a.y, w.b.y) < b[1] ||
            std::min(w.a.y, w.b.y) > b[3])
            continue;
        std::printf("wall %u: (%g, %g)-(%g, %g) level %d/%d type 0x%X patterns %04X/%04X z %g..%g%s\n", w.id, w.a.x,
                    w.a.y, w.b.x, w.b.y, w.a.level, w.b.level, w.type, w.patterns[0], w.patterns[1],
                    lot.elevationAt(w.a.level, w.a.x, w.a.y), lot.elevationAt(w.a.level + 1, w.a.x, w.a.y),
                    w.layered ? "" : " (no layer entry)");
    }
    for (const auto& o : lot.objects)
        if (!o.person && o.position[0] >= b[0] && o.position[0] <= b[2] && o.position[1] >= b[1] && o.position[1] <= b[3])
            std::printf("object %s (%s) at (%g, %g, %g)\n", o.definition.c_str(), o.model.c_str(), o.position[0],
                        o.position[1], o.position[2]);
    return 0;
}

// --- SimAntics VM ------------------------------------------------------------

// What stand-in objects are built from: an object group whose GLOB names each
// semi-global group, an OBJD (the lowest instance) of each object group, and
// a Sim. Interaction trees (TTAB actions and guards) and the "CT - " trees
// that Run Tree by Name calls on the stack object run in the Sim's thread with
// the object as the stack object: in them the object's attributes are read
// through the stack object (2,116 times in private "Interaction - " trees,
// against 133 through "my") and person data through "my" (551 against 172).
struct StandIns {
    std::map<uint32_t, uint32_t> semiUsers;
    std::map<uint32_t, uint16_t> definitions;
    std::pair<uint32_t, uint16_t> person{}; // OBJD of a Sim (the first outside a neighbourhood)
    std::set<std::pair<uint32_t, uint16_t>> interactions; // (group, BHAV) named by a TTAB
    // The neighborhood the stand-ins live in (sweepNeighborhood): the Sim is
    // the first member of the first family with a house, a Sim "me" the
    // second, and the VM is on that family's lot.
    std::optional<hood::Neighborhood> hood;
    int16_t simNeighbor = 1, meNeighbor = 2, lot = 0;
    // Its character files: the Sims stand in as their own characters (with
    // the person semi-global's semi-attributes, their attributes and slots).
    std::filesystem::path charactersDir;
};

// The neighborhood for bhav/bhav-sweep: OS2_HOOD (a neighborhood folder, or
// "none"), else UserData/Neighborhoods/N001 under the Res directory.
void sweepNeighborhood(const std::filesystem::path& root, StandIns& s) {
    const char* env = std::getenv("OS2_HOOD");
    if (env && std::string(env) == "none")
        return;
    const std::filesystem::path dir = env ? std::filesystem::path(env) : root / "UserData" / "Neighborhoods" / "N001";
    if (!std::filesystem::exists(dir))
        return;
    s.hood = hood::Neighborhood::load(dir, true);
    s.charactersDir = dir / "Characters";
    for (const auto& f : s.hood->families)
        if (f.record.lot && f.members.size() >= 2) {
            auto members = f.members;
            std::sort(members.begin(), members.end());
            s.simNeighbor = int16_t(members[0]), s.meNeighbor = int16_t(members[1]), s.lot = int16_t(f.record.lot);
            break;
        }
}

// A VM for running stand-in trees: in the neighborhood, on its lot.
void prepareStandInVm(vm::VM& machine, const StandIns& s) {
    if (!s.hood)
        return;
    vm::loadNeighborhood(machine, *s.hood);
    vm::enterLot(machine, s.lot, s.hood->info ? int16_t(s.hood->info->id) : int16_t(0));
}

// A stand-in Sim is the neighbor `id`: its person data, and the stand-in's GUID.
void makeNeighbor(vm::VM& machine, vm::Object& o, int16_t id) {
    if (const vm::Neighbor* n = machine.neighbor(id))
        o.person->data = n->personData;
    o.person->data[vm::persondata::NeighborId] = id;
    machine.neighbors[id].guid = o.guid();
}

// The neighbor `id` as its own character (the OBJD of its character file,
// found through the code library's GUID fallback), null without one.
vm::Object* characterStandIn(vm::VM& machine, int16_t id) {
    const vm::Neighbor* n = machine.neighbor(id);
    const auto def = n && n->guid ? machine.code().characterDefinition(n->guid) : std::nullopt;
    if (!def)
        return nullptr;
    vm::Object& o = machine.instantiate(def->first, def->second);
    if (!o.person)
        o.person.emplace();
    makeNeighbor(machine, o, id);
    return &o;
}

StandIns standIns(ResourceIndex& index) {
    StandIns s;
    std::map<uint32_t, uint32_t> semiOf;
    for (const auto& loc : index.ofType(vm::kTypeGlob)) {
        const uint32_t semi = vm::semiGlobalGroup(SemiGlobal::parse(index.read(loc)).semiGlobalName);
        s.semiUsers.emplace(semi, loc.entry->tgi.group);
        semiOf.emplace(loc.entry->tgi.group, semi);
    }
    for (const auto& loc : index.ofType(vm::kTypeTtab)) {
        const uint32_t group = loc.entry->tgi.group;
        InteractionTable table;
        try {
            table = InteractionTable::parse(index.read(loc));
        } catch (const std::exception&) {
            continue; // a few TTABs carry bytes the parser does not explain
        }
        for (const auto& i : table.interactions)
            for (uint16_t tree : {i.action, i.guard}) {
                if (tree >= 0x2000 && semiOf.count(group))
                    s.interactions.emplace(semiOf[group], tree);
                else if (tree >= 0x1000 && tree < 0x2000)
                    s.interactions.emplace(group, tree);
                else if (tree >= 0x100 && tree < 0x1000)
                    s.interactions.emplace(vm::kGlobalGroup, tree);
            }
    }
    for (const auto& loc : index.ofType(vm::kTypeObjd)) {
        const auto& tgi = loc.entry->tgi;
        auto [it, added] = s.definitions.emplace(tgi.group, uint16_t(tgi.instance));
        if (!added)
            it->second = std::min(it->second, uint16_t(tgi.instance));
        if (!s.person.first && tgi.group != 0xFFFFFFFF &&
            ObjectDefinition::parse(index.read(loc)).objectType() == vm::kObjectTypePerson)
            s.person = {tgi.group, uint16_t(tgi.instance)};
    }
    return s;
}

// The semi-global group of the Sims' characters (PersonGlobals), 0 if unknown.
uint32_t personSemiGlobal(vm::VM& machine, const StandIns& s) {
    const vm::Neighbor* n = machine.neighbor(s.meNeighbor);
    const auto def = n && n->guid ? machine.code().characterDefinition(n->guid) : std::nullopt;
    return def ? machine.code().semiGlobalFor(def->first) : 0;
}

// Stand-in "me" for running a tree of `group`: an instance of the object, or
// for a semi-global group of one of the objects that use it. Trees of any group
// may act on a Sim, so the stand-in also carries person data.
vm::Object& standInObject(vm::VM& machine, uint32_t group, const StandIns& s) {
    // Global trees and the person semi-global's run on Sims: the "me"
    // neighbor's character.
    if (group == vm::kGlobalGroup || group == personSemiGlobal(machine, s))
        if (vm::Object* me = characterStandIn(machine, s.meNeighbor))
            return *me;
    auto user = s.semiUsers.find(group);
    const uint32_t objGroup = user != s.semiUsers.end() ? user->second : group;
    auto def = s.definitions.find(objGroup);
    vm::Object& me =
        def != s.definitions.end() ? machine.instantiate(objGroup, def->second) : machine.createObject(objGroup);
    if (user != s.semiUsers.end())
        me.semiGroup = group;
    if (me.person) {
        // A Sim on the lot is always a neighbor too.
        makeNeighbor(machine, me, s.meNeighbor);
    } else {
        me.person.emplace();
    }
    return me;
}

bool isInteraction(const StandIns& s, uint32_t group, uint16_t instance, const Behavior& b) {
    return s.interactions.count({group, instance}) || b.fileName.rfind("CT - ", 0) == 0;
}

// The stand-in Sim is also a neighbor (ID 1), so the neighbor searches and
// data owners have someone to find.
vm::Object& standInPerson(vm::VM& machine, const StandIns& s) {
    if (vm::Object* o = characterStandIn(machine, s.simNeighbor))
        return *o;
    vm::Object* o = s.person.first ? &machine.instantiate(s.person.first, s.person.second) : nullptr;
    if (!o) {
        o = &machine.createObject(0);
        o->person.emplace();
    }
    makeNeighbor(machine, *o, s.simNeighbor);
    return *o;
}

// Starts a tree of `object` with a stand-in Sim: in the Sim's thread with the
// object as the stack object for interactions, in the object's otherwise.
// They stand in an empty 32x32 world, the object (every part of it) on tile
// (10, 10) facing -X and the Sim two tiles in front of it, so the
// primitives that place and move things have somewhere to work.
vm::Thread startStandIn(vm::VM& machine, const StandIns& s, vm::Object& object, uint32_t group, uint16_t instance,
                        std::span<const int16_t> args, const Behavior& tree) {
    const vm::ObjectId sim = standInPerson(machine, s).id;
    machine.world = vm::World(32, 32);
    for (const auto& [id, o] : machine.objects()) {
        vm::Object& x = *machine.object(id);
        x.position = id == sim ? vm::Position{8.5f, 10.5f, 1} : vm::Position{10.5f, 10.5f, 1};
    }
    const bool interaction = isInteraction(s, group, instance, tree);
    // A parameter the tree makes its stack object ("stack object ID :=
    // param i", 1,500 of the sweep's "stack object 0 does not exist") gets
    // the other stand-in, as the game's callers pass an object there.
    std::vector<int16_t> given(args.begin(), args.end());
    if (given.empty() && tree.argCount) {
        given.assign(tree.argCount, 0);
        for (const auto& in : tree.instructions)
            if (in.opcode == 0x02 && in.operands[5] == 5 && in.operands[6] == 0x0A && in.operands[7] == 0x09) {
                const uint16_t i = uint16_t(in.operands[2] | in.operands[3] << 8);
                if (i < given.size())
                    given[i] = interaction ? object.id : sim;
            }
    }
    if (interaction)
        return machine.start(sim, instance, given, object.id, object.id);
    return machine.start(object.id, instance, given, sim);
}

const char* statusName(const vm::Thread& t) {
    switch (t.status) {
    case vm::Thread::Status::Running: return "yielded";
    case vm::Thread::Status::Finished: return t.result ? "returned true" : "returned false";
    case vm::Thread::Status::Failed: return "failed";
    case vm::Thread::Status::Killed: return "killed";
    }
    return "?";
}

// Runs one tree with zeroed state and prints every instruction it executes.
int cmdBhav(const std::filesystem::path& root, uint32_t group, uint16_t instance, const std::vector<int16_t>& args) {
    ResourceIndex index(root);
    vm::CodeLibrary code(&index);
    vm::VM machine(code);
    auto users = standIns(index);
    sweepNeighborhood(root, users);
    vm::useCharacterFiles(code, users.charactersDir);
    prepareStandInVm(machine, users);
    vm::Object& object = standInObject(machine, group, users);
    machine.trace = [](const vm::Thread& t, const vm::Frame& f, const Behavior::Instruction& in) {
        std::string ops;
        char buf[8];
        for (auto b : in.operands)
            std::snprintf(buf, sizeof buf, "%02X", b), ops += buf;
        const std::string op = in.opcode < 0x100 ? primitiveName(in.opcode) : "call " + hexString(in.opcode);
        std::printf("%*s0x%04X #%-3u %-30s [%s]\n", int(t.stack.size() - 1) * 2, "", f.instance, f.pc, op.c_str(),
                    ops.c_str());
    };
    auto [treeGroup, tree] = machine.resolveTree(object, instance);
    if (!tree)
        throw std::runtime_error("tree not found");
    // S2PKG_BHAV_LIST=1: the tree's listing instead of running it.
    if (std::getenv("S2PKG_BHAV_LIST")) {
        std::printf("%s (group 0x%08X)\n%s", tree->fileName.c_str(), treeGroup, tree->disassemble().c_str());
        return 0;
    }
    vm::Thread t = startStandIn(machine, users, object, treeGroup, instance, args, *tree);
    if (t.status == vm::Thread::Status::Running && t.stack.back().group != group)
        std::fprintf(stderr, "warning: tree resolved in group 0x%08X\n", t.stack.back().group);
    machine.run(t);
    std::printf("%s after %zu instructions\n", statusName(t), t.instructions);
    if (t.status == vm::Thread::Status::Failed)
        std::printf("error: %s\n", t.error.c_str());
    std::printf("temps:");
    for (auto v : t.temps)
        std::printf(" %d", v);
    std::printf("\n");
    // A dialog the tree put up (its strings from the code owner's files).
    for (const auto& [who, d] : machine.dialogs) {
        std::printf("dialog type %d: title \"%s\" message \"%s\"", d.type, d.title.c_str(), d.message.c_str());
        for (const auto& b : d.buttons)
            std::printf(" [%s]", b.c_str());
        std::printf("\n");
    }
    return t.status == vm::Thread::Status::Failed ? 1 : 0;
}

// Runs every BHAV of the game from a zeroed state and tallies how far the
// interpreter gets: outcomes, failure reasons and the missing primitives and
// data owners that stopped it, most frequent first.
int cmdBhavSweep(const std::filesystem::path& root) {
    ResourceIndex index(root);
    vm::CodeLibrary code(&index);
    auto users = standIns(index);
    sweepNeighborhood(root, users);
    vm::useCharacterFiles(code, users.charactersDir);
    if (users.hood)
        std::printf("neighborhood %s: Sim = neighbor %d, me = neighbor %d, on lot %d\n", users.hood->prefix.c_str(),
                    users.simNeighbor, users.meNeighbor, users.lot);
    std::map<std::string, size_t> outcomes, reasons;
    std::map<uint16_t, size_t> missingPrimitives;
    std::map<uint8_t, size_t> missingOwners;
    size_t total = 0, instructions = 0, interactions = 0;
    const std::regex number("0x[0-9A-F]+|\\d+");
    for (const auto& loc : index.ofType(vm::kTypeBhav)) {
        const auto& tgi = loc.entry->tgi;
        if (tgi.instance < 0x100)
            continue; // not callable
        vm::VM machine(code);
        prepareStandInVm(machine, users);
        vm::Object& object = standInObject(machine, tgi.group, users);
        uint16_t instance = uint16_t(tgi.instance);
        // Private trees of a global/semi-global group are not reachable by call opcodes.
        auto [group, tree] = machine.resolveTree(object, instance);
        if (group != tgi.group)
            continue;
        vm::Thread t = startStandIn(machine, users, object, group, instance, {}, *tree);
        machine.run(t, s2pkgBudget());
        ++total;
        interactions += t.me != object.id;
        instructions += t.instructions;
        ++outcomes[statusName(t)];
        if (t.status == vm::Thread::Status::Failed) {
            std::string reason = t.error.substr(0, t.error.find('\n'));
            if (reason.find("not implemented") == std::string::npos)
                reason = std::regex_replace(reason, number, "N");
            ++reasons[reason];
            // S2PKG_SWEEP_REASON=text: the whole message and stack of the failures that contain it
            static const char* only = std::getenv("S2PKG_SWEEP_REASON");
            if (only && t.error.find(only) != std::string::npos)
                std::printf("[%08X/%04X args %u] %s\n", tgi.group, instance, unsigned(tree->argCount), t.error.c_str());
        }
        for (auto [op, n] : machine.missingPrimitives)
            missingPrimitives[op] += n;
        for (auto [owner, n] : machine.missingOwners)
            missingOwners[owner] += n;
    }
    auto byCount = [](const auto& m) {
        std::vector<std::pair<size_t, typename std::decay_t<decltype(m)>::key_type>> v;
        for (const auto& [k, n] : m)
            v.emplace_back(n, k);
        std::sort(v.rbegin(), v.rend());
        return v;
    };
    std::printf("%zu trees (%zu run as interactions, in a Sim's thread), %zu instructions executed\n", total,
                interactions, instructions);
    for (const auto& [n, k] : byCount(outcomes))
        std::printf("  %6zu %s\n", n, k.c_str());
    std::printf("failure reasons:\n");
    for (const auto& [n, k] : byCount(reasons))
        std::printf("  %6zu %s\n", n, k.c_str());
    std::printf("missing primitives:\n");
    for (const auto& [n, op] : byCount(missingPrimitives))
        std::printf("  %6zu 0x%02X %s\n", n, op, primitiveName(op).c_str());
    std::printf("missing data owners:\n");
    for (const auto& [n, owner] : byCount(missingOwners))
        std::printf("  %6zu 0x%02X\n", n, owner);
    return 0;
}

// Failure reason of a thread without the numbers that vary (IDs, counts),
// unless it names a missing primitive or data owner.
std::string failureReason(const vm::Thread& t) {
    static const std::regex number("0x[0-9A-F]+|\\d+");
    std::string reason = t.error.substr(0, t.error.find('\n'));
    if (reason.find("not implemented") == std::string::npos)
        reason = std::regex_replace(reason, number, "N");
    return reason;
}

template <class M>
std::vector<std::pair<size_t, typename M::key_type>> byCount(const M& m) {
    std::vector<std::pair<size_t, typename M::key_type>> v;
    for (const auto& [k, n] : m)
        v.emplace_back(n, k);
    std::sort(v.rbegin(), v.rend());
    return v;
}

// Loads the objects of each lot into a VM (no rendering), runs every
// object's Init, then ticks the Main threads, and reports how far they got.
int cmdLotRun(const std::vector<std::filesystem::path>& lots, long ticks, bool list, int traceId, bool fromInit) {
    const auto root = resRootOf(lots.front());
    if (root.empty())
        throw std::runtime_error("the lot is not under a TSData/Res directory");
    ResourceIndex index(root);
    vm::CodeLibrary code(&index);
    constexpr size_t kTop = 15;
    int failed = 0;
    // Each lot's neighborhood (OS2_HOOD=none leaves them out), loaded once.
    const char* hoodEnv = std::getenv("OS2_HOOD");
    const bool useHoods = !(hoodEnv && std::string(hoodEnv) == "none");
    std::map<std::filesystem::path, hood::Neighborhood> hoods;
    for (const auto& path : lots) {
        DbpfPackage pkg(path);
        Lot lot;
        try {
            lot = Lot::load(pkg);
        } catch (const EmptyLotPackage& e) {
            // The empty lot package (the 96-byte stub): not an error.
            std::printf("%s: %s\n", path.filename().string().c_str(), e.what());
            continue;
        } catch (const std::exception& e) {
            std::printf("%s: error: %s\n", path.filename().string().c_str(), e.what());
            ++failed;
            continue;
        }
        const LotObjects saved = LotObjects::load(pkg);
        vm::VM machine(code);
        machine.initCreatedObjects = true;
        machine.instructionsPerTurn = s2pkgBudget();
        machine.setClockGlobals();
        size_t traced = 0;
        if (traceId)
            machine.trace = [traceId, &traced, &machine](const vm::Thread& t, const vm::Frame& f, const Behavior::Instruction& in) {
                // --trace -1: every thread, each line tagged with its object;
                // S2PKG_TRACE_HOUR=h: only during that game hour
                static const char* hourEnv = std::getenv("S2PKG_TRACE_HOUR");
                if (hourEnv && machine.globals[vm::global::Hour] != std::atoi(hourEnv))
                    return;
                if ((traceId != -1 && t.me != traceId) || ++traced > (traceId == -1 ? 4000000u : 400000u))
                    return;
                if (traceId == -1)
                    std::printf("[%d] ", t.me);
                const std::string op = in.opcode < 0x100 ? primitiveName(in.opcode) : "call " + hexString(in.opcode);
                std::string ops;
                char buf[8];
                for (auto b : in.operands)
                    std::snprintf(buf, sizeof buf, "%02X", b), ops += buf;
                std::printf("%*s0x%04X #%-3u %-30s %s [%s]", int(t.stack.size() - 1) * 2, "", f.instance, f.pc,
                            op.c_str(), f.behavior->fileName.c_str(), ops.c_str());
                // S2PKG_TRACE_STATE=1: also the hour, the temps 0-3 and the frame's locals
                static const bool state = std::getenv("S2PKG_TRACE_STATE") != nullptr;
                if (state) {
                    std::printf(" | %02d:%02d t %d %d %d %d l", machine.globals[vm::global::Hour],
                                machine.globals[vm::global::Minute], t.temps[0], t.temps[1], t.temps[2], t.temps[3]);
                    for (int16_t v : f.locals)
                        std::printf(" %d", v);
                    if (const vm::Object* me = machine.object(t.me))
                        std::printf(" @ %.2f,%.2f dir %d", me->position.x, me->position.y,
                                    me->data[vm::objdata::Direction]);
                }
                std::printf("\n");
            };
        const hood::Neighborhood* hood = nullptr;
        if (const auto hoodPackage = useHoods ? vm::neighborhoodPackageOf(path) : std::filesystem::path{};
            !hoodPackage.empty()) {
            auto it = hoods.find(hoodPackage);
            if (it == hoods.end())
                it = hoods.emplace(hoodPackage, hood::Neighborhood::load(hoodPackage, true)).first;
            hood = &it->second;
            vm::loadNeighborhood(machine, *hood);
            vm::enterLot(machine, vm::lotIdOf(path), hood->info ? int16_t(hood->info->id) : int16_t(0));
        }
        const vm::LotLoad load = vm::loadLot(machine, lot, saved, vm::charactersDirOf(path));
        const size_t boundSims = hood ? vm::bindLotSims(machine) : 0;

        std::map<std::string, size_t> unresolved, initOutcomes, mainOutcomes, reasons;
        std::map<vm::ObjectId, std::string> initOf;
        for (const auto& e : load.objects)
            if (!e.created)
                ++unresolved[std::regex_replace(e.error, std::regex("\"[^\"]*\"|0x[0-9A-F]+|N\\d+_User\\d+"), "X")];
        // By default the objects resume their saved state, as the game loads
        // a lot (lot_resume.hpp); --init starts them from their Inits.
        vm::applySavedSimulator(machine, saved);
        std::optional<vm::ResumeResult> resumed;
        if (!fromInit) {
            resumed = vm::resumeLotObjects(machine, load, saved);
            for (const auto& t : resumed->failedLoads)
                ++reasons["load: " + failureReason(t)];
        } else {
            vm::clearUseCounts(machine);
        }
        for (const auto& e : load.objects) {
            if (!fromInit || !e.created || !machine.object(e.id))
                continue;
            const auto t = machine.runFunction(e.id, 0);
            std::string outcome = !t ? "no init" : statusName(*t);
            if (t && t->status == vm::Thread::Status::Running)
                outcome = "still running";
            if (t && t->status == vm::Thread::Status::Failed)
                ++reasons["init: " + failureReason(*t)];
            ++initOutcomes[outcome];
            initOf[e.id] = t && t->status == vm::Thread::Status::Failed ? "init failed: " + failureReason(*t) : outcome;
        }
        size_t mains = 0;
        if (fromInit) {
            vm::applySavedMotives(machine, load, saved);
            for (const auto& e : load.objects)
                mains += e.created && machine.startMain(e.id);
        } else {
            mains = resumed->stacks + resumed->freshMains;
        }
        // The environment's light term from the lot's light maps, made
        // without a window as the app's (vm/lot_light.hpp; S2PKG_NO_LIGHT=1: left out).
        std::unique_ptr<vm::HeadlessLotLighting> lighting;
        if (!std::getenv("S2PKG_NO_LIGHT")) {
            lighting = std::make_unique<vm::HeadlessLotLighting>(index, lot, pkg, root, machine);
            lighting->install();
        }
        for (long i = 0; i < ticks; ++i)
            machine.tick();
        for (const auto& [id, t] : machine.threads) {
            ++mainOutcomes[t.status == vm::Thread::Status::Running ? "running (yielded)" : statusName(t)];
            if (t.status == vm::Thread::Status::Failed)
                ++reasons["main: " + failureReason(t)];
        }
        for (const auto& t : machine.createdInits)
            if (t.status == vm::Thread::Status::Failed)
                ++reasons["init of a created object: " + failureReason(t)];
        if (std::getenv("S2PKG_ERRORS")) // the whole message and stack of each failure
            for (const auto* list : {&machine.createdInits})
                for (const auto& t : *list)
                    if (t.status == vm::Thread::Status::Failed)
                        std::printf("  [init of %d] %s\n", t.me, t.error.c_str());
        if (std::getenv("S2PKG_ERRORS"))
            for (const auto& [id, t] : machine.threads)
                if (t.status == vm::Thread::Status::Failed)
                    std::printf("  [main of %d] %s\n", id, t.error.c_str());

        std::printf("%s: %zu saved objects, %zu in the VM (%zu Sims), %zu objects now; %ld ticks (%02d:%02d)\n",
                    path.filename().string().c_str(), load.objects.size(), load.created,
                    size_t(std::count_if(load.objects.begin(), load.objects.end(),
                                         [](const auto& e) { return e.created && e.person; })),
                    machine.objects().size(), ticks, machine.clock.hour, machine.clock.minute);
        std::printf("  world: %zu stairs, %zu door parts opening a wall (all cObjects: %zu stairs)\n", load.stairs, load.doors, vm::lotStairs(lot).size());
        if (hood)
            std::printf("  neighborhood %s: %zu neighbors; lot %d, family %d, funds %d; %zu Sims are neighbors\n",
                        hood->prefix.c_str(), machine.neighbors.size(), machine.globals[vm::global::CurrentHouse],
                        machine.globals[vm::global::CurrentFamily],
                        vm::familyFunds(machine, machine.globals[vm::global::CurrentFamily]), boundSims);
        std::map<std::string, size_t> warnings;
        for (const auto& w : saved.warnings)
            ++warnings[std::regex_replace(w, std::regex("state 0x[0-9A-F]+"), "state X")];
        for (const auto& [n, w] : byCount(warnings))
            std::printf("  warning: %zu x %s\n", n, w.c_str());
        if (!unresolved.empty()) {
            std::printf("  not created:\n");
            for (const auto& [n, k] : byCount(unresolved))
                std::printf("    %5zu %s\n", n, k.c_str());
        }
        if (resumed) {
            std::printf("  resumed: %zu states restored, %zu saved tree stacks, %zu fresh Mains, %zu loads, %zu queued interactions;",
                        resumed->restored, resumed->stacks, resumed->freshMains, resumed->loads, resumed->queued);
            for (const auto& [n, k] : byCount(resumed->problems))
                std::printf(" %zu %s,", n, k.c_str());
            std::printf("\n");
        } else {
            std::printf("  init:");
            for (const auto& [n, k] : byCount(initOutcomes))
                std::printf(" %zu %s,", n, k.c_str());
        }
        std::printf("\n  main threads (%zu started):", mains);
        for (const auto& [n, k] : byCount(mainOutcomes))
            std::printf(" %zu %s,", n, k.c_str());
        std::printf("\n  created while running: %zu (inits: %zu failed)\n", machine.createdInits.size(),
                    size_t(std::count_if(machine.createdInits.begin(), machine.createdInits.end(),
                                         [](const auto& t) { return t.status == vm::Thread::Status::Failed; })));
        std::printf("  failure reasons:\n");
        size_t shown = 0;
        for (const auto& [n, k] : byCount(reasons))
            if (shown++ < kTop)
                std::printf("    %5zu %s\n", n, k.c_str());
        std::printf("  missing primitives:");
        shown = 0;
        for (const auto& [n, op] : byCount(machine.missingPrimitives))
            if (shown++ < kTop)
                std::printf(" %s %zu,", primitiveName(op).c_str(), n);
        std::printf("\n  missing data owners:");
        for (const auto& [n, owner] : byCount(machine.missingOwners))
            std::printf(" 0x%02X %zu,", owner, n);
        std::printf("\n");
        if (list)
            for (const auto& e : load.objects) {
                std::printf("  %5d %-45s ", e.id, e.name.c_str());
                if (!e.cObject.empty())
                    std::printf("[leftover cObject \"%s\"] ", e.cObject.c_str());
                if (!e.created) {
                    std::printf("not created: %s\n", e.error.c_str());
                    continue;
                }
                const vm::Object* o = machine.object(e.id);
                if (o)
                    std::printf("(%.2f, %.2f, %d) dir %d in %d/%d; ", o->position.x, o->position.y, o->position.level,
                                o->data[vm::objdata::Direction], o->data[vm::objdata::ContainerId],
                                o->data[vm::objdata::SlotNumber]);
                if (o && o->route && !o->route->points.empty())
                    std::printf("walking to (%.2f, %.2f) for %d; ", o->route->points.back().x,
                                o->route->points.back().y, o->route->target);
                std::printf("init %s", initOf[e.id].c_str());
                if (auto it = machine.threads.find(e.id); it != machine.threads.end())
                    std::printf("; main %s%s", it->second.status == vm::Thread::Status::Running ? "running"
                                                                                               : statusName(it->second),
                                it->second.status == vm::Thread::Status::Failed
                                    ? (": " + failureReason(it->second)).c_str()
                                    : "");
                std::printf("\n");
            }
        if (list) { // the objects made while running (or saved ones replaced by a new object with their ID)
            std::map<int, std::string> saved;
            for (const auto& e : load.objects)
                saved[e.id] = e.name;
            for (const auto& [id, o] : machine.objects()) {
                const std::string name = o.definition ? o.definition->fileName : "?";
                if (auto it = saved.find(id); it != saved.end() && it->second == name)
                    continue;
                if (saved.count(id) && o.person)
                    continue; // Sims are listed by their character file
                std::printf("  %5d %-45s (%.2f, %.2f, %d) lead %d in %d/%d; new", id, name.c_str(), o.position.x,
                            o.position.y, o.position.level, o.data[vm::objdata::LeadTileObjectId],
                            o.data[vm::objdata::ContainerId], o.data[vm::objdata::SlotNumber]);
                if (auto it = machine.threads.find(id); it != machine.threads.end())
                    std::printf("; main %s", statusName(it->second));
                for (const auto& a : o.animation.overlays)
                    std::printf("; overlay %s", a.name.c_str());
                for (const auto& e : o.animation.overlayEvents)
                    std::printf("; events of %s: %zu left, %.0f of %.0f", e.name.c_str(), e.pending.size(), e.played,
                                e.length);
                std::printf("\n");
            }
        }
    }
    return failed ? 1 : 0;
}

int usage() {
    std::fprintf(stderr, "usage: s2pkg ls|stats|verify|decode-all <file.package>\n"
                         "       s2pkg dump <file.package> <TTTTTTTT-GGGGGGGG-HHHHHHHH-IIIIIIII>\n"
                         "       s2pkg extract <file.package> <out-dir>\n"
                         "       s2pkg repack <in.package> <out.package> [--recompress|--decompress]\n"
                         "       s2pkg compare <a.package> <b.package>\n"
                         "       s2pkg texture <file.package> <tgi> <out.png>\n"
                         "       s2pkg mesh <file.package> <tgi> <out.obj>\n"
                         "       s2pkg find <Res-dir> <name> <type-hex>\n"
                         "       s2pkg object <Res-dir> <cres-name|0xGUID|object-name> <out-dir>\n"
                         "       s2pkg lot <Res-dir> <lot.package>...\n"
                         "       s2pkg lot-walls <lot.package> [x0,y0,x1,y1]\n"
                         "       s2pkg bhav <Res-dir> <group-hex> <instance-hex> [arg...]\n"
                         "       s2pkg lua <file.objLua>\n"
                         "       s2pkg lua-script <Res-dir> <name>\n"
                         "       s2pkg lua-prelude <Res-dir> <name|*> [text]\n"
                         "       s2pkg bhav-sweep <Res-dir>\n"
                         "       s2pkg lot-run <lot.package>... [--ticks N] [--list] [--trace object-id] [--init]\n"
                         "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s", animUsage(), materialUsage(), audioUsage(), movieUsage(), uiUsage(), hoodUsage(), hoodPlayUsage(), primUsage(), pieUsage(), stateUsage(), saveUsage(), autonomyUsage(), wantsUsage(), routeUsage(), buyUsage(), buildUsage(), effectsUsage(), simLookUsage(), casUsage(), tutorialUsage(), shadowUsage());
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3)
        return usage();
    const std::string cmd = argv[1];
    try {
        if (int rc = animCommand(cmd, argc, argv); rc >= 0)
            return rc;
        if (int rc = materialCommand(cmd, argc, argv); rc >= 0)
            return rc;
        if (int rc = audioCommand(cmd, argc, argv); rc >= 0)
            return rc;
        if (int rc = movieCommand(cmd, argc, argv); rc >= 0)
            return rc;
        if (int rc = uiCommand(cmd, argc, argv); rc >= 0)
            return rc;
        if (int rc = hoodCommand(cmd, argc, argv); rc >= 0)
            return rc;
        if (int rc = hoodPlayCommand(cmd, argc, argv); rc >= 0)
            return rc;
        if (int rc = primCommand(cmd, argc, argv); rc >= 0)
            return rc;
        if (int rc = effectsCommand(cmd, argc, argv); rc >= 0)
            return rc;
        if (int rc = shadowCommand(cmd, argc, argv); rc >= 0)
            return rc;
        if (int rc = simLookCommand(cmd, argc, argv); rc >= 0)
            return rc;
        if (int rc = scriptCommand(cmd, argc, argv); rc >= 0)
            return rc;
        if (int rc = tutorialCommand(cmd, argc, argv); rc >= 0)
            return rc;
        if (int rc = pieCommand(cmd, argc, argv); rc >= 0)
            return rc;
        if (int rc = routeCommand(cmd, argc, argv); rc >= 0)
            return rc;
        if (int rc = autonomyCommand(cmd, argc, argv); rc >= 0)
            return rc;
        if (int rc = wantsCommand(cmd, argc, argv); rc >= 0)
            return rc;
        if (int rc = stateCommand(cmd, argc, argv); rc >= 0)
            return rc;
        if (int rc = saveCommand(cmd, argc, argv); rc >= 0)
            return rc;
        if (int rc = buyCommand(cmd, argc, argv); rc >= 0)
            return rc;
        if (int rc = buildCommand(cmd, argc, argv); rc >= 0)
            return rc;
        if (int rc = casCommand(cmd, argc, argv); rc >= 0)
            return rc;
        if (cmd == "find" && argc == 5) return cmdFind(argv[2], argv[3], argv[4]);
        if (cmd == "object" && argc == 5) return cmdObject(argv[2], argv[3], argv[4]);
        if (cmd == "lot" && argc >= 4) return cmdLot(argv[2], {argv + 3, argv + argc});
        if (cmd == "lot-walls" && argc >= 3) return cmdLotWalls(argv[2], argc > 3 ? argv[3] : "");
        if (cmd == "bhav-sweep" && argc == 3) return cmdBhavSweep(argv[2]);
        if (cmd == "lot-run") {
            std::vector<std::filesystem::path> lots;
            long ticks = 300;
            bool list = false;
            bool fromInit = false;
            int traceId = 0;
            for (int i = 2; i < argc; ++i) {
                const std::string a = argv[i];
                if (a == "--ticks" && i + 1 < argc)
                    ticks = std::stol(argv[++i]);
                else if (a == "--list")
                    list = true;
                else if (a == "--trace" && i + 1 < argc)
                    traceId = std::stoi(argv[++i]);
                else if (a == "--init")
                    fromInit = true;
                else
                    lots.emplace_back(a);
            }
            if (lots.empty())
                return usage();
            return cmdLotRun(lots, ticks, list, traceId, fromInit);
        }
        if (cmd == "bhav" && argc >= 5) {
            std::vector<int16_t> args;
            for (int i = 5; i < argc; ++i)
                args.push_back(int16_t(std::stoi(argv[i], nullptr, 0)));
            return cmdBhav(argv[2], uint32_t(std::stoul(argv[3], nullptr, 16)),
                           uint16_t(std::stoul(argv[4], nullptr, 16)), args);
        }
        DbpfPackage pkg(argv[2]);
        if (cmd == "ls") return cmdLs(pkg);
        if (cmd == "stats") return cmdStats(pkg);
        if (cmd == "verify") return cmdVerify(pkg);
        if (cmd == "decode-all") return cmdDecodeAll(pkg);
        if (cmd == "dump" && argc == 4) return cmdDump(pkg, parseTgi(argv[3]));
        if (cmd == "extract" && argc == 4) return cmdExtract(pkg, argv[3]);
        if (cmd == "repack" && (argc == 4 || argc == 5)) return cmdRepack(pkg, argv[3], argc == 5 ? argv[4] : "");
        if (cmd == "compare" && argc == 4) {
            DbpfPackage other(argv[3]);
            return cmdCompare(pkg, other);
        }
        if (cmd == "texture" && argc == 5) return cmdTexture(pkg, parseTgi(argv[3]), argv[4]);
        if (cmd == "mesh" && argc == 5) return cmdMesh(pkg, parseTgi(argv[3]), argv[4]);
        return usage();
    } catch (const EmptyLotPackage& ex) {
        // The empty lot package (the 96-byte stub): the game loads it as a lot
        // with nothing in it; the soak counts it apart from the errors.
        std::fprintf(stderr, "empty lot package: %s\n", ex.what());
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
