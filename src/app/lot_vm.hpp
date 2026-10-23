#pragma once

// The app's side of the SimAntics VM: a lot's objects running in the VM while
// it is shown, with the Sims' actors following what the VM does to them.
// Plain C++ (no AppKit), driven from main.mm.

#include "app/look_at.hpp"
#include "app/seat_module.hpp"
#include "formats/lot.hpp"
#include "formats/sim_actor.hpp"
#include "vm/sim_motion.hpp"
#include "vm/vm.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace os2 {

class DbpfPackage;
class ResourceIndex;
struct Character;

// The VM runs at the game's rate: a tick per 42 ms of the scene clock, about
// 23.8 a second at normal speed, 30 ticks a game minute (docs/ui-live-mac.md
// §1.2); the game speed (the VM's Speed and Paused globals) scales the scene
// clock by TS2Config.txt's AnimSpeedMedium/Fast/Ultra (1, 3, 21).
// Walkers move as the VM moves them, from tile centre to tile centre at its
// walking speed, the speed of the Sim's walk animation (vm::walkTilesPerTick:
// 2 tiles/s for the normal adult walk), not by the walk animation's root motion: the VM's position is the truth (routing, slots,
// other trees read it), and the renderer only interpolates between the last
// two ticks and turns the Sim smoothly towards where it goes. The walk
// animation loops while the VM says the Sim walks (its locomotion set's
// "walk"), Animate Sim's animations play once when they start, and the
// Sim's idle plays otherwise.
class LotSimulation {
public:
    // The lot's objects resume their saved state (vm/lot_resume.hpp), or
    // start from their Inits with `fromInit`.
    LotSimulation(ResourceIndex& index, const Lot& lot, DbpfPackage& lotPackage,
                  const std::filesystem::path& lotFile, bool fromInit = false);
    LotSimulation(const LotSimulation&) = delete;
    LotSimulation& operator=(const LotSimulation&) = delete;

    vm::VM& machine() { return *vm_; }
    // What loading said (objects created, Inits that failed, ...).
    std::string summary() const { return summary_; }
    // The Sims the first day's "reset lot" broadcast made (vm/lot.hpp
    // runResetLot) and their character files: the host gives them actors.
    const std::vector<std::pair<int16_t, std::filesystem::path>>& arrivedSims() const { return arrived_; }

    // Lets the VM drive `actor`, the Sim of object `id` (its character's
    // age and gender go into the person data the animation names need);
    // `idle` is what it plays standing.
    void attach(SimActor& actor, int16_t id, const Character& character, const std::string& idle);
    // The attached Sims whose object the VM removed (an NPC gone home, a
    // carpool driver whose car left), its ID maybe already another
    // object's (Object::serial differs): the host drops their actors.
    std::vector<int16_t> goneSims() const;
    // Stops driving the actor of object `id`.
    void detach(int16_t id);
    // Makes the Sim walk to an object (vm::startScriptedWalk), then play
    // entry `animation` of the global animation list if given.
    void walk(int16_t sim, int16_t target, std::optional<uint16_t> animation);

    // Called when the VM changes what an attached Sim wears (person data 8
    // "current outfit", STR# 0xFA outfit types), with the outfit category
    // bit of its character file's keys (vm::outfitCategoryBit); the host
    // rebuilds the actor's model.
    std::function<void(int16_t id, SimActor& actor, uint32_t category)> onOutfitChange;
    // Called when a Sim's person data 58 "Person Age" changes (the Age
    // primitive): `ageBit` is the new life stage (1 toddler, 2 child, 4
    // teen, 8 adult, 0x10 elder, 0x20 baby); the host rebuilds the body.
    std::function<void(int16_t id, SimActor& actor, uint32_t ageBit)> onAgeChange;
    // Called when an object (not a Sim) starts an animation the VM plays on
    // it (Animate Object / Animate Overlay: AnimationState::overlays), at
    // `at` seconds in the caller's clock; the host poses the object with it.
    std::function<void(int16_t id, const std::string& animation, double at)> onObjectAnimation;
    // Instead, when set: each animation the VM plays on an object, one call
    // per overlay it starts (`overlay` its AnimationState id, `flipped` as
    // the VM plays it) and one per overlay it takes off (ended or stopped),
    // so that the host plays them all at once as the VM poses the object
    // (VM::objectPose): a two-tile door's leaves are two overlays of one
    // animation, the second flipped ("Set door open", semi-global
    // 0x7FD0DEBA 0x200D); with only the last one cued, one leaf opened.
    // `loop`: an Animate Object's played until stopped, looping (Overlay::forever).
    std::function<void(int16_t id, const std::string& animation, bool flipped, uint32_t overlay, bool loop, double at)>
        onObjectOverlay;
    std::function<void(int16_t id, uint32_t overlay, double at)> onObjectOverlayEnd;
    // Called when a Sim starts an animation the VM's Animate Sim plays (null:
    // it walks, climbs or idles instead), cued at `at` seconds.
    std::function<void(int16_t id, SimActor& actor, const Animation* animation, double at)> onSimAnimation;

    // Where the attached Sims are drawn: the VM's place, or the slot of the
    // object holding them (app/seat_module.hpp); the host sets its `slot`.
    SeatModule seats;
    // The actor drawing a VM object other than a bound Sim (an object the VM
    // animates), for the IK targets on its bones; null: none.
    std::function<const SimActor*(int16_t id)> actorOf;
    // Else a bone of an object's model (by name CRC) where it is drawn
    // (world), for an object drawn static: a loveseat's arm for the seated
    // idles' hands (the game reads the IK object's scene graph, animated or
    // not: AddChoreoAnimRequest, Mac BV 0x696644).
    std::function<std::optional<Xform>(int16_t id, uint32_t boneCrc)> objectBone;

