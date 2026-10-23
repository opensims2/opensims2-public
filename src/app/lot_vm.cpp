#include "lot_vm.hpp"

#include <map>

#include "formats/dbpf.hpp"
#include "formats/lot_arrivals.hpp"
#include "formats/lot_objects.hpp"
#include "formats/resindex.hpp"
#include "formats/sim_character.hpp"
#include "vm/animation.hpp"
#include "vm/autonomy.hpp"
#include "vm/hood_bridge.hpp"
#include "vm/lot_neighborhood.hpp"
#include "vm/lot.hpp"
#include "vm/lot_resume.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace os2 {

namespace {
constexpr int16_t kCurrentOutfit = 8; // person data "current outfit" (STR# 0xC8)
constexpr uint16_t kModelBoneNames = 0x90; // STR# "Model - Bones" (slot_bones.cpp)
}

LotSimulation::LotSimulation(ResourceIndex& index, const Lot& lot, DbpfPackage& lotPackage,
                             const std::filesystem::path& lotFile, bool fromInit)
    : index_(index), lot_(lot), code_(std::make_unique<vm::CodeLibrary>(&index)),
      vm_(std::make_unique<vm::VM>(*code_)) {
    std::fprintf(stderr, "opensims2: vm constructed\n");
    std::fflush(stderr);
    vm::VM& m = *vm_;
    // The placement tests read the lot's heights and floors (vm/placement.cpp);
    // Build mode keeps lot_ in step (updateGeometry).
    m.placementLot = &lot_;
    m.initCreatedObjects = true;
    m.instructionsPerTurn = vm::kNodeLimitPerTurn; // the binary's limit (cTSTreeSim::DoNodeAction: 0x44D past 100000 nodes)
    // The game's tick: 42 ms of its (speed-scaled) clock (docs/ui-live-mac.md §1.2).
    m.clock.ticksPerSecond = kTicksPerSecond;
    m.walkTilesPerTick = float(2.0 / kTicksPerSecond);
    m.setClockGlobals();
    std::fprintf(stderr, "opensims2: loading saved objects\n");
    std::fflush(stderr);
    LotObjects saved = LotObjects::load(lotPackage);
    std::fprintf(stderr, "opensims2: saved objects %zu, arrivals\n", saved.objects.size());
    std::fflush(stderr);
    // A family that just moved in: its Sims join the lot (formats/lot_arrivals.hpp).
    const LotArrivals arrivals = addArrivingSims(saved, lot, lotFile, &index_, false);
    std::fprintf(stderr, "opensims2: arrivals done, neighborhood\n");
    std::fflush(stderr);
    // The neighborhood first, as lot-run (lot_neighborhood.hpp).
    const bool hood = vm::loadLotNeighborhood(m, lotFile);
    std::fprintf(stderr, "opensims2: neighborhood %d, loading lot into the VM\n", hood ? 1 : 0);
    std::fflush(stderr);
    if (hood && arrivals.family && arrivals.charge) {
        // The move-in charge (LoadLot step 0x1B), into the budget globals.
        const auto f = int16_t(arrivals.family);
        vm::setFamilyFunds(m, f, int32_t(int64_t(vm::familyFunds(m, f)) - arrivals.charge));
        std::fprintf(stderr, "opensims2: family %d moved in: charged %lld\n", f, (long long)arrivals.charge);
    }
    const vm::LotLoad load = vm::loadLot(m, lot, saved, vm::charactersDirOf(lotFile));
    std::fprintf(stderr, "opensims2: lot loaded, %zu created, resuming\n", load.created);
    std::fflush(stderr);
    // What the lot's file had that could not be read and the objects the VM
    // could not create (s2pkg's lot commands print them; the app did not):
    // the first 10 of each load.
    {
        const std::string file = lotFile.filename().string();
        size_t lines = 0;
        auto line = [&](const std::string& s) {
            if (lines++ < 10)
                std::fprintf(stderr, "opensims2: %s: %s\n", file.c_str(), s.c_str());
        };
        for (const auto& w : saved.warnings)
            line(w);
        for (const auto& e : load.objects)
            if (!e.error.empty())
                line("object " + std::to_string(e.id) + " (" + e.name + ") not created: " + e.error);
        if (lines > 10)
            std::fprintf(stderr, "opensims2: %s: %zu more\n", file.c_str(), lines - 10);
    }
    if (hood)
        vm::bindLotSims(m);
    const vm::ResumeResult r = vm::startSavedLot(m, load, saved, fromInit);
    std::fprintf(stderr, "opensims2: saved lot resumed\n");
    std::fflush(stderr);
    // The family's first day (SetupFamilyFirstDay): the "reset lot"
    // broadcast, whose "Sub - Generate Family" brings the family's Sims.
    if (hood && arrivals.firstDay) {
        const vm::ResetLotResult rl = vm::runResetLot(m, arrivals.arriving);
        for (size_t i = 0; i < rl.sims.size(); ++i)
            if (!rl.simFiles[i].empty())
                arrived_.emplace_back(rl.sims[i], rl.simFiles[i]);
        std::fprintf(stderr, "opensims2: first day: %zu \"reset lot\" trees, %zu Sims arrived\n", rl.trees,
                     rl.sims.size());
        for (const auto& e : rl.errors)
            std::fprintf(stderr, "opensims2: reset lot: %s\n", e.c_str());
    }
    // The lot is ready (cTSGameStateController, message 0x4C8E7905): "CT -
    // Lot Loaded" plays the move-in scene when a family has just moved in.
    if (std::string err; !vm::runLotLoaded(m, &err) && !err.empty())
        std::fprintf(stderr, "opensims2: CT - Lot Loaded: %s\n", err.c_str());
    // A saved lot goes on from its saved tick count (vm/thread_state: the
    // routes' checks are timed by it), and the scene clock starts there:
    // the game's cTSSimulator::Simulate (Mac BV 0x49c244) runs the ticks
    // the time since its last call owes (42 ms each, at most 125 a call),
    // whatever the tick count is. The VM used to wait for the scene clock
    // to reach the saved count: N001_Lot11 saved at 21:55 after 8789 ticks
    // stood still for 6 minutes at speed 1 once loaded.
    slip_ = -double(m.ticks) / m.clock.ticksPerSecond;
    char buf[256];
    if (fromInit)
        std::snprintf(buf, sizeof buf, "%zu of %zu saved objects in the VM, from their Inits, %zu Mains", load.created,
                      load.objects.size(), r.freshMains);
    else
        std::snprintf(buf, sizeof buf,
                      "%zu of %zu saved objects in the VM, %zu resumed from their saved stacks, %zu fresh Mains, %02d:%02d",
                      load.created, load.objects.size(), r.stacks, r.freshMains, m.clock.hour, m.clock.minute);
    summary_ = buf;
    // OS2_AUTONOMY_LOG=1: what each Find Best Interaction picks (s2pkg autonomy has more).
    if (std::getenv("OS2_AUTONOMY_LOG"))
        vm::autonomyObserver() = [this](const vm::AutonomyDecision& d) {
            std::fprintf(stderr, "autonomy: tick %llu Sim %d: %s\n", (unsigned long long)vm_->ticks, d.sim,
                         d.found ? ("\"" + d.best.front().name + "\" on object " + std::to_string(d.best.front().object)).c_str()
                                 : "nothing");
        };
}

