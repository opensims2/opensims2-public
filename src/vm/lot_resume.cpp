// Starting a lot's objects from their saved state (lot_resume.hpp).

#include "lot_resume.hpp"
#include "motives.hpp"
#include "rooms.hpp"

#include <cstdio>
#include <cstdlib>
#include "placement.hpp"

#include "formats/lot_objects.hpp"
#include "interactions.hpp"
#include "vm.hpp"
#include "thread_state.hpp"
#include "formats/strings.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace os2::vm {

namespace {

// OBJD field "object version" (label 49 of STR# 0xCC) and object data 58
// "object version" (STR# 0x8D).
constexpr size_t kLoadFunction = 2;

// The saved floats are the motive engine's (cTSMotives, current and old
// sets; docs/motives-mac.md §3): the trees see them rounded, the engine
// goes on from the fractions. Not clamped (a mood booster keeps the mood
// above 100: Mortimer's 232.1 in N001_Lot11).
void restoreMotives(Object& o, const LotObjects::State& s) {
    if (!o.person || !s.personRead)
        return;
    Person& p = *o.person;
    for (size_t k = 0; k < kMotiveCount && k < s.motives.size(); ++k) {
        p.exactMotives[k] = s.motives[k];
        p.motives[k] = p.writtenMotives[k] = motiveView(s.motives[k]);
        p.oldMotives[k] = k < s.motives2.size() ? s.motives2[k] : s.motives[k];
    }
    p.exactMotivesValid = true;
    p.oldMotivesValid = true;
}

// The Set Motive Changes that were running (cTSPerson +0x618,
// cMotiveInc::Read, Mac BV 0xad2764: motive, change per tick, limit), as
// cTSPerson::OnSimulatorTick goes on with them.
void restoreMotiveChanges(Object& o, const LotObjects::State& s) {
    if (!o.person || !s.personRead)
        return;
    o.person->motiveChanges.clear();
    for (const auto& m : s.motiveIncs)
        if (m.motive < kMotiveCount)
            o.person->motiveChanges.push_back({int16_t(m.motive), m.a, m.b});
}

// As cTSObject::Read (BV 0xb14cb0): the attributes take the saved count,
// the semi-attributes grow to the saved count (never shrink), and the object
// keeps its definition's arrays, the saved ones copied into them (more saved
// arrays than that are read and dropped). The game never makes fewer than 8
// attributes, so a state without any is one we could not read: the object
// keeps its own.
void restoreState(VM& vm, Object& o, const LotObjects::State& s) {
    if (!s.attributes.empty())
        o.attributes.assign(s.attributes.begin(), s.attributes.end());
    if (o.semiAttributes.size() < s.semiAttributes.size())
        o.semiAttributes.resize(s.semiAttributes.size());
    std::copy(s.semiAttributes.begin(), s.semiAttributes.end(), o.semiAttributes.begin());
    for (size_t i = 0; i < s.data.size() && i < o.data.size(); ++i)
        o.data[i] = s.data[i];
    o.data[objdata::ObjectId] = o.id;
    vm.noteRoomChanged(o.id); // a loaded object is told its room (the pass that used to scan for it)
    vm.noteContainer(o);
    if (o.person && s.personRead)
        for (size_t i = 0; i < s.personData.size() && i < o.person->data.size(); ++i)
            o.person->data[i] = s.personData[i];
    restoreMotives(o, s);
    restoreMotiveChanges(o, s);
    for (auto& a : o.arrays)
        a = {};
    for (size_t i = 0; i < s.arrays.size() && i < o.arrays.size(); ++i)
        o.arrays[i] = {s.arrays[i], i < s.arrayIterators.size() ? s.arrayIterators[i] : int16_t(-1)};
    // The temporary token: its fields as data owner 0x27 numbers them (kind,
    // inventory ID, counted, index, GUID, flags, count) and the token itself.
    const auto& f = s.tempTokenFields;
    TempToken& t = o.tempToken;
    t.kind = f[0] >= 0 && f[0] <= 3 ? InventoryKind(f[0]) : InventoryKind::Global;
    t.inventoryId = f[1];
    t.counted = f[2] != 0;
    t.index = f[3];
    std::copy(f.begin() + 4, f.end(), t.copied.begin());
    t.token.guid = s.token.guid;
    t.token.flags = int16_t(s.token.flags);
    t.token.properties = s.token.properties;
    t.loaded = s.token.guid != 0;
    // Timers (cTSObject::ReadTimers; docs/save-mac.md §8).
    o.timers.clear();
    for (const auto& st : s.timers) {
        if (o.timers.size() >= kMaxTimersPerObject)
            break;
        ObjectTimer x;
        x.id = int16_t(st.id);
        x.tree = uint16_t(st.tree);
        x.period = st.period;
        x.ticks = st.ticksLeft;
        x.flags = st.flags;
        x.object = ObjectId(st.object);
        x.params = st.params;
        o.timers.push_back(x);
    }
    vm.noteTimers(o);
}

// The saved interactions of a Sim (cTSPerson: the interaction in its slot
// at +0x5D4, which holds the running one, then the queue at +0x584; in the
// game's lots the queues are empty and 3 of the 55 Sims have one running):
// each record becomes a queue entry of the TTAB entry of its stack object
// whose English TTA string is the record's name (else whose TTA index is the
// record's tree table entry), with its priority and parameters. When the
// running one's object is the code owner of a frame of the resumed Main,
// that frame (the first such) is its action tree's root: it is marked as the
// interaction, so it ends the entry when it returns. Records whose entry
// cannot be found are dropped.
// The ID a saved interaction keeps (+0x40, with its link at +0x42; records
// older than version 0x7C have none and get a new one): the IDs go on from
// the saved global 36 (newInteractionId).
uint16_t savedInteractionId(VM& vm, Object& sim, const LotObjects::SavedInteraction& rec) {
    return rec.u40 ? rec.u40 : newInteractionId(vm, sim);
}

// The queue entry a saved interaction record names (see restoreQueue).
std::optional<QueuedInteraction> entryOf(VM& vm, Object& sim, const LotObjects::SavedInteraction& rec) {
    const ObjectId leader = interactionLeader(vm, rec.stackObject);
    const Object* o = vm.object(leader);
    const auto ref = o ? interactionTable(vm, *o) : std::nullopt;
    if (!ref)
        return std::nullopt;
    std::optional<QueuedInteraction> q;
    auto english = [&](uint32_t i) { return ref->strings ? ref->strings->get(i, lang::EnglishUS) : std::string(); };
    if (rec.ttabEntry < 0x10000 && (rec.name.empty() || english(rec.ttabEntry) == rec.name))
        q = makeInteraction(vm, leader, uint16_t(rec.ttabEntry));
    if (!q && ref->strings && !rec.name.empty())
        for (const auto& e : ref->table->interactions)
            if (english(e.stringIndex) == rec.name) {
                q = makeInteraction(vm, leader, uint16_t(e.stringIndex));
                break;
            }
    if (!q)
        return std::nullopt;
    q->priority = int16_t(std::clamp<int64_t>(rec.priority, 0, kPriorityMaximum));
    q->pushedBy = rec.initiator > 0 ? rec.initiator : sim.id;
    q->args = rec.params;
    q->uid = savedInteractionId(vm, sim, rec);
    q->link = rec.u42;
    if (rec.u54 & 2) // the continuation (lot_save.cpp toRecord)
        q->flags |= 0x04;
    q->autonomous = rec.u54 & 0x400;     // autonomous (cTSInteraction vtable 0xbc)
    q->testAutonomous = rec.u54 & 0x800; // tested as autonomous (vtable 0xc4)
    q->noContinuation = rec.u54 & 0x4000; // no continuation after it (vtable 0x8c)
    return q;
}

// The saved sub-queue (+0x5AC) and current sub-action (+0x5DC), as the
// queue's: the sub-action's frame is the first frame of the resumed Main,
// above the running interaction's, whose code owner is its object.
size_t restoreSubQueue(VM& vm, Object& sim, const LotObjects::State& s) {
    sim.subQueue.clear();
    sim.subAction.reset();
    for (const auto& rec : s.list5AC)
        if (auto q = entryOf(vm, sim, rec); q && sim.subQueue.size() < kMaxSubQueue)
            sim.subQueue.push_back(std::move(*q));
    if (s.pending && s.pending->ttabEntry != 0xFFFFFFFF && s.pending->stackObject > 0)
        if (auto q = entryOf(vm, sim, *s.pending)) {
            if (auto t = vm.threads.find(sim.id); t != vm.threads.end()) {
                bool above = false;
                for (Frame& f : t->second.stack) {
                    above = above || f.interaction;
                    if (above && !f.interaction && f.codeOwner != sim.id &&
                        (f.codeOwner == s.pending->stackObject || f.codeOwner == q->object)) {
                        f.subInteraction = true;
                        sim.subAction = std::move(*q);
                        break;
                    }
                }
            }
        }
    syncQueueData(sim);
    return sim.subQueue.size() + (sim.subAction ? 1 : 0);
}

size_t restoreQueue(VM& vm, Object& sim, const LotObjects::State& s) {
    if (!sim.person)
        return 0;
    std::vector<const LotObjects::SavedInteraction*> records;
    const bool running = s.slotA.ttabEntry != 0xFFFFFFFF && s.slotA.stackObject > 0;
    if (running)
        records.push_back(&s.slotA);
    for (const auto& q : s.queue)
        records.push_back(&q);
    if (records.empty())
        return 0;
    sim.queue.clear();
    sim.activeInteraction = -1;
    for (const auto* rec : records) {
        const ObjectId leader = interactionLeader(vm, rec->stackObject);
        const Object* o = vm.object(leader);
        const auto ref = o ? interactionTable(vm, *o) : std::nullopt;
        if (!ref)
            continue;
        // The tree table entry when its English string is the record's name
        // (or the record has none), else the entry with that name.
        std::optional<QueuedInteraction> q;
        auto english = [&](uint32_t i) { return ref->strings ? ref->strings->get(i, lang::EnglishUS) : std::string(); };
        if (rec->ttabEntry < 0x10000 && (rec->name.empty() || english(rec->ttabEntry) == rec->name))
            q = makeInteraction(vm, leader, uint16_t(rec->ttabEntry));
        if (!q && ref->strings && !rec->name.empty())
            for (const auto& e : ref->table->interactions)
                if (english(e.stringIndex) == rec->name) {
                    q = makeInteraction(vm, leader, uint16_t(e.stringIndex));
                    break;
                }
        if (!q || sim.queue.size() >= kMaxQueuedInteractions)
            continue;
        q->priority = int16_t(std::clamp<int64_t>(rec->priority, 0, kPriorityMaximum));
        q->pushedBy = rec->initiator > 0 ? rec->initiator : sim.id;
        q->args = rec->params;
        q->uid = savedInteractionId(vm, sim, *rec);
        q->link = rec->u42;
        if (rec->u54 & 2) // the continuation (lot_save.cpp toRecord)
            q->flags |= 0x04;
        q->autonomous = rec->u54 & 0x400;     // autonomous (cTSInteraction vtable 0xbc)
        q->testAutonomous = rec->u54 & 0x800; // tested as autonomous (vtable 0xc4)
        q->noContinuation = rec->u54 & 0x4000; // no continuation after it (vtable 0x8c)
        sim.queue.push_back(std::move(*q));
    }
    if (sim.queue.empty())
        return 0;
    if (auto t = vm.threads.find(sim.id); running && t != vm.threads.end() && sim.queue[0].object == interactionLeader(vm, s.slotA.stackObject))
        for (Frame& f : t->second.stack)
            if ((f.codeOwner == s.slotA.stackObject || f.codeOwner == sim.queue[0].object) && f.codeOwner != sim.id) {
                f.interaction = true;
                sim.activeInteraction = 0;
                break;
            }
    syncQueueData(sim);
    return sim.queue.size();
}

} // namespace

