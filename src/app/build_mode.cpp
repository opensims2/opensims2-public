#include "app/build_mode.hpp"
#include "formats/log_once.hpp"
#include "app/build_mode_internal.hpp"

#include "app/design_tool.hpp"

#include "app/lot_vm.hpp"
#include "app/vm_objects.hpp"
#include "formats/font.hpp"
#include "formats/image.hpp"
#include "formats/lot.hpp"
#include "formats/model.hpp"
#include "formats/rcol.hpp"
#include "formats/resindex.hpp"
#include "formats/txmt.hpp"
#include "formats/ui_resources.hpp"
#ifdef OS2_SDL_GPU
#include "render/gpu_renderer.hpp"
#else
#include "render/renderer.hpp"
#include "render/ui_layer.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace os2 {

namespace {

constexpr uint32_t kPanelScript = 0x49060003, kPuckScript = 0x4905F85B, kDescriptionScript = 0x49063004;

// A flat unit square, alpha blended and unlit: the previews.
ObjectModel quadModel(const std::string& name, const char* rgb) {
    ObjectModel m;
    m.name = name;
    ObjectModel::Part part;
    part.node = part.shape = name;
    part.material = name;
    part.mesh.positions = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    part.mesh.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    part.mesh.uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    part.mesh.triangles = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
    m.parts.push_back(std::move(part));
    MaterialDefinition md;
    md.materialType = "StandardMaterial";
    md.properties = {{"stdMatDiffCoef", rgb},          {"stdMatEmissiveCoef", rgb},  {"stdMatUntexturedDiffAlpha", "0.5"},
                     {"stdMatAlphaBlendMode", "blend"}, {"stdMatLightingEnabled", "false"},
                     {"stdMatCullMode", "none"},        {"stdMatLayer", "1"}};
    m.materials[name] = md;
    return m;
}

} // namespace

BuildMode::BuildMode(LotSimulation& simulation, VmObjectView& view, Renderer& renderer, ResourceIndex& index,
                     const std::filesystem::path& resDir, Lot& lot)
    : sim_(simulation), vm_(simulation.machine()), view_(view), renderer_(renderer), index_(index), lot_(lot),
      res_(std::make_unique<UiResources>(index, resDir)), fonts_(std::make_unique<FontLibrary>(resDir / "UI/Fonts")),
      patterns_(BuildCatalog::load(index)), objects_(Catalog::load(index, true)) {
    objects_.setThumbnailPackage(resDir / "UserData/Thumbnails/ObjectThumbnails.package");
    editor_ = std::make_unique<vm::BuildEditor>(vm_, lot_, &patterns_);
    editor_->onForget = [this](vm::ObjectId part) { view_.forget(part); };
    design_ = std::make_unique<DesignTool>(vm_, view_, index_, *res_, *fonts_);
    design_->say = [this](const std::string& m) { say(m); };
    design_->playSound = [this](const std::string& n) { sound(n); };
    design_->onAccepted = [this](const DesignTool::Change& c) {
        syncHistory();
        Entry e;
        e.kind = Entry::Kind::Design, e.id = c.id, e.cost = c.cost, e.before = c.before, e.after = c.after;
        undo_.push_back(std::move(e));
        recorded();
        dirty_ = true;
    };
    auto load = [&](uint32_t id, UiScript& into) {
        if (auto s = res_->script(id)) {
            into = std::move(*s);
            return !into.roots.empty();
        }
        std::fprintf(stderr, "opensims2: build mode: no UI script %08X\n", id);
        return false;
    };
    ok_ = load(kPanelScript, panel_) && load(kPuckScript, puck_) && load(kDescriptionScript, description_);
    try {
        previewModels_[0] = renderer_.addObject(quadModel("os2_build_ok", "0.25,1,0.35"), index_);
        previewModels_[1] = renderer_.addObject(quadModel("os2_build_no", "1,0.15,0.1"), index_);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: build mode: %s\n", e.what());
    }
}

BuildMode::~BuildMode() = default;

void BuildMode::redraw() {
    sim_.updateGeometry(lot_);
    if (onGeometryChanged)
        onGeometryChanged();
}

void BuildMode::say(const std::string& m) {
    message_ = m;
    std::printf("opensims2: build mode: %s\n", m.c_str());
    std::fflush(stdout);
    dirty_ = true;
}

void BuildMode::enter() {
    if (active_)
        return;
    active_ = true;
    savedPaused_ = vm_.globals[vm::global::Paused];
    vm_.globals[vm::global::Paused] = 1;
    level_ = vm_.world ? vm_.world->firstLevel() : 1;
    if (vm_.world && vm_.world->firstLevel() < 1)
        level_ = 1;
    if (const vm::Object* s = vm_.object(vm_.globals[vm::global::SelectedPersonId]); s && s->position.inWorld())
        level_ = s->position.level;
    say("entered (" + std::to_string(patterns_.floors().size()) + " floors, " + std::to_string(patterns_.walls().size()) +
        " wall coverings, " + std::to_string(objects_.entries().size()) + " build objects; funds " +
        money(vm::funds(vm_)) + ")");
}