const Lot::SwimmingPool* LotSimulation::poolAt(int level, float x, float y) const {
    const int l = level - 1; // the VM's levels count from 1 = the lot's level 0
    for (const auto& pool : lot_.pools)
        if (l == pool.level - 1 && x >= pool.x && y >= pool.y && x < pool.x + float(pool.width) &&
            y < pool.y + float(pool.height))
            return &pool;
    return nullptr;
}

float LotSimulation::floorHeight(int level, float x, float y) const {
    // VM levels count from 1 = the lot's level 0.
    const int l = std::clamp(level - 1, lot_.minLevel, lot_.minLevel + lot_.levelCount - 1);
    // In a pool (its bottom is the level below the pool's): the water's
    // surface, `depth` below the pool level's floor. The swim clips are made
    // about the surface ("a-swimSlow-idle": root 0.59 below, feet 1.4 below),
    // so a swimmer stood on the bottom was drawn under the water (inferred
    // from the clips; the lot model draws the water at the same height).
    if (const auto* pool = poolAt(level, x, y))
        return lot_.elevationAt(pool->level, std::clamp(x, 0.0f, float(lot_.width)),
                                std::clamp(y, 0.0f, float(lot_.height))) -
               pool->depth;
    return lot_.elevationAt(l, std::clamp(x, 0.0f, float(lot_.width)), std::clamp(y, 0.0f, float(lot_.height)));
}

void LotSimulation::updateGeometry(const Lot& lot) {
    lot_.minLevel = lot.minLevel, lot_.levelCount = lot.levelCount;
    lot_.elevations = lot.elevations;
    lot_.floors = lot.floors;
    lot_.rooms = lot.rooms;
    lot_.walls = lot.walls;
    lot_.pools = lot.pools;
}

