#include "app/buy_mode.hpp"
#include "formats/log_once.hpp"

#include "app/design_tool.hpp"

#include "app/lot_vm.hpp"
#include "app/vm_objects.hpp"
#include "formats/font.hpp"
#include "formats/model.hpp"
#include "formats/resindex.hpp"
#include "formats/ui_resources.hpp"
#ifdef OS2_SDL_GPU
#include "render/gpu_renderer.hpp"
#else
#include "render/renderer.hpp"
#include "render/ui_layer.hpp"
#endif
#include "vm/sim_motion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace os2 {

namespace {

constexpr uint32_t kPanelScript = 0x49060004, kPuckScript = 0x4905F85B, kDescriptionScript = 0x49063004;
constexpr uint32_t kUiTextGroup = 0x7FA7DC7C; // Text/UIText.package
constexpr uint32_t kPlacementErrors = 0x89;   // its placement error texts (code - 2)

// Buy panel windows (0x49060004, docs/buy-build-mac.md §1.4).
constexpr uint32_t kTabFunction = 0x2C584E6C, kTabRoom = 0x4C578F23, kTabCollections = 0x2CACE0CD;
constexpr uint32_t kBgFunction = 0x4CC48AA1, kBgRoom = 0x4CC48AA2, kBgCollections = 0x4CC48AA3;
constexpr uint32_t kFunctionSort = 0x0A00, kFunctionButtons = 0xA000, kRoomSort = 0x0C00, kRoomButtons = 0xC000,
                   kRoomFunctions = 0xD000, kRoomBack = 0xDA00, kCollections = 0x0E00;
constexpr uint32_t kCatalog = 0x898F42CD, kCollapsed = 0x1000, kPrev = 0x1004, kNext = 0x1005;
constexpr uint32_t kToolHand = 0xCC607031, kToolEyedropper = 0xCC607032, kToolDesign = 0xCC607030, kUndo = 0xCC607033,
                   kRedo = 0xCC607034, kDayNight = 0xCC607035, kTopDown = 0xCC607036;
// The grid's slot within the catalog's window (0x1006 of 0x1000: 176 x 100;
// the expanded 0x2006 of 0x2000: 176 x 196). The cells come at the window's
// own (52, 19) corner with no extra inset, 44 px on a 48 px pitch (measured:
// the first cell at 589 over the window's 537, the rows at 591, 639 and 687
// at 1280x720; the script's own margins are the collections window's,
// inferred, and go unused here).
constexpr float kGridX = 52, kGridY = 19;
constexpr float kCell = 44, kCellStep = 48, kCellInX = 0, kCellInY = 8, kThumb = 40, kCellRadius = 8;
constexpr int kRows = 2;
// The original keeps the catalog's left part and stretches its right one
// (docs/buy-build-mac.md §1.6): the window's left cap at 537, the first cell
// at x 589, and the strip's own last cap at the right edge — 20 px wide with
// its right at width-30 — with the cells' block (44 px plates on a 48 px
// pitch, its 4 last px of slack) filling up to it (13 columns at 1280 wide,
// 3 at 800).
constexpr float kGridLeft = 589, kStripRight = 29, kCapW = 20;
int catalogColumns(float width) {
    const float capL = width - kStripRight - kCapW; // the cap's left edge
    return std::max(1, int((capL - (kGridLeft - 1) + kCellInX) / kCellStep));
}
// The expanded catalog (window 0x2000, opened by "Expandir" 0x1009 and shut
// by "Recolher" 0x2009): 4 rows on the same pitch.
constexpr uint32_t kExpanded = 0x2000, kExpandedPrev = 0x2004, kExpandedNext = 0x2005, kExpand = 0x1009,
                   kCollapse = 0x2009;
constexpr int kExpandedRows = 4;
// The expanded catalog's top over the panel's origin; the collapsed one's
// is 96 (window 0x1000).
constexpr float kExpandedTop = 9;

// The room buttons 0xC100-0xC900: room sort bit of each (§1.4).
constexpr std::array<int, 9> kRoomBits = {3, 5, 4, 6, 7, 1, 0, 8, 2};

// The subsort a button 0xB<k><n>0 of function k selects (§1.4): its bits,
// 0 for "all", -1 for none (back, or not a button).
int subSortOf(int k, int n) {
    if (n == 7 && k != 7)
        return 0;
    if (n == 6 && k != 7)
        return 0x80;
    switch (k) {
    case 6: { const int t[] = {-1, -1, 1, 2, 0x20, 8, -1, -1, 0x10, 4}; return n < 10 ? t[n] : -1; }
    case 7: { const int t[] = {-1, -1, -1, -1, 8, 2, 0x10, 0x80, 0, -1}; return n < 10 ? t[n] : -1; }
    case 8: { const int t[] = {-1, -1, 1, 2, 8, 4, -1, -1, 0x10, -1}; return n < 10 ? t[n] : -1; }
    case 9: { const int t[] = {-1, -1, 1, -1, 2, 4, -1, -1, 8, -1}; return n < 10 ? t[n] : -1; }
    default: { const int t[] = {-1, -1, 1, 2, 4, 8, -1, -1, 0x10, -1}; return n < 10 ? t[n] : -1; }
    }
}

bool inside(const std::array<float, 4>& r, simd_float2 p) {
    return p.x >= r[0] && p.x < r[2] && p.y >= r[1] && p.y < r[3];
}

std::string money(int32_t v) {
    std::string digits = std::to_string(v < 0 ? -int64_t(v) : int64_t(v)), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0)
            out += '.';
        out += digits[i];
    }
    return std::string(v < 0 ? "-" : "") + "§" + out;
}

std::string area(int x0, int y0, int x1, int y1) {
    return "(" + std::to_string(x0) + "," + std::to_string(y0) + "," + std::to_string(x1) + "," + std::to_string(y1) +
           ")";
}

// A flat unit tile, alpha blended and unlit, for the footprint.
ObjectModel tileModel(const std::string& name, const char* rgb) {
    ObjectModel m;
    m.name = name;
    ObjectModel::Part part;
    part.node = part.shape = name;
    part.material = name;
    const float e = 0.04f;
    part.mesh.positions = {{e, e, 0}, {1 - e, e, 0}, {1 - e, 1 - e, 0}, {e, 1 - e, 0}};
    part.mesh.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    part.mesh.uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    part.mesh.triangles = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
    m.parts.push_back(std::move(part));
    MaterialDefinition md;
    md.materialType = "StandardMaterial";
    md.properties = {{"stdMatDiffCoef", rgb},
                     {"stdMatEmissiveCoef", rgb},
                     {"stdMatUntexturedDiffAlpha", "0.45"},
                     {"stdMatAlphaBlendMode", "blend"},
                     {"stdMatLightingEnabled", "false"},
                     {"stdMatCullMode", "none"},
                     {"stdMatLayer", "1"}};
    m.materials[name] = md;
    return m;
}

simd_float4x4 at(float x, float y, float z) {
    simd_float4x4 m = matrix_identity_float4x4;
    m.columns[3] = simd_make_float4(x, y, z, 1);
    return m;
}

} // namespace

BuyMode::BuyMode(LotSimulation& simulation, VmObjectView& view, Renderer& renderer, ResourceIndex& index,
                 const std::filesystem::path& resDir)
    : sim_(simulation), vm_(simulation.machine()), view_(view), renderer_(renderer), index_(index),
      res_(std::make_unique<UiResources>(index, resDir)),
      fonts_(std::make_unique<FontLibrary>(resDir / "UI/Fonts")), catalog_(Catalog::load(index)) {
    catalog_.setThumbnailPackage(resDir / "UserData/Thumbnails/ObjectThumbnails.package");
    auto load = [&](uint32_t id, UiScript& into) {
        if (auto s = res_->script(id)) {
            into = std::move(*s);
            return !into.roots.empty();
        }
        std::fprintf(stderr, "opensims2: buy mode: no UI script %08X\n", id);
        return false;
    };
    ok_ = load(kPanelScript, panel_) && load(kPuckScript, puck_) && load(kDescriptionScript, description_);
    design_ = std::make_unique<DesignTool>(vm_, view_, index_, *res_, *fonts_);
    design_->say = [this](const std::string& m) { say(m); };
    design_->playSound = [this](const std::string& n) { sound(n.c_str()); };
    design_->onAccepted = [this](const DesignTool::Change& c) {
        Op op;
        op.kind = Op::Kind::Design, op.id = c.id, op.cost = c.cost, op.before = c.before, op.after = c.after;
        undo_.push_back(std::move(op));
        recorded();
        dirty_ = true;
    };
    // The footprint: the game's tile outlines (cTSUserToolObject::
    // CreateAndAttachTileOutlineNode with "objectPlacementIconGood" /
    // "objectPlacementIconBad", Sims3D/Objects05.package): a tile frame whose
    // front edge has the chevron that shows where the object faces (+Y of
    // the model, turned as the object). Without them, plain tiles.
    for (int i = 0; i < 2; ++i)
        try {
            const ObjectModel m = loadObjectModel(index_, i ? "objectPlacementIconBad" : "objectPlacementIconGood");
            if (!m.parts.empty())
                tileModels_[size_t(i)] = renderer_.addObject(m, index_), outlines_ = true;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: buy mode: %s\n", e.what());
        }
    if (outlines_ && tileModels_[0] >= 0 && tileModels_[1] >= 0)
        std::printf("opensims2: buy mode: footprint from objectPlacementIconGood/Bad (with the facing chevron)\n");
    if (tileModels_[0] < 0 || tileModels_[1] < 0)
        try {
            outlines_ = false;
            tileModels_[0] = renderer_.addObject(tileModel("os2_buy_ok", "0.2,1,0.2"), index_);
            tileModels_[1] = renderer_.addObject(tileModel("os2_buy_no", "1,0.15,0.1"), index_);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: buy mode: %s\n", e.what());
        }
}

