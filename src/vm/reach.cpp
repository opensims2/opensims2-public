// Find Best Object for Function (0x0E), Run Functional Tree (0x14) and
// Reach/Put (0x74). Operand layouts and
// enumerations come from the game's own data: the SimAntics editor's labels
// in objects.package (group 0x7FE59FD0: STR# 0xC9 "Function Table Names",
// STR# 0xF5 "entry points"), the editor's disassembly strings left in
// Sims2.exe (cEdithLanguage, in reverse order of the code) and how the
// game's trees use each byte (`s2pkg prim-usage <Res> 14|74`). What is
// inferred is said where it is used; TODO.md (M5) keeps the list.

#include "vm.hpp"
#include "go_here.hpp"

#include "animation.hpp"
#include "errors.hpp"
#include "interactions.hpp"
#include "kill.hpp"
#include "routing_internal.hpp"

#include "formats/binreader.hpp"
#include "formats/anim.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace os2::vm {

namespace {


// --- Run Functional Tree -------------------------------------------------------

// Function numbers (STR# 0xC9 "Function Table Names") to OBJf entries (STR#
// 0xF5 "entry points"), matched by name: prepare food -> "prep" (18), ...,
// serving surface (10) -> 14, garden (12) -> "gardening" (16), dry/wash
// clothes -> 31/32, message handler -> 36, ..., along route callback -> 41,
// awareness -> 42, reset -> 43, lookatTarget -> 44, Utility State Change ->
// 46, Global Awareness -> 53. The first 15 are The Sims 1's table (FreeSO's
// VMFindBestObjectForFunction.FunctionToEntryPoint); the OBJf actions at
// those entries are the objects' "Function - Sit", "Function - Dispose", ...
// The Mac binary has the same fixed table, except that 24 "reset" has no
// entry (0 here) and 27 "Global Awareness" (0x35) is Bon Voyage only
// (docs/primitives-mac.md §4/§5b): both give false.
constexpr uint8_t kFunctionEntry[] = {18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 14, 28, 16, 17,
                                      29, 31, 32, 36, 37, 38, 39, 40, 41, 42, 0, 44, 46, 0};

const ObjectFunctions::Entry* functionEntry(VM& vm, const Object& o, uint16_t function) {
    if (function >= std::size(kFunctionEntry) || kFunctionEntry[function] == 0)
        return nullptr;
    const ObjectFunctions* f = vm.code().functions(o.group, o.definitionInstance);
    const size_t entry = kFunctionEntry[function];
    return f && entry < f->functions.size() ? &f->functions[entry] : nullptr;
}

// Runs `guard` of `owner`'s code for `me` as a check tree, to the end, in a
// thread of its own: true if it returns true. A guard that does not exist
// passes (as FreeSO's "else Execute = true"). The binary runs it in my own
// tree sim (cTSObject::RunOneTickTree with no parameters: they default to
// my temps) and does not restore my temps afterwards, so what the tree
// writes to them is what the caller reads next: "Function - Prep - TEST"
// returns false for a free counter in autonomy and leaves the kinds of food
// it can prepare in temp 2, which "Function Test - Prep" hands to the
// fridge's meal check trees.
bool runCheck(Context& c, ObjectId owner, uint16_t guard, std::array<int16_t, 4> args, ObjectId runner = 0) {
    const Object* o = c.vm.object(owner);
    if (!o || !c.vm.resolveTree(*o, guard).second)
        return true;
    Thread t = c.vm.start(runner ? runner : c.thread.me, guard, args, owner, owner);
    t.temps = c.thread.temps;
    constexpr size_t kMaxTurns = 1000; // sleeps in a check tree count down without waiting
    for (size_t turn = 0; t.status == Thread::Status::Running && turn < kMaxTurns; ++turn)
        c.vm.run(t, c.vm.instructionsPerTurn);
    dropThreadRecords(c.vm, t); // records.cpp
    c.thread.temps = t.temps;
    if (t.status == Thread::Status::Failed) {
        if (std::getenv("OS2_CHECK_TREE_ERRORS")) // where in the check tree, for looking into
            std::fprintf(stderr, "t%llu check tree of %d failed: %s\n", (unsigned long long)c.vm.ticks, owner, t.error.c_str());
        throw VMError("Run Functional Tree: check tree failed: " + t.error.substr(0, t.error.find('\n')));
    }
    return t.status == Thread::Status::Finished && t.result;
}

// Run Functional Tree (0x14), as the Mac binary has it
// (TryCallFunctionalTree; docs/primitives-mac.md §4): function u16 at 0
// (STR# 0xC9), flags u16 at 2: 0x01 changes the interaction's icon (there is
// no icon here), 0x02 passes the running tree's parameters on, 0x04 runs
// the entry's check tree instead of its action tree. The target is the stack
// object (none: error 0x15); the function goes through the fixed table above
// (24 "reset" and, in the base game, 27 "Global Awareness" have no entry:
// false), and that OBJf entry's action tree (with 0x04, its check tree)
// runs as a subroutine in my thread with the stack object as "me" for its
// code (GosubObjectTree); the instruction branches on its result. Tree 0,
// one that does not exist or a function past the table: false. The binary
// does not run the check tree before the action tree.
Exit runFunctionalTree(Context& c) {
    const auto& o = c.instruction.operands;
    const uint16_t function = le16(&o[0]);
    const uint16_t flags = le16(&o[2]);
    const Object* target = c.vm.object(c.frame.stackObject);
    if (!target)
        throw gameError(err::NoStackObject, "Run Functional Tree");
    const ObjectFunctions::Entry* e = functionEntry(c.vm, *target, function);
    if (!e)
        return Exit::False;
    const uint16_t tree = flags & 0x04 ? e->guard : e->action;
    if (!tree || !c.vm.resolveTree(*target, tree).second)
        return Exit::False;
    // With 0x02 all of the current tree's parameters go along, not only the
    // first four ("Function - Message Handler", 8 arguments, hands its
    // parameter 7 to the "reaction Handler" this way); without it the
    // binary passes no parameters, and the tree sim's default ones are the
    // object's temps (cTSTreeSim::Gosub, cTSObject::OnInit).
    std::array<int16_t, 4> args{};
    std::vector<int16_t> params;
    if (flags & 0x02)
        params = c.frame.params;
    else
        params.assign(c.thread.temps.begin(), c.thread.temps.end());
    std::copy_n(params.begin(), std::min<size_t>(params.size(), 4), args.begin());
    const ObjectId id = target->id;
    // GosubObjectTree with the "change icon" flag 0x01 (TryCallFunctionalTree
    // BV 0xe253a4): the person's object record carries it (records.cpp).
    c.vm.call(c.thread, id, tree, args, flags & 0x01);
    Frame& callee = c.thread.stack.back();
    callee.stackObject = id;
    for (size_t i = 4; i < params.size() && i < callee.params.size(); ++i)
        callee.params[i] = params[i];
    return Exit::Call;
}

// Find Best Object for Function (0x0E), as the Mac binary's
// cTSObject::TryFindFunctionalObject (BV 0xe35a94; docs/primitives-mac.md
// §4): function u16 at 0 (STR# 0xC9) -> the object data with the object's
// value for it and its OBJf entry: prep 31, cook 32, flat surface 33,
// dispose 36, wash dish 37, eating surface 38, sit 26 ("Sitting Value"),
// serving surface 61, clean 39 ("dirty level"), garden 64, wash hands 65,
// repair 15 ("Repair Level"); dry clothes, awareness and lookatTarget have
// no value (50); the other functions are false at once. Flags u8 at 2: 0x08
// the reference is (owner u8 at 3, data u16 at 4) instead of me (missing:
// error 0x17), 0x10 only in its line of sight, 0x80 also objects locked out
// (object data 25 "lockout count"); the place filter is the first of 0x01
// only outside, 0x02 only inside, 0x04 only in the reference's room, 0x20
// only outside it, 0x40 only in the room in my temp 0. For every other
// object with that OBJf entry, in order: awareness and lookatTarget skip
// only locked-out objects; the others skip objects in use (object data 8
// bit 0x20), locked out, with object data 87 "Disable All Interactions" or
// object data 82 bit 0x04 (even with 0x80) and burning (object data 8 bit
// 0x200); my object data 66 "function score" := the object's value (0:
// skipped); the entry's check tree, if any, runs with the reference as
// "me", the object as stack object and my temps as parameters, and may
// change the score (false or 0: skipped); then the place filter; score =
// that x 1 / (1 + d / 5), d = sqrt(distance^2 + (10 x levels apart)^2) from
// the reference ("functional half value distance"). The best becomes the
// stack object (true); none: false. Not done (inferred as no-ops): the
// rooms the Sim can reach (cTSPerson::GetAccessibleRooms; the router fills
// it), object data 56 bit 0 and 107 (Bon Voyage only), the Sim's ownership
// and autonomy biases (0 and 1 with an empty bias table).
Exit findBestObjectForFunction(Context& c) {
    const auto& o = c.instruction.operands;
    const uint16_t function = le16(&o[0]);
    const uint8_t flags = o[2];
    constexpr int16_t kNoValue = -1, kNotDone = -2;
    constexpr int16_t kValue[] = {31, 32, 33, 36, kNotDone, kNotDone, 37, 38, 26, kNotDone, 61, 39, 64, 65,
                                  15, kNoValue, kNotDone, kNotDone, kNotDone, kNotDone, kNotDone, kNotDone,
                                  kNotDone, kNoValue, kNotDone, kNoValue, kNotDone, kNotDone};
    if (function >= std::size(kValue) || kValue[function] == kNotDone)
        return Exit::False;
    const int16_t valueField = kValue[function];
    const bool awareness = function == 23 || function == 25;
    constexpr int16_t kLockoutCount = 25, kFunctionScore = 66, kPlacementFlags2 = 82, kDisableAllInteractions = 87;
    constexpr int16_t kInUse = 0x20, kBurning = 0x200;
    constexpr float kHalfValueDistance = 5.0f, kFloorToFloor = 10.0f;
    VM& vm = c.vm;
    const ObjectId meId = c.thread.me;
    ObjectId refId = meId;
    if (flags & 0x08) {
        refId = c.get(o[3], int16_t(le16(&o[4])));
        if (!vm.object(refId))
            throw gameError(err::NoObject, "Find Best Object for Function: reference object " + std::to_string(refId));
    }
    auto roomOf = [&](const Object& x) { return x.data[objdata::Room]; };
    auto outside = [&](const Object& x) {
        const int16_t room = roomOf(x);
        auto it = vm.rooms.find(room);
        return room <= 0 || (it != vm.rooms.end() && (it->second.known & 0x02) && it->second.values[1] != 0);
    };
    std::vector<ObjectId> ids;
    for (const auto& [id, x] : vm.eachObject())
        ids.push_back(id);
    // A Sim looks only in the rooms it can reach from its own (binary: the
    // set from cTSPerson::GetAccessibleRooms; the router fills it, here the
    // planner's static floor and portals).
    const std::vector<int16_t>* reachable = c.me().person ? accessibleRooms(vm, c.me().data[objdata::Room]) : nullptr;
    const std::vector<int16_t> reachableRooms = reachable ? *reachable : std::vector<int16_t>{};
    ObjectId best = 0;
    float bestScore = -std::numeric_limits<float>::max();
    for (ObjectId id : ids) {
        const Object* x = vm.object(id);
        if (!x || id == meId || x->person || !x->position.inWorld() || dying(vm, id))
            continue; // ValidateObject: not an object queued to die
        if (reachable && !std::binary_search(reachableRooms.begin(), reachableRooms.end(), x->data[objdata::Room]))
            continue;
        const int16_t objectFlags = x->data[objdata::Flags];
        const bool lockedOut = !(flags & 0x80) && x->data[kLockoutCount] > 0;
        static const char* why = std::getenv("OS2_FBOF_WHY"); // OS2_FBOF_WHY=function: each candidate's verdict
        const bool tell = why && std::atoi(why) == int(function) && functionEntry(vm, *x, function);
        if (awareness ? lockedOut
                      : (objectFlags & kInUse) || lockedOut || x->data[kDisableAllInteractions] > 0 ||
                            (x->data[kPlacementFlags2] & 0x04) || (objectFlags & kBurning)) {
            if (tell)
                std::fprintf(stderr, "t%llu FBOF %u: %d skipped (flags %04X lockout %d od87 %d od82 %04X)\n", (unsigned long long)vm.ticks,
                             function, id, uint16_t(objectFlags), x->data[kLockoutCount], x->data[kDisableAllInteractions],
                             uint16_t(x->data[kPlacementFlags2]));
            continue;
        }
        const ObjectFunctions::Entry* e = functionEntry(vm, *x, function);
        if (!e || (!e->action && !e->guard))
            continue;
        Object& me = c.me();
        me.data[kFunctionScore] = valueField == kNoValue ? int16_t(50) : x->data[size_t(valueField)];
        if (me.data[kFunctionScore] == 0)
            continue;
        if (e->guard) {
            std::array<int16_t, 4> temps{};
            std::copy_n(c.thread.temps.begin(), std::min<size_t>(4, c.thread.temps.size()), temps.begin());
            if (!runCheck(c, id, e->guard, temps, refId) || !vm.object(meId) || c.me().data[kFunctionScore] == 0) {
                if (tell)
                    std::fprintf(stderr, "t%llu FBOF %u: %d refused by its check tree\n", (unsigned long long)vm.ticks, function, id);
                continue;
            }
            x = vm.object(id);
            if (!x)
                continue;
        }
        const Object* ref = vm.object(refId);
        if (!ref)
            ref = &c.me();
        // Only the first place flag counts.
        if (flags & 0x01) {
            if (!outside(*x))
                continue;
        } else if (flags & 0x02) {
            if (outside(*x))
                continue;
        } else if (flags & 0x04) {
            if (roomOf(*x) != roomOf(*ref))
                continue;
        } else if (flags & 0x20) {
            if (roomOf(*x) == roomOf(*ref))
                continue;
        } else if (flags & 0x40) {
            if (roomOf(*x) != c.thread.temps[0])
                continue;
        }
        const float dx = x->position.x - ref->position.x, dy = x->position.y - ref->position.y;
        const float dz = kFloorToFloor * float(x->position.level - ref->position.level);
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float score = float(c.me().data[kFunctionScore]) / (distance / kHalfValueDistance + 1.0f);
        if (score <= bestScore)
            continue;
        if ((flags & 0x10) && vm.world && !vm.world->lineOfSight(VM::tileOf(*ref), VM::tileOf(*x)))
            continue;
        best = id, bestScore = score;
    }
    if (std::getenv("OS2_FBOF_FAILS") && !best) // what Find Best Object for Function found nothing for, for looking into
        std::fprintf(stderr, "t%llu FBOF %u flags %02X of %d in \"%s\" #%u found nothing\n", (unsigned long long)vm.ticks, function, flags, meId,
                     c.frame.behavior->fileName.c_str(), c.frame.pc);
    if (!best)
        return Exit::False;
    c.frame.stackObject = best;
    return Exit::True;
}

// --- Reach/Put -----------------------------------------------------------------

const Slots* slotTable(VM& vm, const Object& o) {
    return o.definition ? vm.code().slots(o.group, o.definition->slotId()) : nullptr;
}

// Containment slot `index` of `o`: the SLOT's type 0 entries, in order (as
// Snap Into and the slot data owners count them); null if there is none.
const Slots::Slot* containmentSlot(VM& vm, const Object& o, int index) {
    if (index < 0)
        return nullptr;
    if (const Slots* t = slotTable(vm, o))
        for (const auto& s : t->slots)
            if (s.type == 0 && index-- == 0)
                return &s;
    return nullptr;
}

size_t containmentSlotCount(VM& vm, const Object& o) {
    size_t n = 0;
    if (const Slots* t = slotTable(vm, o))
        for (const auto& s : t->slots)
            n += s.type == 0;
    return n;
}

// The surface of the generic reach animations ("a2o-<surface>-get/put") of a
// reach index's height band, as the global list names them.
const char* surfaceOfIndex(int index) {
    static const char* const kSurfaces[] = {"floor", "tableCoffee", "table", "counter"};
    return kSurfaces[index & 3];
}

// The reach animation for `sim`: "<age>2o-<surface>-<get|put>" ("a", "c"),
// falling back to the adult one (teens and elders use the adult reaches;
// only adults and children have them in the game).
AnimationLibrary& library(VM& vm) {
    if (!vm.animations.index())
        vm.animations.setIndex(vm.code().index());
    return vm.animations;
}

std::string reachAnimation(VM& vm, const Object& sim, const std::string& surface, bool get) {
    const std::string rest = "2o-" + surface + (get ? "-get" : "-put");
    const std::string own = agePrefix(sim) + rest;
    if (library(vm).exists(own))
        return own;
    return "a" + rest;
}

// cAnimatable::ComputeReachAnimIndex (BV 0x68ba50): the height band (under
// 0.4 the floor 0, 0.7 a coffee table 1, 0.9 a table 2, else a counter 3)
// + 4 when farther than 0.8 + 8 for a put.
int reachAnimIndex(float height, float distance, bool get) {
    int i = height < 0.4f ? 0 : height < 0.7f ? 1 : height < 0.9f ? 2 : 3;
    if (distance > 0.8f)
        i += 4;
    return get ? i : i + 8;
}

// Where an object is in the lot (x, y and the height over level 1's floor;
// tiles): its position, or the point of the containment slot it is in
// (containmentSlotPoint), as the scene graph places it. Inferred: a level
// as 3 high (StandardLevelHeight) and the terrain flat.
std::array<float, 3> objectPoint(VM& vm, const Object& o, int depth = 0);

// The point of containment slot `slot` of `container`, as try_new_reach
// takes it (BV 0x69111c; docs/seat-mac.md §F): the slot's bone in the
// model when its descriptor stands at bones (0x20000; the model frame is
// the lead tile's), else the slot's own offset, turned by the object's
// direction, from the container's point.
std::array<float, 3> containmentSlotPoint(VM& vm, const Object& container, int slot, int depth = 0) {
    const Slots::Slot* s = containmentSlot(vm, container, slot);
    const Object* frame = &container;
    std::array<float, 3> offset{};
    if (s && (s->unknown10 & 0x20000)) {
        if (const Object* lead = vm.object(multiTileGroup(container)))
            frame = lead;
        if (const auto bone = vm.slotBone(*s, *frame))
            offset = *bone;
    } else if (s) {
        offset = s->offset;
    }
    std::array<float, 3> at = objectPoint(vm, *frame, depth + 1);
    const int dir = ((frame->data[objdata::Direction] % 8) + 8) % 8;
    const double yaw = (90.0 - 45.0 * dir) * M_PI / 180.0;
    at[0] += float(offset[0] * std::cos(yaw) - offset[1] * std::sin(yaw));
    at[1] += float(offset[0] * std::sin(yaw) + offset[1] * std::cos(yaw));
    at[2] += offset[2];
    return at;
}

std::array<float, 3> objectPoint(VM& vm, const Object& o, int depth) {
    if (depth < 8)
        if (const Object* c = vm.object(o.data[objdata::ContainerId]); c && c != &o && !c->person)
            return containmentSlotPoint(vm, *c, o.data[objdata::SlotNumber], depth);
    const float z = o.position.hasHeight() ? o.position.height : 3.0f * float(o.position.level - 1);
    return {o.position.x, o.position.y, z};
}

// The body's reach animation from the "Reach Animations" (STR# 0x92), as
// try_new_reach looks it up (Mac BV 0x69111c; docs/seat-mac.md §F; the
// order read in round n01 at BV 0x692610, the rest in round n44): entry
// `index` (ComputeReachAnimIndex's, or the operand's) of the surface's own
// list (the container the object is picked up from, or the target put
// into; a person too), then its semi-global's, then the global one (none
// but the global without a surface: the floor), whose eighteen entries are
// "a2o-<floor|tableCoffee|table|counter>-<get|put>" close and far, then
// "c2o-urn-get" and "c2o-floor-book-get". An empty entry passes on to the
// next list; any other is the animation (a "." too: the game finds no
// resource for it, error 0x71 MissingReachAnimation). Objects keep their
// own lists, reached by the operand's index: the chessboard's
// "a2o-chessBoard-get/put-northEast/.../southWest", the bar's
// "a2o-barstool-seated-get" (entry 0 of a two-entry list), the phones',
// the books'. The reached object's own lists are not looked at (its STR#
// 0x86 gives the object's animation, not the body's). Empty when no list
// has the entry (the generic name then; inferred: the game's lookup gives
// an empty name).
std::string reachListAnimation(VM& vm, const Object& sim, const Object* surface, size_t index) {
    std::vector<uint32_t> groups;
    if (surface)
        groups.push_back(surface->group), groups.push_back(surface->semiGroup);
    groups.push_back(kGlobalGroup);
    for (uint32_t g : groups) {
        const StringTable* t = g ? vm.code().strings(g, 0x92) : nullptr;
        if (!t)
            continue;
        std::string name = index < t->count(lang::EnglishUS) ? t->get(index, lang::EnglishUS) : std::string();
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
            name.pop_back();
        if (name.empty())
            continue;
        // The Sim's own version when there is one ("c2o-...": AnimPrefix).
        if (name.size() > 1 && name[0] == 'a') {
            const std::string own = agePrefix(sim) + name.substr(1);
            if (library(vm).exists(own))
                return own;
        }
        return name;
    }
    return {};
}

// Tick of the animation's event 0 ("transfer object to hand" / "... to
// slot"), the moment the object changes hands, in VM ticks; nothing if the
// animation has none.
std::optional<int> transferTick(VM& vm, const std::string& name) {
    const auto t = library(vm).eventTicks(name, "event=0");
    if (!t)
        return std::nullopt;
    return int(std::lround(double(*t) / Animation::kTicksPerSecond * vm.clock.ticksPerSecond));
}

int ticksFor(VM& vm, float seconds) {
    return int(std::ceil(double(seconds) * vm.clock.ticksPerSecond - 1e-6));
}

// Entry `index` of the adult animation list (STR# 0x81) for the running code:
// the tree's group, the code owner's, its semi-global, then the global one
// (as the animation primitives look lists up). Empty if none has it.
std::string adultListEntry(Context& c, int index) {
    CodeLibrary& code = c.vm.code();
    std::vector<uint32_t> groups{c.frame.group};
    if (const Object* owner = c.vm.object(c.frame.codeOwner))
        groups.push_back(owner->group), groups.push_back(owner->semiGroup);
    groups.push_back(kGlobalGroup);
    for (uint32_t g : groups)
        if (const StringTable* t = g ? code.strings(g, animlist::Adult) : nullptr)
            if (index >= 0 && size_t(index) < t->count(lang::EnglishUS))
                return t->get(size_t(index), lang::EnglishUS);
    return {};
}

// The grasp and carry overlays of a list entry "a2o-newspaper-grasp,
// a2o-newspaper-carry" or "a2o-phoneWall-grasp, a2o-phoneTable-carry(carry)":
// each name without its role and speed, resolved for the Sim's age.
std::vector<std::string> carryOverlays(VM& vm, const Object& sim, const std::string& entry) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t comma = entry.find(',', start);
        std::string item = entry.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (const size_t cut = item.find_first_of("(@"); cut != std::string::npos)
            item.resize(cut);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t'))
            item.erase(item.begin());
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t'))
            item.pop_back();
        if (!item.empty() && item != ".")
            out.push_back(resolveListAnimation(vm, sim, item));
        if (comma == std::string::npos)
            return out;
        start = comma + 1;
    }
}

