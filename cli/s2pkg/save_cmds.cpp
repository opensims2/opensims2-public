// s2pkg lot-save: a lot loaded into the VM, run and written back (vm/lot_save.hpp).

#include "save_cmds.hpp"
#include "budget.hpp"
#include "data_index.hpp"

#include "build_cmds.hpp"
#include "buy_cmds.hpp"

#include "formats/binreader.hpp"
#include "formats/dbpf.hpp"
#include "formats/lot.hpp"
#include "formats/lot_arrivals.hpp"
#include "formats/lot_objects.hpp"
#include "formats/lot_write.hpp"
#include "formats/neighborhood.hpp"
#include "vm/hood_bridge.hpp"
#include "formats/resindex.hpp"
#include "vm/lot.hpp"
#include "vm/lot_resume.hpp"
#include "vm/lot_save.hpp"
#include "vm/save_types.hpp"
#include "vm/lot_records.hpp"
#include "vm/hood_save.hpp"
#include "vm/baby_character.hpp"
#include "vm/dialog.hpp"
#include "vm/engine.hpp"
#include "vm/interactions.hpp"
#include "formats/ui_resources.hpp"
#include "formats/bhav.hpp"
#include "app/game_save.hpp"
#include <set>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <cstdlib>
#include <string>
#include <vector>