BuyMode::~BuyMode() = default;

void BuyMode::say(const std::string& m) {
    message_ = m;
    std::printf("opensims2: buy mode: %s\n", m.c_str());
    std::fflush(stdout);
    dirty_ = true;
}

void BuyMode::enter() {
    if (active_)
        return;
    active_ = true;
    savedPaused_ = vm_.globals[vm::global::Paused];
    vm_.globals[vm::global::Paused] = 1;
    // The level of the selected Sim, else the lot's ground.
    level_ = vm_.world ? vm_.world->firstLevel() : 1;
    if (const vm::Object* s = vm_.object(vm_.globals[vm::global::SelectedPersonId]); s && s->position.inWorld())
        level_ = s->position.level;
    say("entered (" + std::to_string(catalog_.entries().size()) + " catalog items, funds " + money(vm::funds(vm_)) +
        ")");
}

void BuyMode::exit() {
    if (!active_)
        return;
    cancel();
    design_->cancel();
    tool_ = Tool::Hand;
    // The game empties its one undo manager on entering Live mode, not on
    // leaving Buy mode (docs/buy-build-mac.md §7.24): with the shared order
    // the host flushes it then.
    if (!undoOrder)
        flushHistory();
    vm_.globals[vm::global::Paused] = savedPaused_;
    active_ = false;
    selected_ = nullptr;
    update();
    say("left");
}

void BuyMode::flushHistory() {
    // What was sold, and the purchases undone, die now (ResetSelf).
    auto die = [&](vm::ObjectId id) {
        if (const vm::Object* o = vm_.object(id); o && !o->position.inWorld()) {
            for (vm::ObjectId part : vm::groupOf(vm_, id))
                view_.forget(part);
            vm::deleteGroup(vm_, id);
        }
    };
    for (const Op& op : undo_)
        if (op.kind == Op::Kind::Deleted)
            die(op.id);
    for (const Op& op : redo_)
        if (op.kind == Op::Kind::Created)
            die(op.id);
    undo_.clear(), redo_.clear();
}

void BuyMode::dropRedo() {
    // ReleaseRedoList: a purchase undone dies (its undoable is "deleted").
    for (const Op& op : redo_)
        if (op.kind == Op::Kind::Created)
            if (const vm::Object* o = vm_.object(op.id); o && !o->position.inWorld()) {
                for (vm::ObjectId part : vm::groupOf(vm_, op.id))
                    view_.forget(part);
                vm::deleteGroup(vm_, op.id);
            }
    redo_.clear();
}

// --- The object tool --------------------------------------------------------

bool BuyMode::select(const CatalogEntry& entry) {
    cancel();
    try {
        held_ = vm::createGhost(vm_, entry.group, entry.instance);
    } catch (const std::exception& e) {
        say(entry.name + ": " + e.what());
        held_ = 0;
        return false;
    }
    heldNew_ = true;
    selected_ = &entry;
    last_ = {};
    twin_ = vm::createDiagonalTwin(vm_, held_);
    say("holding " + entry.name + " (" + money(entry.price) + ")");
    return true;
}

vm::Placement BuyMode::current(vm::ObjectId id) {
    vm::Placement p;
    const vm::Object* lead = vm_.object(id);
    if (!lead)
        return p;
    p.ok = lead->position.inWorld();
    p.level = lead->position.level;
    p.direction = lead->data[vm::objdata::Direction];
    p.container = lead->data[vm::objdata::ContainerId];
    p.slot = p.container ? lead->data[vm::objdata::SlotNumber] : -1;
    for (vm::ObjectId part : vm::groupOf(vm_, id))
        if (const vm::Object* o = vm_.object(part))
            p.parts.push_back({part, vm::VM::tileOf(*o)});
    return p;
}

// Shows the held object where `p` says, whether it fits or not (the
// trees run only when it is placed).
void BuyMode::preview(const vm::Placement& p) {
    for (const auto& part : p.parts) {
        vm::Object* o = vm_.object(part.id);
        if (!o)
            continue;
        o->data[vm::objdata::Direction] = int16_t(p.direction);
        vm_.setPosition(*o, {float(part.tile.x) + 0.5f, float(part.tile.y) + 0.5f, part.tile.level});
        vm_.setContainer(*o, p.ok ? p.container : vm::ObjectId(0), int16_t(p.ok && p.container ? p.slot : -1));
        if (p.ok && p.container)
            if (const vm::Object* c = vm_.object(p.container))
                vm_.setPosition(*o, c->position);
    }
}

bool BuyMode::changeLevel(int delta) {
    if (!active_ || !vm_.world)
        return false;
    // Not the pool's level (0, under the ground): the floors stop at the ground.
    const int first = vm_.world->firstLevel(), hi = first + std::max(1, vm_.world->levelCount()) - 1;
    const int lo = std::min(std::max(first, 1), hi);
    const int level = std::clamp(level_ + delta, lo, hi);
    if (level == level_)
        return false;
    level_ = level;
    say("level " + std::to_string(level_));
    // What is held goes to the new floor at the same tile.
    if (held_ && !last_.parts.empty()) {
        const vm::TilePos lead = last_.parts.front().tile;
        holdAt(lead.x, lead.y, direction_);
    }
    return true;
}

// OnRelease's sound (docs/buy-build-mac.md §6): one for doors and windows
// (object types 8 and 9), one for plants, one for the rest. The sound names
// are inferred from the executable's ui_* list; plants = decorative
// function (0x20) with subsort 8 (the "plants" button, §1.4).
const char* BuyMode::dropSound(vm::ObjectId id) const {
    const vm::Object* o = vm_.object(id);
    if (!o || !o->definition)
        return "ui_object_plop";
    const auto& d = *o->definition;
    const int type = d.field(objd::ObjectType);
    if (type == 8 || type == 9)
        return "ui_build_door_plop";
    if ((d.field(objd::FunctionSort) & 0x20) && (d.field(objd::FunctionSubSort) & 8))
        return "ui_build_garden_plop";
    return "ui_object_plop";
}

// Where the mouse ray meets the floor of the current level: the first
// point along the ray at or under that floor's height there. The floor is
// not one plane (a foundation's top is 0.75 up, the same level off the
// foundation is a storey up), and a ray met with one plane, then another,
// landed tiles away from the mouse, back and forth (the held object did not
// follow the mouse on the tutorial lot, round 21).
std::optional<simd_float3> BuyMode::rayHit(simd_float3 eye, simd_float3 dir) const {
    if (!vm_.world || dir.z > -1e-4f)
        return std::nullopt;
    auto above = [&](float t) {
        const simd_float3 p = eye + dir * t;
        return p.z > sim_.floorHeight(level_, p.x, p.y);
    };
    if (!above(0))
        return std::nullopt;
    const float far = (eye.z + 20) / -dir.z; // 20 under the ground at most
    const float step = 0.05f;
    for (float t = step; t < far; t += step)
        if (!above(t)) {
            float lo = t - step, hi = t;
            for (int i = 0; i < 12; ++i) {
                const float mid = (lo + hi) / 2;
                (above(mid) ? lo : hi) = mid;
            }
            const simd_float3 hit = eye + dir * hi;
            if (std::getenv("OS2_DEBUG_BUY"))
                std::fprintf(stderr, "buy: ray eye (%.2f %.2f %.2f) dir (%.3f %.3f %.3f) -> (%.2f %.2f %.2f) level %d\n", eye.x,
                             eye.y, eye.z, dir.x, dir.y, dir.z, hit.x, hit.y, hit.z, level_);
            return hit;
        }
    return std::nullopt;
}

std::optional<vm::TilePos> BuyMode::tileUnder(simd_float3 eye, simd_float3 dir) const {
    const auto hit = rayHit(eye, dir);
    if (!hit)
        return std::nullopt;
    return vm::TilePos{int(std::floor(hit->x)), int(std::floor(hit->y)), level_};
}