void BuildMode::exit() {
    if (!active_)
        return;
    cancel();
    stopBrushLoop();
    design_->cancel();
    // The game empties its one undo manager on entering Live mode, not on
    // leaving Build mode (docs/buy-build-mac.md §7.24): with the shared
    // order the host flushes it then.
    if (!undoOrder)
        flushHistory();
    vm_.globals[vm::global::Paused] = savedPaused_;
    active_ = false;
    tool_ = Tool::None;
    update();
    say("left");
}

void BuildMode::flushHistory() {
    editor_->resetHistory([this](vm::ObjectId part) { view_.forget(part); });
    // Objects whose purchase was undone die with the history (ResetSelf).
    dropRedo();
    undo_.clear();
    editorDepth_ = 0;
}

void BuildMode::dropRedo() {
    editor_->dropRedo();
    for (const Entry& e : redo_)
        if (e.kind == Entry::Kind::Object)
            if (const vm::Object* o = vm_.object(e.id); o && !o->position.inWorld()) {
                for (vm::ObjectId part : vm::groupOf(vm_, e.id))
                    view_.forget(part);
                vm::deleteGroup(vm_, e.id);
            }
    redo_.clear();
}

void BuildMode::recorded() {
    dropRedo();
    if (undoOrder)
        undoOrder->pushed(UndoOrder::Build);
}

// --- Tools -------------------------------------------------------------------

void BuildMode::selectWallTool(bool room) {
    cancel();
    tool_ = room ? Tool::Room : Tool::Wall;
    pattern_ = nullptr, object_ = nullptr, fence_ = nullptr;
    say(room ? "room tool" : "wall tool");
}

void BuildMode::selectFence(const BuildFence& f) {
    cancel();
    tool_ = Tool::Fence;
    fence_ = &f;
    pattern_ = nullptr, object_ = nullptr;
    say("fence " + f.name + " (" + money(f.price) + " a section)");
}

void BuildMode::selectFoundationTool(int product) {
    cancel();
    tool_ = Tool::Foundation;
    deckProduct_ = product;
    pattern_ = nullptr, object_ = nullptr, fence_ = nullptr;
    static const int prices[3] = {kFoundationTilePrice, kLatticeDeckTilePrice, kColumnDeckTilePrice};
    say(std::string(product ? "deck" : "foundation") + " tool (" + money(prices[std::clamp(product, 0, 2)]) +
        " a tile)");
}

void BuildMode::selectTerrainTool(vm::BuildEditor::TerrainOp op, const BuildTerrainPaint* paint) {
    cancel();
    tool_ = Tool::Terrain;
    terrainOp_ = paint ? vm::BuildEditor::TerrainOp::Paint : op;
    paint_ = paint;
    pattern_ = nullptr, object_ = nullptr, fence_ = nullptr;
    static const char* const names[] = {"raise", "lower", "flatten", "smooth", "paint", "erase paint", "water"};
    say(std::string("terrain tool: ") + names[int(terrainOp_)] + (paint ? " " + paint->name : std::string()));
}

void BuildMode::selectRoofTool(uint32_t type) {
    cancel();
    tool_ = Tool::Roof;
    roofType_ = type;
    pattern_ = nullptr, object_ = nullptr, fence_ = nullptr, paint_ = nullptr;
    say("roof tool, type " + std::to_string(type));
}

void BuildMode::selectPoolTool() {
    cancel();
    tool_ = Tool::Pool;
    pattern_ = nullptr, object_ = nullptr, fence_ = nullptr, paint_ = nullptr;
    say("pool tool (" + money(kPoolTilePrice) + " a tile)");
}

void BuildMode::selectStairsTool(const BuildStairs& product) {
    cancel();
    tool_ = Tool::Stairs;
    stairs_ = &product;
    pattern_ = nullptr, object_ = nullptr, fence_ = nullptr, paint_ = nullptr;
    say(product.name + " (" + money(product.price) + " a step)");
}

void BuildMode::selectRailingTool() {
    cancel();
    tool_ = Tool::Railing;
    pattern_ = nullptr, object_ = nullptr, fence_ = nullptr, paint_ = nullptr;
    say("railing tool (" + money(vm_.railingPrice ? vm_.railingPrice : patterns_.railingPrice()) + " a step)");
}

void BuildMode::selectDormerTool(const BuildRoofProduct& product) {
    cancel();
    tool_ = Tool::Dormer;
    dormer_ = &product;
    pattern_ = nullptr, object_ = nullptr, fence_ = nullptr, paint_ = nullptr;
    say(product.name + " (" + money(product.price) + ")");
}

void BuildMode::selectFireplaceTool(const BuildFireplace& product) {
    cancel();
    const auto def = vm_.code().definitionByName(product.mantle);
    if (!def) {
        say(product.name + ": no object \"" + product.mantle + "\"");
        return;
    }
    try {
        held_ = vm::createGhost(vm_, def->first, def->second);
    } catch (const std::exception& ex) {
        say(product.name + ": " + ex.what());
        held_ = 0;
        return;
    }
    tool_ = Tool::Fireplace;
    fireplace_ = &product;
    pattern_ = nullptr, object_ = nullptr, fence_ = nullptr, paint_ = nullptr;
    last_ = {};
    say("holding " + product.name + " (" + money(product.price) + ")");
}