void applySavedSimulator(VM& vm, const LotObjects& saved) {
    // The simulator's tick counter (cTSSimulator::Read, Mac BV 0x499c20:
    // the u32 after the globals, at least 0), the clock of the persons'
    // 60-tick motive steps (docs/motives-mac.md §8.5).
    if (saved.simulator.read)
        vm.simulatorTicks = uint64_t(std::max<int32_t>(int32_t(saved.simulator.fA0), 0));
    // The base game's lots hold 54 globals (0x94/0x9F: 46; the rest keep
    // what the VM has).
    if (saved.globals.size() <= global::Year)
        return;
    std::copy_n(saved.globals.begin(), std::min(saved.globals.size(), kGlobalCount), vm.globals.begin());
    const auto& g = saved.globals;
    auto& c = vm.clock;
    c.hour = std::clamp<int>(g[global::Hour], 0, 23);
    c.minute = std::clamp<int>(g[global::Minute], 0, 59);
    c.tick = std::clamp<int>(g[global::Second], 0, 59) * c.ticksPerMinute / 60;
    c.day = std::max<int>(g[global::DayOfMonth], 1);
    c.month = std::clamp<int>(g[global::Month], 1, 12);
    c.year = g[global::Year];
    vm.setClockGlobals();
    if (saved.simulator.read) {
        vm.lotValues.lot = int32_t(saved.simulator.fA4);
        vm.lotValues.lotObjects = int32_t(saved.simulator.fA8);
        vm.lotValues.familyObjects = int32_t(saved.simulator.fAC);
        vm.lotValues.known = true;
    }
}