    // Runs the VM up to `seconds` (whole ticks) and places and animates the
    // attached actors for that moment (one tick behind the VM, so that
    // there is a tick to interpolate towards).
    // `seconds` is the caller's (real) clock; returns the scene clock, the
    // moment to pose the actors at (it runs slower or faster with the game
    // speed, and stops while paused).
    double update(double seconds);
    uint64_t ticks() const { return vm_->ticks; }
    // The scene-clock moment of VM tick `tick` (VM::ticks while it runs), as
    // the actors play what it started.
    double tickTime(uint64_t tick) const { return double(tick ? tick - 1 : 0) / vm_->clock.ticksPerSecond + slip_; }
    static constexpr double kTicksPerSecond = 1000.0 / 42.0;
    // Scene clock scale of speeds 2, 3 and 4 (TS2Config.txt AnimSpeedMedium, Fast, Ultra).
    std::array<double, 3> speedScales{1.0, 3.0, 21.0};
    // The current scale (0 while paused).
    double speedScale() const;
    // Height of the floor (or terrain) of VM level `level` (1 = the lot's
    // ground level) at (x, y).
    // The pool whose water covers (x, y) of a VM level (its bottom level), if any.
    const Lot::SwimmingPool* poolAt(int level, float x, float y) const;
    float floorHeight(int level, float x, float y) const;
    // Build mode changed the lot's shape: the heights (and levels) the
    // floors are drawn at follow.
    void updateGeometry(const Lot& lot);

private:
    struct Bound {
        SimActor* actor = nullptr;
        int16_t id = 0;
        std::string idle;
        vm::SimMotion motion;
        std::string playing; // what the actor was last cued to play
        uint32_t serial = 0;
        uint64_t objectSerial = 0; // the vm::Object::serial attached (IDs are reused)
        uint32_t climbSerial = 0;
        uint32_t walkSerial = 0;
        int walkExtras = 0; // blended locomotion clips over the base one (priorities kWalkBlend + 1...)
        int16_t outfit = 0; // person data 8 the actor is dressed for
        int16_t age = 0;    // person data 58 the actor was built for
        int partial = -1;   // the priority of a part-body Animate Sim animation playing over the base, -1 none
        float zOffset = 0; // the cObject's height over the floor the VM's level gives
        struct PlayingOverlay {
            std::string name;
            int priority = 0; // its layer in the actor's player
            double start = 0; // when its animation's frame 0 was (seconds)
            bool blendingOut = false; // its last play's blend-out started (event 9997)
        };
        std::map<uint32_t, PlayingOverlay> overlays; // by vm::Overlay::id, over the base animation
        // Looping overlays taken off: when, and their frame 0 (a restart of
        // the same one right after goes on in phase, LotSimulation::playOverlays).
        std::map<std::string, std::pair<double, double>> stoppedOverlays;
        std::shared_ptr<HeadLook> look;    // the head turning towards Object::lookAt
        uint32_t lookSerial = 0;           // the last Look At seen (OS2_DEBUG_LOOK)
    };
public:
    // Where a Sim's Look At points its head now (world space), if anywhere.
    std::optional<Vec3> lookTarget(const vm::Object& o) const;
    // An object's "default 3/4 height" point (world space) as the host
    // draws it: its model's box centre in x and y, 0.75 x (bottom + top) in
    // z, in the model's frame (try_look_at, BV 0x6a19f8). Unset or nothing:
    // 0.75 over its floor.
    std::function<std::optional<Vec3>(int16_t id)> objectLookPoint;
    // Look At's slot point (world space; byte 14 kinds 1/2/3): the point of
    // the slot's bone by name (STR# 0x90 "Model - Bones"; a Sim's drawn
    // bones included, so the head follows one) or, without a bone, of the
    // slot's model-space offset through the object's placement (the Mac
    // binary: the look's slot's bone by name and its position as an
    // offset).
    std::function<std::optional<Vec3>(int16_t id, const std::array<float, 3>& offset, const std::string& bone)>
        objectSlotPoint;

private:
    // The bones of a request's IK object for `self`'s IK targets of type 2
    // (AnimPlayer::Options::ikObjectBone): the object's actor (actorOf, else
    // a bound Sim's) as posed last, in `self`'s model space. Empty for none.
    IkObjectBone ikObjectBones(const SimActor& self, int16_t object) const;
    // The object whose slot holds the Sim drawn by `actor` (0 if none).
    int16_t seatOf(const SimActor* actor) const;
    void playOverlays(Bound& b, const vm::Object& o, double at);
    void sampleAll(double at);
    void updateScene(double seconds);

    ResourceIndex& index_;
    Lot lot_;
    std::unique_ptr<vm::CodeLibrary> code_;
    std::unique_ptr<vm::VM> vm_;
    std::vector<Bound> bound_;
    std::map<int16_t, uint32_t> objectAnimSerials_; // objects' overlaySerial last seen
    std::map<int16_t, std::set<uint32_t>> objectOverlaysCued_; // onObjectOverlay's, per object
    std::string summary_;
    std::vector<std::pair<int16_t, std::filesystem::path>> arrived_;
    double last_ = 0;
    // The rest pose's box of each Sim actor (model space), for lookTarget.
    mutable std::map<const SimActor*, std::pair<Vec3, Vec3>> actorBoxes_;
    double scene_ = 0, caller_ = 0; // scene clock, caller's clock at the last update
    double slip_ = 0; // seconds the VM has fallen behind the caller's clock (below 0: a saved lot's ticks before it)
    std::map<vm::ObjectId, std::string> loggedAnimations_; // OS2_LOG_ANIMS: each Sim's last line
};

} // namespace os2