bool BuildMode::placeFireplaceAt(int x, int y, int direction) {
    if (!held_ || !fireplace_)
        return false;
    holdObjectAt(x, y, direction, true);
    const vm::ObjectId id = held_;
    const auto r = editor_->placeFireplace(*fireplace_, id, {x, y, level_}, direction_);
    if (!r.ok) {
        sound("ui_error");
        applied(r);
        holdObjectAt(x, y, direction_); // shown where it would go again
        return false;
    }
    sound(objectSound(id));
    held_ = 0;
    applied(r);
    syncAfterObject();
    selectFireplaceTool(*fireplace_); // another of the same (inferred: as the object tool keeps buying)
    return true;
}

BuildMode::StairsDrag BuildMode::stairsDrag(simd_float2 from, simd_float2 to) const {
    StairsDrag d;
    d.top = {int(std::floor(from.x)), int(std::floor(from.y)), level_};
    const float cx = float(d.top.x) + 0.5f, cy = float(d.top.y) + 0.5f;
    const bool dragged = int(std::floor(to.x)) != d.top.x || int(std::floor(to.y)) != d.top.y;
    // Off the side of the tile the mouse is on (the floor tile's quarter,
    // cFloorTile +0xC: 1 -> -x, 2 -> +y, 4 -> +x, 8 -> -y), or the way of the drag.
    const float dx = (dragged ? to.x : from.x) - cx, dy = (dragged ? to.y : from.y) - cy;
    if (std::fabs(dx) >= std::fabs(dy))
        d.direction = dx < 0 ? 0 : 4;
    else
        d.direction = dy > 0 ? 2 : 6;
    if (dragged)
        d.end = std::array<float, 2>{to.x, to.y};
    return d;
}

void BuildMode::selectPattern(const BuildPattern& p) {
    cancel();
    tool_ = p.floor ? Tool::Floor : Tool::Wallpaper;
    pattern_ = &p;
    object_ = nullptr, fence_ = nullptr;
    say(std::string(p.floor ? "floor " : "wall covering ") + p.name + " (" + money(p.price) + ")");
}

bool BuildMode::selectObject(const CatalogEntry& e) {
    cancel();
    try {
        held_ = vm::createGhost(vm_, e.group, e.instance);
    } catch (const std::exception& ex) {
        say(e.name + ": " + ex.what());
        held_ = 0;
        return false;
    }
    tool_ = Tool::Object;
    object_ = &e;
    pattern_ = nullptr, fence_ = nullptr;
    last_ = {};
    // TryGrab: the twin of a door or window, for the diagonal walls.
    swapped_ = false;
    if (const auto twin = vm::diagonalTwin(vm_, held_); twin && vm_.code().index()) {
        twin_ = vm::createDiagonalTwin(vm_, held_);
        twinEntry_ = catalogTwin(e, *vm_.code().index(), twin->first, twin->second);
    }
    say("holding " + e.name + " (" + money(e.price) + ")");
    return true;
}

void BuildMode::cancel() {
    for (vm::ObjectId* id : {&held_, &twin_})
        if (*id) {
            for (vm::ObjectId part : vm::groupOf(vm_, *id))
                view_.forget(part);
            vm::deleteGroup(vm_, *id);
            *id = 0;
        }
    swapped_ = false;
    last_ = {};
    lastOk_ = false;
    dragFrom_.reset();
    if (design_)
        design_->cancel();
    dirty_ = true;
}

void BuildMode::holdObjectAt(int x, int y, int direction, bool autoRotate) {
    direction_ = direction;
    if (!held_)
        return;
    // TryPlace step 2 (docs/buy-build-mac.md §7.6.1): a door or window turns
    // to the side its wall is on.
    // On a diagonal wall the twin turns instead (SwapDiagonalObject).
    if (autoRotate && vm::shouldAutoRotate(vm_, held_)) {
        const vm::AutoRotation turned = vm::autoRotate(vm_, held_, twin_, {x, y, level_}, direction_);
        if (turned.held != held_)
            swapTwin();
        direction_ = turned.direction;
    }
    last_ = vm::checkPlacement(vm_, held_, {x, y, level_}, direction_);
    // TestPlacementCommon's staircase test (CeilingTooHighForObject, 0x30),
    // which reads the lot's support and heights.
    if (const int e = editor_->stairCeilingError(last_)) {
        last_.ok = false, last_.error = e;
        last_.reason = "the ceiling above is too high";
    }
    // Turning green: ui_build_door_green / ui_build_stair_green.
    if (last_.ok && !lastOk_) {
        const std::string plop = objectSound(held_);
        if (plop == "ui_build_door_plop")
            sound("ui_build_door_green");
        else if (plop == "ui_build_stair_plop")
            sound("ui_build_stair_green");
    }
    lastOk_ = last_.ok;
    // Shown where it would go, whether it fits or not (as Buy mode does).
    for (const auto& part : last_.parts)
        if (vm::Object* o = vm_.object(part.id)) {
            o->data[vm::objdata::Direction] = int16_t(direction_);
            vm_.setPosition(*o, {float(part.tile.x) + 0.5f, float(part.tile.y) + 0.5f, part.tile.level});
        }
    dirty_ = true;
}