void BuyMode::holdAt(int x, int y, int direction, bool autoRotate) {
    direction_ = direction;
    if (!held_)
        return;
    // TryPlace step 2: what needs a wall turns to it (docs/buy-build-mac.md §7.6.1).
    // On a diagonal wall the twin turns instead (SwapDiagonalObject).
    if (autoRotate && vm::shouldAutoRotate(vm_, held_)) {
        const vm::AutoRotation turned = vm::autoRotate(vm_, held_, twin_, {x, y, level_}, direction_);
        if (turned.held != held_) {
            for (vm::ObjectId part : vm::groupOf(vm_, held_)) {
                view_.forget(part);
                if (vm::Object* o = vm_.object(part))
                    vm_.setPosition(*o, vm::Position{});
            }
            std::swap(held_, twin_);
        }
        direction_ = turned.direction;
    }
    last_ = vm::checkPlacement(vm_, held_, {x, y, level_}, direction_);
    preview(last_);
    dirty_ = true;
}

void BuyMode::pointAt(simd_float3 eye, simd_float3 dir) {
    if (active_ && !held_) // the hand's cursor over what it can pick up
        overObject_ = objectAt(eye, dir, tileUnder(eye, dir)) != 0;
    if (!active_ || !held_ || pressing_)
        return;
    if (auto t = tileUnder(eye, dir))
        holdAt(t->x, t->y, direction_, true);
}

std::string BuyMode::errorText(const vm::Placement& p) {
    if (p.error >= 2 && vm::placementerror::textIndex(p.error) >= 0)
        if (const StringTable* t = res_->table(kUiTextGroup, kPlacementErrors)) {
            std::string s = t->get(size_t(vm::placementerror::textIndex(p.error)));
            if (!s.empty())
                return s + " (" + p.reason + ")";
        }
    return p.reason;
}

bool BuyMode::placeAt(int x, int y, int direction) {
    if (!held_)
        return false;
    holdAt(x, y, direction);
    const vm::Placement p = last_;
    if (!p.ok) {
        say("cannot place at (" + std::to_string(x) + ", " + std::to_string(y) + ", level " + std::to_string(level_) +
            "): " + errorText(p));
        sound("ui_error");
        return false;
    }
    const vm::ObjectId id = held_;
    if (heldNew_) {
        const int price = selected_ ? selected_->price : 0;
        std::string why;
        if (!vm::buy(vm_, id, p, price, &why)) {
            say("not bought: " + why);
            sound("ui_error");
            return false;
        }
        dropTwin();
        undo_.push_back({Op::Kind::Created, id, p, price});
        sound(dropSound(id));
        say("bought " + (selected_ ? selected_->name : std::string("?")) + " as object " + std::to_string(id) + " at (" +
            std::to_string(x) + ", " + std::to_string(y) + ") facing " + std::to_string(p.direction) + " for " +
            money(price) + "; funds " + money(vm::funds(vm_)));
        held_ = 0;
        recorded();
        // "Keep Buying" (OBJD field 96): another one is held at once.
        if (selected_ && vm_.object(id) && vm_.object(id)->definition &&
            vm_.object(id)->definition->field(objd::KeepBuying)) {
            // At the mouse at once, facing the same way (over the one just
            // placed, so red until the mouse moves on).
            if (select(*selected_))
                holdAt(x, y, direction);
        }
    } else {
        vm::applyPlacement(vm_, id, p);
        vm::runUserPlacement(vm_, id);
        undo_.push_back({Op::Kind::Moved, id, heldFrom_, 0});
        sound(dropSound(id));
        recorded();
        held_ = 0;
        say("moved object " + std::to_string(id));
    }
    dirty_ = true;
    return true;
}

bool BuyMode::grab(vm::ObjectId id) {
    const vm::Object* o = vm_.object(id);
    if (!o || o->person || !o->definition)
        return false;
    cancel();
    const vm::ObjectId lead = vm::multiTileGroup(*o);
    heldFrom_ = current(lead);
    // The user pickup tree (12) runs on single-tile objects (the group
    // handles multi-tile ones, §4).
    if (vm::groupOf(vm_, lead).size() == 1)
        try {
            vm_.runFunction(lead, vm::entry::UserPickup);
        } catch (const std::exception& err) {
            os2::logOnce("Buy mode: the pickup tree", o->definition->fileName, err);
        }
    held_ = lead;
    heldNew_ = false;
    direction_ = heldFrom_.direction;
    overObject_ = false;
    // The outline at once, on the tile it stands on (it showed only after
    // the mouse moved: none while the button that picked it up was down).
    if (!heldFrom_.parts.empty()) {
        const vm::TilePos at = heldFrom_.parts.front().tile;
        last_ = vm::checkPlacement(vm_, held_, {at.x, at.y, at.level}, direction_);
        if (!heldFrom_.ok)
            last_.parts.clear();
    }
    sound("ui_object_pickup");
    std::string outline;
    if (!last_.parts.empty())
        outline = "; outline on (" + std::to_string(last_.parts.front().tile.x) + ", " +
                  std::to_string(last_.parts.front().tile.y) + ")";
    say("picked up object " + std::to_string(lead) + " (" + vm_.object(lead)->definition->fileName + ", worth " +
        money(vm::saleValue(vm_, lead)) + ")" + outline);
    dirty_ = true;
    return true;
}

bool BuyMode::grabDrag(vm::ObjectId id, int x, int y) {
    if (!active_ || !grab(id))
        return false;
    grabPress_ = true;
    const vm::TilePos from = last_.parts.empty() ? vm::TilePos{x, y, level_} : last_.parts.front().tile;
    grabMoved_ = x != from.x || y != from.y;
    pressTile_ = {x, y, level_};
    holdAt(x, y, direction_);
    return mouseUp();
}

// What the tools act on: the object under the mouse ray, else (and for a
// slot's contents first) what stands on the tile under it. People are never
// picked.
vm::ObjectId BuyMode::objectAt(simd_float3 eye, simd_float3 dir, const std::optional<vm::TilePos>& tile) const {
    auto usable = [&](vm::ObjectId id) {
        const vm::Object* o = vm_.object(id);
        return o && !o->person && o->definition;
    };
    if (pickObject)
        if (const vm::ObjectId id = pickObject(eye, dir); usable(id))
            return id;
    if (!tile)
        return 0;
    // An object in a slot of something on the tile first, then what stands on it.
    const std::vector<vm::ObjectId> on = vm_.objectsOnTile(*tile);
    for (vm::ObjectId id : on)
        for (const auto& [other, x] : vm_.objects())
            if (x.data[vm::objdata::ContainerId] == id && !x.person && x.definition)
                return other;
    for (vm::ObjectId id : on)
        if (usable(id))
            return id;
    return 0;
}

void BuyMode::setTool(Tool t) {
    cancel();
    design_->cancel();
    tool_ = t;
    dirty_ = true;
}

// The catalog entry of an object (its group's lead): by GUID, else by file name.
// A multi-tile object is in the catalog by its master OBJD, whose GUID none of
// the parts has ("Sofa - Loveseat - Art Nouveau (0,0)"): the multi-tile entry
// of the parts' group.
const CatalogEntry* BuyMode::catalogEntryOf(const vm::Object* l) const {
    if (!l || !l->definition)
        return nullptr;
    const CatalogEntry* entry = catalog_.byGuid(l->guid());
    if (!entry)
        entry = catalog_.find(l->definition->fileName);
    if (!entry)
        for (const auto& e : catalog_.entries())
            if (e.multiTile && e.group == l->group) {
                entry = &e;
                break;
            }
    return entry;
}

bool BuyMode::eyedrop(vm::ObjectId id) {
    const vm::Object* o = vm_.object(id);
    if (!o || o->person || !o->definition)
        return false;
    const vm::ObjectId lead = vm::multiTileGroup(*o);
    const vm::Object* l = vm_.object(lead);
    const CatalogEntry* entry = catalogEntryOf(l);
    if (!entry) {
        say(std::string(o->definition->fileName) + " is not in the catalog");
        sound("ui_error");
        return false;
    }
    if (!select(*entry))
        return false;
    tool_ = Tool::Hand; // the object tool takes over, as after a catalog click
    return true;
}

bool BuyMode::design(vm::ObjectId id, simd_float2 at) {
    const vm::Object* o = vm_.object(id);
    if (!o || o->person || !o->definition)
        return false;
    const vm::Object* lead = vm_.object(vm::multiTileGroup(*o));
    const CatalogEntry* entry = catalogEntryOf(lead);
    const std::string name = entry ? entry->name : o->definition->fileName;
    dirty_ = true;
    return design_->open(id, at, name);
}

bool BuyMode::click(simd_float3 eye, simd_float3 dir, std::optional<simd_float2> at) {
    if (!active_)
        return false;
    if (held_) {
        if (!mouseDown(eye, dir, at))
            return false;
        return mouseUp();
    }
    return mouseDown(eye, dir, at);
}

std::optional<simd_float2> BuyMode::groundUnder(simd_float3 eye, simd_float3 dir) const {
    if (const auto hit = rayHit(eye, dir))
        return simd_make_float2(hit->x, hit->y);
    return std::nullopt;
}