void LotSimulation::attach(SimActor& actor, int16_t id, const Character& character, const std::string& idle) {
    vm::Object* o = vm_->object(id);
    if (!o)
        return;
    if (o->person) {
        // Person data 58 "Person Age" and 65 "Gender" from the character
        // file (the lot loader does not read them from the neighbourhood yet).
        int16_t age = vm::lifestage::Adult;
        if (character.age & 1)
            age = vm::lifestage::Toddler;
        else if (character.age & 2)
            age = vm::lifestage::Child;
        else if (character.age & 4)
            age = vm::lifestage::Teen;
        else if (character.age & 16)
            age = vm::lifestage::Elder;
        else if (character.age & 32)
            age = vm::lifestage::Baby;
        if (!o->person->data[vm::persondata::PersonAge])
            o->person->data[vm::persondata::PersonAge] = age;
        o->person->data[65] = character.gender & 1 ? 1 : 0;
    }
    Bound b;
    b.actor = &actor;
    b.id = id;
    b.idle = idle;
    b.motion.sample(*vm_, *o);
    b.zOffset = actor.position[2] - floorHeight(o->position.level, o->position.x, o->position.y);
    if (std::fabs(b.zOffset) > 0.25f)
        std::fprintf(stderr, "opensims2: Sim %d stands %.2f over its floor\n", id, b.zOffset);
    b.playing = idle;
    b.outfit = o->person ? o->person->data[kCurrentOutfit] : 0;
    b.age = o->person ? o->person->data[vm::persondata::PersonAge] : 0;
    b.objectSerial = o->serial;
    b.look = std::make_shared<HeadLook>(actor);
    actor.adjust = [look = b.look](std::vector<Xform>& local, double now) { look->apply(local, now); };
    bound_.push_back(std::move(b));
}

std::vector<int16_t> LotSimulation::goneSims() const {
    std::vector<int16_t> gone;
    for (const auto& b : bound_) {
        const vm::Object* o = vm_->object(b.id);
        if (!o || !o->person || o->serial != b.objectSerial)
            gone.push_back(b.id);
    }
    return gone;
}

void LotSimulation::detach(int16_t id) {
    std::erase_if(bound_, [&](const Bound& b) { return b.id == id; });
    seats.forget(id);
}

void LotSimulation::walk(int16_t sim, int16_t target, std::optional<uint16_t> animation) {
    vm::startScriptedWalk(*vm_, sim, target, animation);
}

// Overlays the VM plays on a Sim (Animate Overlay, Reach/Put's grasp and
// carry animations: "a2o-newspaper-carry" holds the arm up; the idle
// system's idles: breathing, finger twitches, stretches, looking around).
// Each start of one (vm::Overlay::id) is a layer of its own, at the level
// the VM gave it (vm::animlevel: Animate Overlay 2 x priority + 1, a carry
// pose 5), mixed with the base animation (walk, Animate Sim) bone by bone
// by level as the game's channel blender does (AnimPlayer): an idle (level
// 1) gives way to an Animate Sim (level 2) on the bones both key, a baby's
// carry pose (priority 2-3, level 5-7) stays over it. A layer blends in
// over the engine's blend time (vm::kBlendTicks), starts to blend out that
// long before the end of its last play (when the VM sends 9997), and goes
// as Animate Stop says (blended, short-blended or cut: Animate Stop's
// stoppedFades) when the VM takes it off. The carry poses also play while
// Animate Sim plays: the trees stop them themselves before an animation
// that needs the arm (Animate Stop mode 4, 237 uses: "Eat - Anim - Bite",
// "Interaction - Drink", "Sub - Put Cup Down", ...), and the newspaper's
// "Read" never does: its animations reach the paper by the arms' IK, which
// AnimPlayer solves over the carry pose.
void LotSimulation::playOverlays(Bound& b, const vm::Object& o, double at) {
    constexpr int kOverlayPriority = 100;
    constexpr float kFade = vm::kBlendTicks / 1000.0f;
    constexpr double kRestartGap = 0.25; // seconds
    // Gone from the VM: blend out (or cut) as Animate Stop said.
    for (auto it = b.overlays.begin(); it != b.overlays.end();) {
        const auto x = std::find_if(o.animation.overlays.begin(), o.animation.overlays.end(),
                                    [&](const vm::Overlay& x) { return x.id == it->first; });
        if (x != o.animation.overlays.end()) {
            if (x->blendOutSent && !it->second.blendingOut) {
                it->second.blendingOut = true;
                b.actor->cueStop(at, it->second.priority, x->fadeOutMs / 1000.0f);
            }
            ++it;
            continue;
        }
        float fade = kFade;
        for (const auto& [id, seconds] : o.animation.stoppedFades)
            if (id == it->first)
                fade = seconds;
        if (std::getenv("OS2_DEBUG_VM"))
            std::fprintf(stderr, "tick %llu sim %d overlay off: %s (fade %.3f)\n", (unsigned long long)vm_->ticks, b.id,
                         it->second.name.c_str(), fade);
        b.actor->cueStop(at, it->second.priority, fade);
        b.stoppedOverlays[it->second.name] = {at, it->second.start};
        it = b.overlays.erase(it);
    }
    // New ones: cue.
    for (const auto& x : o.animation.overlays) {
        if (b.overlays.count(x.id))
            continue;
        Bound::PlayingOverlay p;
        p.name = x.name;
        try {
            const Animation& anim = b.actor->animation(index_, x.name);
            p.priority = kOverlayPriority + int(x.id % 1000000);
            // Grasps play once (into the carry); what the VM ends or holds
            // loops until it is taken off.
            const bool loop = x.name.find("grasp") == std::string::npos && x.loops != 1;
            // The same looping idle started again right after it went off
            // (the route's waiting tree starts and stops the idle system
            // every (1.25 - distance) * 24 ticks, as the game's data does):
            // it goes on from where it was instead of from frame 0, so the
            // breathing does not start over each time (a visual choice).
            double phase = 0;
            if (const auto st = b.stoppedOverlays.find(x.name);
                loop && st != b.stoppedOverlays.end() && at - st->second.first <= kRestartGap)
                phase = at - st->second.second;
            p.start = at - phase;
            // Turned by the resource's and the request's heading offsets, as
            // SetupChannel does for every request.
            // At its request's speed, blended in as its weight function says.
            const float msPerTick = float(1000.0 / vm_->clock.ticksPerSecond);
            b.actor->cue(at, anim, {.fade = x.fadeInMs / 1000.0f, .loop = loop, .priority = p.priority, .turn = anim.headingOffset + x.headingOffset,
                                    .phase = phase, .level = x.level, .rate = x.perTick > 0 ? x.perTick / msPerTick : 1.0f,
                                    .ikObjectBone = ikObjectBones(*b.actor, x.ikObject)});
            if (std::getenv("OS2_DEBUG_VM"))
                std::fprintf(stderr, "tick %llu sim %d overlay on: %s (kind %d, level %d, loops %d)\n",
                             (unsigned long long)vm_->ticks, b.id, x.name.c_str(), x.oeType, x.level, x.loops);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: Sim %d: overlay %s\n", b.id, e.what());
        }
        b.overlays.emplace(x.id, std::move(p));
    }
}