void BuildMode::swapTwin() {
    for (vm::ObjectId part : vm::groupOf(vm_, held_)) {
        view_.forget(part);
        if (vm::Object* o = vm_.object(part))
            vm_.setPosition(*o, vm::Position{});
    }
    std::swap(held_, twin_);
    swapped_ = !swapped_;
    if (const vm::Object* o = vm_.object(held_); o && o->definition)
        say("holding " + o->definition->fileName + " (the " + (swapped_ ? "twin" : "catalog object") + ")");
}

bool BuildMode::placeObjectAt(int x, int y, int direction, bool autoRotate) {
    if (!held_ || !object_)
        return false;
    holdObjectAt(x, y, direction, autoRotate);
    if (!last_.ok) {
        std::string text = last_.reason;
        if (last_.error >= 2 && vm::placementerror::textIndex(last_.error) >= 0)
            if (const StringTable* t = res_->table(0x7FA7DC7C, 0x89))
                if (std::string g = t->get(size_t(vm::placementerror::textIndex(last_.error))); !g.empty())
                    text = g + " (" + last_.reason + ")";
        say("cannot: " + text);
        sound("ui_error");
        return false;
    }
    const vm::ObjectId id = held_;
    const CatalogEntry& bought = *heldEntry();
    std::string why;
    if (!vm::buy(vm_, id, last_, bought.price, &why)) {
        say("not bought: " + why);
        sound("ui_error");
        return false;
    }
    sound(objectSound(id));
    // A staircase opens the floor above its steps (before its own history
    // entry, so undo takes the stairs away first).
    if (objectSound(id) == "ui_build_stair_plop")
        if (const auto opened = editor_->openStairwell(id); opened.changed)
            say(opened.message);
    syncHistory();
    Entry entry;
    entry.kind = Entry::Kind::Object, entry.id = id, entry.cost = bought.price;
    held_ = 0;
    // A door or window cuts its hole in the wall it stands in (the lot
    // model finds the holes from the lot's cObjects, lot_model.cpp).
    if (const vm::Object* o = vm_.object(id)) {
        Lot::Object c;
        c.instance = uint32_t(id);
        c.definition = bought.fileName + "_objt";
        c.model = bought.model;
        if (c.model.size() > 5 && c.model.ends_with("_cres"))
            c.model.resize(c.model.size() - 5);
        c.position = {o->position.x, o->position.y, sim_.floorHeight(o->position.level, o->position.x, o->position.y)};
        // The model faces 90 - 45 x direction degrees about Z (world.hpp).
        const float yaw = (90.0f - 45.0f * float(o->data[vm::objdata::Direction])) * float(M_PI) / 180.0f;
        c.rotation = {0, 0, std::sin(yaw / 2), std::cos(yaw / 2)};
        lot_.objects.push_back(c);
        entry.cobject = c;
    }
    undo_.push_back(std::move(entry));
    recorded();
    syncAfterObject();
    say("bought " + bought.name + " as object " + std::to_string(id) + " (" + bought.fileName + ") for " + money(bought.price) + "; funds " +
        money(vm::funds(vm_)));
    selectObject(*object_); // another of the same (doors and windows keep buying)
    return true;
}

void BuildMode::syncAfterObject() {
    editor_->syncWorld();
    redraw();
}

void BuildMode::rotate(int steps) {
    if (tool_ == Tool::Floor) {
        // The toolbar's rotation mode: 1 -> 2 -> 4 -> 8 (key 0xACB3A629).
        int k = 0;
        while ((1 << k) < floorRotation_ && k < 3)
            ++k;
        k = ((k + (steps > 0 ? 1 : -1)) % 4 + 4) % 4;
        floorRotation_ = 1 << k;
        say("floor rotation " + std::to_string(90 * k) + " degrees");
        return;
    }
    direction_ = ((direction_ + steps) % 8 + 8) % 8;
    if (held_ && !last_.parts.empty())
        holdObjectAt(last_.parts.front().tile.x, last_.parts.front().tile.y, direction_);
}

void BuildMode::syncHistory() {
    const size_t depth = editor_->undoDepth();
    for (size_t k = editorDepth_; k < depth; ++k) {
        undo_.push_back({});
        editorDepth_ = k + 1;
        recorded();
    }
    editorDepth_ = depth;
}

bool BuildMode::undo() {
    cancel();
    design_->cancel();
    syncHistory();
    if (undoOrder) {
        const bool ok = undoOrder->undo();
        if (!ok) {
            sound("ui_error");
            say("nothing to undo");
        }
        return ok;
    }
    return undoOwn();
}

bool BuildMode::redo() {
    cancel();
    design_->cancel();
    if (undoOrder) {
        const bool ok = undoOrder->redo();
        if (!ok) {
            sound("ui_error");
            say("nothing to redo");
        }
        return ok;
    }
    return redoOwn();
}

