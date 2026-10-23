// s2pkg catalog: the Buy-mode catalog (formats/catalog.hpp).

#include "buy_cmds.hpp"
#include "build_cmds.hpp"
#include "budget.hpp"

#include "formats/build_catalog.hpp"
#include "formats/catalog.hpp"
#include "formats/image.hpp"
#include "formats/dbpf.hpp"
#include "formats/lot.hpp"
#include "formats/lot_objects.hpp"
#include "formats/resindex.hpp"
#include "vm/build_edit.hpp"
#include "vm/hood_bridge.hpp"
#include "vm/lot.hpp"
#include "vm/lot_neighborhood.hpp"
#include "vm/lot_resume.hpp"
#include "vm/placement.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace os2 {

namespace {

namespace fs = std::filesystem;

std::string bits(uint16_t v, const char* (*name)(int), int count) {
    std::string s;
    for (int i = 0; i < count; ++i)
        if (v & (1u << i))
            s += (s.empty() ? "" : ",") + std::string(name(i));
    return s.empty() ? "-" : s;
}

void printEntry(const CatalogEntry& e, Catalog& c, bool verbose) {
    std::printf("  %08X  §%-6d %-40s  sub %04X  rooms %s%s%s\n", e.guid, e.price, e.name.c_str(), e.functionSubSort,
                bits(e.roomSort, roomSortName, kRoomSortCount).c_str(), e.multiTile ? "  [multi-tile]" : "",
                c.thumbnail(e) ? "" : "  [no thumbnail]");
    if (!verbose)
        return;
    std::printf("      %s (group %08X OBJD %04X, CTSS %04X)\n", e.fileName.c_str(), e.group, e.instance,
                e.catalogStrings);
    std::printf("      %s\n", e.description.c_str());
    std::printf("      ratings hunger %d comfort %d hygiene %d bladder %d energy %d fun %d environment %d; skills "
                "%04X\n",
                e.ratings[0], e.ratings[1], e.ratings[2], e.ratings[3], e.ratings[4], e.ratings[5], e.ratings[6],
                e.skillFlags);
    std::printf("      depreciation initial %d daily %d limit %d%s; placement %04X wall %04X adjacent %04X height "
                "%04X\n",
                e.initialDepreciation, e.dailyDepreciation, e.depreciationLimit, e.selfDepreciating ? " self" : "",
                e.placementFlags, e.wallPlacementFlags, e.wallAdjacentFlags, e.allowedHeightFlags);
}

int cmdCatalog(const std::vector<std::string>& args) {
    fs::path res;
    int function = -1, room = -1, sub = -1;
    bool verbose = false, installation = false, build = false;
    std::string thumbKey, thumbOut;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--function" && i + 1 < args.size())
            function = std::stoi(args[++i]);
        else if (args[i] == "--room" && i + 1 < args.size())
            room = std::stoi(args[++i]);
        else if (args[i] == "--sub" && i + 1 < args.size())
            sub = std::stoi(args[++i]);
        else if (args[i] == "-v" || args[i] == "--verbose")
            verbose = true;
        else if (args[i] == "--installation")
            installation = true;
        else if (args[i] == "--build")
            build = true;
        else if (args[i] == "--thumb" && i + 2 < args.size())
            thumbKey = args[i + 1], thumbOut = args[i + 2], i += 2;
        else
            res = args[i];
    }
    if (res.empty())
        throw std::runtime_error("usage: " + std::string(buyUsage()));
    std::unique_ptr<ResourceIndex> owned = std::make_unique<ResourceIndex>(res);
    if (installation) {
        // What the app's index reads (src/app/win_main.cpp): the Res tree of
        // every pack of the installation, the base game's first.
        owned = std::make_unique<ResourceIndex>();
        for (const auto& root : installationResRoots(res))
            owned->addRoot(root);
    }
    ResourceIndex& index = *owned;
    Catalog c = Catalog::load(index, build);
    c.setThumbnailPackage(res / "UserData/Thumbnails/ObjectThumbnails.package");
    if (!thumbKey.empty()) {
        const CatalogEntry* e = c.find(thumbKey);
        if (!e)
            throw std::runtime_error("no catalog entry " + thumbKey);
        const RgbaImage* img = c.thumbnail(*e);
        if (!img)
            throw std::runtime_error("no thumbnail for " + e->name);
        writePng(thumbOut, *img);
        std::printf("%s: %ux%u -> %s\n", e->name.c_str(), img->width, img->height, thumbOut.c_str());
        return 0;
    }
    std::printf("catalog: %zu buyable objects, %zu with a thumbnail\n", c.entries().size(), c.thumbnailCount());
    if (build) {
        // The objects of Build mode (OBJD field 69 "build mode type" and field
        // 74 "build Mode Subsort"; docs/buy-build-mac.md §7.6).
        std::map<std::pair<uint16_t, uint16_t>, std::vector<const CatalogEntry*>> groups;
        for (const auto& e : c.entries())
            if (e.buildModeType)
                groups[{e.buildModeType, e.buildModeSubsort}].push_back(&e);
        for (const auto& [key, list] : groups) {
            std::printf("\nbuild type %u subsort %04X: %zu\n", key.first, key.second, list.size());
            for (size_t i = 0; i < list.size() && i < 6; ++i)
                std::printf("    %s\n", list[i]->name.c_str());
        }
        return 0;
    }
    if (function < 0 && room < 0) {
        for (int f = 0; f < kFunctionSortCount; ++f) {
            const auto list = c.byFunction(f);
            std::printf("\n%s (function bit %d): %zu\n", functionSortName(f), f, list.size());
            for (int s = 0; s < 16; ++s)
                if (const auto l = c.byFunction(f, s); !l.empty())
                    std::printf("  subsort bit %d: %zu (e.g. %s)\n", s, l.size(), l.front()->name.c_str());
            if (verbose)
                for (const auto* e : list)
                    printEntry(*e, c, false);
        }
        std::printf("\nBy room:\n");
        for (int r = 0; r < kRoomSortCount; ++r)
            std::printf("  %-14s %zu\n", roomSortName(r), c.byRoom(r).size());
        size_t none = 0;
        for (const auto& e : c.entries())
            none += (e.functionSort & ((1u << kFunctionSortCount) - 1)) == 0;
        std::printf("  with no function sort: %zu\n", none);
        return 0;
    }
    const auto list = function >= 0 ? c.byFunction(function, sub) : c.byRoom(room, sub);
    std::printf("%s %s%s: %zu\n", function >= 0 ? "function" : "room",
                function >= 0 ? functionSortName(function) : roomSortName(room),
                sub >= 0 ? (" / " + std::to_string(sub)).c_str() : "", list.size());
    for (const auto* e : list)
        printEntry(*e, c, verbose);
    return 0;
}

