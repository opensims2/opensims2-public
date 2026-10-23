#pragma once

// Buy mode over the lot (plain C++, driven from main.mm): the game's Buy
// panel (UI script 0x49060004, cTSWinProcBuyPanel) with the catalog browser
// (by function and subsort, by room and function), the catalog description
// (0x49063004) of the item under the mouse, and the object tool
// (cTSUserToolObject): the picked object, created at once, follows the
// mouse on the lot grid with the game's footprint outline (green or red,
// a chevron at the front), turns in 90°
// steps (keys, or a drag while the button is down), is bought on the
// release; the hand picks up placed objects to move
// or sell them; undo and redo. What the game does is docs/buy-build-mac.md;
// the placement rules are vm/placement.hpp. While Buy mode is on the
// simulation is paused, as in the game.

#include "app/ui_panel_util.hpp"
#include "app/undo_order.hpp"
#include "formats/catalog.hpp"
#include "formats/ui_screen.hpp"
#include "vm/placement.hpp"

#include <simd/simd.h>

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace MTL {
class Device;
class RenderCommandEncoder;
} // namespace MTL

namespace os2 {

class FontLibrary;
class LotSimulation;
class Renderer;
class ResourceIndex;
class UiLayer;
class UiResources;
class VmObjectView;

class DesignTool;

class BuyMode {
public:
    BuyMode(LotSimulation& simulation, VmObjectView& view, Renderer& renderer, ResourceIndex& index,
            const std::filesystem::path& resDir);
    ~BuyMode();

    bool ok() const { return ok_; }
    bool active() const { return active_; }
    // Entering pauses the simulation; leaving drops what the hand holds and
    // restores the speed. The history stays (the game's one undo manager
    // goes on in Build mode) when `undoOrder` is set; without it leaving
    // flushes it.
    void enter();
    void exit();
    // The undo manager shared with Build mode (app/undo_order.hpp); the host
    // attaches this mode's hooks to it.
    UndoOrder* undoOrder = nullptr;
    // ReleaseUndoList / ReleaseRedoList (FlushCommandQueues): what was sold
    // and the purchases undone die now (ResetSelf).
    void flushHistory();
    // The redo list released (another change, here or in Build mode).
    void dropRedo();
    // This mode's own last change undone / redone (the undo manager's hooks).
    bool undoOwn();
    bool redoOwn();

    Catalog& catalog() { return catalog_; }
    // Picks a catalog item: its object is created now (Init run) and held.
    bool select(const CatalogEntry& entry);
    // The level objects go on (1 = the lot's ground level).
    void setLevel(int level) { level_ = level; }
    int level() const { return level_; }
    // Page Up / Page Down and the puck's floor buttons: one floor up or
    // down, within the lot's levels. Returns whether it changed.
    bool changeLevel(int delta);
    // UI sounds by name (the drop, the turn, the pickup, a refusal: the
    // ui_* sounds of docs/buy-build-mac.md §6).
    std::function<void(const std::string&)> playSound;
    // The object under a mouse ray (the host's picking, as the pie menu does
    // it); without it the tool falls back to the tile under the mouse.
    std::function<vm::ObjectId(simd_float3 eye, simd_float3 dir)> pickObject;
    // The left column's tools: the hand (move; Delete sells what it holds),
    // the eyedropper (picks the catalog item of the object clicked) and the
    // Design tool (0xCC607030 "Ferramenta Design": the object design dialog,
    // app/design_tool.hpp).
    enum class Tool { Hand, Eyedropper, Design };
    Tool tool() const { return tool_; }
    void setTool(Tool t);
    // The eyedropper on a placed object: holds a new one of its catalog item.
    bool eyedrop(vm::ObjectId id);
    // The Design tool on an object (the dialog by `at`, a canvas point).
    bool design(vm::ObjectId id, simd_float2 at);
    DesignTool& designTool() { return *design_; }
    // "Alternar Dia/Noite" (0xCC607035): the host flips the lighting.
    std::function<void()> onDayNight;
    // "Vista de cima" (0xCC607036, a toggle button; onBtnToggledID calls
    // cTSMainToolbar::SetTopDown): the host switches the camera; topDown
    // says whether the view is on (the button is pressed).
    std::function<void()> onTopDown;
    std::function<bool()> topDown;