// After a tick: what each Sim should play, cued at that tick's time.
void LotSimulation::sampleAll(double at) {
    if (onObjectAnimation || onObjectOverlay)
        for (const auto& [id, o] : vm_->objects()) {
            if (o.person)
                continue;
            uint32_t& seen = objectAnimSerials_[id];
            if (o.animation.overlaySerial == seen)
                continue;
            seen = o.animation.overlaySerial;
            if (onObjectOverlay) {
                std::set<uint32_t>& cued = objectOverlaysCued_[id];
                std::set<uint32_t> now;
                for (const auto& x : o.animation.overlays)
                    now.insert(x.id);
                for (auto it = cued.begin(); it != cued.end();)
                    if (!now.count(*it)) {
                        if (onObjectOverlayEnd)
                            onObjectOverlayEnd(id, *it, at);
                        it = cued.erase(it);
                    } else {
                        ++it;
                    }
                for (const auto& x : o.animation.overlays)
                    if (cued.insert(x.id).second)
                        onObjectOverlay(id, x.name, x.flipped, x.id, x.forever, at);
            } else if (!o.animation.overlays.empty()) {
                onObjectAnimation(id, o.animation.overlays.back().name, at);
            }
        }
    for (auto& b : bound_) {
        const vm::Object* o = vm_->object(b.id);
        if (!o || o->serial != b.objectSerial) // removed, or its ID went to a new object
            continue;
        b.motion.sample(*vm_, *o);
        playOverlays(b, *o, at);
        const auto& s = b.motion.current();
        if (o->person && o->person->data[vm::persondata::PersonAge] != b.age) {
            // The life stage as ComputeAging maps person data 58 (docs/cas-mac.md §17.2).
            b.age = o->person->data[vm::persondata::PersonAge];
            const int a = b.age;
            const uint32_t bit = a < 2 ? 0x20 : a == 2 ? 0x01 : a <= 15 ? 0x02 : a <= 18 ? 0x04 : a <= 50 ? 0x08 : 0x10;
            if (onAgeChange)
                onAgeChange(b.id, *b.actor, bit);
        }
        if (o->person && o->person->data[kCurrentOutfit] != b.outfit) {
            b.outfit = o->person->data[kCurrentOutfit];
            if (const uint32_t bit = vm::outfitCategoryBit(b.outfit); bit && onOutfitChange)
                onOutfitChange(b.id, *b.actor, bit);
        }
        std::string want = b.idle;
        // Treading water in a pool instead of standing (the pool's
        // controller overlays "a-swimSlow-idle" on the swimmers, and Animate
        // Sim leaves the base layer empty between strokes: the standing idle
        // showed the Sim upright in the water; inferred).
        if (poolAt(o->position.level, o->position.x, o->position.y))
            want = "a-swimSlow-idle";
        bool loop = true;
        float fade = vm::kBlendTicks / 1000.0f;
        bool ease = false;
        // An Animate Sim's request: its own blend out to its end (it goes on
        // after its action ended, while what comes next blends in) and its
        // speed (docs/anim-mac.md §13).
        float fadeOut = -1, rate = 1;
        // The base layer's channel level: the standing idle under
        // everything, a walk or a stair phase at the locomotion's 0
        // (CreateLocoRequest's priority), Animate Sim's its own.
        int level = 0;
        float weight = 1;
        if (!s.climb.empty()) {
            // Stair phases chain without blending (each starts where the last ended).
            const bool chained = b.playing.find("stairs-") != std::string::npos;
            want = s.climb, loop = false, fade = chained ? 0.0f : 0.1f, level = vm::animlevel::Walk;
        } else if (s.walking && !s.walk.empty()) {
            // The locomotion's clips: a start or a stop once, the walk cycle
            // looping; the first blends in over the request's fade in, each
            // next one over the request's blend distance, a cosine crossover
            // (cLocoCycle::AnimUpdate; docs/anim-mac.md §8.4).
            want = s.walk, loop = s.walkLoop, level = vm::animlevel::Walk;
            if (s.framed)
                fade = s.walkFade, ease = s.walkEase;
        }
        else if (!s.animation.empty()) {
            want = s.animation, loop = false, level = s.animLevel, weight = s.animWeight;
            fade = s.animFadeIn, fadeOut = s.animFadeOut, rate = s.animSpeed;
            // (Snapped as it starts, a chair's get-out from the corner the
            // trees snap the Sim to: what played before stays where it was in
            // the world, SeatModule's rule 3, so the blend starts from the
            // body as it was.)
        }
        // OS2_DEBUG_VM=1: each Sim's state after each tick.
        if (std::getenv("OS2_DEBUG_VM")) {
            auto it = vm_->threads.find(b.id);
            std::fprintf(stderr, "tick %llu sim %d at %.2f,%.2f walking %d anim '%s' playing '%s' thread %d %s\n",
                         (unsigned long long)vm_->ticks, b.id, s.x, s.y, s.walking, s.animation.c_str(), b.playing.c_str(),
                         it == vm_->threads.end() ? -1 : int(it->second.status),
                         it == vm_->threads.end() ? "" : it->second.error.substr(0, 200).c_str());
        }
        const bool walkClip = s.climb.empty() && s.walking && !s.walk.empty();
        const bool restart = (!s.climb.empty() && s.climbSerial != b.climbSerial) ||
                             (walkClip && s.framed && s.walkSerial != b.walkSerial) ||
                             (s.climb.empty() && !walkClip && !s.animation.empty() && s.serial != b.serial);
        b.climbSerial = s.climbSerial;
        b.walkSerial = s.walkSerial;
        if (want == b.playing && !restart)
            continue;
        // Seated with nothing to play: the body holds its pose (SeatModule's
        // rule 4: the get-in's end under the chair's idles) instead of
        // standing up in the standing idle between two of them, or while the
        // trees stop the idles before the get-out.
        if (want == b.idle && seats.seated(b.id))
            continue;
        try {
            const Animation& anim = b.actor->animation(index_, want);
            const bool animated = s.climb.empty() && !(s.walking && !s.walk.empty()) && !s.animation.empty();
            // Animate Sim's animations play in their own frame: turned by
            // their header heading (a chair's get-in from its corner).
            // An Animate Sim animation that keys only part of the body (the
            // face's "a-noExpression") plays over what the body does instead
            // of replacing it (the Sim in a chair kept its seat; inferred).
            if (b.partial >= 0)
                b.actor->cueStop(at, b.partial, fade), b.partial = -1;
            // So does one that only adds to the bones (delta channels: the
            // talking "SWAT" shifts are full-body by their flag).
            const bool partial = animated && (!(anim.flags & 1) || anim.additive());
            if (partial)
                b.partial = std::max<int>(1, anim.priority);
            const bool mirror = animated ? s.flipped : !s.climb.empty() ? s.turnMirror : (walkClip && s.walkMirror);
            // The locomotion's clips blended by weight (two start or stop
            // clips 45 degrees apart, the weighed walk cycles), each at its
            // rate over the blend's length, from the cycle portion the walk
            // is at; the extra ones as layers of their own at the same level.
            constexpr int kWalkBlend = 60;
            int extras = 0;
            if (walkClip && !s.walkLayers.empty() && s.walkLayers.front().animation == want) {
                const auto& base = s.walkLayers.front();
                b.actor->cue(at, anim, {.fade = fade, .loop = loop, .mirror = base.mirror,
                                        .phase = double(s.walkPortion) * anim.duration / 1000.0, .level = level,
                                        .weight = base.weight, .rate = base.rate, .expires = false, .ease = ease});
                for (size_t i = 1; i < s.walkLayers.size() && i < 8; ++i) {
                    const auto& l = s.walkLayers[i];
                    const Animation& more = b.actor->animation(index_, l.animation);
                    b.actor->cue(at, more, {.fade = l.fade >= 0 ? l.fade : fade, .loop = loop, .priority = kWalkBlend + int(i), .mirror = l.mirror,
                                            .phase = double(s.walkPortion) * more.duration / 1000.0, .level = level,
                                            .weight = l.weight, .rate = l.rate, .expires = false, .ease = ease});
                    extras = int(i);
                }
            } else {
                // Our standing idle (the stand-in for the idle trees) mixes
                // over the pose the last request left.
                const bool standIn = !animated && !walkClip && s.climb.empty();
                b.actor->cue(at, anim, {.fade = fade, .loop = loop, .priority = partial ? b.partial : -1,
                                        .mirror = mirror, .turn = animated ? anim.headingOffset + s.animHeading : 0.0f,
                                        .phase = walkClip ? double(s.walkPhase) : animated ? double(s.animPlayed) / 1000.0 : 0.0,
                                        .level = level,
                                        .weight = weight,
                                        .rate = animated ? rate : 1.0f,
                                        .ikObjectBone = animated ? ikObjectBones(*b.actor, s.animIkObject) : IkObjectBone{},
                                        .expires = animated,
                                        .fadeOut = animated ? fadeOut : -1.0f,
                                        .overHeld = standIn});
            }
            for (int j = extras + 1; j <= b.walkExtras; ++j)
                b.actor->cueStop(at, kWalkBlend + j, fade);
            b.walkExtras = extras;
            if (onSimAnimation)
                onSimAnimation(b.id, *b.actor, s.climb.empty() && !(s.walking && !s.walk.empty()) && !s.animation.empty() ? &anim : nullptr, at);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: Sim %d: %s\n", b.id, e.what());
        }
        b.playing = want;
        b.serial = s.serial;
    }
}

