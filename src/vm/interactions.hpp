#pragma once

// Interactions: the TTAB entries of objects, the Sims' interaction queues and
// the pie menu.
//
// An object's interactions are the TTAB its OBJD names ("tree table id",
// the 16-bit field at file offset 0x4E, as in The Sims 1; 0 and 0xFFFF =
// none), looked up in the object's own group, then in its semi-global group
// (Sims: TTAB 0x81 of the person semi-global). The pie menu strings are the
// TTAs with the same group and instance; an entry's "string index" is also
// its number for Push Interaction and the queue (unique in every TTAB, as
// FreeSO's TTA index). Parts of a multi-tile object share the interactions
// of their "interaction group" leader: a part whose OBJD interaction group
// (field 0x50) is N > 0 uses the part of its object whose interaction group
// is -N (as FreeSO's GetInteractionGroupLeader).
//
// The queue follows FreeSO's VMThread (EnqueueAction, AttemptPush,
// EndCurrentInteraction, CancelAction), checked against what The Sims 2's
// own trees expect: the person Main ("Function - Main - Check Queue", person
// semi-global 0x205A) reads person data 79 "cn: Next Queued Interaction
// Object ID" to see whether anything is queued, calls "Wait For Notify"
// (global 0x119), whose Idle for Input sets object flag 7 "notified by idle
// for input" when the queue is not empty, and runs the interaction from an
// Idle for Input with "allow push" (u16 at 2, as in The Sims 1). The engine
// keeps the queue's person data current (labels from STR# 0xC8): 21/80
// current interaction index/object, 33 priority, 45/79 next queued
// interaction index/object, 102 current running interaction index, 166
// interaction autonomous; 71 non-interruptible is cleared when an
// interaction ends.

#include "vm.hpp"

#include <optional>
#include <string>
#include <vector>