// cTSObject::PlaceInSlot (BV 0xafcc28): the container's position, room and
// direction (docs/seat-mac.md §B).
void putInSlot(VM& vm, Object& x, const Object& container, int slot) {
    vm.setContainer(x, container.id, int16_t(slot));
    vm.setPosition(x, container.position);
    x.data[objdata::Room] = container.data[objdata::Room];
    vm.noteRoomChanged(x.id);
    x.data[objdata::Direction] = container.data[objdata::Direction];
    x.route.reset();
}

// The hands are a Sim's containment slots 0 and 1 ("Object in Hand - Get
// ID(tmp 0) & Hand(tmp 3)" reads my slot 0, then my slot 1, into temp 3).
constexpr int kHands = 2;

// Reach/Put (0x74), a Sims 2 primitive ("Reach/Put" in STR# 0x8B; The Sims
// 1's Reach, 0x2F, is "~(old)Reach"), checked against the Mac binary
// (nTSSG::cAnimatable::try_new_reach, NewReachParam; docs/primitives-mac.md
// §4). Operands: u16 at 1 and u16 at 7 (not read), the object (owner u8 at
// 3, data u16 at 4), the containment slot u8 at 6 (mode 1), flags u8 at 9
// (0x01 the slot is in temp 0, 0x02 the hand is in temp 3, 0x04 the object
// animation is filtered by the Sim's age, 0x08 the body's animation is entry
// byte 15 of the Reach Animations, BV 0x692698), mode u8 at 10 (0 pick up the
// object, 1 put what the hand holds into the object's slot, 2 put it on the
// floor), the grasp animation u16 at 11 (an entry of the adult list STR#
// 0x81, 0xFFFF none) and the object animation u16 at 13 (STR# 0x86, 0xFFFF
// none; not played here). The doc reads both as index + 1, but the game's
// lists fit 0-based indices ("CT - Reach - Grab" of the toasting glass gives
// 0 = "a2o-toastingGlass-grasp, a2o-toastingGlass-carry" and object
// animation 1 = "o2a-toastingGlass-pose"), so they are taken as is. Only
// Sims reach (error 0x69). Picking up an object that does not exist is
// false; one already in that hand is true at once. Putting: an out-of-range
// slot is error 0x1B; a target that does not exist is error 0x6A
// (inferred); an occupied slot or empty hands are false (inferred). The
// animation errors are the game's (0x71 missing, 0x6E without event 0).
//
// The Sim plays the reach animation of the Reach Animations
// (reachListAnimation: "a2o-floor-get", "a2o-counter-put",
// "a2o-chessBoard-get-southEast", ...) and the object changes hands at the
// animation's event 0;
// the instruction returns true when the animation ends. Picking up puts the
// object in the hand (temp 3 with 0x02, else the first free one; false with
// both full) and adds the grasp and carry animations as overlays on the Sim;
// putting takes them off and puts what the hand holds into the slot, or on
// the floor tile in front of the Sim (else its own tile), facing as the Sim.
// Not done: the binary's "reach object ID" / "reach surface object ID" of
// the Sim (cTSPerson::SetReachObjectID; which person data the trees read
// them through is not known), the object animation.
Exit reachPut(Context& c) {
    const auto& o = c.instruction.operands;
    Object& me = c.me();
    if (!me.person)
        throw gameError(err::ReachOnNonSim);
    const uint8_t mode = o[10], flags = o[9];
    if (mode > 2)
        throw VMError("Reach/Put: mode " + std::to_string(mode) + " not implemented");
    const size_t depth = c.thread.stack.size();
    AnimationState& anim = me.animation;

    if (me.reach && me.reach->depth == depth && me.reach->tree == c.frame.behavior && me.reach->node == c.frame.pc) {
        Object::Reach& r = *me.reach;
        ++r.tick;
        if (!r.transferred && r.tick >= r.transferAt) {
            r.transferred = true;
            Object* x = c.vm.object(r.object);
            Object* to = c.vm.object(r.target);
            if (r.mode == 0) {
                if (x && x->data[objdata::ContainerId] != me.id) {
                    putInSlot(c.vm, *x, me, r.hand);
                    me.carryOverlays = r.overlays;
                    // The grasp at level 3, the carry pose at 5 (try_new_reach).
                    for (size_t k = 0; k < r.overlays.size(); ++k) {
                        Overlay& ov = anim.addOverlay(r.overlays[k]);
                        ov.oeType = oetype::Carry;
                        ov.level = k == 0 && r.overlays.size() > 1 ? animlevel::Grasp : animlevel::Carry;
                    }
                    ++anim.overlaySerial;
                }
            } else if (x) {
                // The carry poses go: those the pick-up added, and those the
                // put's own grasp entry names, every overlay of each name
                // (a mug poured at the coffee machine is put in the hand by
                // the trees, its "a2o-coffeeMug-carry" started by "Interaction
                // - Walk and Sip" with Animate Overlay; "CT - Reach - Drop"
                // puts it down with "a2o-coffeeMug-grasp,a2o-coffeeMug-carry"
                // and nothing else stopped it: the arm held the mug that was
                // on the table until the carpool came; live QA 15. Inferred:
                // try_new_reach hands the entry's names to AddToAnimList,
                // whose operation removes them when putting).
                std::vector<std::string> names = me.carryOverlays;
                names.insert(names.end(), r.overlays.begin(), r.overlays.end());
                std::erase_if(anim.overlays, [&](const Overlay& ov) {
                    return std::find(names.begin(), names.end(), ov.name) != names.end();
                });
                me.carryOverlays.clear();
                ++anim.overlaySerial;
                if (r.mode == 1 && to) {
                    putInSlot(c.vm, *x, *to, r.slot);
                } else {
                    c.vm.setContainer(*x, 0, -1);
                    c.vm.setPosition(*x, {float(r.floor.x) + 0.5f, float(r.floor.y) + 0.5f, r.floor.level});
                    x->data[objdata::Direction] = me.data[objdata::Direction];
                    if (c.vm.world) {
                        x->data[objdata::Room] = c.vm.world->room(r.floor);
                        c.vm.noteRoomChanged(x->id);
                    }
                }
            }
        }
        if (r.tick < r.end)
            return Exit::Yield;
        const std::string played = r.animation;
        me.reach.reset();
        if (anim.name == played) {
            anim.name.clear();
            anim.tree = nullptr;
        }
        return Exit::True;
    }
    me.reach.reset();

    Object::Reach r;
    r.mode = mode;
    r.depth = depth, r.tree = c.frame.behavior, r.node = c.frame.pc;
    auto held = [&](int hand) { return c.vm.containedIn(me.id, int16_t(hand)); };
    // The surface whose Reach Animations name the body's animation, and the
    // reach point (try_new_reach, BV 0x69111c; docs/seat-mac.md §F): the
    // slot the object is in or goes into (its bone or offset), the object's
    // own place when it is in no slot, 0.7 ahead of the Sim for the floor.
    const Object* surface = nullptr;
    std::array<float, 3> point{};
    if (mode == 0) {
        Object* x = c.vm.object(c.get(o[3], int16_t(le16(&o[4]))));
        if (!x)
            return Exit::False; // the binary: no such object is false
        if (x->data[objdata::ContainerId] == me.id &&
            (!(flags & 0x02) || x->data[objdata::SlotNumber] == c.thread.temps[3]))
            return Exit::True; // already in (that) hand: true without animating
        r.object = x->id;
        r.hand = -1;
        if (flags & 0x02) {
            const int h = c.thread.temps[3];
            if (h >= 0 && h < kHands && !held(h))
                r.hand = h;
        } else {
            for (int h = 0; h < kHands && r.hand < 0; ++h)
                if (!held(h))
                    r.hand = h;
        }
        if (r.hand < 0)
            return Exit::False;
        if (const Object* on = c.vm.object(x->data[objdata::ContainerId]); on && on != x) {
            surface = on;
            point = on->person ? objectPoint(c.vm, *on) : containmentSlotPoint(c.vm, *on, x->data[objdata::SlotNumber]);
        } else {
            point = objectPoint(c.vm, *x);
        }
        if (const uint16_t grasp = le16(&o[11]); grasp != 0xFFFF)
            if (const std::string entry = adultListEntry(c, grasp); !entry.empty())
                r.overlays = carryOverlays(c.vm, me, entry);
    } else {
        // What the hand holds: the hand in temp 3 with flag 0x02, else the first that holds something.
        ObjectId x = 0;
        if (flags & 0x02) {
            const int h = c.thread.temps[3];
            x = h >= 0 && h < kHands ? held(h) : 0;
        } else {
            for (int h = 0; h < kHands && !x; ++h)
                x = held(h);
        }
        if (!x)
            return Exit::False;
        r.object = x;
        if (const uint16_t grasp = le16(&o[11]); grasp != 0xFFFF)
            if (const std::string entry = adultListEntry(c, grasp); !entry.empty())
                r.overlays = carryOverlays(c.vm, me, entry);
        if (mode == 1) {
            const Object* to = c.vm.object(c.get(o[3], int16_t(le16(&o[4]))));
            if (!to)
                throw gameError(err::NoReachObject);
            r.target = to->id;
            const size_t slots = containmentSlotCount(c.vm, *to);
            r.slot = flags & 0x01 ? c.thread.temps[0] : o[6];
            if (r.slot < 0 || size_t(r.slot) >= slots)
                throw gameError(err::SlotOutOfRange, "slot " + std::to_string(r.slot) + " of " + std::to_string(slots));
            if (const ObjectId there = c.vm.containedIn(to->id, int16_t(r.slot)); there && there != x)
                return Exit::False;
            surface = to;
            point = containmentSlotPoint(c.vm, *to, r.slot);
        } else {
            if (!me.position.inWorld())
                throw VMError("Reach/Put: dropping onto the floor while out of world");
            const TilePos at = VM::tileOf(me);
            const auto [dx, dy] = directionStep(((me.data[objdata::Direction] % 8) + 8) % 8);
            const TilePos front{at.x + dx, at.y + dy, at.level};
            const ObjectId ignore[] = {x, me.id};
            r.floor = c.vm.world && c.vm.tileFree(front, ignore) ? front : at;
            const double yaw = (90.0 - 45.0 * (((me.data[objdata::Direction] % 8) + 8) % 8)) * M_PI / 180.0;
            point = objectPoint(c.vm, me);
            point[0] += float(-0.7 * std::sin(yaw));
            point[1] += float(0.7 * std::cos(yaw));
            point[2] = 3.0f * float(me.position.level - 1);
        }
    }

    // The entry: the operand's (flag 0x08, byte 15), else ComputeReachAnimIndex
    // of the reach point's height over the Sim's root and its distance from
    // it (the point in the Sim's frame: a seated Sim's root is in the seat).
    const std::array<float, 3> root = objectPoint(c.vm, me);
    const float height = point[2] - root[2], distance = std::hypot(point[0] - root[0], point[1] - root[1]);
    const int index = (flags & 0x08) ? o[15] : reachAnimIndex(height, distance, mode == 0);
    const std::string listed = reachListAnimation(c.vm, me, surface, size_t(index));
    r.animation = listed.empty() ? reachAnimation(c.vm, me, surfaceOfIndex(index), mode == 0) : listed;
    if (std::getenv("OS2_DEBUG_REACH")) {
        const Object* x = c.vm.object(r.object);
        const Object* on = x ? c.vm.object(x->data[objdata::ContainerId]) : nullptr;
        std::fprintf(stderr, "reach: Sim %d mode %d object %d (%s, %08X/%08X) in %d (%s, %08X/%08X) target %d: height %.2f distance %.2f index %d%s: %s\n",
                     me.id, mode, r.object, x && x->definition ? x->definition->fileName.c_str() : "?", x ? x->group : 0,
                     x ? x->semiGroup : 0, on ? on->id : 0, on && on->definition ? on->definition->fileName.c_str() : "",
                     on ? on->group : 0, on ? on->semiGroup : 0, r.target, height, distance, index, (flags & 0x08) ? " (operand)" : "",
                     r.animation.c_str());
    }
    const auto seconds = library(c.vm).seconds(r.animation);
    if (!seconds)
        throw gameError(err::MissingReachAnimation, r.animation);
    const auto transfer = transferTick(c.vm, r.animation);
    if (!transfer)
        throw gameError(err::NoReachEvent0, r.animation);
    r.end = std::max(1, ticksFor(c.vm, *seconds));
    r.transferAt = std::clamp(*transfer, 0, r.end);
    anim.name = r.animation;
    ++anim.serial;
    anim.ticksLeft = r.end;
    anim.requestId = 0;
    anim.depth = depth, anim.tree = c.frame.behavior, anim.node = c.frame.pc;
    me.reach = std::move(r);
    return Exit::Yield;
}

