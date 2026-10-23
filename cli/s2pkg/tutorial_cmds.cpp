// s2pkg tutorial: a tutorial lot run headless with its tutorial controller
// (src/vm/tutorial.hpp, docs/tutorial-mac.md): the progress windows it
// shows, the controls it highlights, disables and listens to; --auto plays
// along by clicking what the step asks for.

#include "tutorial_cmds.hpp"
#include "budget.hpp"
#include "data_index.hpp"

#include "formats/dbpf.hpp"
#include "formats/lot.hpp"
#include "formats/lot_objects.hpp"
#include "formats/resindex.hpp"
#include "vm/hood_bridge.hpp"
#include "vm/lot.hpp"
#include "vm/lot_neighborhood.hpp"
#include "vm/lot_resume.hpp"
#include "vm/tutorial.hpp"
#include "vm/vm.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace os2 {

namespace {

std::filesystem::path resRootOf(const std::filesystem::path& pkg) {
    for (auto p = std::filesystem::absolute(pkg).parent_path(); p.has_relative_path(); p = p.parent_path())
        if (p.filename() == "Res")
            return p;
    if (const char* data = std::getenv("OS2_DATA"); data && std::filesystem::is_directory(std::filesystem::path(data) / "TSData/Res"))
        return std::filesystem::path(data) / "TSData/Res";
    throw std::runtime_error("the lot is not under a TSData/Res directory (set OS2_DATA to data/game)");
}

std::string firstLine(const std::string& s, size_t max = 90) {
    std::string out = s.substr(0, s.find_first_of("\r\n"));
    if (out.size() > max)
        out = out.substr(0, max) + "...";
    return out;
}

int16_t attr(vm::VM& m, uint32_t guid, size_t i) {
    for (const vm::Object* o : m.objectTable())
        if (o && o->guid() == guid)
            return i < o->attributes.size() ? o->attributes[i] : 0;
    return -1;
}

void printStack(vm::VM& m, uint32_t guid) {
    for (const vm::Object* o : m.objectTable())
        if (o && o->guid() == guid) {
            auto it = m.threads.find(o->id);
            if (it == m.threads.end())
                return;
            std::printf("    controller %d thread:", o->id);
            for (const vm::Frame& f : it->second.stack)
                std::printf(" %04X:%d", f.instance, f.pc);
            std::printf("  temps %d %d %d\n", it->second.temps[0], it->second.temps[1], it->second.temps[2]);
        }
}

int cmdTutorial(const std::filesystem::path& lotFile, int ticks, bool autoPlay, int wait, bool verbose) {
    ResourceIndex index;
    s2pkgAddInstallation(index, resRootOf(lotFile));
    vm::CodeLibrary code(&index);
    DbpfPackage pkg(lotFile);
    const Lot lot = Lot::load(pkg);
    const LotObjects saved = LotObjects::load(pkg);
    vm::VM m(code);
    m.initCreatedObjects = true;
    m.instructionsPerTurn = s2pkgBudget();
    m.setClockGlobals();
    m.autoDialogAnswer = 0;
    const bool hood = vm::loadLotNeighborhood(m, lotFile);
    const vm::LotLoad load = vm::loadLot(m, lot, saved, vm::charactersDirOf(lotFile));
    if (hood)
        vm::bindLotSims(m);
    vm::startSavedLot(m, load, saved, false);
    const int created = vm::ensureTutorialControllers(m);
    const std::string lotName = lotFile.filename().string();
    std::printf("%s: lot %d, neighborhood %d, speed %d%s; %d tutorial controller(s) created\n", lotName.c_str(),
                m.globals[vm::global::CurrentHouse], m.globals[vm::global::CurrentNeighborhoodId],
                m.globals[vm::global::Speed], m.globals[vm::global::Paused] ? " (paused)" : "", created);
    vm::TutorialUi ui;
    uint32_t seen = 0;
    vm::TutorialUi::Key lastHighlight{0, 0};
    int idle = 0, maxStep = 0;
    for (int t = 0; t < ticks; ++t) {
        m.tick();
        ui.update(m);
        for (const auto& k : ui.takePresses())
            std::printf("  [%6d] press 0x%08X/0x%08X\n", t, k.first, k.second);
        const vm::TutorialProgress& p = m.tutorial.progress;
        if (p.serial != seen) {
            seen = p.serial;
            idle = 0;
            if (p.visible) {
                maxStep = std::max(maxStep, p.step);
                std::printf("  [%6d] step %d/%d%s \"%s\": %s%s\n", t, p.step, p.total, p.modal ? " (modal)" : "",
                            firstLine(p.title, 40).c_str(), firstLine(p.message).c_str(),
                            p.image ? (" [picture " + std::to_string(p.image) + "]").c_str() : "");
            } else
                std::printf("  [%6d] progress window hidden\n", t);
        }
        if (ui.highlight() != lastHighlight) {
            lastHighlight = ui.highlight();
            std::printf("  [%6d] highlight 0x%08X/0x%08X\n", t, lastHighlight.first, lastHighlight.second);
        }
        if (verbose && t % 300 == 0)
            printStack(m, vm::kTutorialControllerGuid);
        if (verbose && t % 300 == 0)
            std::printf("  [%6d] controller: episode %d section %d of %d\n", t, attr(m, vm::kTutorialControllerGuid, 1),
                        attr(m, vm::kTutorialControllerGuid, 2), attr(m, vm::kTutorialControllerGuid, 3));
        if (!autoPlay || ++idle < wait)
            continue;
        // Play along: the highlighted control if a click on it does something,
        // else Next.
        const auto h = ui.highlight();
        if (h.second && ui.hasEventTree(h.first, h.second) && ui.enabled(h.first, h.second)) {
            std::printf("  [%6d] click highlighted 0x%08X/0x%08X\n", t, h.first, h.second);
            ui.click(m, h.first, h.second);
            idle = 0;
        } else if (p.visible && ui.enabled(vm::kProgressWindow, vm::kProgressNext) &&
                   (ui.hasEventTree(vm::kProgressWindow, vm::kProgressNext) || p.modal)) {
            std::printf("  [%6d] click Next\n", t);
            ui.click(m, vm::kProgressWindow, vm::kProgressNext);
            idle = 0;
        } else if (idle % (10 * wait) == wait) {
            // A step that waits on the Sim (no highlight, no Next): do it.
            if (const std::string done = vm::tutorialAutoAct(m, &index); !done.empty())
                std::printf("  [%6d] %s\n", t, done.c_str());
            else if (idle == wait)
                for (const auto& k : ui.withTrees())
                    std::printf("  [%6d] waiting; a click on 0x%08X/0x%08X runs a tree%s\n", t, k.first, k.second,
                                ui.enabled(k.first, k.second) ? "" : " (disabled)");
        }
    }
    std::printf("controller: episode %d section %d of %d; furthest step shown %d\n",
                attr(m, vm::kTutorialControllerGuid, 1), attr(m, vm::kTutorialControllerGuid, 2),
                attr(m, vm::kTutorialControllerGuid, 3), maxStep);
    return 0;
}

} // namespace

int tutorialCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd != "tutorial" || argc < 3)
        return -1;
    int ticks = 3000, wait = 60;
    bool autoPlay = false, verbose = false;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--ticks" && i + 1 < argc)
            ticks = std::atoi(argv[++i]);
        else if (a == "--wait" && i + 1 < argc)
            wait = std::atoi(argv[++i]);
        else if (a == "--auto")
            autoPlay = true;
        else if (a == "-v")
            verbose = true;
    }
    return cmdTutorial(argv[2], ticks, autoPlay, wait, verbose);
}

const char* tutorialUsage() {
    return "       s2pkg tutorial <tutorial lot.package> [--ticks N] [--auto [--wait ticks]] [-v]\n";
}

} // namespace os2