namespace os2 {

namespace {

namespace fs = std::filesystem;

fs::path resRootOfLot(const fs::path& pkg) {
    for (auto p = fs::absolute(pkg).parent_path(); p.has_relative_path(); p = p.parent_path())
        if (p.filename() == "Res")
            return p;
    // A lot in the player's folder (app/user_data.hpp): the game's data from $OS2_DATA.
    if (const char* data = std::getenv("OS2_DATA"); data && fs::is_directory(fs::path(data) / "TSData/Res"))
        return fs::path(data) / "TSData/Res";
    throw std::runtime_error("the lot is not under a TSData/Res directory (set OS2_DATA to data/game)");
}

// A VM holding the lot in `package`, resumed from its saved state; the Sims'
// character files come from `charactersDir`.
struct LoadedLot {
    std::unique_ptr<vm::VM> vm;
    // The lot as read: the placement tests' geometry (VM::placementLot).
    std::unique_ptr<Lot> lot;
    LotObjects saved;
    vm::LotLoad load;
    vm::ResumeResult resumed;
};

// As lot-run: the lot's neighbourhood (`source` names it), entered, and the
// Sims bound to their neighbors.
LoadedLot loadInto(vm::CodeLibrary& code, const fs::path& package, const fs::path& source,
                   const hood::Neighborhood* hood, const fs::path& charactersDir, bool movedIn = false,
                   bool loadEntries = true) {
    LoadedLot out;
    DbpfPackage pkg(package);
    out.lot = std::make_unique<Lot>(Lot::load(pkg));
    const Lot& lot = *out.lot;
    out.saved = LotObjects::load(pkg);
    // As the app's lot load (app/lot_vm.cpp): a family that just moved in
    // arrives and pays the lot's price, and its first day starts
    // (formats/lot_arrivals.hpp).
    // movedIn: the lot was saved after the family's first day started, but
    // the neighborhood was not (lot-save --check): the game's save drops the
    // family's "new lot" flag with it (vm/hood_save.hpp), so the move-in
    // charge and the first day's clock are not applied again.
    LotArrivals arrivals;
    if (hood) {
        const LotObjects before = movedIn ? out.saved : LotObjects{};
        arrivals = addArrivingSims(out.saved, lot, source, code.index(), false);
        // The same for an evicted family's lot: the save clears the lot's
        // "evicted" flag (vm/hood_save.cpp), so the reload does not clean
        // up after the eviction again (N001_Lot80: the global sim objects
        // the first load made were thrown away and made again, without the
        // objects they had made).
        if (movedIn && (arrivals.evictedSims || arrivals.evictedObjects) && !arrivals.family) {
            out.saved = before;
            arrivals.evictedSims = arrivals.evictedObjects = 0;
        }
        if (movedIn) {
            out.saved.globals = before.globals;
            out.saved.simulator = before.simulator;
            arrivals.charge = 0;
            arrivals.firstDay = false;
        }
    }
    out.vm = std::make_unique<vm::VM>(code);
    out.vm->placementLot = out.lot.get();
    vm::VM& m = *out.vm;
    m.initCreatedObjects = true;
    m.runLoadEntries = loadEntries;
    m.instructionsPerTurn = s2pkgBudget();
    m.setClockGlobals();
    if (hood) {
        vm::loadNeighborhood(m, *hood);
        if (arrivals.family && arrivals.charge) {
            const auto f = int16_t(arrivals.family);
            vm::setFamilyFunds(m, f, int32_t(int64_t(vm::familyFunds(m, f)) - arrivals.charge));
        }
        vm::enterLot(m, vm::lotIdOf(source), hood->info ? int16_t(hood->info->id) : int16_t(0));
    }
    out.load = vm::loadLot(m, lot, out.saved, charactersDir);
    if (std::getenv("S2PKG_LOAD_ERRORS"))
        std::printf("  load: %zu states, %zu objects in the VM\n", out.saved.objects.size(), m.objects().size());
    if (std::getenv("S2PKG_LOAD_ERRORS"))
        for (const auto& e : out.load.objects)
            if (!e.error.empty())
                std::printf("  load: object %d (%s): %s\n", e.id, e.name.c_str(), e.error.c_str());
    if (hood)
        vm::bindLotSims(m);
    out.resumed = vm::startSavedLot(m, out.load, out.saved);
    if (out.resumed.globalsMade)
        std::printf("  global sim objects made at load: %zu\n", out.resumed.globalsMade);
    // The first day: the game's "reset lot" broadcast brings the family's
    // Sims (vm/lot.hpp runResetLot).
    if (hood && arrivals.firstDay) {
        const vm::ResetLotResult r = vm::runResetLot(m, arrivals.arriving);
        std::printf("  first day: %zu \"reset lot\" trees, %zu Sims arrived\n", r.trees, r.sims.size());
        for (const auto& e : r.errors)
            std::printf("  warning: reset lot: %s\n", e.c_str());
    }
    return out;
}

int cmdLotSave(const std::vector<std::string>& args) {
    std::vector<fs::path> paths;
    long ticks = 0, continueTicks = 0;
    bool check = false;
    std::vector<std::pair<std::string, std::string>> buyEdits; // --buy/--sell/--move (buy_cmds.hpp)
    std::vector<std::pair<std::string, std::string>> buildEdits; // --wall/--floor/... (build_cmds.hpp)
    for (size_t i = 0; i < args.size(); ++i) {
        if ((args[i] == "--buy" || args[i] == "--sell" || args[i] == "--move") && i + 1 < args.size())
            buyEdits.emplace_back(args[i], args[i + 1]), ++i;
        else if (isBuildEdit(args[i]) && i + 1 < args.size())
            buildEdits.emplace_back(args[i], args[i + 1]), ++i;
        else if (args[i] == "--ticks" && i + 1 < args.size())
            ticks = std::stol(args[++i]);
        else if (args[i] == "--check")
            check = true;
        else if (args[i] == "--continue" && i + 1 < args.size())
            check = true, continueTicks = std::stol(args[++i]);
        else
            paths.emplace_back(args[i]);
    }
    if (paths.size() != 2)
        throw std::runtime_error("usage: s2pkg lot-save <lot.package> <out.package> [--ticks N] [--check] [--continue N]");
    const fs::path& src = paths[0];
    const fs::path& dst = paths[1];
    if (fs::exists(dst) && fs::equivalent(src, dst))
        throw std::runtime_error("refusing to write over the lot itself");
    const fs::path res = resRootOfLot(src);
    const fs::path game = fs::weakly_canonical(res.parent_path().parent_path()); // Res -> TSData -> the game
    if (fs::weakly_canonical(fs::absolute(dst)).string().starts_with(game.string()))
        throw std::runtime_error("refusing to write into the game's data: pick an output outside " + game.string());
    ResourceIndex index;
    s2pkgAddInstallation(index, res);
    vm::CodeLibrary code(&index);
    const fs::path characters = vm::charactersDirOf(src);
    std::optional<hood::Neighborhood> hood;
    if (const auto hoodPackage = vm::neighborhoodPackageOf(src); !hoodPackage.empty())
        hood = hood::Neighborhood::load(hoodPackage, true);
    const hood::Neighborhood* h = hood ? &*hood : nullptr;
    LoadedLot a = loadInto(code, src, src, h, characters);
    if (!buyEdits.empty() && !applyBuyEdits(*a.vm, index, buyEdits))
        throw std::runtime_error("a Buy-mode edit failed");
    // Build-mode edits change the lot's geometry, written below.
    std::optional<Lot> built;
    if (!buildEdits.empty()) {
        DbpfPackage pkg(src);
        built = Lot::load(pkg);
        if (!applyBuildEdits(*a.vm, *built, index, buildEdits))
            throw std::runtime_error("a Build-mode edit failed");
    }
    for (long i = 0; i < ticks; ++i)
        a.vm->tick();
    // Save types of objects the run may create, from the other lots' tables.
    std::map<uint32_t, LotObjects::SaveType> more;
    for (const auto& f : fs::directory_iterator(src.parent_path()))
        if (f.path().extension() == ".package" && !fs::equivalent(f.path(), src)) {
            DbpfPackage other(f.path());
            for (auto& [g, t] : LotObjects::loadSaveTypes(other))
                more.emplace(g, std::move(t));
        }
    vm::CaptureResult captured = vm::captureLot(*a.vm, a.saved, &more);
    for (const auto& w : captured.warnings)
        std::printf("  warning: %s\n", w.c_str());
    vm::RecordsResult records;
    {
        DbpfPackage pkg(src);
        const Lot world = Lot::load(pkg);
        // cObjects for created objects are copied from the other lots.
        std::vector<fs::path> others;
        for (const auto& d : fs::directory_iterator(res / "UserData" / "Neighborhoods"))
            if (fs::is_directory(d.path() / "Lots"))
                for (const auto& f : fs::directory_iterator(d.path() / "Lots"))
                    if (f.path().extension() == ".package" && !fs::equivalent(f.path(), src))
                        others.push_back(f.path());
        std::sort(others.begin(), others.end());
        records = vm::captureLotRecords(*a.vm, world, pkg, a.saved, captured.lot, others);
        for (const auto& w : records.warnings)
            std::printf("  warning: %s\n", w.c_str());
        if (built)
            for (auto& r : writeLotGeometry(*built, pkg))
                captured.lot.otherRecords.push_back({r.type, r.group, r.instance, std::move(r.bytes)});
        captured.lot.save(pkg, dst);
    }
    std::printf("  records: %zu cObjects moved, %zu new; %zu routing states; %zu relationships changed, %zu new, "
                "%zu gone\n",
                records.sceneMoved, records.sceneNew, records.routing, records.relationshipsChanged,
                records.relationshipsNew, records.relationshipsGone);
    std::printf("%s: %zu objects saved after %ld ticks (%zu updated, %zu added, %zu gone, %zu kept as read; %zu new "
                "objt records) to %s\n",
                src.filename().string().c_str(), captured.lot.objects.size(), ticks, captured.updated, captured.added,
                captured.removed, captured.kept, captured.newSaveTypes, dst.string().c_str());
    if (!check)
        return 0;
    // The saved package reads back, state by state, to what was written.
    size_t same = 0, differ = 0, unread = 0;
    {
        DbpfPackage written(dst);
        const LotObjects back = LotObjects::load(written);
        for (const auto& w : back.warnings)
            std::printf("  read back: %s\n", w.c_str());
        for (const auto& s : back.objects) {
            unread += !s.tailError.empty();
            const LotObjects::State* w = captured.lot.state(s.id);
            if (w && LotObjects::writeState(*w, back.version) == LotObjects::writeState(s, back.version))
                ++same;
            else
                ++differ;
        }
        // With no ticks run, each state is written back as the game wrote it.
        if (ticks == 0) {
            DbpfPackage original(src);
            size_t bytesSame = 0, bytesDiffer = 0;
            for (const auto& s : a.saved.objects) {
                const auto* e = original.find({LotObjects::resourceType(s.kind), 0xFFFFFFFF, uint32_t(uint16_t(s.id)), 0});
                const auto* f = written.find({LotObjects::resourceType(s.kind), 0xFFFFFFFF, uint32_t(uint16_t(s.id)), 0});
                if (e && f && original.read(*e) == written.read(*f))
                    ++bytesSame;
                else
                    ++bytesDiffer;
            }
            std::printf("  states byte for byte as the game wrote them: %zu, different: %zu\n", bytesSame,
                        bytesDiffer);
            // The other records the save rewrites, by type.
            std::map<uint32_t, std::pair<size_t, size_t>> others; // type -> (same, different)
            for (const auto& e : original.entries()) {
                const uint32_t t = e.tgi.type;
                if (t == type::DIR || t == 0x584F424A || t == 0x584D544F || t == 0x50455253)
                    continue;
                const auto* f = written.find(e.tgi);
                auto& c = others[t];
                if (f && original.read(e) == written.read(*f))
                    ++c.first;
                else if (f || t != 0x3053CF74)
                    ++c.second;
            }
            for (const auto& [t, c] : others)
                if (c.second)
                    std::printf("  records of type %s different from the game's: %zu of %zu\n", hexString(t).c_str(),
                                c.second, c.first + c.second);
        }
    }
    std::printf("  read back: %zu states as written, %zu different, %zu not read through\n", same, differ, unread);
    size_t geometryDiffers = 0;
    if (built) {
        DbpfPackage written(dst);
        const auto diffs = compareLotGeometry(*built, Lot::load(written));
        geometryDiffers = diffs.size();
        std::printf("  build-mode geometry read back: %s\n", diffs.empty() ? "the same" : "differs:");
        for (const auto& d : diffs)
            std::printf("    %s\n", d.c_str());
    }
    // The reload skips the load entries (VM::runLoadEntries): the check is
    // of what the save carries.
    LoadedLot b = loadInto(code, dst, src, h, characters, true, false);
    const auto diffs = vm::compareLotState(*a.vm, *b.vm);
    std::printf("  VM state after load -> save -> load: %s\n", diffs.empty() ? "the same" : "differs:");
    for (const auto& d : diffs)
        std::printf("    %s\n", d.c_str());
    // --continue N: both VMs (the one that saved, the reloaded one) run on
    // N ticks with the same random numbers (the VM's generator is not the
    // game's and is not saved: the reloaded VM takes the first's state), and
    // are compared every game minute; the first tick they differ tells what
    // the save did not carry (a route or an animation in progress, ...).
    size_t continued = 0;
    if (continueTicks > 0) {
        b.vm->random = a.vm->random;
        // OS2_CONTINUE_WATCH=id:motive: the instruction before each change of
        // that Sim's motive, in both.
        if (const char* w = std::getenv("OS2_CONTINUE_WATCH")) {
            int wid = 0, wm = 0;
            std::sscanf(w, "%d:%d", &wid, &wm);
            for (vm::VM* m : {a.vm.get(), b.vm.get()}) {
                auto last = std::make_shared<std::pair<int16_t, std::string>>(0, "");
                m->trace = [m, wid, wm, last, first = m == a.vm.get()](const vm::Thread& t, const vm::Frame& f, const auto& in) {
                    const vm::Object* o = m->object(vm::ObjectId(wid));
                    if (!o || !o->person)
                        return;
                    const int16_t v = o->person->motives[size_t(wm)];
                    if (v != last->first)
                        std::printf("    watch %s t%llu: %d -> %d after %s\n", first ? "first" : "second",
                                    (unsigned long long)m->ticks, last->first, v, last->second.c_str());
                    last->first = v;
                    last->second = std::to_string(t.me) + " " + (f.behavior ? f.behavior->fileName : "?") + " #" +
                                   std::to_string(f.pc) + " op " + std::to_string(in.opcode);
                };
            }
        }
        long at = -1;
        std::vector<std::string> later;
        // OS2_CONTINUE_ALL=1: run all N ticks (not to the first difference)
        // and print every game minute how many differences there are, then
        // the last ones (a resumed walk plans its way again: does it come
        // back to the same?).
        const bool all = std::getenv("OS2_CONTINUE_ALL") != nullptr;
        for (long i = 1; i <= continueTicks && (at < 0 || all); ++i) {
            a.vm->tick();
            b.vm->tick();
            // OS2_CONTINUE_TRACE=id: that object's tree stack in both, each tick.
            if (const char* tr = std::getenv("OS2_CONTINUE_TRACE"))
                for (vm::VM* m : {a.vm.get(), b.vm.get()}) {
                    std::printf("    %ld %s:", i, m == a.vm.get() ? "first " : "second");
                    if (auto t = m->threads.find(vm::ObjectId(std::atoi(tr))); t != m->threads.end())
                        for (const auto& f : t->second.stack) {
                            std::printf(" %04X@%u", f.instance, f.pc);
                            if (&f == &t->second.stack.back() && f.behavior && f.pc < f.behavior->instructions.size())
                                std::printf(" [%s op %X]", f.behavior->fileName.c_str(), f.behavior->instructions[f.pc].opcode);
                            if (&f == &t->second.stack.back())
                                for (int16_t l : f.locals)
                                    std::printf(" l%d", l);
                        }
                    std::printf("\n");
                }
            auto d = vm::compareLotState(*a.vm, *b.vm);
            if (all && i % a.vm->clock.ticksPerMinute == 0)
                std::printf("    tick %ld: %zu differences\n", i, d.size());
            if (!d.empty() && at < 0)
                at = i;
            if (!d.empty() || !all)
                later = std::move(d);
            else if (all)
                later.clear();
        }
        continued = later.size();
        if (at < 0)
            std::printf("  then %ld ticks more in both: the same\n", continueTicks);
        else if (all && later.empty())
            std::printf("  then %ld ticks more in both: differ from tick %ld, the same again at the end\n",
                        continueTicks, at);
        else
            std::printf("  then %ld ticks more in both: differ from tick %ld:\n", continueTicks, at);
        for (const auto& d : later)
            std::printf("    %s\n", d.c_str());
        // Errors (the object reset) in one of the two only.
        for (vm::VM* m : {a.vm.get(), b.vm.get()})
            for (const auto& [id, r] : m->errorResets) {
                vm::VM* other = m == a.vm.get() ? b.vm.get() : a.vm.get();
                auto o = other->errorResets.find(id);
                if (o == other->errorResets.end() || o->second.count != r.count)
                    std::printf("    object %d reset after an error in the %s only (tick %llu): %s\n", int(id),
                                m == a.vm.get() ? "first" : "second", (unsigned long long)r.lastTick,
                                r.last.substr(0, r.last.find('\n')).c_str());
            }
        std::set<int> named;
        for (const auto& d : later)
            if (int id = 0; std::sscanf(d.c_str(), "object %d:", &id) == 1 && named.insert(id).second)
                if (const vm::Object* o = a.vm->object(vm::ObjectId(id)); o && o->definition)
                    std::printf("    (object %d is %s)\n", id, o->definition->fileName.c_str());
    }
    return diffs.empty() && differ == 0 && geometryDiffers == 0 && continued == 0 ? 0 : 1;
}

// hood-save: every lot of a neighborhood loaded with the neighborhood, run
// and saved to <out>/Lots, each lot's changes written back to the
// neighborhood package (<out>/<P>_Neighborhood.package, which the next lot
// is loaded with); the character files and the rest of the folder are
// copied. With --check each saved lot is loaded back with the saved
// neighborhood and compared with the VM that saved it.
int cmdHoodSave(const std::vector<std::string>& args) {
    std::vector<fs::path> paths;
    long ticks = 0;
    bool check = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--ticks" && i + 1 < args.size())
            ticks = std::stol(args[++i]);
        else if (args[i] == "--check")
            check = true;
        else
            paths.emplace_back(args[i]);
    }
    if (paths.size() != 2)
        throw std::runtime_error("usage: s2pkg hood-save <neighborhood dir> <out dir> [--ticks N] [--check]");
    const fs::path hoodDir = fs::canonical(paths[0]);
    const fs::path outDir = fs::weakly_canonical(fs::absolute(paths[1]));
    const std::string prefix = hoodDir.filename().string();
    const fs::path hoodPackage = hoodDir / (prefix + "_Neighborhood.package");
    if (!fs::exists(hoodPackage))
        throw std::runtime_error("no " + hoodPackage.filename().string() + " in " + hoodDir.string());
    std::vector<fs::path> lots;
    if (fs::is_directory(hoodDir / "Lots"))
        for (const auto& f : fs::directory_iterator(hoodDir / "Lots"))
            if (f.path().extension() == ".package")
                lots.push_back(f.path());
    std::sort(lots.begin(), lots.end());
    if (lots.empty())
        throw std::runtime_error("no lots in " + hoodDir.string());
    const fs::path res = resRootOfLot(lots.front());
    const fs::path game = fs::weakly_canonical(res.parent_path().parent_path());
    if (outDir.string().starts_with(game.string()) || outDir == hoodDir)
        throw std::runtime_error("refusing to write into the game's data: pick an output outside " + game.string());
    fs::create_directories(outDir / "Lots");
    // The folder's other files (characters, thumbnails, images) as they are.
    for (const auto& e : fs::recursive_directory_iterator(hoodDir)) {
        const fs::path rel = fs::relative(e.path(), hoodDir);
        if (*rel.begin() == "Lots" || e.path() == hoodPackage)
            continue;
        const fs::path to = outDir / rel;
        if (e.is_directory())
            fs::create_directories(to);
        else if (!fs::exists(to))
            fs::copy_file(e.path(), to);
    }
    const fs::path outHood = outDir / hoodPackage.filename();
    const fs::path tmpHood = outDir / (prefix + "_Neighborhood.package.tmp");
    fs::copy_file(hoodPackage, outHood, fs::copy_options::overwrite_existing);
    ResourceIndex index(res);
    // The copy's characters: the lots' Sims, and those made in play (babies,
    // NPCs: written to a session folder by the character writers and copied
    // in with each save, as the app's save does, app/game_save.hpp).
    const fs::path characters = outDir / "Characters";
    fs::create_directories(characters);
    const fs::path session = outDir / ".session";
    fs::remove_all(session);
    UiResources texts(index, res);
    vm::textTables() = [&texts](uint32_t group, uint32_t instance) { return texts.table(group, instance); };
    vm::installBabyCharacterWriter(index, characters, session);
    std::vector<fs::path> templates;
    for (const auto& d : fs::directory_iterator(res / "UserData" / "Neighborhoods"))
        if (fs::is_directory(d.path() / "Lots"))
            for (const auto& f : fs::directory_iterator(d.path() / "Lots"))
                if (f.path().extension() == ".package")
                    templates.push_back(f.path());
    std::sort(templates.begin(), templates.end());
    std::map<uint32_t, LotObjects::SaveType> more;
    for (const auto& f : lots) {
        DbpfPackage other(f);
        for (auto& [g, t] : LotObjects::loadSaveTypes(other))
            more.emplace(g, std::move(t));
    }
    size_t same = 0, differ = 0, skipped = 0;
    for (const auto& src : lots) {
        const fs::path dst = outDir / "Lots" / src.filename();
        vm::CodeLibrary code(&index); // each lot adds its Sims' character files
        const hood::Neighborhood hood = hood::Neighborhood::load(outHood, true);
        std::unique_ptr<LoadedLot> a;
        try {
            a = std::make_unique<LoadedLot>(loadInto(code, src, src, &hood, characters));
        } catch (const std::exception& ex) {
            std::printf("%s: not saved (%s); copied as it is\n", src.filename().string().c_str(), ex.what());
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
            ++skipped;
            continue;
        }
        for (long i = 0; i < ticks; ++i)
            a->vm->tick();
        std::vector<fs::path> others;
        for (const auto& t : templates)
            if (!fs::equivalent(t, src))
                others.push_back(t);
        vm::CaptureResult captured = vm::captureLot(*a->vm, a->saved, &more);
        vm::RecordsResult records;
        {
            DbpfPackage pkg(src);
            const Lot world = Lot::load(pkg);
            records = vm::captureLotRecords(*a->vm, world, pkg, a->saved, captured.lot, others);
            captured.lot.save(pkg, dst);
        }
        for (const auto& l : copySessionCharacters(*a->vm, session, characters, prefix))
            std::printf("  %s\n", l.c_str());
        const vm::HoodSaveResult h = vm::saveNeighborhood(*a->vm, hood, outHood, tmpHood);
        fs::rename(tmpHood, outHood);
        std::printf("%s: %zu objects (%zu added, %zu gone, %zu new objt records); cObjects %zu moved, %zu new; %zu "
                    "routing states; lot relationships %zu changed, %zu new, %zu gone; neighborhood: %zu families, %zu "
                    "Sim descriptions, relationships %zu changed, %zu new%s; %zu born, %zu NPCs made\n",
                    src.filename().string().c_str(), captured.lot.objects.size(), captured.added, captured.removed,
                    captured.newSaveTypes, records.sceneMoved, records.sceneNew, records.routing,
                    records.relationshipsChanged, records.relationshipsNew, records.relationshipsGone, h.families,
                    h.descriptions, h.relationshipsChanged, h.relationshipsNew, h.inventories ? ", inventories" : "",
                    h.born, h.npcs);
        for (const auto& w : captured.warnings)
            std::printf("  warning: %s\n", w.c_str());
        for (const auto& w : records.warnings)
            std::printf("  warning: %s\n", w.c_str());
        for (const auto& w : h.warnings)
            std::printf("  warning: %s\n", w.c_str());
        if (!check)
            continue;
        vm::CodeLibrary code2(&index);
        const hood::Neighborhood back = hood::Neighborhood::load(outHood, true);
        LoadedLot b = loadInto(code2, dst, dst, &back, characters, false, false);
        auto diffs = vm::compareLotState(*a->vm, *b.vm);
        for (auto& d : vm::compareNeighborhoodState(*a->vm, *b.vm))
            diffs.push_back("neighborhood: " + d);
        if (diffs.empty()) {
            ++same;
        } else {
            ++differ;
            std::printf("  load -> save -> load differs:\n");
            for (const auto& d : diffs)
                std::printf("    %s\n", d.c_str());
        }
    }
    fs::remove_all(session);
    vm::babyCharacterWriter() = nullptr;
    vm::npcCharacterWriter() = nullptr;
    vm::textTables() = nullptr;
    std::printf("%s: %zu lots saved to %s after %ld ticks", prefix.c_str(), lots.size() - skipped, outDir.string().c_str(),
                ticks);
    if (check)
        std::printf("; the same VM after load -> save -> load in %zu, different in %zu", same, differ);
    std::printf("\n");
    return differ ? 1 : 0;
}

// play-loop: the whole play -> save -> neighborhood -> reload loop on a
// copy of a neighborhood, through the app's own save (app/game_save.hpp):
// the lot played for N ticks with the character writers installed (the
// babies' and the NPCs' files go to a session folder, as the app's), an
// optional scripted birth (--birth mother,father, as pie's) and NPC made
// from a template (--npc GUID[:outfit GUID], as "NPC - Make New Character -
// Specific" runs Make New Character 0xC8), the newborns named after a game
// minute (as autonomy --name-babies); saveGame writes the lot, the
// neighborhood and the character files; the neighborhood is read back (its
// families and new neighbors with their character files, as the loading
// screen and the neighborhood view show them); the lot is loaded again
// with it and compared with the VM that saved it.
int cmdPlayLoop(const std::vector<std::string>& args) {
    std::vector<std::string> paths;
    long ticks = 0;
    std::string birth;
    std::vector<std::string> npcs;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--ticks" && i + 1 < args.size())
            ticks = std::stol(args[++i]);
        else if (args[i] == "--birth" && i + 1 < args.size())
            birth = args[++i];
        else if (args[i] == "--npc" && i + 1 < args.size())
            npcs.push_back(args[++i]);
        else
            paths.push_back(args[i]);
    }
    if (paths.size() != 3)
        throw std::runtime_error("usage: s2pkg play-loop <neighborhood dir> <lot id> <out dir> [--ticks N] "
                                 "[--birth mother,father] [--npc GUID[:outfit GUID]]");
    const fs::path hoodDir = fs::canonical(paths[0]);
    const fs::path outDir = fs::weakly_canonical(fs::absolute(paths[2]));
    const std::string prefix = hoodDir.filename().string();
    if (fs::exists(outDir) && !fs::is_empty(outDir))
        throw std::runtime_error(outDir.string() + " is not empty");
    // A Neighborhoods folder with the copy in it, as the player's.
    const fs::path hoodOut = outDir / prefix;
    const fs::path lotFile = hoodOut / "Lots" / (prefix + "_Lot" + paths[1] + ".package");
    if (!fs::exists(hoodDir / "Lots" / lotFile.filename()))
        throw std::runtime_error("no " + lotFile.filename().string() + " in " + hoodDir.string());
    fs::create_directories(hoodOut);
    fs::copy(hoodDir, hoodOut, fs::copy_options::recursive);
    const fs::path res = resRootOfLot(lotFile);
    const fs::path game = fs::weakly_canonical(res.parent_path().parent_path());
    if (outDir.string().starts_with(game.string()))
        throw std::runtime_error("refusing to write into the game's data: pick an output outside " + game.string());
    const fs::path characters = hoodOut / "Characters";
    const fs::path session = outDir / "session"; // the app's sessionCharactersDir()
    fs::remove_all(session);
    fs::create_directories(session);