struct Where {
    int x = 0, y = 0, dir = 0, level = 1;
};

Where parseWhere(const std::string& s) {
    Where w;
    if (std::sscanf(s.c_str(), "%d,%d,%d,%d", &w.x, &w.y, &w.dir, &w.level) < 2)
        throw std::runtime_error("expected x,y[,dir[,level]], got \"" + s + "\"");
    return w;
}

std::pair<std::string, std::string> splitAt(const std::string& s) {
    const auto at = s.rfind('@');
    if (at == std::string::npos)
        throw std::runtime_error("expected <what>@x,y[,dir[,level]], got \"" + s + "\"");
    return {s.substr(0, at), s.substr(at + 1)};
}

fs::path resRootOf(const fs::path& lot) {
    for (auto p = fs::absolute(lot).parent_path(); p.has_relative_path(); p = p.parent_path())
        if (p.filename() == "Res")
            return p;
    throw std::runtime_error("the lot is not under a TSData/Res directory");
}

int cmdBuy(const std::vector<std::string>& args) {
    fs::path lotFile;
    std::vector<std::pair<std::string, std::string>> edits;
    for (size_t i = 0; i < args.size(); ++i) {
        if ((args[i] == "--buy" || args[i] == "--sell" || args[i] == "--move" || args[i] == "--check") &&
            i + 1 < args.size())
            edits.emplace_back(args[i], args[i + 1]), ++i;
        else
            lotFile = args[i];
    }
    if (lotFile.empty() || edits.empty())
        throw std::runtime_error("usage: " + std::string(buyUsage()));
    ResourceIndex index(resRootOf(lotFile));
    vm::CodeLibrary code(&index);
    DbpfPackage pkg(lotFile);
    const Lot lot = Lot::load(pkg);
    const LotObjects saved = LotObjects::load(pkg);
    vm::VM machine(code);
    machine.placementLot = &lot; // the placement tests' heights and floors
    machine.initCreatedObjects = true;
    machine.instructionsPerTurn = s2pkgBudget();
    machine.setClockGlobals();
    const bool hood = vm::loadLotNeighborhood(machine, lotFile);
    const vm::LotLoad load = vm::loadLot(machine, lot, saved, vm::charactersDirOf(lotFile));
    if (hood)
        vm::bindLotSims(machine);
    vm::startSavedLot(machine, load, saved);
    std::printf("%s: funds §%d\n", lotFile.filename().string().c_str(), vm::funds(machine));
    return applyBuyEdits(machine, index, edits) ? 0 : 1;
}