void applySavedMotives(VM& vm, const LotLoad& load, const LotObjects& saved) {
    std::map<int16_t, const LotObjects::State*> states;
    for (const auto& s : saved.objects)
        states[s.id] = &s;
    for (const auto& e : load.objects)
        if (Object* o = e.created ? vm.object(e.id) : nullptr)
            if (auto it = states.find(e.id); it != states.end())
                restoreMotives(*o, *it->second);
}

// The effects the object had running when the lot was saved
// (cTSObject::ReadEffects: name and node): back as the VM's effects (the
// PlumbBob's glow, the flowers' bees), numbered by the object's own STR#
// 0x8F (its semi-global's, the global's) so that a later stop finds them.
void restoreEffects(VM& vm, Object& o, const LotObjects::State& s) {
    static uint32_t serial = 0x40000000; // apart from Effect Stop/Start's serials
    for (const auto& e : s.effects) {
        if (e.name.empty() || o.animation.effects.size() >= 64)
            continue;
        ActiveEffect a;
        a.name = e.name;
        a.node = e.node;
        a.index = -1;
        for (uint32_t g : {o.group, vm.code().semiGlobalFor(o.group), kGlobalGroup})
            if (const StringTable* t = g ? vm.code().strings(g, 0x8F) : nullptr)
                for (size_t i = 0; i < t->count(lang::EnglishUS) && a.index < 0; ++i)
                    if (t->get(i, lang::EnglishUS) == e.name)
                        a.index = int16_t(i);
        a.serial = ++serial;
        o.animation.effects.push_back(std::move(a));
    }
}