    ResourceIndex index;
    s2pkgAddInstallation(index, res);
    UiResources texts(index, res);
    vm::textTables() = [&texts](uint32_t group, uint32_t instance) { return texts.table(group, instance); };
    std::map<int16_t, fs::path> written;
    vm::installBabyCharacterWriter(index, characters, session, &written);
    vm::CodeLibrary code(&index);
    const hood::Neighborhood hood = hood::Neighborhood::load(hoodOut, true);
    LoadedLot a = loadInto(code, lotFile, lotFile, &hood, characters);
    vm::VM& m = *a.vm;
    m.instructionsPerTurn = s2pkgBudget(); // as autonomy (the binary's limit)
    m.dialogTimeouts = true;
    if (std::string err; !vm::runLotLoaded(m, &err) && !err.empty())
        std::printf("CT - Lot Loaded: %s\n", err.c_str());
    std::set<int16_t> before;
    for (const auto& [id, n] : m.neighbors)
        before.insert(id);

    // A tree of one Make New Character instruction run on `me` (pie --birth).
    auto makeCharacter = [&](vm::ObjectId me, std::array<uint8_t, 16> operands, std::array<int16_t, vm::kTempCount> temps,
                             std::vector<int16_t> params, uint16_t instance) {
        Behavior b;
        b.fileName = "Test - Make New Character";
        b.signature = 0x8009;
        b.argCount = uint16_t(params.size());
        b.localCount = 4;
        Behavior::Instruction in;
        in.opcode = 0x13;
        in.onTrue = {Behavior::TargetKind::True, 0};
        in.onFalse = {Behavior::TargetKind::False, 0};
        in.operands = operands;
        b.instructions.push_back(in);
        const vm::Object& o = *m.object(me);
        code.add(o.group, instance, std::move(b));
        return vm::runOneTickTree(m, temps, me, me, me, instance, params);
    };
    if (!birth.empty()) {
        const auto comma = birth.find(',');
        const vm::ObjectId ma = vm::findSim(m, birth.substr(0, comma));
        const vm::ObjectId pa = comma == std::string::npos ? 0 : vm::findSim(m, birth.substr(comma + 1));
        if (!ma || !pa)
            throw std::runtime_error("--birth: no Sim " + birth);
        std::array<int16_t, vm::kTempCount> temps{};
        temps[0] = 1;
        const bool ok = makeCharacter(ma, {0, 0, 0, 0x09, 1, 0, 0x09, 0, 0, 0x01}, temps, {ma, pa}, 0x1FF0);
        std::printf("birth (Make New Character, baby of %d and %d): %s\n", ma, pa, ok ? "true" : "false");
    }
    for (const std::string& spec : npcs) {
        // Make New Character 0xC8 as the NPC trees run it: the template GUID
        // in temps 0/1, the outfit's in temps 2/3, age and gender drawn.
        const auto colon = spec.find(':');
        const uint32_t guid = uint32_t(std::stoul(spec.substr(0, colon), nullptr, 0));
        const uint32_t outfit = colon == std::string::npos ? 0 : uint32_t(std::stoul(spec.substr(colon + 1), nullptr, 0));
        const vm::ObjectId me = vm::findSim(m, "");
        std::array<int16_t, vm::kTempCount> temps{};
        temps[0] = int16_t(guid & 0xFFFF), temps[1] = int16_t(guid >> 16);
        temps[2] = int16_t(outfit & 0xFFFF), temps[3] = int16_t(outfit >> 16);
        const uint8_t flags = outfit ? 0xC8 : 0x08;
        // Locals 0-2 are 0: age and gender drawn.
        const bool ok = makeCharacter(me, {0, 1, 2, 0, 0, 0, 0, 0, 0, flags}, temps, {}, 0x1FF1);
        int16_t id = 0;
        if (vm::Object* o = m.object(me); o && !o->arrays.empty() && !o->array(0).values.empty())
            id = o->array(0).values.back();
        // "Generic Sims Call 4" puts the NPCs in the service family (the
        // trees pass 0x7FFF; inferred from the shipped neighborhoods).
        if (vm::Neighbor* n = m.neighbor(id))
            n->personData[vm::persondata::FamilyNumber] = 0x7FFF;
        std::printf("NPC from template %s: %s, neighbor %d\n", hexString(guid).c_str(), ok ? "true" : "false", id);
    }
    for (long i = 0; i < ticks; ++i) {
        m.tick();
        for (auto& [who, d] : m.dialogs) { // the newborns' names, after a game minute (autonomy --name-babies)
            static std::map<uint32_t, uint64_t> seen;
            if (d.type != vm::dialogtype::BabyName || d.answer >= 0)
                continue;
            const uint64_t first = seen.emplace(d.id, m.ticks).first->second;
            if (m.ticks - first < 30)
                continue;
            for (size_t k = 0; k < d.names.size(); ++k)
                if (d.names[k].text.empty())
                    d.names[k].text = k ? "Bebê " + std::to_string(k + 1) : std::string("Bebê");
            if (!d.names.empty())
                d.text = d.names[0].text;
            d.answer = 0;
        }
    }
    std::printf("%s: played %ld ticks\n", lotFile.filename().string().c_str(), ticks);
    std::vector<int16_t> made;
    for (const auto& [id, n] : m.neighbors)
        if (!before.count(id)) {
            made.push_back(id);
            const auto f = written.find(id);
            std::printf("  new neighbor %d \"%s %s\" GUID %s family %d age %d%s%s\n", id, n.firstName.c_str(),
                        n.lastName.c_str(), hexString(n.guid).c_str(), n.personData[vm::persondata::FamilyNumber],
                        n.personData[vm::persondata::PersonAge], f == written.end() ? "" : ", character ",
                        f == written.end() ? "" : f->second.filename().string().c_str());
        }