// Every object the lot keeps where it stands, tested where it stands
// (checkPlacement with footprints, no slot): without the lot's geometry and
// with it (VM::placementLot: TestPlacementCommon's IsFlat 0x12 and the floor
// of all four quarters 0x8E, docs/buy-build-mac.md §7.24). The game placed
// them, so what the geometry alone refuses is a test that does not match.
int cmdPlacementSweep(const std::vector<std::string>& args) {
    if (args.empty())
        throw std::runtime_error("usage: s2pkg placement-sweep <lot.package>...");
    std::map<int, int> before, after;
    int objects = 0, lots = 0, stairs = 0;
    for (const auto& a : args) {
        const fs::path lotFile = a;
        ResourceIndex index(resRootOf(lotFile));
        vm::CodeLibrary code(&index);
        DbpfPackage pkg(lotFile);
        Lot lot;
        try {
            lot = Lot::load(pkg);
        } catch (const std::exception& ex) {
            std::printf("%s: skipped (%s)\n", lotFile.filename().string().c_str(), ex.what());
            continue;
        }
        const LotObjects saved = LotObjects::load(pkg);
        vm::VM machine(code);
        machine.initCreatedObjects = true;
        machine.instructionsPerTurn = s2pkgBudget();
        machine.setClockGlobals();
        const bool hood = vm::loadLotNeighborhood(machine, lotFile);
        const vm::LotLoad load = vm::loadLot(machine, lot, saved, vm::charactersDirOf(lotFile));
        if (hood)
            vm::bindLotSims(machine);
        vm::startSavedLot(machine, load, saved); // the saved object data (placement flags, ...)
        ++lots;
        Lot edited = lot;
        vm::BuildEditor editor(machine, edited);
        std::vector<vm::ObjectId> leads;
        for (const auto& [id, o] : machine.eachObject())
            if (!o.person && o.definition && o.position.inWorld() && !o.data[vm::objdata::ContainerId] &&
                vm::multiTileGroup(o) == id)
                leads.push_back(id);
        for (vm::ObjectId id : leads) {
            const vm::Object& o = *machine.object(id);
            const vm::TilePos t = vm::VM::tileOf(o);
            machine.placementLot = nullptr;
            const vm::Placement p0 = vm::checkPlacement(machine, id, t, o.data[vm::objdata::Direction], false);
            machine.placementLot = &lot;
            const vm::Placement p1 = vm::checkPlacement(machine, id, t, o.data[vm::objdata::Direction], false);
            ++objects;
            ++before[p0.ok ? 0 : p0.error];
            ++after[p1.ok ? 0 : p1.error];
            static std::map<int, int> shown;
            static const char* why = std::getenv("S2PKG_SWEEP_WHY");
            if (why && !p0.ok && (std::string(why) == "all" || shown[p0.error]++ < 3))
                std::printf("  %s: %d \"%s\" od4 %04X od42 %04X od82 %04X at (%d,%d,%d) dir %d: 0x%02X %s\n",
                            lotFile.filename().string().c_str(), id, o.definition->fileName.c_str(),
                            uint16_t(o.data[vm::objdata::AllowedHeightFlags]), uint16_t(o.data[vm::objdata::PlacementFlags]),
                            uint16_t(o.data[vm::objdata::PlacementFlags2]), t.x, t.y, t.level, o.data[vm::objdata::Direction],
                            p0.error, p0.reason.c_str());
            // The staircases' CeilingTooHighForObject (0x30), which reads
            // the lot's support (the build editor's HasSupport).
            if (p1.ok && o.definition->objectType() == 10) {
                ++stairs;
                if (const int e = editor.stairCeilingError(p1))
                    std::printf("%s: %d \"%s\" at (%d,%d,%d): refused 0x%02X (the ceiling above)\n",
                                lotFile.filename().string().c_str(), id, o.definition->fileName.c_str(), t.x, t.y,
                                t.level, e);
            }
            if (p0.ok && !p1.ok)
                std::printf("%s: %d \"%s\" at (%d,%d,%d): refused 0x%02X (%s)\n", lotFile.filename().string().c_str(),
                            id, o.definition->fileName.c_str(), t.x, t.y, t.level, p1.error, p1.reason.c_str());
        }
    }
    std::printf("placement-sweep: %d lots, %d objects where they stand (%d staircases fitting)\n", lots, objects, stairs);
    for (const auto* m : {&before, &after}) {
        std::printf("  %s:", m == &before ? "without the lot's geometry" : "with it");
        for (const auto& [code, n] : *m)
            code ? std::printf(" 0x%02X %d", code, n) : std::printf(" fit %d", n);
        std::printf("\n");
    }
    return 0;
}