// The object tool's press (cTSUserToolObject): what is held stays on its
// tile while the button is down, and a drag turns it toward the mouse; the
// release drops it (OnRelease -> TryDrop, docs/buy-build-mac.md §3.1).
bool BuyMode::mouseDown(simd_float3 eye, simd_float3 dir, std::optional<simd_float2> at) {
    if (!active_)
        return false;
    if (design_->isOpen()) // a click off the dialog closes it, as Cancel
        design_->cancel(), dirty_ = true;
    const auto t = tileUnder(eye, dir);
    if (held_) {
        if (!t)
            return false;
        holdAt(t->x, t->y, direction_, true);
        pressing_ = true;
        turned_ = false;
        pressTile_ = *t;
        return true;
    }
    const vm::ObjectId id = objectAt(eye, dir, t);
    if (!id)
        return false;
    switch (tool_) {
    case Tool::Eyedropper: return eyedrop(id);
    case Tool::Design:
        return design(id, at.value_or(mouse_.value_or(simd_make_float2(float(canvasW_) / 2, float(canvasH_) / 2))));
    case Tool::Hand:
        if (!grab(id))
            return false;
        // Still down: a drag carries it, the release drops it (inferred:
        // TryGrab then OnRelease -> TryDrop, docs/buy-build-mac.md §3.1).
        grabPress_ = true, grabMoved_ = false;
        if (!last_.parts.empty())
            pressTile_ = last_.parts.front().tile;
        else if (t)
            pressTile_ = *t;
        return true;
    }
    return false;
}

bool BuyMode::pressAt(int x, int y) {
    if (!active_ || !held_)
        return false;
    holdAt(x, y, direction_, true);
    pressing_ = true;
    turned_ = false;
    pressTile_ = {x, y, level_};
    return true;
}

void BuyMode::mouseDrag(simd_float3 eye, simd_float3 dir) {
    if (grabPress_ && held_ && !pressing_) { // carrying what the press picked up
        if (auto t = tileUnder(eye, dir)) {
            grabMoved_ |= t->x != pressTile_.x || t->y != pressTile_.y;
            pressTile_ = *t;
            holdAt(t->x, t->y, direction_, true);
        }
        return;
    }
    if (!pressing_ || !held_)
        return;
    if (auto g = groundUnder(eye, dir))
        aimAt(*g);
}

void BuyMode::aimAt(simd_float2 ground) {
    if (!pressing_ || !held_)
        return;
    // Half a tile away from the tile's centre at least, so a shaky click
    // does not turn it. Facing the mouse, in 90° steps (TryTurn's steps of
    // 2: the diagonal selector is BV only, §3.5).
    const float dx = ground.x - (float(pressTile_.x) + 0.5f), dy = ground.y - (float(pressTile_.y) + 0.5f);
    if (dx * dx + dy * dy < 0.25f)
        return;
    const float heading = vm::headingOfMotion(dx, dy);
    int d = direction_;
    float best = 10;
    for (int k = 0; k < 8; k += 2)
        if (const float off = std::fabs(std::remainder(heading - vm::headingOfDirection(k), float(2 * M_PI))); off < best)
            best = off, d = k;
    if (d == direction_)
        return;
    direction_ = d;
    turned_ = true;
    sound("ui_object_rotate");
    holdAt(pressTile_.x, pressTile_.y, direction_);
}

bool BuyMode::mouseUp() {
    if (grabPress_) {
        grabPress_ = false;
        // Dragged elsewhere: dropped there; a click without a drag keeps it in the hand.
        if (held_ && !pressing_ && grabMoved_)
            return placeAt(pressTile_.x, pressTile_.y, direction_);
    }
    if (!pressing_)
        return false;
    pressing_ = false;
    turned_ = false;
    if (!held_)
        return false;
    return placeAt(pressTile_.x, pressTile_.y, direction_);
}

void BuyMode::rotate(int steps) {
    direction_ = ((direction_ + steps) % 8 + 8) % 8;
    if (held_)
        sound("ui_object_rotate");
    if (held_ && !last_.parts.empty()) {
        const vm::TilePos lead = last_.parts.front().tile;
        holdAt(lead.x, lead.y, direction_);
    }
}

void BuyMode::dropTwin() {
    if (!twin_)
        return;
    for (vm::ObjectId part : vm::groupOf(vm_, twin_))
        view_.forget(part);
    vm::deleteGroup(vm_, twin_);
    twin_ = 0;
}

void BuyMode::cancel() {
    pressing_ = turned_ = grabPress_ = grabMoved_ = false;
    dropTwin();
    if (!held_)
        return;
    say(heldNew_ ? "dropped what was held" : "put back object " + std::to_string(held_));
    if (heldNew_) {
        for (vm::ObjectId part : vm::groupOf(vm_, held_))
            view_.forget(part);
        vm::deleteGroup(vm_, held_);
    } else {
        if (heldFrom_.ok)
            vm::applyPlacement(vm_, held_, heldFrom_);
    }
    held_ = 0;
    last_ = {};
    dirty_ = true;
}

bool BuyMode::deleteHeld() {
    if (!held_)
        return false;
    if (heldNew_) { // never bought: it just goes
        cancel();
        return true;
    }
    const vm::ObjectId id = held_;
    // Bought in this session: destroyed at once, the price back (the
    // purchase leaves the history, §2).
    auto created = std::find_if(undo_.begin(), undo_.end(),
                                [&](const Op& op) { return op.kind == Op::Kind::Created && op.id == id; });
    if (created != undo_.end()) {
        vm::chargeFunds(vm_, created->cost);
        undo_.erase(std::remove_if(undo_.begin(), undo_.end(), [&](const Op& op) { return op.id == id; }),
                    undo_.end());
        for (vm::ObjectId part : vm::groupOf(vm_, id))
            view_.forget(part);
        vm::deleteGroup(vm_, id);
        held_ = 0;
        sound("ui_object_sell");
        say("returned object " + std::to_string(id) + " for its full price; funds " + money(vm::funds(vm_)));
        return true;
    }
    if (heldFrom_.ok)
        vm::applyPlacement(vm_, id, heldFrom_); // back where it was, for undo
    const int value = vm::sell(vm_, id);
    sound("ui_object_sell");
    undo_.push_back({Op::Kind::Deleted, id, heldFrom_, -value});
    recorded();
    held_ = 0;
    say("sold object " + std::to_string(id) + " for " + money(value) + "; funds " + money(vm::funds(vm_)));
    return true;
}

void BuyMode::recorded() {
    dropRedo();
    if (undoOrder)
        undoOrder->pushed(UndoOrder::Buy);
}

bool BuyMode::undo() {
    cancel();
    return undoOrder ? undoOrder->undo() : undoOwn();
}

bool BuyMode::redo() {
    cancel();
    return undoOrder ? undoOrder->redo() : redoOwn();
}

bool BuyMode::undoOwn() {
    cancel();
    if (undo_.empty())
        return false;
    Op op = undo_.back();
    undo_.pop_back();
    switch (op.kind) {
    case Op::Kind::Created: // out of the world, the price back
        op.placement = current(op.id);
        vm::pickUp(vm_, op.id);
        vm::chargeFunds(vm_, op.cost);
        break;
    case Op::Kind::Deleted: // back where it was, the refund paid back
        vm::applyPlacement(vm_, op.id, op.placement);
        vm::chargeFunds(vm_, op.cost);
        break;
    case Op::Kind::Moved: {
        const vm::Placement now = current(op.id);
        vm::applyPlacement(vm_, op.id, op.placement);
        op.placement = now;
        break;
    }
    case Op::Kind::Design: // the old design back, the price back
        DesignTool::apply(vm_, view_, op.id, op.before);
        vm::chargeFunds(vm_, op.cost);
        break;
    }
    redo_.push_back(op);
    sound("ui_undo");
    say("undone; funds " + money(vm::funds(vm_)));
    return true;
}

bool BuyMode::redoOwn() {
    cancel();
    if (redo_.empty())
        return false;
    Op op = redo_.back();
    redo_.pop_back();
    switch (op.kind) {
    case Op::Kind::Created:
        // RedoIt charges again (AdjustFunds -> cTSSimulator::Spend) without
        // a funds test: the funds may go below zero (docs/buy-build-mac.md
        // §7.25).
        vm::applyPlacement(vm_, op.id, op.placement);
        vm::chargeFunds(vm_, -op.cost);
        break;
    case Op::Kind::Deleted:
        op.placement = current(op.id);
        vm::pickUp(vm_, op.id);
        vm::chargeFunds(vm_, -op.cost);
        break;
    case Op::Kind::Moved: {
        const vm::Placement now = current(op.id);
        vm::applyPlacement(vm_, op.id, op.placement);
        op.placement = now;
        break;
    }
    case Op::Kind::Design:
        DesignTool::apply(vm_, view_, op.id, op.after);
        vm::chargeFunds(vm_, -op.cost);
        break;
    }
    undo_.push_back(op);
    sound("ui_redo");
    say("redone; funds " + money(vm::funds(vm_)));
    return true;
}