    // Save, as the app's Options -> Save.
    const GameSaveResult saved = saveGame(m, lotFile, game, session);
    for (const auto& l : saved.lines)
        std::printf("  save: %s\n", l.c_str());
    if (!saved.ok)
        throw std::runtime_error("save failed: " + saved.error);

    // Back to the neighborhood: what it reads of the families and Sims.
    size_t problems = 0;
    const hood::Neighborhood back = hood::Neighborhood::load(hoodOut, true);
    const int16_t family = m.globals[vm::global::CurrentFamily];
    if (const hood::Family* f = back.family(uint32_t(family))) {
        std::printf("  neighborhood: family %d \"%s\", funds %d:", family, f->name.c_str(), f->record.funds);
        for (uint16_t id : f->members) {
            const hood::Neighbor* n = back.neighbor(id);
            std::printf(" %s%s", n ? n->name().c_str() : "?", n && n->character >= 0 ? "" : " (no character file)");
            problems += !n || n->character < 0;
        }
        std::printf("\n");
    }
    for (int16_t id : made) {
        const hood::Neighbor* n = back.neighbor(uint16_t(id));
        const bool ok = n && n->character >= 0;
        std::printf("  neighborhood: new neighbor %d %s\n", id,
                    !n ? "missing" : (n->name() + (ok ? ", character file" : ", no character file")).c_str());
        problems += !ok;
    }
    for (const auto& e : back.errors)
        std::printf("  neighborhood: %s\n", e.c_str());