bool BuildMode::undoOwn() {
    cancel();
    design_->cancel();
    syncHistory();
    bool ok = false;
    if (!undo_.empty()) {
        Entry e = std::move(undo_.back());
        undo_.pop_back();
        switch (e.kind) {
        case Entry::Kind::Editor:
            ok = editor_->undo();
            editorDepth_ = editor_->undoDepth();
            break;
        case Entry::Kind::Object: // out of the world, the price back (as Buy mode's)
            if (vm_.object(e.id)) {
                e.placement = vm::currentPlacement(vm_, e.id);
                vm::pickUp(vm_, e.id);
                vm::chargeFunds(vm_, e.cost);
                if (e.cobject)
                    std::erase_if(lot_.objects, [&](const Lot::Object& c) { return c.instance == e.cobject->instance; });
                syncAfterObject();
                ok = true;
            }
            break;
        case Entry::Kind::Design:
            DesignTool::apply(vm_, view_, e.id, e.before);
            vm::chargeFunds(vm_, e.cost);
            ok = true;
            break;
        }
        if (ok)
            redo_.push_back(std::move(e));
    }
    if (ok)
        redraw();
    sound(ok ? "ui_undo" : "ui_error");
    say(ok ? "undone; funds " + money(vm::funds(vm_)) : "nothing to undo");
    return ok;
}

bool BuildMode::redoOwn() {
    cancel();
    design_->cancel();
    bool ok = false;
    if (!redo_.empty()) {
        Entry e = std::move(redo_.back());
        redo_.pop_back();
        switch (e.kind) {
        case Entry::Kind::Editor:
            ok = editor_->redo();
            editorDepth_ = editor_->undoDepth();
            break;
        case Entry::Kind::Object:
            // No funds test: RedoIt never asks (docs/buy-build-mac.md §7.25).
            if (vm_.object(e.id)) {
                vm::applyPlacement(vm_, e.id, e.placement);
                vm::chargeFunds(vm_, -e.cost);
                if (e.cobject)
                    lot_.objects.push_back(*e.cobject);
                syncAfterObject();
                ok = true;
            }
            break;
        case Entry::Kind::Design:
            DesignTool::apply(vm_, view_, e.id, e.after);
            vm::chargeFunds(vm_, -e.cost);
            ok = true;
            break;
        }
        if (ok)
            undo_.push_back(std::move(e));
        else
            redo_.push_back(std::move(e));
    }
    if (ok)
        redraw();
    sound(ok ? "ui_redo" : "ui_error");
    say(ok ? "redone; funds " + money(vm::funds(vm_)) : "nothing to redo");
    return ok;
}

vm::BuildEditor::Result BuildMode::autoRoof(int tool) {
    pressSound(Tool::Roof, false);
    const auto r = tool == 0 ? editor_->removeRoofs() : editor_->autoRoof(tool, preserveRoofs_);
    releaseSound(Tool::Roof, false, r);
    applied(r);
    return r;
}

void BuildMode::selectDesignTool() {
    cancel();
    design_->cancel();
    tool_ = Tool::Design;
    pattern_ = nullptr, object_ = nullptr, fence_ = nullptr, paint_ = nullptr;
    say("design tool");
}

bool BuildMode::design(vm::ObjectId id, simd_float2 at) {
    const vm::Object* o = vm_.object(id);
    if (!o || o->person || !o->definition)
        return false;
    const vm::Object* lead = vm_.object(vm::multiTileGroup(*o));
    const CatalogEntry* entry = lead ? objects_.byGuid(lead->guid()) : nullptr;
    dirty_ = true;
    return design_->open(id, at, entry ? entry->name : o->definition->fileName);
}

// The on-screen text of a refusal that has no text of its own in the game's
// table (the editor's English reasons stay in the log): the game's strings
// where one fits (UIText STR# 0x89), else a short PT-BR phrase (inferred).
std::string BuildMode::refusalText(const vm::BuildEditor::Result& r) const {
    const StringTable* t = res_->table(0x7FA7DC7C, 0x89);
    auto game = [&](size_t i) { return t ? t->get(i) : std::string(); };
    auto has = [&](const char* s) { return r.message.find(s) != std::string::npos; };
    std::string g;
    if (has("not enough money"))
        g = game(20); // "Dinheiro Insuficiente"
    else if (has("lower terrain further"))
        g = game(56);
    else if (has("raise terrain further"))
        g = game(57);
    else if (has("diagonal") || has("45 degrees"))
        g = game(46);
    else if (has("only exposed terrain"))
        g = game(88);
    else if (has("a Sim is in") || has("Sim is in"))
        g = game(43);
    return g.empty() ? "Não é possível fazer isso aqui" : g;
}

void BuildMode::applied(const vm::BuildEditor::Result& r) {
    // A refusal shows the game's text for its code (UIText STR# 0x89).
    std::string text = r.message;
    if (!r.ok) {
        text.clear();
        if (r.error >= 2 && vm::placementerror::textIndex(r.error) >= 0)
            if (const StringTable* t = res_->table(0x7FA7DC7C, 0x89))
                text = t->get(size_t(vm::placementerror::textIndex(r.error)));
        if (text.empty())
            text = refusalText(r);
        text += " (" + r.message + ")"; // the reason, for the log (the tooltip cuts it)
    }
    say((r.ok ? "" : "cannot: ") + text + "; funds " + money(vm::funds(vm_)));
    syncHistory();
    if (r.ok && r.changed)
        redraw();
}