void BuyMode::update() {
    size_t used = 0;
    if (active_ && held_ && tileModels_[0] >= 0)
        for (const auto& part : last_.parts) {
            const int model = tileModels_[last_.ok ? 0 : 1];
            const float z = sim_.floorHeight(part.tile.level, float(part.tile.x) + 0.5f, float(part.tile.y) + 0.5f) +
                            0.02f;
            simd_float4x4 m = at(float(part.tile.x), float(part.tile.y), z);
            if (outlines_) { // centred on the tile, turned to the object's direction
                const float h = vm::headingOfDirection(last_.direction), c = std::cos(h), s = std::sin(h);
                m.columns[0] = simd_make_float4(c, s, 0, 0);
                m.columns[1] = simd_make_float4(-s, c, 0, 0);
                m.columns[3] = simd_make_float4(float(part.tile.x) + 0.5f, float(part.tile.y) + 0.5f, z, 1);
            }
            if (used == tileInstances_.size())
                tileInstances_.push_back(renderer_.addInstance(model, m));
            else
                renderer_.setInstanceModel(tileInstances_[used], model), renderer_.setInstanceTransform(tileInstances_[used], m);
            ++used;
        }
    for (size_t i = used; i < tileInstances_.size(); ++i)
        renderer_.setInstanceTransform(tileInstances_[i], at(0, 0, -1000));
}

// --- The panel ---------------------------------------------------------------

std::vector<const CatalogEntry*> BuyMode::items() const {
    if (sort_ == Sort::Function && function_ >= 0) {
        std::vector<const CatalogEntry*> out;
        for (const auto* e : catalog_.byFunction(function_))
            if (subSort_ <= 0 || (e->functionSubSort & subSort_))
                out.push_back(e);
        return out;
    }
    if (sort_ == Sort::Room && room_ >= 0) {
        std::vector<const CatalogEntry*> out;
        for (const auto* e : catalog_.byRoom(room_))
            if (roomFunction_ == 0 || (e->functionSort & roomFunction_))
                out.push_back(e);
        return out;
    }
    return {};
}

