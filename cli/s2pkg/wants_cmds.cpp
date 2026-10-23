// s2pkg wants: the wants, fears, aspiration and memories of a lot's Sims
// (src/vm/wants, src/formats/wants), optionally after running the lot.

#include "wants_cmds.hpp"
#include "budget.hpp"
#include "data_index.hpp"

#include "formats/binreader.hpp"
#include "formats/dbpf.hpp"
#include "formats/lot.hpp"
#include "formats/lot_objects.hpp"
#include "formats/neighborhood.hpp"
#include "formats/resindex.hpp"
#include "vm/hood_bridge.hpp"
#include "vm/lot.hpp"
#include "vm/lot_neighborhood.hpp"
#include "vm/lot_resume.hpp"
#include "vm/wants.hpp"

#include <cstdio>
#include <map>
#include <filesystem>
#include <stdexcept>

namespace os2 {

namespace {

namespace fs = std::filesystem;

fs::path resRoot(const fs::path& pkg) {
    for (auto p = fs::absolute(pkg).parent_path(); p.has_relative_path(); p = p.parent_path())
        if (p.filename() == "Res")
            return p;
    // A lot in the player's folder: the game's data from $OS2_DATA (as autonomy and pie).
    if (const char* data = std::getenv("OS2_DATA"); data && std::filesystem::is_directory(std::filesystem::path(data) / "TSData/Res"))
        return std::filesystem::path(data) / "TSData/Res";
    throw std::runtime_error("the lot is not under a TSData/Res directory (set OS2_DATA to data/game)");
}

const char* aspirationName(int v) {
    switch (v) {
    case 1: return "Romance";
    case 2: return "Family";
    case 4: return "Fortune";
    case 0x10: return "Popularity";
    case 0x20: return "Knowledge";
    case 0x40: return "Grow Up";
    case 0: return "none";
    }
    return "?";
}

const char* levelName(int level) {
    static const char* const n[] = {"rock bottom", "severe failure", "neutral", "bronze", "silver", "gold"};
    return level >= 0 && level < 6 ? n[level] : "?";
}

std::string simLabel(vm::VM& m, int16_t id) {
    const vm::Neighbor* n = m.neighbor(id);
    std::string s = n ? n->firstName + " " + n->lastName : std::string("?");
    return s + " (" + std::to_string(id) + ")";
}

void printWant(vm::VM& m, const wants::Instance& w, const char* kind, size_t slot) {
    const std::string text = vm::wantText(m, w);
    std::printf("    %s %zu: %-48s %+6d  %s%s\n", kind, slot + 1, text.c_str(), w.score, hexString(w.guid).c_str(),
                (w.flags & 4) ? "" : (w.flags & 2) ? " (check failed)" : "");
}

void printSim(vm::VM& m, int16_t id, bool history) {
    const vm::SimWants* s = vm::simWants(m, id);
    const vm::Neighbor* n = m.neighbor(id);
    const vm::Object* sim = m.neighborSim(id);
    const auto& pd = sim && sim->person ? sim->person->data : n->personData;
    vm::WantSystem* ws = vm::wantSystem(m);
    const int age = wants::ageEnum(pd[vm::persondata::PersonAge]);
    const int32_t raw = int32_t(pd[162]) * 10;
    std::printf("  %s: %s, aspiration %s, score %d (raw %d, %s), reward points %d%s\n", simLabel(m, id).c_str(),
                wants::ageName(age), aspirationName(pd[46]), pd[160], raw,
                ws ? levelName(vm::aspirationLevel(*ws->library, age, raw)) : "?", pd[161],
                sim ? ", on the lot" : "");
    if (!s) {
        std::printf("    no SWAF record\n");
    } else {
        for (size_t i = 0; i < s->wants.size(); ++i)
            if (s->wants[i])
                printWant(m, s->wants[i]->data, "want", i);
            else
                std::printf("    want %zu: (empty)\n", i + 1);
        for (size_t i = 0; i < s->fears.size(); ++i)
            if (s->fears[i])
                printWant(m, s->fears[i]->data, "fear", i);
            else
                std::printf("    fear %zu: (empty)\n", i + 1);
        size_t achievements = 0;
        for (const auto& [g, list] : s->history)
            achievements += list.size();
        std::printf("    history: %zu achievements of %zu wants, personal want clock %d (%.1f hours)\n", achievements,
                    s->history.size(), s->personal, s->personal / 3600.0);
        if (history)
            for (const auto& [g, list] : s->history)
                for (const auto& a : list)
                    std::printf("      %-48s at %u%s\n", vm::wantText(m, a->data).c_str(), a->data.time,
                                a->data.score ? (" (" + std::to_string(a->data.score) + " points)").c_str() : "");
    }
    // Memories: the neighbor inventory's singular tokens with flags 3
    // (docs/data-findings.md; the token's GUID is the memory object).
    const auto it = m.inventories.find({vm::InventoryKind::Neighbor, id});
    size_t memories = 0;
    if (it != m.inventories.end())
        for (const auto& t : it->second.singular) {
            if (t.flags != 3)
                continue;
            ++memories;
            std::string name = hexString(t.guid);
            if (auto d = m.code().definitionByGuid(t.guid))
                if (const ObjectDefinition* od = m.code().definition(d->first, d->second))
                    name = od->fileName;
            std::printf("    memory: %s", name.c_str());
            for (size_t k = 0; k < t.properties.size() && k < 8; ++k)
                std::printf(" %d", t.properties[k]);
            std::printf("\n");
        }
    if (!memories)
        std::printf("    memories: none\n");
}

int cmdWants(const fs::path& lotFile, const std::string& simQuery, long ticks, bool history) {
    ResourceIndex index;
    s2pkgAddInstallation(index, resRoot(lotFile));
    vm::CodeLibrary code(&index);
    DbpfPackage pkg(lotFile);
    const Lot lot = Lot::load(pkg);
    const LotObjects saved = LotObjects::load(pkg);
    vm::VM machine(code);
    machine.initCreatedObjects = true;
    machine.instructionsPerTurn = s2pkgBudget();
    machine.setClockGlobals();
    const bool hood = vm::loadLotNeighborhood(machine, lotFile);
    const vm::LotLoad load = vm::loadLot(machine, lot, saved, vm::charactersDirOf(lotFile));
    if (hood)
        vm::bindLotSims(machine);
    vm::startSavedLot(machine, load, saved, false);
    vm::WantSystem* ws = vm::wantSystem(machine);
    if (!ws) {
        std::printf("no want data (neighborhood %s)\n", hood ? "loaded" : "missing");
        return 1;
    }
    std::printf("want library: %zu definitions (%zu fears), %zu trees, %zu nodes, %zu texts; %zu SWAF records\n",
                ws->library->definitions.size(),
                size_t(std::count_if(ws->library->definitions.begin(), ws->library->definitions.end(),
                                     [](const auto& d) { return d.second.isFear(); })),
                ws->library->trees.size(), ws->library->nodes.size(), ws->library->texts.size(), ws->sims.size());
    for (const auto& e : ws->library->errors)
        std::printf("  library: %s\n", e.c_str());
    for (long i = 0; i < ticks; ++i)
        machine.tick();
    if (ticks) {
        std::printf("after %ld ticks:\n", ticks);
        for (const auto& e : ws->events)
            std::printf("  %s\n", describeWantEvent(machine, e).c_str());
    }
    std::vector<int16_t> ids;
    for (const auto& [id, o] : machine.objects())
        if (o.person && o.position.inWorld())
            if (const int16_t nid = vm::neighborIdOf(o); nid > 0 && machine.neighbor(nid))
                ids.push_back(nid);
    for (int16_t id : ids) {
        if (!simQuery.empty() && simLabel(machine, id).find(simQuery) == std::string::npos)
            continue;
        printSim(machine, id, history);
    }
    for (const auto& w : ws->warnings)
        std::printf("  warning: %s\n", w.c_str());
    return 0;
}

// Every SWAF of the neighborhoods under a folder, read and written back:
// the bytes must come out the same.
int cmdWantsSweep(const fs::path& dir) {
    size_t records = 0, same = 0, failed = 0, wants = 0, fears = 0, achievements = 0, tails = 0;
    std::map<uint32_t, size_t> versions;
    for (const auto& hood : hood::findNeighborhoods(dir)) {
        const fs::path pkgPath = hood / (hood.filename().string() + "_Neighborhood.package");
        DbpfPackage pkg(pkgPath);
        for (const auto& e : pkg.entries()) {
            if (e.tgi.type != wants::kTypeSwaf)
                continue;
            ++records;
            const auto bytes = pkg.read(e);
            try {
                const auto r = wants::Record::parse(bytes);
                ++versions[r.version];
                wants += r.wants.size();
                fears += r.fears.size();
                tails += !r.tail.empty();
                for (const auto& [g, l] : r.history.entries)
                    achievements += l.size();
                if (r.write() == bytes)
                    ++same;
                else
                    std::printf("%s SWAF %u: written back differently\n", hood.filename().string().c_str(),
                                e.tgi.instance);
            } catch (const std::exception& ex) {
                ++failed;
                std::printf("%s SWAF %u: %s\n", hood.filename().string().c_str(), e.tgi.instance, ex.what());
            }
        }
    }
    std::printf("%zu SWAF records: %zu written back the same, %zu failed; %zu wants, %zu fears, %zu achievements; "
                "%zu with stale bytes after the history; versions:",
                records, same, failed, wants, fears, achievements, tails);
    for (const auto& [v, n] : versions)
        std::printf(" %u x%zu", v, n);
    std::printf("\n");
    return failed || same != records ? 1 : 0;
}

} // namespace

std::string describeWantEvent(vm::VM& m, const vm::WantEvent& e) {
    using K = vm::WantEvent::Kind;
    wants::Instance w;
    w.guid = e.guid;
    w.subjectType = e.subjectType;
    w.subjectValue = e.subjectValue;
    w.subjectData = e.subjectData;
    w.owner = uint16_t(e.sim);
    char head[96];
    std::snprintf(head, sizeof head, "tick %6llu %-22s ", static_cast<unsigned long long>(e.tick),
                  simLabel(m, e.sim).c_str());
    std::string s = head;
    const char* kind = e.fear ? "fear" : "want";
    switch (e.kind) {
    case K::Rolled:
        s += std::string("rolls ") + kind + " " + std::to_string(e.slot + 1) + ": \"" + vm::wantText(m, w) + "\" (" +
             std::to_string(e.points) + ")";
        break;
    case K::Dropped:
        s += std::string("drops ") + kind + " " + std::to_string(e.slot + 1) + ": \"" + vm::wantText(m, w) + "\"";
        break;
    case K::Achieved:
        s += "FULFILS want " + std::to_string(e.slot + 1) + ": \"" + vm::wantText(m, w) + "\" (+" +
             std::to_string(e.points) + ")";
        break;
    case K::FearRealized:
        s += "FEAR COMES TRUE " + std::to_string(e.slot + 1) + ": \"" + vm::wantText(m, w) + "\" (" +
             std::to_string(e.points) + ")";
        break;
    case K::Satisfied:
        s += std::string("did \"") + vm::wantText(m, w) + "\" (" + kind + " not in a slot: history only)";
        break;
    case K::Score:
        s += "aspiration score " + std::to_string(e.before) + " -> " + std::to_string(e.after) + " (raw " +
             std::to_string(e.points) + ")";
        break;
    }
    return s;
}

int wantsCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "wants-sweep" && argc == 3)
        return cmdWantsSweep(argv[2]);
    if (cmd != "wants" || argc < 3)
        return -1;
    std::string lot, sim;
    long ticks = 0;
    bool history = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--sim" && i + 1 < argc)
            sim = argv[++i];
        else if (a == "--ticks" && i + 1 < argc)
            ticks = std::stol(argv[++i]);
        else if (a == "--history")
            history = true;
        else if (lot.empty())
            lot = a;
        else
            return -1;
    }
    if (lot.empty())
        return -1;
    return cmdWants(lot, sim, ticks, history);
}

const char* wantsUsage() {
    return "       s2pkg wants <lot.package> [--sim name] [--ticks N] [--history]\n"
           "       s2pkg wants-sweep <Neighborhoods dir>\n";
}

} // namespace os2
