// s2pkg autonomy: runs a lot with its saved state for N ticks and prints
// what each Sim chooses on its own (Find Best Interaction, src/vm/autonomy)
// and how its motives change (src/vm/motives).

#include "autonomy_cmds.hpp"
#include "budget.hpp"
#include "data_index.hpp"

#include "formats/bhav.hpp"
#include "formats/binreader.hpp"
#include "formats/dbpf.hpp"
#include "formats/environment_catalog.hpp"
#include "formats/lot.hpp"
#include "formats/lot_arrivals.hpp"
#include "formats/lot_objects.hpp"
#include "formats/resindex.hpp"
#include "vm/autonomy.hpp"
#include "vm/engine.hpp"
#include "vm/environment.hpp"
#include "vm/interactions.hpp"
#include "vm/lot.hpp"
#include "vm/lot_light.hpp"
#include "vm/hood_bridge.hpp"
#include "vm/lot_neighborhood.hpp"
#include "vm/lot_resume.hpp"
#include "vm/motives.hpp"
#include "vm/placement.hpp"
#include "vm/engine.hpp"
#include "vm/wants.hpp"
#include "wants_cmds.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <optional>
#include <stdexcept>
#include <string>

namespace os2 {

namespace {

// --dialog-answer N: blocking dialogs (a phone call for another Sim, ...)
// get button N at once, as `pie --dialog-answer`; by default they wait for
// the player, as the game does.
int gDialogAnswer = -1;
// --no-dialog-timeout: dialogs wait for an answer forever (by default
// they time out as the game's, on game time: vm::autoTimeOutDialogs).
bool gNoDialogTimeout = false;
// --name-babies: the soak's stand-in for a player at the newborn's name
// dialog (type 23, which waits for the player in the game too: no time
// limit): after a game minute it is given "Bebê", "Bebê 2" ... and OK.
// Ours, not the game's.
bool gNameBabies = false;
bool gSkipFailures = false; // --skip-failures: VM::autonomySkipsFailures (ours, not the binary's)

std::filesystem::path resRoot(const std::filesystem::path& pkg) {
    for (auto p = std::filesystem::absolute(pkg).parent_path(); p.has_relative_path(); p = p.parent_path())
        if (p.filename() == "Res")
            return p;
    // A lot in the player's folder: the game's data from $OS2_DATA (as lot-save).
    if (const char* data = std::getenv("OS2_DATA"); data && std::filesystem::is_directory(std::filesystem::path(data) / "TSData/Res"))
        return std::filesystem::path(data) / "TSData/Res";
    throw std::runtime_error("the lot is not under a TSData/Res directory (set OS2_DATA to data/game)");
}

std::string simName(const vm::Object& o) {
    const std::string n = o.definition ? o.definition->fileName : "?";
    const auto dash = n.find(" - ");
    return (dash == std::string::npos ? n : n.substr(dash + 3)) + "#" + std::to_string(o.id);
}

std::string objectName(vm::VM& machine, vm::ObjectId id) {
    const vm::Object* o = machine.object(id);
    if (!o)
        return "#" + std::to_string(id);
    if (o->person)
        return simName(*o);
    return o->definition ? o->definition->fileName : "?";
}

std::string clockText(const vm::VM& m) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d:%02d", m.clock.hour, m.clock.minute);
    return buf;
}

void printMotivesHeader() {
    std::printf("    %-12s %6s %6s %6s %6s %6s %6s %6s %6s %6s\n", "", "mood", "energy", "comfrt", "hunger", "hygien",
                "bladdr", "envir", "social", "fun");
}

void printMotives(const vm::Object& s) {
    const auto& m = s.person->motives;
    std::printf("    %-12s %6d %6d %6d %6d %6d %6d %6d %6d %6d\n", simName(s).c_str(), m[3], m[5], m[6], m[7], m[8], m[9],
                m[13], m[14], m[15]);
}

// S2PKG_RELS=1: the relationships between the Sims in the world (variables
// 0 daily, 1 flags, 2 lifetime).
void printRelationships(vm::VM& machine, const std::vector<vm::ObjectId>& sims, const char* when) {
    if (!std::getenv("S2PKG_RELS"))
        return;
    std::printf("relationships %s:\n", when);
    for (vm::ObjectId a : sims)
        for (vm::ObjectId b : sims) {
            if (a == b || !machine.object(a) || !machine.object(b))
                continue;
            auto it = machine.relationships.find({machine.relationshipKey(a), machine.relationshipKey(b)});
            if (it == machine.relationships.end() || it->second.size() < 3)
                continue;
            std::printf("    %-14s -> %-14s daily %4d lifetime %4d flags %04X\n", simName(*machine.object(a)).c_str(),
                        simName(*machine.object(b)).c_str(), it->second[0], it->second[2], uint16_t(it->second[1]));
        }
}

int cmdAutonomy(const std::filesystem::path& lotFile, long ticks, long every, const std::string& simQuery,
                std::optional<int> motives, bool fromInit, bool verbose, bool noDecay, int traceId, size_t traceDepth,
                size_t fbaDump, int fbaSim, const std::string& fbaFilter) {
    // The whole installation's Res trees, as the app indexes them
    // (src/app/win_main.cpp, cli/s2pkg/data_index.hpp): the folder `resRoot`
    // finds first and the packs beside it. With a single root the Episode's
    // content ran without the base game's (the AL pack alone lacks its
    // animations: "a-heatstroke-..." and even "o-door-open-CCW" were missing,
    // and wait loops that rely on an animation's length spun into the
    // instruction budget).
    ResourceIndex index;
    s2pkgAddInstallation(index, resRoot(lotFile));
    vm::CodeLibrary code(&index);
    DbpfPackage pkg(lotFile);
    const Lot lot = Lot::load(pkg);
    LotObjects saved = LotObjects::load(pkg);
    // A family that just moved in: its first day, as the app's
    // LotSimulation (formats/lot_arrivals.hpp; the charge is left out): the
    // "reset lot" broadcast below brings its Sims.
    const LotArrivals arrivals = addArrivingSims(saved, lot, lotFile, &index, false);
    vm::VM machine(code);
    machine.initCreatedObjects = true;
    // The binary's limit (budget.hpp): a lower one made the N003
    // "Controller - Relationship Decay" fail: it normalizes every household
    // Sim against every neighbor in one tick.
    machine.instructionsPerTurn = s2pkgBudget();
    // OS2_SEED=N: another seed for the VM's random numbers (to tell a
    // change's effect from the run's chance in soaks).
    if (const char* seed = std::getenv("OS2_SEED"))
        machine.random.seed(uint32_t(std::strtoul(seed, nullptr, 10)));
    // S2PKG_NO_RESET=1: a Main that fails stays dead (for looking into the
    // error) instead of being reset as the game does.
    machine.resetOnError = std::getenv("S2PKG_NO_RESET") == nullptr;
    machine.autoDialogAnswer = gDialogAnswer;
    machine.dialogTimeouts = !gNoDialogTimeout;
    machine.autonomySkipsFailures = gSkipFailures;
    // The environment's light term comes from the lot's light maps, made
    // without a window as the app makes them (vm/lot_light.hpp, below; S2PKG_NO_LIGHT=1:
    // left out). S2PKG_LIGHT=day[:night], a test aid, stands every Sim in
    // that illuminance instead (the day one from 7:00 to 18:59, as
    // Lighting.txt's states; the night one otherwise), e.g. 0.55 for the
    // full -20 of a dark room.
    if (const char* light = std::getenv("S2PKG_LIGHT")) {
        const float day = std::strtof(light, nullptr);
        const char* colon = std::strchr(light, ':');
        const float night = colon ? std::strtof(colon + 1, nullptr) : day;
        machine.floorIllumination = [&machine, day, night](int, float, float) {
            return std::optional<float>(machine.clock.hour >= 7 && machine.clock.hour < 19 ? day : night);
        };
    }
    machine.setClockGlobals();
    const bool hood = vm::loadLotNeighborhood(machine, lotFile);
    const vm::LotLoad load = vm::loadLot(machine, lot, saved, vm::charactersDirOf(lotFile));
    if (hood)
        vm::bindLotSims(machine);
    vm::startSavedLot(machine, load, saved, fromInit);
    if (hood && arrivals.firstDay) {
        const vm::ResetLotResult r = vm::runResetLot(machine, arrivals.arriving);
        std::printf("first day: %zu \"reset lot\" trees, %zu Sims of family %u arrive\n", r.trees, r.sims.size(),
                    arrivals.family);
        for (const auto& e : r.errors)
            std::printf("  reset lot: %s\n", e.c_str());
    }
    // The lot is ready: "CT - Lot Loaded" (the move-in scene after a move).
    if (std::string err; !vm::runLotLoaded(machine, &err) && !err.empty())
        std::printf("CT - Lot Loaded: %s\n", err.c_str());
    // The light maps (docs/motives-mac.md §8.8): made once the lot is set up,
    // as the app's object view and LotLighting are.
    std::unique_ptr<vm::HeadlessLotLighting> lighting;
    if (!std::getenv("S2PKG_LIGHT") && !std::getenv("S2PKG_NO_LIGHT")) {
        lighting = std::make_unique<vm::HeadlessLotLighting>(index, lot, pkg, resRoot(lotFile), machine);
        lighting->install();
    }
    if (motives)
        vm::setAllMotives(machine, int16_t(*motives));
    vm::gMotiveDecay = !noDecay;

    std::vector<vm::ObjectId> sims;
    for (const auto& [id, o] : machine.objects())
        if (o.person && o.position.inWorld())
            if (simQuery.empty() || simName(o).find(simQuery) != std::string::npos)
                sims.push_back(id);
    std::printf("%s: %zu Sims in the world, %ld ticks from %s (%ld game minutes)\n",
                lotFile.filename().string().c_str(), sims.size(), ticks, clockText(machine).c_str(),
                ticks / machine.clock.ticksPerMinute);
    // The family's funds and each Sim's job (person data 56 school grade, 57
    // job level, 63 job performance), printed again when they change, and
    // when a Sim leaves the lot (out of world: at work or school) or comes back.
    const int16_t family = machine.globals[vm::global::CurrentFamily];
    int32_t funds = vm::familyFunds(machine, family);
    constexpr uint32_t kBillsGuid = 0xD840F488;
    std::set<vm::ObjectId> seenBills;
    for (const auto& [oid, ob] : machine.objects())
        if (ob.guid() == kBillsGuid)
            seenBills.insert(oid); // already in the lot
    std::array<int16_t, 4> values{-1, -1, -1, -1};
    std::printf("  family %d funds %d\n", family, funds);
    std::map<vm::ObjectId, uint32_t> simGuid; // 0 once reported gone
    std::map<vm::ObjectId, std::string> simLabel;
    for (vm::ObjectId id : sims)
        if (const vm::Object* s = machine.object(id); s && s->person)
            simGuid[id] = s->guid(), simLabel[id] = simName(*s);
    if (std::getenv("S2PKG_GLOBALS")) { // the simulator's globals (STR# 0x81) after loading
        std::printf("  globals:");
        for (size_t g = 0; g < 54; ++g)
            std::printf(" %zu=%d", g, machine.globals[g]);
        std::printf("\n");
    }
    std::map<vm::ObjectId, std::array<int16_t, 3>> job;
    std::map<vm::ObjectId, bool> onLot;
    auto jobOf = [](const vm::Object& s) {
        return std::array<int16_t, 3>{s.person->data[56], s.person->data[57], s.person->data[63]};
    };
    for (vm::ObjectId id : sims) {
        const vm::Object& s = *machine.object(id);
        job[id] = jobOf(s);
        onLot[id] = true;
        std::printf("  %-12s grade %d, job level %d, job performance %d\n", simName(s).c_str(), job[id][0], job[id][1],
                    job[id][2]);
    }
    printMotivesHeader();
    for (vm::ObjectId id : sims)
        printMotives(*machine.object(id));
    printRelationships(machine, sims, "at the start");

    // S2PKG_VISITORS=1: the people who come in while running (visitors,
    // NPCs) are followed too: what they run and choose, from when they come
    // (not in the motive tables or the summary, which stay the household's).
    static const bool followVisitors = std::getenv("S2PKG_VISITORS") != nullptr;
    std::vector<vm::ObjectId> tracked = sims;
    std::map<vm::ObjectId, size_t> choices, searches;
    vm::autonomyObserver() = [&](const vm::AutonomyDecision& d) {
        if (std::find(tracked.begin(), tracked.end(), d.sim) == tracked.end())
            return;
        ++searches[d.sim];
        if (!d.found && !verbose)
            return;
        std::printf("  tick %6llu %s %-10s Find Best Interaction (mask %04X): %zu advertise, %zu checked",
                    static_cast<unsigned long long>(machine.ticks), clockText(machine).c_str(),
                    objectName(machine, d.sim).c_str(), d.motiveMask, d.candidates, d.checked);
        if (!d.checkErrors.empty())
            std::printf(", %zu check trees failed", d.checkErrors.size());
        if (verbose) {
            std::map<std::string, int> why;
            for (const auto& e : d.checkErrors)
                ++why[e.substr(e.find(": ") + 2)];
            for (const auto& [w, n] : why)
                std::printf("\n      check tree failure x%d: %s", n, w.c_str());
        }
        if (fbaDump && (fbaSim == 0 || fbaSim == int(d.sim))) {
            std::printf("\n      all candidates (static score, check, score):");
            size_t i = 0;
            for (const auto& c : d.considered) {
                const bool match = !fbaFilter.empty() && (c.name.find(fbaFilter) != std::string::npos ||
                                                          objectName(machine, c.object).find(fbaFilter) != std::string::npos);
                if (i++ >= fbaDump && !match)
                    continue;
                std::printf("\n      %3zu %-34s on %-30s %9.3e %s %9.3e", i, c.name.c_str(),
                            objectName(machine, c.object).c_str(), c.staticScore,
                            !c.checked ? "unchecked" : c.passed ? "pass     " : "FAIL     ", c.score);
            }
        }
        if (!d.found) {
            std::printf(", nothing\n");
            return;
        }
        ++choices[d.sim];
        std::printf("\n");
        for (size_t i = 0; i < d.best.size() && i < (verbose ? 5u : 3u); ++i)
            std::printf("      %s %-34s on %-38s score %9.3e, %4.1f tiles\n", i == 0 ? "->" : "  ", d.best[i].name.c_str(),
                        objectName(machine, d.best[i].object).c_str(), d.best[i].score, d.best[i].distance);
    };

    // --trace ID: the Sim's instructions in its first frames, from its first
    // Find Best Interaction on (at most 3000 lines).
    bool tracing = false;
    size_t traced = 0;
    if (traceId)
        machine.trace = [&](const vm::Thread& t, const vm::Frame& f, const Behavior::Instruction& in) {
            // S2PKG_TRACE_ANY=1: also the object's one-tick trees (Run Tree by Name in its stack, ...).
            static const bool any = std::getenv("S2PKG_TRACE_ANY") != nullptr;
            if (t.me != traceId || t.stack.empty() ||
                (!any && &t.stack.front() != &machine.threads[traceId].stack.front()))
                return;
            static const long from = std::getenv("S2PKG_TRACE_FROM") ? std::atol(std::getenv("S2PKG_TRACE_FROM")) : 0;
            static const size_t most = std::getenv("S2PKG_TRACE_MAX") ? std::atol(std::getenv("S2PKG_TRACE_MAX")) : 3000;
            if ((in.opcode == 0x03 || from > 0) && long(machine.ticks) >= from)
                tracing = true;
            if (!tracing || t.stack.size() > traceDepth || traced++ >= most)
                return;
            const std::string op = in.opcode < 0x100 ? primitiveName(in.opcode) : "call " + hexString(in.opcode);
            std::printf("    t%-6llu %*s%04X #%-3u %-30s %s %s\n", static_cast<unsigned long long>(machine.ticks),
                        int(t.stack.size() - 1) * 2, "", f.instance, f.pc, op.c_str(), f.behavior->fileName.c_str(),
                        vm::describeOperands(machine, t, f, in).c_str());
        };

    // S2PKG_LOG_TREES=text[|text...]: every start of a tree whose name has
    // one of the texts, with the tick, who runs it and the tree below it on
    // the stack.
    if (const char* logTrees = std::getenv("S2PKG_LOG_TREES"); logTrees && !traceId)
        machine.trace = [&machine, want = std::string(logTrees)](const vm::Thread& t, const vm::Frame& f, const Behavior::Instruction&) {
            if (f.pc != 0 || &f != &t.stack.back())
                return;
            bool match = false;
            for (size_t at = 0; !match && at <= want.size();) {
                const size_t bar = std::min(want.find('|', at), want.size());
                match = f.behavior->fileName.find(want.substr(at, bar - at)) != std::string::npos;
                at = bar + 1;
            }
            if (!match)
                return;
            // One line per call, not per loop back to the start or re-run in the same tick.
            static std::map<std::pair<const vm::Thread*, size_t>, std::pair<const Behavior*, uint64_t>> last;
            auto& seen = last[{&t, t.stack.size()}];
            if (seen == std::pair<const Behavior*, uint64_t>{f.behavior, machine.ticks})
                return;
            seen = {f.behavior, machine.ticks};
            const std::string caller = t.stack.size() > 1 ? t.stack[t.stack.size() - 2].behavior->fileName : "";
            std::printf("  tick %6llu %s %s (%d) \"%s\" (0x%08X:0x%04X) from \"%s\", stack object %d %s\n", static_cast<unsigned long long>(machine.ticks), clockText(machine).c_str(),
                        objectName(machine, t.me).c_str(), t.me, f.behavior->fileName.c_str(), f.group, f.instance, caller.c_str(),
                        f.stackObject, machine.object(f.stackObject) ? objectName(machine, f.stackObject).c_str() : "-");
        };

    // Wants and fears rolled, fulfilled and come true, and the aspiration
    // score (src/vm/wants), as they happen.
    size_t wantEvents = 0;
    auto printWantEvents = [&] {
        vm::WantSystem* ws = vm::wantSystem(machine);
        if (!ws)
            return;
        while (wantEvents < ws->events.size()) {
            const auto& e = ws->events[wantEvents++];
            if (e.kind == vm::WantEvent::Kind::Satisfied && !verbose)
                continue;
            std::printf("  %s %s\n", clockText(machine).c_str(), describeWantEvent(machine, e).c_str());
        }
        if (ws->events.size() >= 4000) { // keep the window small
            ws->events.clear();
            wantEvents = 0;
        }
    };
    std::map<vm::ObjectId, std::string> running;
    std::map<vm::ObjectId, std::pair<uint64_t, size_t>> lastMemory; // newest remembered tick, count
    // S2PKG_FIRE=tick:object: test aid, a Fire set under that object at
    // that tick, as the food's "Sub - Set Fire" makes one (Create New Object
    // Instance of the Fire, GUID 0x24C95F99, below the object; here the object
    // comes in parameter 0).
    long fireTick = -1;
    vm::ObjectId fireUnder = 0;
    if (const char* f = std::getenv("S2PKG_FIRE"); f && std::strchr(f, ':')) {
        fireTick = std::atol(f);
        fireUnder = vm::ObjectId(std::atoi(std::strchr(f, ':') + 1));
    }
    // S2PKG_ROOMS=1: each Sim's room and the rooms it can reach (Find Best
    // Object for Function's GetAccessibleRooms).
    if (std::getenv("S2PKG_ROOMS"))
        for (vm::ObjectId id : sims)
            if (const vm::Object* s = machine.object(id)) {
                std::printf("  %s in room %d reaches:", simName(*s).c_str(), s->data[vm::objdata::Room]);
                if (const auto* r = vm::accessibleRooms(machine, s->data[vm::objdata::Room]))
                    for (int16_t room : *r)
                        std::printf(" %d", room);
                std::printf("\n");
                if (const auto* r = vm::accessibleRooms(machine, s->data[vm::objdata::Room])) {
                    std::map<int16_t, std::string> out;
                    for (const auto& [oid, o] : machine.objects())
                        if (o.position.inWorld() && !std::binary_search(r->begin(), r->end(), o.data[vm::objdata::Room]) &&
                            out[o.data[vm::objdata::Room]].size() < 160)
                            out[o.data[vm::objdata::Room]] += " " + objectName(machine, oid) + "@" + std::to_string(int(o.position.x)) + "," +
                                                               std::to_string(int(o.position.y)) + "," + std::to_string(o.position.level) + ";";
                    for (const auto& [room, names] : out)
                        std::printf("    not reached: room %d:%s\n", room, names.c_str());
                }
            }
    if (const char* want = std::getenv("S2PKG_OBJECTS")) // the objects whose name has the text
        for (const auto& [oid, o] : machine.objects())
            if (objectName(machine, oid).find(want) != std::string::npos)
                std::printf("  object %d %s at (%.2f, %.2f, %d) dir %d lead %d container %d flags 0x%04x placement 0x%04x heights 0x%04x%s\n", oid,
                            objectName(machine, oid).c_str(), o.position.x, o.position.y, o.position.level, o.data[vm::objdata::Direction],
                            o.data[vm::objdata::LeadTileObjectId], o.data[vm::objdata::ContainerId], uint16_t(o.data[vm::objdata::Flags]),
                            uint16_t(o.data[42]), uint16_t(o.data[4]), vm::VM::blocksRouting(o) ? " blocks" : ""),
                std::printf("    attributes:"), [&] { for (int16_t a : o.attributes) std::printf(" %d", a); }(),
                std::printf(o.person ? " person type %d greet status %d\n" : "\n", o.person ? o.person->data[32] : 0,
                            o.person ? o.person->data[34] : 0);
    if (const char* want = std::getenv("S2PKG_OBJECTS"); want && std::getenv("S2PKG_FOOTPRINTS")) // and their routing footprints
        for (const auto& [oid, o] : machine.objects())
            if (objectName(machine, oid).find(want) != std::string::npos && o.position.inWorld() && !o.person) {
                std::printf("  footprint %d %s:", oid, objectName(machine, oid).c_str());
                for (const auto& r : machine.routingFootprint(o))
                    std::printf(" [%.2f,%.2f-%.2f,%.2f]", r[0], r[1], r[2], r[3]);
                std::printf("\n");
            }
    // S2PKG_WALLADJ=<name text>: the saved wall adjacency (object data 5,
    // low byte) of the objects with a "wall adjacency" entry whose name has
    // the text, against what vm::wallAdjacencyFlags computes.
    if (const char* want = std::getenv("S2PKG_WALLADJ")) {
        size_t agree = 0, all = 0;
        for (const auto& [oid, o] : machine.objects())
            if (o.position.inWorld() && !o.data[vm::objdata::ContainerId] && machine.functionTree(o, 6) &&
                objectName(machine, oid).find(want) != std::string::npos) {
                const int saved = o.data[5] & 0xFF, computed = vm::wallAdjacencyFlags(machine, o) & 0xFF;
                ++all;
                agree += saved == computed;
                if (saved != computed)
                    std::printf("  wall adjacency of %d %s: saved 0x%02x, computed 0x%02x\n", oid, objectName(machine, oid).c_str(),
                                saved, computed);
            }
        std::printf("  wall adjacency: %zu of %zu agree\n", agree, all);
    }
    if (std::getenv("S2PKG_DOORS"))
        for (const auto& [oid, o] : machine.objects())
            if (o.definition && o.definition->objectType() == vm::kObjectTypeDoor && o.position.inWorld())
                std::printf("  door object %d %s at (%.2f, %.2f, %d) dir %d lead %d\n", oid, objectName(machine, oid).c_str(), o.position.x,
                            o.position.y, o.position.level, o.data[vm::objdata::Direction], o.data[vm::objdata::LeadTileObjectId]);
    if (std::getenv("S2PKG_ROOMS") && machine.planner)
        for (const auto& g : machine.planner->roomGroups()) {
            std::printf("  walkable group, rooms:");
            for (int16_t r : g)
                std::printf(" %d", r);
            std::printf("\n");
        }
    if (std::getenv("S2PKG_ROOMS") && machine.planner)
        for (const auto& p : machine.planner->portals())
            std::printf("  portal %s %d: (%.2f, %.2f, %d) [%s] <-> (%.2f, %.2f, %d) [%s]\n", p.stair >= 0 ? "stair" : "door",
                        p.stair >= 0 ? p.stair : p.door, p.a[0], p.a[1], p.levelA,
                        machine.planner->free(p.levelA, p.a, vm::PathPlanner::kStatic) ? "free" : "blocked", p.b[0], p.b[1], p.levelB,
                        machine.planner->free(p.levelB, p.b, vm::PathPlanner::kStatic) ? "free" : "blocked");
    for (long i = 0; i < ticks; ++i) {
        if (i == fireTick && machine.object(fireUnder)) {
            Behavior b;
            b.fileName = "Test - Set Fire";
            b.signature = 0x8009;
            b.argCount = 1;
            Behavior::Instruction in;
            in.opcode = 0x2A;
            in.onTrue = {Behavior::TargetKind::True, 0};
            in.onFalse = {Behavior::TargetKind::False, 0};
            in.operands = {0x99, 0x5F, 0xC9, 0x24, 0x07, 0x01, 0, 0};
            b.instructions.push_back(in);
            const vm::Object& under = *machine.object(fireUnder);
            code.add(under.group, 0x1FF1, std::move(b));
            std::array<int16_t, vm::kTempCount> temps{};
            const std::array<int16_t, 1> params{int16_t(fireUnder)};
            const bool ok = vm::runOneTickTree(machine, temps, fireUnder, fireUnder, fireUnder, 0x1FF1, params);
            std::printf("  tick %6ld fire set under %d: %s\n", i, fireUnder, ok ? "true" : "false");
        }
        // S2PKG_QUEUES=1: each Sim's queue as it changes (before the first
        // tick too: what the lot loads with). S2PKG_ANIMS=1: each Sim's
        // Animate Sim and overlays as they change (as the app's OS2_LOG_ANIMS).
        auto logSims = [&] {
            static const bool queues = std::getenv("S2PKG_QUEUES") != nullptr;
            static const bool anims = std::getenv("S2PKG_ANIMS") != nullptr;
            static std::map<vm::ObjectId, std::string> lastQ, lastA;
            for (const auto& [oid, ob] : machine.objects()) {
                if (!ob.person)
                    continue;
                if (queues) {
                    std::string s;
                    for (size_t k = 0; k < ob.queue.size(); ++k)
                        s += (int(k) == ob.activeInteraction ? "[*" : "[") + ob.queue[k].name + " on " +
                             std::to_string(ob.queue[k].object) + " p" + std::to_string(ob.queue[k].priority) + "] ";
                    if (ob.lookAt)
                        s += "looks at " + std::to_string(ob.lookAt->target);
                    if (s != lastQ[oid])
                        std::printf("  tick %6llu Sim %d queue: %s\n", static_cast<unsigned long long>(machine.ticks), oid,
                                    s.c_str()),
                            lastQ[oid] = s;
                }
                if (anims) {
                    std::string s = ob.animation.name + "@" + std::to_string(ob.animation.level) + " [";
                    for (const auto& o : ob.animation.overlays)
                        s += o.name + "@" + std::to_string(o.level) + "#" + std::to_string(o.id) + " ";
                    s += "] carry {";
                    for (const auto& c : ob.carryOverlays)
                        s += c + " ";
                    s += "} loco {";
                    for (const auto& l : ob.animation.locomotion) {
                        char b[32];
                        std::snprintf(b, sizeof b, "%X:%d ", l.group, l.set);
                        s += b;
                    }
                    s += "} in " + std::to_string(ob.data[vm::objdata::ContainerId]);
                    if (s != lastA[oid])
                        std::printf("  tick %6llu Sim %d anim: %s at (%.1f, %.1f, %d)\n",
                                    static_cast<unsigned long long>(machine.ticks), oid, s.c_str(), ob.position.x,
                                    ob.position.y, int(ob.position.level)),
                            lastA[oid] = s;
                }
            }
        };
        if (i == 0)
            logSims();
        machine.tick();
        logSims();
        if (gNameBabies)
            for (auto& [who, d] : machine.dialogs) {
                static std::map<uint32_t, uint64_t> seen;
                if (d.type != vm::dialogtype::BabyName || d.answer >= 0)
                    continue;
                const uint64_t first = seen.emplace(d.id, machine.ticks).first->second;
                if (machine.ticks - first < 30)
                    continue;
                for (size_t k = 0; k < d.names.size(); ++k)
                    if (d.names[k].text.empty())
                        d.names[k].text = k ? "Bebê " + std::to_string(k + 1) : std::string("Bebê");
                if (!d.names.empty())
                    d.text = d.names[0].text;
                d.answer = 0;
                std::printf("  tick %6llu %s %d's name dialog answered: \"%s\"%s\n", static_cast<unsigned long long>(machine.ticks),
                            clockText(machine).c_str(), who, d.text.c_str(), d.names.size() > 1 ? " (and more)" : "");
            }
        printWantEvents();
        for (const auto& [rid, r] : machine.errorResets)
            if (r.lastTick == machine.ticks && (r.count <= 20 || r.count % 1000 == 0)) {
                const vm::Object* x = machine.object(rid);
                std::printf("  tick %6llu %s %s reset after an error: %s\n", static_cast<unsigned long long>(machine.ticks), clockText(machine).c_str(),
                            x && x->person ? simName(*x).c_str() : objectName(machine, rid).c_str(),
                            (verbose ? r.last : r.last.substr(0, r.last.find('\n'))).c_str());
            }
        // S2PKG_WATCH=id: that object's place and container every 30 ticks.
        if (static const char* w = std::getenv("S2PKG_WATCH"); w && machine.ticks % 30 == 0)
            if (const vm::Object* x = machine.object(vm::ObjectId(std::atoi(w))))
            {
                std::printf("  tick %6llu watch %d at (%.2f, %.2f, %d) dir %d in %d", static_cast<unsigned long long>(machine.ticks), x->id,
                            x->position.x, x->position.y, x->position.level, x->data[vm::objdata::Direction], x->data[vm::objdata::ContainerId]);
                if (x->person) { // its priority and queue
                    std::printf(", priority %d, active %d, queue:", x->person->data[vm::persondata::Priority], x->activeInteraction);
                    for (const auto& q : x->queue)
                        std::printf(" [%s %d]", q.name.c_str(), q.priority);
                }
                std::printf("\n");
            }
        // Cinematics the trees ask for (Cinematic 0x7B): the move-in scene, births, ...
        for (const auto& r : vm::cinematicRequests())
            std::printf("  tick %6llu %s cinematic \"%s\" from %d (%zu actors)\n",
                        static_cast<unsigned long long>(machine.ticks), clockText(machine).c_str(), r.scene.c_str(),
                        r.me, r.actors.size());
        vm::cinematicRequests().clear();
        if (followVisitors)
            for (vm::ObjectId id : machine.people())
                if (const vm::Object* s = machine.object(id); s && s->position.inWorld() && simGuid[id] != s->guid() &&
                                                             std::find(sims.begin(), sims.end(), id) == sims.end()) {
                    if (std::find(tracked.begin(), tracked.end(), id) == tracked.end())
                        tracked.push_back(id);
                    simGuid[id] = s->guid(), simLabel[id] = simName(*s), job[id] = jobOf(*s), onLot[id] = true;
                    running[id].clear();
                    std::printf("  tick %6llu %s %-10s comes in (visitor) at (%.2f, %.2f, %d), person type %d\n",
                                static_cast<unsigned long long>(machine.ticks), clockText(machine).c_str(),
                                simName(*s).c_str(), s->position.x, s->position.y, s->position.level,
                                s->person->data[32]);
                }
        for (vm::ObjectId id : tracked) {
            const vm::Object* s = machine.object(id);
            if (!s || !s->person || s->guid() != simGuid[id]) {
                // Gone: an urn took the dead, the social worker's car took
                // a child away (Sub - Send Passengers Out of World removes
                // them), a visitor left; its ID may be another object's now.
                if (!simGuid[id])
                    continue;
                simGuid[id] = 0;
                std::printf("  tick %6llu %s %-10s is gone (removed from the lot)\n",
                            static_cast<unsigned long long>(machine.ticks), clockText(machine).c_str(),
                            simLabel[id].c_str());
                continue;
            }
            simLabel[id] = simName(*s);
            if (followVisitors) { // person data 32 "Person Type" (not 0: a visitor)
                static std::map<vm::ObjectId, int16_t> type;
                const int16_t t = s->person->data[32];
                if (auto [it, fresh] = type.try_emplace(id, t); !fresh && it->second != t) {
                    std::printf("  tick %6llu %s %-10s person type %d -> %d\n", static_cast<unsigned long long>(machine.ticks),
                                clockText(machine).c_str(), simName(*s).c_str(), it->second, t);
                    it->second = t;
                }
            }
            if (const bool in = s->position.inWorld(); in != onLot[id]) {
                onLot[id] = in;
                std::printf("  tick %6llu %s %-10s %s\n", static_cast<unsigned long long>(machine.ticks),
                            clockText(machine).c_str(), simName(*s).c_str(),
                            in ? "back on the lot" : "leaves the lot (out of world)");
            }
            if (const auto now = jobOf(*s); now != job[id]) {
                std::printf("  tick %6llu %s %-10s grade %d -> %d, job level %d -> %d, job performance %d -> %d\n",
                            static_cast<unsigned long long>(machine.ticks), clockText(machine).c_str(),
                            simName(*s).c_str(), job[id][0], now[0], job[id][1], now[1], job[id][2], now[2]);
                job[id] = now;
            }
            std::string now;
            if (s->activeInteraction >= 0 && size_t(s->activeInteraction) < s->queue.size())
                now = s->queue[size_t(s->activeInteraction)].name;
            if (const auto& mem = s->person->autonomyMemory; !mem.empty()) {
                const auto key = std::make_pair(mem.back().tick, mem.size());
                if (lastMemory[id] != key) {
                    lastMemory[id] = key;
                    const vm::Object* o = machine.object(mem.back().object);
                    std::string name = "#" + std::to_string(mem.back().interaction);
                    if (o)
                        if (auto q = vm::makeInteraction(machine, o->id, mem.back().interaction))
                            name = q->name;
                    std::printf("  tick %6llu %s %-10s autonomous \"%s\" on %s ended: %s\n",
                                static_cast<unsigned long long>(machine.ticks), clockText(machine).c_str(),
                                simName(*s).c_str(), name.c_str(), objectName(machine, mem.back().object).c_str(),
                                mem.back().failed ? "false (remembered as failed)" : "true");
                }
            }
            if (now != running[id]) {
                if (!now.empty())
                    std::printf("  tick %6llu %s %-10s running \"%s\"\n", static_cast<unsigned long long>(machine.ticks),
                                clockText(machine).c_str(), simName(*s).c_str(), now.c_str());
                else
                    std::printf("  tick %6llu %s %-10s done with \"%s\"\n", static_cast<unsigned long long>(machine.ticks),
                                clockText(machine).c_str(), simName(*s).c_str(), running[id].c_str());
                // S2PKG_FIRES=1: where the Sim is and whether it burns (object
                // data 8 bit 0x200), on a line of its own.
                if (static const bool fires = std::getenv("S2PKG_FIRES") != nullptr; fires)
                    std::printf("  tick %6llu   %s at (%.2f, %.2f, %d)%s\n", static_cast<unsigned long long>(machine.ticks),
                                simName(*s).c_str(), s->position.x, s->position.y, s->position.level,
                                (s->data[vm::objdata::Flags] & 0x200) ? " burning" : "");
                running[id] = now;
            }
        }
        // S2PKG_FIRES=1: the fires (objects named "Fire") made and gone, with their tiles.
        if (static const bool fires = std::getenv("S2PKG_FIRES") != nullptr; fires) {
            static std::map<vm::ObjectId, std::string> seenFires;
            std::set<vm::ObjectId> alive;
            for (const auto& [oid, ob] : machine.objects())
                if (ob.definition && ob.definition->fileName == "Fire" && ob.position.inWorld()) {
                    alive.insert(oid);
                    if (!seenFires.count(oid)) {
                        char where[64];
                        std::snprintf(where, sizeof where, "(%.2f, %.2f, %d)", ob.position.x, ob.position.y, ob.position.level);
                        seenFires[oid] = where;
                        const auto fp = machine.routingFootprint(ob);
                        std::printf("  tick %6llu %s fire %d at %s, flags 0x%X, %s, %zu footprint rectangles\n",
                                    static_cast<unsigned long long>(machine.ticks), clockText(machine).c_str(), oid, where,
                                    uint16_t(ob.data[vm::objdata::Flags]), vm::VM::blocksRouting(ob) ? "blocks" : "does not block",
                                    fp.size());
                        // The first fire of an outbreak: what is near it and
                        // who was using it (whose active interaction is on an
                        // object within 2 tiles), to tell how it started.
                        if (seenFires.size() == 1) {
                            auto near = [&](const vm::Object& x) {
                                return x.position.level == ob.position.level && std::abs(x.position.x - ob.position.x) < 2 &&
                                       std::abs(x.position.y - ob.position.y) < 2;
                            };
                            std::printf("  tick %6llu   outbreak near:", static_cast<unsigned long long>(machine.ticks));
                            for (const auto& [nid, n] : machine.objects())
                                if (nid != oid && !n.person && n.position.inWorld() && near(n) && n.definition &&
                                    n.definition->fileName != "Fire")
                                    std::printf(" %d %s;", nid, n.definition->fileName.c_str());
                            std::printf("\n");
                            for (const auto& [pid, p] : machine.objects())
                                if (p.person && p.activeInteraction >= 0 && size_t(p.activeInteraction) < p.queue.size()) {
                                    const auto& q = p.queue[size_t(p.activeInteraction)];
                                    const vm::Object* t = machine.object(q.object);
                                    if ((t && near(*t)) || near(p))
                                        std::printf("  tick %6llu   outbreak: %s#%d%s running \"%s\" on %d %s\n",
                                                    static_cast<unsigned long long>(machine.ticks), simName(p).c_str(), pid,
                                                    std::find(sims.begin(), sims.end(), pid) == sims.end() ? " (visitor)" : "",
                                                    q.name.c_str(), q.object, t && t->definition ? t->definition->fileName.c_str() : "?");
                                }
                        }
                    }
                }
            for (auto it = seenFires.begin(); it != seenFires.end();)
                if (!alive.count(it->first)) {
                    std::printf("  tick %6llu %s fire %d out\n", static_cast<unsigned long long>(machine.ticks),
                                clockText(machine).c_str(), it->first);
                    it = seenFires.erase(it);
                } else {
                    ++it;
                }
            // Every person (household, visitors, NPCs) that catches fire or
            // stops burning (object data 8 bit 0x200, set by the Fire's
            // "Main - Set on Fire" trees), and every NPC that comes on the
            // lot with its person data 155 "NPC type" (the firefighter is
            // the one the smoke alarm's "Sub - Call Fire Fighter" schedules).
            static std::map<vm::ObjectId, bool> burning;
            static std::map<vm::ObjectId, uint32_t> npcSeen; // id -> GUID when seen
            for (vm::ObjectId id : machine.people()) {
                const vm::Object* s = machine.object(id);
                if (!s || !s->person || !s->position.inWorld())
                    continue;
                const bool now = (s->data[vm::objdata::Flags] & 0x200) != 0;
                if (auto [it, fresh] = burning.try_emplace(id, now); fresh ? now : it->second != now) {
                    std::printf("  tick %6llu %s %s %s at (%.2f, %.2f, %d)\n", static_cast<unsigned long long>(machine.ticks),
                                clockText(machine).c_str(), simName(*s).c_str(), now ? "catches fire" : "stops burning",
                                s->position.x, s->position.y, s->position.level);
                    it->second = now;
                }
                if (const int16_t npc = s->person->data[155]; npc != 0 && npcSeen[id] != s->guid()) {
                    npcSeen[id] = s->guid();
                    std::printf("  tick %6llu %s NPC %s (type %d) on the lot at (%.2f, %.2f, %d)\n",
                                static_cast<unsigned long long>(machine.ticks), clockText(machine).c_str(),
                                simName(*s).c_str(), npc, s->position.x, s->position.y, s->position.level);
                }
            }
        }
        // Bills the mailman brings (the "Bills" object, its attribute 0 the
        // amount: docs/primitives-mac.md §4e) and the lot and objects values
        // the bill is made from (globals 46/47 and 48/49).
        for (const auto& [oid, ob] : machine.objects())
            if (ob.guid() == kBillsGuid && !seenBills.count(oid)) {
                seenBills.insert(oid);
                std::printf("  tick %6llu %s bill %d: amount %d (globals 46 %d, 48 %d: 4 x 46 + 6 x 48 = %d)\n",
                            static_cast<unsigned long long>(machine.ticks), clockText(machine).c_str(), oid,
                            ob.attributes.empty() ? 0 : ob.attributes[0], machine.globals[46], machine.globals[48],
                            4 * machine.globals[46] + 6 * machine.globals[48]);
            }
        if (std::array<int16_t, 4> v{machine.globals[46], machine.globals[47], machine.globals[48], machine.globals[49]};
            v != values) {
            std::printf("  tick %6llu %s lot value %d, objects value %d (globals 46-49 %d %d %d %d)\n",
                        static_cast<unsigned long long>(machine.ticks), clockText(machine).c_str(), v[0] * 1000 + v[1],
                        v[2] * 1000 + v[3], v[0], v[1], v[2], v[3]);
            values = v;
        }
        if (const int32_t f = vm::familyFunds(machine, family); f != funds) {
            std::printf("  tick %6llu %s family funds %d -> %d (%+d)\n", static_cast<unsigned long long>(machine.ticks),
                        clockText(machine).c_str(), funds, f, f - funds);
            funds = f;
        }
        if (every > 0 && (i + 1) % every == 0) {
            std::printf("  motives at tick %ld (%s):\n", i + 1, clockText(machine).c_str());
            printMotivesHeader();
            for (vm::ObjectId id : sims)
                if (const vm::Object* s = machine.object(id); s && s->person)
                    printMotives(*s);
        }
    }
    vm::autonomyObserver() = nullptr;
    printRelationships(machine, sims, "at the end");

    std::printf("after %ld ticks (%s): family funds %d\n", ticks, clockText(machine).c_str(), funds);
    printMotivesHeader();
    for (vm::ObjectId id : sims)
        if (const vm::Object* s = machine.object(id); s && s->person)
            printMotives(*s);
    // Mains reset after an error (VM::resetAfterError; tools/soak.sh counts
    // the household's by "its Main was reset").
    auto printResets = [&](vm::ObjectId id) {
        if (auto r = machine.errorResets.find(id); r != machine.errorResets.end())
            std::printf("; its Main was reset %zu time%s after an error, first: %s", r->second.count, r->second.count == 1 ? "" : "s",
                        (verbose ? r->second.first : r->second.first.substr(0, r->second.first.find('\n'))).c_str());
    };
    for (vm::ObjectId id : sims) {
        const vm::Object* s = machine.object(id);
        if (!s || !s->person)
            continue;
        std::printf("  %-12s %zu searches, %zu picks", simName(*s).c_str(), searches[id], choices[id]);
        if (auto it = machine.threads.find(id); it != machine.threads.end()) {
            const vm::Thread& t = it->second;
            if (t.status == vm::Thread::Status::Failed)
                std::printf("; its Main died: %s", (verbose ? t.error : t.error.substr(0, t.error.find('\n'))).c_str());
            else if (!t.stack.empty())
                std::printf("; Main in %s #%u", t.stack.back().behavior->fileName.c_str(), t.stack.back().pc);
        }
        printResets(id);
        std::printf("\n");
    }
    // People who came in while running (visitors, NPCs): where they are and
    // what their Main does ("its Main died", not "Main failed": tools/soak.sh
    // counts the household's dead Mains by that).
    for (const auto& [id, o] : machine.objects()) {
        if (!o.person || std::find(sims.begin(), sims.end(), id) != sims.end())
            continue;
        std::printf("  also: %s at (%.2f, %.2f, %d)%s", simName(o).c_str(), o.position.x, o.position.y, o.position.level,
                    o.position.inWorld() ? "" : " (out of the world)");
        if (auto it = machine.threads.find(id); it != machine.threads.end()) {
            const vm::Thread& t = it->second;
            if (t.status == vm::Thread::Status::Failed)
                std::printf("; its Main died: %s", (verbose ? t.error : t.error.substr(0, t.error.find('\n'))).c_str());
            else if (!t.stack.empty())
                std::printf("; Main in %s #%u", t.stack.back().behavior->fileName.c_str(), t.stack.back().pc);
            printResets(id);
        } else {
            std::printf("; no thread");
        }
        std::printf("\n");
    }
    // S2PKG_VEHICLES=1: the vehicles left (carpools, school buses, NPC cars)
    // and what their Main does.
    // S2PKG_VEHICLES=<text>: the objects whose name has that text instead.
    if (const char* vf = std::getenv("S2PKG_VEHICLES"))
        for (const auto& [id, o] : machine.objects())
            if (o.definition && (std::string(vf) != "1" ? o.definition->fileName.find(vf) != std::string::npos
                                                        : (o.definition->fileName.rfind("Car - ", 0) == 0 ||
                                                           o.definition->fileName.find("Vehicle") != std::string::npos ||
                                                           o.definition->fileName.find("Bus") != std::string::npos))) {
                std::printf("  vehicle %d %s at (%.2f, %.2f, %d)", id, o.definition->fileName.c_str(), o.position.x,
                            o.position.y, o.position.level);
                if (auto it = machine.threads.find(id); it != machine.threads.end() && !it->second.stack.empty())
                    std::printf("; Main in %s #%u", it->second.stack.back().behavior->fileName.c_str(),
                                it->second.stack.back().pc);
                std::printf("\n");
            }
    if (!machine.missingPrimitives.empty()) {
        std::printf("missing primitives:");
        for (const auto& [op, n] : machine.missingPrimitives)
            std::printf(" %s %zu,", primitiveName(op).c_str(), n);
        std::printf("\n");
    }
    if (!machine.animations.missing.empty()) {
        std::printf("missing animations:");
        for (const auto& [name, n] : machine.animations.missing)
            std::printf(" \"%s\" %zu,", name.c_str(), n);
        std::printf("\n");
    }
    return 0;
}

// The saved mood of every Sim on the lots under a Neighborhoods folder,
// against the base game's UpdateMood of its saved old motive set (the set
// the last mood was computed from) plus person data 163 (vm/motives.hpp;
// docs/motives-mac.md §4).
int cmdMoodSweep(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    ResourceIndex index(resRoot(dir / "x"));
    vm::CodeLibrary code(&index);
    vm::VM machine(code);
    size_t sims = 0, exact = 0, close = 0, boosted = 0, unclamped = 0, moodMem = 0, fromCurrent = 0;
    float worst = 0;
    std::vector<fs::path> lots;
    for (const auto& e : fs::recursive_directory_iterator(dir))
        if (e.path().extension() == ".package" && e.path().parent_path().filename() == "Lots")
            lots.push_back(e.path());
    std::sort(lots.begin(), lots.end());
    for (const auto& lot : lots) {
        DbpfPackage pkg(lot);
        const LotObjects saved = LotObjects::load(pkg);
        for (const auto& st : saved.objects) {
            if (!st.personRead || st.personData.size() <= vm::persondata::MoodBooster)
                continue;
            ++sims;
            const auto& pd = st.personData;
            const auto& curves = vm::happyCurvesOfAge(machine, vm::curveAgeIndexOf(pd[vm::persondata::PersonAge]));
            std::array<float, vm::kMotiveCount> old{}, now{};
            for (size_t k = 0; k < vm::kMotiveCount; ++k)
                old[k] = st.motives2[k], now[k] = st.motives[k];
            const int16_t booster = pd[vm::persondata::MoodBooster];
            const float mood = vm::moodFrom(curves, old, pd[vm::persondata::MotivesNotInMood], booster);
            const float saved3 = st.motives[vm::motive::Mood];
            const bool same = mood == saved3, near = std::abs(mood - saved3) < 0.05f;
            exact += same;
            close += near;
            worst = std::max(worst, std::abs(mood - saved3));
            boosted += booster != 0;
            unclamped += saved3 > 100.0f || saved3 < -100.0f;
            moodMem += st.motives[vm::motive::MoodMem] == saved3;
            fromCurrent += vm::moodFrom(curves, now, pd[vm::persondata::MotivesNotInMood], booster) == saved3;
            if (!same || booster)
                std::printf("%s #%d: saved mood %.4f, computed %.4f%s (booster %d, old mood %.2f)\n",
                            lot.filename().string().c_str(), st.id, double(saved3), double(mood),
                            same ? "" : near ? " (close)" : " (DIFFERENT)", booster, double(st.motives2[3]));
        }
    }
    std::printf("%zu Sims on %zu lots: mood = UpdateMood(old set) + person data 163 exactly for %zu, within 0.05 for "
                "%zu (largest difference %.3g); %zu with a booster, %zu above 100 or below -100; Mood+Mem = Mood for "
                "%zu; from the current set %zu\n",
                sims, lots.size(), exact, close, double(worst), boosted, unclamped, moodMem, fromCurrent);
    return close == sims ? 0 : 1;
}

// The saved environment (motive 13) of every Sim on the lots under a
// Neighborhoods folder, against cTSEnvironmentScoreManager's score of its
// place in the loaded lot (vm/environment.hpp; docs/motives-mac.md §8).
// The light's term (0 to -20) comes from the lot's light maps made without
// a window (vm/lot_light.hpp, round n42), as the app's; with S2PKG_NO_LIGHT=1
// it is left out, and a saved value up to 20 under the room's score is the
// light. --verbose: every room's sums.
int cmdEnvSweep(const std::filesystem::path& dir, bool verbose) {
    namespace fs = std::filesystem;
    ResourceIndex index(resRoot(dir / "x"));
    vm::CodeLibrary code(&index);
    std::vector<fs::path> lots;
    for (const auto& e : fs::recursive_directory_iterator(dir))
        if (e.path().extension() == ".package" && e.path().parent_path().filename() == "Lots")
            lots.push_back(e.path());
    std::sort(lots.begin(), lots.end());
    size_t sims = 0, exact = 0, close = 0, within2 = 0, light = 0, other = 0, outdoors = 0, noRoom = 0;
    const bool withLight = !std::getenv("S2PKG_NO_LIGHT");
    for (const auto& file : lots) try {
        DbpfPackage pkg(file);
        const Lot lot = Lot::load(pkg);
        const LotObjects saved = LotObjects::load(pkg);
        vm::VM machine(code);
        const vm::LotLoad load = vm::loadLot(machine, lot, saved, vm::charactersDirOf(file));
        vm::startSavedLot(machine, load, saved); // the objects' saved data, motives, the simulator's counter
        // The lot's light maps, as the app makes them (vm/lot_light.hpp),
        // for the light's term; S2PKG_NO_LIGHT=1 leaves the term out.
        std::unique_ptr<vm::HeadlessLotLighting> lighting;
        if (withLight) {
            lighting = std::make_unique<vm::HeadlessLotLighting>(index, lot, pkg, resRoot(file), machine);
            lighting->install();
        }
        const vm::EnvironmentRooms rooms = vm::environmentRooms(machine);
        if (verbose) {
            const PatternScores& ps = patternScores(index);
            for (const auto& [ref, name] : lot.floorPatterns) {
                const PatternScore* p = ps.floor(name);
                std::printf("  floor pattern %d %-24s %s price %d niceness %g crap %g\n", ref, name.c_str(),
                            p ? "" : "(none)", p ? p->price : 0, p ? double(p->niceness) : 0.0, p ? double(p->crap) : 0.0);
            }
            for (const auto& [ref, name] : lot.wallPatterns) {
                const PatternScore* p = ps.wall(name);
                std::printf("  wall pattern %d %-24s %s price %d niceness %g crap %g\n", ref, name.c_str(),
                            p ? "" : "(none)", p ? p->price : 0, p ? double(p->niceness) : 0.0, p ? double(p->crap) : 0.0);
            }
        }
        if (verbose)
            for (const auto& [key, e] : rooms)
                std::printf("  key %6d (%s): walls %g / %g crap %g -> %.3f; floors %g / %g crap %g -> %.3f; objects %g (%g) "
                            "crap %g -> %.3f; base %.3f\n",
                            key, key >= vm::kEnvironmentIndoorBase ? "room" : "cell", double(e.wallNice),
                            double(e.wallCount), double(e.wallCrap), double(e.wallScore), double(e.floorNice),
                            double(e.floorArea), double(e.floorCrap), double(e.floorScore), double(e.objectNice),
                            double(e.objectCount), double(e.objectCrap), double(e.objectScore),
                            double(vm::baseEnvironmentScore(rooms, key)));
        if (verbose)
            for (const auto& [id, o] : machine.objects())
                if (!o.person && o.definition && o.position.inWorld())
                    std::printf("  object %d %-40s price %5d niceness %4d crap %3d room %3d lead %d type %d\n", id,
                                o.definition->fileName.c_str(), int16_t(o.definition->price()), o.data[7], o.data[86],
                                o.data[vm::objdata::Room], o.data[vm::objdata::LeadTileObjectId],
                                o.definition->objectType());
        std::map<int16_t, const LotObjects::State*> states;
        for (const auto& st : saved.objects)
            states[st.id] = &st;
        bool header = false;
        for (const auto& [id, o] : machine.objects()) {
            if (!o.person)
                continue;
            auto it = states.find(id);
            if (it == states.end() || !it->second->personRead)
                continue;
            const float savedEnv = it->second->motives[vm::motive::Environment];
            const float oldEnv = it->second->motives2[vm::motive::Environment];
            const float score = vm::environmentScore(machine, o, rooms);
            const int16_t room = o.position.inWorld() ? o.data[vm::objdata::Room] : int16_t(-1);
            const int32_t key = vm::environmentRoomId(room, o.position.x, o.position.y);
            ++sims;
            const float d = savedEnv - score;
            const bool same = std::abs(d) < 0.01f, near = std::abs(d) < 0.5f;
            const bool byLight = !withLight && !near && d < 0 && d >= -20.01f && score - 20.0f > -100.0f;
            exact += same;
            close += near;
            within2 += std::abs(d) < 2.0f;
            light += byLight;
            other += withLight ? std::abs(d) >= 2.0f : !near && !byLight;
            outdoors += key >= 0 && key < vm::kEnvironmentIndoorBase;
            noRoom += key < 0;
            if (!header) {
                std::printf("%s (simulator tick %u):\n", file.filename().string().c_str(), saved.simulator.fA0);
                header = true;
            }
            const std::string what = same ? "" : near ? " (close)" : byLight ? " (light?)" : " (DIFFERENT)";
            std::printf("  %-28s #%-4d room %3d key %6d at (%.1f, %.1f, %d): saved %8.3f (old %8.3f), room score %8.3f%s",
                        (o.definition ? o.definition->fileName : std::string("?")).c_str(), id, room, key,
                        double(o.position.x), double(o.position.y), o.position.level, double(savedEnv), double(oldEnv),
                        double(score), what.c_str());
            if (lighting)
                if (const auto lit = machine.floorIllumination(o.position.level, o.position.x, o.position.y))
                    std::printf(" (light %.3f)", double(*lit));
            if (byLight)
                std::printf(" -> illuminance %.3f", double(vm::envtuning::LightDark +
                                                           (vm::envtuning::LightBright - vm::envtuning::LightDark) *
                                                               (1.0f + d / (vm::envtuning::LightWeight * 100.0f))));
            std::printf("\n");
            if (verbose || (!near && key >= vm::kEnvironmentIndoorBase))
                if (auto r = rooms.find(key); r != rooms.end()) {
                    const auto& e = r->second;
                    std::printf("      walls %g / %g crap %g -> %.3f; floors %g / %g crap %g -> %.3f; objects %g (%g) crap %g -> %.3f\n",
                                double(e.wallNice), double(e.wallCount), double(e.wallCrap), double(e.wallScore),
                                double(e.floorNice), double(e.floorArea), double(e.floorCrap), double(e.floorScore),
                                double(e.objectNice), double(e.objectCount), double(e.objectCrap), double(e.objectScore));
                }
        }
    } catch (const std::exception& err) {
        std::printf("%s: %s\n", file.filename().string().c_str(), err.what());
    }
    if (withLight)
        std::printf("%zu Sims on %zu lots, with the light maps: the environment score of the loaded lot is the saved "
                    "environment for %zu (within 0.5 for %zu, within 2 for %zu), other %zu; %zu outdoors, %zu in no room\n",
                    sims, lots.size(), exact, close, within2, other, outdoors, noRoom);
    else
        std::printf("%zu Sims on %zu lots: the environment score of the loaded lot is the saved environment for %zu "
                    "(within 0.5 for %zu), up to 20 over it (the light's term) for %zu, other %zu; %zu outdoors, %zu in no room\n",
                    sims, lots.size(), exact, close, light, other, outdoors, noRoom);
    return 0;
}

} // namespace

int autonomyCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "mood-sweep" && argc == 3)
        return cmdMoodSweep(argv[2]);
    if (cmd == "env-sweep" && (argc == 3 || (argc == 4 && std::string(argv[3]) == "--verbose")))
        return cmdEnvSweep(argv[2], argc == 4);
    if (cmd != "autonomy" || argc < 3)
        return -1;
    std::string lotPath, sim;
    long ticks = 1800, every = 0;
    std::optional<int> motives;
    bool fromInit = false, verbose = false, noDecay = false;
    int traceId = 0;
    size_t traceDepth = 4;
    size_t fbaDump = 0;
    int fbaSim = 0;
    std::string fbaFilter;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--ticks" && i + 1 < argc)
            ticks = std::stol(argv[++i]);
        else if (a == "--every" && i + 1 < argc)
            every = std::stol(argv[++i]);
        else if (a == "--sim" && i + 1 < argc)
            sim = argv[++i];
        else if (a == "--motives" && i + 1 < argc)
            motives = std::stoi(argv[++i]);
        else if (a == "--init")
            fromInit = true;
        else if (a == "--verbose")
            verbose = true;
        else if (a == "--trace" && i + 1 < argc)
            traceId = std::stoi(argv[++i]);
        else if (a == "--trace-depth" && i + 1 < argc)
            traceDepth = std::stoul(argv[++i]);
        else if (a == "--fba" && i + 1 < argc)
            fbaDump = std::stoul(argv[++i]);
        else if (a == "--fba-sim" && i + 1 < argc)
            fbaSim = std::stoi(argv[++i]);
        else if (a == "--fba-match" && i + 1 < argc)
            fbaFilter = argv[++i];
        else if (a == "--no-decay")
            noDecay = true;
        else if (a == "--skip-failures")
            gSkipFailures = true;
        else if (a == "--dialog-answer" && i + 1 < argc)
            gDialogAnswer = std::stoi(argv[++i]);
        else if (a == "--name-babies")
            gNameBabies = true;
        else if (a == "--no-dialog-timeout")
            gNoDialogTimeout = true;
        else if (lotPath.empty())
            lotPath = a;
        else
            return -1;
    }
    if (lotPath.empty())
        return -1;
    return cmdAutonomy(lotPath, ticks, every, sim, motives, fromInit, verbose, noDecay, traceId, traceDepth, fbaDump, fbaSim, fbaFilter);
}

const char* autonomyUsage() {
    return "       s2pkg autonomy <lot.package> [--ticks N] [--every N] [--sim name] [--motives N] [--verbose] [--no-decay] [--trace sim-id [--trace-depth N]] [--fba N [--fba-sim id] [--fba-match text]] [--init] [--dialog-answer N] [--no-dialog-timeout] [--skip-failures] [--name-babies]\n"
           "       s2pkg mood-sweep <Neighborhoods dir>\n"
           "       s2pkg env-sweep <Neighborhoods dir> [--verbose]\n";
}

} // namespace os2