// --- Snap ----------------------------------------------------------------------

// Snap (0x2E): moves me at once (no walking) to a routing slot of the stack
// object (TrySnap in the Mac binary; docs/primitives-mac.md §4/§5): slot u16
// at 0, mode u16 at 2 (STR# 0xCF: 0 the routing slot whose number is in the
// parameter of u16 0, 1 be contained: into the stack object's containment
// slot 0, 2 in front of it, 3 the literal routing slot, 4 the global SLOT),
// flags u16 at 4: 0x02 ask a person in the way to move (nobody is asked
// here: the snap just fails), 0x08 the slot number is in temp 0, 0x10 test
// only; u16 8 = 1 "from slot in temp 1" (not done). No stack object: error
// 0x15; a slot that does not exist: error 0x1B. A failure is false and
// writes my object data 52 "primitive result": 1 when a Sim in the way was
// asked to move (0x02), else the placement error + 2 (TrySnap: the first
// error of the placement test's list): 0x0C "Can't intersect other
// objects" + 2 = 0x0E for a goal taken, 2 "Location out of bounds" + 2 = 4
// when every goal is off the lot (the carpool's "Drive - Drive Leave" waits
// for 4 to know the car has reached the end of the street). The place is the slot's goal
// with the lowest cost whose tile is free (the Sim's own and, for a slot on
// the object, the object's tile do not count), with the slot's facing. The
// Sims 2's SLOTs have no snap target slot (-1 everywhere): sitting is a Snap
// followed by a Snap Into.
// Whether a wall of the point's own tile comes within `r` of it (TrySnap's
// wall test, FUN_008a6c84, BV: the tile's four edges by their distance
// from the point, the diagonals by the distance to their line; a wall with
// a door counts only by its ends there, and here doors open their edge, so
// they do not count: inferred).
bool wallWithin(const World& w, float px, float py, int level, float r) {
    const TilePos t{int(std::floor(px)), int(std::floor(py)), level};
    if (!w.contains(t))
        return false;
    const float fx = px - float(t.x), fy = py - float(t.y);
    if ((w.wallBetween(t, 0, -1) && fy < r) || (w.wallBetween(t, -1, 0) && fx < r) ||
        (w.wallBetween(t, 0, 1) && 1 - fy < r) || (w.wallBetween(t, 1, 0) && 1 - fx < r))
        return true;
    const int d = w.diagonalSides(t);
    return ((d & 0x10) && std::fabs(fy - (1 - fx)) / float(M_SQRT2) < r) || ((d & 0x20) && std::fabs(fy - fx) / float(M_SQRT2) < r);
}