// Every saved fireplace against the chimney the fireplace tool would make
// for its mantle where it stands (checkFireplaces, build_cmds.hpp).
int cmdFireplaceSweep(const std::vector<std::string>& args) {
    if (args.empty())
        throw std::runtime_error("usage: s2pkg fireplace-sweep <lot.package>...");
    int lots = 0;
    FireplaceCheck total;
    for (const auto& a : args) {
        const fs::path lotFile = a;
        ResourceIndex index(resRootOf(lotFile));
        vm::CodeLibrary code(&index);
        DbpfPackage pkg(lotFile);
        Lot lot;
        try {
            lot = Lot::load(pkg);
        } catch (const std::exception& ex) {
            std::printf("%s: skipped (%s)\n", lotFile.filename().string().c_str(), ex.what());
            continue;
        }
        const LotObjects saved = LotObjects::load(pkg);
        vm::VM machine(code);
        machine.initCreatedObjects = true;
        machine.instructionsPerTurn = s2pkgBudget();
        machine.setClockGlobals();
        machine.placementLot = &lot;
        const bool hood = vm::loadLotNeighborhood(machine, lotFile);
        const vm::LotLoad load = vm::loadLot(machine, lot, saved, vm::charactersDirOf(lotFile));
        if (hood)
            vm::bindLotSims(machine);
        vm::startSavedLot(machine, load, saved);
        ++lots;
        static std::unique_ptr<BuildCatalog> catalog;
        if (!catalog)
            catalog = std::make_unique<BuildCatalog>(BuildCatalog::load(index));
        Lot edited = lot;
        vm::BuildEditor editor(machine, edited, catalog.get());
        const FireplaceCheck c = checkFireplaces(machine, editor, *catalog, lotFile.filename().string(),
                                                 std::getenv("S2PKG_SWEEP_WHY") != nullptr);
        total.chains += c.chains, total.same += c.same, total.stacked += c.stacked, total.stackedSame += c.stackedSame;
    }
    std::printf("fireplace-sweep: %d lots, %d fireplaces, %d the tool's chimney (%d of them in a chain with other mantles, "
                "%d of those the tool's without them)\n",
                lots, total.chains, total.same, total.stacked, total.stackedSame);
    return 0;
}

} // namespace

