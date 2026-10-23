// Interaction queues, Push Interaction, Idle for Input and the pie menu
// (interactions.hpp). The queue logic follows FreeSO's
// tso.simantics/Engine/VMThread.cs (MPL-2.0), rewritten for this VM.

#include "autonomy.hpp"
#include "engine.hpp"
#include "interactions.hpp"

#include "errors.hpp"
#include "formats/binreader.hpp"
#include "lot.hpp"
#include "ownership.hpp"

#include <algorithm>
#include <cctype>
#include <functional>

namespace os2::vm {

namespace {

constexpr uint8_t kOwnerParam = 0x09;
constexpr uint8_t kOwnerLocal = 0x19;

int16_t interactionGroupOf(const Object& o) {
    return o.definition ? int16_t(o.definition->field(kObjdInteractionGroup)) : 0;
}

int16_t currentPriority(const Object& sim) {
    if (sim.person)
        return sim.person->data[persondata::Priority];
    return sim.activeInteraction >= 0 && size_t(sim.activeInteraction) < sim.queue.size()
               ? sim.queue[size_t(sim.activeInteraction)].priority
               : kPriorityIdle;
}

void setPriority(Object& sim, int16_t p) {
    if (sim.person)
        sim.person->data[persondata::Priority] = p;
}

// Drops waiting entries whose object is gone (FreeSO's
// EvaluateQueuePriorities), and tells the running interaction to stop when
// its priority was dropped to 0 (how The Sims 1 cancels an interaction from
// its own tree; kept from FreeSO). Something more important waiting behind
// it does not cancel it: the binary asks cTSPerson::ShouldInterrupt each
// time instead (shouldInterrupt below), and only a priority strictly above
// the person's interrupts. FreeSO lets another autonomous entry interrupt
// an autonomous one ("allow other auto actions to interrupt us"); the TS2
// binary does not, and with that rule every "*Push Look At" or social
// reaction another Sim pushed cancelled cooking half-way (the route to the
// stove failed "interrupted", "Food Chain - Failure - Cook" dropped the tray).
void evaluatePriorities(VM& vm, Object& sim) {
    auto& q = sim.queue;
    const int active = sim.activeInteraction;
    for (size_t i = size_t(active + 1); i < q.size();)
        if (!vm.object(q[i].object))
            q.erase(q.begin() + std::ptrdiff_t(i));
        else
            ++i;
    if (active < 0 || size_t(active) >= q.size())
        return;
    auto& running = q[size_t(active)];
    if (running.priority != kPriorityIdle && currentPriority(sim) == kPriorityIdle)
        running.cancelled = true;
}

QueuedInteraction entryFor(ObjectId object, const InteractionTableRef& ref, const InteractionTable::Interaction& e,
                           uint8_t language) {
    QueuedInteraction q;
    q.object = object;
    q.interaction = uint16_t(e.stringIndex);
    q.priority = kPriorityUserDriven;
    q.action = e.action;
    q.guard = e.guard;
    q.ttabFlags = uint32_t(e.flags) | uint32_t(e.flags2) << 16;
    if (ref.strings)
        q.name = ref.strings->get(e.stringIndex, language);
    return q;
}

// A nested interaction goes to the end of the sub-queue.
uint16_t enqueueSub(VM& vm, Object& sim, QueuedInteraction q) {
    q.uid = newInteractionId(vm, sim);
    const uint16_t uid = q.uid;
    sim.subQueue.push_back(std::move(q));
    syncQueueData(sim);
    setInteractionData(sim); // AddAction
    return uid;
}

// What cEdithObjectTestSim::TestInteraction (Mac BV 0x4c6214) tests before
// the check tree, for the Sim the entry is for: its object exists, the
// entry is not a debug one (TTAB flag 0x80, cTSTreeTableEntry vtable 0x9c:
// refused out of the game's debug mode, always in a push), and it is for
// the Sim's age and, for a visitor, has "allow visitors" (availableToAge:
// the person's age bits, vtable 0xd0, against the entry's, 0x48; IsVisitor
// and TestFlags(1)); the object is not burning (vtable 0x214, IsBurning:
// object data 8 bit 0x200) and its interactions are not disabled (0x2e8,
// AreInteractionsDisabled(autonomous) BV 0xb01634: when autonomous a
// "lockout count" (data 25) of 1 or more, else data 87 of 1 or more or data
// 82 bit 0x04); with global 35 "Parental Control" on, no entry with the
// flag 0x20000 (flags2 bit 1, vtable 0x84; GetGlobal(35) of the simulator).
bool entryAvailable(VM& vm, const Object& sim, const QueuedInteraction& q, bool autonomous) {
    const Object* o = vm.object(q.object);
    if (!o)
        return false;
    if (q.ttabFlags & 0x80)
        return false;
    if (o->data[objdata::Flags] & 0x200)
        return false;
    if ((autonomous && o->data[25] >= 1) || o->data[87] >= 1 || (o->data[82] & 0x04))
        return false;
    if (vm.globals.size() > 35 && vm.globals[35] != 0 && (q.ttabFlags & 0x20000))
        return false;
    if (const auto ref = interactionTable(vm, *o))
        for (const auto& e : ref->table->interactions)
            if (e.stringIndex == q.interaction)
                return availableToAge(sim, *ref->table, e);
    return true;
}

// TestInteraction with the check tree forced, as Idle for Input, its
// sub-queue mode and AddAction run it: the tests above, then the check tree
// with parameter 0 = `autonomous`.
bool testInteraction(VM& vm, ObjectId simId, const QueuedInteraction& q, bool autonomous) {
    const Object* sim = vm.object(simId);
    if (!sim || !entryAvailable(vm, *sim, q, autonomous))
        return false;
    const CheckResult r = checkInteraction(vm, simId, q, nullptr, autonomous);
    return r == CheckResult::NoGuard || r == CheckResult::True;
}

// A "Run Immediately" interaction (TTAB flag 0x04: "*Push Look At" of
// "Controller - Look At", pushed onto every Sim near a social each few
// seconds) as cTSPerson::AddAction (Mac BV 0xadbbdc) adds it: it never goes
// into the queue (the four "*Push Look At" piled up behind Daniel's dance in
// N001_Lot68, shown in the queue, live QA 15). Its check tree runs
// (cEdithObjectTestSim::TestInteraction); when it passes and no other
// immediate interaction is running (a global guard, DAT_02cc0210), the
// action tree runs to its end at once as a one-tick tree
// (cTSObject::RunOneTickTree) for the Sim, the interaction's object as
// stack object and code owner, its parameters, with the Sim's person data
// 33 (priority) and 166 (autonomous) set to the interaction's for that
// time and put back after. False (Push Interaction's result 6, "the target
// refused") when the check fails or one is already running. Person data 166
// is the interaction's autonomous bit (cTSInteraction vtable 0xbc), which
// AddAction also lends the current action for the time of the run (0xc0).
// Inferred: the tree's temps are fresh, and an error in it is a refusal
// rather than an error of the pushing tree.
bool runImmediately(VM& vm, Object& sim, const QueuedInteraction& q) {
    static bool running = false;
    if (running || !sim.person || !vm.object(q.object))
        return false;
    if (!testInteraction(vm, sim.id, q, false))
        return false;
    if (!q.action)
        return true;
    running = true;
    auto& d = sim.person->data;
    const int16_t priority = d[persondata::Priority], autonomous = d[persondata::InteractionAutonomous];
    // The current action's autonomous bit, lent (by its ID: the run may move the queue).
    auto current = [](Object& s) -> QueuedInteraction* {
        return s.activeInteraction >= 0 && size_t(s.activeInteraction) < s.queue.size() ? &s.queue[size_t(s.activeInteraction)]
                                                                                        : nullptr;
    };
    const uint16_t currentUid = current(sim) ? current(sim)->uid : 0;
    const bool currentAutonomous = current(sim) && current(sim)->autonomous;
    if (QueuedInteraction* a = current(sim))
        a->autonomous = q.autonomous;
    d[persondata::Priority] = q.priority;
    d[persondata::InteractionAutonomous] = q.autonomous;
    const ObjectId id = sim.id;
    std::array<int16_t, kTempCount> temps{};
    try {
        runOneTickTree(vm, temps, id, q.object, q.object, q.action, q.args);
    } catch (const VMError&) {
    }
    running = false;
    if (Object* s = vm.object(id); s && s->person) {
        s->person->data[persondata::Priority] = priority;
        s->person->data[persondata::InteractionAutonomous] = autonomous;
        if (currentUid)
            for (auto& x : s->queue)
                if (x.uid == currentUid)
                    x.autonomous = currentAutonomous;
    }
    return true;
}

// Push Interaction (0x0D), as the Mac binary has it (TryPushAction,
// PushActionParam; docs/primitives-mac.md §4): interaction (TTA index) u8
// at 0, where the object is u8 at 1 (a parameter, or a local with flag
// 0x02), priority u8 at 2 (STR# 0xE0: 0 inherited = my person data 33, 1 for
// an object that is not a Sim; 1 maximum = 100; 2 autonomous = 20; 3 user
// driven = 50; others: error 0x2D), flags u8 at 3 (0x01 icon of the object
// in the local of byte 4, 0x02 object in a local, 0x04 "continue as
// current": at the head of the queue, 0x08 "use name", 0x10 the interaction
// number is in (owner u8 at 5, data u16), 0x20 "force run check tree",
// 0x40 link to the interaction whose ID is in (owner u8 at 8, data u16),
// 0x80 the new interaction's ID goes to (owner u8 at 11, data u16)), flags
// u8 at 14 (0x01 pass the first 4 parameters, 0x02 the interaction is the
// last Find Best Interaction's: the object in the target's person data 53
// and the entry in that object's data 20, 0x04/0x08 icon from the temps),
// icon index u8 at 15. The entry goes onto the stack object's queue, which
// must exist (error 0x15) and be a Sim (0x1C). The object must exist
// (0x17), have a TTAB (0x1F) and the entry (0x16); a parameter or local out
// of range is 0x08/0x33. My object data 52 "primitive result" is set to 0,
// or to why the push failed (false): 1 inherited priority but I have none,
// 2 the forced check tree said no, 3 the target's queue is full (more than 6
// entries). Not done: icons and names (no UI here), the link of 0x40 (kept
// in the entry's operand), nested pushes (sub-queue 4, error 0x6F) and the
// part of my running interaction a Sim's push inherits (inferred: its
// owner/group).
Exit pushInteraction(Context& c) {
    const auto& o = c.instruction.operands;
    const uint8_t flags = o[3], flags2 = o[14];
    Object& me = c.me();
    me.data[kObjdataPrimitiveResult] = 0;
    auto failed = [&](int16_t why) {
        if (Object* m = c.vm.object(c.thread.me))
            m->data[kObjdataPrimitiveResult] = why;
        return Exit::False;
    };
    if (!c.vm.object(c.frame.stackObject))
        throw gameError(err::NoStackObject, "Push Interaction");
    const ObjectId targetId = c.frame.stackObject;
    if (!c.vm.object(targetId)->person)
        throw gameError(err::NotAPerson, "Push Interaction onto object " + std::to_string(targetId));

    ObjectId source = 0;
    uint16_t number = flags & 0x10 ? uint16_t(c.get(o[5], int16_t(le16(&o[6])))) : o[0];
    if (flags2 & 0x02) {
        source = c.vm.object(targetId)->person->data[kPersondataFindBestObject];
        const Object* x = c.vm.object(source);
        if (!x)
            throw gameError(err::NoObject, "object " + std::to_string(source));
        // The entry is the TARGET's object data 20, where its Find Best
        // Interaction left it (TryPushAction BV 0xe33530 reads it from the
        // stack object, not from the found object): reading the object's
        // made every "Desespero" push fail (error 0x16) when the found object
        // was another Sim, and its Main died (N001_Lot36 Beau).
        number = uint16_t(c.vm.object(targetId)->data[kObjdataTreeTableEntry]);
    } else if (flags & 0x02) {
        if (o[1] >= c.frame.locals.size())
            throw gameError(err::LocalOutOfRange, "local " + std::to_string(o[1]));
        source = c.get(kOwnerLocal, o[1]);
    } else {
        if (o[1] >= c.frame.params.size())
            throw gameError(err::StackNumberOutOfRange, "parameter " + std::to_string(o[1]));
        source = c.get(kOwnerParam, o[1]);
    }
    const Object* x = c.vm.object(source);
    if (!x)
        throw gameError(err::NoObject, "object " + std::to_string(source));
    if (!x->definition)
        return Exit::False;
    if (!interactionTable(c.vm, *x))
        throw gameError(err::NoTreeTable, "object " + std::to_string(source));
    auto q = makeInteraction(c.vm, source, number);
    if (!q)
        throw gameError(err::NoTreeTableEntry, "interaction " + std::to_string(number) + " of object " +
                                                   std::to_string(source));
    switch (o[2]) {
    case 0: { // inherited: my priority
        const int16_t mine = me.person ? me.person->data[persondata::Priority] : int16_t(1);
        if (mine == 0)
            return failed(1);
        q->priority = mine;
        break;
    }
    case 1: q->priority = kPriorityMaximum; break;
    case 2: q->priority = kPriorityAutonomous; break;
    case 3: q->priority = kPriorityUserDriven; break;
    default: throw gameError(err::BadPriority, "Push Interaction priority " + std::to_string(o[2]));
    }
    if (flags2 & 0x01)
        for (size_t i = 0; i < 4 && i < c.frame.params.size(); ++i)
            q->args[i] = c.frame.params[i];
    // With 0x02 the entry also takes my last Find Best Interaction's
    // parameters (TryPushAction: this object's cITSPerson vtable 0x24c into
    // the interaction's 0x6c, after the 0x01 ones; the icon from 0x248):
    // a dynamic entry's, as "Interaction - Give Toddler a Bath - TEST" makes
    // with the toddler in parameter 0. Without them the "Desespero de
    // Higiene" push of the bath had no toddler and the Main died in "Pickup
    // - Do Carry Sim" (N001_Lot36 Brandi). Inferred: the name goes along.
    if (flags2 & 0x02)
        if (const Object* m = c.vm.object(c.thread.me); m && m->person) {
            q->args = m->person->foundActionArgs;
            if (!m->person->foundActionName.empty())
                q->name = m->person->foundActionName;
        }
    // The interaction's autonomous bits (cTSInteraction +0x54; TryPushAction
    // BV 0xe33530): with 0x02 both, "autonomous" (0x400, vtable 0xc0: person
    // data 166 when it runs) and "test as autonomous" (0x800, 0xc8: the check
    // tree's parameter 0 when Idle for Input takes it). Without it a Sim
    // that pushes passes on its current interaction's 0x400 (0xbc -> 0xc0);
    // an object's push leaves both 0 (Init zeroes them).
    if (flags2 & 0x02) {
        q->autonomous = q->testAutonomous = true;
    } else if (const Object* m = c.vm.object(c.thread.me); m && m->person) {
        if (m->activeInteraction >= 0 && size_t(m->activeInteraction) < m->queue.size())
            q->autonomous = m->queue[size_t(m->activeInteraction)].autonomous;
    }
    // A nested interaction (TTAB flag 0x8000) needs the target's current
    // interaction to allow nesting (TTAB flag 0x4000), else error 0x6F, and
    // goes to the sub-queue, which takes 4 (more: result 4)
    // (cTSObject::TryPushAction, docs/primitives-mac.md §4b).
    // The limits are only for pushes that are not "continue as current"
    // (byte 3 bit 0x04): TryPushAction (BV 0xe33530) sets the
    // interaction's continuation from that bit (cTSInteraction vtable 0x88,
    // SetContinuation) and tests the queue (result 3) and sub-queue (4)
    // sizes only when it reads back false (0x84). "Pickup - Do Carry Sim"
    // pushes the carry onto the carrier and the carried Sim that way: with
    // the limit the toddler's full queue (a crib Sim with queued Look Ats)
    // made "Interaction - Give Toddler a Bath" loop without a yield, a carry
    // object a pass (N001_Lot36, budget hits).
    const bool continuation = flags & 0x04;
    const bool nested = q->ttabFlags & 0x8000;
    if (nested) {
        const Object* t = c.vm.object(targetId);
        const bool running = t->activeInteraction >= 0 && size_t(t->activeInteraction) < t->queue.size();
        if (!running || !(t->queue[size_t(t->activeInteraction)].ttabFlags & 0x4000))
            throw gameError(err::NestedPush, "Push Interaction");
    }
    // Then TryPushAction always tests the interaction for the target
    // (cEdithObjectTestSim::TestInteraction, BV 0x4c6214, "autonomous" =
    // byte 14 bit 0x02): the entry's age and visitor flags and the debug
    // flag (entryAvailable), and the check tree only with "force run check
    // tree" (byte 3 0x20). Refused: result 2.
    if (const Object* t = c.vm.object(targetId); !t || !entryAvailable(c.vm, *t, *q, flags2 & 0x02))
        return failed(2);
    if (flags & 0x20) {
        const CheckResult r = checkInteraction(c.vm, targetId, *q, nullptr, flags2 & 0x02);
        if (r != CheckResult::NoGuard && r != CheckResult::True)
            return failed(2);
    }
    Object* target = c.vm.object(targetId);
    if (!target)
        throw gameError(err::NoStackObject, "Push Interaction");
    if (nested && !continuation && target->subQueue.size() > kMaxSubQueue)
        return failed(4);
    // More than 6 waiting (CountActions, vtable 0x1f4: the queue without the
    // current action) and none of a lower priority to give way
    // (AnyLowerPriorityInteractions, vtable 0x1FC, BV 0xad65b0; AddAction
    // then skips those): result 3 (TryPushAction BV 0xe33de0). Was: the
    // queue with the running ones counted, and no giving way.
    constexpr size_t kMaxQueue = 6;
    const size_t running = size_t(std::max(target->activeInteraction + 1, 0));
    const size_t waiting = target->queue.size() > running ? target->queue.size() - running : 0;
    const bool lower = std::any_of(target->queue.begin() + std::ptrdiff_t(std::min(running, target->queue.size())),
                                   target->queue.end(),
                                   [&](const QueuedInteraction& x) { return x.priority < q->priority; });
    if (!continuation && !nested && waiting > kMaxQueue && !lower)
        return failed(3);
    // Flag 0x01: the object in the local of byte 4 is the interaction's
    // target (and icon; TryPushAction sets it with cTSInteraction's
    // SetTargetObject); a local out of range is error 0x33.
    if (flags & 0x01) {
        if (o[4] >= c.frame.locals.size())
            throw gameError(err::LocalOutOfRange, "local " + std::to_string(o[4]));
        q->target = c.get(kOwnerLocal, o[4]);
    }
    q->flags = flags;
    q->noContinuation = flags2 & 0x10; // +0x54 bit 0x4000 (vtable 0x90)
    q->pushedBy = c.thread.me;
    q->operands = o;
    static const bool logPush = std::getenv("OS2_LOG_PUSH") != nullptr; // test aid: who pushes what
    if (logPush)
        std::fprintf(stderr, "t%llu push \"%s\" (TTA %u of %d) onto %d by %d in \"%s\" prio %d flags %02X\n",
                     (unsigned long long)c.vm.ticks, q->name.c_str(), q->interaction, q->object, targetId, c.thread.me,
                     c.frame.behavior->fileName.c_str(), q->priority, flags);
    // A "Run Immediately" entry (TTAB flag 0x04) never waits in the queue:
    // cTSPerson::AddAction (Mac BV 0xadbbdc) runs it at once (runImmediately),
    // nested or not (AddAction tests the flag before it picks the queue or
    // the sub-queue).
    if (q->ttabFlags & 0x04) {
        if (!runImmediately(c.vm, *target, *q))
            return failed(6);
        if (flags & 0x80)
            c.set(o[11], int16_t(le16(&o[12])), int16_t(newInteractionId(c.vm, *target)));
        return Exit::True;
    }
    const uint16_t uid = nested ? enqueueSub(c.vm, *target, std::move(*q)) : enqueue(c.vm, *target, std::move(*q), flags & 0x04);
    if (flags & 0x40) // link to the interaction whose ID is in (owner u8 at 8, data u16)
        if (const auto linked = uint16_t(c.get(o[8], int16_t(le16(&o[9])))))
            if (Object* t = c.vm.object(targetId)) {
                auto find = [&](std::vector<QueuedInteraction>& v) -> QueuedInteraction* {
                    for (auto& x : v)
                        if (x.uid == uid)
                            return &x;
                    return nullptr;
                };
                if (QueuedInteraction* made = nested ? find(t->subQueue) : find(t->queue))
                    linkInteractions(c.vm, *made, linked);
            }
    if (flags & 0x80)
        c.set(o[11], int16_t(le16(&o[12])), int16_t(uid));
    return Exit::True;
}

// Idle for Input in "Handle Sub Queue Interactions" mode (byte 4 bit 0;
// cTSPerson::TryIdleForInput, Mac BV): with nothing in the sub-queue, true.
// Else the first entry leaves it and its check tree runs; when it passes
// and the entry has an action tree, that becomes the current sub-action and
// its tree is pushed on top of this one (the primitive's 3); unlike the
// queue's, this tree's result is what this instruction branches on (the
// binary does not ask for the re-run here). A failed check clears the whole
// sub-queue and the result is true.
Exit handleSubQueue(Context& c) {
    Object& me = c.me();
    if (me.subQueue.empty())
        return Exit::True;
    const QueuedInteraction item = me.subQueue.front();
    me.subQueue.erase(me.subQueue.begin());
    const bool exists = c.vm.object(item.object) != nullptr;
    bool ok = exists && item.action != 0;
    if (ok)
        ok = testInteraction(c.vm, c.thread.me, item, false);
    Object* sim = c.vm.object(c.thread.me);
    if (!sim)
        return Exit::True;
    // SetCurrentSubAction comes before the gosub in the binary: +0x644 is
    // the number of object records without the sub-action's own.
    const uint16_t records = sim->person ? uint16_t(sim->person->objectRecords.size()) : 0;
    if (ok) {
        try {
            c.vm.call(c.thread, item.object, item.action, item.args);
        } catch (const VMError&) {
            ok = false;
        }
    }
    if (!ok) {
        // ClearSubActions, and ActionSkipped for the refused one when its
        // object exists (its links undone; TryIdleForInput's sub-queue mode).
        const ObjectId id = sim->id;
        clearSubActions(c.vm, *sim);
        if (exists)
            actionSkipped(c.vm, id, item);
        if (Object* s = c.vm.object(id))
            syncQueueData(*s);
        return Exit::True;
    }
    Frame& f = c.thread.stack.back();
    f.stackObject = item.object;
    f.subInteraction = true;
    // The sub-action it replaces has its links undone (vtable 0x10c with
    // no person, before SetCurrentSubAction).
    if (sim->subAction) {
        const QueuedInteraction before = *sim->subAction;
        sim->subAction.reset();
        unlinkInteraction(c.vm, 0, before);
        sim = c.vm.object(c.thread.me);
        if (!sim)
            return Exit::Call;
    }
    sim->subAction = item;
    if (sim->person)
        sim->person->recordsAtSubAction = records;
    // SetCurrentSubAction (base 1.0 0xb849ec): with the sub-action's object,
    // person data 39 "route status" := 0 (0xb84ac0); then, as
    // SetCurrentAction, 73 "footprint extension", 71 "non-interruptible" and
    // 110 "Interactions Linked Deleted" := 0 (docs/primitives-mac.md §4j).
    if (sim->person)
        for (size_t k : {size_t(persondata::RouteStatus), size_t(73), size_t(persondata::NonInterruptible), size_t(110)})
            sim->person->data[k] = 0;
    updateObjectOwnership(c.vm, c.thread.me, item.object);
    syncQueueData(*sim);
    // ... and 109 := the sub-action's GUID (vtable 0xec in base 1.0, 0x110
    // in the BV: GetGUID), which stays when it ends (was the action's).
    if (Object* s = c.vm.object(c.thread.me); s && s->person)
        s->person->data[109] = int16_t(item.uid);
    return Exit::Call;
}

// Idle for Input (0x11), as the Mac binary has it (cTSPerson::
// TryIdleForInput; docs/primitives-mac.md §4): the parameter (u16 at 0)
// holding the ticks left (out of range: error 0x08), "allow push" u16 at 2
// (non-zero), byte 4 bit 0 "Handle Sub Queue Interactions". With nothing to
// do (ShouldInterrupt: an interaction waiting in the queue, the running one
// cancelled, or a Notify the Stack Object out of Idle) it counts the
// parameter down and yields while it is above 0 and, at 0, clears the
// object flag "notified by idle for input" and returns true: it waits
// exactly N ticks. With something to do it sets that flag; with "allow push"
// outside an interaction the next queued interaction that may run (its
// priority beats the running one and its check tree passes: attemptPush)
// is started, and inside one the front entry when its priority is higher
// (it nests over the running one): its action tree is pushed on top of this tree and the counter
// goes to 0; when it returns, this instruction runs again at once (see
// below). An entry taken and refused (TestInteraction) is skipped
// (ActionSkipped) and the instruction yields. Otherwise true. In the
// sub-queue mode the binary runs the queued sub-actions (handleSubQueue).
Exit idleForInput(Context& c) {
    Thread& t = c.thread;
    // The interaction this instruction ran has returned: the binary runs the
    // instruction again at once (the tree stack element's "re-run" flag, set
    // when it started the action, keeps DoNodeAction from branching) and, in
    // that re-run, only starts the next entry when it was pushed "continue as
    // current" (Push Interaction byte 3 bit 0x04, cTSInteraction vtable
    // 0x84: +0x54 bit 0x02) and the finished one (the person's +0x5d8) does
    // not say "no continuation" (vtable 0x8c: +0x54 bit 0x4000, which Push
    // Interaction's byte 14 bit 0x10 sets through 0x90; no tree of the base
    // game sets it; this was "not cancelled", inferred); else it returns
    // true and Main runs its "Post Interaction" (cTSPerson::TryIdleForInput,
    // BV 0xade664). "Sub - Push Eat" pushes Eat that way, so Main's "Kill -
    // Objects in Hands" does not take the snack.
    if (t.interactionReturned) {
        const bool noContinuation = t.interactionReturned == 2;
        t.interactionReturned = 0;
        const Object& me = c.me();
        const size_t next = size_t(me.activeInteraction + 1);
        if (noContinuation || next >= me.queue.size() || !(me.queue[next].flags & 0x04))
            return Exit::True;
    }
    const auto& o = c.instruction.operands;
    if (o[4] & 0x01)
        return handleSubQueue(c);
    const auto param = int16_t(le16(&o[0]));
    if (param < 0 || size_t(param) >= c.frame.params.size())
        throw gameError(err::StackNumberOutOfRange, "Idle for Input parameter " + std::to_string(param));
    const bool allowPush = le16(&o[2]) != 0;
    Object& me = c.me();
    const bool inAction = inInteraction(t);
    const size_t next = size_t(me.activeInteraction + 1);
    const bool waiting = next < me.queue.size();
    // cTSPerson::ShouldInterrupt (BV 0xad6388): person data 71
    // "non-interruptible" says no; the current action or sub-action
    // cancelled says yes; else only a first waiting entry whose priority is
    // above the person's (data 33). A waiting entry of no higher priority
    // left Mains spinning here without yielding (the social bunny's
    // "Function - Main - Check Queue" ran out of its instruction budget).
    const bool pending = me.idleInterrupt || (me.person ? shouldInterrupt(me) : inAction ? interactionCancelled(me) : waiting);
    const int16_t ticks = c.get(kOwnerParam, param);
    if (!pending) {
        if (ticks > 0) {
            c.set(kOwnerParam, param, int16_t(ticks - 1));
            return Exit::Yield;
        }
        me.data[objdata::Flags] = int16_t(me.data[objdata::Flags] & ~kFlagNotifiedByIdleForInput);
        return Exit::True;
    }
    me.data[objdata::Flags] = int16_t(me.data[objdata::Flags] | kFlagNotifiedByIdleForInput);
    me.idleInterrupt = false;
    // Inside an interaction the front entry only runs when it may interrupt
    // the running one (a higher priority: cTSPerson::TryIdleForInput, "the
    // frame says 1"); it runs on top of it as the current action, and the
    // interrupted one goes on when it returns (endInteraction steps back to
    // it). The social worker's "Entrar" (100) reaches the child waiting in
    // "Chorar por Filho Perdido" (50) this way.
    const bool mayInterrupt = !inAction || (me.person && !interactionCancelled(me) && shouldInterrupt(me));
    if (allowPush && waiting && mayInterrupt) {
        c.set(kOwnerParam, param, 0); // before the push, which may move this frame
        const size_t depth = t.stack.size();
        const PushOutcome took = attemptPush(c.vm, t, inAction);
        if (took == PushOutcome::Started)
            return Exit::Call;
        if (t.stack.size() == depth) // nothing could run: the counter is left as it was
            t.stack.back().params[size_t(param)] = ticks;
        // An entry was taken and refused: the binary returns 2 and comes
        // back next tick (the next entry waits for it).
        if (took == PushOutcome::Skipped)
            return Exit::Yield;
    }
    return Exit::True;
}

// Notify the Stack Object out of Idle (0x31): its next Idle for Input
// returns at once (FreeSO's VMNotifyOutOfIdle; the operand is not used).
Exit notifyOutOfIdle(Context& c) {
    if (Object* o = c.vm.object(c.frame.stackObject))
        o->idleInterrupt = true;
    return Exit::True;
}

// A name for the $-tokens of action strings: a Sim's first name (from the
// OBJD file name "N001_User00011 - Don"), else the object's OBJD title.
std::string displayName(const Object* o) {
    if (!o || !o->definition)
        return "?";
    const std::string& n = o->definition->fileName;
    if (o->person)
        if (auto dash = n.find(" - "); dash != std::string::npos)
            return n.substr(dash + 3);
    return o->definition->title.empty() ? n : o->definition->title;
}

// The $-tokens of action strings that can be filled in here: $Object and $Me
// (stack object and me), $NameLocal:N / $FullNameLocal:N (the object in
// local N), $Local:N / $$Local:N (the value of local N; "$$" is money in the
// game's strings). Others stay as they are (inferred from STR# 0x12E).
std::string expandActionString(Context& c, std::string s) {
    auto replaceAll = [&](const std::string& token, const std::function<std::string(int)>& value, bool indexed) {
        for (size_t at = 0; (at = s.find(token, at)) != std::string::npos;) {
            size_t end = at + token.size();
            int n = 0;
            if (indexed) {
                if (end >= s.size() || s[end] != ':' || end + 1 >= s.size() || !std::isdigit(static_cast<unsigned char>(s[end + 1]))) {
                    at = end;
                    continue;
                }
                ++end;
                while (end < s.size() && std::isdigit(static_cast<unsigned char>(s[end])))
                    n = n * 10 + (s[end++] - '0');
            }
            std::string v;
            try {
                v = value(n);
            } catch (const VMError&) {
                at = end;
                continue;
            }
            s.replace(at, end - at, v);
            at += v.size();
        }
    };
    auto local = [&](int n) { return c.get(kOwnerLocal, int16_t(n)); };
    replaceAll("$FullNameLocal", [&](int n) { return displayName(c.vm.object(local(n))); }, true);
    replaceAll("$NameLocal", [&](int n) { return displayName(c.vm.object(local(n))); }, true);
    replaceAll("$$Local", [&](int n) { return "§" + std::to_string(local(n)); }, true);
    replaceAll("$Local", [&](int n) { return std::to_string(local(n)); }, true);
    replaceAll("$Object", [&](int) { return displayName(c.vm.object(c.frame.stackObject)); }, false);
    replaceAll("$Me", [&](int) { return displayName(c.vm.object(c.thread.me)); }, false);
    return s;
}

// Add/Change the Action String (0x32), as the Mac base-game binary has it
// (cTSObject::TryMakeActionString, base 1.0): STR# u16 at 0 (0 = 0x12E, the
// "MakeActionString prim string set" every group with action strings has),
// flags u16 at 2: 0x04 the global table, 0x08 the semi-global one, neither:
// the private one, then the semi-global, then the global one (the first
// that has the string); 0x10 the string number is temp 0; string number u8
// at 4 (1-based). In a check tree run for the pie menu or autonomy it adds
// an entry with parameters {stack object, temp 0} (sMakeNewMenuItem); in an
// interaction it renames it.
//
// Mode s8 at 9 (docs/primitives-mac.md §5): not 0 sets the entry's icon
// (an object's thumbnail: GUID u32 at 5, index byte 10) instead of adding
// or renaming one: "Interaction - Watch Clouds/Stars - TEST" of the go-here
// destination sets the cloud icon and then adds "Ver Nuvens" (string 2);
// read as text, the icon operand added a second entry "Nadar Aqui" (string
// 4). The pie menu shows no icons: nothing is kept.
Exit changeActionString(Context& c) {
    const auto& o = c.instruction.operands;
    if (int8_t(o[9]) != 0)
        return Exit::True;
    uint16_t table = le16(&o[0]);
    if (table == 0)
        table = 0x12E;
    const uint16_t flags = le16(&o[2]);
    const Object* owner = c.vm.object(c.frame.codeOwner);
    if (!owner)
        return Exit::True;
    // The string number: the base game (node version 0) reads the u8 at 4;
    // the node version 1 (Bon Voyage) the u16 at 14 (docs/primitives-mac.md
    // §5, "texto u16 14").
    const int index = flags & 0x10 ? c.thread.temps[0]
                                    : (c.instruction.nodeVersion ? int(le16(&o[14])) : int(o[4]));
    const StringTable* strings = nullptr;
    auto tryGroup = [&](uint32_t group) {
        if (strings || !group)
            return;
        const StringTable* t = c.vm.code().strings(group, table);
        if (t && index >= 1 && !t->get(size_t(index - 1)).empty())
            strings = t;
    };
    // The semi-global table is the running code's (a tree of a semi-global
    // run for an object of another library, as the food semi-global's "CT -
    // String - Clean Up" for a dish: the dish's semi-global named the
    // food's Clean Up "Ask For Lessons From ..."), as Run Tree by Name finds
    // its table (inferred for this primitive).
    const uint32_t semi = c.frame.group != owner->group && c.frame.group != kGlobalGroup ? c.frame.group : owner->semiGroup;
    if (flags & 0x04)
        tryGroup(kGlobalGroup);
    else if (flags & 0x08)
        tryGroup(semi);
    else {
        tryGroup(owner->group);
        tryGroup(semi);
        tryGroup(kGlobalGroup);
    }
    if (!strings || index < 1)
        throw VMError("Add/Change the Action String: no STR# " + hexString(table) + " string " + std::to_string(index));
    const std::string name = expandActionString(c, strings->get(size_t(index - 1)));
    if (c.thread.actionStrings) {
        ActionString a{name, c.frame.stackObject};
        a.params = {c.frame.stackObject, c.thread.temps[0], 0, 0};
        a.adMin = c.thread.adMin;
        a.adRange = c.thread.adRange;
        a.adPersonality = c.thread.adPersonality;
        c.thread.actionStrings->push_back(std::move(a));
    } else if (inInteraction(c.thread)) {
        Object& me = c.me();
        if (me.activeInteraction >= 0 && size_t(me.activeInteraction) < me.queue.size())
            me.queue[size_t(me.activeInteraction)].name = name;
    }
    return Exit::True;
}

} // namespace

void actionSkipped(VM& vm, ObjectId person, const QueuedInteraction& q) {
    const bool exists = vm.object(q.object) != nullptr;
    unlinkInteraction(vm, person, q);
    if (exists && vm.object(q.object) && vm.object(person))
        try {
            vm.runFunction(q.object, 4, 1000, {}, person); // OBJf "queue skipped"
        } catch (const VMError&) {
        }
}

std::optional<InteractionTableRef> interactionTable(VM& vm, const Object& o) {
    if (!o.definition)
        return std::nullopt;
    uint16_t tt = o.definition->field(kObjdTreeTableId);
    // A tree table ID of -1 is the master's, for every part
    // (cTSObjectSelector::GetEffectiveTreeTableID, BV 0x51d0e4, through
    // GetMasterSelector; cTSObject::GetTreeTab 0xb017b4 -> the selector's
    // GetTreeTable 0x51edf0): the vehicles' 0,5 part, the crib's parts, and
    // the changing table's 0,0 part, whose lead "Change - Common -
    // Subqueue" pushes the baby's entry 4 from ("Tree table not found
    // (object 204)" on N002_Lot4 when only interaction-group leaders and 0,0
    // parts without groups had it). Autonomy still takes each object once,
    // through its interaction leader (CanChooseAutonomously 0xafea58).
    if (tt == 0xFFFF)
        if (const uint16_t m = o.definition->masterId())
            if (auto mi = vm.code().masterDefinition(o.group, m))
                if (const ObjectDefinition* md = vm.code().definition(o.group, *mi))
                    tt = md->field(kObjdTreeTableId);
    if (tt == 0 || tt == 0xFFFF)
        return std::nullopt;
    for (uint32_t g : {o.group, o.semiGroup})
        if (g)
            if (const InteractionTable* t = vm.code().interactions(g, tt))
                return InteractionTableRef{g, tt, t, vm.code().interactionStrings(g, tt)};
    return std::nullopt;
}

ObjectId interactionLeader(VM& vm, ObjectId id) {
    const Object* o = vm.object(id);
    if (!o)
        return id;
    const int16_t g = interactionGroupOf(*o);
    if (g < 1)
        return id;
    const ObjectId lead = multiTileGroup(*o);
    for (const auto& [oid, x] : vm.eachObject())
        if (multiTileGroup(x) == lead && interactionGroupOf(x) == -g)
            return oid;
    // The lots' saved multi-tile parts do not say which object they belong
    // to (each leads itself), so fall back to the nearest leader of the same
    // object (OBJD group) within two tiles on the same level (inferred).
    ObjectId best = id;
    float bestD = 2.0f * 2.0f + 0.01f;
    for (const auto& [oid, x] : vm.eachObject()) {
        if (x.group != o->group || interactionGroupOf(x) != -g || x.position.level != o->position.level)
            continue;
        const float dx = x.position.x - o->position.x, dy = x.position.y - o->position.y;
        if (dx * dx + dy * dy < bestD)
            bestD = dx * dx + dy * dy, best = oid;
    }
    return best;
}

std::optional<QueuedInteraction> makeInteraction(VM& vm, ObjectId object, uint16_t interaction, uint8_t language) {
    const Object* o = vm.object(object);
    if (!o)
        return std::nullopt;
    const auto ref = interactionTable(vm, *o);
    if (!ref)
        return std::nullopt;
    for (const auto& e : ref->table->interactions)
        if (e.stringIndex == interaction)
            return entryFor(object, *ref, e, language);
    return std::nullopt;
}

uint16_t addAction(VM& vm, Object& sim, QueuedInteraction q) {
    if (q.ttabFlags & 0x04) {
        runImmediately(vm, sim, q);
        return 0;
    }
    return enqueue(vm, sim, std::move(q));
}

uint16_t enqueue(VM& vm, Object& sim, QueuedInteraction q, bool pushHead) {
    // A nested interaction picked while the running one allows nesting goes
    // to the sub-queue (as Push Interaction puts it; inferred for the menu).
    if ((q.ttabFlags & 0x8000) && sim.activeInteraction >= 0 && size_t(sim.activeInteraction) < sim.queue.size() &&
        (sim.queue[size_t(sim.activeInteraction)].ttabFlags & 0x4000) && sim.subQueue.size() <= kMaxSubQueue)
        return enqueueSub(vm, sim, std::move(q));
    auto& queue = sim.queue;
    if (queue.size() >= kMaxQueuedInteractions)
        throw VMError("interaction queue of object " + std::to_string(sim.id) + " is full");
    q.uid = newInteractionId(vm, sim);
    const uint16_t uid = q.uid;
    const size_t first = size_t(sim.activeInteraction + 1);
    size_t at = first;
    if (queue.empty()) {
        at = 0;
    } else if (!pushHead) {
        // cTSPerson::AddAction (Mac BV 0xadbbdc): from the first waiting entry
        // of a lower priority on, the waiting entries are skipped (Action-
        // Skipped: they leave the queue, their links undone) and the new one
        // goes last; none lower: it goes last. (A fire's panic clears what
        // the Sims had queued; a user pick drops the autonomous ones waiting.)
        size_t cut = queue.size();
        for (size_t i = first; i < queue.size(); ++i)
            if (queue[i].priority < q.priority) {
                cut = i;
                break;
            }
        std::vector<QueuedInteraction> skipped;
        while (queue.size() > cut) {
            skipped.push_back(queue.back());
            queue.pop_back();
        }
        const ObjectId simId = sim.id;
        for (const auto& gone : skipped)
            actionSkipped(vm, simId, gone);
        at = queue.size();
    }
    queue.insert(queue.begin() + std::ptrdiff_t(std::min(at, queue.size())), std::move(q));
    evaluatePriorities(vm, sim);
    syncQueueData(sim);
    setInteractionData(sim); // AddAction
    return uid;
}

bool cancelInteraction(VM& vm, Object& sim, uint16_t uid) {
    auto& q = sim.queue;
    auto it = std::find_if(q.begin(), q.end(), [&](const QueuedInteraction& x) { return x.uid == uid; });
    if (it == q.end()) {
        // A sub-action: a waiting one leaves the sub-queue, the running one
        // is told to stop as the running interaction is (inferred, as for
        // the queue; the binary's cTSPerson::DeleteTopSubAction drops the
        // first waiting one when a Sim is cleaned up).
        auto& sq = sim.subQueue;
        auto st = std::find_if(sq.begin(), sq.end(), [&](const QueuedInteraction& x) { return x.uid == uid; });
        if (st != sq.end()) {
            sq.erase(st);
            setInteractionData(sim); // RemoveAction
        } else if (sim.subAction && sim.subAction->uid == uid)
            sim.subAction->cancelled = true;
        else
            return false;
        syncQueueData(sim);
        return true;
    }
    const auto index = int(it - q.begin());
    if (index > sim.activeInteraction) {
        const QueuedInteraction gone = *it;
        q.erase(it);
        setInteractionData(sim); // RemoveAction
        unlinkInteraction(vm, sim.id, gone);
    } else {
        it->cancelled = true;
        setPriority(sim, kPriorityIdle);
    }
    evaluatePriorities(vm, sim);
    syncQueueData(sim);
    return true;
}

void cancelInteractionsWith(VM& vm, ObjectId object) {
    std::vector<ObjectId> ids;
    for (const auto& [id, o] : vm.eachObject())
        if (!o.queue.empty() || !o.subQueue.empty())
            ids.push_back(id);
    for (ObjectId id : ids) {
        Object& sim = *vm.object(id);
        auto& q = sim.queue;
        for (size_t i = q.size(); i-- > 0;) {
            if (q[i].object != object)
                continue;
            if (int(i) > sim.activeInteraction)
                q.erase(q.begin() + std::ptrdiff_t(i));
            else
                q[i].cancelled = true;
        }
        std::erase_if(sim.subQueue, [&](const QueuedInteraction& x) { return x.object == object; });
        syncQueueData(sim);
        setInteractionData(sim); // RemoveAction
    }
}

// cTSPerson::ShouldInterrupt (BV 0xad6388): person data 71
// "non-interruptible" says no; the current action or sub-action cancelled
// says yes; else only a first waiting entry whose priority is above the
// person's (data 33). A waiting entry of no higher priority left Mains
// spinning in Idle for Input without yielding (the social bunny's "Function
// - Main - Check Queue" ran out of its instruction budget).
bool shouldInterrupt(const Object& sim) {
    if (!sim.person)
        return interactionCancelled(sim);
    if (sim.person->data[persondata::NonInterruptible] != 0)
        return false;
    const size_t next = size_t(sim.activeInteraction + 1);
    return interactionCancelled(sim) ||
           (next < sim.queue.size() && sim.queue[next].priority > sim.person->data[persondata::Priority]);
}

bool interactionCancelled(const Object& sim) {
    if (sim.subAction && sim.subAction->cancelled)
        return true;
    return sim.activeInteraction >= 0 && size_t(sim.activeInteraction) < sim.queue.size() &&
           sim.queue[size_t(sim.activeInteraction)].cancelled;
}

CheckResult checkInteraction(VM& vm, ObjectId sim, const QueuedInteraction& q, std::string* error, bool autonomous,
                             size_t maxTurns, std::vector<ActionString>* strings) {
    if (!q.guard)
        return CheckResult::NoGuard;
    const int16_t args[1] = {int16_t(autonomous ? 1 : 0)};
    Thread t = vm.start(sim, q.guard, args, q.object, q.object);
    t.actionStrings = strings;
    if (auto it = vm.threads.find(sim); it != vm.threads.end())
        t.temps = it->second.temps;
    for (size_t turn = 0; t.status == Thread::Status::Running && turn < maxTurns; ++turn)
        vm.run(t, vm.instructionsPerTurn);
    dropThreadRecords(vm, t); // records.cpp
    switch (t.status) {
    case Thread::Status::Finished: return t.result ? CheckResult::True : CheckResult::False;
    case Thread::Status::Running:
        if (error)
            *error = "still running after " + std::to_string(maxTurns) + " ticks";
        return CheckResult::Running;
    default:
        if (error)
            *error = t.status == Thread::Status::Killed ? "killed" : t.error;
        return CheckResult::Failed;
    }
}

PushOutcome attemptPush(VM& vm, Thread& t, bool nested) {
    Object* sim = vm.object(t.me);
    if (!sim)
        return PushOutcome::None;
    int compare = currentPriority(*sim);
    if (compare <= kPriorityAutonomous && !nested)
        compare -= 1; // autonomous interactions fall through to other ones
    {
        const size_t next = size_t(sim->activeInteraction + 1);
        if (sim->queue.size() <= next)
            return PushOutcome::None;
        const QueuedInteraction item = sim->queue[next];
        if (item.priority <= compare)
            return PushOutcome::None;
        // Taking the next entry clears the sub-actions, whether it passes its
        // check or not (cTSPerson::TryIdleForInput calls ClearSubActions before
        // the test; SetCurrentAction does it again).
        clearSubActions(vm, *sim);
        sim = vm.object(t.me);
        if (!sim)
            return PushOutcome::None;
        const bool exists = vm.object(item.object) != nullptr;
        bool ok = exists;
        if (ok) {
            // The check tree's parameter 0: the entry's "test as autonomous"
            // bit (TryIdleForInput BV 0xadeb6c: cEdithObjectTestSim::Init
            // with the interaction's vtable 0xc4), 1 for the entry the Find
            // Best Interaction found and Main pushed.
            ok = testInteraction(vm, t.me, item, item.testAutonomous);
            sim = vm.object(t.me); // the check tree may have removed the Sim
            if (!sim)
                return PushOutcome::None;
        }
        // The check tree may have changed the queue: find the entry again
        // (the binary has already taken it out: gone, it was taken).
        auto it = std::find_if(sim->queue.begin(), sim->queue.end(),
                               [&](const QueuedInteraction& x) { return x.uid == item.uid; });
        if (it == sim->queue.end())
            return PushOutcome::Skipped;
        if (ok) {
            try {
                vm.call(t, item.object, item.action, item.args);
            } catch (const VMError&) {
                ok = false; // the action tree does not exist
            }
        }
        if (!ok) {
            // ActionSkipped (Mac BV 0xaca0cc): out of the queue with its
            // links undone, so the Sim on the other side of a social stops
            // waiting (person data 110), and its object's OBJf entry 4
            // "queue skipped" runs; not for an entry whose object is gone.
            const ObjectId id = sim->id;
            sim->queue.erase(it);
            if (exists)
                actionSkipped(vm, id, item);
            if (Object* s = vm.object(id))
                syncQueueData(*s);
            return PushOutcome::Skipped;
        }
        Frame& f = t.stack.back();
        f.stackObject = item.object;
        f.interaction = true;
        // Run it from where it is now in the queue (normally `next`).
        const auto at = it - sim->queue.begin();
        if (size_t(at) != next) {
            QueuedInteraction moved = std::move(*it);
            sim->queue.erase(it);
            sim->queue.insert(sim->queue.begin() + std::ptrdiff_t(next), std::move(moved));
        }
        sim->activeInteraction = int(next);
        // Over a running interaction (inside its tree): the base game keeps
        // no stack of interrupted actions (base 1.0 TryIdleForInput 0xb7fc08:
        // the current action's links are undone, UnLinkInteractions with no
        // person, and SetCurrentAction puts the new one in its place). The
        // interrupted one's tree goes on under the new one's, as an action of
        // no one: when the new one ends the Sim's current action is the empty
        // interaction (StackJustPopped; person data 21, 80 and 33 at 0). (The
        // BV keeps it in a deque, +0x5e0, and makes it current again.)
        if (next > 0 && next - 1 < sim->queue.size()) {
            const QueuedInteraction old = sim->queue[next - 1];
            sim->queue.erase(sim->queue.begin() + std::ptrdiff_t(next - 1));
            sim->activeInteraction = int(next - 1);
            const ObjectId id = sim->id;
            unlinkInteraction(vm, 0, old);
            sim = vm.object(id);
            if (!sim)
                return PushOutcome::Started;
        }
        setPriority(*sim, item.priority);
        setCurrentAction(vm, *sim);
        updateObjectOwnership(vm, t.me, item.object);
        sim = vm.object(t.me);
        if (!sim)
            return PushOutcome::Started;
        evaluatePriorities(vm, *sim);
        syncQueueData(*sim);
        return PushOutcome::Started;
    }
}

void endInteraction(VM& vm, Thread& t) {
    t.interactionReturned = 1;
    Object* sim = vm.object(t.me);
    if (!sim)
        return;
    auto& q = sim->queue;
    if (sim->activeInteraction >= 0 && size_t(sim->activeInteraction) < q.size() &&
        q[size_t(sim->activeInteraction)].noContinuation)
        t.interactionReturned = 2;
    if (sim->activeInteraction >= 0) {
        if (size_t(sim->activeInteraction) < q.size()) {
            const QueuedInteraction gone = q[size_t(sim->activeInteraction)];
            q.erase(q.begin() + sim->activeInteraction);
            unlinkInteraction(vm, sim->id, gone);
            sim = vm.object(t.me);
            if (!sim)
                return;
        }
        --sim->activeInteraction;
    }
    if (sim->person)
        sim->person->data[persondata::NonInterruptible] = 0; // FreeSO: "verified in ts1"
    setPriority(*sim, sim->activeInteraction >= 0 ? q[size_t(sim->activeInteraction)].priority : kPriorityIdle);
    evaluatePriorities(vm, *sim);
    // The action's end in StackJustPopped: SetCurrentAction of the
    // interrupted action (BV; the VM keeps it, see endInteraction's header)
    // or of the empty interaction, then SetInteractionData.
    if (sim->person) {
        if (sim->activeInteraction >= 0)
            sim->person->recordsAtAction = uint16_t(sim->person->objectRecords.size());
        else
            becomeEmptyAction(vm, *sim);
    }
    syncQueueData(*sim);
    setInteractionData(*sim);
}

void setCurrentAction(VM& vm, Object& sim) {
    if (sim.person) {
        auto& d = sim.person->data;
        for (size_t k : {size_t(persondata::RouteStatus), size_t(persondata::NonInterruptible), size_t(73), size_t(110)})
            if (k < d.size())
                d[k] = 0;
        // +0x646: the object records with the action's own (the gosub
        // came first): the action ends when its record goes.
        sim.person->recordsAtAction = uint16_t(sim.person->objectRecords.size());
    }
    clearSubActions(vm, sim);
    syncQueueData(sim);
    setInteractionData(sim);
}

void clearSubActions(VM& vm, Object& sim) {
    if (sim.subQueue.empty() && !sim.subAction)
        return;
    const ObjectId id = sim.id;
    const std::vector<QueuedInteraction> waiting = std::move(sim.subQueue);
    sim.subQueue.clear();
    sim.subAction.reset();
    for (const QueuedInteraction& q : waiting)
        unlinkInteraction(vm, id, q);
    if (Object* s = vm.object(id))
        syncQueueData(*s);
}

void endSubInteraction(VM& vm, Thread& t) {
    if (Object* sim = vm.object(t.me)) {
        if (sim->subAction) {
            const QueuedInteraction gone = *sim->subAction;
            sim->subAction.reset();
            unlinkInteraction(vm, sim->id, gone);
            sim = vm.object(t.me);
            if (!sim)
                return;
        }
        syncQueueData(*sim);
    }
}

bool inInteraction(const Thread& t) {
    return std::any_of(t.stack.begin(), t.stack.end(), [](const Frame& f) { return f.interaction; });
}

void syncQueueData(Object& sim) {
    if (!sim.person)
        return;
    auto& d = sim.person->data;
    const auto& q = sim.queue;
    const bool running = sim.activeInteraction >= 0 && size_t(sim.activeInteraction) < q.size();
    const QueuedInteraction* a = running ? &q[size_t(sim.activeInteraction)] : nullptr;
    d[persondata::CurrentInteractionIndex] = a ? int16_t(a->interaction) : 0;
    d[persondata::CurrentInteractionObject] = a ? a->object : 0;
    // 166: the running interaction's autonomous bit (cTSInteraction vtable
    // 0xbc, which SetCurrentAction copies; BV 0xadc6d8). Was "priority 20".
    d[persondata::InteractionAutonomous] = a && a->autonomous;
    // 164 "Current Interaction Joinable" (SetCurrentAction BV 0xadc6d8 /
    // base 0xb83f54): the current action's TTAB entry has flag 0x02
    // (cTSTreeTableEntry vtable 0x38 / base 0x30); "Joinable - Are Sim X
    // and Y in same joinable Obj?" reads it. Was never written.
    d[164] = a && (a->ttabFlags & 0x02) ? 1 : 0;
    const size_t next = size_t(sim.activeInteraction + 1);
    d[persondata::NextInteractionIndex] = next < q.size() ? int16_t(q[next].interaction) : 0;
    d[persondata::NextInteractionObject] = next < q.size() ? q[next].object : 0;
    // 72 "cn: Next Queued Interaction Continuation?": the next entry was
    // pushed "continue as current" (Push Interaction byte 3 bit 0x04).
    // "Function - Main - Process Autonomy" reads it after Gosub Found Action
    // and, when set, waits for input (Wait For Notify) instead of running
    // "Post Interaction", so an autonomous snack goes on to "Eat" before
    // Main's "Kill - Objects in Hands" takes it (inferred from the label).
    d[72] = next < q.size() && (q[next].flags & 0x04) ? 1 : 0;
    // The sub-queue: its size, the interaction it nests in (the running
    // one), the next waiting entry and the running sub-action (labels of
    // STR# 0xC8; filled in as the queue's are, inferred).
    const auto& sq = sim.subQueue;
    d[persondata::SubQueueCount] = int16_t(sq.size());
    // cTSPerson::SetSubInteractionData (Mac BV 0xadaa30): the master is the
    // current action whether or not anything is nested in it, and 102/103
    // "ci: Current Running Interaction Index/Object ID" are the running
    // sub-action's, else the action's, else 0 (SetCurrentAction copies 21
    // and 80 into them first). "Pickup - Wait For Current Interaction" waits
    // on the carried baby's 102/103 for its "Be Picked Up" sub-action.
    d[persondata::SubQueueMasterObject] = a ? a->object : 0;
    d[persondata::SubQueueMasterIndex] = a ? int16_t(a->interaction) : 0;
    d[persondata::NextSubQueueIndex] = sq.empty() ? 0 : int16_t(sq.front().interaction);
    d[persondata::NextSubQueueObject] = sq.empty() ? 0 : sq.front().object;
    d[persondata::SubQueueObject] = sim.subAction ? sim.subAction->object : 0;
    const QueuedInteraction* current = sim.subAction ? &*sim.subAction : a;
    d[persondata::CurrentRunningInteractionIndex] = current ? int16_t(current->interaction) : 0;
    d[persondata::CurrentRunningObject] = current ? current->object : 0;
}

void setInteractionData(Object& sim) {
    if (!sim.person)
        return;
    const bool running = sim.activeInteraction >= 0 && size_t(sim.activeInteraction) < sim.queue.size();
    sim.person->data[109] =
        int16_t(running ? sim.queue[size_t(sim.activeInteraction)].uid : sim.person->emptyActionGuid);
}

void becomeEmptyAction(VM& vm, Object& sim) {
    if (!sim.person)
        return;
    sim.person->emptyActionGuid = newInteractionId(vm, sim);
    sim.person->recordsAtAction = 1;
}

std::vector<std::string> splitPiePath(const std::string& name) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t slash; (slash = name.find('/', start)) != std::string::npos; start = slash + 1)
        out.push_back(name.substr(start, slash - start));
    out.push_back(name.substr(start));
    return out;
}

