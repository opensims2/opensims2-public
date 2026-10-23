// The lot's audio and the lot's objects in the VM (--vm, --run-vm, the live
// UI): every hook between the VM and the views, and the VM's test aids
// (--vm-create, --vm-queue, --dialog-demo, ...) (app_state.hpp).

#include "app/app_state.hpp"

namespace os2app {

void startAudio(os2::ResourceIndex& index, const os2::Lot& lot) {
    if (!gGameAudio)
        return;
    os2::LotAudio::Options o;
    o.music = argument(@"--music").value_or("");
    gAudio = std::make_unique<os2::LotAudio>(*gGameAudio, index, lot, o);
    for (const auto& s : gLotSims) {
        os2::DbpfPackage character(s.character);
        const std::string shoe = os2::characterShoe(index, os2::readCharacter(character, index), gAudio->sound());
        gAudio->attachSim(*s.actor, s.id, shoe.empty() ? "rub" : shoe);
    }
}

void updateAudio(double seconds) {
    if (!gAudio)
        return;
    os2::LotAudio::Listener l;
    if (gCamera) {
        // (the free camera hears from where it is: its orbit pivot is stale)
        const simd_float3 p = gCameraSelector.freeCamera() ? gCamera->eye() : gCamera->pivot(),
                          look = gCameraSelector.freeCamera() ? gCamera->forward() : p - gCamera->eye();
        l.x = p.x, l.y = p.y, l.z = std::max(0.0f, p.z - 1);
        l.rightX = look.y, l.rightY = -look.x; // forward x up
        l.zoom = std::clamp(gCamera->zoom() / 5.0f, 0.0f, 1.0f);
    }
    // The mode's music state: Build, Buy, or (live mode) the lot's: only the
    // stereos, as the game's music manager switches it.
    gGameAudio->setMusic(gBuild && gBuild->active() ? "Build"
                         : gBuy && gBuy->active()   ? "Buy"
                                                    : gAudio->liveMusic());
    gAudio->update(gSimulation ? &gSimulation->machine() : nullptr, seconds, l);
    if (gTv && gSimulation) {
        // A channel's sound comes out of its first receiver (the lowest
        // object ID showing it), as the binary's sound emitter proxy does
        // (app/tv_broadcast.hpp); the other TVs on the channel are silent.
        std::vector<os2::LotAudio::TvSource> on;
        std::set<std::string> heard;
        for (const auto& [id, o] : gSimulation->machine().objects()) // (by ID)
            for (const auto& [group, material] : o.materials)
                if (const auto* s = gTv->isChannel(material) ? gTv->sound(material) : nullptr) {
                    if (heard.insert(material).second)
                        on.push_back({id, s->serial, s->pcm, std::max(0.0, gTv->now() - s->started)});
                    break;
                }
        gAudio->tvSounds(gSimulation->machine(), on);
    }
}

// cTSMainToolbar::ApplySavedUIState (docs/game-flow-mac.md §7.6): the pause,
// the wall mode and the floor saved with the lot (cTSMainToolbar::Read,
// resource 0x8B0C79D6) put back after it loads, over the first day's Resume
// too; done before Buy or Build mode is entered, as they keep the pause they
// find for Live mode. A lot without the resource keeps what it had.
static void applySavedUiState(os2::DbpfPackage& pkg, const os2::Lot& lot) {
    gScene.lotUi = os2::readLotUiState(pkg);
    if (!gScene.lotUi || !gSimulation)
        return;
    const os2::LotUiState& ui = *gScene.lotUi;
    gSimulation->machine().globals[os2::vm::global::Paused] = ui.paused ? 1 : 0;
    setWallMode(ui.wallMode);
    const int level = os2::savedUiLevel(ui.level, lot.minLevel, int(lot.levelCount));
    if (gCameraFollow)
        gCameraFollow->setLevel(level);
    std::printf("opensims2: saved UI state: %s, floor %d (app %d), walls %d\n", ui.paused ? "paused" : "running",
                int(ui.level), level, int(ui.wallMode));
}

// --vm / --run-vm: the lot's objects in the VM, with its Sims attached to
// their actors; --vm-walk <sim>,<object>[,<anim>] starts a scripted walk.
void startSimulation(os2::Renderer& renderer, os2::ResourceIndex& index, const os2::Lot& lot, os2::DbpfPackage& pkg,
                     const fs::path& lotFile) {
    CFTimeInterval start = CACurrentMediaTime();
    gSimulation = std::make_unique<os2::LotSimulation>(index, lot, pkg, lotFile,
                                                      [sceneArgs() containsObject:@"--init"]);
    // The Sims the first day's "reset lot" made (a family moving in).
    for (const auto& [id, file] : gSimulation->arrivedSims())
        if (const os2::vm::Object* o = gSimulation->machine().object(id))
            spawnSimActor(renderer, index, file, id,
                          simd_make_float3(o->position.x, o->position.y,
                                           gSimulation->floorHeight(o->position.level, o->position.x, o->position.y)),
                          float(o->data[os2::vm::objdata::Direction]) * float(M_PI) / 4, 0.37 * double(gLotSims.size()),
                          "arrived");
    for (const auto& s : gLotSims) {
        os2::DbpfPackage character(s.character);
        gSimulation->attach(*s.actor, s.id, os2::readCharacter(character, index), s.idle);
    }
    perfLap("  LotSimulation");
    gObjectView = std::make_unique<os2::VmObjectView>(renderer, index, *gSimulation);
    // The objects' ground and wall shadow planes follow the wall mode, the
    // level shown and the lot's walls (docs/lighting-mac.md 12.3).
    if (gLotShape)
        gObjectView->setWalls(os2::LotWallEdges(*gLotShape));
    // (OS2_GUOB_AS_SAVED=1: the planes as the lot saved them, all on.)
    if (!std::getenv("OS2_GUOB_AS_SAVED"))
        gObjectView->guobView = [] { return std::pair<int, int>{gWallView.mode(), viewLevel()}; };
    // Look At's "default 3/4 height" of an object: its model's box where it
    // is drawn (LotSimulation::lookTarget).
    gSimulation->objectLookPoint = [](int16_t id) -> std::optional<os2::Vec3> {
        if (!gObjectView)
            return std::nullopt;
        const auto at = gObjectView->placement(id);
        if (!at)
            return std::nullopt;
        for (const auto& b : gPickBoxes)
            if (int16_t(b.instance & 0xFFFF) == id)
                return at->apply(os2::Vec3{(b.lo.x + b.hi.x) * 0.5f, (b.lo.y + b.hi.y) * 0.5f, 0.75f * (b.lo.z + b.hi.z)});
        return std::nullopt;
    };
    // Look At's slot point (byte 14 kinds 1/2/3): the slot's bone by name
    // (a Sim's drawn bones included) or, without one, its stored offset
    // through the object's placement (LotSimulation::lookTarget).
    gSimulation->objectSlotPoint = [](int16_t id, const std::array<float, 3>& offset,
                                      const std::string& bone) -> std::optional<os2::Vec3> {
        if (!gObjectView)
            return std::nullopt;
        if (!bone.empty())
            if (const auto w = gObjectView->nodePlacement(id, bone))
                return os2::Vec3{w->t[0], w->t[1], w->t[2]};
        if (const auto at = gObjectView->placement(id))
            return at->apply(os2::Vec3{offset[0], offset[1], offset[2]});
        return std::nullopt;
    };
    // TV channels and arcade screens (app/tv_broadcast.hpp): a broadcast's material shows its movies.
    try {
        gTv = std::make_unique<os2::TvBroadcast>(renderer, gameDir() / "TSData/Res");
        gObjectView->adjustModel = [](os2::ObjectModel& m, const std::map<std::string, std::string>& materials) {
            if (gTv)
                gTv->adjust(m, materials);
        };
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: tv: %s\n", e.what());
    }
    gSnaps = std::make_unique<os2::AnimSnapView>(renderer, index, *gObjectView, gSimulation->machine());
    gSimulation->seats.slot = [](int16_t id, double now) { return gObjectView ? gObjectView->slotPlacement(id, now) : std::nullopt; };
    gSimulation->objectBone = [](int16_t id, uint32_t crc) -> std::optional<os2::Xform> {
        return gObjectView ? gObjectView->boneByHash(id, crc) : std::nullopt;
    };
    gSimulation->actorOf = [](int16_t id) -> const os2::SimActor* {
        const auto it = gObjectActors.find(id);
        return it != gObjectActors.end() ? it->second : nullptr;
    };
    gSimulation->onSimAnimation = [](int16_t id, os2::SimActor& actor, const os2::Animation* anim, double at) {
        gSnaps->simAnimation(id, actor, anim, at);
        if (anim)
            cinematicActorAnimates(id, at);
    };
    for (const auto& [id, p] : gPlacedObjects) {
        std::optional<os2::Xform> saved;
        if (!p.contained)
            saved = os2::Xform{os2::Quat{p.rotation[0], p.rotation[1], p.rotation[2], p.rotation[3]},
                               {p.position[0], p.position[1], p.position[2]}};
        gObjectView->addPlaced(id, p.definition, p.cres, p.instance, saved, p.materials, p.pose);
    }
    for (const auto& s : gLotSims)
        gObjectView->addSim(s.id, s.actor);
    perfLap("  object view");
    try {
        const fs::path lights = gameDir() / "TSData/Res/Lights";
        gLighting = std::make_unique<os2::LotLighting>(renderer, index, *gSimulation, *gObjectView,
                                                       os2::loadLightingConfig(lights / "Lighting.txt"),
                                                       os2::loadLightOverrides(lights / "LotLights.txt"));
        for (const auto& s : gLotSims)
            gLighting->addSim(s.id, s.actor);
        if (gLightMapLayout)
            gLighting->setLightMapLayout(*gLightMapLayout);
        // --lighting <state> keeps that state instead of the clock's.
        if (auto state = argument(@"--lighting"))
            gLighting->forceState(*state);
        // --lamps <0-100>: test aid, lamps Change Light has not touched shine at that level.
        if (auto lamps = argument(@"--lamps"))
            gLighting->setDefaultLampLevel(std::stoi(*lamps));
        // The environment motive's light (vm/environment.hpp): the floor light map under the Sim,
        // made before the first tick (the first 60-tick step comes on it: docs/motives-mac.md 8.5).
        gSimulation->machine().floorIllumination = [](int level, float x, float y) -> std::optional<float> {
            return gLighting ? gLighting->floorIllumination(level, x, y) : std::nullopt;
        };
        gLighting->update(0.0);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: lighting: %s\n", e.what());
    }
    perfLap("  LotLighting");
    // A Sim that changes clothes gets the outfit of its character file.
    gSimulation->onOutfitChange = [&renderer, &index](int16_t id, os2::SimActor& actor, uint32_t category) {
        const auto it = std::find_if(gLotSims.begin(), gLotSims.end(), [&](const LotSim& s) { return s.id == id; });
        if (it == gLotSims.end())
            return;
        try {
            actor.skin = os2::loadCharacterSim(index, simParts(index), it->character, category);
            actor.player.setSkeleton(&actor.skin.skeleton);
            const int model = renderer.addObject(actor.skin.model, index, true);
            renderer.retargetInstances(actor.model, model);
            actor.model = model;
            std::printf("opensims2: Sim %d now wears outfit category 0x%03X\n", id, category);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: Sim %d: outfit 0x%03X: %s\n", id, category, e.what());
        }
    };
    // The dialogs' $AspirationString and the newborns' names read Text/*.package (vm/dialog.hpp).
    os2::vm::textTables() = [](uint32_t group, uint32_t instance) -> const os2::StringTable* {
        try {
            if (!gTexts && gIndex)
                gTexts = std::make_unique<os2::UiResources>(*gIndex, gameDir() / "TSData/Res");
            return gTexts ? gTexts->table(group, instance) : nullptr;
        } catch (const std::exception& err) {
            os2::logOnce("text tables", std::to_string(group) + ":" + std::to_string(instance), err);
            return nullptr;
        }
    };
    // Babies born in play get their character files (vm/baby_character.hpp),
    // kept with the session's until the game is saved.
    if (const auto q = lotQuery())
        try {
            gLotCharactersDir = os2::vm::charactersDirOf(lotPath(*q));
            os2::vm::installBabyCharacterWriter(index, gLotCharactersDir, os2::sessionCharactersDir(), &gBornCharacters);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: babies' characters: %s\n", e.what());
        }
    // A Sim that grows up (the Age primitive) gets its character file grown
    // into the new age (cas::writeAgedCharacter: MakeNewAge's hair and
    // clothes, the face of the new age from its LxNR), written beside the
    // session's other files, and is rebuilt from it.
    gSimulation->onAgeChange = [&renderer, &index](int16_t id, os2::SimActor& actor, uint32_t ageBit) {
        const auto it = std::find_if(gLotSims.begin(), gLotSims.end(), [&](const LotSim& s) { return s.id == id; });
        if (it == gLotSims.end())
            return;
        try {
            auto& cas = gGameData.casCatalog(index); // (the game's data: for the app's life)
            const os2::cas::CasCatalog& catalog = *cas.catalog;
            os2::cas::FaceBaker& baker = *cas.baker;
            // The save copies it to the neighborhood; an NPC made in play
            // stays in the session's NPCs folder (vm/baby_character.hpp).
            const fs::path session = os2::sessionCharactersDir();
            const fs::path dir = it->character.parent_path() == session / "NPCs" ? session / "NPCs" : session;
            const fs::path out = dir / (it->character.stem().string() + ".package");
            const fs::path tmp = dir / (it->character.stem().string() + ".new.package");
            os2::cas::writeAgedCharacter(index, catalog, baker, it->character, ageBit, tmp, uint32_t(id) * 7919u + ageBit);
            fs::rename(tmp, out);
            it->character = out;
            actor.skin = os2::loadCharacterSim(index, simParts(index), it->character, 1);
            actor.player.setSkeleton(&actor.skin.skeleton);
            const int model = renderer.addObject(actor.skin.model, index, true);
            renderer.retargetInstances(actor.model, model);
            actor.model = model;
            std::printf("opensims2: Sim %d grew into age 0x%02X (%s)\n", id, ageBit, out.string().c_str());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: Sim %d: age 0x%02X: %s\n", id, ageBit, e.what());
        }
    };
    // Objects the VM animates (doors, fridges, phones, ...) swap their static
    // model for a skinned one of the same CRES, posed by the animation.
    auto& objectActor = gScene.objectActor; // (the scene's renderer: per scene)
    objectActor = [&renderer, &index](int16_t id) -> os2::SimActor* {
        auto it = gObjectActors.find(id);
        if (it == gObjectActors.end()) {
            const auto placed = gPlacedObjects.find(id);
            if (placed == gPlacedObjects.end())
                return nullptr; // no model in the world
            // Not drawn by the object view (out of the world: saved at
            // (0, 0) in no slot, as the PlumbBobs of an empty lot): an actor
            // would be drawn at the lot's corner, where nobody hides it
            // (live QA, N002_Lot23: a plumbob floating past the lot's edge).
            if (!gObjectView || !gObjectView->hasModel(id)) {
                if (std::getenv("OS2_DEBUG_VM"))
                    std::fprintf(stderr, "opensims2: object %d (%s) out of the world: not animated\n", id,
                                 placed->second.definition.c_str());
                return nullptr;
            }
            const PlacedObject& p = placed->second;
            try {
                const os2::ObjectModel model = os2::loadObjectModel(index, p.cres, gObjectView->materialsOf(id));
                os2::SkinnedModel skin(model.skeleton);
                skin.add(model);
                auto actor = std::make_unique<os2::SimActor>(std::move(skin));
                // From the pose the lot saved (a chair pushed in), and kept
                // where its animations leave it.
                if (p.pose.size() == actor->skin.skeleton.bones.size())
                    actor->player.setBasePose(p.pose);
                actor->player.setHoldPose(true);
                actor->position = {p.position[0], p.position[1], p.position[2]};
                actor->heading = 2 * std::atan2(p.rotation[2], p.rotation[3]);
                actor->model = renderer.addObject(actor->skin.model, index, true);
                renderer.addInstance(actor->model, matrix_identity_float4x4);
                gObjectView->setActor(id, actor.get()); // hides the static instance
                it = gObjectActors.emplace(id, actor.get()).first;
                gActors.push_back(std::move(actor));
            } catch (const std::exception& e) {
                std::fprintf(stderr, "opensims2: object %d: %s\n", id, e.what());
                gObjectActors.emplace(id, nullptr);
                return nullptr;
            }
        }
        return it->second;
    };
    if (const char* f = std::getenv("OS2_FORCE_OBJECT_ACTOR")) // test aid: this object animated from the start
        gScene.objectActor(int16_t(std::atoi(f)));
    // Once per name: the base game's trees ask for some the data lacks (the
    // double bed's "-", o2a-bookcase-get-middle: only the child's o2c-
    // exists) and the game shows nothing for them.
    static auto reportMissing = [](int16_t id, const std::string& name, const std::exception& e) {
        static std::set<std::string> reported; // (animation names: for the app's life)
        if (reported.insert(name).second)
            std::fprintf(stderr, "opensims2: object %d: %s: %s\n", id, name.c_str(), e.what());
    };
    gSimulation->onObjectAnimation = [&index](int16_t id, const std::string& name, double at) {
        if (gSnaps)
            gSnaps->objectAnimation(id, name, at);
        if (std::getenv("OS2_DEBUG_VM"))
            std::fprintf(stderr, "opensims2: object %d starts %s (%s)\n", id, name.c_str(),
                         gPlacedObjects.count(id) ? "placed" : "not placed");
        os2::SimActor* actor = gScene.objectActor(id);
        if (!actor)
            return;
        try {
            // Mirrored as the VM says (a chair pulled out to the side the Sim gets in from).
            const os2::vm::Object* vo = gSimulation ? gSimulation->machine().object(id) : nullptr;
            const bool mirror = vo && !vo->animation.overlays.empty() && vo->animation.overlays.back().name == name &&
                                vo->animation.overlays.back().flipped;
            actor->cue(at, actor->animation(index, name), {.fade = 0.1f, .loop = false, .mirror = mirror});
            if (std::getenv("OS2_DEBUG_VM"))
                std::fprintf(stderr, "opensims2: object %d plays %s\n", id, name.c_str());
        } catch (const std::exception& e) {
            reportMissing(id, name, e);
        }
    };
    // What the VM plays on an object, overlay by overlay, as VM::objectPose
    // poses it: each overlay its own layer (priority and level 1 + its id:
    // a later one takes the bones it keys whole), mirrored as the VM plays
    // it, and taken off when the VM takes it off, the object keeping the
    // pose of that moment (AnimPlayer::setHoldPose). A two-tile door's
    // leaves are one animation played twice, the second flipped: as one
    // layer replacing the other, only one leaf opened.
    gSimulation->onObjectOverlay = [&index](int16_t id, const std::string& name, bool flipped, uint32_t overlay,
                                            bool loop, double at) {
        if (gSnaps)
            gSnaps->objectAnimation(id, name, at);
        if (std::getenv("OS2_DEBUG_VM"))
            std::fprintf(stderr, "opensims2: object %d starts %s%s (overlay %u, %s%s) at %.2f\n", id, name.c_str(),
                         flipped ? " flipped" : "", overlay, gPlacedObjects.count(id) ? "placed" : "not placed",
                         loop ? ", looping" : "", at);
        os2::SimActor* actor = gScene.objectActor(id);
        if (!actor)
            return;
        try {
            const int layer = 1 + int(overlay & 0x3FFFFFFF);
            actor->cue(at, actor->animation(index, name),
                       {.fade = 0.1f, .loop = loop, .priority = layer, .mirror = flipped, .level = layer});
        } catch (const std::exception& e) {
            reportMissing(id, name, e);
        }
    };
    gSimulation->onObjectOverlayEnd = [](int16_t id, uint32_t overlay, double at) {
        if (std::getenv("OS2_DEBUG_VM"))
            std::fprintf(stderr, "opensims2: object %d ends overlay %u at %.2f\n", id, overlay, at);
        const auto it = gObjectActors.find(id);
        if (it != gObjectActors.end() && it->second)
            it->second->cueStop(at, 1 + int(overlay & 0x3FFFFFFF), 0);
    };
    // An animated object whose material the VM changes (the plumbob's
    // colour) gets its skinned model again with the new materials.
    gObjectView->onActorMaterials = [&renderer, &index](int16_t id) {
        const auto it = gObjectActors.find(id);
        if (it == gObjectActors.end() || !it->second)
            return;
        os2::SimActor& actor = *it->second;
        try {
            const os2::ObjectModel model = os2::loadObjectModel(index, gObjectView->cresOf(id), gObjectView->materialsOf(id));
            os2::SkinnedModel skin(model.skeleton);
            skin.add(model);
            actor.skin = std::move(skin);
            actor.player.setSkeleton(&actor.skin.skeleton);
            const int m = renderer.addObject(actor.skin.model, index, true);
            renderer.retargetInstances(actor.model, m);
            actor.model = m;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: object %d: %s\n", id, e.what());
        }
    };
    gDoors = std::make_unique<os2::DoorPortals>(
        [](int16_t id, const std::string& name, double at) {
            if (gSimulation && gSimulation->onObjectAnimation)
                gSimulation->onObjectAnimation(id, name, at);
        },
        [](int16_t id) -> std::optional<os2::Xform> {
            if (gObjectView && gObjectView->hasModel(id))
                return gObjectView->placement(id);
            const auto p = gPlacedObjects.find(id); // a door the VM does not have: where it was saved
            if (p == gPlacedObjects.end() || p->second.contained)
                return std::nullopt;
            const auto& q = p->second.rotation;
            return os2::Xform{os2::Quat{q[0], q[1], q[2], q[3]}, {p->second.position[0], p->second.position[1], p->second.position[2]}};
        });
    for (const auto& [id, p] : gPlacedObjects)
        if (p.definition.rfind("Door", 0) == 0 && !p.contained)
            gDoors->addDoor(id);
    // --object-anim <id>,<animation>: test aid, an object plays an animation
    // as if the VM's Animate Object had started it.
    if (auto anim = argument(@"--object-anim")) {
        const auto comma = anim->find(',');
        if (comma == std::string::npos)
            throw std::runtime_error("--object-anim: expected <object-id>,<animation>");
        gSimulation->onObjectAnimation(int16_t(std::stoi(anim->substr(0, comma))), anim->substr(comma + 1), 0);
    }
    // --look-at <sim>,<object-id>: test aid, the Sim (ID or name) looks at
    // the object as if the VM's Look At had pointed it there, until changed.
    if (auto look = argument(@"--look-at")) {
        const auto comma = look->find(',');
        if (comma == std::string::npos)
            throw std::runtime_error("--look-at: expected <sim>,<object-id>");
        const os2::vm::ObjectId sim = os2::vm::findSim(gSimulation->machine(), look->substr(0, comma));
        if (auto* o = gSimulation->machine().object(sim)) {
            o->lookAt = os2::vm::Object::LookAt{int16_t(std::stoi(look->substr(comma + 1))), 0, 0};
            o->lookAt->serial = ++o->lookAtSerial;
        }
        else
            throw std::runtime_error("--look-at: no Sim " + look->substr(0, comma));
    }
    // --outfit <type>: test aid, every Sim changes into outfit type <type>
    // (person data 8, STR# 0xFA: 0 everyday, 2 swimwear, 3 athletic, 4
    // formal, 5 pyjamas, 11 underwear, ...).
    if (auto outfit = argument(@"--outfit"))
        for (const auto& s : gLotSims)
            if (auto* o = gSimulation->machine().object(s.id); o && o->person)
                o->person->data[8] = int16_t(std::stoi(*outfit));
    if (auto walk = argument(@"--vm-walk")) {
        int sim = 0, target = 0, anim = -1;
        if (std::sscanf(walk->c_str(), "%d,%d,%d", &sim, &target, &anim) < 2)
            throw std::runtime_error("--vm-walk: expected <sim-id>,<object-id>[,<anim>]");
        gSimulation->walk(int16_t(sim), int16_t(target),
                          anim >= 0 ? std::optional<uint16_t>(uint16_t(anim)) : std::nullopt);
    }
    std::printf("opensims2: VM: %s, %zu Sims attached (%.2f s)\n", gSimulation->summary().c_str(), gLotSims.size(),
                CACurrentMediaTime() - start);
    // --vm-create GUID[:attr=value,...]: test aid, an object made out of the
    // world (vm::createTestObject; a party controller 0x2C91C16A:2=1 makes
    // the birthday play its cinematic).
    // "...@x,y[,level]" puts it on that tile (the debug tombstone's "New Baby" places the baby beside it).
    // Repeatable (a baby's tombstone and a crib). A final "!" starts its
    // Main (a Fire 0x24C95F99@x,y,level! burns, wakes the room's smoke alarm).
    std::vector<std::string> creates;
    {
        NSArray<NSString*>* a = sceneArgs();
        for (NSUInteger i = 1; i + 1 < a.count; ++i)
            if ([a[i] isEqualToString:@"--vm-create"])
                creates.push_back(a[i + 1].UTF8String);
    }
    for (const std::string& specText : creates) {
        const std::string* spec = &specText;
        const auto at = spec->find('@');
        const os2::vm::ObjectId id = os2::vm::createTestObject(gSimulation->machine(), spec->substr(0, at));
        int x = 0, y = 0, level = 1;
        std::string where = "out of the world";
        if (at != std::string::npos && std::sscanf(spec->c_str() + at + 1, "%d,%d,%d", &x, &y, &level) >= 2) {
            const os2::vm::Placement p = os2::vm::checkPlacement(gSimulation->machine(), id, {x, y, level}, 0);
            if (p.ok)
                os2::vm::applyPlacement(gSimulation->machine(), id, p), where = "placed";
            else
                where = "not placed: " + p.reason;
        }
        // "...!" at the end: its Main starts once placed (a Fire that burns).
        if (!spec->empty() && spec->back() == '!' && gSimulation->machine().startMain(id))
            where += ", Main started";
        std::printf("opensims2: created object %d (%s)\n", id, where.c_str());
    }
    // --vm-queue <object>:<entry>: test aid, the selected Sim (--sim-id) queues
    // the pie entry (English name, debug and cheat entries too) of an object
    // found by ID or name, also one out of the world (the debug tombstone
    // --vm-create 0x0CB53A3A: "New Baby", "CINECAM BABY", "Age Transition Cinematic").
    if (auto spec = argument(@"--vm-queue")) {
        os2::vm::VM& m = gSimulation->machine();
        const auto colon = spec->find(':');
        const std::string what = spec->substr(0, colon), entry = colon == std::string::npos ? "" : spec->substr(colon + 1);
        os2::vm::ObjectId target = 0;
        for (const auto& [id, o] : m.objects())
            if (!o.person && o.definition &&
                (what == std::to_string(id) || strcasestr(o.definition->fileName.c_str(), what.c_str())))
                target = target ? target : id;
        const os2::vm::ObjectId sim = os2::vm::findSim(m, argument(@"--sim-id").value_or(""));
        os2::vm::PieOptions opt;
        opt.language = os2::lang::EnglishUS;
        bool queued = false;
        if (target && sim)
            for (const auto& e : os2::vm::pieMenu(m, sim, target, opt))
                if (!queued && strcasecmp(e.name.c_str(), entry.c_str()) == 0)
                    if (auto q = os2::vm::interactionFromPie(m, e)) {
                        os2::vm::addAction(m, *m.object(sim), *q);
                        queued = true;
                    }
        std::printf("opensims2: --vm-queue %s: %s\n", spec->c_str(), queued ? "queued" : "no such entry");
    }
    // --vm-go-here x,y[,level]: test aid, a click on the ground there and the
    // selected Sim's first Go Here entry ("Vir Aqui") queued (vm/go_here.hpp).
    if (auto spec = argument(@"--vm-go-here")) {
        os2::vm::VM& m = gSimulation->machine();
        float x = 0, y = 0;
        int level = 1;
        std::sscanf(spec->c_str(), "%f,%f,%d", &x, &y, &level);
        const os2::vm::ObjectId sim = os2::vm::findSim(m, argument(@"--sim-id").value_or(""));
        const os2::vm::ObjectId dest = sim ? os2::vm::createGoHereDestination(m, sim, x, y, level) : 0;
        bool queued = false;
        if (dest)
            for (const auto& e : os2::vm::pieMenu(m, sim, dest))
                if (!queued && e.available() && !e.debug)
                    if (auto q = os2::vm::interactionFromPie(m, e)) {
                        q->pushedBy = sim;
                        os2::vm::enqueue(m, *m.object(sim), *q);
                        queued = true;
                    }
        if (dest)
            os2::vm::releaseDestination(m, dest);
        std::printf("opensims2: --vm-go-here %s: %s\n", spec->c_str(), queued ? "queued" : "nothing");
    }
    // --motives <n>: test aid, every Sim's motives at n (instead of the saved ones).
    if (auto m = argument(@"--motives"))
        os2::vm::setAllMotives(gSimulation->machine(), int16_t(std::stoi(*m)));
    if (std::getenv("OS2_TRACE_VM")) // test aid: every instruction run (group:tree#pc opcode, stack object)
        gSimulation->machine().trace = [](const os2::vm::Thread&, const os2::vm::Frame& f, const os2::Behavior::Instruction& in) {
            std::fprintf(stderr, "trace: %08X:%04X #%u op 0x%X (stack object %d)\n", f.group, f.instance, f.pc, in.opcode, f.stackObject);
        };
    // Pie menus over the same VM.
    gPie = std::make_unique<os2::PieMenuHost>(gSimulation->machine(), gameDir() / "TSData/Res/UI/Fonts",
                                              argument(@"--sim-id").value_or(""));
    // A go-here destination goes when its menu closes, unless an interaction on it was queued.
    gPie->onClose = [](os2::vm::ObjectId id) { os2::vm::releaseDestination(gPie->vm(), id); };
    gPie->simAway = [](os2::vm::ObjectId sim) { return !simInTheWorld(int16_t(sim)); };
    for (const auto& b : gPickBoxes)
        if (b.contained)
            gPie->addFollowingPickBox(b.instance, b.lo, b.hi, b.mesh);
        else
            gPie->addPickBox(b.instance, b.lo, b.hi, b.at, b.mesh);
    // Objects in slots are picked where the VM view draws them.
    gPie->setPlacementSource([](os2::vm::ObjectId id) -> std::optional<simd_float4x4> {
        if (!gObjectView || !gObjectView->hasModel(id))
            return std::nullopt;
        const auto x = gObjectView->placement(id);
        if (!x)
            return std::nullopt;
        simd_float4x4 m = simd_matrix4x4(simd_quaternion(x->q.x, x->q.y, x->q.z, x->q.w));
        m.columns[3] = simd_make_float4(x->t[0], x->t[1], x->t[2], 1);
        return m;
    });
    // Objects the view hides (windows and doors of walls cut down) are not picked.
    gPie->drawn = [](os2::vm::ObjectId id) { return !gObjectView || gObjectView->shown(int16_t(id)); };
    // A floor shown stops the ray (a Sim under the floor shown is not picked).
    gPie->floorHit = [](simd_float3 eye, simd_float3 dir) { return floorUnderRay(eye, dir); };
    // The levels above the camera's are not drawn: their objects are not picked.
    gPie->pickable = [](simd_float3 lo, simd_float3) {
        return !gLotShape || os2::WallView::visible(*gLotShape, viewLevel(), lo.x, lo.y, lo.z, gWallView.lowestCut(viewLevel()));
    };
    // What is hidden with the wall it is on when that wall is cut down:
    // nTSSG::cWallManager::HideShowAttachedObjects (Mac BV 0x6c2ee4) hides an
    // object attached to the wall when cTSObject::HideWhenWallDown (BV
    // 0xaff050: a window, OBJD type 9, or object data 8 "flags" bit 0x400,
    // FreeSO's HideForCutaway, which the object's own trees set: windows,
    // doors, gates, wall lamps, paintings, ceiling lamps...), unless its
    // wall placement flags (od 13) have 0x1000 (on a fence), and, in Build
    // mode (play mode 4), not doors and windows (IsWallPortal): there they
    // stay, the wall cut down round them. What the hand holds is not hidden
    // (cTSUserToolUndoableObject clears the flag while held,
    // Suppress/RestoreHideWhenWallDownFlag, BV 0x35ceb0). Which wall an
    // object hangs on is onCutWall's geometry (inferred).
    // Nothing else hides: the wall mirrors, the wall phone and the smoke
    // alarm stay on a cut wall, as in the game (MirrorGlobals' "Function -
    // Init", group 0x7F84A9F4 tree 0x2000, clears the 0x400 bit; paintings,
    // wall lamps and ceiling lamps set it in theirs; docs/buy-build-mac.md
    // §7.6.1).
    gObjectView->visible = [](int16_t id, const os2::Xform& at) {
        if (!gLotShape)
            return true;
        if (!os2::WallView::visible(*gLotShape, viewLevel(), at.t[0], at.t[1], at.t[2], gWallView.lowestCut(viewLevel())))
            return false;
        const os2::vm::Object* o = gSimulation ? gSimulation->machine().object(id) : nullptr;
        if (!o || !o->definition)
            return true;
        if ((gBuy && gBuy->held() == os2::vm::ObjectId(id)) || (gBuild && gBuild->held() == os2::vm::ObjectId(id)))
            return true;
        const uint16_t type = o->definition->objectType();
        if (static const bool debug = std::getenv("OS2_DEBUG_WALL_HIDE") != nullptr; debug) {
            static std::set<int16_t> told; // OS2_DEBUG_WALL_HIDE=1: each object's HideWhenWallDown inputs, once
            if (told.insert(id).second)
                std::printf("opensims2: wall hide: object %d (%s) type %u flags 0x%04X wall flags 0x%04X\n", id,
                            o->definition->fileName.c_str(), unsigned(type), unsigned(uint16_t(o->data[os2::vm::objdata::Flags])),
                            unsigned(uint16_t(o->data[os2::vm::objdata::WallPlacementFlags])));
        }
        if (type != os2::vm::kObjectTypeWindow && !(o->data[os2::vm::objdata::Flags] & 0x400))
            return true;
        // Attached to no wall: the walls an object hangs on come from its
        // wall placement flags (cTSObject vtable 0x274/0x284 from 0x25c, read
        // by 0x26c in HideShowAttachedObjects); none asked for (0x5F) -> never
        // hidden (the ceiling lamps carry the flag too). On a fence: 0x1000.
        const int16_t wallFlags = o->data[os2::vm::objdata::WallPlacementFlags];
        if (!(wallFlags & 0x5F) || (wallFlags & 0x1000))
            return true;
        if (gBuild && gBuild->active() && (type == os2::vm::kObjectTypeDoor || type == os2::vm::kObjectTypeWindow))
            return true;
        const bool cut = onCutWall(at);
        if (static const bool debug = std::getenv("OS2_DEBUG_WALL_HIDE") != nullptr; debug) {
            static std::map<int16_t, bool> told;
            if (auto [it, fresh] = told.emplace(id, cut); fresh || it->second != cut) {
                it->second = cut;
                std::printf("opensims2: wall hide: object %d at (%.2f, %.2f, %.2f) %s\n", id, at.t[0], at.t[1], at.t[2],
                            cut ? "hidden (its wall is cut down)" : "shown");
            }
        }
        return !cut;
    };
    perfLap("  VM hooks");
    applySavedUiState(pkg, lot);
    setupPieMenu(index);
    perfLap("  pie menu");
    if (liveUiRequested())
        setupLiveUi(index);
    perfLap("  live UI");
    if (liveUiRequested() || startMode()) {
        setupBuyMode(renderer, index);
        perfLap("  Buy mode");
        setupBuildMode(renderer, index);
        perfLap("  Build mode");
    }
    if (liveUiRequested())
        setupOptions(index); // below: the puck's Options mode
    if (liveUiRequested())
        setupTutorial(index); // below: a lot of the Tutorial neighborhood
    // --game-speed <2|3|4>: the game speed global (2 normal, 3 fast, 4 fastest), as the live UI's speed buttons.
    if (auto sp = argument(@"--game-speed"))
        gSimulation->machine().globals[os2::vm::global::Speed] = int16_t(std::clamp(std::stoi(*sp), 2, 4));
    // --dialog-answer <n>: every dialog the trees put up gets button n at once.
    if (auto a = argument(@"--dialog-answer"))
        gSimulation->machine().autoDialogAnswer = std::stoi(*a);
    // --dialog-demo <type>[:<title>[:<message>]]: test aid, puts a dialog of that Dialog type up for the
    // selected Sim, to see the window (answer it with --dialog-click / --dialog-type, or keys): 0 message,
    // 1 yes/no, 2 yes/no/cancel, 3 text entry ("0i": a message with the Sim's icon, "0c": with a close box),
    // 23 baby name ("23g" a girl, "23t" twins, "23r" a rename), 5 services / 6 invite / 7 party phone books
    // over the lot's objects and neighbors, 17 the wardrobe ("17f" formal wear), 16 the clothing rack
    // ("16f" formal), 20 the try-on, 21 the vanity.
    if (auto spec = argument(@"--dialog-demo"); spec && gPie) {
        os2::vm::VM& m = gSimulation->machine();
        os2::vm::Dialog d;
        d.id = 0x7000;
        d.me = gPie->sim();
        d.type = std::atoi(spec->c_str());
        d.cls = os2::vm::dialogClass(d.type);
        std::vector<std::string> parts{""};
        for (char ch : *spec)
            if (ch == ':')
                parts.emplace_back();
            else
                parts.back().push_back(ch);
        const std::string kind = parts[0];
        d.title = parts.size() > 1 ? parts[1] : "";
        d.message = parts.size() > 2 ? parts[2] : "Isto é uma mensagem de teste do diálogo, longa o bastante para quebrar em mais de uma linha na janela.";
        using namespace os2::vm::dialogtype;
        if (d.type == YesNo)
            d.buttons = {"Sim", "Não"};
        else if (d.type == YesNoCancel)
            d.buttons = {"Sim", "Não", "Cancelar"};
        else if (d.cls.cls == os2::vm::DialogClass::Object)
            d.buttons = {"OK"};
        else
            d.buttons = {"OK", "Cancelar"};
        if (kind == "0i")
            d.iconObject = gPie->sim();
        if (kind == "0c")
            d.continueOthers = true;
        if (d.cls.cls == os2::vm::DialogClass::SelectItems) { // 11/13: a rack of the lot's first objects, several picks
            d.multiSelect = d.obeyPrice = true;
            d.fillFunds = 300;
            for (const auto& [oid, o] : m.objects())
                if (!o.person && o.definition && o.definition->price() > 0 && d.entries.size() < 7) {
                    os2::vm::DialogEntry e;
                    e.id = oid, e.guid = o.guid(), e.name = os2::vm::displayName(m, o), e.price = o.definition->price();
                    d.entries.push_back(std::move(e));
                }
        }
        if (d.cls.cls == os2::vm::DialogClass::Fill) { // 14: buying food (40 of 200 units, §20 per 50, §50 delivery)
            d.fillUnits = d.fillValue = 40, d.fillCost = 20, d.fillDelivery = 50, d.fillMax = 200;
            d.fillFunds = 1000;
        }
        if (d.cls.cls == os2::vm::DialogClass::SetAspiration) { // 24: the selected Sim's aspiration
            if (const os2::vm::Object* o = m.object(gPie->sim()); o && o->person) {
                d.aspirationNeighbor = o->person->data[os2::vm::persondata::NeighborId];
                d.female = o->person->data[65] == 1;
                d.text = os2::vm::displayName(m, *o);
            }
        }
        if (d.cls.cls == os2::vm::DialogClass::Clothing) { // 17: the wardrobe, everyday clothes ("17f": formal)
            d.outfitSim = gPie->sim();
            d.outfitCategory = kind == "17f" ? 0x020 : 0x001;
        }
        if (d.cls.cls == os2::vm::DialogClass::ClothingPurchase || d.cls.cls == os2::vm::DialogClass::ClothingTryOn) {
            // 16: the clothing rack (everyday, §150 an item; "16f": formal), 20: the try-on.
            d.outfitSim = gPie->sim();
            d.outfitCategory = kind == "16f" || kind == "20f" ? 0x020 : 0x001;
            d.outfitSeparates = d.outfitCategory == 0x001;
            d.pricePerItem = 150;
            d.fillFunds = 2000;
            if (const os2::vm::Object* o = m.object(gPie->sim()); o && o->person)
                if (auto f = m.families.find(o->person->data[os2::vm::persondata::FamilyNumber]); f != m.families.end())
                    d.fillFunds = f->second.funds;
        }
        if (d.cls.cls == os2::vm::DialogClass::Vanity) // 21: the mirror's "Change Appearance"
            d.outfitSim = gPie->sim();
        if (d.type == BabyName) {
            const bool girl = kind == "23g";
            d.names.push_back({0, girl ? "Bebezinha" : "Bebezinho", girl});
            if (kind == "23t")
                d.names.push_back({0, "Bebezinha", true});
            d.namePanel = kind == "23r" ? 0xCDC2CAA4 : d.names.size() > 1 ? 0xCDC2CAA6 : 0xCDC2CAA5;
            d.message.clear();
        }
        if (d.cls.cls == os2::vm::DialogClass::PhoneServices) {
            d.message.clear();
            for (const auto& [oid, o] : m.objects())
                if (!o.person && o.definition && d.entries.size() < 8) {
                    os2::vm::DialogEntry e;
                    e.id = oid;
                    e.name = os2::vm::displayName(m, o);
                    if (!e.name.empty())
                        d.entries.push_back(std::move(e));
                }
            if (!d.entries.empty())
                d.selection = {d.entries[0].id};
        }
        if (d.cls.cls == os2::vm::DialogClass::PhoneNeighbors || d.cls.cls == os2::vm::DialogClass::PhoneParty) {
            d.message.clear();
            // The neighbors a call list could hold: ones with a family (the
            // hood's ancestors, family 0, are not callable: N002 has three
            // "Mãe Rainha" of them), not the caller's own family; the
            // relationship as setupPhone reads it (the trees fill the real lists).
            const os2::vm::Object* me = m.object(gPie->sim());
            const int16_t myFamily = me && me->person ? me->person->data[os2::vm::persondata::FamilyNumber] : 0;
            for (const auto& [id, n] : m.neighbors) {
                const int16_t family = n.personData[os2::vm::persondata::FamilyNumber];
                if (n.firstName.empty() || d.entries.size() >= 9 || !family || family == myFamily ||
                    !m.families.count(family))
                    continue;
                os2::vm::DialogEntry e;
                e.id = id;
                e.guid = n.guid;
                e.first = n.firstName, e.last = n.lastName;
                e.name = n.lastName.empty() ? n.firstName : n.firstName + " " + n.lastName;
                const auto key = std::make_pair(m.relationshipKey(d.me), os2::vm::VM::RelationshipKey{1, id});
                if (auto r = m.relationships.find(key); r != m.relationships.end()) {
                    const auto& vars = r->second;
                    e.relationship = (vars.size() > 0 ? vars[0] : 0) + (vars.size() > 2 ? vars[2] : 0);
                    e.flags = vars.size() > 1 ? (vars[1] & 0xFFFF) : 0;
                }
                d.entries.push_back(std::move(e));
            }
            if (d.cls.cls == os2::vm::DialogClass::PhoneParty) {
                d.maxSelection = 10;
                if (d.entries.size() > 2)
                    d.selection = {d.entries[1].id, d.entries[2].id};
            } else if (!d.entries.empty()) {
                d.selection = {d.entries[0].id};
            }
        }
        m.dialogs[d.me] = std::move(d);
    }
    try {
        const auto plumbob = os2::loadObjectModel(index, "plumbob");
        gPlumbob = renderer.addInstance(renderer.addObject(plumbob, index), matrix_identity_float4x4);
        gPlumbobBottom = os2::modelBounds(plumbob).first.z;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: plumbob: %s\n", e.what());
    }
    // The Sim the player controls is the VM's "selected person ID" (global
    // 3), as when the game selects a Sim: the game's PlumbBob follows it.
    if (gPie->sim())
        gSimulation->machine().globals[os2::vm::global::SelectedPersonId] = gPie->sim();
    const os2::vm::Object* sim = gPie->vm().object(gPie->sim());
    std::printf("opensims2: pie menus for Sim %d (%s)\n", gPie->sim(),
                sim && sim->definition ? sim->definition->fileName.c_str() : "none");
}

} // namespace os2app