UiScreen BuyMode::screen(int width, int height) {
    UiScreen out;
    out.width = width, out.height = height;
    laidW_ = width, laidH_ = height;
    targets_.clear();
    solid_.clear();
    frameImages_.clear();
    if (!ok_ || !active_)
        return out;
    std::map<const UiElement*, std::array<float, 4>> rects;
    auto place = [&](const UiElement& e, float x, float y, UiDrawHooks& hk) {
        const auto a = e.area();
        hk.rects = &rects;
        layoutUiElement(e, x - float(a[0]), y - float(a[1]), *res_, *fonts_, out, hk);
    };
    auto rectOf = [&](const UiElement* e) -> std::optional<std::array<float, 4>> {
        if (!e)
            return std::nullopt;
        auto it = rects.find(e);
        return it == rects.end() ? std::nullopt : std::optional(it->second);
    };
    auto target = [&](const UiElement* e, Action a, int v) {
        if (auto r = rectOf(e))
            targets_.push_back({*r, a, v});
    };
    auto text = [&](const std::string& s, float x, float y, float w, float h, const char* align,
                    const char* color = "(5,6,113)") {
        UiElement t;
        t.tag = "LEGACY";
        t.attrs = {{"clsid", "GZWinText"}, {"caption", s},          {"font", "LiveModePanelPuck"},
                   {"forecolor", color},    {"align", align},       {"winflag_visible", "yes"},
                   {"area", area(0, 0, int(w), int(h))}};
        UiDrawHooks th;
        layoutUiElement(t, x, y, *res_, *fonts_, out, th);
    };

    // Joined to the puck as the live panel is (live_ui.cpp, inferred): the
    // panel's hidden rectangle captioned "4bdeffaa:bl" over the puck's
    // window 0x4BDEFFAA, the puck at the client's left edge (measured at
    // both 800x600 and 1280x720, docs/buy-build-mac.md §1.6).
    const UiElement& puck = puck_.roots.front();
    // The catalog keeps the script's place (x 447, over the strip's own end)
    // and the strip its own width there; the end cap closes the bar at the
    // sort area's end when there is nothing to show (the top-level sorts),
    // and, with the catalog open, sits at its own place over the window's
    // left, where its grey closes the seam between the bar and the window
    // (532..536 over the window's cap at 537, measured at 800x600 and
    // 1280x720; docs/buy-build-mac.md §1.6; as Build mode's panel,
    // build_mode_panel.cpp). The joint ("center piece", 0x4CC48AA8) draws
    // nowhere in either state (the script hides it and the seam shows only
    // the cap's own greys), so it keeps its script's place and flag.
    const auto list = items();
    const bool showCatalog = !list.empty();
    {
        UiElement& r = panel_.roots.front();
        const UiElement* sortArea = r.child(sort_ == Sort::Function ? kFunctionSort : kRoomSort);
        const int right = sortArea ? sortArea->area()[2] : 447;
        if (UiElement* e = r.child(0x4CC48AA4)) { // the end cap: at the sort's end, or over the window's left
            const auto a = e->area();
            const int x = showCatalog ? 447 : right;
            e->set("area", area(x, a[1], x + (a[2] - a[0]), a[3]));
        }
        // The bar's repeat piece (0x4CC48AA6): the script's own 265..447 with
        // the catalog open, reaching the end cap at the sort's own end when
        // there is nothing to show — the original's grey runs, in a stretched
        // gradient, all the way to the cap's piece (measured at 800x600,
        // docs/buy-build-mac.md §1.6).
        if (UiElement* t = r.child(0x4CC48AA6)) {
            const auto a = t->area();
            const int end = showCatalog ? 447 : right;
            t->set("area", area(a[0], a[1], std::max(a[0], end), a[3]));
        }
    }
    const UiElement& root = panel_.roots.front();
    const auto pa = puck.area();
    // The original's pod sits 3 px above the script's own reach to the
    // client's foot: the pill's art at 516 and 396 at 720 and 600 over the
    // 519/399 of the script's place (measured in Buy, Build and Live alike;
    // docs/buy-build-mac.md §1.6).
    const float puckX = 0, puckY = float(height) - float(pa[3] - pa[1]) - 3;
    float x0 = puckX, y0 = puckY - 137;
    if (const UiElement* a = puck.child(0x4BDEFFAA))
        if (const UiElement* b = root.child(0x0000AAAA))
            x0 = puckX + float(a->area()[0] - b->area()[0]), y0 = puckY + float(a->area()[1] - b->area()[1]);

    UiDrawHooks hk;
    const bool byFunction = sort_ == Sort::Function;
    for (uint32_t id : {kBgFunction, kBgRoom, kBgCollections, kCollections})
        if (const UiElement* e = root.child(id))
            (id == (byFunction ? kBgFunction : kBgRoom) ? hk.show : hk.hide).insert(e);
    if (const UiElement* e = root.child(kTabFunction))
        hk.buttonState[e] = byFunction ? 2 : 1;
    if (const UiElement* e = root.child(kTabRoom))
        hk.buttonState[e] = byFunction ? 1 : 2;
    if (const UiElement* e = root.child(kTabCollections)) // collections are Bon Voyage's
        hk.hide.insert(e);
    if (const UiElement* e = root.child(kCatalog)) // out with nothing to show (as Build mode's panel)
        (showCatalog ? hk.show : hk.hide).insert(e);
    // The end cap draws in both states (its own place over the window's left
    // with the catalog open, the sort's end without: set above), and the
    // joint stays as the script leaves it (hidden).
    if (const UiElement* e = root.child(kToolHand))
        hk.buttonState[e] = (held_ && !heldNew_) || (tool_ == Tool::Hand && !held_) ? 2 : 1;
    if (const UiElement* e = root.child(kToolEyedropper))
        hk.buttonState[e] = tool_ == Tool::Eyedropper ? 2 : 1;
    if (const UiElement* e = root.child(kToolDesign))
        hk.buttonState[e] = tool_ == Tool::Design ? 2 : 1;
    if (const UiElement* e = root.child(kTopDown))
        hk.buttonState[e] = topDown && topDown() ? 2 : 1;
    if (const UiElement* e = root.child(kUndo))
        hk.buttonState[e] = undo_.empty() ? 0 : 1;
    if (const UiElement* e = root.child(kRedo))
        hk.buttonState[e] = redo_.empty() ? 0 : 1;
    // The sort area: the category buttons, or the chosen category's subsorts.
    if (const UiElement* fs = root.child(kFunctionSort)) {
        (byFunction ? hk.show : hk.hide).insert(fs);
        const uint32_t open = function_ < 0 ? kFunctionButtons : uint32_t(0xB000 + (function_ + 1) * 0x100);
        for (const auto& c : fs->children)
            (c.id() == open ? hk.show : hk.hide).insert(&c);
        if (function_ >= 0)
            if (const UiElement* panel = fs->child(uint32_t(0xB000 + (function_ + 1) * 0x100)))
                for (const auto& b : panel->children) {
                    const int n = int(b.id() >> 4 & 0xF);
                    const int ss = subSortOf(function_ + 1, n);
                    if (ss >= 0 && ss == subSort_ && n != 1)
                        hk.buttonState[&b] = 2;
                }
    }
    if (const UiElement* rs = root.child(kRoomSort)) {
        (byFunction ? hk.hide : hk.show).insert(rs);
        if (const UiElement* c = rs->child(kRoomButtons))
            (room_ < 0 ? hk.show : hk.hide).insert(c);
        if (const UiElement* d = rs->child(kRoomFunctions)) {
            (room_ < 0 ? hk.hide : hk.show).insert(d);
            for (const auto& c : d->children) {
                const uint32_t id = c.id();
                if (id >= 0xE100 && id <= 0xE900) { // the chosen room's header
                    const int k = int(id >> 8 & 0xF);
                    (room_ >= 0 && kRoomBits[size_t(k - 1)] == room_ ? hk.show : hk.hide).insert(&c);
                }
                if (id >= 0xD100 && id <= 0xD900 && roomFunction_ == (1 << ((id >> 8 & 0xF) - 1)))
                    hk.buttonState[&c] = 2;
            }
        }
    }
    // The grid: the current page's cells, the selected one marked, the ones
    // the family cannot afford dimmed (UpdateAffordability, §1.5: frame 0,
    // inferred). The list was taken above, for the catalog's show state.
    const int columns = catalogColumns(float(width));
    const int rows = expanded_ ? kExpandedRows : kRows;
    const size_t perPage = size_t(columns * rows);
    const size_t pages = std::max<size_t>(1, (list.size() + perPage - 1) / perPage);
    page_ = std::min(page_, pages - 1);
    const int64_t funds = vm::noCharge(vm_) ? INT64_MAX : vm::funds(vm_); // no charge: nothing is too dear
    std::vector<std::pair<std::array<float, 4>, const CatalogEntry*>> cells;
    const UiElement* catalog = root.child(kCatalog);
    const UiElement* collapsed = catalog ? catalog->child(expanded_ ? kExpanded : kCollapsed) : nullptr;
    if (catalog)
        if (const UiElement* other = catalog->child(expanded_ ? kCollapsed : kExpanded))
            hk.hide.insert(other);
    // The catalog's right side rides its stretched end: the strip's last cap
    // lands with its right edge at width-30 with the middle piece filling up
    // to it, and the next and expand buttons over the cap — their right edge
    // 5 px in (their glyphs at width-61; measured at 800x600 and 1280x720,
    // docs/buy-build-mac.md §1.6). The left cap and the prev arrow keep the
    // script's place.
    if (UiElement* cat = panel_.roots.front().child(kCatalog))
        if (UiElement* win = cat->child(expanded_ ? kExpanded : kCollapsed)) {
            // The original's window sits 5 px right of the script's own slot:
            // its left cap at 537 while the bar's own pieces keep the
            // anchors' place (the sort's buttons at 361, the end cap at 588
            // and the join at 85, measured at 800x600 and 1280x720;
            // docs/buy-build-mac.md §1.6). The window carries its pieces, so
            // the cap/middle/arrows ride the same 5 px and the conversions
            // below move with the base. The window's own left in the script
            // is 0, so the 5 is set outright (never compounded frame over
            // frame).
            const auto wa = win->area();
            win->set("area", area(5, wa[1], 5 + (wa[2] - wa[0]), wa[3]));
            const float base = x0 + 452; // the catalog window's left on the client
            const float capL = float(width) - kStripRight - kCapW;
            const uint32_t d = expanded_ ? 0x2000 : 0x1000;
            if (UiElement* e = win->child(d | 2)) { // middle: up to the cap
                const auto a = e->area();
                e->set("area", area(a[0], a[1], int(capL - base), a[3]));
            }
            if (UiElement* e = win->child(d | 3)) { // cap: at the end
                const auto a = e->area();
                e->set("area", area(int(capL - base), a[1], int(capL + kCapW - base), a[3]));
            }
            for (uint32_t k : {d | 5, d | 9}) // next and expand/collapse: over the cap
                if (UiElement* e = win->child(k)) {
                    const auto a = e->area();
                    e->set("area", area(int(capL + 5 - base) - (a[2] - a[0]), a[1], int(capL + 5 - base), a[3]));
                }
        }

    // The cells' plates, drawn by the game in code (the grid window has no
    // art of its own): a 44 px rounded rect (radius 8) of the fill over a
    // 2 px navy edge; the hovered and the selected one take a ring in its
    // place — 1 px of the dark outer line, 2 px of the colour and 1 px of
    // navy over the fill (sampled from the original's screenshots: the
    // fill (143,153,216), the navy (5,6,113), the rings' outer line
    // (43,50,127), the hover (204,214,255), the selection (0,255,0)).
    auto makePlate = [&](const uint8_t* ring) {
        constexpr int S = 4; // supersampled, for the rounded corners
        RgbaImage img;
        img.width = uint32_t(kCell), img.height = uint32_t(kCell);
        img.pixels.assign(size_t(kCell) * size_t(kCell) * 4, 0);
        for (int py = 0; py < int(kCell); ++py)
            for (int px = 0; px < int(kCell); ++px) {
                float r = 0, g = 0, b = 0;
                int n = 0;
                for (int sy = 0; sy < S; ++sy)
                    for (int sx = 0; sx < S; ++sx) {
                        const float qx = float(px) + (float(sx) + 0.5f) / S, qy = float(py) + (float(sy) + 0.5f) / S;
                        const float ix = std::min(qx, kCell - qx), iy = std::min(qy, kCell - qy);
                        float d = std::min(ix, iy);
                        if (ix < kCellRadius && iy < kCellRadius) {
                            const float dx = kCellRadius - ix, dy = kCellRadius - iy;
                            d = kCellRadius - std::sqrt(dx * dx + dy * dy);
                        }
                        if (d < 0)
                            continue; // outside the plate
                        float cr = 143, cg = 153, cb = 216;
                        if (!ring) {
                            if (d < 2)
                                cr = 5, cg = 6, cb = 113;
                        } else if (d < 1) {
                            cr = 43, cg = 50, cb = 127;
                        } else if (d < 3) {
                            cr = ring[0], cg = ring[1], cb = ring[2];
                        } else if (d < 4) {
                            cr = 5, cg = 6, cb = 113;
                        }
                        r += cr, g += cg, b += cb, ++n;
                    }
                uint8_t* o = &img.pixels[(size_t(py) * size_t(kCell) + size_t(px)) * 4];
                if (n)
                    o[0] = uint8_t(r / n), o[1] = uint8_t(g / n), o[2] = uint8_t(b / n), o[3] = uint8_t(255 * n / (S * S));
            }
        frameImages_.push_back(std::make_unique<RgbaImage>(std::move(img)));
        return frameImages_.back().get();
    };
    static const uint8_t kRingHover[3] = {204, 214, 255}, kRingSelect[3] = {0, 255, 0};
    const RgbaImage* plateNormal = makePlate(nullptr);
    const RgbaImage* plateHover = makePlate(kRingHover);
    const RgbaImage* plateSelect = makePlate(kRingSelect);

    // The page slider (window 0x1008 of 0x1000): a 12 px tall rounded bar
    // under the cells — its left edge at the first column's left, its right
    // one px short of the block's end (the navy closing at 1207 with the
    // paper at 1208, measured) — and a knob of the page's share of the
    // block at its head (the knob 6 px wider than that fill; measured: 304
    // px of fill for a 26 of 53 list); the original draws it at the cells'
    // block, not at the script's own rectangle (4 px in and a row lower;
    // its corners 4 px — the outermost column spans 8 of the 12 rows — the
    // track (135,163,152) under a 1 px navy edge on its top and bottom with
    // the fill reaching the ends, the knob (174,189,255) under 2 px of navy
    // on all its edges with a (157,171,241) inner on the ends).
    auto makeSlider = [&](float w, bool knob) {
        constexpr int S = 4;
        RgbaImage img;
        img.width = uint32_t(w), img.height = 12;
        img.pixels.assign(size_t(w) * 12 * 4, 0);
        // A point inside a rounded rectangle (its corners at rr).
        auto inRound = [](float qx, float qy, float x0, float x1, float y0, float y1, float rr) {
            if (qx < x0 || qx > x1 || qy < y0 || qy > y1)
                return false;
            const float dx = std::max(std::max(x0 + rr - qx, qx - (x1 - rr)), 0.0f);
            const float dy = std::max(std::max(y0 + rr - qy, qy - (y1 - rr)), 0.0f);
            return dx * dx + dy * dy <= rr * rr;
        };
        for (int py = 0; py < 12; ++py)
            for (int px = 0; px < int(w); ++px) {
                float r = 0, g = 0, b = 0;
                int n = 0;
                for (int sy = 0; sy < S; ++sy)
                    for (int sx = 0; sx < S; ++sx) {
                        const float qx = float(px) + (float(sx) + 0.5f) / S, qy = float(py) + (float(sy) + 0.5f) / S;
                        if (!inRound(qx, qy, 0, w, 0, 12, 4))
                            continue;
                        float cr = 135, cg = 163, cb = 152;
                        if (knob) {
                            cr = 174, cg = 189, cb = 255;
                            if (!inRound(qx, qy, 2, w - 2, 2, 10, 2))
                                cr = 5, cg = 6, cb = 113;
                            else if (!inRound(qx, qy, 3, w - 3, 2, 10, 2))
                                cr = 157, cg = 171, cb = 241;
                        } else if (!inRound(qx, qy, 0, w, 1, 11, 3))
                            cr = 5, cg = 6, cb = 113;
                        r += cr, g += cg, b += cb, ++n;
                    }
                uint8_t* o = &img.pixels[(size_t(py) * size_t(w) + size_t(px)) * 4];
                if (n)
                    o[0] = uint8_t(r / n), o[1] = uint8_t(g / n), o[2] = uint8_t(b / n), o[3] = uint8_t(255 * n / (S * S));
            }
        frameImages_.push_back(std::make_unique<RgbaImage>(std::move(img)));
        return frameImages_.back().get();
    };
    auto drawGrid = [&](UiScreen& s, float x, float y) {
        for (size_t i = 0; i < perPage && page_ * perPage + i < list.size(); ++i) {
            const CatalogEntry* e = list[page_ * perPage + i];
            const float cx = x + kGridX + kCellInX + float(i % columns) * kCellStep;
            const float cy = y + kGridY + kCellInY + float(i / columns) * kCellStep;
            UiQuad q;
            q.image = e == selected_ ? plateSelect : e == hovered_ ? plateHover : plateNormal;
            q.x0 = cx, q.y0 = cy, q.x1 = cx + kCell, q.y1 = cy + kCell;
            s.quads.push_back(q);
            if (const RgbaImage* img = catalog_.thumbnail(*e)) {
                UiQuad t;
                t.image = img;
                t.x0 = cx + 2, t.y0 = cy + 2, t.x1 = cx + 2 + kThumb, t.y1 = cy + 2 + kThumb;
                if (e->price > funds)
                    t.color = {0.45f, 0.45f, 0.45f, 0.8f};
                s.quads.push_back(t);
            }
            cells.push_back({{cx, cy, cx + kCell, cy + kCell}, e});
        }
        const float ty = y + kGridY + kCellInY + float(rows - 1) * kCellStep + kCell + 4;
        const float trackX = x + kGridX + kCellInX;
        const float trackW = float(columns) * kCellStep - 5;
        const float blockW = float(columns) * kCellStep - 4; // the cells' block
        const float fill = list.empty() ? blockW
                                        : std::clamp(float(int(blockW * float(perPage) / float(list.size()))), 4.0f, trackW - 6);
        const float knobW = fill + 6;
        UiQuad track;
        track.image = makeSlider(trackW, false);
        track.x0 = trackX, track.y0 = ty, track.x1 = trackX + trackW, track.y1 = ty + 12;
        s.quads.push_back(track);
        const float head = pages > 1 ? float(page_) / float(pages - 1) : 0;
        UiQuad knob;
        knob.image = makeSlider(knobW, true);
        knob.x0 = trackX + head * (trackW - knobW), knob.y0 = ty, knob.x1 = knob.x0 + knobW, knob.y1 = ty + 12;
        s.quads.push_back(knob);
    };
    // The original shows no prev arrow on the first page (measured at 800x600
    // and 1280x720; the row's start sits without it).
    if (collapsed && page_ == 0)
        if (const UiElement* e = collapsed->child(expanded_ ? kExpandedPrev : kPrev))
            hk.hide.insert(e);
    // The original's grid sits at the window's own left (the script's slot
    // plus the window's 1 px, above) and 4 px higher (measured at 1280x720
    // against the script's own place).
    place(root, x0, y0, hk);
    if (auto r = rectOf(collapsed))
        drawGrid(out, (*r)[0], (*r)[1] - 4);
    for (const auto& [r, e] : cells)
        targets_.push_back({r, Action::Item, int(std::find(list.begin(), list.end(), e) - list.begin())});
    // Targets.
    target(root.child(kTabFunction), Action::Tab, 0);
    target(root.child(kTabRoom), Action::Tab, 1);
    target(root.child(kToolHand), Action::Tool, 0);
    target(root.child(kToolEyedropper), Action::Tool, 1);
    target(root.child(kToolDesign), Action::Tool, 2);
    target(root.child(kDayNight), Action::DayNight, 0);
    target(root.child(kTopDown), Action::TopDown, 0);
    target(root.child(kUndo), Action::Undo, 0);
    target(root.child(kRedo), Action::Redo, 0);
    if (collapsed) {
        if (page_ > 0) // no prev arrow on the first page (as the original)
            target(collapsed->child(expanded_ ? kExpandedPrev : kPrev), Action::Prev, 0);
        target(collapsed->child(expanded_ ? kExpandedNext : kNext), Action::Next, 0);
        target(collapsed->child(expanded_ ? kCollapse : kExpand), Action::Expand, 0);
    }
    if (const UiElement* fs = root.child(kFunctionSort)) {
        if (const UiElement* c = fs->child(kFunctionButtons))
            for (const auto& b : c->children)
                target(&b, Action::Function, int(b.id() >> 8 & 0xF) - 1);
        if (function_ >= 0)
            if (const UiElement* panel = fs->child(uint32_t(0xB000 + (function_ + 1) * 0x100)))
                for (const auto& b : panel->children) {
                    const int n = int(b.id() >> 4 & 0xF);
                    if (n == 1)
                        target(&b, Action::Back, 0);
                    else if (const int ss = subSortOf(function_ + 1, n); ss >= 0)
                        target(&b, Action::SubSort, ss);
                }
    }
    if (const UiElement* rs = root.child(kRoomSort)) {
        if (const UiElement* c = rs->child(kRoomButtons))
            for (const auto& b : c->children)
                target(&b, Action::Room, kRoomBits[size_t((b.id() >> 8 & 0xF) - 1)]);
        if (const UiElement* d = rs->child(kRoomFunctions))
            for (const auto& b : d->children) {
                const uint32_t id = b.id();
                if (id == kRoomBack)
                    target(&b, Action::Back, 0);
                else if (id >= 0xD100 && id <= 0xD900)
                    target(&b, Action::RoomFunction, 1 << ((id >> 8 & 0xF) - 1));
            }
    }
    for (const auto& [e, r] : rects)
        if (e->clsid() == "GZWinBMP" || e->clsid() == "GZWinGen" || e->clsid().rfind("0x", 0) == 0)
            solid_.push_back(r);

    // The hovered item's price tip (§), over the cell's top edge (the
    // original's: a 47 x 21 plate at (cell - (2, 10)), a 1 px navy edge
    // (30,43,113) over the beige fill (223,207,145), the text in black).
    for (const auto& [r, e] : cells)
        if (e == hovered_) {
            const int S = 4, W = 47, H = 21;
            RgbaImage img;
            img.width = W, img.height = H;
            img.pixels.assign(size_t(W) * H * 4, 0);
            for (int py = 0; py < H; ++py)
                for (int px = 0; px < W; ++px) {
                    float cr = 0, cg = 0, cb = 0;
                    int n = 0;
                    for (int sy = 0; sy < S; ++sy)
                        for (int sx = 0; sx < S; ++sx) {
                            const float qx = float(px) + (float(sx) + 0.5f) / S, qy = float(py) + (float(sy) + 0.5f) / S;
                            const float ix = std::min(qx, float(W) - qx), iy = std::min(qy, float(H) - qy);
                            float d = std::min(ix, iy);
                            if (ix < 3 && iy < 3) {
                                const float dx = 3 - ix, dy = 3 - iy;
                                d = 3 - std::sqrt(dx * dx + dy * dy);
                            }
                            if (d < 0)
                                continue;
                            float r = 223, g = 207, b = 145;
                            if (d < 1)
                                r = 30, g = 43, b = 113;
                            cr += r, cg += g, cb += b, ++n;
                        }
                    uint8_t* o = &img.pixels[(size_t(py) * W + size_t(px)) * 4];
                    if (n)
                        o[0] = uint8_t(cr / n), o[1] = uint8_t(cg / n), o[2] = uint8_t(cb / n), o[3] = uint8_t(255 * n / (S * S));
                }
            frameImages_.push_back(std::make_unique<RgbaImage>(std::move(img)));
            UiQuad q;
            q.image = frameImages_.back().get();
            q.x0 = r[0] - 2, q.y0 = r[1] - 10, q.x1 = r[0] + 45, q.y1 = r[1] + 11;
            out.quads.push_back(q);
            text(money(e->price), r[0] - 1, r[1] - 8, 45, 17, "center", e->price > funds ? "(150,40,40)" : "(1,1,1)");
            break;
        }

    // The catalog description of the item bought and held, above the panel
    // (inferred placement; the original shows it with an item selected —
    // its screenshots — not on a bare hover).
    if (const CatalogEntry* e = held_ && selected_ ? selected_ : nullptr) {
        UiElement& dm = description_.roots.front();
        if (UiElement* n = dm.child(0x4000))
            n->set("area", "(158,36,456,120)"), n->set("wrapped", "yes");
        if (UiElement* n = dm.child(0x5000))
            n->set("winflag_visible", "no");
        const UiElement& d = dm;
        const auto da = d.area();
        const float dx = x0 + 205, dy = y0 + (expanded_ ? kExpandedTop : 96) - float(da[3] - da[1]) - 4;
        UiDrawHooks dh;
        if (const UiElement* n = d.child(0x3000))
            dh.captions[n] = e->name;
        if (const UiElement* n = d.child(0x4000))
            dh.captions[n] = e->description;
        // The ratings (GetCatalogRating: fields 82-88), named as the needs
        // panel names the motives (Live.package STR# 0x82).
        std::string ratings;
        const std::array<int, 7> captions = {2, 6, 10, 14, 4, 8, 16};
        for (size_t i = 0; i < e->ratings.size(); ++i)
            if (e->ratings[i])
                if (auto s = res_->string({0x7F96C284, 0x82, uint32_t(captions[i])}))
                    ratings += (ratings.empty() ? "" : "   ") + s->substr(0, s->find('|')) + " " +
                               (e->ratings[i] > 0 ? "+" : "") + std::to_string(e->ratings[i]);
        // The description wraps down to the ratings line, which takes the
        // debug line's place (inferred: the game's text layout is in code).
        if (const UiElement* n = d.child(0x6000))
            dh.captions[n] = ratings;
        if (const UiElement* f = d.child(0x8000))
            if (const UiElement* img = f->child(0x8001))
                if (const RgbaImage* t = catalog_.thumbnail(*e))
                    dh.images[img] = t;
        for (uint32_t id : {0x9000u, 0xA000u})
            if (const UiElement* g = d.child(id))
                dh.hide.insert(g);
        dh.rects = &rects;
        layoutUiElement(d, dx - float(da[0]), dy - float(da[1]), *res_, *fonts_, out, dh);
        if (const UiElement* pb = d.child(0x0002))
            if (auto r = rectOf(pb))
                text(money(e->price), (*r)[0], (*r)[1], (*r)[2] - (*r)[0], (*r)[3] - (*r)[1], "center",
                     e->price > funds ? "(150,40,40)" : "(5,6,113)");
    }
    // Why the held object does not fit, by the mouse (the game's error
    // callout; its look is inferred).
    if (held_ && !last_.ok && last_.error) {
        const std::string why = errorText(last_);
        const std::string shown = why.substr(0, why.find(" ("));
        const simd_float2 m = mouse_.value_or(simd_make_float2(float(width) / 2, float(height) / 2));
        UiQuad q;
        const float boxW = 8 * float(shown.size()) + 12;
        q.x0 = std::max(4.0f, std::min(m.x + 14, float(width) - boxW - 4)); // never past the window's edge
        q.y0 = std::min(m.y + 14, float(height) - 26);
        q.x1 = q.x0 + boxW, q.y1 = q.y0 + 22;
        q.color = {178 / 255.0f, 193 / 255.0f, 1, 0.9f};
        out.quads.push_back(q);
        text(shown, q.x0, q.y0 + 3, q.x1 - q.x0, 18, "center");
    }
    // The object design dialog over everything.
    design_->layout(out, width, height);
    windows_.capture(rects); // for the tutorial (controlRect)
    return out;
}