std::vector<PieEntry> pieMenu(VM& vm, ObjectId sim, ObjectId object, const PieOptions& opt) {
    std::vector<PieEntry> out;
    const ObjectId leader = interactionLeader(vm, object);
    const Object* o = vm.object(leader);
    if (!o)
        return out;
    const auto ref = interactionTable(vm, *o);
    if (!ref)
        return out;
    for (size_t i = 0; i < ref->table->interactions.size(); ++i) {
        const auto& e = ref->table->interactions[i];
        QueuedInteraction q = entryFor(leader, *ref, e, opt.language);
        PieEntry p;
        p.object = leader;
        p.interaction = q.interaction;
        p.tableIndex = i;
        p.name = q.name.empty() ? "#" + std::to_string(q.interaction) : q.name;
        p.path = splitPiePath(p.name);
        // Debug interactions: TTAB flag 0x80 (as in The Sims 1: 212 distinct
        // "DEBUG - ...", "CHEAT - ..." names have it) or a US English name
        // starting with '*' (the translations sometimes drop the star).
        const std::string english = ref->strings ? ref->strings->get(e.stringIndex, lang::EnglishUS) : "";
        // As autonomy sees it, only the flag counts (the "*Autonomous
        // subqueue" of the beds is an autonomous entry, not a cheat).
        const bool flagDebug = e.flags & 0x80;
        p.debug = flagDebug ||
                  (!opt.autonomous && ((!english.empty() && english[0] == '*') || (!p.name.empty() && p.name[0] == '*')));
        // Debug interactions (the flag) are only offered with the testing
        // cheats; their check trees are not run (as FreeSO does for its debug
        // flag). A '*' name without the flag is not shown itself, but its
        // check tree runs: the strings it adds are the menu's entries (the
        // person's "*Object Menu Subqueue" runs the bed's "CT - Object Menu
        // Subqueue" on the Sim lying in it: Cuddle, Kiss, WooHoo, Try For
        // Baby, ...).
        std::vector<ActionString> strings;
        if (const Object* s = vm.object(sim))
            p.wrongAge = !availableToAge(*s, *ref->table, e);
        if (!flagDebug && !p.wrongAge) {
            if (Object* s = vm.object(sim))
                s->data[objdata::HideInteraction] = 0;
            p.check = checkInteraction(vm, sim, q, &p.error, opt.autonomous, opt.maxTurns, &strings);
            if (Object* s = vm.object(sim)) {
                p.hidden = s->data[objdata::HideInteraction] == 1;
                s->data[objdata::HideInteraction] = 0;
            }
        }
        // The strings the check tree added are menu entries of their own
        // whatever the tree then returns or hides: cEdithObjectTestSim::
        // sMakeNewMenuItem (Mac BV 0x4c8f28) puts each in the menu as Add/
        // Change the Action String runs, with check result 1 and not hidden,
        // and RunMenuCheckTree (0x4c93e4) adds the interaction itself only
        // when the tree added none and it passed unhidden. (The bed's "CT -
        // Object Menu Subqueue" adds its socials and returns false.)
        const bool finished = p.check == CheckResult::True || p.check == CheckResult::False;
        if (strings.empty() || !finished) {
            out.push_back(std::move(p));
            continue;
        }
        for (const auto& s : strings) {
            PieEntry d = p;
            d.debug = false;
            d.hidden = false;
            d.check = CheckResult::True;
            d.dynamic = true;
            d.name = s.name;
            d.path = splitPiePath(s.name);
            d.args = s.params;
            out.push_back(std::move(d));
        }
    }
    return out;
}