int16_t LotSimulation::seatOf(const SimActor* actor) const {
    for (const auto& b : bound_)
        if (b.actor == actor)
            if (const vm::Object* o = vm_->object(b.id); o && seats.seated(b.id))
                return o->data[vm::objdata::ContainerId];
    return 0;
}

IkObjectBone LotSimulation::ikObjectBones(const SimActor& self, int16_t object) const {
    // OS2_NO_IK_OBJECT=1: no IK object (type-2 goals in model space), to compare.
    if (!object || std::getenv("OS2_NO_IK_OBJECT"))
        return {};
    // By name CRC, cached per skeleton.
    auto cache = std::make_shared<std::map<std::pair<const Skeleton*, uint32_t>, int>>();
    return [this, self = &self, object, cache](uint32_t crc) -> std::optional<Xform> {
        const SimActor* other = nullptr;
        for (const auto& b : bound_)
            if (b.id == object)
                other = b.actor;
        if (!other && actorOf)
            other = actorOf(object);
        // The seat holding the Sim, drawn static (a loveseat): its model's
        // bone where it stands (the seated idles' hands on its arm). Other
        // static objects stay out, as before (inferred: their goals were
        // not checked against the game).
        if (!other && objectBone && seatOf(self) == object)
            if (const auto w = objectBone(object, crc))
                return self->placement().inverse().then(*w);
        if (!other || other == self || other->bones.size() != other->skin.skeleton.bones.size())
            return std::nullopt;
        const Skeleton* sk = &other->skin.skeleton;
        auto it = cache->find({sk, crc});
        if (it == cache->end()) {
            int found = -1;
            for (size_t i = 0; i < sk->bones.size() && found < 0; ++i)
                if (nameInstanceHiHash(sk->bones[i].name) == crc)
                    found = int(i);
            it = cache->emplace(std::make_pair(sk, crc), found).first;
        }
        if (it->second < 0)
            return std::nullopt;
        return self->placement().inverse().then(other->bones[size_t(it->second)]);
    };
}