void BuyMode::layoutIfNeeded() {
    if (canvasW_ > 0 && canvasH_ > 0 && (laidW_ != canvasW_ || laidH_ != canvasH_ || targets_.empty() || dirty_))
        screen(canvasW_, canvasH_);
}

bool BuyMode::press(simd_float2 p) {
    if (!ok_ || !active_)
        return false;
    layoutIfNeeded();
    if (design_->press(p)) {
        dirty_ = true;
        return true;
    }
    // A control the tutorial turned off takes the click and does nothing.
    if (allowPress && !allowPress(p))
        for (const Target& t : targets_)
            if (inside(t.rect, p))
                return true;
    for (const Target& t : std::vector<Target>(targets_)) {
        if (!inside(t.rect, p))
            continue;
        switch (t.action) {
        case Action::DayNight:
            if (onDayNight)
                onDayNight();
            break;
        case Action::TopDown:
            if (onTopDown)
                onTopDown();
            break;
        case Action::Tab:
            sort_ = t.value == 0 ? Sort::Function : Sort::Room;
            function_ = room_ = -1, subSort_ = -1, roomFunction_ = 0, page_ = 0;
            break;
        case Action::Function: function_ = t.value, subSort_ = -1, page_ = 0; break;
        case Action::SubSort: subSort_ = t.value, page_ = 0; break;
        case Action::Room: room_ = t.value, roomFunction_ = 0, page_ = 0; break;
        case Action::RoomFunction:
            roomFunction_ = roomFunction_ == t.value ? 0 : t.value, page_ = 0;
            if (items().empty()) // AnyObjectsByRoom: nothing there, back to all
                roomFunction_ = 0;
            break;
        case Action::Back:
            if (sort_ == Sort::Function)
                function_ = -1, subSort_ = -1;
            else
                room_ = -1, roomFunction_ = 0;
            page_ = 0;
            break;
        case Action::Expand: {
            // The first item shown stays on the page.
            const int cols = catalogColumns(float(canvasW_));
            const size_t first = page_ * size_t(cols * (expanded_ ? kExpandedRows : kRows));
            expanded_ = !expanded_;
            page_ = first / size_t(cols * (expanded_ ? kExpandedRows : kRows));
            sound(expanded_ ? "ui_panel_expand" : "ui_panel_collapse");
            break;
        }
        case Action::Prev: page_ = page_ ? page_ - 1 : 0; break;
        case Action::Next: ++page_; break;
        case Action::Item: {
            const auto list = items();
            if (size_t(t.value) < list.size())
                select(*list[size_t(t.value)]);
            break;
        }
        case Action::Tool: setTool(Tool(t.value)); break;
        case Action::Undo: undo(); break;
        case Action::Redo: redo(); break;
        case Action::None: break;
        }
        std::printf("opensims2: buy panel: %s %d (%zu items)\n",
                    t.action == Action::Item ? "item" : t.action == Action::Function ? "function"
                    : t.action == Action::SubSort ? "subsort" : t.action == Action::Room ? "room" : "button",
                    t.value, items().size());
        std::fflush(stdout);
        dirty_ = true;
        return true;
    }
    for (const auto& r : solid_)
        if (inside(r, p)) {
            if (std::getenv("OS2_DEBUG_BUY"))
                std::fprintf(stderr, "buy: press (%.0f, %.0f) taken by panel rect (%.0f %.0f %.0f %.0f)\n", p.x, p.y, r[0], r[1], r[2], r[3]);
            return true;
        }
    return false;
}