bool applyBuyEdits(vm::VM& vm, ResourceIndex& index, const std::vector<std::pair<std::string, std::string>>& edits) {
    static std::unique_ptr<Catalog> catalog;
    if (!catalog)
        catalog = std::make_unique<Catalog>(Catalog::load(index));
    bool ok = true;
    for (const auto& [kind, arg] : edits) {
        if (kind == "--buy" || kind == "--check") {
            const auto [what, where] = splitAt(arg);
            const Where w = parseWhere(where);
            const CatalogEntry* e = catalog->find(what);
            if (!e) {
                std::printf("  buy %s: not in the catalog\n", what.c_str());
                ok = false;
                continue;
            }
            const vm::ObjectId ghost = vm::createGhost(vm, e->group, e->instance);
            const vm::Placement p = vm::checkPlacement(vm, ghost, {w.x, w.y, w.level}, w.dir);
            std::string why;
            const bool bought = kind == "--buy" && vm::buy(vm, ghost, p, e->price, &why);
            if (!bought)
                vm::deleteGroup(vm, ghost);
            std::printf("  %s %s (%s, §%d) at (%d, %d) level %d facing %d: %s%s%s", kind.c_str() + 2, e->name.c_str(),
                        e->fileName.c_str(), e->price, w.x, w.y, w.level, w.dir,
                        p.ok ? "fits" : ("does not fit: " + p.reason).c_str(),
                        bought ? ", bought as object " : (kind == "--buy" && p.ok ? (", not bought: " + why).c_str() : ""),
                        bought ? std::to_string(ghost).c_str() : "");
            if (p.container)
                std::printf(" (in slot %d of object %d)", p.slot, p.container);
            std::printf("; funds §%d\n", vm::funds(vm));
            ok = ok && (kind == "--check" || bought);
        } else if (kind == "--sell") {
            const vm::ObjectId id = vm::ObjectId(std::stoi(arg));
            const vm::Object* o = vm.object(id);
            if (!o || !o->definition) {
                std::printf("  sell %d: no such object\n", id);
                ok = false;
                continue;
            }
            const std::string name = o->definition->fileName;
            const int value = vm::sell(vm, id);
            vm::deleteGroup(vm, id); // the session ends with the command
            std::printf("  sell %d (%s) for §%d; funds §%d\n", id, name.c_str(), value, vm::funds(vm));
        } else if (kind == "--move") {
            const auto [what, where] = splitAt(arg);
            const Where w = parseWhere(where);
            const vm::ObjectId id = vm::ObjectId(std::stoi(what));
            if (!vm.object(id)) {
                std::printf("  move %d: no such object\n", id);
                ok = false;
                continue;
            }
            const vm::Placement p = vm::checkPlacement(vm, id, {w.x, w.y, w.level}, w.dir);
            if (p.ok)
                vm::applyPlacement(vm, id, p);
            std::printf("  move %d to (%d, %d) level %d facing %d: %s\n", id, w.x, w.y, w.level, w.dir,
                        p.ok ? "moved" : ("does not fit: " + p.reason).c_str());
            ok = ok && p.ok;
        }
    }
    return ok;
}

int buyCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "catalog")
        return cmdCatalog({argv + 2, argv + argc});
    if (cmd == "buy")
        return cmdBuy({argv + 2, argv + argc});
    if (cmd == "placement-sweep")
        return cmdPlacementSweep({argv + 2, argv + argc});
    if (cmd == "fireplace-sweep")
        return cmdFireplaceSweep({argv + 2, argv + argc});
    return -1;
}

const char* buyUsage() {
    return "       s2pkg catalog <Res-dir> [--installation] [--function bit [--sub bit] | --room bit [--sub function-bit]] [-v]\n"
           "       s2pkg catalog <Res-dir> --thumb <guid|name> <out.png>\n"
           "       s2pkg buy <lot.package> [--buy|--check <guid|name>@x,y[,dir[,level]]] [--sell id] [--move id@x,y[,dir[,level]]]...\n"
           "       s2pkg placement-sweep <lot.package>...   (each saved object tested where it stands, with and without the lot's heights)\n"
           "       s2pkg fireplace-sweep <lot.package>...   (each saved fireplace's chain against the fireplace tool's)\n";
}

} // namespace os2