ResumeResult resumeLotObjects(VM& vm, const LotLoad& load, const LotObjects& saved) {
    ResumeResult out;
    std::map<int16_t, const LotObjects::State*> states;
    for (const auto& s : saved.objects)
        states[s.id] = &s;
    std::vector<ObjectId> ids;
    for (const auto& e : load.objects)
        if (e.created && vm.object(e.id))
            ids.push_back(e.id);
    // Restore every object first: the tree stacks name other objects.
    for (ObjectId id : ids)
        if (auto it = states.find(id); it != states.end()) {
            restoreState(vm, *vm.object(id), *it->second);
            restoreEffects(vm, *vm.object(id), *it->second);
            ++out.restored;
        }
    for (ObjectId id : ids) {
        auto it = states.find(id);
        const LotObjects::State* s = it == states.end() ? nullptr : it->second;
        std::string problem;
        if (!s || s->stack.empty()) {
            problem = "no saved tree stack";
        } else {
            Thread t;
            t.me = id;
            t.serial = ++vm.threadSerials;
            t.meSerial = vm.object(id) ? vm.object(id)->serial : 0;
            std::copy(s->temps.begin(), s->temps.end(), t.temps.begin());
            for (const auto& f : s->stack) {
                // A frame runs the code of "running on" (0 in the older layout: me).
                const ObjectId ownerId = f.runningOn ? ObjectId(f.runningOn) : id;
                const Object* owner = vm.object(ownerId);
                if (!owner) {
                    problem = "a frame runs the code of a missing object";
                    break;
                }
                auto [group, behavior] = vm.resolveTree(*owner, f.treeId);
                if (!behavior || behavior->instructions.empty()) {
                    problem = "a frame's tree is missing";
                    break;
                }
                if (f.node >= behavior->instructions.size()) {
                    problem = "a frame's node is past its tree";
                    break;
                }
                Frame fr;
                fr.behavior = behavior;
                fr.group = group;
                fr.instance = f.treeId;
                fr.codeOwner = ownerId;
                fr.pc = f.node;
                fr.stackObject = ObjectId(f.objectId);
                fr.params = f.params;
                fr.params.resize(std::max<size_t>({fr.params.size(), behavior->argCount, 4}), 0);
                fr.locals = f.locals;
                fr.locals.resize(std::max<size_t>(fr.locals.size(), behavior->localCount), 0);
                // The stack element's primitive state (an Animate Sim's
                // request ID, Generic Sims Call's family, ...).
                fr.primitiveState = f.primitiveState;
                t.stack.push_back(std::move(fr));
            }
            if (problem.empty() && t.stack.size() <= kMaxStackDepth) {
                // A person's object records (cTSPerson::cObjectRecord::Read
                // BV 0xad27fc) name frames of this stack.
                if (Object* p = vm.object(id); p && p->person) {
                    p->person->objectRecords.clear();
                    for (const auto& r : s->objectRecords)
                        p->person->objectRecords.push_back(
                            {ObjectId(r.object), int16_t(uint16_t(r.value)), r.flag != 0, t.serial});
                    p->person->recordsAtAction = s->personU646;
                    p->person->recordsAtSubAction = s->personU644;
                }
                vm.threads.insert_or_assign(id, std::move(t));
                ++out.stacks;
            } else if (problem.empty()) {
                problem = "the saved tree stack is too deep";
            }
        }
        if (!problem.empty()) {
            // The saved object records name frames that are not there: their
            // objects' data 62 loses them (as StackJustPopped would).
            if (s)
                for (const auto& r : s->objectRecords)
                    if (Object* x = vm.object(ObjectId(r.object)))
                        x->data[62] = int16_t(x->data[62] - 1);
            ++out.problems[problem];
            out.freshMains += vm.startMain(id);
        }
    }
    // OS2_DEBUG_RECORDS=1: the objects whose saved data 62 "use count" is not
    // the number of the persons' object records on them, and each record.
    if (std::getenv("OS2_DEBUG_RECORDS")) {
        std::map<ObjectId, int> uses;
        for (const Object* o : vm.objectTable())
            if (o && o->person)
                for (const auto& r : o->person->objectRecords) {
                    ++uses[r.object];
                    const auto t = vm.threads.find(o->id);
                    std::fprintf(stderr, "records: %d -> %d depth %d of %zu%s\n", o->id, r.object, r.depth,
                                 t == vm.threads.end() ? size_t(0) : t->second.stack.size(), r.flag ? " flag" : "");
                }
        for (const Object* o : vm.objectTable())
            if (o && o->data[62] != uses[o->id])
                std::fprintf(stderr, "records: object %d (%s) data 62 = %d, %d records\n", o->id,
                             o->definition ? o->definition->fileName.c_str() : "?", o->data[62], uses[o->id]);
    }
    // The Sims' saved interaction queues, now that their stacks are back.
    for (ObjectId id : ids)
        if (auto it = states.find(id); it != states.end())
            if (Object* o = vm.object(id)) {
                out.queued += restoreQueue(vm, *o, *it->second);
                if (o->person)
                    out.queued += restoreSubQueue(vm, *o, *it->second);
            }
    // The routing instructions that were walking go on: exactly, from the
    // VM's own record (thread_state.hpp), else as the game's do, planning
    // their way again (resumeRoutes).
    for (ObjectId id : ids) {
        if (!states.count(id))
            continue;
        const auto& extra = states.at(id)->vmThreadState;
        if (!extra.empty() && applyThreadState(vm, *vm.object(id), extra))
            ++out.threadStates;
        else if (auto t = vm.threads.find(id); t != vm.threads.end())
            out.routes += resumeRoutes(vm, t->second);
    }
    if (!saved.vmLotState.empty())
        applyLotState(vm, saved.vmLotState);
    // The Animate Sims that were playing go on from their requests in the
    // Sims' animation queues (formats/scene_anim.hpp), now that the queues
    // (which name the other Sim of a social animation) are back.
    vm.animationCounter = std::max(vm.animationCounter, saved.animationCounter);
    // The overlays (Animate Overlay's: the idles, the faces, the carry
    // poses, whose event trees the idle system waits on; Animate Object's
    // on objects, with their event trees).
    for (ObjectId id : ids) {
        auto it = states.find(id);
        Object* o = vm.object(id);
        if (it == states.end() || !o || it->second->animations.empty())
            continue;
        o->animation.overlays.clear();
        o->animation.overlayEvents.clear();
        for (const auto& q : it->second->animations) {
            if (q.blocking())
                continue;
            ResumedOverlay r;
            r.name = q.name;
            r.oeType = int8_t(q.oeType);
            r.level = int8_t(std::clamp<uint32_t>(q.priority, 0, 8));
            r.loops = int(q.loops);
            r.played = q.played;
            r.speed = q.speed;
            r.heading = q.heading;
            r.mirrored = q.mirrored;
            r.ikObject = int16_t(q.target);
            r.eventTree = q.eventTree;
            // Its event tree runs as the Sim whose tree started it: the VM
            // keeps it in MovedObjectID when that is not this object
            // (scene_anim.hpp; a game's save: this object, inferred).
            r.me = int16_t(id);
            if (q.movedObject && vm.object(ObjectId(q.movedObject)))
                r.me = int16_t(q.movedObject);
            r.stackObject = int16_t(q.stackObject);
            const bool animateObject = q.channel == "obj";
            if (animateObject)
                r.ikObject = 0;
            // The code whose event tree it runs: the object of the saved
            // GUID (the stack object's or mine first; inferred: the request
            // keeps the tree's object only by GUID).
            r.codeOwner = int16_t(id);
            if (const Object* so = vm.object(q.stackObject); so && so->guid() == q.objectGuid)
                r.codeOwner = so->id;
            else if (o->guid() != q.objectGuid)
                for (const auto& [oid, x] : vm.objects())
                    if (x.guid() == q.objectGuid) {
                        r.codeOwner = oid;
                        break;
                    }
            if (animateObject) {
                r.level = animlevel::Object;
                // Played until stopped (animateObject): looping when saved
                // with loops 0xFFFFFFFF (temp 1 = -1; the PlumbBob's spin).
                r.loops = q.loops == 0xFFFFFFFFu ? -1 : 0;
                const uint16_t tree = r.eventTree;
                r.eventTree = 0;
                r.treeEvents = false;
                resumeOverlay(vm, *o, r);
                if (tree)
                    resumeOverlayEvents(vm, *o, r.name, tree, r.me, r.codeOwner, r.stackObject, q.played, q.speed);
            } else {
                resumeOverlay(vm, *o, r);
            }
            ++out.overlays;
        }
    }
    for (ObjectId id : ids)
        if (auto it = states.find(id); it != states.end() && !it->second->animations.empty())
            if (auto t = vm.threads.find(id); t != vm.threads.end() && !t->second.stack.empty())
                for (const auto& q : it->second->animations)
                    if (uint32_t(t->second.stack.back().primitiveState) == q.id) {
                        ResumedAnimation r;
                        r.id = q.id;
                        r.name = q.name;
                        r.played = q.played;
                        r.speed = q.speed;
                        r.weight = q.weight;
                        r.heading = q.heading;
                        r.mirrored = q.mirrored;
                        r.level = int(q.priority);
                        r.target = int16_t(q.target);
                        out.animations += resumeAnimateSim(vm, t->second, r);
                        break;
                    }
    // cEdithObjectModule::PostLoad (BV 0x52d590) calls cTSObject::PostLoad
    // (BV 0xb0b1e0) on every loaded object, containers before what they
    // hold: RunTree(2, no stack object, four zero parameters), the "load"
    // entry, whatever the object's version (docs/primitives-mac.md §4i; the
    // editor's text, "only if its version differs", is not what the game
    // does). Its FinishEditing then tells the room (OnRoomChanged); the
    // saved room is kept (the binary recomputes it from the place, which our
    // tile rooms only approximate).
    std::vector<std::pair<int, ObjectId>> order;
    for (ObjectId id : ids) {
        const Object* o = vm.object(id);
        if (!o || !states.count(id))
            continue;
        int depth = 0;
        for (const Object* c = o; c && c->data[objdata::ContainerId] && c->data[objdata::ContainerId] != c->id && depth < 8; ++depth)
            c = vm.object(c->data[objdata::ContainerId]);
        order.push_back({depth, id});
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [depth, id] : order) {
        if (!vm.object(id))
            continue;
        constexpr std::array<int16_t, 4> kNoParams{};
        if (!vm.runLoadEntries) {
        } else if (auto t = vm.runFunction(id, kLoadFunction, 1000, kNoParams)) {
            ++out.loads;
            if (t->status == Thread::Status::Failed)
                out.failedLoads.push_back(std::move(*t));
        }
        finishMove(vm, id);
    }
    return out;
}