    // --- The lot -------------------------------------------------------------
    // The mouse ray over the lot (world space): what is held follows it.
    void pointAt(simd_float3 eye, simd_float3 dir);
    // A click on the lot: places what is held (bought, or moved), or picks
    // up the object under the mouse with the hand. Returns whether it did.
    // `at` is the canvas point (where the design dialog opens).
    bool click(simd_float3 eye, simd_float3 dir, std::optional<simd_float2> at = std::nullopt);
    // The same in three steps, as the mouse does it: the press keeps what is
    // held on the tile under the mouse, a drag turns it to face the mouse
    // (aimAt: a ground point), the release places it.
    bool mouseDown(simd_float3 eye, simd_float3 dir, std::optional<simd_float2> at = std::nullopt);
    void mouseDrag(simd_float3 eye, simd_float3 dir);
    void aimAt(simd_float2 ground);
    bool mouseUp();
    bool pressAt(int x, int y); // headless: the press on a tile
    bool pressing() const { return pressing_ || grabPress_; }
    // The press's drag turned what is held (the cursor turns to simsrotate).
    bool turned() const { return turned_; }
    // Nothing held: the object the tool acts on is under the mouse (pointAt).
    bool overObject() const { return overObject_; }
    // A panel (or the design dialog) is under canvas point p.
    bool overPanel(simd_float2 p) const;
    int direction() const { return direction_; }
    // Headless: holds at a tile / places there. `autoRotate`: turned to fit
    // the walls first, as TryPlace does on a mouse move (vm::autoRotate).
    void holdAt(int x, int y, int direction, bool autoRotate = false);
    bool placeAt(int x, int y, int direction);
    // Picks up a placed object (the hand) by ID.
    bool grab(vm::ObjectId id);
    // Headless: the hand's press on a placed object, a drag to tile x,y, the release.
    bool grabDrag(vm::ObjectId id, int x, int y);
    // , and . turn by 90° (2 direction steps); Esc drops what is held;
    // Delete sells it.
    void rotate(int steps);
    void cancel();
    bool deleteHeld();
    bool undo();
    bool redo();
    vm::ObjectId held() const { return held_; }
    // The last placement's verdict (with the game's error text), for the host.
    const std::string& message() const { return message_; }

    // --- The panel -------------------------------------------------------------
    // The tutorial (app/tutorial.hpp): allowPress before a click on a
    // control (false: turned off); a control by the game's window ids, while
    // the panel is up.
    std::function<bool(simd_float2)> allowPress;
    std::optional<std::array<float, 4>> controlRect(uint32_t window, uint32_t control);
    bool press(simd_float2 p); // true: the panel took it
    void hover(simd_float2 p);
    void setCanvas(int width, int height) { canvasW_ = width, canvasH_ = height, dirty_ = true; }
    UiScreen screen(int width, int height);
#ifndef OS2_SDL_GPU
    void attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat);
    void encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale);
#endif
    // Called every frame: the footprint highlight follows what is held.
    void update();

