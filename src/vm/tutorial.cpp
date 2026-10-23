// The tutorial's primitives and controllers: tutorial.hpp, docs/tutorial-mac.md.

#include "tutorial.hpp"
#include "formats/log_once.hpp"

#include "engine.hpp"
#include "go_here.hpp"
#include "interactions.hpp"
#include "errors.hpp"
#include "kill.hpp"
#include "placement.hpp"
#include "vm.hpp"

#include "formats/binreader.hpp"
#include "formats/catalog.hpp"
#include "formats/resindex.hpp"
#include "formats/strings.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <exception>

namespace os2::vm {

namespace {

constexpr uint16_t kButtonIds = 0x96;     // STR# "Button GUIDs"
constexpr uint16_t kDialogStrings = 0x12D;
constexpr size_t kMaxCommands = 512;

// The STR# of a scope: 0 the tree's own group (else the code owner's), 1
// its semi-global, 2 the global one.
const StringTable* tableOf(Context& c, int scope, uint16_t table) {
    CodeLibrary& code = c.vm.code();
    const Object* owner = c.vm.object(c.frame.codeOwner);
    switch (scope) {
    case 0:
        if (const StringTable* t = code.strings(c.frame.group, table))
            return t;
        return owner ? code.strings(owner->group, table) : nullptr;
    case 1: {
        if (owner && owner->semiGroup)
            if (const StringTable* t = code.strings(owner->semiGroup, table))
                return t;
        if (const uint32_t semi = code.semiGlobalFor(c.frame.group))
            if (const StringTable* t = code.strings(semi, table))
                return t;
        return code.strings(c.frame.group, table);
    }
    default: return code.strings(kGlobalGroup, table);
    }
}

// Entry `index` (0-based) as a number ("0x480e8304"); the strings of these
// tables are the same in every language, so English first.
uint32_t idAt(const StringTable* t, int index) {
    if (!t || index < 0)
        return 0;
    std::string s = t->get(size_t(index), lang::EnglishUS);
    if (s.empty())
        s = t->get(size_t(index));
    return uint32_t(std::strtoul(s.c_str(), nullptr, 0));
}

// UI Effect (cTSObject::TryUIEffect, BV 0xe2b310). Operands: mode s8 0,
// window index u16 1, control index u16 3, flags byte 5 (0x02 the window's
// table is the semi-global one, 0x01 the global one, else the tree's own;
// 0x08 / 0x04 the same for the control's; docs/tutorial-mac.md §2), effect
// u16 6, event tree u16 8.
// Mode 8 (an event tree on a text notification) takes the notification from
// the variable (owner 13, data u16 11). Always true (the binary's errors for
// a window that is not up are left to the host).
Exit uiEffect(Context& c) {
    const auto& o = c.instruction.operands;
    // The engine primitives' recorder still sees it (engine.hpp uiEvents).
    {
        auto& events = uiEvents();
        if (events.size() >= 256)
            events.pop_front();
        events.push_back({uint8_t(c.instruction.opcode), c.thread.me, c.frame.stackObject, c.instruction.operands});
    }
    const int mode = int8_t(o[0]);
    const uint8_t flags = o[5];
    TutorialUiCommand cmd;
    cmd.mode = mode;
    cmd.effect = le16(&o[6]);
    cmd.tree = le16(&o[8]);
    cmd.object = c.thread.me;
    cmd.tick = c.vm.ticks;
    if (mode != uieffect::TnsEventTree) {
        const int windowScope = flags & 0x01 ? 2 : flags & 0x02 ? 1 : 0;
        const int controlScope = flags & 0x04 ? 2 : flags & 0x08 ? 1 : 0;
        cmd.window = idAt(tableOf(c, windowScope, kButtonIds), le16(&o[1]));
        cmd.control = idAt(tableOf(c, controlScope, kButtonIds), le16(&o[3]));
    }
    auto& q = c.vm.tutorial.commands;
    if (q.size() >= kMaxCommands)
        q.pop_front();
    q.push_back(cmd);
    return Exit::True;
}

std::string text(const StringTable* t, int oneBased) {
    if (!t || oneBased <= 0)
        return {};
    return t->get(size_t(oneBased - 1));
}

} // namespace

int tutorialNextDialog(Context& c, int type, uint32_t stringsGroup) {
    VM& vm = c.vm;
    const auto& o = c.instruction.operands;
    const auto& temps = c.thread.temps;
    TutorialProgress& p = vm.tutorial.progress;
    // TryDialog (BV 0xe2bb64): temp 1 = 0 hides the progress window
    // (cTSTutorialManager::HideProgressDisplay).
    if (temps[1] == 0) {
        p.visible = false;
        ++p.serial;
        return int(Exit::True);
    }
    const StringTable* t = vm.code().strings(stringsGroup, kDialogStrings);
    const uint8_t flags = o[8];
    // Title: byte 6 (temp 3 with flag 0x10); message: u16 13 (temp 0 with
    // 0x02, as is); picture: the string at byte 0 | byte 2 << 8 (temp 4 with 0x20), an
    // image ID ("2d8fac55"; English only: the other languages leave it empty).
    const int title = flags & 0x10 ? temps[3] : o[6];
    const int message = flags & 0x02 ? temps[0] : le16(&o[13]);
    const int picture = flags & 0x20 ? temps[4] : (o[0] | o[2] << 8);
    p.title = expandDialogTokens(c, text(t, title));
    p.message = expandDialogTokens(c, text(t, message));
    p.image = 0;
    if (t && picture > 0) {
        std::string s = t->get(size_t(picture - 1), lang::EnglishUS);
        if (s.empty())
            s = t->get(size_t(picture - 1));
        p.image = uint32_t(std::strtoul(s.c_str(), nullptr, 16));
    }
    p.step = temps[1];
    p.total = temps[2];
    p.modal = type == 25;
    p.owner = c.thread.me;
    p.visible = true;
    ++p.serial;
    TutorialUiCommand shown;
    shown.mode = uieffect::ProgressShown;
    shown.window = kProgressWindow;
    shown.object = c.thread.me;
    shown.tick = vm.ticks;
    vm.tutorial.commands.push_back(shown);
    return int(Exit::True);
}

void registerTutorialPrimitives(VM& vm) {
    vm.setPrimitive(0x22, uiEffect);
}

int ensureTutorialControllers(VM& vm) {
    int created = 0;
    for (uint32_t guid : {kTutorialControllerGuid, kTutorialDeathCheckerGuid}) {
        bool present = false;
        for (const Object* o : vm.objectTable())
            if (o && o->definition && o->definition->guid() == guid) {
                present = true;
                break;
            }
        if (present || !vm.code().definitionByGuid(guid))
            continue;
        std::vector<ObjectId> before;
        for (const auto& [id, o] : vm.objects())
            before.push_back(id);
        try {
            Object* o = vm.instantiateGuid(guid);
            if (!o)
                continue;
            vm.setPosition(*o, Position{});
            vm.setContainer(*o, 0, -1);
            vm.initialize(o->id, before);
            ++created;
        } catch (const std::exception& err) {
            os2::logOnce("tutorial controller", std::to_string(guid), err);
            // A controller that does not start leaves the lot as it was.
        }
    }
    return created;
}

void tickTutorialControllers(VM& vm) {
    std::vector<ObjectId> ids;
    for (const Object* o : vm.objectTable())
        if (o && (o->guid() == kTutorialControllerGuid || o->guid() == kTutorialDeathCheckerGuid))
            ids.push_back(o->id);
    for (ObjectId id : ids) {
        auto it = vm.threads.find(id);
        if (it == vm.threads.end())
            continue;
        Thread& t = it->second;
        if (t.status == Thread::Status::Finished) {
            const Object* o = vm.object(id);
            const uint16_t tree = o ? vm.functionTree(*o, 1) : 0;
            if (!tree)
                continue;
            const size_t done = t.instructions;
            t = vm.start(id, tree);
            t.instructions = done;
        }
        if (t.status == Thread::Status::Running)
            vm.run(t, vm.instructionsPerTurn);
    }
    flushKillQueue(vm);
}

bool runTutorialEventTree(VM& vm, ObjectId object, uint16_t tree) {
    if (!vm.object(object) || !tree)
        return false;
    std::array<int16_t, kTempCount> temps{};
    try {
        runOneTickTree(vm, temps, object, object, 0, tree, {});
    } catch (const std::exception& err) {
        os2::logOnce("tutorial event tree", std::to_string(tree), err);
        return false;
    }
    return true;
}

void TutorialUi::reset() {
    callbacks_.clear();
    enabled_.clear();
    hidden_.clear();
    highlight_ = {0, 0};
    presses_.clear();
}

void TutorialUi::update(VM& vm) {
    auto& q = vm.tutorial.commands;
    while (!q.empty()) {
        const TutorialUiCommand c = q.front();
        q.pop_front();
        const Key k{c.window, c.control};
        switch (c.mode) {
        case uieffect::ProgressShown:
            // A new window: what was set on the old one's controls is gone.
            for (auto it = callbacks_.begin(); it != callbacks_.end();)
                it = it->first.first == kProgressWindow ? callbacks_.erase(it) : std::next(it);
            for (auto it = enabled_.begin(); it != enabled_.end();)
                it = it->first.first == kProgressWindow ? enabled_.erase(it) : std::next(it);
            for (auto it = hidden_.begin(); it != hidden_.end();)
                it = it->first == kProgressWindow ? hidden_.erase(it) : std::next(it);
            if (highlight_.first == kProgressWindow)
                highlight_ = {0, 0};
            break;
        case uieffect::Press: presses_.push_back(k); break;
        case uieffect::Disable: enabled_[k] = false; break;
        case uieffect::Enable: enabled_[k] = true; break;
        case uieffect::Highlight: highlight_ = c.control ? k : Key{0, 0}; break;
        case uieffect::EventTree:
            if (c.control)
                callbacks_[k] = {c.object, c.tree};
            break;
        case uieffect::Restore: enabled_.erase(k); break;
        case uieffect::ResetTrees: callbacks_.clear(); break;
        case uieffect::Show: hidden_.erase(k); break;
        case uieffect::Hide:
            if (c.control)
                hidden_.insert(k);
            break;
        default: break;
        }
    }
}

bool TutorialUi::enabled(uint32_t window, uint32_t control) const {
    auto it = enabled_.find({window, control});
    return it == enabled_.end() || it->second;
}

bool TutorialUi::visible(uint32_t window, uint32_t control) const {
    return !hidden_.count({window, control});
}

std::vector<TutorialUi::Key> TutorialUi::blocked() const {
    std::vector<Key> out(hidden_.begin(), hidden_.end());
    for (const auto& [k, on] : enabled_)
        if (!on)
            out.push_back(k);
    return out;
}

std::vector<TutorialUi::Key> TutorialUi::withTrees() const {
    std::vector<Key> out;
    for (const auto& [k, cb] : callbacks_)
        out.push_back(k);
    return out;
}

bool TutorialUi::hasEventTree(uint32_t window, uint32_t control) const {
    return callbacks_.count({window, control}) != 0;
}

bool TutorialUi::click(VM& vm, uint32_t window, uint32_t control) {
    update(vm);
    if (!enabled(window, control) || !visible(window, control))
        return false;
    TutorialProgress& p = vm.tutorial.progress;
    if (window == kProgressWindow && control == kProgressNext && p.modal && p.visible) {
        p.visible = false; // the modal window ends with its button
        ++p.serial;
    }
    if (auto it = callbacks_.find({window, control}); it != callbacks_.end()) {
        const Callback cb = it->second;
        runTutorialEventTree(vm, cb.object, cb.tree);
        update(vm);
    }
    return true;
}

// The controller's frame that called the sleep it is in (null if none).
static const Frame* tutorialWaitFrame(VM& vm, const Thread** thread = nullptr) {
    for (const Object* o : vm.objectTable()) {
        if (!o || o->guid() != kTutorialControllerGuid)
            continue;
        auto it = vm.threads.find(o->id);
        if (it == vm.threads.end() || it->second.stack.size() < 2)
            continue;
        if (thread)
            *thread = &it->second;
        const Frame& f = it->second.stack[it->second.stack.size() - 2];
        return f.behavior ? &f : nullptr;
    }
    return nullptr;
}

int tutorialAwaitedCategory(VM& vm) {
    const Thread* t = nullptr;
    const Frame* f = tutorialWaitFrame(vm, &t);
    if (!f)
        return -1;
    for (const auto& in : f->behavior->instructions)
        if (in.opcode == 0x1F && (in.operands[4] & 0x7F) == 12 && in.onFalse.kind == Behavior::TargetKind::Instruction &&
            in.onFalse.index == f->pc)
            return t->temps[0];
    return -1;
}

int tutorialAwaitedPersonData(VM& vm) {
    const Frame* f = tutorialWaitFrame(vm);
    if (!f)
        return -1;
    auto toPc = [&](const Behavior::Target& t) { return t.kind == Behavior::TargetKind::Instruction && t.index == f->pc; };
    for (const auto& in : f->behavior->instructions)
        if (in.opcode == 0x02 && in.operands[6] == 0x13 && (toPc(in.onTrue) || toPc(in.onFalse)))
            return in.operands[0] | in.operands[1] << 8;
    return -1;
}

std::string tutorialActNamed(VM& vm, const std::string& part) {
    const ObjectId sim = findSim(vm);
    if (!sim || part.empty())
        return {};
    for (const Object* o : vm.objectTable()) {
        if (!o || o->person || !o->position.inWorld() || !o->definition || interactionLeader(vm, o->id) != o->id)
            continue;
        for (const auto& e : pieMenu(vm, sim, o->id))
            if (e.available() && !e.debug && e.name.find(part) != std::string::npos)
                if (auto q = interactionFromPie(vm, e)) {
                    q->pushedBy = sim;
                    enqueue(vm, *vm.object(sim), *q);
                    return q->name;
                }
    }
    return {};
}

uint32_t tutorialAwaitedGuid(VM& vm) {
    for (const Object* o : vm.objectTable()) {
        if (!o || o->guid() != kTutorialControllerGuid)
            continue;
        auto it = vm.threads.find(o->id);
        if (it == vm.threads.end() || it->second.stack.size() < 2)
            continue;
        // The frame that called the sleep: a Test Object Type there whose
        // false branch is the call.
        const Frame& f = it->second.stack[it->second.stack.size() - 2];
        if (!f.behavior)
            continue;
        for (const auto& in : f.behavior->instructions)
            if (in.opcode == 0x20 && in.onFalse.kind == Behavior::TargetKind::Instruction && in.onFalse.index == f.pc &&
                in.operands[6] == 0x13 && (in.operands[4] | in.operands[5] << 8) == 0x50)
                return uint32_t(in.operands[0]) | uint32_t(in.operands[1]) << 8 | uint32_t(in.operands[2]) << 16 |
                       uint32_t(in.operands[3]) << 24;
    }
    return 0;
}

std::string tutorialAct(VM& vm, uint32_t guid) {
    const ObjectId sim = findSim(vm);
    const Object* me = sim ? vm.object(sim) : nullptr;
    if (!me || !guid)
        return {};
    auto queueFirst = [&](ObjectId object) -> std::string {
        for (const auto& e : pieMenu(vm, sim, object))
            if (e.available() && !e.debug)
                if (auto q = interactionFromPie(vm, e)) {
                    q->pushedBy = sim;
                    enqueue(vm, *vm.object(sim), *q);
                    return q->name;
                }
        return {};
    };
    if (guid == kGoHereGuid) {
        // A Go Here a few tiles from where the Sim stands, the first spot a
        // destination can stand on.
        const float dx[] = {3, -3, 0, 0, 2, -2, 4, -4}, dy[] = {0, 0, 3, -3, 2, -2, 1, -1};
        for (size_t k = 0; k < std::size(dx); ++k) {
            const ObjectId d = createGoHereDestination(vm, sim, me->position.x + dx[k], me->position.y + dy[k],
                                                       me->position.level);
            if (!d)
                continue;
            std::string name = queueFirst(d);
            releaseDestination(vm, d);
            if (!name.empty())
                return name;
        }
        return {};
    }
    for (const Object* o : vm.objectTable()) {
        if (!o || o->person || !o->position.inWorld() || !o->definition)
            continue;
        bool match = o->guid() == guid;
        if (!match && o->definition->masterId())
            if (auto m = vm.code().masterDefinition(o->group, o->definition->masterId()))
                if (const ObjectDefinition* md = vm.code().definition(o->group, *m))
                    match = md->guid() == guid;
        if (match)
            if (std::string name = queueFirst(interactionLeader(vm, o->id)); !name.empty())
                return name;
    }
    return {};
}

std::string tutorialBuyCategory(VM& m, ResourceIndex& index, int category) {
    static std::unique_ptr<Catalog> catalog;
    if (!catalog)
        catalog = std::make_unique<Catalog>(Catalog::load(index));
    std::vector<const CatalogEntry*> byPrice;
    for (const auto& e : catalog->entries())
        byPrice.push_back(&e);
    std::stable_sort(byPrice.begin(), byPrice.end(), [](auto* a, auto* b) { return a->price < b->price; });
    const ObjectId sim = findSim(m);
    const Object* me = sim ? m.object(sim) : nullptr;
    if (!me)
        return {};
    const int sx = int(me->position.x), sy = int(me->position.y), level = me->position.level;
    for (const CatalogEntry* e : byPrice) {
        ObjectId ghost = 0;
        try {
            ghost = createGhost(m, e->group, e->instance);
        } catch (const std::exception& err) {
            os2::logOnce("tutorial ghost", e->name, err);
            continue;
        }
        const Object* g = m.object(ghost);
        if (!g || g->data[objdata::Category] != category) {
            deleteGroup(m, ghost);
            continue;
        }
        for (int r = 1; r <= 8; ++r)
            for (int dx = -r; dx <= r; ++dx)
                for (int dy = -r; dy <= r; ++dy) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r)
                        continue;
                    for (int dir = 0; dir < 8; dir += 2) {
                        const Placement p = checkPlacement(m, ghost, {sx + dx, sy + dy, level}, dir);
                        if (p.ok && buy(m, ghost, p, e->price))
                            return e->name + " at (" + std::to_string(sx + dx) + ", " + std::to_string(sy + dy) + ")";
                    }
                }
        deleteGroup(m, ghost);
        return {};
    }
    return {};
}

std::string tutorialAutoAct(VM& vm, ResourceIndex* index) {
    auto said = [](const std::string& what, const std::string& done, const char* verb) {
        return "the step waits for " + what + ": " + (done.empty() ? std::string("nothing to do") : verb + (" \"" + done + "\""));
    };
    if (const int category = tutorialAwaitedCategory(vm); category >= 0)
        return said("an object of category " + std::to_string(category),
                    index ? tutorialBuyCategory(vm, *index, category) : std::string(), "bought");
    if (const uint32_t guid = tutorialAwaitedGuid(vm)) {
        char hex[16];
        std::snprintf(hex, sizeof hex, "0x%08X", guid);
        return said(std::string("an interaction with ") + hex, tutorialAct(vm, guid), "queued");
    }
    // Person data 89: the job's GUID ("find a job"); others come by
    // themselves (a skill point while exercising).
    if (const int pd = tutorialAwaitedPersonData(vm); pd == 89)
        return said("a job (person data 89)", tutorialActNamed(vm, "Emprego"), "queued");
    return {};
}

} // namespace os2::vm