    // The lot again, with the saved neighborhood.
    vm::CodeLibrary code2(&index);
    LoadedLot b = loadInto(code2, lotFile, lotFile, &back, characters, false, false);
    for (const auto& e : b.load.objects)
        if (!e.error.empty()) {
            std::printf("  reload: object %d (%s): %s\n", e.id, e.name.c_str(), e.error.c_str());
            ++problems;
        }
    auto diffs = vm::compareLotState(m, *b.vm);
    for (auto& d : vm::compareNeighborhoodState(m, *b.vm))
        diffs.push_back("neighborhood: " + d);
    std::printf("  play -> save -> neighborhood -> reload: %s\n", diffs.empty() ? "the same" : "differs:");
    for (const auto& d : diffs)
        std::printf("    %s\n", d.c_str());
    fs::remove_all(session);
    vm::babyCharacterWriter() = nullptr;
    vm::npcCharacterWriter() = nullptr;
    vm::textTables() = nullptr;
    return diffs.empty() && problems == 0 ? 0 : 1;
}

// objt-check: each record of the lots' save type tables against the one
// saveTypeRecord builds from the definition, field by field.
int cmdObjtCheck(const std::vector<std::string>& args) {
    std::map<std::string, size_t> wrong;
    std::map<std::string, std::vector<std::string>> examples;
    size_t records = 0, missing = 0, same = 0;
    for (const auto& a : args) {
        const fs::path src(a);
        ResourceIndex index;
        s2pkgAddInstallation(index, resRootOfLot(src));
        vm::CodeLibrary code(&index);
        std::optional<hood::Neighborhood> hood;
        if (const auto hp = vm::neighborhoodPackageOf(src); !hp.empty())
            hood = hood::Neighborhood::load(hp, true);
        LoadedLot l = loadInto(code, src, src, hood ? &*hood : nullptr, vm::charactersDirOf(src));
        for (const auto& [st, t] : l.saved.saveTypes) {
            ++records;
            const auto g = vm::saveTypeRecord(l.vm->code(), t.guid);
            if (!g) {
                ++missing;
                continue;
            }
            bool ok = true;
            auto check = [&](const char* name, auto x, auto y) {
                if (x == y)
                    return;
                ok = false;
                ++wrong[name];
                auto& e = examples[name];
                if (e.size() < 4) {
                    std::string sx, sy;
                    if constexpr (std::is_same_v<decltype(x), std::string>)
                        sx = x, sy = y;
                    else
                        sx = std::to_string(x), sy = std::to_string(y);
                    e.push_back(src.stem().string() + " " + hexString(t.guid) + " \"" + t.catalogName + "\": " + sx +
                                " vs " + sy);
                }
            };
            check("init tree version", t.initTreeVersion, g->initTreeVersion);
            check("main tree version", t.mainTreeVersion, g->mainTreeVersion);
            check("field 0x3A", t.field3A, g->field3A);
            check("field 0x3B", t.field3B, g->field3B);
            check("string count", t.stringCount, g->stringCount);
            check("object type", t.objectType, g->objectType);
            check("name", t.catalogName, g->catalogName);
            check("flags", t.flags, g->flags);
            check("field 0xD0", t.guid2, g->guid2);
            same += ok;
        }
    }
    std::printf("%zu records: %zu built the same, %zu without a definition\n", records, same, missing);
    for (const auto& [k, n] : wrong) {
        std::printf("  %s differs in %zu\n", k.c_str(), n);
        for (const auto& e : examples[k])
            std::printf("    %s\n", e.c_str());
    }
    return 0;
}

} // namespace

int saveCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "lot-save" && argc >= 4)
        return cmdLotSave({argv + 2, argv + argc});
    if (cmd == "hood-save" && argc >= 4)
        return cmdHoodSave({argv + 2, argv + argc});
    if (cmd == "play-loop" && argc >= 5)
        return cmdPlayLoop({argv + 2, argv + argc});
    if (cmd == "objt-check" && argc >= 3)
        return cmdObjtCheck({argv + 2, argv + argc});
    return -1;
}

const char* saveUsage() {
    return "       s2pkg lot-save <lot.package> <out.package> [--ticks N] [--check] [--continue N] [--buy|--sell|--move ...]\n"
           "              [--wall|--room|--delete-wall x0,y0,x1,y1[,level]] [--floor x0,y0,x1,y1,pattern[,level[,rot]]]\n"
           "              [--fence x0,y0,x1,y1,fence[,level]] [--delete-fence x0,y0,x1,y1[,level]] [--floor-click px,py,pattern[,level]]\n"
           "              [--wallpaper x0,y0,x1,y1,r|l|b,pattern[,level]] [--wallpaper-room x,y,pattern[,level]]\n"
           "              [--wallpaper-flood x0,y0,x1,y1,r|l,pattern[,level]] (Shift: the connected faces with the clicked one's pattern)\n"
           "              [--design id,subset=material[,subset=material]] (the Design tool, §15)\n"
           "              [--auto-roof tool 1-5|0[,keep]] (auto-roof: hip, gable, the mixes, the slab; 0 removes)\n"
           "              [--stairs product,x,y,level,dir[,endx,endy[,whole]]] [--delete-stairs part-id] (modular stairs)\n"
           "              [--railing part-id,side 0|1[,delete]] (the Railing Tool: 1 = left of the steps' facing; Ctrl deletes)\n"
           "              [--dormer product 6-11,x,y,dir[,level]] (dormers)\n"
           "              [--fireplace product 0-4,x,y,dir[,level]] [--delete-fireplace piece-id] (the fireplace tool; the sledgehammer)\n"
           "              [--fence-arch|--delete-fence-arch x0,y0,x1,y1[,level]] [--build-funds n]\n"
           "              [--check-build 1] (each roof and upper floor tile against IsRoofValid / IsFloorable)\n"
           "       s2pkg hood-save <neighborhood dir> <out dir> [--ticks N] [--check] [--buy|--sell|--move ...]\n"
           "       s2pkg play-loop <neighborhood dir> <lot id> <out dir> [--ticks N] [--birth mother,father] [--npc GUID[:outfit GUID]]\n"
           "              (out dir/<P>: play, save as the app does, read the neighborhood back, reload the lot and compare)\n"
           "       s2pkg objt-check <lot.package>...\n";
}

} // namespace os2