private:
    const CatalogEntry* catalogEntryOf(const vm::Object* lead) const;
    enum class Sort { Function, Room };
    Tool tool_ = Tool::Hand;
    vm::ObjectId objectAt(simd_float3 eye, simd_float3 dir, const std::optional<vm::TilePos>& tile) const;
    enum class Action { None, Tab, Function, SubSort, Room, RoomFunction, Back, Prev, Next, Expand, Item, Tool, Undo, Redo, DayNight, TopDown };
    struct Target {
        std::array<float, 4> rect;
        Action action = Action::None;
        int value = 0;
    };
    struct Op {
        enum class Kind { Created, Deleted, Moved, Design } kind = Kind::Created;
        vm::ObjectId id = 0;
        vm::Placement placement; // Deleted/Moved: where it was; Created: where it went
        int cost = 0;
        std::map<std::string, std::string> before, after; // Design: the design choices
    };
    std::vector<const CatalogEntry*> items() const;
    std::optional<vm::TilePos> tileUnder(simd_float3 eye, simd_float3 dir) const;
    std::optional<simd_float3> rayHit(simd_float3 eye, simd_float3 dir) const;
    std::optional<simd_float2> groundUnder(simd_float3 eye, simd_float3 dir) const;
    void preview(const vm::Placement& p);
    vm::Placement current(vm::ObjectId id);
    void layoutIfNeeded();
    std::string errorText(const vm::Placement& p);
    void say(const std::string& m);
    void sound(const char* name) {
        if (playSound)
            playSound(name);
    }
    const char* dropSound(vm::ObjectId id) const;

    LotSimulation& sim_;
    vm::VM& vm_;
    VmObjectView& view_;
    Renderer& renderer_;
    ResourceIndex& index_;
    std::unique_ptr<UiResources> res_;
    std::unique_ptr<FontLibrary> fonts_;
    Catalog catalog_;
    std::unique_ptr<DesignTool> design_;
    UiScript panel_, puck_, description_;
    bool ok_ = false, active_ = false;
    int16_t savedPaused_ = 0;

    // Catalog browsing (cTSWinProcBuyPanel).
    Sort sort_ = Sort::Function;
    int function_ = -1; // function sort bit chosen (-1: the category buttons)
    int subSort_ = -1;  // subsort bits (-1: nothing pressed yet, 0: all)
    int room_ = -1;     // room sort bit chosen
    int roomFunction_ = 0;
    size_t page_ = 0;
    bool expanded_ = false; // the catalog's window 0x2000 (4 rows) instead of 0x1000 (2)
    const CatalogEntry* selected_ = nullptr;
    const CatalogEntry* hovered_ = nullptr;

    // The object tool.
    vm::ObjectId held_ = 0;
    bool heldNew_ = false; // created from the catalog (not bought yet)
    // A new object's grid-aligned / diagonal twin, out of the world, which
    // the auto-turn swaps in on a diagonal wall (vm::createDiagonalTwin,
    // SwapDiagonalObject). Not made for a grabbed object (inferred scope:
    // the binary's TryGrab makes one for those too).
    vm::ObjectId twin_ = 0;
    void dropTwin();
    vm::Placement heldFrom_; // where a grabbed object was
    vm::Placement last_;     // its last test
    int direction_ = 2;
    bool pressing_ = false; // the button is down on what is held (a drag turns it)
    bool turned_ = false;   // that drag turned it
    bool grabPress_ = false; // the button that picked it up is still down (a drag moves it, the release drops it)
    bool grabMoved_ = false; // that drag left the tile it was picked up from
    bool overObject_ = false;
    vm::TilePos pressTile_{};
    int level_ = 1;
    std::vector<Op> undo_, redo_;
    // A change pushed: the redo lists released, the shared order told.
    void recorded();
    std::string message_;

    // Footprint tiles: models (green, red) and instances.
    std::array<int, 2> tileModels_{-1, -1};
    bool outlines_ = false; // the game's outlines (with the facing chevron), not plain tiles
    std::vector<int> tileInstances_;

    std::vector<Target> targets_;
    DrawnWindows windows_; // the panel's windows in the last screen
    std::vector<std::array<float, 4>> solid_;
    std::optional<simd_float2> mouse_;
    std::vector<std::unique_ptr<RgbaImage>> frameImages_;
#ifndef OS2_SDL_GPU
    std::array<std::unique_ptr<UiLayer>, 3> layers_;
    size_t layer_ = 0;
#endif
    bool dirty_ = true;
    int canvasW_ = 0, canvasH_ = 0, laidW_ = 0, laidH_ = 0;
};

} // namespace os2
