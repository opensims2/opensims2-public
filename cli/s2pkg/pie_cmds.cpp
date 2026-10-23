// s2pkg pie: the pie menu a lot's Sim gets for an object, from the TTABs and
// check trees run in the VM (src/vm/interactions.hpp), and what happens when
// an entry is picked.

#include "pie_cmds.hpp"
#include "budget.hpp"
#include "data_index.hpp"

#include "formats/bhav.hpp"
#include "formats/binreader.hpp"
#include "formats/dbpf.hpp"
#include "formats/lot.hpp"
#include "formats/lot_objects.hpp"
#include "formats/lot_arrivals.hpp"
#include "formats/resindex.hpp"
#include "formats/ui_resources.hpp"
#include "vm/hood_bridge.hpp"
#include "vm/lot_neighborhood.hpp"
#include "vm/autonomy.hpp"
#include "vm/dialog.hpp"
#include "vm/engine.hpp"
#include "vm/go_here.hpp"
#include "vm/interactions.hpp"
#include "vm/lot.hpp"
#include "vm/lot_resume.hpp"
#include "vm/placement.hpp"
#include "vm/motives.hpp"
#include "vm/baby_character.hpp"
#include "vm/hood_save.hpp"
#include "formats/neighborhood.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace os2 {

namespace {

std::filesystem::path resRoot(const std::filesystem::path& pkg) {
    for (auto p = std::filesystem::absolute(pkg).parent_path(); p.has_relative_path(); p = p.parent_path())
        if (p.filename() == "Res")
            return p;
    // A lot in the player's folder: the game's data from $OS2_DATA (as lot-save).
    if (const char* data = std::getenv("OS2_DATA"); data && std::filesystem::is_directory(std::filesystem::path(data) / "TSData/Res"))
        return std::filesystem::path(data) / "TSData/Res";
    throw std::runtime_error("the lot is not under a TSData/Res directory (set OS2_DATA to data/game)");
}

const char* checkName(vm::CheckResult r) {
    switch (r) {
    case vm::CheckResult::NoGuard: return "no guard";
    case vm::CheckResult::True: return "true";
    case vm::CheckResult::False: return "false";
    case vm::CheckResult::Failed: return "failed";
    case vm::CheckResult::Running: return "running";
    }
    return "?";
}

std::string nameOf(const vm::Object& o) { return o.definition ? o.definition->fileName : "?"; }

int cmdPie(const std::filesystem::path& lotFile, const std::string& simQuery, const std::string& objectQuery,
           int select, int ticks, bool all, std::optional<int> motives, bool fromInit, size_t trace,
           int dialogAnswer, size_t traceChecks, bool anims, const std::vector<std::string>& creates,
           const std::string& birth, const std::string& userDir, const std::string& saveHood) {
    ResourceIndex index;
    s2pkgAddInstallation(index, resRoot(lotFile));
    vm::CodeLibrary code(&index);
    // Text/*.package's tables for the dialogs' $AspirationString and the babies' names.
    UiResources texts(index, resRoot(lotFile));
    vm::textTables() = [&texts](uint32_t group, uint32_t instance) { return texts.table(group, instance); };
    DbpfPackage pkg(lotFile);
    const Lot lot = Lot::load(pkg);
    LotObjects saved = LotObjects::load(pkg);
    // A family that just moved in comes by its first day's "reset lot", as
    // in the app and s2pkg autonomy (formats/lot_arrivals.hpp).
    const LotArrivals arrivals = addArrivingSims(saved, lot, lotFile, &index, false);
    vm::VM machine(code);
    machine.initCreatedObjects = true;
    machine.instructionsPerTurn = s2pkgBudget();
    machine.setClockGlobals();
    machine.autoDialogAnswer = dialogAnswer; // headless: every dialog gets this button
    const bool hood = vm::loadLotNeighborhood(machine, lotFile);
    const vm::LotLoad load = vm::loadLot(machine, lot, saved, vm::charactersDirOf(lotFile));
    if (hood)
        vm::bindLotSims(machine);
    // The saved state (motives, tree stacks, clock), or every Init with --init.
    vm::startSavedLot(machine, load, saved, fromInit);
    if (hood && arrivals.firstDay) {
        const vm::ResetLotResult r = vm::runResetLot(machine, arrivals.arriving);
        std::printf("first day: %zu \"reset lot\" trees, %zu Sims arrive\n", r.trees, r.sims.size());
    }
    vm::runLotLoaded(machine); // the lot is ready: "CT - Lot Loaded" (the move-in scene)
    if (motives)
        vm::setAllMotives(machine, int16_t(*motives));
    // S2PKG_NO_DECAY=1: test aid, the motives do not decay (a pregnancy runs
    // its three days without the mother starving), as autonomy --no-decay.
    if (std::getenv("S2PKG_NO_DECAY"))
        vm::gMotiveDecay = false;

    // --create GUID[:attr=value,...]: test aid, an object made out of the
    // world as Buy mode makes one (Init only, no Main), with those
    // attributes (e.g. a party controller for the birthday cinematic:
    // --create 0x2C91C16A:2=1).
    // "...@x,y[,level]" puts it on that tile, as the app's --vm-create (the
    // debug tombstone's "New Baby" places the baby beside it).
    for (const std::string& spec : creates) {
        const auto at = spec.find('@');
        const vm::ObjectId id = vm::createTestObject(machine, spec.substr(0, at));
        std::string where;
        int x = 0, y = 0, level = 1;
        if (at != std::string::npos && std::sscanf(spec.c_str() + at + 1, "%d,%d,%d", &x, &y, &level) >= 2) {
            const vm::Placement p = vm::checkPlacement(machine, id, {x, y, level}, 0);
            if (p.ok)
                vm::applyPlacement(machine, id, p), where = " (placed)";
            else
                where = " (not placed: " + p.reason + ")";
        }
        // "...!" at the end: its Main starts once it is placed, as an object a
        // tree makes (a Fire set on the stove's tile burns, wakes the room's
        // smoke alarm and the Sims panic).
        if (!spec.empty() && spec.back() == '!' && machine.startMain(id))
            where += ", Main started";
        std::printf("created %d: %s%s\n", id, nameOf(*machine.object(id)).c_str(), where.c_str());
    }
    // Babies' character files (Make New Character, vm/baby_character.hpp)
    // go to <--user-dir>/Characters, never into the game's data.
    if (!userDir.empty())
        vm::installBabyCharacterWriter(index, vm::charactersDirOf(lotFile), std::filesystem::path(userDir) / "Characters");
    // --birth mother,father (Sims' names or IDs): test aid, Make New Character
    // 0x13 with flag 0x01 (a baby of those two, as "Home Birth - Make
    // Baby(ies)" runs it), from a one-instruction tree run on the mother.
    if (!birth.empty()) {
        const auto comma = birth.find(',');
        const vm::ObjectId ma = vm::findSim(machine, birth.substr(0, comma));
        const vm::ObjectId pa = comma == std::string::npos ? 0 : vm::findSim(machine, birth.substr(comma + 1));
        if (!ma || !pa)
            throw std::runtime_error("--birth: no Sim " + birth);
        Behavior b;
        b.fileName = "Test - Make Baby";
        b.signature = 0x8009;
        b.argCount = 2;
        Behavior::Instruction in;
        in.opcode = 0x13;
        in.onTrue = {Behavior::TargetKind::True, 0};
        in.onFalse = {Behavior::TargetKind::False, 0};
        in.operands = {0, 0, 0, 0x09, 1, 0, 0x09, 0, 0, 0x01};
        b.instructions.push_back(in);
        const vm::Object& mother = *machine.object(ma);
        code.add(mother.group, 0x1FF0, std::move(b));
        std::array<int16_t, vm::kTempCount> temps{};
        temps[0] = 1; // one baby
        const std::array<int16_t, 2> params{ma, pa};
        const bool ok = vm::runOneTickTree(machine, temps, ma, ma, ma, 0x1FF0, params);
        std::printf("Make New Character (baby of %d and %d): %s;", ma, pa, ok ? "true" : "false");
        for (int16_t id : machine.object(ma)->array(0).values)
            if (const vm::Neighbor* n = machine.neighbor(id))
                std::printf(" neighbor %d \"%s %s\" (GUID 0x%08X, person data 58 %d, gender %d, family %d)", id,
                            n->firstName.c_str(), n->lastName.c_str(), n->guid,
                            n->personData[vm::persondata::PersonAge], n->personData[65],
                            n->personData[vm::persondata::FamilyNumber]);
        std::printf("\n");
    }
    const vm::ObjectId sim = vm::findSim(machine, simQuery);
    if (!sim)
        throw std::runtime_error("no Sim \"" + simQuery + "\" in the lot's world");
    std::printf("Sim %d: %s\n", sim, nameOf(*machine.object(sim)).c_str());
    // The queue as the saved lot left it (vm/lot_resume.hpp).
    if (const vm::Object* so = machine.object(sim); so && !so->queue.empty()) {
        std::printf("saved queue:");
        for (size_t i = 0; i < so->queue.size(); ++i)
            std::printf(" [%s on %d%s]", so->queue[i].name.c_str(), so->queue[i].object,
                        int(i) == so->activeInteraction ? ", running" : "");
        std::printf("\n");
    }

    if (objectQuery.empty()) {
        // Every object in the world with interactions, by interaction leader.
        std::map<vm::ObjectId, std::vector<vm::ObjectId>> leaders;
        for (const auto& [id, o] : machine.objects())
            if (o.position.inWorld() && !o.person)
                if (const vm::Object* l = machine.object(vm::interactionLeader(machine, id)); l && vm::interactionTable(machine, *l))
                    leaders[l->id].push_back(id);
        std::printf("%zu objects with interactions:\n", leaders.size());
        for (const auto& [id, parts] : leaders) {
            const vm::Object& o = *machine.object(id);
            const auto ref = vm::interactionTable(machine, o);
            std::printf("  %5d %-50s (%.1f, %.1f, %d) TTAB %08X/%X, %zu entries\n", id, nameOf(o).c_str(),
                        o.position.x, o.position.y, o.position.level, ref->group, ref->instance,
                        ref->table->interactions.size());
        }
        return 0;
    }

    // here:x,y[,level]: a click on the ground there (vm/go_here.hpp): the
    // destination object the game puts under the mouse.
    vm::ObjectId object = 0, merged = 0;
    if (objectQuery.rfind("here:", 0) == 0) {
        float hx = 0, hy = 0;
        int hl = 1;
        if (std::sscanf(objectQuery.c_str() + 5, "%f,%f,%d", &hx, &hy, &hl) < 2)
            throw std::runtime_error("here:x,y[,level] expected");
        object = vm::createGoHereDestination(machine, sim, hx, hy, hl);
        if (!object)
            throw std::runtime_error("no destination can stand at " + objectQuery.substr(5));
        const vm::Object& d = *machine.object(object);
        std::printf("go here: destination %d at (%.3f, %.3f, %d)\n", object, d.position.x, d.position.y,
                    d.position.level);
    } else if (const size_t at = objectQuery.find('@'); at != std::string::npos) {
        // object@x,y[,level]: a click on that object with the floor at
        // (x, y) under the mouse (a rug merges a go-here destination there).
        float hx = 0, hy = 0;
        int hl = 1;
        if (std::sscanf(objectQuery.c_str() + at + 1, "%f,%f,%d", &hx, &hy, &hl) < 2)
            throw std::runtime_error("object@x,y[,level] expected");
        const std::string name = objectQuery.substr(0, at);
        vm::ObjectId clicked = 0;
        if (!name.empty() && name.find_first_not_of("0123456789") == std::string::npos)
            clicked = vm::ObjectId(std::stoi(name));
        else
            clicked = vm::findInteractionObject(machine, name);
        if (!clicked || !machine.object(clicked))
            throw std::runtime_error("no object \"" + name + "\"");
        bool has = false;
        for (const auto& e : vm::pieMenu(machine, sim, clicked))
            has = has || (e.available() && !e.debug);
        const auto menu = vm::objectClick(machine, sim, clicked, vm::Position{hx, hy, hl}, has);
        object = menu.object, merged = menu.destination;
        std::printf("click on %d%s: menu of %d", clicked, vm::isGoHereRug(machine, clicked) ? " (a rug)" : "", object);
        if (merged)
            std::printf(" with go-here destination %d at (%.3f, %.3f, %d)", merged, machine.object(merged)->position.x,
                        machine.object(merged)->position.y, machine.object(merged)->position.level);
        std::printf("\n");
    } else {
        object = vm::findInteractionObject(machine, objectQuery);
    }
    if (!object)
        throw std::runtime_error("no object \"" + objectQuery + "\" with interactions in the lot's world");
    const vm::ObjectId leader = vm::interactionLeader(machine, object);
    std::printf("object %d: %s", object, nameOf(*machine.object(object)).c_str());
    if (leader != object)
        std::printf(" (interactions of %d: %s)", leader, nameOf(*machine.object(leader)).c_str());
    const auto ref = vm::interactionTable(machine, *machine.object(leader));
    std::printf("\nTTAB %08X/%X\n", ref->group, ref->instance);
    // --trace-checks N: the first N instructions of the entries' check trees.
    size_t checkTraced = 0;
    if (traceChecks)
        machine.trace = [&](const vm::Thread& t, const vm::Frame& f, const Behavior::Instruction& in) {
            if (t.me != sim || checkTraced++ >= traceChecks)
                return;
            const std::string op = in.opcode < 0x100 ? primitiveName(in.opcode) : "call " + hexString(in.opcode);
            std::printf("    %*s%08X %04X #%-3u %-28s %s %s\n", int(t.stack.size() - 1) * 2, "", f.group, f.instance, f.pc,
                        op.c_str(), f.behavior->fileName.c_str(), vm::describeOperands(machine, t, f, in).c_str());
        };
    auto pie = vm::pieMenu(machine, sim, object);
    if (merged) // a rug's menu with its destination's entries after (PickObject(rug, destination))
        for (auto& e : vm::pieMenu(machine, sim, merged))
            pie.push_back(std::move(e));
    machine.trace = nullptr;
    int n = 0;
    std::vector<const vm::PieEntry*> shown;
    for (const auto& e : pie) {
        if (!all && (e.debug || !e.available()))
            continue;
        shown.push_back(&e);
        std::printf("  [%d] TTA %-3u %-9s %s", n++, e.interaction, e.debug ? "debug" : e.wrongAge ? "age" : e.hidden ? "hidden" : checkName(e.check),
                    e.name.c_str());
        if (!e.error.empty())
            std::printf("  -- %s", e.error.substr(0, e.error.find('\n')).c_str());
        if (all && ref->table) // the TTAB entry's flags and trees
            for (const auto& x : ref->table->interactions)
                if (x.stringIndex == e.interaction) {
                    std::printf("  (flags %04X/%04X, action %04X, check %04X)", x.flags, x.flags2, x.action, x.guard);
                    break;
                }
        std::printf("\n");
    }
    std::printf("%zu of %zu entries shown%s\n", shown.size(), pie.size(),
                all ? "" : " (available, not debug; --all shows every entry)");
    if (select < 0)
        return 0;
    if (size_t(select) >= shown.size())
        throw std::runtime_error("--select: no entry " + std::to_string(select));
    const vm::PieEntry& e = *shown[size_t(select)];
    auto q = vm::interactionFromPie(machine, e); // keeps a dynamic entry's name and parameter 0
    q->pushedBy = sim;
    vm::Object& s = *machine.object(sim);
    vm::addAction(machine, s, *q);
    vm::releaseDestination(machine, object); // the pie menu closes (a go-here destination stays while queued)
    if (merged)
        vm::releaseDestination(machine, merged);
    std::printf("queued \"%s\" (TTA %u, action %04X) on %s; %d ticks:\n", q->name.c_str(), q->interaction, q->action,
                nameOf(s).c_str(), ticks);
    // --trace N: the first N instructions the Sim's thread runs from here on.
    size_t traced = 0;
    if (trace)
        machine.trace = [&](const vm::Thread& t, const vm::Frame& f, const Behavior::Instruction& in) {
            // S2PKG_TRACE_ALL=1: every thread, each line tagged with its object.
            static const bool everyThread = std::getenv("S2PKG_TRACE_ALL") != nullptr;
            // S2PKG_TRACE_ID=n: that object's threads instead; S2PKG_TRACE_FROM=tick: from then on.
            static const long traceId = std::getenv("S2PKG_TRACE_ID") ? std::atol(std::getenv("S2PKG_TRACE_ID")) : 0;
            static const long traceFrom = std::getenv("S2PKG_TRACE_FROM") ? std::atol(std::getenv("S2PKG_TRACE_FROM")) : 0;
            if (long(machine.ticks) < traceFrom)
                return;
            if ((!everyThread && t.me != (traceId ? vm::ObjectId(traceId) : sim)) || traced++ >= trace)
                return;
            const std::string op = in.opcode < 0x100 ? primitiveName(in.opcode) : "call " + hexString(in.opcode);
            if (everyThread)
                std::printf("[%d]", t.me);
            std::printf("    %*s%08X %04X #%-3u %-28s %s %s\n", int(t.stack.size() - 1) * 2, "", f.group, f.instance, f.pc,
                        op.c_str(), f.behavior->fileName.c_str(), vm::describeOperands(machine, t, f, in).c_str());
        };
    uint32_t lastDialog = 0;
    auto showDialog = [&](const vm::Dialog& d, int tick) {
        if (d.id <= lastDialog)
            return;
        lastDialog = d.id;
        std::printf("  tick %d: dialog %d \"%s\" from %d (%s): \"%s\"\n      \"%s\"\n", tick, d.type,
                    d.typeName.c_str(), d.me, machine.object(d.me) ? nameOf(*machine.object(d.me)).c_str() : "gone", d.title.c_str(), d.message.c_str());
        for (size_t b = 0; b < d.buttons.size(); ++b)
            std::printf("      [%zu] %s%s\n", b, d.buttons[b].c_str(), int(b) == d.answer ? "  <- answered" : "");
        // The special dialogs' class, rows and names (vm/dialog.hpp).
        if (d.cls.cls != vm::DialogClass::Object)
            std::printf("      class %s (script 0x%08X)\n", d.cls.name, d.cls.script);
        for (const auto& e : d.entries)
            std::printf("      row %d %s%s%s\n", e.id, e.name.c_str(), e.detail.empty() ? "" : (" / " + e.detail).c_str(),
                        std::find(d.selection.begin(), d.selection.end(), e.id) != d.selection.end() ? "  <- chosen" : "");
        for (const auto& n : d.names)
            std::printf("      name box: neighbor %d \"%s\"%s\n", n.neighbor, n.text.c_str(), n.female ? " (girl)" : "");
        if (d.cls.cls == vm::DialogClass::ClothingPurchase || d.cls.cls == vm::DialogClass::ClothingTryOn ||
            d.cls.cls == vm::DialogClass::Clothing || d.cls.cls == vm::DialogClass::Vanity)
            std::printf("      Sim %d, outfit category 0x%03X%s", d.outfitSim, d.outfitCategory,
                        d.outfitSeparates ? " (tops, bottoms and whole outfits)" : "");
        if (d.cls.cls == vm::DialogClass::ClothingPurchase)
            std::printf(", %d per item, funds %d", d.pricePerItem, d.fillFunds);
        if (d.cls.cls == vm::DialogClass::ClothingPurchase || d.cls.cls == vm::DialogClass::ClothingTryOn ||
            d.cls.cls == vm::DialogClass::Clothing || d.cls.cls == vm::DialogClass::Vanity)
            std::printf("\n");
    };
    std::map<vm::ObjectId, std::string> lastAnim, lastOverlays;
    // S2PKG_FBA=text: each Find Best Interaction of anyone, its pick and the
    // candidates whose name has the text (static score, check, score).
    if (const char* fba = std::getenv("S2PKG_FBA"))
        vm::autonomyObserver() = [&machine, want = std::string(fba)](const vm::AutonomyDecision& d) {
            std::printf("  tick %llu: %d Find Best Interaction: %s", static_cast<unsigned long long>(machine.ticks), d.sim,
                        d.best.empty() ? "nothing" : d.best.front().name.c_str());
            for (const auto& c : d.considered)
                if (c.name.find(want) != std::string::npos)
                    std::printf("; %s on %d %.3e %s %.3e", c.name.c_str(), c.object, c.staticScore,
                                !c.checked ? "unchecked" : c.passed ? "pass" : "FAIL", c.score);
            std::printf("\n");
        };
    std::map<vm::ObjectId, int16_t> lastAge;
    // S2PKG_PIE_ALSO="sim,object,entry,tick[;sim,object,entry,tick...]": at
    // that tick another Sim also picks that entry (the available ones,
    // numbered as listed; a negative entry lists them) - for socials towards
    // a busy Sim, or both partners in a bed.
    std::string also = std::getenv("S2PKG_PIE_ALSO") ? std::getenv("S2PKG_PIE_ALSO") : "";
    std::map<vm::ObjectId, std::string> lastQueue; // with S2PKG_PIE_ALSO: every Sim's queue as it changes
    const bool showQueues = !also.empty() || std::getenv("S2PKG_SHOW_QUEUES");
    // OS2_SEED=N: another seed for the VM's random numbers (a try for baby
    // that conceives, ...), as s2pkg autonomy has it.
    if (const char* seed = std::getenv("OS2_SEED"))
        machine.random.seed(uint32_t(std::strtoul(seed, nullptr, 10)));
    // Objects made during the run and gone during it, by name (S2PKG_SHOW_CREATED=1).
    static const bool showCreated = std::getenv("S2PKG_SHOW_CREATED") != nullptr;
    std::map<vm::ObjectId, std::string> known;
    if (showCreated)
        for (const auto& [oid, ob] : machine.objects())
            known[oid] = nameOf(ob);
    for (int i = 0; i < ticks; ++i) {
        if (showCreated) {
            for (const auto& [oid, ob] : machine.objects())
                if (auto [it, fresh] = known.emplace(oid, nameOf(ob)); fresh || it->second != nameOf(ob)) {
                    it->second = nameOf(ob);
                    std::printf("  tick %d: object %d \"%s\" made at (%.2f, %.2f)\n", i, oid, it->second.c_str(), ob.position.x, ob.position.y);
                }
            // and the family's funds as they change (bills paid, pay).
            static int32_t lastFunds = vm::familyFunds(machine, machine.globals[vm::global::CurrentFamily]);
            if (const int32_t f = vm::familyFunds(machine, machine.globals[vm::global::CurrentFamily]); f != lastFunds) {
                std::printf("  tick %d: family funds %d -> %d (%+d)\n", i, lastFunds, f, f - lastFunds);
                lastFunds = f;
            }
            // S2PKG_WATCH=name: where objects of that name move (tile and level).
            static const char* watch = std::getenv("S2PKG_WATCH");
            static std::map<vm::ObjectId, std::string> lastAt;
            if (watch)
                for (const auto& [oid, ob] : machine.objects())
                    if (nameOf(ob).find(watch) != std::string::npos) {
                        char buf[128];
                        // A Sim's room and person data 34 "Greet Status" too (a visitor: -1 not
                        // to be greeted, 0 waiting at the door, > 0 greeted).
                        std::snprintf(buf, sizeof buf, "(%.2f, %.2f, %d) dir %d in %d/%d%s", ob.position.x, ob.position.y, int(ob.position.level),
                                      int(ob.data[vm::objdata::Direction]), int(ob.data[vm::objdata::ContainerId]),
                                      int(ob.data[vm::objdata::SlotNumber]),
                                      ob.person ? (" room " + std::to_string(ob.data[vm::objdata::Room]) + " greet " +
                                                   std::to_string(ob.person->data[34])).c_str() : "");
                        if (lastAt[oid] != buf) {
                            lastAt[oid] = buf;
                            std::printf("  tick %d: %d \"%s\" at %s\n", i, oid, nameOf(ob).c_str(), buf);
                        }
                    }
            for (auto it = known.begin(); it != known.end();)
                if (!machine.object(it->first)) {
                    std::printf("  tick %d: object %d \"%s\" gone\n", i, it->first, it->second.c_str());
                    it = known.erase(it);
                } else
                    ++it;
        }
        if (showQueues)
            for (const auto& [oid, ob] : machine.objects()) {
                if (!ob.person)
                    continue;
                std::string text;
                for (size_t k = 0; k < ob.queue.size(); ++k)
                    text += " [" + ob.queue[k].name + (int(k) == ob.activeInteraction ? "*" : "") + "]";
                if (text != lastQueue[oid]) {
                    lastQueue[oid] = text;
                    std::printf("  tick %d: queue of %d:%s\n", i, oid, text.empty() ? " empty" : text.c_str());
                }
            }
        // Several picks: "sim,object,entry,tick;sim,object,entry,tick;..." in tick order.
        while (!also.empty()) {
            const size_t semi = also.find(';');
            const std::string one = also.substr(0, semi);
            std::vector<std::string> f;
            for (size_t a = 0, b; a <= one.size(); a = b + 1) {
                b = one.find(',', a);
                if (b == std::string::npos)
                    b = one.size();
                f.push_back(one.substr(a, b - a));
            }
            if (f.size() == 4 && std::stoi(f[3]) > i)
                break;
            if (f.size() == 4 && std::stoi(f[3]) == i) {
                const vm::ObjectId other = vm::findSim(machine, f[0]);
                const vm::ObjectId obj = vm::findInteractionObject(machine, f[1]);
                // "@name": the menu as autonomy sees it (check trees with
                // parameter 0 = 1, the '*' entries too), e.g. the bed's
                // "*Autonomous subqueue" with its "Try For Baby" string.
                const bool asAutonomy = !f[2].empty() && f[2][0] == '@';
                if (asAutonomy)
                    f[2] = f[2].substr(1);
                vm::PieOptions po;
                po.autonomous = asAutonomy;
                // S2PKG_ALSO_TRACE=text: the check trees whose entry tree's name has it, traced.
                static const char* alsoTrace = std::getenv("S2PKG_ALSO_TRACE");
                std::string tracing;
                if (alsoTrace)
                    machine.trace = [&](const vm::Thread& t, const vm::Frame& f, const Behavior::Instruction& in) {
                        if (t.stack.empty() || (t.stack.front().behavior->fileName.find(alsoTrace) == std::string::npos && f.behavior->fileName.find(alsoTrace) == std::string::npos))
                            return;
                        const std::string op = in.opcode < 0x100 ? primitiveName(in.opcode) : "call " + hexString(in.opcode);
                        std::printf("    %*s%08X %04X #%-3u %-28s %s %s\n", int(t.stack.size() - 1) * 2, "", f.group, f.instance, f.pc,
                                    op.c_str(), f.behavior->fileName.c_str(), vm::describeOperands(machine, t, f, in).c_str());
                    };
                std::vector<vm::PieEntry> menu = other && obj ? vm::pieMenu(machine, other, obj, po) : std::vector<vm::PieEntry>{};
                if (alsoTrace)
                    machine.trace = nullptr;
                // "!name": also entries the check tree hid from the menu (Hide
                // Menu) but passed, e.g. the bed's "Tentar Fazer um Bebê".
                const bool withHidden = !f[2].empty() && f[2][0] == '!';
                if (withHidden)
                    f[2] = f[2].substr(1);
                std::vector<const vm::PieEntry*> ok;
                for (const auto& e2 : menu)
                    if (!e2.debug && (e2.available() || (withHidden && e2.hidden && !e2.wrongAge &&
                                                         (e2.check == vm::CheckResult::NoGuard || e2.check == vm::CheckResult::True))))
                        ok.push_back(&e2);
                // The entry: its number, or a part of its name ("Relaxar").
                const bool numeric = !f[2].empty() && (std::isdigit(static_cast<unsigned char>(f[2][0])) || f[2][0] == '-');
                size_t pick = ok.size();
                if (numeric && std::stoi(f[2]) >= 0)
                    pick = size_t(std::stoi(f[2]));
                // "name#2": the second entry with that name (the B side of a pair).
                std::string want = f[2];
                int nth = 1;
                if (const size_t hash = want.rfind('#'); !numeric && hash != std::string::npos && hash + 1 < want.size()) {
                    nth = std::max(1, std::atoi(want.c_str() + hash + 1));
                    want = want.substr(0, hash);
                }
                for (size_t k = 0; !numeric && k < ok.size() && pick == ok.size(); ++k)
                    if (ok[k]->name.find(want) != std::string::npos && --nth == 0)
                        pick = k;
                if ((numeric && std::stoi(f[2]) < 0) || (!numeric && pick == ok.size())) { // lists the menu at that tick
                    for (size_t k = 0; k < ok.size(); ++k)
                        std::printf("  tick %d: %d's menu [%zu] %s\n", i, other, k, ok[k]->name.c_str());
                    for (const auto& e2 : menu) // and what is not offered, and why
                        if (!e2.available())
                            std::printf("  tick %d: %d's menu (not offered: %s%s%s) %s %s\n", i, other,
                                        e2.hidden ? "hidden" : e2.wrongAge ? "age" : "guard",
                                        e2.debug ? ", debug" : "", e2.error.empty() ? "" : (", " + e2.error).c_str(),
                                        e2.name.c_str(), "");
                }
                if (size_t k = pick; k < ok.size()) {
                    auto q2 = vm::interactionFromPie(machine, *ok[k]);
                    q2->pushedBy = other;
                    vm::addAction(machine, *machine.object(other), *q2);
                    std::printf("  tick %d: %d also queued \"%s\"\n", i, other, q2->name.c_str());
                }
            }
            also = semi == std::string::npos ? std::string() : also.substr(semi + 1);
        }
        const bool before = s.activeInteraction >= 0;
        // S2PKG_HOLD_MOTIVES=n: test aid, every Sim's motives back at n each
        // game hour (a pregnancy runs its three days without anyone starving).
        static const char* hold = std::getenv("S2PKG_HOLD_MOTIVES");
        if (hold && i % 1800 == 0)
            vm::setAllMotives(machine, int16_t(std::atoi(hold)));
        machine.tick();
        for (const auto& [who, d] : machine.dialogs)
            showDialog(d, i + 1);
        for (const auto& d : machine.notifications)
            showDialog(d, i + 1);
        // Cinematics the trees start (Cinematic 0x7B), and ages changing (Age 0x75).
        for (auto& r : vm::cinematicRequests()) {
            std::printf("  tick %d: cinematic \"%s\" from %d, actors", i + 1, r.scene.c_str(), r.me);
            for (vm::ObjectId a : r.actors)
                std::printf(" %d", a);
            std::printf("%s\n", r.mirrored() ? ", mirrored" : "");
        }
        vm::cinematicRequests().clear();
        for (const auto& [oid, ob] : machine.objects())
            if (ob.person) {
                const int16_t age = ob.person->data[vm::persondata::PersonAge];
                auto [it, fresh] = lastAge.emplace(oid, age);
                if (!fresh && it->second != age) {
                    std::printf("  tick %d: %d %s ages: person data 58 %d -> %d\n", i + 1, oid, nameOf(ob).c_str(),
                                it->second, age);
                    it->second = age;
                }
            }
        vm::Object* now = machine.object(sim);
        if (!now)
            break;
        // --anims: every Sim's full-body animation as it changes (both sides of a social).
        if (anims)
            for (const auto& [oid, ob] : machine.objects())
                if (ob.person && ob.animation.name != lastAnim[oid]) {
                    lastAnim[oid] = ob.animation.name;
                    std::printf("  tick %d: %d %s plays \"%s\"@%d%s at (%.2f, %.2f) dir %d in %d/%d\n", i + 1, oid,
                                nameOf(ob).c_str(), ob.animation.name.c_str(), int(ob.animation.level),
                                ob.animation.flipped && ob.animation.playing() ? " flipped" : "", ob.position.x, ob.position.y, ob.data[vm::objdata::Direction],
                                ob.data[vm::objdata::ContainerId], ob.data[vm::objdata::SlotNumber]);
                }
        // --anims: and the overlays (carry poses, partial animations) with their levels.
        if (anims)
            for (const auto& [oid, ob] : machine.objects())
                if (ob.person) {
                    std::string list;
                    for (const auto& o : ob.animation.overlays)
                        list += (list.empty() ? "" : ", ") + o.name + "@" + std::to_string(o.level) +
                                (o.flipped ? " flipped" : "");
                    if (list != lastOverlays[oid]) {
                        lastOverlays[oid] = list;
                        std::printf("  tick %d: %d %s overlays [%s]\n", i + 1, oid, nameOf(ob).c_str(), list.c_str());
                    }
                }
        if (!before && now->activeInteraction >= 0)
            std::printf("  tick %d: running \"%s\"\n", i + 1, now->queue[size_t(now->activeInteraction)].name.c_str());
        if ((i + 1) % 60 == 0)
            std::printf("  tick %d: at (%.2f, %.2f, %d)%s, hunger %d\n", i + 1, now->position.x, now->position.y,
                        now->position.level, now->route ? ", walking" : "", now->person ? now->person->motives[7] : 0);
    }
    if (auto it = machine.threads.find(sim); it != machine.threads.end() && it->second.status == vm::Thread::Status::Failed)
        std::printf("  the Sim's Main failed: %s\n", it->second.error.substr(0, it->second.error.find('\n')).c_str());
    for (int16_t hand = 0; hand < 2; ++hand)
        if (const vm::ObjectId held = machine.containedIn(sim, hand)) {
            const vm::Object& h = *machine.object(held);
            std::printf("  in hand %d: %d %s; semi-attributes", hand, held, nameOf(h).c_str());
            for (int16_t v : h.semiAttributes)
                std::printf(" %d", v);
            std::printf("\n");
        }
    // Lights Change Light set (vm/dialog.hpp), of the object and its parts.
    for (const auto& [id, o] : machine.objects())
        for (const auto& [light, st] : o.lights)
            std::printf("  light of %d %s%s%s: %d%%\n", id, nameOf(o).c_str(), light.empty() ? "" : " / ",
                        light.c_str(), st.level(machine.ticks));
    if (!machine.animations.missing.empty()) {
        std::printf("  missing animations:");
        for (const auto& [name, n] : machine.animations.missing)
            std::printf(" \"%s\" %zu,", name.c_str(), n);
        std::printf("\n");
    }
    // --save-hood <out.package>: what the run changed in the neighborhood
    // (FAMI, SDSC, SREL, the Sims born: vm/hood_save.hpp), to a new file.
    if (!saveHood.empty()) {
        const std::filesystem::path src = vm::neighborhoodPackageOf(lotFile);
        const hood::Neighborhood h = hood::Neighborhood::load(src, true);
        const vm::HoodSaveResult r = vm::saveNeighborhood(machine, h, src, saveHood);
        std::printf("neighborhood saved to %s: %zu families, %zu descriptions, relationships %zu changed, %zu new, "
                    "%zu Sims born\n",
                    saveHood.c_str(), r.families, r.descriptions, r.relationshipsChanged, r.relationshipsNew, r.born);
        for (const auto& w : r.warnings)
            std::printf("  warning: %s\n", w.c_str());
    }
    std::printf("queue of %s:", nameOf(s).c_str());
    for (size_t i = 0; i < s.queue.size(); ++i)
        std::printf(" [%s%s]", s.queue[i].name.c_str(), int(i) == s.activeInteraction ? ", running" : "");
    std::printf("%s\n", s.queue.empty() ? " empty" : "");
    // Sims whose Main died (an error the trees hit), with where.
    for (const auto& [id, o] : machine.objects())
        if (o.person)
            if (auto it = machine.threads.find(id); it != machine.threads.end() && it->second.status == vm::Thread::Status::Failed)
                std::printf("Main of %d %s died: %s\n", id, nameOf(o).c_str(), it->second.error.c_str());
    return 0;
}

} // namespace

int pieCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd != "pie" || argc < 3)
        return -1;
    std::vector<std::string> pos;
    int select = -1, ticks = 30;
    size_t trace = 0, traceChecks = 0;
    int dialogAnswer = 0;
    std::optional<int> motives;
    bool fromInit = false;
    bool all = false, anims = false;
    std::vector<std::string> creates;
    std::string birth, userDir, saveHood;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--select" && i + 1 < argc)
            select = std::stoi(argv[++i]);
        else if (a == "--ticks" && i + 1 < argc)
            ticks = std::stoi(argv[++i]);
        else if (a == "--init")
            fromInit = true;
        else if (a == "--motives" && i + 1 < argc)
            motives = std::stoi(argv[++i]);
        else if (a == "--trace" && i + 1 < argc)
            trace = std::stoul(argv[++i]);
        else if (a == "--trace-checks" && i + 1 < argc)
            traceChecks = std::stoul(argv[++i]);
        else if (a == "--dialog-answer" && i + 1 < argc)
            dialogAnswer = std::stoi(argv[++i]);
        else if (a == "--all")
            all = true;
        else if (a == "--anims")
            anims = true;
        else if (a == "--create" && i + 1 < argc)
            creates.push_back(argv[++i]);
        else if (a == "--birth" && i + 1 < argc)
            birth = argv[++i];
        else if (a == "--user-dir" && i + 1 < argc)
            userDir = argv[++i];
        else if (a == "--save-hood" && i + 1 < argc)
            saveHood = argv[++i];
        else
            pos.push_back(a);
    }
    if (pos.empty() || pos.size() > 3)
        return -1;
    return cmdPie(pos[0], pos.size() > 1 ? pos[1] : "", pos.size() > 2 ? pos[2] : "", select, ticks, all, motives, fromInit,
                  trace, dialogAnswer, traceChecks, anims, creates, birth, userDir, saveHood);
}

const char* pieUsage() {
    return "       s2pkg pie <lot.package> [sim [object]] [--all] [--select N [--ticks N] [--trace N] [--dialog-answer N] [--anims]] [--trace-checks N] [--motives N] [--init] [--create GUID[:attr=value,...]] (object here:x,y[,level] = a click on the ground; object@x,y[,level] = a click on it over that floor point) [--birth mother,father] [--user-dir dir] [--save-hood out.package]\n";
}

} // namespace os2
