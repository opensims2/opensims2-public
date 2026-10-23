// Animation primitives: Animate Sim/Object/Overlay/Stop, Assign Locomotion
// Animations and Effect Stop/Start. Operand layouts are inferred from how
// the game's trees use them (`s2pkg prim-usage`) and from the strings of the
// SimAntics editor's disassembler left in Sims2.exe; see TODO.md (M5) for
// what is inferred.
#include "engine.hpp"
#include "formats/log_once.hpp"
#include "animation.hpp"

#include "errors.hpp"
#include "interactions.hpp"
#include "locomotion.hpp"
#include "placement.hpp"
#include "world.hpp"

#include "vm.hpp"

#include "formats/binreader.hpp"
#include "formats/resindex.hpp"
#include "formats/sim_actor.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstdlib>

namespace os2::vm {

std::optional<float> AnimationLibrary::seconds(const std::string& name) {
    if (auto it = cache_.find(name); it != cache_.end())
        return it->second;
    std::optional<float> out;
    if (!index_)
        return out; // not cached: the library may get its index later (a route planned first)
    {
        try {
            const Animation a = loadAnimation(*index_, name);
            out = float(a.duration) / Animation::kTicksPerSecond;
            fullBody_[name] = a.flags & 1;
        } catch (const AnimationNotFound&) { // (an answer: the routes ask for variants the game may not have)
        } catch (const std::exception& err) {
            os2::logOnce("animation", name, err);
        }
    }
    cache_[name] = out;
    return out;
}

std::shared_ptr<const Animation> AnimationLibrary::animation(const std::string& name) {
    if (auto it = anims_.find(name); it != anims_.end())
        return it->second;
    if (!index_)
        return nullptr;
    std::shared_ptr<const Animation> a;
    try {
        a = std::make_shared<const Animation>(loadAnimation(*index_, name));
    } catch (const AnimationNotFound&) { // (an answer: the routes ask for variants the game may not have)
    } catch (const std::exception& err) {
        os2::logOnce("animation", name, err);
    }
    anims_[name] = a;
    return a;
}

float AnimationLibrary::rootTravel(const std::string& name, float restY) {
    if (auto it = travel_.find(name); it != travel_.end())
        return it->second;
    auto it = rootEnd_.find(name);
    if (it == rootEnd_.end()) {
        std::optional<float> end;
        if (index_)
            try {
                const Animation a = loadAnimation(*index_, name);
                for (const auto& t : a.targets)
                    for (const auto& ch : t.channels)
                        if (ch.name == "root_trans" && ch.components.size() >= 2 && !ch.components[1].keys.empty())
                            end = ch.components[1].sample(float(ch.duration()), ch.duration());
            } catch (const AnimationNotFound&) { // (an answer: the routes ask for variants the game may not have)
            } catch (const std::exception& err) {
                os2::logOnce("animation", name, err);
            }
        it = rootEnd_.emplace(name, end).first;
    }
    if (!it->second)
        return 0;
    const float d = *it->second - restY;
    return std::fabs(d) < 0.5f ? 0.0f : d; // the loops' ends sit within a few hundredths of the rest place
}

float AnimationLibrary::tilesPerSecond(const std::string& name) {
    if (auto it = speeds_.find(name); it != speeds_.end())
        return it->second;
    float v = 0;
    if (!index_)
        return v;
    {
        try {
            v = loadAnimation(*index_, name).velocity * Animation::kTicksPerSecond;
        } catch (const AnimationNotFound&) { // (an answer: the routes ask for variants the game may not have)
        } catch (const std::exception& err) {
            os2::logOnce("animation", name, err);
        }
    }
    speeds_[name] = v;
    return v;
}

const LocoClip* AnimationLibrary::loco(const std::string& name, int ticksPerSecond) {
    if (auto it = loco_.find(name); it != loco_.end())
        return it->second ? &*it->second : nullptr;
    if (!index_)
        return nullptr; // not cached: the index may come later
    std::optional<LocoClip> out;
    try {
        const Animation a = loadAnimation(*index_, name);
        LocoClip c;
        c.ticks = a.duration;
        c.distance = a.locomotionDistance;
        c.velocity = a.velocity * Animation::kTicksPerSecond;
        c.turn = a.turnRotation;
        c.cycle = a.cyclePortionCompleted;
        c.type = a.locomotionType;
        // The root: root_trans' translation channel (the offset from the
        // frame; the skeletons' root_trans rests at x = y = 0) and its facing,
        // root_trans' rest rotation (the same in every Sim skeleton) then
        // root_rot's Euler channel, forward being the root's local +Z.
        const Animation::Channel *trans = nullptr, *rot = nullptr;
        for (const auto& t : a.targets)
            for (const auto& ch : t.channels) {
                if (ch.components.size() < 3)
                    continue;
                if (ch.name == "root_trans" && ch.attribute() == Animation::Translation &&
                    ch.type() == Animation::TransformXYZ)
                    trans = &ch;
                else if (ch.name == "root_rot" && ch.attribute() == Animation::Rotation &&
                         ch.type() == Animation::EulerRotation)
                    rot = &ch;
            }
        const Xform rest{{0, 0.70710678f, 0.70710678f, 0}, {0, 0, 0}};
        const float step = Animation::kTicksPerSecond / float(std::max(ticksPerSecond, 1));
        const int n = int(std::ceil(float(a.duration) / step));
        for (int k = 0; k <= n; ++k) {
            const float t = std::min(float(k) * step, float(a.duration));
            std::array<float, 3> r{0, 0, 0};
            if (trans)
                r[0] = trans->components[0].sample(t, trans->duration()),
                r[1] = trans->components[1].sample(t, trans->duration());
            Quat q = rest.q;
            if (rot)
                q = q * rot->rotation(t);
            const Vec3 f = Xform{q, {0, 0, 0}}.rotate({0, 0, 1});
            r[2] = std::atan2(-f[0], f[1]);
            c.root.push_back(r);
        }
        out = std::move(c);
    } catch (const AnimationNotFound&) { // (an answer: the routes ask for variants the game may not have)
    } catch (const std::exception& err) {
        os2::logOnce("animation", name, err);
    }
    auto& slot = loco_[name];
    slot = std::move(out);
    return slot ? &*slot : nullptr;
}

std::optional<int> AnimationLibrary::eventTicks(const std::string& name, const std::string& line) {
    const std::string key = name + '\n' + line;
    if (auto it = events_.find(key); it != events_.end())
        return it->second;
    std::optional<int> out;
    if (!index_)
        return out;
    {
        try {
            for (const auto& e : loadAnimation(*index_, name).events) {
                const std::string first = e.data.substr(0, e.data.find_first_of("\r\n"));
                if (first == line) {
                    out = e.ticks;
                    break;
                }
            }
        } catch (const AnimationNotFound&) { // (an answer: the routes ask for variants the game may not have)
        } catch (const std::exception& err) {
            os2::logOnce("animation", name, err);
        }
    }
    events_[key] = out;
    return out;
}

const std::vector<std::pair<int, int16_t>>& AnimationLibrary::treeEvents(const std::string& name) {
    if (auto it = treeEvents_.find(name); it != treeEvents_.end())
        return it->second;
    auto& out = treeEvents_[name];
    if (index_) {
        try {
            for (const auto& e : loadAnimation(*index_, name).events) {
                if (e.type != 1005 || e.data.rfind("event=", 0) != 0)
                    continue;
                const auto end = e.data.find_first_of("|\r\n");
                try {
                    out.push_back({e.ticks, int16_t(std::stoi(e.data.substr(6, end == std::string::npos ? end : end - 6)))});
                } catch (const std::logic_error& err) { // not a number (std::stoi): the event is left out
                    os2::logOnce("animation tree event", name + ": " + e.data, err);
                }
            }
        } catch (const AnimationNotFound&) { // (an answer: the routes ask for variants the game may not have)
        } catch (const std::exception& err) {
            os2::logOnce("animation", name, err);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

std::string agePrefix(const Object& sim) {
    if (!sim.person)
        return "a";
    switch (sim.person->data[persondata::PersonAge]) {
    case lifestage::Baby: return "b";
    case lifestage::Toddler: return "p";
    case lifestage::Child: return "c";
    case lifestage::Teen: return "t";
    case lifestage::Elder: return "e";
    default: return "a";
    }
}

std::string genderName(const Object& sim) {
    constexpr int16_t kGender = 65;
    return sim.person && sim.person->data[kGender] == 1 ? "female" : "male";
}

namespace {

constexpr uint8_t kOwnerParam = 0x09;

AnimationLibrary& library(VM& vm) {
    if (!vm.animations.index())
        vm.animations.setIndex(vm.code().index());
    return vm.animations;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    return s;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t end = s.find(sep, start);
        out.push_back(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos)
            return out;
        start = end + 1;
    }
}

// "walk-alt0-normal(walk)", "turnInPlace-alt@1.5(turnInPlace@1.5)": the
// name and the role, without the speeds.
struct LocoItem {
    std::string name, role;
};
LocoItem parseItem(const std::string& item) {
    LocoItem out;
    std::string s = trim(item);
    if (const size_t open = s.find('('); open != std::string::npos) {
        const size_t close = s.find(')', open);
        out.role = trim(s.substr(open + 1, close == std::string::npos ? std::string::npos : close - open - 1));
        s = trim(s.substr(0, open));
    }
    if (const size_t at = s.find('@'); at != std::string::npos)
        s.resize(at);
    if (const size_t at = out.role.find('@'); at != std::string::npos)
        out.role.resize(at);
    out.name = trim(s);
    return out;
}

// The global group's list for a Sim's age (Mac binary,
// DetermineAppropriateAnimTable, person data 58): >= 51 elder 0x8B, 19-50
// adult 0x81, 16-18 teen 0x8A, 3-15 child 0x82, 2 toddler 0x89, < 2 baby
// 0x91. The elder and teen lists fall back to the adult one (`reserve`).
uint16_t globalListFor(const Object& sim, bool reserve = false) {
    const int16_t age = sim.person ? sim.person->data[persondata::PersonAge] : lifestage::Adult;
    if (age == 0)
        return animlist::Adult;
    if (age >= 51)
        return reserve ? animlist::Adult : animlist::Elder;
    if (age >= 19)
        return animlist::Adult;
    if (age >= 16)
        return reserve ? animlist::Adult : animlist::Teen;
    if (age >= 3)
        return animlist::Child;
    if (age == 2)
        return animlist::Toddler;
    return animlist::Baby;
}

// The age letter of the Sim-to-Sim prefix ("a2a-", list 0x83), by the same
// ranges ("a" when the age is not set).
char ageLetter(const Object& sim) {
    const int16_t age = sim.person ? sim.person->data[persondata::PersonAge] : lifestage::Adult;
    if (age == 0 || (age >= 19 && age < 51))
        return 'a';
    if (age >= 51)
        return 'e';
    if (age >= 16)
        return 't';
    if (age >= 3)
        return 'c';
    return age == 2 ? 'p' : 'b';
}

bool placeholder(const std::string& e) {
    std::string t = e;
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t'))
        t.erase(t.begin());
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t'))
        t.pop_back();
    return t.empty() || t == ".";
}

// STR# `list` of the object whose behavior holds the running tree, and its
// semi-global's where that entry is empty (the Mac binary: try_animate_sim
// 0x69d634 and try_animate_overlay 0x69ea54 take the selector from the
// stack element's tree, cTSTreeStackElem +0x20 (vtable 0x50) -> the tree's
// cITSBehavior (vtable 0x94) -> cTSObjectFolder::GetSelectorByBehavior
// 0x4e56b8; FindSpecificName 0x690700 reads that selector's list and, for
// an empty entry, its semi-global's). Each object type's cTSBehavior holds
// its private, semi-global and global trees, so this is the code owner's
// type, or the group of a private tree run for another object (an
// animation's event tree). Not the stack object's: "Event - Sit Carry -
// Trans Stand - Start" of a drink, sitting on a bar stool, reads the
// drink's entry 9 "a2o-trans-drinkCarry-SlotIKpose", not the stool's
// " ~Get Out" label. The lists' "~" rows are labels the binary indexes
// like any other (cTSAnimTable::GetEntry 0x594c6c: entry index + 1). Null
// if neither has the entry.
const StringTable* objectList(Context& c, uint16_t list, int index) {
    CodeLibrary& code = c.vm.code();
    uint32_t group = 0, semi = 0;
    const Object* owner = c.vm.object(c.frame.codeOwner);
    if (owner && (c.frame.group == owner->group || c.frame.group == owner->semiGroup || c.frame.group == kGlobalGroup))
        group = owner->group, semi = owner->semiGroup;
    else
        group = c.frame.group, semi = code.semiGlobalFor(group);
    const StringTable* first = nullptr;
    for (uint32_t g : {group, semi}) {
        const StringTable* t = g ? code.strings(g, list) : nullptr;
        if (!t || index < 0 || size_t(index) >= t->count(lang::EnglishUS))
            continue;
        if (!first)
            first = t;
        if (!t->get(size_t(index), lang::EnglishUS).empty())
            return t;
    }
    return first;
}

// Entry `index` of animation list `list` for code running in `frame`, as the
// Mac binary's try_animate_sim finds it: 0x80 = the global group's list for
// the Sim's age (FindGlobalName; an elder/teen entry whose animation does
// not exist retries in the adult list); any other list is STR# `list` of
// the running tree's object and, where its entry is empty, of that object's
// semi-global (objectList, FindSpecificName). Blank and "." entries are
// placeholders for the age retries (the chair semi-global's 0x81 is all
// blank; the reaction semi-global's lists are "." up to 19).
std::string listEntry(Context& c, const Object& sim, uint16_t list, int index) {
    CodeLibrary& code = c.vm.code();
    const StringTable* table = nullptr;
    if (list == animlist::Global) {
        table = code.strings(kGlobalGroup, globalListFor(sim));
        if (table && index >= 0 && size_t(index) < table->count(lang::EnglishUS)) {
            const std::string e = table->get(size_t(index), lang::EnglishUS);
            if (globalListFor(sim) != animlist::Adult &&
                (placeholder(e) || !library(c.vm).exists(parseItem(split(e, ',')[0]).name)))
                table = code.strings(kGlobalGroup, globalListFor(sim, true));
        } else if (globalListFor(sim) != animlist::Adult) {
            // Past the end of the age's list: its reserve, and the adult list
            // for a baby, toddler or child (inferred: "Anims - Baby" stops at
            // 140, inside the blinks "Facial - Background - Start Anim"
            // picks from 135-165, and its own blinks are the adult ones).
            table = code.strings(kGlobalGroup, globalListFor(sim, true));
            if (!table || size_t(index) >= table->count(lang::EnglishUS))
                table = code.strings(kGlobalGroup, animlist::Adult);
        }
    } else {
        // An object's list 0x81 is the Sim's age list too
        // (DetermineAppropriateAnimTable: a child reads the object's 0x82,
        // "c2o-bed-jump-start"); a teen's or elder's that has no animation
        // there retries in 0x81 (the binary's reserve table).
        const bool byAge = list == animlist::Adult && sim.person;
        table = objectList(c, byAge ? globalListFor(sim) : list, index);
        if (byAge && globalListFor(sim, true) != globalListFor(sim)) {
            const std::string e = table ? table->get(size_t(index), lang::EnglishUS) : "";
            if (!table || placeholder(e) || !library(c.vm).exists(resolveListAnimation(c.vm, sim, e)))
                if (const StringTable* adult = objectList(c, animlist::Adult, index))
                    table = adult;
        }
        if (!table && byAge)
            table = objectList(c, list, index);
    }
    if (!table)
        throw VMError("no animation list " + hexString(list));
    if (index < 0 || size_t(index) >= table->count(lang::EnglishUS))
        throw VMError("animation " + std::to_string(index) + " out of range of list " + hexString(list) + " (" +
                      std::to_string(table->count(lang::EnglishUS)) + ")");
    return table->get(size_t(index), lang::EnglishUS);
}

// List 0x83 holds Sim-to-Sim animations without their prefix: the name is
// "<my age>2<the other Sim's age>-" + the entry ("a2a-soc-entertain-...";
// GetAnimPrefixStart/End in the Mac binary: the other is the Sim of the
// interaction, here the stack object, else the object of my current
// interaction; not a Sim = error 0x5A). When that
// animation does not exist my age becomes "a" (the binary's reserve).
std::string socialName(Context& c, const Object& me, const std::string& entry) {
    // The other Sim is the target of my current sub-action, else of my
    // current action (DetermineAppropriateAnimTable: GetTargetObject of the
    // person's current (sub)action; Push Interaction's flag 0x01 sets it, as
    // the social objects' "Person A" pushes do); without one, the stack
    // object (inferred).
    const Object* other = nullptr;
    auto targetOf = [&](const QueuedInteraction& q) { return c.vm.object(q.target ? q.target : q.object); };
    if (me.subAction)
        other = targetOf(*me.subAction);
    else if (me.activeInteraction >= 0 && size_t(me.activeInteraction) < me.queue.size())
        other = targetOf(me.queue[size_t(me.activeInteraction)]);
    else
        other = c.vm.object(c.frame.stackObject);
    if (!other || !other->person)
        throw gameError(0x5A, "Sim-to-Sim animation without a Sim: stack object " + std::to_string(c.frame.stackObject) +
                                  ", target " + std::to_string(other ? other->id : 0));
    // When "<mine>2<theirs>-" does not exist, the binary retries with my age
    // as "a" (the reserve table) and then swaps the age letters for others
    // (FUN_0068bcb0); here: mine as "a", theirs as "a" (an elder's are the
    // adult ones: "a2e-soc-hugFriendly-hugger-reject" is missing,
    // "a2a-..." exists; inferred), then both.
    const std::string item = parseItem(split(entry, ',')[0]).name;
    const char mine = ageLetter(me), theirs = ageLetter(*other);
    auto named = [&](char a, char b) { return std::string(1, a) + "2" + b + "-" + item; };
    for (const auto& [a, b] : {std::pair{mine, theirs}, std::pair{'a', theirs}, std::pair{mine, 'a'}, std::pair{'a', 'a'}})
        if (library(c.vm).exists(named(a, b)))
            return named(a, b);
    return named(mine, theirs);
}

// The animation index of an operand: u16 at `at`, or the parameter of that
// number when the flag says so ("anim id in s.v.").
int animationIndex(Context& c, size_t at, bool fromParam) {
    const auto v = int16_t(le16(&c.instruction.operands[at]));
    return fromParam ? c.get(kOwnerParam, v) : v;
}

Object& objectAt(Context& c, size_t ownerAt) {
    const auto& o = c.instruction.operands;
    Object* x = c.vm.object(c.get(o[ownerAt], int16_t(le16(&o[ownerAt + 1]))));
    if (!x)
        throw VMError("Invalid object id.");
    return *x;
}

int ticksFor(VM& vm, float seconds) {
    return int(std::ceil(double(seconds) * vm.clock.ticksPerSecond - 1e-6));
}

// The speed of an Animate Sim/Object/Overlay operand: byte 3 in 1/32 (0x20
// = normal), or temp 2 with flag 0x02 of byte 2 (Mac binary). Taken as
// signed (0xE0, 0xC0 in ~4% of the uses: backwards, inferred), so only its
// size changes how long it plays. 0 is the game's error 0x7E.
float animationSpeed(Context& c) {
    const auto& o = c.instruction.operands;
    const int v = o[2] & 0x02 ? int(c.thread.temps[2]) : int(int8_t(o[3]));
    if (v == 0)
        throw VMError("Animation speed cannot be zero");
    return std::abs(float(v)) / 32.0f;
}

// Animate Sim (0x6A): plays a full-body animation on me and waits for it to
// end (Mac binary: it returns 2, "wait", until the animation is over, then
// 1). Operands (editor disassembler, docs/primitives-mac.md §5): animation
// index u16 at 0, flags u8 at 2 (0x01 flipped, 0x02 speed in temp 2, 0x04
// the index is in the parameter of that number, "anim id in s.v.", 0x08
// interruptible, 0x10 start at the tag in temp 0, 0x20 "Trans to Idle",
// 0x40 no blend out, 0x80 no blend in), speed u8 at 3 (1/32: 0x20 = 1.0;
// animationSpeed), event tree u16 at 4 (a call opcode, 0 none; s8 7 says
// where it is: 0 private, 1 semi-global, 2 global), the list u8 at 6 (0x80
// the global list for the Sim's age, else that STR#), flags u8 at 8 (0x01
// flip flag in temp 3, 0x02 sync to the last animation, 0x10 the
// controlling object as the source, 0x20 not hurryable), the IK object
// (owner u8 at 9, data u16), priority s8 at 12 (0 low, 1 medium, 2 high)
// and a blend weight byte 13 / 100. Only the speed and the index are used
// here; the animation's events would run the event tree, and they are not
// read. True when the animation ends; the game's error for an empty name;
// an animation that does not exist ends at once (counted in
// AnimationLibrary::missing).
// An animation request's event tree, as the game's event handler runs it
// (nTSSG::cCustomEventHandler2::HandleEvents, Mac BV 0x780ad4,
// docs/anim-mac.md §10): on the animated object, with the code of the
// request's tree (its ObjectScopeGUID) and the request's stack object, at
// once (cTSObject::RunTree -> RunOneTickTree), with the object's temps put
// back afterwards (RunTree's last bool: a tree's temps do not leak into the
// thread that is running), and five parameters: the event (the ANIM's
// "event=N", or the engine's 9997 blending out, 9998 end of a play, 9999
// end, -1 stopped), the request's stack object, how much of the animation
// has played in percent (int(PortionCompleted x 100)), the request's loop
// count (NumLoops) and whether it was aborted (IsAborted).
// OS2_DEBUG_ANIM_EVENTS=1 lists them.
bool runEventTree(VM& vm, ObjectId me, ObjectId codeOwner, ObjectId stackObject, uint16_t tree,
                  const std::array<int16_t, kTempCount>& temps, int16_t event, float portion, int16_t loops,
                  bool aborted) {
    const int16_t params[5] = {event, int16_t(stackObject), int16_t(int(portion * 100.0f)), loops,
                               int16_t(aborted ? 1 : 0)};
    if (std::getenv("OS2_DEBUG_ANIM_EVENTS"))
        std::fprintf(stderr, "anim event: object %d tree 0x%04X code %d stack object %d params %d %d %d %d %d\n", me,
                     tree, codeOwner, stackObject, params[0], params[1], params[2], params[3], params[4]);
    auto copy = temps;
    return runOneTickTree(vm, copy, me, codeOwner, stackObject, tree, params);
}

// The temps of `id`'s Main thread (the object's temps in the game), zero
// without one.
std::array<int16_t, kTempCount> objectTemps(const VM& vm, ObjectId id) {
    const auto t = vm.threads.find(id);
    return t == vm.threads.end() ? std::array<int16_t, kTempCount>{} : t->second.temps;
}

// VM ticks until `ms` of real time have passed (ticksFor's rounding).
int ticksForMs(const VM& vm, double ms) {
    return ms <= 0 ? 0 : int(std::ceil(ms / 1000.0 * vm.clock.ticksPerSecond - 1e-6));
}
// An Animate Sim's ticks to its end and to its action end (the blend out's
// start), from what it has played at its speed.
void retimeAnimateSim(const VM& vm, AnimationState& a) {
    const double left = std::max(0.0, double(a.lengthTicks - a.playedTicks) / double(a.speed));
    a.ticksLeft = ticksForMs(vm, left);
    a.actionTicksLeft = a.fadeOutMs > 0 ? ticksForMs(vm, left - a.fadeOutMs) : a.ticksLeft;
}
enum class AnimEvents { Progress, ActionEnd, End };

// Runs the event tree for the tree events the animation has reached (all
// of them once it ends) and the engine's: 9997 when its blend out starts
// (the action end; never without a blend out: cActiveAnimEvent::
// AnimUpdateDefault, BV 0xd38ffc, posts it only once HasFadeOutStartTicks
// Passed), 9998 and 9999 at its end (BV 0xd38bec makes those keys for
// every request; the Animate Sim's trees handle them too: "Event - Get",
// "Event - Clean", "Event Tree - Read Stop", ... s2pkg event-tree-sweep).
// In my thread (I am the animated object); a tree that fails fails the
// Animate Sim.
void runAnimationEvents(Context& c, AnimationState& a, AnimEvents mode) {
    const bool all = mode == AnimEvents::End;
    const float length = std::max(a.lengthTicks, 1.0f);
    // Where the blend out starts, in animation ticks played.
    const float fadeAt = a.fadeOutMs > 0 ? a.lengthTicks - a.fadeOutMs * a.speed : std::numeric_limits<float>::max();
    auto fire = [&](int16_t event, float portion) {
        if (a.eventTree)
            runEventTree(c.vm, c.thread.me, c.frame.codeOwner, a.eventStackObject, a.eventTree, c.thread.temps, event,
                         std::clamp(portion, 0.0f, 1.0f), 1, false);
    };
    for (;;) {
        const bool tree = !a.pendingEvents.empty() && (all || a.pendingEvents.front().first <= a.playedTicks);
        const bool fade = !a.blendOutSent && a.fadeOutMs > 0 &&
                          (mode != AnimEvents::Progress || a.playedTicks >= fadeAt);
        if (fade && (!tree || float(a.pendingEvents.front().first) > fadeAt)) {
            a.blendOutSent = true;
            fire(animevent::BlendOut, a.playedTicks / length);
            continue;
        }
        if (!tree)
            break;
        const int16_t event = a.pendingEvents.front().second;
        a.pendingEvents.erase(a.pendingEvents.begin());
        fire(event, a.playedTicks / length);
    }
    if (all) {
        fire(animevent::ActionEnd, 1.0f);
        fire(animevent::End, 1.0f);
    }
}

// Runs an overlay's event tree for `event` (runEventTree).
void runOverlayEvent(VM& vm, const Overlay& o, int16_t event, bool aborted) {
    if (!o.eventTree || !vm.object(o.me) || !vm.object(o.codeOwner))
        return;
    const float portion = o.length > 0 ? std::fmod(o.played, o.length) / o.length : 0.0f;
    try {
        runEventTree(vm, o.me, o.codeOwner, o.stackObject, o.eventTree, objectTemps(vm, o.me), event, portion,
                     o.requestLoops, aborted);
    } catch (const VMError&) {
        // the error goes to the animated object's tree sim in the game
        // (cTSTreeSim::RunOneTickTree); here it is dropped (inferred)
    }
}

// The Animate Sim's request starts to blend out, or ends without a blend
// out: its action has ended and the primitive is done (OnPreSimulatorTick,
// BV 0x6980d8 / base 0x94765c, sets the request's ActualEndActionTicks to
// its ActualFadeOutStart, or its end; try_animate_sim returns 1 once it is
// set; docs/anim-mac.md §13.3). The Sim keeps when it ends and when its
// action ended for what comes next (the blocking primitive's EndTicks and
// EndActionTicks); the request plays on, blending out, with the rest of its
// tree events and 9998 and 9999 at its end (AnimationState::tails).
void endAnimateSimAction(Context& c, AnimationState& a) {
    const double now = animationClockMs(c.vm);
    const double end = now + double(a.lengthTicks - a.playedTicks) / double(a.speed); // its start + duration
    a.previousEndMs = end;
    a.previousActionEndMs = a.fadeOutMs > 0 ? end - double(a.fadeOutMs) : end;
    a.previousPortion = 0.5f; // no locomotion in it
    if (a.ticksLeft > 0) {
        runAnimationEvents(c, a, AnimEvents::ActionEnd);
        if (a.eventTree) {
            AnimationState::Tail t;
            t.name = a.name;
            t.me = c.thread.me, t.codeOwner = c.frame.codeOwner, t.stackObject = a.eventStackObject;
            t.eventTree = a.eventTree;
            t.pending = std::move(a.pendingEvents);
            t.played = a.playedTicks, t.perTick = a.ticksPerVmTick, t.length = a.lengthTicks;
            t.speed = a.speed, t.hurry = a.hurry;
            t.ticksLeft = a.ticksLeft;
            t.madeAt = c.vm.ticks;
            a.tails.push_back(std::move(t));
        }
    } else {
        runAnimationEvents(c, a, AnimEvents::End);
    }
    a.pendingEvents.clear();
    a.name.clear();
    a.tree = nullptr;
    a.requestId = 0;
}

Exit animateSim(Context& c) {
    Object& me = c.me();
    AnimationState& a = me.animation;
    const size_t depth = c.thread.stack.size();
    if (a.playing() && a.depth == depth && a.tree == c.frame.behavior && a.node == c.frame.pc) {
        a.playedTicks += a.ticksPerVmTick;
        --a.ticksLeft;
        if (--a.actionTicksLeft > 0) {
            runAnimationEvents(c, a, AnimEvents::Progress);
            return Exit::Yield;
        }
        endAnimateSimAction(c, a);
        return Exit::True;
    }
    const auto& o = c.instruction.operands;
    const std::string entry = listEntry(c, me, o[6], animationIndex(c, 0, o[2] & 0x04));
    const std::string name = o[6] == animlist::Social && !placeholder(entry) ? socialName(c, me, entry)
                                                                             : resolveListAnimation(c.vm, me, entry);
    const auto seconds = library(c.vm).seconds(name);
    if (!seconds && name != ".")
        ++c.vm.animations.missing[name];
    // A full-body animation does not take the overlays off: the engine
    // mixes them with it bone by bone by their levels (animlevel; Mac BV:
    // nothing in try_animate_sim or AddChoreoAnimRequest touches the other
    // requests).
    a.name = name;
    ++a.serial;
    // Byte 8 flag 0x01: temp 3 != 0 instead of the literal (try_animate_sim, BV 0x69d634).
    a.flipped = (o[8] & 0x01) ? c.thread.temps[3] != 0 : (o[2] & 0x01) != 0;
    a.level = animlevel::ofSim(int8_t(o[12]));
    const float weight = float(int8_t(o[13])) / 100.0f;
    a.weight = weight <= 0 || weight > 1 ? 1.0f : weight;
    a.depth = depth, a.tree = c.frame.behavior, a.node = c.frame.pc;
    a.pendingEvents = library(c.vm).treeEvents(name);
    a.eventTree = le16(&o[4]);
    a.eventStackObject = c.frame.stackObject;
    a.lengthTicks = seconds ? *seconds * 1000.0f : 0.0f;
    a.blendOutSent = false;
    a.headingOffset = (o[8] & 0x04) ? animlevel::headingOfDirections(c.thread.temps[4]) : 0.0f;
    a.eventObject = 0;
    try {
        a.eventObject = c.get(o[9], int16_t(le16(&o[10])));
    } catch (const VMError&) {
    }
    a.speed = animationSpeed(c);
    a.ticksPerVmTick = float(1000.0 / c.vm.clock.ticksPerSecond) * a.speed;
    // Its weight function: blended in and out over the blend time unless
    // byte 2 says "no blend in" (0x80) or "no blend out" (0x40), shrunk in
    // a short one (CreateTransitionBlend).
    const RequestFades fades = requestFades(a.lengthTicks / a.speed, !(o[2] & 0x80), !(o[2] & 0x40));
    a.fadeInMs = fades.in, a.fadeOutMs = fades.out;
    a.hurry = (o[8] & 0x20) ? 0.0f : 1.0f; // byte 8 0x20: not hurryable
    // Byte 8 0x02 starts it at the end of the last blocking primitive's
    // request and 0x08 at its action's end, when that is within 140 ms of
    // now (else now); the request is made in the past or the future by that
    // much (try_animate_sim; base 1.0 0x952f60).
    double offset = 0;
    const double now = animationClockMs(c.vm);
    auto sync = [&](double at) {
        if (at == AnimationState::kNoTicks)
            return;
        offset = std::abs(at - now) <= kSyncWindowMs ? at - now : 0.0;
    };
    if (o[8] & 0x02)
        sync(a.previousEndMs);
    if (o[8] & 0x08)
        sync(a.previousActionEndMs);
    a.playedTicks = float(-offset) * a.speed;
    // The request's ID, kept in the stack frame's primitive state
    // (try_animate_sim: the counter goes up first; bit 0 when flipped).
    ++c.vm.animationCounter;
    a.requestId = (a.flipped ? 0x40000001u : 0x40000000u) | ((c.vm.animationCounter & 0xFFu) << 21);
    c.frame.primitiveState = int32_t(a.requestId);
    if (seconds) {
        a.previousEndMs = a.previousActionEndMs = AnimationState::kNoTicks;
        // Its end and its action's end, from now; at the earliest in the
        // next tick (the request is updated before the trees run).
        const double duration = double(*seconds) * 1000.0 / double(a.speed) + offset;
        a.ticksLeft = std::max(ticksForMs(c.vm, duration), 1);
        a.actionTicksLeft =
            a.fadeOutMs > 0 ? std::max(ticksForMs(c.vm, duration - double(a.fadeOutMs)), 1) : a.ticksLeft;
        runAnimationEvents(c, a, AnimEvents::Progress);
        return Exit::Yield;
    }
    // A missing animation ends at once.
    a.ticksLeft = a.actionTicksLeft = 0;
    runAnimationEvents(c, a, AnimEvents::End);
    a.name.clear();
    a.tree = nullptr;
    a.requestId = 0;
    return Exit::True;
}

// Adds an overlay animation to an object (Animate Overlay, Animate Object):
// they do not block the thread (inferred: "Anim - Sim & Object by Index"
// starts the object's animation, plays the Sim's and then stops the
// object's). Its kind comes from the entry's role (oeTypeOfRole). Null when
// two of that name already play: try_animate_overlay looks the name up
// twice (AnimRequestByNameAndID) and starts nothing more then, so an idle
// started again while the last one blends out does not pile up.
Overlay* startOverlay(Context& c, Object& target, const std::string& entry, bool flipped = false, bool social = false,
                      const Object* ageOf = nullptr) {
    // A Sim-to-Sim list's entry takes the ages' prefix as Animate Sim's
    // ("soc-playRedHands-playee-loopOw" -> "a2a-soc-..."; the social
    // overlays of the play-fight and gross-out interactions).
    std::string name = target.person && social && !placeholder(entry) ? socialName(c, target, entry)
                       : target.person ? resolveListAnimation(c.vm, ageOf ? *ageOf : target, entry)
                                                                       : parseItem(split(entry, ',')[0]).name;
    if (trim(name).empty())
        throw VMError("Animation name is empty string.");
    if (name != "." && !library(c.vm).exists(name))
        ++c.vm.animations.missing[name];
    auto& ov = target.animation.overlays;
    if (std::count(ov.begin(), ov.end(), name) >= 2)
        return nullptr;
    Overlay& o = target.animation.addOverlay(name);
    o.oeType = oeTypeOfRole(parseItem(split(entry, ',')[0]).role);
    o.flipped = flipped;
    if (ov.size() > 8)
        ov.erase(ov.begin());
    ++target.animation.overlaySerial;
    return &ov.back();
}

// The loop count of Animate Object/Overlay with flag 0x20 of byte 2 is in
// temp 1; 0 or less than -1 is the game's error 0x70 (Mac binary).
void checkLoops(Context& c) {
    if ((c.instruction.operands[2] & 0x20) && (c.thread.temps[1] == 0 || c.thread.temps[1] < -1))
        throw gameError(err::BadLoopCount, std::to_string(c.thread.temps[1]));
}

// Animate Overlay (0x6B): animation index u16 at 0, flags u8 at 2 (as
// Animate Sim's: 0x02 speed in temp 2, 0x04 index in a parameter; 0x20 loop
// count in temp 1), speed u8 at 3, event tree u16 at 4 (s8 14 where), the
// object (owner u8 at 6, data u16), the list u8 at 9 (as Animate Sim's byte
// 6: 0x80 global, 0x81 adult, 0x86 object, ...), priority s8 11, byte 12 bit
// 0 not hurryable, flags byte 15 (Mac binary, docs/primitives-mac.md §5).
// It does not block (the binary returns 1 once it has started it).
Exit animateOverlay(Context& c) {
    // The base game's trees are version 0 (AnimateOverlayParam::
    // UpgradeVersion, Mac BV 0xac4af8): byte 10 keeps the low byte of the
    // old u16 at 10, the priority (byte 11) is the low byte of the old u16
    // at 12, and bytes 12-13 become 0 ("CT - Set Carry"'s carry poses are
    // priority 2 there, not the -1 byte 11 holds).
    std::array<uint8_t, 16> o = c.instruction.operands;
    if (c.instruction.nodeVersion == 0) {
        o[11] = o[12];
        o[12] = o[13] = 0;
    }
    checkLoops(c);
    Object& target = objectAt(c, 6);
    // The global list (0x80) is the one of the age of the Sim running the
    // primitive, not the target's (try_animate_overlay 0x69ea54 calls
    // FindGlobalName on its own cAnimatable; the other lists go through
    // DetermineAppropriateAnimTable with the target person): "CT - Route
    // Failure Feedback - Animation" plays entry 90 on the baby in the
    // adult's arms, the adult list's "b2a-routeFail-carry-baby"; the baby's
    // own list has none, and every route failure with a baby in arms reset
    // the adult's Main ("Animation name is empty string").
    const Object& ageOf = o[9] == animlist::Global && c.me().person ? c.me() : target;
    const std::string entry = listEntry(c, ageOf, o[9], animationIndex(c, 0, o[2] & 0x04));
    // Flipped as Animate Sim's: flag 0x01 of byte 15 takes temp 3 (try_animate_overlay, BV 0x69ea54).
    Overlay* ov = startOverlay(c, target, entry, (o[15] & 0x01) ? c.thread.temps[3] != 0 : (o[2] & 0x01) != 0,
                               o[9] == animlist::Social, &ageOf);
    if (!ov)
        return Exit::True;
    ov->level = animlevel::ofOverlay(int8_t(o[11]));
    // It plays once, or temp 1 times with flag 0x20 (-1: until stopped); an
    // animation without length is a pose that holds (the carry poses).
    const float length = library(c.vm).seconds(ov->name).value_or(0) * 1000.0f;
    const int loops = o[2] & 0x20 ? c.thread.temps[1] : 1;
    ov->loops = length > 0 && loops > 0 ? loops : 0;
    ov->forever = loops == -1; // until stopped (saved as 0xFFFFFFFF loops, as the game's)
    ov->requestLoops = int16_t(loops);
    ov->length = length;
    ov->perTick = float(1000.0 / c.vm.clock.ticksPerSecond) * animationSpeed(c);
    // Its weight function (as Animate Sim's, requestFades): no blend out
    // for one that holds or loops until stopped.
    const RequestFades fades = requestFades(ov->loops > 0 ? length * float(ov->loops) / animationSpeed(c) : 1e9f,
                                            !(o[2] & 0x80), !(o[2] & 0x40));
    ov->fadeInMs = fades.in;
    ov->fadeOutMs = ov->loops > 0 ? fades.out : 0.0f;
    ov->hurry = (o[12] & 0x01) ? 0.0f : 1.0f; // byte 12 bit 0: not hurryable
    ov->pending = library(c.vm).treeEvents(ov->name);
    ov->eventTree = le16(&o[4]);
    // Its event tree runs on the animated object (the binary's event handler,
    // nTSSG::cCustomEventHandler2 BV 0x780ad4, runs it on its cAnimatable's
    // object, vtable 0x3ac, with the request's stack object; the request
    // keeps no "me": MovedObjectID is 0 in every saved request), with the
    // starting tree's code and stack object. The crib's "Animate - Events -
    // Get" snaps the toddler (the overlay's object) into the carrier's hands
    // on 9998; run as the carrier it snapped nothing and the toddler stayed
    // in the crib (N001_Lot36, 16-32 h).
    ov->me = target.id, ov->codeOwner = c.frame.codeOwner, ov->stackObject = c.frame.stackObject;
    // Its IK object: temp 6 with flag 0x08 of byte 15, else none
    // (try_animate_overlay, Mac BV 0x69ea54, passes it to
    // AddChoreoAnimRequest, whose IK targets of type 2 are its bones).
    ov->ikObject = (o[15] & 0x08) ? c.thread.temps[6] : int16_t(0);
    ov->headingOffset = (o[15] & 0x02) ? animlevel::headingOfDirections(c.thread.temps[4]) : 0.0f;
    return Exit::True;
}

// Animate Object (0x69): animation index u16 at 0, flags u8 at 2 (as
// Animate Overlay's), speed u8 at 3, event tree u16 at 4 (s8 9 where), the
// object (owner u8 at 6, data u16), from the object list (STR# 0x86) of the
// running tree's group or the code owner's; flags byte 10 (0x01 flip flag in
// temp 3, 0x04 sync to the caller's animation, 0x08 align the end, 0x80 not
// hurryable). It does not block (Mac binary).
Exit animateObject(Context& c) {
    const auto& o = c.instruction.operands;
    checkLoops(c);
    Object& target = objectAt(c, 6);
    const std::string entry = listEntry(c, target, animlist::Object, animationIndex(c, 0, o[2] & 0x04));
    if (trim(entry).empty() || trim(entry) == ".")
        return Exit::True; // the lists keep empty slots for objects with nothing to play there (inferred)
    // A vehicle drives by playing its drive loop again from its event tree
    // at each loop's end: the same animation started again replaces the one
    // playing (Animate Object's play until stopped, loops 0). With Animate
    // Overlay's "two of a name at most" the third loop did not start, its
    // event tree never got 9999 and "Wait for Vehicle Stop" waited forever:
    // the maid's van (and the school bus) stayed at the curb all day.
    {
        const std::string name = target.person ? std::string() : parseItem(split(entry, ',')[0]).name;
        auto& ov = target.animation.overlays;
        if (!target.person)
            c.vm.holdObjectPose(target); // where the animation playing left the bones
        if (!name.empty())
            std::erase_if(ov, [&](const Overlay& x) { return x.loops == 0 && x.name == name; });
    }
    // Mirrored with the Sim's get-in from the other side (flag 0x01 of
    // byte 10: the flip in temp 3, as Animate Sim's).
    Overlay* started = startOverlay(c, target, entry, (o[10] & 0x01) ? c.thread.temps[3] != 0 : (o[2] & 0x01) != 0);
    if (!started)
        return Exit::True;
    started->level = animlevel::Object;
    // Flag 0x20: temp 1 loops, -1 until stopped (checkLoops lets no other
    // value under 1 through); a count still plays once and holds (inferred).
    started->forever = (o[2] & 0x20) && c.thread.temps[1] == -1;
    // Its time, for the bones its slots stand at (VM::objectPose).
    started->length = library(c.vm).seconds(started->name).value_or(0) * 1000.0f;
    started->perTick = float(1000.0 / c.vm.clock.ticksPerSecond) * animationSpeed(c);
    started->fadeInMs = (o[2] & 0x80) ? 0.0f : kBlendTicks; // played until stopped: no blend out of its own
    started->fadeOutMs = 0;
    started->hurry = (o[10] & 0x80) ? 0.0f : 1.0f; // byte 10 0x80: not hurryable
    // Its event tree gets the animation's tree events as it plays, on the
    // animated object (as Animate Overlay's) (the vehicles drive on
    // "Event Tree - Drive": loop again or stop, which ends "Wait for Vehicle
    // Stop"). A new one from the same code replaces the last (inferred).
    if (const uint16_t tree = le16(&o[4])) {
        const std::string name = target.animation.overlays.back().name;
        auto& list = target.animation.overlayEvents;
        std::erase_if(list, [&](const auto& x) { return x.codeOwner == c.frame.codeOwner; });
        AnimationState::OverlayEvents ev;
        ev.name = name;
        ev.me = target.id, ev.codeOwner = c.frame.codeOwner, ev.stackObject = c.frame.stackObject; // as Animate Overlay's
        ev.eventTree = tree;
        ev.pending = library(c.vm).treeEvents(name);
        ev.length = library(c.vm).seconds(name).value_or(0) * 1000.0f;
        ev.perTick = float(1000.0 / c.vm.clock.ticksPerSecond) * animationSpeed(c);
        ev.requestLoops = (o[2] & 0x20) ? c.thread.temps[1] : int16_t(1);
        ev.hurry = (o[10] & 0x80) ? 0.0f : 1.0f;
        list.push_back(std::move(ev));
    }
    return Exit::True;
}


// Animate Stop (0x6C), by the Mac binary's disassembler
// (docs/primitives-mac.md §5) and try_animate_stop: the object (owner u8 at
// 3, data u16), mode s8 at 7: 0 the animation named by u16 at 0 of the list
// byte 6, every request of that name (try_animate_stop collects all the
// overlay requests whose name matches, as many as Animate Overlay's "two
// of a name" left: stopping only the first left one of the two
// "a2o-coffee-DN-carry" the get-in starts on, the arm held at the seated
// thigh while the Sim walked off; live QA 15), 1 the overlays, 2 the full-body animations, 3 all, 4-11 the overlays
// of one kind (Overlay::oeType: 4 carry, 5 idle, 6 gesture, 7 reaction, 8
// normal, 9 facial, 10 idle facial, 11 receptivity); flags byte 2 (0x01
// flipped, 0x02 blended, 0x20 short blend, 0x08 flip flag in temp 3, 0x40
// normal and flipped), priority s8 at 8. Mode 4 also takes off the carry
// poses of what a Sim holds (Object::carryOverlays). The full-body
// animation is left to the Animate Sim waiting for it. A priority byte 8
// p > 0 stops only the requests of levels 2p - 2 and 2p - 1 (the request's
// level with its low bit cleared, try_animate_stop: Animate Sim's p - 1 and
// Animate Overlay's). How they go (AbortAnimRequest): with 0x02 they
// blend out over kBlendTicks, with 0x20 over 0.3 of it, else at once
// (AnimationState::stoppedFades tells the renderer). The event trees of
// the overlays stopped get animevent::Stopped (inferred from the idle
// trees, which clear their "playing" flag on it; the binary aborts the
// request with status 3 and its event handler sends the event).
Exit animateStop(Context& c) {
    const auto& o = c.instruction.operands;
    Object& target = objectAt(c, 3);
    auto& a = target.animation;
    const int mode = int8_t(o[7]);
    std::string byName;
    if (mode == 0) {
        // Flag 0x04 of byte 2: the index is in that parameter ("anim id in
        // s.v.", as Animate Sim's and Overlay's): "Breathe - Overlay - Stop
        // All" sets parameter 1 to each breathing overlay's entry before each
        // stop, and "Stance - Stop" its talk stance's in parameter 0; read
        // as the entry itself they stopped another animation or nothing,
        // and the standing "breatheLoop50" overlay (level 3: over talk,
        // walk and seats) and the talk stances' lean ("...Pose-SWAT")
        // stayed on after a conversation (user playtests, r20-stand).
        const std::string entry = listEntry(c, target, o[6], animationIndex(c, 0, o[2] & 0x04));
        const std::string first = trim(split(entry, ',')[0]);
        if (first.empty() || first == ".")
            return Exit::True;
        byName = target.person ? resolveListAnimation(c.vm, target, entry) : parseItem(first).name;
    }
    auto isCarry = [&](const Overlay& x) {
        return std::find(target.carryOverlays.begin(), target.carryOverlays.end(), x.name) != target.carryOverlays.end();
    };
    auto stops = [&](const Overlay& x) {
        switch (mode) {
        case 0: return x.name == byName;
        case 1:
        case 3: return true;
        case 4: return x.oeType == oetype::Carry || isCarry(x);
        case 5: return x.oeType == oetype::Idle;
        case 6: return x.oeType == oetype::Gesture;
        case 7: return x.oeType == oetype::Reaction;
        case 8: return x.oeType == oetype::Normal && !isCarry(x);
        case 9: return x.oeType == oetype::Facial;
        case 10: return x.oeType == oetype::FacialIdle;
        case 11: return x.oeType == oetype::Receptivity;
        default: return false;
        }
    };
    const int priority = int8_t(o[8]);
    auto ofPriority = [&](const Overlay& x) { return priority <= 0 || (x.level & ~1) == 2 * priority - 2; };
    // Only those played as flipped as the operand says (byte 2 bit 0, or
    // temp 3 != 0 with 0x08), both ways with 0x40 (try_animate_stop).
    const bool flip = (o[2] & 0x08) ? c.thread.temps[3] != 0 : (o[2] & 0x01) != 0;
    auto ofFlip = [&](const Overlay& x) { return (o[2] & 0x40) || x.flipped == flip; };
    if (!target.person)
        c.vm.holdObjectPose(target); // an object stays where its animation left it
    std::vector<Overlay> stopped;
    for (auto it = a.overlays.begin(); it != a.overlays.end();) {
        if (stops(*it) && ofPriority(*it) && ofFlip(*it)) {
            stopped.push_back(std::move(*it));
            it = a.overlays.erase(it);
        } else {
            ++it;
        }
    }
    if (stopped.empty())
        return Exit::True;
    ++a.overlaySerial;
    const float fade = (o[2] & 0x20) ? kBlendTicks * 0.3f / 1000.0f : (o[2] & 0x02) ? kBlendTicks / 1000.0f : 0.0f;
    for (const Overlay& x : stopped)
        a.stoppedFades.emplace_back(x.id, fade);
    if (a.stoppedFades.size() > 32)
        a.stoppedFades.erase(a.stoppedFades.begin(), a.stoppedFades.end() - 32);
    for (const Overlay& x : stopped)
        runOverlayEvent(c.vm, x, animevent::Stopped, true);
    return Exit::True;
}

// Assign Locomotion Animations (0x72): the Sim's stack of locomotion sets.
// Mode u16 at 0 (Mac binary, docs/primitives-mac.md §5): 0 pops every set
// ("Pop all entries from Stack."), 1 pops the last ("Pop last entry from
// Stack."), n >= 2 pushes group n - 1 ("Push group #%d onto Stack"), taken
// as the block of six strings n - 2 (groups numbered from 1, inferred: "CT
// - Walk Like A Ghost" pushes 2 = the first block of the Sims' semi-global,
// the ghost walk). Where the set comes from, byte 2: 0x02 STR# 0x84 of the
// code owner's semi-global group, 0x04 the global group, else its own group.
Exit assignLocomotion(Context& c) {
    const auto& o = c.instruction.operands;
    Object& me = c.me();
    auto& stack = me.animation.locomotion;
    const uint16_t mode = le16(&o[0]);
    switch (mode) {
    case 0: stack.clear(); return Exit::True;
    case 1:
        if (stack.empty())
            throw VMError("Invalid pop: locomotion stack is empty");
        stack.pop_back();
        return Exit::True;
    default: {
        const Object* owner = c.vm.object(c.frame.codeOwner);
        const uint32_t group = o[2] & 0x04 ? kGlobalGroup : !owner ? 0 : o[2] & 0x02 ? owner->semiGroup : owner->group;
        if (!group || !c.vm.code().strings(group, animlist::Locomotion))
            throw VMError("Assign Locomotion Animations: no locomotion set (STR# 0x84) in group " + hexString(group));
        if (stack.size() >= kMaxLocomotionDepth)
            throw VMError("Locomotion stack is too deep: check for imbalanced push/pops");
        stack.push_back({group, uint8_t(mode - 2)});
        return Exit::True;
    }
    }
}

// Effect Stop/Start (0x70): particle effects and balloons, which only the
// renderer shows (cTSObject::TryChangeEffect, docs/effects-mac.md). Mode u8
// at 0: 0 soft start, 1 hard start, 2 soft stop, 3 hard stop, 4 soft stop
// all, 5 hard stop all, 6 fire and forget, 7 interrogate bone, 8 hard stop
// all and clear the queue, 9 stop ALL, 10-13 state 1-4, 14 stop ALL soft;
// the object (owner u8 at 1, data u16), the effect u16 at 4 (-1: the
// default effect, an error in TS2 except for the stop-all modes), slot byte
// 6 of type s8 9, flags u16 at 10: 0x01 the global STR# 0x8F, 0x02 the
// semi-global's, else the running tree's group; 0x08 the effect index in
// temp 0; 0x80 priority queue; icons 0x04 (object owner u8 12, data u16 13;
// 0x400 GUID in temps 4/5), 0x20 (atlas texture from STR# 0x95 entry byte
// 15, cell from that variable), 0x10, 0x1000, 0x2000. Always true.
Exit effectStopStart(Context& c) {
    const auto& o = c.instruction.operands;
    Object& target = objectAt(c, 1);
    auto& fx = target.animation.effects;
    const uint16_t flags = le16(&o[10]);
    const int16_t effect = flags & 0x08 ? c.thread.temps[0] : int16_t(le16(&o[4]));
    const uint8_t mode = o[0];
    static uint32_t serial = 0;
    // The name: entry `effect` of STR# 0x8F (the binary adds 1: its string
    // lookups count from 1).
    auto tableName = [&](uint16_t list, int index) -> std::string {
        CodeLibrary& code = c.vm.code();
        std::vector<uint32_t> groups;
        if (flags & 0x01)
            groups = {kGlobalGroup};
        else if (flags & 0x02)
            groups = {code.semiGlobalFor(c.frame.group)};
        else
            groups = {c.frame.group};
        if (list == 0x95) // icon names fall back like the binary: tree, semi-global, global
            groups = {c.frame.group, code.semiGlobalFor(c.frame.group), kGlobalGroup};
        for (uint32_t g : groups) {
            const StringTable* t = g ? code.strings(g, list) : nullptr;
            if (t && index >= 0 && size_t(index) < t->count(lang::EnglishUS)) {
                auto n = t->get(size_t(index), lang::EnglishUS);
                if (!n.empty())
                    return n;
            }
        }
        return {};
    };
    switch (mode) {
    case 0:
    case 1:
    case 6: {
        if (effect == -1 || (effect & 0xFF) == 0xFF)
            break; // "Trying to run default effect - not supported anymore."
        ActiveEffect e;
        e.index = effect;
        e.name = tableName(0x8F, effect);
        e.slot = o[6];
        e.slotType = int8_t(o[9]);
        e.flags = flags;
        e.serial = ++serial;
        e.oneShot = mode == 6;
        e.hard = mode == 1;
        // The icon, in the binary's order: 0x1000, 0x04, 0x20, 0x10, 0x2000.
        if (flags & 0x1000) {
            e.iconId = c.thread.temps[6];
        } else if (flags & 0x04) {
            if (flags & 0x400)
                e.iconGuid = uint32_t(uint16_t(c.thread.temps[4])) | (uint32_t(uint16_t(c.thread.temps[5])) << 16);
            else if (flags & 0x100)
                e.iconObject = c.thread.me; // (the binary: another object of the caller's; taken as me)
            else
                e.iconObject = c.get(o[12], int16_t(le16(&o[13])));
        } else if (flags & 0x20) {
            // The atlas named by STR# 0x95 entry byte 15 ("Set FX - Topic -
            // Speech Balloon" passes 4-9: conversationshared01-06), the
            // cell from the variable (the topic).
            e.icon = tableName(0x95, o[15]);
            e.iconFrame = c.get(o[12], int16_t(le16(&o[13])));
        } else if (flags & 0x10) {
            e.iconSim = c.get(o[12], int16_t(le16(&o[13])));
        } else if (flags & 0x2000) {
            e.wantSim = c.get(o[12], int16_t(le16(&o[13])));
            e.wantFear = c.thread.temps[4] != 0;
            e.wantSlot = c.thread.temps[5];
        }
        if (!e.oneShot)
            std::erase_if(fx, [&](const ActiveEffect& x) { return !x.oneShot && x.index == effect && x.flags == flags; });
        else if (std::count_if(fx.begin(), fx.end(), [](const ActiveEffect& x) { return x.oneShot; }) >= 8)
            fx.erase(std::find_if(fx.begin(), fx.end(), [](const ActiveEffect& x) { return x.oneShot; }));
        if (fx.size() < 64)
            fx.push_back(std::move(e));
        break;
    }
    case 2:
    case 3:
        std::erase_if(fx, [&](const ActiveEffect& x) { return !x.oneShot && x.index == effect; });
        break;
    case 4:
    case 5:
    case 8:
    case 9:
    case 14: std::erase_if(fx, [](const ActiveEffect& x) { return !x.oneShot; }); break;
    case 10:
    case 11:
    case 12:
    case 13:
        // States 1-4: the running effect's float parameter 0xF = 0..3
        // ("Interaction - Feed Fish" mode 11, "Kill Fish" mode 12).
        for (auto& x : fx)
            if (x.index == effect)
                x.state = int8_t(mode - 10);
        break;
    default: break;
    }
    return Exit::True;
}

} // namespace

std::string resolveListAnimation(VM& vm, const Object& sim, std::string entry) {
    // Only an empty name is error 0x43 (try_animate_sim 0x69d634 and
    // try_animate_overlay 0x69ea54 test the length after the type parse,
    // vtable 0x380 = 0x68d934, which takes a name without "(role)" as type 1
    // and trims it). A "." placeholder is a name like any other: no such
    // animation, so nothing plays and the primitive returns true ("missing
    // animation tree"). The baby list's "." at 84 ("CT - Route Blocked
    // Animation" on a baby on the floor) reset the baby's Main here.
    std::string name = parseItem(split(entry, ',')[0]).name;
    if (trim(name).empty())
        throw VMError("Animation name is empty string.");
    const std::string p = agePrefix(sim);
    if ((p == "t" || p == "e") && name.size() > 1 && name[0] == 'a' && (name[1] == '-' || name[1] == '2')) {
        std::string aged = p + name.substr(1);
        if (library(vm).exists(aged))
            return aged;
    }
    return name;
}

std::string locomotionAnimation(VM& vm, const Object& sim, std::string_view role) {
    LocomotionSet set{kGlobalGroup, 0};
    if (!sim.animation.locomotion.empty())
        set = sim.animation.locomotion.back();
    const StringTable* table = vm.code().strings(set.group, animlist::Locomotion);
    if (!table)
        return {};
    // Walks and poses are in the second string of the walk block, turns in the
    // first; runs in the fifth and fourth.
    const bool run = role.starts_with("run");
    const bool turn = role.find("urn") != std::string_view::npos;
    const size_t index = size_t(set.set) * 6 + (run ? 3 : 0) + (turn ? 0 : 1);
    const std::vector<std::string> items = split(table->get(index, lang::EnglishUS), ',');
    std::string name;
    bool anyRole = false;
    for (const auto& s : items) {
        const LocoItem it = parseItem(s);
        anyRole = anyRole || !it.role.empty();
        if (it.role == role && name.empty())
            name = it.name;
    }
    if (name.empty() && !anyRole && !turn) {
        // short, normal, pose (swimming)
        std::vector<std::string> names;
        for (const auto& s : items)
            if (auto n = parseItem(s).name; !n.empty())
                names.push_back(n);
        const bool isShort = role.find("Short") != std::string_view::npos;
        const bool pose = role.find("Pose") != std::string_view::npos;
        for (const auto& n : names) {
            const bool nShort = n.find("Short") != std::string::npos, nPose = n.find("Pose") != std::string::npos;
            if (nShort == isShort && nPose == pose && n.find("InPlace") == std::string::npos) {
                name = n;
                break;
            }
        }
    }
    if (name.empty())
        return {};
    AnimationLibrary& lib = library(vm);
    const std::string p = agePrefix(sim), g = genderName(sim);
    for (const std::string& candidate : {p + "-" + g + "-" + name, p + "-" + name, "a-" + g + "-" + name, "a-" + name})
        if (lib.exists(candidate))
            return candidate;
    return {};
}

float walkTilesPerTick(VM& vm, const Object& sim) {
    if (sim.person)
        if (const float v = gaitOf(vm, sim).speed; v > 0)
            return v / float(vm.clock.ticksPerSecond);
    return vm.walkTilesPerTick;
}

RequestFades requestFades(float durationMs, bool blendIn, bool blendOut) {
    RequestFades f{blendIn ? kBlendTicks : 0.0f, blendOut ? kBlendTicks : 0.0f};
    const float both = f.in + f.out;
    if (durationMs + 1e-6f < both) {
        const float k = durationMs / both * 0.9999f;
        f.in *= k, f.out *= k;
    }
    if (f.in < 1)
        f.in = 0;
    if (f.out < 1)
        f.out = 0;
    return f;
}

double animationClockMs(const VM& vm) { return double(vm.ticks) * 1000.0 / vm.clock.ticksPerSecond; }

bool resumeAnimateSim(VM& vm, Thread& t, const ResumedAnimation& r) {
    Object* me = vm.object(t.me);
    if (!me || t.stack.empty() || r.name.empty())
        return false;
    Frame& f = t.stack.back();
    if (!f.behavior || f.pc >= f.behavior->instructions.size() || f.behavior->instructions[f.pc].opcode != 0x6A ||
        uint32_t(f.primitiveState) != r.id)
        return false;
    const auto seconds = library(vm).seconds(r.name);
    if (!seconds)
        return false;
    AnimationState& a = me->animation;
    const float speed = r.speed > 0 ? r.speed : 1.0f;
    a.name = r.name;
    ++a.serial;
    a.flipped = r.mirrored;
    a.level = int8_t(std::clamp(r.level, 0, 8));
    a.weight = r.weight <= 0 || r.weight > 1 ? 1.0f : r.weight;
    a.speed = speed;
    a.ticksPerVmTick = float(1000.0 / vm.clock.ticksPerSecond) * speed;
    a.depth = t.stack.size(), a.tree = f.behavior, a.node = f.pc;
    a.pendingEvents.clear();
    for (const auto& e : library(vm).treeEvents(r.name))
        if (float(e.first) > r.played)
            a.pendingEvents.push_back(e);
    a.eventTree = le16(&f.behavior->instructions[f.pc].operands[4]);
    a.headingOffset = r.heading;
    a.eventObject = r.target;
    a.eventStackObject = f.stackObject;
    a.lengthTicks = *seconds * 1000.0f;
    const auto& operands = f.behavior->instructions[f.pc].operands;
    const RequestFades fades = requestFades(a.lengthTicks / speed, !(operands[2] & 0x80), !(operands[2] & 0x40));
    a.fadeInMs = fades.in, a.fadeOutMs = fades.out;
    a.hurry = (operands[8] & 0x20) ? 0.0f : 1.0f;
    a.blendOutSent = a.fadeOutMs > 0 && r.played >= a.lengthTicks - a.fadeOutMs * speed;
    a.playedTicks = r.played;
    // The VM ticks it had played: its played time in ticks (the float the
    // ticks added up to, so the ticks left come out as they were).
    const int played = int(std::lround(r.played / a.ticksPerVmTick));
    a.ticksLeft = std::max(ticksFor(vm, *seconds / speed) - played, 1);
    a.actionTicksLeft =
        a.fadeOutMs > 0 ? std::max(ticksForMs(vm, double(*seconds) * 1000.0 / speed - a.fadeOutMs) - played, 1)
                        : a.ticksLeft;
    a.requestId = r.id;
    return true;
}

void resumeOverlay(VM& vm, Object& target, const ResumedOverlay& r) {
    if (r.name.empty())
        return;
    Overlay& o = target.animation.addOverlay(r.name);
    o.oeType = r.oeType;
    o.level = r.level;
    o.flipped = r.mirrored;
    o.ikObject = r.ikObject;
    o.headingOffset = r.heading;
    o.eventTree = r.eventTree;
    o.me = r.me, o.codeOwner = r.codeOwner, o.stackObject = r.stackObject;
    o.length = library(vm).seconds(r.name).value_or(0) * 1000.0f;
    o.loops = o.length > 0 && r.loops > 0 ? r.loops : 0;
    o.forever = r.loops == -1;
    o.requestLoops = r.loops != 0 ? int16_t(r.loops) : int16_t(1);
    o.perTick = float(1000.0 / vm.clock.ticksPerSecond) * (r.speed > 0 ? r.speed : 1.0f);
    o.played = r.played;
    // The events of the loop it is in not reached yet (tickOverlayEvents;
    // a pose held until stopped never reaches any; one looping until
    // stopped goes on round).
    if (o.loops > 0 || (o.forever && o.length > 0)) {
        const float inPlay = std::fmod(o.played, o.length);
        if (r.treeEvents)
            for (const auto& e : library(vm).treeEvents(r.name))
                if (float(e.first) > inPlay)
                    o.pending.push_back(e);
        if (o.loops > 0) {
            // (The saved request's blend flags are not read: both, inferred.)
            const float speed = r.speed > 0 ? r.speed : 1.0f;
            const RequestFades fades = requestFades(o.length * float(o.loops) / speed, true, true);
            o.fadeInMs = fades.in, o.fadeOutMs = fades.out;
            o.blendOutSent = o.played >= o.length * float(o.loops) - o.fadeOutMs * speed;
        } else {
            o.fadeOutMs = 0;
        }
    }
    ++target.animation.overlaySerial;
}

void resumeOverlayEvents(VM& vm, Object& target, const std::string& name, uint16_t tree, int16_t me,
                         int16_t codeOwner, int16_t stackObject, float played, float speed) {
    AnimationState::OverlayEvents ev;
    ev.name = name;
    ev.me = me, ev.codeOwner = codeOwner, ev.stackObject = stackObject;
    ev.eventTree = tree;
    for (const auto& e : library(vm).treeEvents(name))
        if (float(e.first) > played)
            ev.pending.push_back(e);
    ev.length = library(vm).seconds(name).value_or(0) * 1000.0f;
    ev.perTick = float(1000.0 / vm.clock.ticksPerSecond) * (speed > 0 ? speed : 1.0f);
    ev.played = played;
    target.animation.overlayEvents.push_back(std::move(ev));
}

void registerAnimationPrimitives(VM& vm) {
    vm.setPrimitive(0x69, animateObject);
    vm.setPrimitive(0x6A, animateSim);
    vm.setPrimitive(0x6B, animateOverlay);
    vm.setPrimitive(0x6C, animateStop);
    vm.setPrimitive(0x70, effectStopStart);
    vm.setPrimitive(0x72, assignLocomotion);
}

void startScriptedWalk(VM& vm, int16_t simId, int16_t target, std::optional<uint16_t> animation) {
    // A tree of the Sim's own group, at an instance the game's private trees
    // do not reach (they stop well below 0x1F00).
    constexpr uint16_t kTree = 0x1FF0;
    Object* sim = vm.object(simId);
    if (!sim)
        throw VMError("startScriptedWalk: no object " + std::to_string(simId));
    Behavior b;
    b.fileName = "opensims2 - Scripted Walk";
    b.signature = 0x8007;
    Behavior::Instruction go;
    go.opcode = 0x1B; // Go To Relative Position: anywhere near (-1), facing it (-2)
    go.operands[2] = uint8_t(int8_t(-1));
    go.operands[3] = uint8_t(int8_t(-2));
    go.onFalse = {Behavior::TargetKind::False, 0};
    if (animation) {
        go.onTrue = {Behavior::TargetKind::Instruction, 1};
        Behavior::Instruction anim;
        anim.opcode = 0x6A;
        anim.operands[0] = uint8_t(*animation & 0xFF);
        anim.operands[1] = uint8_t(*animation >> 8);
        anim.operands[3] = 0x20;
        anim.operands[6] = uint8_t(animlist::Global);
        anim.onTrue = {Behavior::TargetKind::True, 0};
        anim.onFalse = {Behavior::TargetKind::True, 0};
        b.instructions = {go, anim};
    } else {
        go.onTrue = {Behavior::TargetKind::True, 0};
        b.instructions = {go};
    }
    vm.code().add(sim->group, kTree, std::move(b));
    vm.threads.insert_or_assign(simId, vm.start(simId, kTree, {}, target));
}

int8_t oeTypeOfRole(const std::string& role) {
    std::string r = trim(role.substr(0, role.find(',')));
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    if (r == "carry")
        return oetype::Carry;
    if (r == "idle")
        return oetype::Idle;
    if (r == "gesture")
        return oetype::Gesture;
    if (r == "reaction")
        return oetype::Reaction;
    if (r == "facial")
        return oetype::Facial;
    if (r == "facialidle" || r == "facial idle")
        return oetype::FacialIdle;
    if (r == "receptivity")
        return oetype::Receptivity;
    return oetype::Normal;
}

void tickHurry(VM& vm) {
    constexpr int16_t kHurrySpeed = 85; // object data 85 "Hurry Speed" (STR# 0x8D)
    const float msPerTick = float(1000.0 / vm.clock.ticksPerSecond);
    for (Object* po : vm.objectTable()) {
        if (!po || po->data[kHurrySpeed] == 0)
            continue;
        const float f = float(po->data[kHurrySpeed]) / 100.0f;
        const float factor = f > 0 ? 1 + f : 1 / (1 - f);
        // A request hurried by another factor (its hurry 0: not hurryable)
        // plays the rest at its speed x factor / hurry (FUN_00a4757c: what
        // played stays, its weight function is made again for the rest).
        auto hurry = [&](float& by, float& speed) {
            if (!(by > 0) || by == factor)
                return false;
            speed = speed * factor / by;
            by = factor;
            return true;
        };
        AnimationState& a = po->animation;
        if (a.playing() && a.requestId && hurry(a.hurry, a.speed)) {
            a.ticksPerVmTick = msPerTick * a.speed;
            retimeAnimateSim(vm, a);
        }
        for (auto& t : a.tails)
            if (hurry(t.hurry, t.speed)) {
                t.perTick = msPerTick * t.speed;
                t.ticksLeft = ticksForMs(vm, std::max(0.0, double(t.length - t.played) / double(t.speed)));
            }
        for (auto& ov : a.overlays) {
            float speed = ov.perTick / msPerTick;
            if (ov.perTick > 0 && hurry(ov.hurry, speed))
                ov.perTick = msPerTick * speed;
        }
        for (auto& ev : a.overlayEvents) {
            float speed = ev.perTick / msPerTick;
            if (ev.perTick > 0 && hurry(ev.hurry, speed))
                ev.perTick = msPerTick * speed;
        }
    }
}

void tickOverlayEvents(VM& vm) {
    const float msPerTick = float(1000.0 / vm.clock.ticksPerSecond);
    struct Fire {
        int16_t me, codeOwner, stackObject;
        uint16_t tree;
        int16_t event;
        float portion; // of the animation played (0-1)
        int16_t loops; // the request's loop count
    };
    std::vector<Fire> fires;
    for (Object* po : vm.objectTable()) {
        if (!po)
            continue;
        // Animate Overlay's: tree events as they are reached, 9998 at the
        // end of every loop (cActiveAnimEvent's +0x40 key, type 6, posted
        // each time the request's time goes round: BV 0xd3a290/0xd38bec,
        // docs/anim-mac.md §12.1) with the next loop's tree events again,
        // 9997 as the last play starts blending out, 9999 and off the object
        // at its end.
        auto& ov = po->animation.overlays;
        // The loops `it` went round this tick: the rest of each loop's tree
        // events, its 9998, and the next loop's events from the start.
        auto goRound = [&](std::vector<Overlay>::iterator it, float before, float portion) {
            const int from = int(before / it->length), to = int(it->played / it->length);
            for (int k = from; k < to; ++k) {
                for (const auto& e : it->pending)
                    fires.push_back({it->me, it->codeOwner, it->stackObject, it->eventTree, e.second, portion,
                                     it->requestLoops});
                fires.push_back({it->me, it->codeOwner, it->stackObject, it->eventTree, animevent::ActionEnd, portion,
                                 it->requestLoops});
                it->pending = library(vm).treeEvents(it->name);
            }
        };
        for (auto it = ov.begin(); it != ov.end();) {
            if (it->loops <= 0 || it->length <= 0) {
                // Animate Object's: played until stopped, holding its last
                // frame, or looping (temp 1 = -1).
                if (!po->person && it->perTick > 0)
                    it->played = it->forever && it->length > 0 ? std::fmod(it->played + it->perTick, it->length)
                                                               : std::min(it->played + it->perTick, it->length);
                // A Sim's overlay looping until stopped (Animate Overlay's
                // temp 1 = -1) runs its event tree every loop too (no 9999
                // while it loops; stopped, it gets -1, aborted).
                if (po->person && it->forever && it->length > 0 && it->eventTree && it->perTick > 0) {
                    const float before = it->played;
                    it->played += it->perTick;
                    const float inPlay = std::fmod(it->played, it->length);
                    const float portion = inPlay / it->length;
                    goRound(it, before, portion);
                    while (!it->pending.empty() && it->pending.front().first <= inPlay) {
                        fires.push_back({it->me, it->codeOwner, it->stackObject, it->eventTree,
                                         it->pending.front().second, portion, it->requestLoops});
                        it->pending.erase(it->pending.begin());
                    }
                    // (No float drift over hours: the loops played are not needed.)
                    if (it->played > it->length * 1000)
                        it->played = std::fmod(it->played, it->length);
                }
                ++it;
                continue;
            }
            const float before = it->played;
            it->played += it->perTick;
            const float total = it->length * float(it->loops);
            const bool ended = it->played >= total;
            const float inPlay = std::fmod(it->played, it->length);
            const float portion = ended ? 1.0f : inPlay / it->length;
            if (!ended)
                goRound(it, before, portion);
            while (!it->pending.empty() && (ended || it->pending.front().first <= inPlay)) {
                fires.push_back({it->me, it->codeOwner, it->stackObject, it->eventTree, it->pending.front().second,
                                 portion, it->requestLoops});
                it->pending.erase(it->pending.begin());
            }
            if (!it->blendOutSent && it->fadeOutMs > 0 && it->played >= total - it->fadeOutMs * it->perTick / msPerTick) {
                it->blendOutSent = true;
                fires.push_back({it->me, it->codeOwner, it->stackObject, it->eventTree, animevent::BlendOut, portion,
                                 it->requestLoops});
            }
            if (ended) {
                fires.push_back({it->me, it->codeOwner, it->stackObject, it->eventTree, animevent::ActionEnd, 1.0f,
                                 it->requestLoops});
                fires.push_back({it->me, it->codeOwner, it->stackObject, it->eventTree, animevent::End, 1.0f,
                                 it->requestLoops});
                // An object keeps the bones where it ends (a door's leaf
                // open, until its close animation), as for Animate Stop.
                if (!po->person) {
                    it->played = std::min(it->played, it->length);
                    const size_t at = size_t(it - ov.begin());
                    vm.holdObjectPose(*po);
                    it = ov.begin() + std::ptrdiff_t(at);
                }
                it = ov.erase(it);
                ++po->animation.overlaySerial;
            } else {
                ++it;
            }
        }
        auto& list = po->animation.overlayEvents;
        for (auto it = list.begin(); it != list.end();) {
            it->played += it->perTick;
            const bool ended = it->played >= it->length;
            while (!it->pending.empty() && (ended || it->pending.front().first <= it->played)) {
                fires.push_back({it->me, it->codeOwner, it->stackObject, it->eventTree, it->pending.front().second,
                                 ended || it->length <= 0 ? 1.0f : it->played / it->length, it->requestLoops});
                it->pending.erase(it->pending.begin());
            }
            // At its end the event tree also gets event 9999 ("Event Tree -
            // Drive" handles the end of each drive loop on 9999: loop
            // again, or stop and set the speed to 0; inferred from it).
            // The engine moves an animated object by its animation's root
            // (cAnimatable): where the root ends, from its rest place, along
            // the object's facing, to every part of its multi-tile group, at
            // the animation's end (inferred). The vehicles' drive, start and
            // stop loops end at the rest place (the vehicle snaps ahead to
            // its "slot_travel_drive" bone and the loop brings the root up
            // from behind): no move; a drive-off ends ahead. With the travel
            // from the first frame (r12) the fire truck stopped 9 tiles past
            // its "Service Stop" portal, its door off N001_Lot11, and the
            // firefighter never got out (round 18).
            if (ended && !po->person && po->position.inWorld())
                if (const float d = library(vm).rootTravel(it->name, vm.rootRestY(*po)); d != 0) {
                    const auto step = directionStep(po->data[objdata::Direction]);
                    const float len = std::hypot(float(step[0]), float(step[1]));
                    const float dx = d * float(step[0]) / len, dy = d * float(step[1]) / len;
                    for (ObjectId part : groupOf(vm, po->id))
                        if (Object* x = vm.object(part); x && x->position.inWorld())
                        {
                            x->position.x += dx, x->position.y += dy;
                            if (vm.world) {
                                x->data[objdata::Room] = vm.world->room(VM::tileOf(*x));
                                vm.noteRoomChanged(x->id);
                            }
                        }
                }
            if (ended)
                fires.push_back({it->me, it->codeOwner, it->stackObject, it->eventTree, animevent::End, 1.0f,
                                 it->requestLoops});
            it = ended ? list.erase(it) : it + 1;
        }
        // The Animate Sim requests blending out after their action ended
        // (endAnimateSimAction): the rest of their tree events as they are
        // reached, and 9998 and 9999 at their end.
        auto& tails = po->animation.tails;
        for (auto it = tails.begin(); it != tails.end();) {
            if (it->madeAt == vm.ticks) { // its primitive played it this tick
                ++it;
                continue;
            }
            it->played += it->perTick;
            const bool ended = --it->ticksLeft <= 0;
            const float length = std::max(it->length, 1.0f);
            while (!it->pending.empty() && (ended || float(it->pending.front().first) <= it->played)) {
                fires.push_back({it->me, it->codeOwner, it->stackObject, it->eventTree, it->pending.front().second,
                                 std::clamp(it->played / length, 0.0f, 1.0f), 1});
                it->pending.erase(it->pending.begin());
            }
            if (ended) {
                fires.push_back({it->me, it->codeOwner, it->stackObject, it->eventTree, animevent::ActionEnd, 1.0f, 1});
                fires.push_back({it->me, it->codeOwner, it->stackObject, it->eventTree, animevent::End, 1.0f, 1});
                it = tails.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const Fire& f : fires) {
        if (!f.tree || !vm.object(f.me) || !vm.object(f.codeOwner))
            continue;
        try {
            runEventTree(vm, f.me, f.codeOwner, f.stackObject, f.tree, objectTemps(vm, f.me), f.event, f.portion,
                         f.loops, false);
        } catch (const VMError&) {
            // the error goes to the animated object's tree sim in the game
            // (cTSTreeSim::RunOneTickTree); here it is dropped (inferred)
        }
    }
}

} // namespace os2::vm