namespace os2::vm {

struct LotLoad;

// Queue priorities. Push Interaction's priority byte (STR# 0xE0: inherited,
// maximum, autonomous, user driven) maps onto these numbers in the Mac
// binary (TryPushAction: 100, 20, 50; docs/primitives-mac.md §4); the pie
// menu queues user driven ones.
constexpr int16_t kPriorityIdle = 0;
constexpr int16_t kPriorityAutonomous = 20;
constexpr int16_t kPriorityUserDriven = 50;
constexpr int16_t kPriorityMaximum = 100;

// Person data the queue maintains (STR# 0xC8).
namespace persondata {
constexpr int16_t CurrentInteractionIndex = 21;
constexpr int16_t Priority = 33;
constexpr int16_t NextInteractionIndex = 45;
constexpr int16_t NonInterruptible = 71;
constexpr int16_t NextInteractionObject = 79;
constexpr int16_t CurrentInteractionObject = 80;
constexpr int16_t CurrentRunningInteractionIndex = 102;
constexpr int16_t CurrentRunningObject = 103;
constexpr int16_t InteractionAutonomous = 166;
// The species: only Bon Voyage has the field, so the base game's Sims read
// 0 (the human); the Set to Next types 40-45 and 46-51 pick one each
// (docs/primitives-mac.md §0x1F).
constexpr int16_t Species = 186;
// The sub-queue (STR# 0xC8 labels).
constexpr int16_t SubQueueCount = 26;
constexpr int16_t SubQueueMasterObject = 75;
constexpr int16_t SubQueueMasterIndex = 76;
constexpr int16_t NextSubQueueIndex = 77;
constexpr int16_t NextSubQueueObject = 78;
constexpr int16_t SubQueueObject = 157;
} // namespace persondata
// More than this many waiting nested interactions: Push Interaction fails
// with result 4 (Mac binary).
constexpr size_t kMaxSubQueue = 4;

// Object data 52 "primitive result": Push Interaction and Snap leave why
// they failed there (Mac binary).
constexpr int16_t kObjdataPrimitiveResult = 52;
// Person data 53 "FindBestAction result Interaction Object ID" and object
// data 20 "tree table entry" (STR# 0xC8, 0x8D): what the last Find Best
// Interaction picked, for Push Interaction's byte 14 bit 1.
constexpr int16_t kPersondataFindBestObject = 53;
// Person data 32 "Person Type" (STR# 0xC8): not 0 is a visitor
// (cTSPerson::IsVisitor).
constexpr int16_t kPersondataPersonType = 32;
constexpr int16_t kObjdataTreeTableEntry = 20;

// Object flags (object data 8, STR# 0x8E; bit = label index).
constexpr int16_t kFlagNotifiedByIdleForInput = 1 << 6;

// OBJD fields (file offsets).
constexpr size_t kObjdTreeTableId = 0x4E;
constexpr size_t kObjdInteractionGroup = 0x50;

// The TTAB an object uses, with its strings (null if the group has no TTAs).
struct InteractionTableRef {
    uint32_t group = 0;
    uint16_t instance = 0;
    const InteractionTable* table = nullptr;
    const StringTable* strings = nullptr;
};
std::optional<InteractionTableRef> interactionTable(VM& vm, const Object& o);

// The object whose interactions a click on `id` shows (itself unless it is a
// part of an interaction group).
ObjectId interactionLeader(VM& vm, ObjectId id);

// The queue entry for interaction `interaction` (TTA index) of `object`:
// user driven, with the object as stack object and code owner. Nothing if
// the object has no such entry.
std::optional<QueuedInteraction> makeInteraction(VM& vm, ObjectId object, uint16_t interaction,
                                                 uint8_t language = lang::BrazilianPortuguese);

// Queues an interaction on a Sim (FreeSO's EnqueueAction): after the last
// entry of the same or higher priority, or right after the running one with
// `pushHead`. Something of a higher priority than the running interaction
// cancels it (it is told through Idle for Input). Returns the entry's uid.
// Throws VMError when the queue is full.
uint16_t enqueue(VM& vm, Object& sim, QueuedInteraction q, bool pushHead = false);

// What the player's pick does (cTSPerson::AddAction, Mac BV 0xadbbdc): a
// "Run Immediately" entry (TTAB flag 0x04) runs at once and never waits in
// the queue (as Push Interaction has it: the person's "*Object Menu
// Subqueue" picks, Cuddle ... Try For Baby on the Sim in the bed, push the
// bed's pair onto both Sims); anything else is queued. Returns the queued
// entry's uid, 0 when it ran at once or was refused.
uint16_t addAction(VM& vm, Object& sim, QueuedInteraction q);

// Cancels an entry of the queue or the sub-queue: a waiting one is removed,
// the running one (or the running sub-action) is told to stop (Idle for
// Input in its tree returns at once). False if there is none.
bool cancelInteraction(VM& vm, Object& sim, uint16_t uid);

// Generic Sims Call 13: removes the waiting interactions whose object is
// `object` from every queue and cancels the running ones.
void cancelInteractionsWith(VM& vm, ObjectId object);

// cTSPerson::ClearSubActions (Mac BV 0xadc4f4): undoes the links of every
// waiting sub-action (cTSInteraction::UnLinkInteractions), empties the
// sub-queue and drops the current sub-action (person data 26, 75-78 and 157
// go to 0).
void clearSubActions(VM& vm, Object& sim);
// cTSPerson::SetCurrentAction (Mac BV 0xadc6d8), once the Sim's running
// interaction is set (Idle for Input's next entry, Gosub Found Action):
// besides the queue's person data (syncQueueData, 164 "joinable" among
// them) it zeroes person data 39, 71 (non-interruptible), 73 (the
// footprint's trim) and 110 (the "notified" flag "Wait For Notify" reads),
// clears the sub-actions and writes 109 (setInteractionData).
void setCurrentAction(VM& vm, Object& sim);

// Whether the interaction a Sim is running was cancelled (for primitives
// that wait, like animations).
bool interactionCancelled(const Object& sim);

// cTSPerson::ShouldInterrupt: cancelled, or a waiting entry of a priority
// above the Sim's (person data 33), unless person data 71 says no.
bool shouldInterrupt(const Object& sim);

// Runs an entry's guard (check) tree for `sim` to completion, as a check
// tree: in a thread of its own, with the Sim's temps, the object as stack
// object and code owner, parameter 0 = 1 when autonomous (the binary's
// cEdithObjectTestSim byte +0x10, which TestInteraction, BV 0x4c6214, passes
// as parameter 0; docs/primitives-mac.md §4f, round n32). True if there is
// no guard or it returns true; on failure `error` gets why.
enum class CheckResult { NoGuard, True, False, Failed, Running };
// Pie menu entries the check tree adds (Add/Change the Action String) go to
// `strings` if given.
CheckResult checkInteraction(VM& vm, ObjectId sim, const QueuedInteraction& q, std::string* error = nullptr,
                             bool autonomous = false, size_t maxTurns = 100,
                             std::vector<ActionString>* strings = nullptr);

// Idle for Input with "allow push": takes the next queued interaction when
// its priority beats the running one (person data 33; autonomous ones fall
// through to each other). Its guard passes: its action tree is pushed onto
// the thread as an interaction root (Started). It fails, or it has no action
// tree: it leaves the queue with its links undone (cTSPerson::ActionSkipped,
// Mac BV 0xaca0cc) and Idle for Input waits a tick (Skipped, the binary's 2);
// its object is gone: it leaves the queue (Skipped too). None: nothing taken.
enum class PushOutcome { None, Started, Skipped };
PushOutcome attemptPush(VM& vm, Thread& t, bool nested = false);

// Called by the VM when an interaction root returns: removes the entry
// (unless Generic Sims Call 13 already did), resets the priority and
// non-interruptible, and has the Idle for Input that ran it return true on
// the next tick.
void endInteraction(VM& vm, Thread& t);
// A sub-queue interaction's action tree returned: the sub-action is over.
void endSubInteraction(VM& vm, Thread& t);

// Whether a frame of the thread runs a queued interaction.
bool inInteraction(const Thread& t);

// Refreshes the person data that mirrors the queue.
void syncQueueData(Object& sim);
// cTSPerson::SetInteractionData's person data 109 "ci: Current Interaction
// GUID" (BV 0xada7b0, base 1.0 the same): the current action's GUID (the
// empty interaction's when there is none). Written when an entry is added
// or removed, the action changes and on a reset; SetCurrentSubAction writes
// the sub-action's instead, which stays until the next of these.
void setInteractionData(Object& sim);
// The current action became the empty interaction (its action ended, the
// reset): SetCurrentAction gives it a GUID (global 36, AssignUniqueGUID)
// and the records' count +0x646 is 1.
void becomeEmptyAction(VM& vm, Object& sim);

// cTSPerson::ActionSkipped (Mac BV 0xaca0cc): an entry leaves the queue
// unrun: its links are undone and, when its object is there, the object's
// OBJf entry 4 "queue skipped" runs with the person as stack object
// (cTSObject::RunTree(4, person)).
void actionSkipped(VM& vm, ObjectId person, const QueuedInteraction& q);

// The person's object records (records.cpp; Person::ObjectRecord,
// docs/primitives-mac.md §4j). cTSPerson::OnGosubObjectTree: VM::call
// pushed `object`'s tree on a person's thread.
void gosubObjectTree(VM& vm, Thread& t, ObjectId object, bool changeIcon);
// cTSPerson::StackJustPopped: a frame of the thread left the stack.
void stackJustPopped(VM& vm, Thread& t);
// Pops frames down to `size`, each with stackJustPopped.
void popFrames(VM& vm, Thread& t, size_t size);
// The records a thread run apart (one-tick, check or OBJf-entry tree) left
// when it is dropped.
void dropThreadRecords(VM& vm, const Thread& t);
// cTSPerson::HasObjectReference: `id` is in one of the person's records.
bool hasObjectReference(const Object& person, ObjectId id);
// cTSPerson::OnCleanup's records: all of them go (data 62 - 1 each), and
// with `cascade` each record's object and its other users are reset.
void cleanupRecords(VM& vm, ObjectId person, bool cascade);
// cEdithObjectModule::CleanupPeople(x): every other person's OnCleanup(x).
void cleanupPeople(VM& vm, ObjectId x, bool cascade);
// cTSObject::Cleanup(nobody) of an object (VM::resetObject).
void cleanupObject(VM& vm, ObjectId id);

// Linked interactions (links.cpp, cTSInteraction::UnLinkInteractions in the
// Mac binary): IDs are unique across Sims; Push Interaction's flag 0x40
// links the new entry to another interaction; when an interaction ends or
// leaves a queue, the Sim running the one it links to gets person data 110
// "Interactions Linked Deleted" (the social wait loops leave on it), and a
// linked entry still waiting in a queue is taken out.
uint16_t newInteractionId(VM& vm, Object& sim);
void linkInteractions(VM& vm, QueuedInteraction& made, uint16_t other);
void unlinkInteraction(VM& vm, ObjectId owner, const QueuedInteraction& ended);

// --- Pie menu ----------------------------------------------------------------

struct PieEntry {
    ObjectId object = 0;      // whose interaction (the interaction group leader)
    uint16_t interaction = 0; // TTA index, as queued
    size_t tableIndex = 0;    // position in the TTAB
    std::string name;         // "Sub.../Item" for submenus
    std::vector<std::string> path; // name split at '/'
    bool debug = false;       // the game's debug/cheat interactions (TTAB flag 0x80 or a '*' name)
    CheckResult check = CheckResult::NoGuard;
    std::string error; // check tree failure
    // Entries the check tree named itself (Add/Change the Action String):
    // one per string, with the stack object it had as parameter 0.
    bool dynamic = false;
    std::array<int16_t, 4> args{};
    // The check tree set my object data "hide interaction" (50), as "Hide
    // Menu" (global 0x109) does: the entry exists (autonomy and pushes may
    // use it) but the menu does not show it (FreeSO's GetPieMenu).
    bool hidden = false;
    // Not for the Sim's age (availableToAge, autonomy.hpp): not shown, and
    // its check tree is not run.
    bool wrongAge = false;
    bool available() const { return !hidden && !wrongAge && (check == CheckResult::NoGuard || check == CheckResult::True); }
};

struct PieOptions {
    uint8_t language = lang::BrazilianPortuguese;
    bool autonomous = false;
    size_t maxTurns = 100; // for each check tree
};

// Every entry of the TTAB `object` (or its interaction group leader) uses,
// with the outcome of its check tree for `sim`, in TTAB order; an entry
// whose check tree added action strings becomes one entry per string (as
// FreeSO's GetPieMenu). The menu shows the available ones that are not debug.
std::vector<PieEntry> pieMenu(VM& vm, ObjectId sim, ObjectId object, const PieOptions& opt = {});

// The queue entry for a pie menu entry (user driven, its name and parameters).
std::optional<QueuedInteraction> interactionFromPie(VM& vm, const PieEntry& e);

// "Sub.../Item" -> {"Sub...", "Item"}.
std::vector<std::string> splitPiePath(const std::string& name);

// Push Interaction (0x0D), Idle for Input (0x11), Notify the Stack Object
// out of Idle (0x31) and Add/Change the Action String (0x32).
void registerInteractionPrimitives(VM& vm);
// Turn Body Towards (0x16, turn.cpp).
void registerTurnPrimitive(VM& vm);

// --- Hosts (s2pkg pie, the app) ---------------------------------------------

// An object by ID ("123") or by part of its OBJD file name (case-insensitive;
// "Fridge"): the lowest ID among the objects in the world whose interaction
// leader has interactions. 0 if none.
ObjectId findInteractionObject(VM& vm, const std::string& query);
// A Sim by ID or part of its name ("Don"), or (empty query) the selected
// one (global "selected person ID") if it is a Sim in the world, else the
// lowest-ID Sim in the world. 0 if none.
ObjectId findSim(VM& vm, const std::string& query = {});

// Test aid: sets motives 3-15 (Mood ... Fun, STR# 0x86) of every Sim to
// `value`, in place of the saved ones (lot_resume.hpp reads them from the
// PERS states).
void setAllMotives(VM& vm, int16_t value);

// Runs the Init of every object a lot load created, then starts their Mains
// (as `s2pkg lot-run` does) and gives `ticks` ticks.
void startLotObjects(VM& vm, const LotLoad& load, int ticks = 0);

} // namespace os2::vm