// Headless steps, in order (levels default to the current one):
// --build-open <category>[,<subsort>] opens a panel, --build-level <n>,
// --build-funds <n> (test aid: sets the family's funds),
// --wall/--room/--delete-wall x0,y0,x1,y1, --fence x0,y0,x1,y1,<fence>,
// --delete-fence x0,y0,x1,y1, --roof x0,y0,x1,y1[,type 1-6] (over the
// walls of the current level), --delete-roof x,y, --roof-pattern <name|guid>,
// --pool x0,y0,x1,y1 / --delete-pool x,y, --foundation x0,y0,x1,y1[,product 0-2: foundation, lattice deck, deck on columns] / --delete-foundation x0,y0,x1,y1
// (tiles, corners inclusive), --terrain raise|lower|flatten|smooth,x,y[,ticks[,size]]
// and --terrain-paint <paint|erase>,x,y[,ticks[,size]] (a brush stroke of
// ticks of 100 ms), --floor x0,y0,x1,y1,<pattern|none>[,rotation],
// --floor-click px,py,<pattern> (one click: the quarters under the point),
// --floor-rotation <1|2|4|8>, --wallpaper x0,y0,x1,y1,<r|l|b>,<pattern|none>,
// --wallpaper-room x,y,<pattern|none>, --wallpaper-flood x0,y0,x1,y1,<r|l>,<pattern|none>
// (Shift on that face), --door <guid|name>@x,y[,dir] places
// a door, window or gate (no dir: turned to its wall, as a click does;
// --door <guid|name> alone only holds it, for the mouse steps), --build-undo / --build-redo (with any value),
// --build-tool pool (the pool tool, for the mouse steps --drag / --hover),
// --fireplace product,x,y,dir (holds the mantle and clicks there),
// --delete-fireplace <piece id> (the sledgehammer on a fireplace).
void BuildMode::runSteps(const std::vector<std::string>& args) {
    if (!active_)
        return;
    for (size_t i = 1; i + 1 < args.size(); ++i) {
        const std::string& a = args[i];
        const std::string& v = args[i + 1];
        std::vector<std::string> f;
        {
            std::string cur;
            for (char c : v + ",")
                if (c == ',')
                    f.push_back(cur), cur.clear();
                else
                    cur += c;
        }
        auto n = [&](size_t k) { return k < f.size() && !f[k].empty() ? std::stoi(f[k]) : 0; };
        auto pattern = [&](const std::string& key, bool floor) -> const BuildPattern* {
            if (key == "none")
                return nullptr;
            const BuildPattern* p = patterns_.find(key, floor);
            if (!p)
                throw std::runtime_error(a + ": no pattern \"" + key + "\"");
            return p;
        };
        vm::BuildEditor& ed = *editor_;
        std::optional<vm::BuildEditor::Result> r;
        if (a == "--build-open")
            openCategory(n(0), n(1));
        else if (a == "--build-funds") // test aid: the family's funds
            vm::addFunds(vm_, n(0) - vm::funds(vm_));
        else if (a == "--build-level")
            setLevel(n(0));
        else if (a == "--build-tool" && v == "pool") // the tool alone, for --drag / --hover
            selectPoolTool();
        else if (a == "--wall" && f.size() >= 4)
            r = ed.drawWalls(level_, n(0), n(1), n(2), n(3)), selectWallTool(false);
        else if (a == "--room" && f.size() >= 4)
            r = ed.drawRoom(level_, n(0), n(1), n(2), n(3)), selectWallTool(true);
        else if (a == "--delete-wall" && f.size() >= 4)
            r = ed.deleteWalls(level_, n(0), n(1), n(2), n(3));
        else if (a == "--fence" && f.size() >= 5) {
            const BuildFence* fence = patterns_.findFence(f[4]);
            if (!fence)
                throw std::runtime_error("--fence: no fence \"" + f[4] + "\"");
            selectFence(*fence);
            r = ed.drawFences(level_, n(0), n(1), n(2), n(3), *fence);
        } else if (a == "--delete-fence" && f.size() >= 4)
            r = ed.deleteFences(level_, n(0), n(1), n(2), n(3));
        else if (a == "--roof" && f.size() >= 4)
            selectRoofTool(uint32_t(f.size() > 4 ? n(4) : 5)),
                r = ed.drawRoof(level_, n(0), n(1), n(2), n(3), uint32_t(f.size() > 4 ? n(4) : 5));
        else if (a == "--delete-roof" && f.size() >= 2)
            r = ed.deleteRoof(level_, std::stof(f[0]), std::stof(f[1]));
        else if (a == "--roof-pattern") {
            const BuildRoofPattern* p = patterns_.findRoofPattern(v);
            if (!p)
                throw std::runtime_error("--roof-pattern: no pattern \"" + v + "\"");
            r = ed.setRoofPattern(p->guid);
        } else if (a == "--pool" && f.size() >= 4)
            selectPoolTool(), r = ed.drawPool(n(0), n(1), n(2), n(3));
        else if (a == "--delete-pool" && f.size() >= 2)
            r = ed.deletePool(n(0), n(1));
        else if (a == "--erase-pool" && f.size() >= 4)
            r = ed.erasePool(n(0), n(1), n(2), n(3));
        else if (a == "--foundation" && f.size() >= 4)
            selectFoundationTool(n(4)), r = ed.drawFoundation(n(0), n(1), n(2), n(3), n(4));
        else if (a == "--delete-foundation" && f.size() >= 4)
            r = ed.deleteFoundation(n(0), n(1), n(2), n(3));
        else if ((a == "--terrain" || a == "--terrain-paint") && f.size() >= 3) {
            using Op = vm::BuildEditor::TerrainOp;
            Op op = Op::Paint;
            const BuildTerrainPaint* paint = nullptr;
            if (a == "--terrain")
                op = f[0] == "lower" ? Op::Lower : f[0] == "flatten" ? Op::Flatten : f[0] == "smooth" ? Op::Smooth
                     : f[0] == "water" ? Op::Water : Op::Raise;
            else if (f[0] == "erase")
                op = Op::Erase;
            else if (!(paint = patterns_.findTerrainPaint(f[0])))
                throw std::runtime_error("--terrain-paint: no paint \"" + f[0] + "\"");
            selectTerrainTool(op, paint);
            r = ed.terrainStroke(op, std::stof(f[1]), std::stof(f[2]), f.size() > 3 ? n(3) : 10, f.size() > 4 ? n(4) : 1,
                                 op == Op::Lower   ? patterns_.lowerProfile()
                                 : op == Op::Water ? patterns_.waterProfile()
                                                   : patterns_.raiseProfile(),
                                 paint ? paint->texture : "");
            pressSound(Tool::Terrain, op == Op::Erase);
            stopBrushLoop();
            releaseSound(Tool::Terrain, op == Op::Erase, *r);
        } else if (a == "--floor-rotation")
            floorRotation_ = n(0);
        else if (a == "--floor" && f.size() >= 5) {
            const BuildPattern* p = pattern(f[4], true);
            if (p)
                selectPattern(*p);
            r = ed.paintFloor(level_, n(0), n(1), n(2), n(3), p, 0xF, f.size() >= 6 ? n(5) : floorRotation_);
        } else if (a == "--floor-click" && f.size() >= 3) {
            const float px = std::stof(f[0]), py = std::stof(f[1]);
            const BuildPattern* p = pattern(f[2], true);
            if (p)
                selectPattern(*p);
            const int x = int(std::floor(px)), y = int(std::floor(py));
            r = ed.paintFloor(level_, x, y, x, y, p, ed.clickQuarters(level_, px, py), floorRotation_);
        } else if (a == "--wallpaper" && f.size() >= 6) {
            const BuildPattern* p = pattern(f[5], false);
            r = ed.paintWalls(level_, n(0), n(1), n(2), n(3), f[4] == "r" ? 1 : f[4] == "l" ? -1 : 0, p);
            if (p)
                selectPattern(*p);
        } else if (a == "--wallpaper-flood" && f.size() >= 6) {
            const BuildPattern* p = pattern(f[5], false);
            r = ed.paintFlood(level_, n(0), n(1), n(2), n(3), f[4] == "l" ? -1 : 1, p);
            if (p)
                selectPattern(*p);
        } else if (a == "--wallpaper-room" && f.size() >= 3) {
            r = ed.paintRoom(level_, n(0), n(1), pattern(f[2], false));
        } else if (a == "--door") {
            const auto at = v.find('@');
            const CatalogEntry* e = objects_.find(v.substr(0, at));
            if (!e)
                throw std::runtime_error("--door: no build object \"" + v.substr(0, at) + "\"");
            int x = 0, y = 0, dir = 2, got = 0;
            if (at != std::string::npos)
                got = std::sscanf(v.c_str() + at + 1, "%d,%d,%d", &x, &y, &dir);
            selectObject(*e);
            // Without a direction it turns to its wall, as the mouse's click does.
            if (at != std::string::npos)
                placeObjectAt(x, y, dir, got < 3);
        } else if (a == "--build-design") { // <id>[,row:option...][,accept|cancel]
            selectDesignTool();
            if (design(vm::ObjectId(n(0)), simd_make_float2(560, 420)))
                for (size_t k = 1; k < f.size(); ++k) {
                    int row = 0, option = 0;
                    if (f[k] == "accept")
                        design_->accept();
                    else if (f[k] == "cancel")
                        design_->cancel();
                    else if (std::sscanf(f[k].c_str(), "%d:%d", &row, &option) == 2)
                        design_->choose(row, size_t(option));
                }
        } else if (a == "--stairs" && f.size() >= 5) { // product,x,y,level,dir[,endx,endy[,whole]]
            const BuildStairs* b = patterns_.stairsById(n(0));
            if (!b)
                throw std::runtime_error("--stairs: no modular stairs " + f[0]);
            selectStairsTool(*b);
            std::optional<std::array<float, 2>> end;
            if (f.size() >= 7)
                end = std::array<float, 2>{std::stof(f[5]), std::stof(f[6])};
            r = ed.placeStairs(*b, {n(1), n(2), n(3)}, n(4), end, f.size() > 7 && f[7] == "whole");
            releaseSound(Tool::Stairs, false, *r);
        } else if ((a == "--fence-arch" || a == "--delete-fence-arch") && f.size() >= 4 && !patterns_.fenceArches().empty()) {
            const BuildFenceArch& arch = patterns_.fenceArches().front();
            r = a == "--fence-arch" ? ed.placeFenceArch(f.size() > 4 ? n(4) : level_, n(0), n(1), n(2), n(3), arch)
                                    : ed.deleteFenceArch(f.size() > 4 ? n(4) : level_, n(0), n(1), n(2), n(3), &arch);
        } else if (a == "--dormer" && f.size() >= 4) { // product 6-11,x,y,direction[,level]
            if (const BuildRoofProduct* d = patterns_.roofProduct(n(0)))
                selectDormerTool(*d);
            r = ed.placeDormer(n(0), f.size() > 4 ? n(4) : level_, n(1), n(2), n(3));
            releaseSound(Tool::Roof, false, *r);
        } else if (a == "--railing" && f.size() >= 2) { // part id,side 0|1[,delete]
            selectRailingTool();
            r = ed.changeRailing(vm::ObjectId(n(0)), n(1) != 0, f.size() > 2 && f[2] == "delete");
        } else if (a == "--fireplace" && f.size() >= 4) { // product 0-4,x,y,direction (holds it, then a click)
            const BuildFireplace* fp = nullptr;
            for (const auto& b : patterns_.fireplaces())
                if (b.id == n(0))
                    fp = &b;
            if (!fp)
                throw std::runtime_error("--fireplace: no fireplace " + f[0]);
            selectFireplaceTool(*fp);
            placeFireplaceAt(n(1), n(2), n(3));
        } else if (a == "--delete-fireplace" && !f.empty()) { // any piece's object id (the sledgehammer)
            r = ed.deleteFireplace(vm::ObjectId(n(0)));
        } else if (a == "--delete-stairs" && !f.empty()) {
            r = ed.deleteStairs(vm::ObjectId(n(0)));
            releaseSound(Tool::Stairs, true, *r);
        } else if (a == "--auto-roof") { // tool 1-5, 0 removes the roofs; ",keep" preserves
            preserveRoofs_ = f.size() > 1 && f[1] == "keep";
            autoRoof(n(0));
        } else if (a == "--build-day-night") {
            if (onDayNight)
                onDayNight();
        } else if (a == "--build-undo") {
            undo();
        } else if (a == "--build-redo") {
            redo();
        }
        syncHistory();
        if (r) {
            // The step's sounds, as the mouse would make them.
            static const std::map<std::string, std::pair<Tool, bool>> kStepTools{
                {"--wall", {Tool::Wall, false}},        {"--room", {Tool::Room, false}},
                {"--delete-wall", {Tool::Wall, true}},  {"--fence", {Tool::Fence, false}},
                {"--delete-fence", {Tool::Fence, true}}, {"--roof", {Tool::Roof, false}},
                {"--delete-roof", {Tool::Roof, true}},  {"--pool", {Tool::Pool, false}},
                {"--delete-pool", {Tool::Pool, true}}, {"--erase-pool", {Tool::Pool, true}},  {"--foundation", {Tool::Foundation, false}},
                {"--delete-foundation", {Tool::Foundation, true}}, {"--floor", {Tool::Floor, false}},
                {"--floor-click", {Tool::Floor, false}}, {"--wallpaper", {Tool::Wallpaper, false}},
                {"--wallpaper-flood", {Tool::Wallpaper, false}}, {"--wallpaper-room", {Tool::Wallpaper, false}}};
            if (auto t = kStepTools.find(a); t != kStepTools.end()) {
                const auto [tool, erase] = t->second;
                pressSound(tool, erase);
                if (r->ok && (tool == Tool::Wall || tool == Tool::Room || tool == Tool::Fence))
                    for (int k = 0; k < std::min(r->changed, 8); ++k)
                        sound(erase ? "ui_build_walldelete_section" : "ui_build_wall_section");
                if (r->ok && tool == Tool::Foundation && !erase)
                    sound("ui_build_deck_section");
                releaseSound(tool, erase, *r);
            }
            std::string shown; // the refusal's text as the tooltip shows it (code 0x..: STR# 0x89)
            if (!r->ok && r->error) {
                char c[16];
                std::snprintf(c, sizeof c, "0x%02X", r->error);
                std::string g;
                if (vm::placementerror::textIndex(r->error) >= 0)
                    if (const StringTable* t = res_->table(0x7FA7DC7C, 0x89))
                        g = t->get(size_t(vm::placementerror::textIndex(r->error)));
                shown = std::string(" [") + c + " \"" + (g.empty() ? refusalText(*r) : g) + "\"]";
            }
            std::printf("opensims2: build: %s %s: %s%s%s (funds §%d)\n", a.c_str(), v.c_str(), r->ok ? "" : "FAILED: ",
                        r->message.c_str(), shown.c_str(), vm::funds(vm_));
            if (r->ok && r->changed)
                redraw();
        }
    }
    std::fflush(stdout);
    update();
}

} // namespace os2