std::optional<QueuedInteraction> interactionFromPie(VM& vm, const PieEntry& e) {
    auto q = makeInteraction(vm, e.object, e.interaction);
    if (!q)
        return std::nullopt;
    q->name = e.name;
    q->args = e.args;
    return q;
}

void registerInteractionPrimitives(VM& vm) {
    vm.setPrimitive(0x0D, pushInteraction);
    vm.setPrimitive(0x11, idleForInput);
    vm.setPrimitive(0x31, notifyOutOfIdle);
    vm.setPrimitive(0x32, changeActionString);
    registerTurnPrimitive(vm); // Turn Body Towards (turn.cpp)
}

namespace {

std::string lower(std::string s) {
    for (auto& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isNumber(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

} // namespace

ObjectId findInteractionObject(VM& vm, const std::string& query) {
    if (isNumber(query))
        return vm.object(ObjectId(std::stoi(query))) ? ObjectId(std::stoi(query)) : 0;
    const std::string q = lower(query);
    for (const auto& [id, o] : vm.eachObject()) {
        if (!o.position.inWorld() || o.person || !o.definition || lower(o.definition->fileName).find(q) == std::string::npos)
            continue;
        const Object* leader = vm.object(interactionLeader(vm, id));
        if (leader && interactionTable(vm, *leader))
            return id;
    }
    return 0;
}

ObjectId findSim(VM& vm, const std::string& query) {
    if (isNumber(query)) {
        const Object* o = vm.object(ObjectId(std::stoi(query)));
        return o && o->person ? o->id : 0;
    }
    if (query.empty()) {
        const Object* sel = vm.object(vm.globals[global::SelectedPersonId]);
        if (sel && sel->person && sel->position.inWorld())
            return sel->id;
    }
    const std::string q = lower(query);
    for (const auto& [id, o] : vm.eachObject())
        if (o.person && o.position.inWorld() &&
            (q.empty() || (o.definition && lower(o.definition->fileName).find(q) != std::string::npos)))
            return id;
    return 0;
}

void setAllMotives(VM& vm, int16_t value) {
    std::vector<ObjectId> sims;
    for (const auto& [id, o] : vm.eachObject())
        if (o.person)
            sims.push_back(id);
    for (ObjectId id : sims)
        for (size_t k = 3; k < kMotiveCount; ++k)
            vm.object(id)->person->motives[k] = value;
}

void startLotObjects(VM& vm, const LotLoad& load, int ticks) {
    for (const auto& e : load.objects)
        if (e.created && vm.object(e.id))
            vm.runFunction(e.id, 0);
    for (const auto& e : load.objects)
        if (e.created && vm.object(e.id))
            vm.startMain(e.id);
    for (int i = 0; i < ticks; ++i)
        vm.tick();
}

} // namespace os2::vm