Exit snap(Context& c) {
    const auto& o = c.instruction.operands;
    const uint16_t index = le16(&o[0]), where = le16(&o[2]);
    const uint16_t flags = le16(&o[4]);
    Object& me = c.me();
    if (!c.vm.object(c.frame.stackObject))
        throw gameError(err::NoStackObject, "Snap");
    Object& target = c.stackObject();
    constexpr int16_t kPrimitiveResult = 52;
    // (1: a person in the way was asked to move, flag 0x02; the binary asks
    // it, MoveOutOfWay, and writes 1 when it accepts: here nobody is asked,
    // inferred as before. An object in the way: the error + 2.)
    auto fail = [&](int placementError = 0x0C, bool person = true) {
        if (std::getenv("OS2_SNAP_FAILS")) // why snaps fail, for looking into
            std::fprintf(stderr, "t%llu snap of %d to %d at (%.2f, %.2f, %d) (mode %u slot %u flags %X) failed: %d\n", (unsigned long long)c.vm.ticks,
                         me.id, c.frame.stackObject, target.position.x, target.position.y, target.position.level, where, index, flags, placementError);
        me.data[kPrimitiveResult] = int16_t(placementError == 0x0C && person && (flags & 0x02) ? 1 : placementError + 2);
        return Exit::False;
    };
    if (where > 4)
        throw VMError("Snap: slot scope " + std::to_string(where) + " not implemented");
    if (where == 1) {
        if (c.vm.containedIn(target.id, 0) && c.vm.containedIn(target.id, 0) != me.id)
            return fail();
        if (!(flags & 0x10))
            putInSlot(c.vm, me, target, 0);
        return Exit::True;
    }
    Slots::Slot slot;
    if (where == 2) {
        slot.type = 3;
        slot.flags = 0x01; // in front, relative to the object
        slot.minDistance = slot.optimalDistance = slot.maxDistance = 1;
        slot.facing = -2;
    } else {
        int n = flags & 0x08 ? c.thread.temps[0] : where == 0 ? c.get(0x09, int16_t(index)) : int(index);
        const Slots* table = where == 4 ? c.vm.code().slots(kGlobalGroup, 0x64) : slotTable(c.vm, target);
        const Slots::Slot* found = nullptr;
        if (table)
            for (const auto& s : table->slots)
                if (s.type == 3 && n-- == 0) {
                    found = &s;
                    break;
                }
        if (!found)
            throw gameError(err::SlotOutOfRange, "routing slot " + std::to_string(index));
        slot = *found;
    }
    if (!target.position.inWorld())
        return fail();
    // The place (TrySnap, BV 0xe43624; docs/routing-mac.md §9.1): one point,
    // the target's position plus TotalRoutingSlotOffsetFromContainer turned
    // by the target's direction: a bone slot's first bone (the model's, as
    // the bone goals have it), else the slot's own offset; "in front" (mode
    // 2) the tile ahead of the target. No scan, no ring around the bones.
    // The router's radius (+0x210, the slot displacement) is 0 for the base
    // game's Sims. OS2_SNAP_SCAN=1: the cheapest free goal of the slot's
    // scan, as before round n18 (to compare).
    static const bool scan = std::getenv("OS2_SNAP_SCAN") != nullptr;
    float px = target.position.x, py = target.position.y;
    int level = target.position.level, facing = -1;
    const int objDir = ((target.data[objdata::Direction] % 8) + 8) % 8;
    VM::RouteGoals goals;
    if (scan || (slot.unknown10 & 0x20000) || (slot.flags & 0x1000))
        goals = c.vm.routingGoals(slot, target);
    if (where == 2) {
        const auto step = directionStep(objDir);
        px += float(step[0]), py += float(step[1]);
    } else if (!scan && !(slot.unknown10 & 0x20000)) {
        if ((slot.flags & 0x1000) && !goals.goals.empty()) {
            px = goals.points[0][0], py = goals.points[0][1], facing = goals.facings[0];
        } else {
            const double yaw = (90.0 - 45.0 * objDir) * M_PI / 180.0;
            px += float(slot.offset[0] * std::cos(yaw) - slot.offset[1] * std::sin(yaw));
            py += float(slot.offset[0] * std::sin(yaw) + slot.offset[1] * std::cos(yaw));
        }
    } else if (!scan) {
        // The first bone: the bone goals come in the slot's order, the
        // first one first (no bone on the model: the target's point).
        if (!goals.goals.empty() && !goals.multi.empty())
            px = goals.points[0][0], py = goals.points[0][1], facing = goals.facings[0], level = goals.level;
    }
    // The target's own footprint does not count (inferred, as before: the
    // binary's test has no exceptions), nor what its "allow intersection"
    // tree lets me into (the toilet's privacy marker in front of it, for
    // its user getting up).
    static const bool targetFree = std::getenv("OS2_SNAP_TARGET_FREE") != nullptr;
    std::vector<ObjectId> ignore{me.id};
    if (scan || targetFree)
        ignore.push_back(target.id);
    auto ignoreAround = [&](float x, float y) {
        const TilePos t{int(std::floor(x)), int(std::floor(y)), level};
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (const ObjectId id : c.vm.objectsOnTile({t.x + dx, t.y + dy, level}))
                    if (const Object* o = c.vm.object(id); o && VM::blocksRouting(*o) && objectsMayIntersect(c.vm, *o, me))
                        ignore.push_back(id);
    };
    static const bool wholeTiles = std::getenv("OS2_TILE_FOOTPRINTS") != nullptr;
    if (scan) {
        for (size_t i = 0; i < goals.goals.size(); ++i)
            ignoreAround(goals.points[i][0], goals.points[i][1]);
        size_t best = goals.goals.size();
        for (size_t i = 0; i < goals.goals.size(); ++i) {
            const TilePos t{goals.goals[i].x, goals.goals[i].y, goals.level};
            const bool free = !c.vm.world || (wholeTiles ? c.vm.tileFree(t, ignore)
                                                         : c.vm.pointFree(goals.points[i][0], goals.points[i][1], goals.level, ignore));
            if (free && (best == goals.goals.size() || goals.goals[i].cost < goals.goals[best].cost))
                best = i;
        }
        if (best == goals.goals.size()) {
            bool onLot = goals.goals.empty() || !c.vm.world;
            for (const auto& g : goals.goals)
                onLot = onLot || c.vm.world->contains(TilePos{g.x, g.y, goals.level});
            return fail(onLot ? 0x0C : 2);
        }
        px = goals.points[best][0], py = goals.points[best][1], facing = goals.facings[best], level = goals.level;
    } else if (c.vm.world) {
        if (std::getenv("OS2_SNAP_FAILS"))
            std::fprintf(stderr, "t%llu snap of %d to %d: place (%.2f, %.2f, %d)%s, slot offset (%.2f, %.2f) flags %X/%X\n",
                         (unsigned long long)c.vm.ticks, me.id, target.id, px, py, level,
                         (slot.unknown10 & 0x20000) ? " (bone)" : "", slot.offset[0], slot.offset[1], slot.flags, slot.unknown10);
        ignoreAround(px, py);
        // The tests (TrySnap's own, BV 0xe43084), in order: a person (not
        // with bit 0x04 of person data 68) within 0.3 of a wall: error 8;
        // the slot minding rooms (no RS flag 0x800): off the lot, error 2,
        // and another room than the one I stand in fails too (inferred: the
        // partition manager's 0x13c as the room, the error as 2); the slot
        // minding walls (no RS flag 0x80000), on my level: a wall between
        // where I stand and the point (the tiles' line of sight here),
        // error 8; then TSObjectCanSetPositionAndOrientation: objects'
        // footprints and other Sims within 0.283 (error 0x0C).
        const World& w = *c.vm.world;
        const TilePos t{int(std::floor(px)), int(std::floor(py)), level};
        const bool ghost = me.person && me.person->data.size() > 68 && (me.person->data[68] & 0x04);
        if (me.person && !ghost && wallWithin(w, px, py, level, 0.3f))
            return fail(8);
        const bool mindRooms = where == 2 || !(slot.flags & 0x800);
        if (mindRooms && !w.contains(t))
            return fail(2);
        const TilePos from = VM::tileOf(me);
        if (mindRooms && me.position.inWorld() && from.level == level && w.contains(from) && w.room(from) != w.room(t))
            return fail(2);
        if ((where == 2 || !(slot.flags & 0x80000)) && me.position.inWorld() && from.level == level && !w.lineOfSight(from, t))
            return fail(8);
        if (!w.contains(t)) // the placement test's "location out of bounds"
            return fail(2);
        if (!(wholeTiles ? c.vm.tileFree(t, ignore) : c.vm.pointFree(px, py, level, ignore)))
            return fail(0x0C, false);
        if (me.person)
            for (const ObjectId id : c.vm.people()) {
                const Object* o = c.vm.object(id);
                if (!o || id == me.id || !o->position.inWorld() || o->data[objdata::ContainerId] != 0 || o->position.level != level ||
                    std::hypot(o->position.x - px, o->position.y - py) >= 0.283f)
                    continue;
                if ((me.data[objdata::Flags] & 0x14) || (o->data[objdata::Flags] & 0x14) ||
                    routing_internal::intersectionAllowed(c.vm, *o, me))
                    continue;
                return fail(0x0C);
            }
    }
    if (flags & 0x10)
        return Exit::True;
    c.vm.setContainer(me, 0, -1);
    if (!(me.route && me.route->inPortal)) // a portal tree moves the walker
        me.route.reset();
    else // and the walk goes on from where it puts it: the locomotor's frame
         // stayed where the walker went into the portal (a modular stair's
         // tree snaps it to the bottom; the walk cycle carried it back to the
         // top's point on the lower level, N002_Lot2's Olive, walled in)
        me.route->loco = Object::Route::Loco{};
    const Position from = me.position;
    const int fromDir = me.data[objdata::Direction];
    c.vm.setPosition(me, {px, py, level});
    // The direction: the slot's only when it "snaps to direction" (flag
    // 0x1000, the slot's vtable 0xE4: the target's direction turned by the
    // bone's or the slot's); otherwise the snapped object keeps its own
    // (TrySnap, BV 0xe43624; docs/seat-mac.md §H).
    if (facing >= 0 && (slot.flags & 0x1000))
        me.data[objdata::Direction] = int16_t(facing);
    if (std::getenv("OS2_DEBUG_SNAP"))
        std::fprintf(stderr, "t%llu snap of %d to %d (mode %u slot %u flags %X) at (%.2f, %.2f) dir %d -> %d\n",
                     (unsigned long long)c.vm.ticks, me.id, target.id, where, index, flags, me.position.x, me.position.y, fromDir,
                     me.data[objdata::Direction]);
    // A multi-tile object moves whole: its other parts keep their places
    // around me, turned with me (the carpool's car snapping down the street
    // to its "slot_travel_drive" bone; inferred, as placement moves groups).
    if (const ObjectId group = multiTileGroup(me); group && !me.person && from.inWorld()) {
        const int turn = ((me.data[objdata::Direction] - fromDir) % 8 + 8) % 8;
        const double a = -turn * M_PI / 4; // directions go clockwise
        std::vector<ObjectId> parts;
        for (const auto& [id, x] : c.vm.eachObject())
            if (id != me.id && multiTileGroup(x) == group && x.position.inWorld() && !x.person)
                parts.push_back(id);
        for (ObjectId id : parts) {
            Object& part = *c.vm.object(id);
            const float dx = part.position.x - from.x, dy = part.position.y - from.y;
            Position moved = part.position;
            moved.x = me.position.x + float(dx * std::cos(a) - dy * std::sin(a));
            moved.y = me.position.y + float(dx * std::sin(a) + dy * std::cos(a));
            moved.level = me.position.level;
            c.vm.setPosition(part, moved);
            part.data[objdata::Direction] = int16_t(((part.data[objdata::Direction] + turn) % 8 + 8) % 8);
            if (c.vm.world) {
                part.data[objdata::Room] = c.vm.world->room(VM::tileOf(part));
                c.vm.noteRoomChanged(part.id);
            }
        }
    }
    if (c.vm.world) {
        me.data[objdata::Room] = c.vm.world->room(VM::tileOf(me));
        c.vm.noteRoomChanged(me.id);
    }
    return Exit::True;
}

} // namespace

void registerReachPrimitives(VM& vm) {
    vm.setPrimitive(0x2E, snap);
    vm.setPrimitive(0x0E, findBestObjectForFunction);
    vm.setPrimitive(0x14, runFunctionalTree);
    vm.setPrimitive(0x74, reachPut);
}

} // namespace os2::vm