bool BuyMode::overPanel(simd_float2 p) const {
    if (!active_)
        return false;
    if (design_->isOpen())
        return true;
    for (const auto& t : targets_)
        if (inside(t.rect, p))
            return true;
    for (const auto& r : solid_)
        if (inside(r, p))
            return true;
    return false;
}

void BuyMode::hover(simd_float2 p) {
    mouse_ = p;
    if (!active_)
        return;
    layoutIfNeeded();
    const CatalogEntry* over = nullptr;
    const auto list = items();
    for (const Target& t : targets_)
        if (t.action == Action::Item && inside(t.rect, p) && size_t(t.value) < list.size())
            over = list[size_t(t.value)];
    if (over != hovered_)
        hovered_ = over, dirty_ = true;
    if (held_ && !last_.ok)
        dirty_ = true;
}

#ifndef OS2_SDL_GPU
void BuyMode::attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat) {
    for (auto& l : layers_)
        l = std::make_unique<UiLayer>(device, colorFormat, depthFormat);
}

void BuyMode::encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale) {
    if (!layers_[0] || width == 0 || height == 0 || !ok_ || !active_)
        return;
    const int cw = int(std::lround(float(width) / std::max(scale, 0.01f)));
    const int ch = int(std::lround(float(height) / std::max(scale, 0.01f)));
    if (cw != laidW_ || ch != laidH_)
        dirty_ = true;
    canvasW_ = cw, canvasH_ = ch;
    if (dirty_) {
        layer_ = (layer_ + 1) % layers_.size();
        layers_[layer_]->setScreen(screen(cw, ch));
        dirty_ = false;
    }
    layers_[layer_]->encode(enc, width, height);
}
#endif

std::optional<std::array<float, 4>> BuyMode::controlRect(uint32_t window, uint32_t control) {
    if (!active_)
        return std::nullopt;
    layoutIfNeeded();
    return windows_.find(window, control);
}

} // namespace os2