size_t updateSimObjects(VM& vm, bool casLot) {
    size_t made = 0;
    for (const auto& g : vm.code().globalSimObjects(1)) {
        std::vector<ObjectId> have;
        for (const auto& [id, o] : vm.objects())
            if (o.definition && o.definition->guid() == g.guid)
                have.push_back(id);
        const bool mustNotExist = casLot && g.notInCas;
        // Keep the first (lowest ID, inferred: the binary keeps the first of
        // its object list), kill the rest; all of them in a CAS lot.
        for (size_t i = mustNotExist ? 0 : 1; i < have.size(); ++i)
            if (vm.object(have[i])) {
                if (std::getenv("OS2_LOG_CREATE"))
                    std::fprintf(stderr, "load: global sim object %d (GUID %08X) deleted: %zu of its kind, %d kept\n",
                                 have[i], g.guid, have.size(), mustNotExist ? -1 : have[0]);
                deleteGroup(vm, have[i]);
            }
        if (have.empty() && !mustNotExist) {
            // MakeNewOutOfWorldObject: Init, then the Main like any made object.
            const ObjectId id = createGhost(vm, g.group, g.instance);
            vm.globalsMadeAtLoad.insert(id);
            for (ObjectId part : groupOf(vm, id))
                vm.startMain(part);
            ++made;
        }
    }
    return made;
}

void clearUseCounts(VM& vm) {
    for (Object* o : vm.objectTable())
        if (o)
            o->data[62] = 0;
}

ResumeResult startSavedLot(VM& vm, const LotLoad& load, const LotObjects& saved, bool fromInit) {
    applySavedSimulator(vm, saved);
    if (!fromInit) {
        ResumeResult r = resumeLotObjects(vm, load, saved);
        r.globalsMade = updateSimObjects(vm);
        computeLotData(vm); // the lot loaded (cTSLot, message 0x8B)
        vm.refreshObjectPortals(); // a modular stair's pieces are linked by their object data 80
        return r;
    }
    clearUseCounts(vm);
    for (const auto& e : load.objects)
        if (e.created && vm.object(e.id))
            vm.runFunction(e.id, 0);
    applySavedMotives(vm, load, saved);
    ResumeResult out;
    for (const auto& e : load.objects)
        out.freshMains += e.created && vm.startMain(e.id);
    out.globalsMade = updateSimObjects(vm);
    computeLotData(vm);
    vm.refreshObjectPortals();
    return out;
}

} // namespace os2::vm