// Look At's target, "default 3/4 height" (the editor's string; target type
// 0, byte 14): try_look_at (BV 0x6a19f8) takes the bounding box of the
// target's shapes in its root node's frame (FUN_00a45e54) and aims at its
// centre in x and y and at 0.75 x (bottom + top) in z, from the root:
// three quarters up a box standing on the floor (a Sim's chin, a table's
// top). A Sim's box is its rest pose's (the shapes' bounds of a skinned
// model: inferred). Objects: objectLookPoint, else 0.75 over the floor
// (inferred). A slot kind (byte 14 1/2/3) aims at the target's slot point
// (objectSlotPoint). Not done: the camera.
std::optional<Vec3> LotSimulation::lookTarget(const vm::Object& o) const {
    // (How long the head stays on it is the look's own hold, from when it
    // gets there: HeadLook.)
    if (!o.lookAt)
        return std::nullopt;
    const vm::Object* x = vm_->object(o.lookAt->target);
    if (!x || x->id == o.id)
        return std::nullopt;
    // A slot of the target (byte 14 kinds 1/2/3: targeting, routing,
    // containment = its SLOT entries of type 4, 3 and 0, view.cpp): the
    // point the entry names, its bone or, without one, its stored offset.
    if (o.lookAt->slotKind && objectSlotPoint) {
        const uint16_t type = o.lookAt->slotKind == 1 ? 4 : o.lookAt->slotKind == 2 ? 3 : 0;
        const Slots::Slot* entry = nullptr;
        if (x->definition)
            if (const Slots* t = code_->slots(x->group, x->definition->slotId())) {
                size_t n = 0;
                for (const auto& s : t->slots)
                    if (s.type == type && n++ == o.lookAt->slotIndex) {
                        entry = &s;
                        break;
                    }
            }
        if (entry) {
            std::string bone;
            if (const StringTable* names = code_->strings(x->group, kModelBoneNames))
                if (entry->ts2Extra[0] < names->count(lang::EnglishUS))
                    bone = names->get(entry->ts2Extra[0], lang::EnglishUS);
            if (const auto p = objectSlotPoint(x->id, entry->offset, bone))
                return p;
        }
    }
    // OS2_LOOK_AT_HEAD=1: the old inferred target (the head bone), to compare.
    static const bool atHead = std::getenv("OS2_LOOK_AT_HEAD") != nullptr;
    for (const auto& b : bound_)
        if (b.id == x->id && atHead) {
            const int head = b.actor->skin.skeleton.find("head");
            if (head >= 0 && size_t(head) < b.actor->bones.size())
                return b.actor->bones[size_t(head)].t;
        }
    for (const auto& b : bound_)
        if (b.id == x->id) {
            auto it = actorBoxes_.find(b.actor);
            if (it == actorBoxes_.end()) {
                Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
                for (const auto& part : b.actor->skin.model.parts)
                    if (!part.shadowPlane)
                        for (const auto& p : part.mesh.positions)
                            for (int k = 0; k < 3; ++k)
                                lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
                if (lo[0] > hi[0])
                    lo = hi = Vec3{0, 0, 1.6f / 1.5f};
                it = actorBoxes_.emplace(b.actor, std::make_pair(lo, hi)).first;
            }
            const auto& [lo, hi] = it->second;
            return b.actor->placement().apply(
                Vec3{(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, 0.75f * (lo[2] + hi[2])});
        }
    if (objectLookPoint)
        if (const auto p = objectLookPoint(x->id))
            return p;
    if (!x->position.inWorld())
        return std::nullopt;
    return Vec3{x->position.x, x->position.y, floorHeight(x->position.level, x->position.x, x->position.y) + 0.75f};
}

double LotSimulation::speedScale() const {
    const auto& g = vm_->globals;
    // Paused, by the player or by a dialog that blocks the game ("engage
    // and block": the simulator waits for the answer), the scene clock the
    // actors' animations run on stops with the VM's. The scene graph's
    // animation timer (nTSSG::cCustomAnimTimer2: elapsed ticks a frame) runs
    // on the simulator's ticks, and the locomotion's phases are timed on it
    // (docs/anim-mac.md §8.1); inferred for the dialog's pause, as for the
    // player's. Until round n30 a dialog stopped the VM only: a Sim's clip
    // played on to its end while the dialog waited, and once it was
    // answered the VM walked the Sim along the rest of the clip's way with
    // its legs held in the last frame (tests/glides.sh: N001_Lot68, tick
    // 4603, the turnAndGo of a teen leaving a social).
    if (g[vm::global::Paused] || vm::dialogPausesGame(*vm_))
        return 0;
    switch (g[vm::global::Speed]) {
    case 3: return speedScales[1];
    case 4: return speedScales[2];
    default: return speedScales[0];
    }
}

double LotSimulation::update(double seconds) {
    // The game speed scales the scene clock the VM and the animations run on.
    scene_ += std::clamp(seconds - caller_, 0.0, 0.25) * speedScale();
    caller_ = seconds;
    // Dialogs time out on real time, also while they pause the game.
    vm_->dialogClockIsTicks = false;
    vm::autoTimeOutDialogs(*vm_, seconds);
    updateScene(scene_);
    return scene_;
}

void LotSimulation::updateScene(double seconds) {
    const double rate = vm_->clock.ticksPerSecond; // ticks per second of the scene clock
    // After a stall (loading, a window drag) the VM does not race to catch
    // up: past 125 ticks behind (the game's limit per frame), its clock
    // slips instead.
    constexpr uint64_t kMaxCatchUp = 125;
    double vmSeconds = seconds - slip_;
    if (vmSeconds * rate > double(vm_->ticks + kMaxCatchUp)) {
        slip_ = seconds - double(vm_->ticks + kMaxCatchUp) / rate;
        vmSeconds = seconds - slip_;
    }
    seconds = vmSeconds;
    // The actors show the moment one tick behind the VM.
    const auto target = uint64_t(std::max(0.0, std::floor(seconds * rate))) + 1;
    // A dialog that blocks the game (Dialog, "engage and block") pauses it:
    // the VM's clock slips while the player has not answered (or the
    // dialog times out on real time: LotSimulation::update).
    if (vm::dialogPausesGame(*vm_)) {
        slip_ = seconds + slip_ - double(vm_->ticks - 1) / rate;
        seconds = double(vm_->ticks - 1) / rate;
    }
    while (vm_->ticks < target && !vm::dialogPausesGame(*vm_)) {
        vm_->tick();
        sampleAll(double(vm_->ticks - 1) / rate + slip_); // in the caller's clock, as the actors play
        // OS2_LOG_ANIMS=1: each Sim's Animate Sim and overlays as they change (as s2pkg pie --anims).
        if (static const bool log = std::getenv("OS2_LOG_ANIMS") != nullptr; log) {
            auto& last = loggedAnimations_; // (by object ID: this lot's)
            for (const auto& [oid, ob] : vm_->objects())
                if (ob.person) {
                    std::string s = ob.animation.name + "@" + std::to_string(ob.animation.level) + " [";
                    for (const auto& o : ob.animation.overlays)
                        s += o.name + "@" + std::to_string(o.level) + (o.ikObject ? " ik " + std::to_string(o.ikObject) : "") + " ";
                    s += "] ik " + std::to_string(ob.animation.eventObject) + " in " + std::to_string(ob.data[vm::objdata::ContainerId]);
                    if (s != last[oid])
                        std::printf("opensims2: tick %llu: Sim %d %s at (%.1f, %.1f, %d)\n", (unsigned long long)vm_->ticks,
                                    oid, s.c_str(), ob.position.x, ob.position.y, int(ob.position.level)),
                            last[oid] = s;
                }
        }
    }
    const float t = float(std::clamp(seconds * rate - double(vm_->ticks - 1), 0.0, 1.0));
    const float dt = float(std::clamp(seconds - last_, 0.0, 0.1));
    last_ = seconds;
    for (auto& b : bound_) {
        const float z = b.motion.z(t, [this](int level, float fx, float fy) { return floorHeight(level, fx, fy); });
        // (The moment in the caller's clock, as the actors and the objects'
        // slots play: `seconds` is the VM's, ahead of it on a saved lot.)
        seats.place(b.id, *b.actor, b.motion, seconds + slip_, t, dt, z, b.zOffset);
        if (const vm::Object* o = vm_->object(b.id)) {
            const auto target = lookTarget(*o);
            if (std::getenv("OS2_DEBUG_LOOK") && target.has_value() != b.look->hasTarget())
                std::fprintf(stderr, "tick %llu sim %d %s %d\n", (unsigned long long)vm_->ticks, b.id,
                             target ? "looks at" : "stops looking at", o->lookAt ? o->lookAt->target : 0);
            std::optional<HeadLook::Request> request;
            if (o->lookAt) {
                const auto& l = *o->lookAt;
                request = HeadLook::Request{l.serial, float(l.towards), float(l.away), l.durationMs, l.spine, l.noEarlyExit};
                if (std::getenv("OS2_DEBUG_LOOK") && l.serial != b.lookSerial)
                    std::fprintf(stderr, "tick %llu sim %d: Look At %u: object %d, %d/%d degrees a second, hold %d ms%s%s\n",
                                 (unsigned long long)vm_->ticks, b.id, l.serial, l.target, l.towards, l.away, l.durationMs,
                                 l.spine ? ", spine" : "", l.noEarlyExit ? ", no early exit" : "");
                b.lookSerial = l.serial;
            }
            b.look->setRequest(request, target, float(o->lookAwayRate));
        }
    }
}

} // namespace os2
