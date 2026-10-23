// Build mode's panel (build_mode.hpp): the catalog's items and thumbnails,
// the Build panel's screen (UI script 0x49060003), its clicks and hover,
// and the controls the tutorial points at.

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

constexpr uint32_t kTypeTxmt = 0x49596978, kTypeTxtr = 0x1C4A276C;
// Build panel windows (0x49060003, docs/buy-build-mac.md §7).
constexpr uint32_t kSortArea = 0x000000A8, kCategories = 0xA000, kCatalogArea = 0xAD502BBF, kCollapsed = 0x1000,
                   kExpanded = 0x2000, kPrev = 0x1004, kNext = 0x1005, kInfoText = 0x4000;
constexpr uint32_t kDesignBtn = 0xAB, kDayNightBtn = 0xAA, kTopDownBtn = 0xA3;
constexpr uint32_t kCategoriesBtn = 0xA6, kHand = 0xA5, kUndo = 0xA1, kRedo = 0xA2, kCollectionsBtn = 0xA7, kCollectionsWin = 0xA9,
                   kAutoRoof = 0xAC, kBgFunctions = 0xB1, kBgNoSubsort = 0xB2, kBgCollections = 0xB3, kBgTile = 0xB4,
                   kBgEndCap = 0xB5;
// The collapsed grid (window 0x1006 of 0x1000), as Buy mode's: 3 x 2
// cells with the collections constants' margins (inferred).
constexpr float kGridX = 52, kGridY = 19, kGridW = 176, kGridH = 100;
constexpr int kColumns = 3, kRows = 2;
constexpr float kMarginX = 2, kMarginY = 6;
// Wall tool and room tool (walls.txt "wall 1": thumbnailResource 0 and 1,
// catalog text "build" 129 strings 0/1 and 3/4 in Text/Build.package).
constexpr uint32_t kBuildThumbGroup = 0x499DB772, kWallToolThumb = 0xE9261348, kRoomToolThumb = 0xE9261347;
constexpr uint32_t kBuildText = 0x81;

// Floor and wall-covering subsort buttons 0xB4n0 / 0xB5n0 (n from 2):
// the property sets' "subsort" values.
const char* const kFloorSubsorts[] = {"stone", "brick", "wood", "carpet", "tile", "lino", "poured", "other"};
const char* const kWallSubsorts[] = {"brick", "masonry", "siding", "poured", "paint", "wallpaper", "tile", "paneling"};
// Doors and windows (build mode type 8): button 0xB3n0 -> subsort bits
// (doors 1, two-story windows 2, windows 4, arches 0x10; gates 8 are under
// Miscellaneous 0xBA60).
int doorSubsort(int n) {
    switch (n) {
    case 2: return 1;
    case 3: return 2;
    case 4: return 4;
    case 5: return 0x10;
    default: return 1 | 2 | 4 | 0x10;
    }
}

bool inside(const std::array<float, 4>& r, simd_float2 p) {
    return p.x >= r[0] && p.x < r[2] && p.y >= r[1] && p.y < r[3];
}

std::string area(int x0, int y0, int x1, int y1) {
    return "(" + std::to_string(x0) + "," + std::to_string(y0) + "," + std::to_string(x1) + "," + std::to_string(y1) +
           ")";
}

} // namespace

// --- The panel -----------------------------------------------------------------

const RgbaImage* BuildMode::patternThumbnail(const BuildPattern& p) { return materialThumbnail(p.guid, p.material); }

// Fences have no catalog thumbnail in the game's cache: their rail's
// material texture stands in for it (as patterns show their texture).
const RgbaImage* BuildMode::fenceThumbnail(const BuildFence& f) {
    if (auto it = thumbs_.find(f.guid); it != thumbs_.end())
        return it->second.get();
    std::string material;
    try {
        const ObjectModel m = loadObjectModel(index_, f.rail);
        for (const auto& part : m.parts)
            if (!part.material.empty()) {
                material = part.material;
                break;
            }
    } catch (const std::exception& err) {
        os2::logOnce("fence thumbnail", f.rail, err);
    }
    if (material.empty())
        return thumbs_.emplace(f.guid, nullptr).first->second.get();
    return materialThumbnail(f.guid, material);
}

const RgbaImage* BuildMode::materialThumbnail(uint32_t key, const std::string& materialName) {
    if (auto it = thumbs_.find(key); it != thumbs_.end())
        return it->second.get();
    BuildPattern p;
    p.guid = key, p.material = materialName;
    std::shared_ptr<RgbaImage> img;
    // The material's base texture (the game renders its catalog swatches
    // from the material: wallthumbnailmat; here the texture itself).
    try {
        if (auto loc = index_.findByName(p.material + "_txmt", kTypeTxmt)) {
            const auto bytes = index_.read(*loc);
            Rcol rc(bytes);
            if (const auto* block = rc.find("cMaterialDefinition")) {
                const MaterialDefinition md = MaterialDefinition::parse(block->body);
                std::string tex = md.property("stdMatBaseTextureName");
                if (tex.empty() && !md.textureNames.empty())
                    tex = md.textureNames.front();
                if (!tex.empty())
                    if (auto t = index_.findByName(tex + "_txtr", kTypeTxtr)) {
                        // A level about the cell's size (the largest may be a LIFO).
                        const TextureLevels lv = loadTextureLevels(index_.read(*t), &index_);
                        size_t pick = 0;
                        for (size_t l = 0; l < lv.levels.size(); ++l)
                            if (lv.levels[l].width >= 64)
                                pick = l;
                        if (pick < lv.levels.size()) {
                            const auto& L = lv.levels[pick];
                            img = std::make_shared<RgbaImage>(decodeTexture(lv.format, L.width, L.height, L.data));
                        }
                    }
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: build mode: thumbnail of %s: %s\n", p.material.c_str(), e.what());
    }
    thumbs_[p.guid] = std::move(img);
    return thumbs_[p.guid].get();
}

// A texture by name (a terrain paint's swatch).
const RgbaImage* BuildMode::textureThumbnail(uint32_t key, const std::string& texture) {
    if (auto it = thumbs_.find(key); it != thumbs_.end())
        return it->second.get();
    std::shared_ptr<RgbaImage> img;
    try {
        if (auto t = index_.findByName(texture + "_txtr", kTypeTxtr)) {
            const TextureLevels lv = loadTextureLevels(index_.read(*t), &index_);
            size_t pick = 0;
            for (size_t l = 0; l < lv.levels.size(); ++l)
                if (lv.levels[l].width >= 64)
                    pick = l;
            if (pick < lv.levels.size()) {
                const auto& L = lv.levels[pick];
                img = std::make_shared<RgbaImage>(decodeTexture(lv.format, L.width, L.height, L.data));
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: build mode: thumbnail of %s: %s\n", texture.c_str(), e.what());
    }
    thumbs_[key] = std::move(img);
    return thumbs_[key].get();
}

const RgbaImage* BuildMode::stairsThumbnail(const BuildStairs& b) {
    const uint32_t key = 0x5157A125u ^ uint32_t(b.id);
    if (auto it = thumbs_.find(key); it != thumbs_.end())
        return it->second.get();
    std::shared_ptr<RgbaImage> img;
    auto cresOf = [&](uint32_t guid) -> std::string {
        const auto def = guid ? vm_.code().definitionByGuid(guid) : std::nullopt;
        if (!def)
            return {};
        try {
            return objectCresName(index_, Tgi{vm::kTypeObjd, def->first, def->second, 0});
        } catch (const std::exception& err) {
            os2::logOnce("stairs thumbnail", std::to_string(guid), err);
            return {};
        }
    };
    if (renderThumb) {
        const std::string step = cresOf(b.objects[2]), top = cresOf(b.objects[3]);
        std::string bottom = cresOf(b.objects[4]);
        if (bottom.empty())
            bottom = step;
        std::vector<ThumbnailPart> parts;
        const std::string order[3] = {top, step, bottom};
        for (int i = 0; i < 3; ++i)
            if (!order[i].empty())
                parts.push_back({order[i], {0, 0.25f * float(i), -0.1875f * float(i)}});
        if (!parts.empty())
            if (auto r = renderThumb(parts))
                img = std::make_shared<RgbaImage>(std::move(*r));
    }
    if (!img) // no renderer (headless lists): the step's cached thumbnail, if any
        for (const uint32_t guid : {b.objects[2], b.objects[3]}) {
            const auto def = vm_.code().definitionByGuid(guid);
            if (!def)
                continue;
            CatalogEntry e;
            e.guid = guid;
            e.group = def->first;
            e.model = cresOf(guid);
            if (e.model.ends_with("_cres"))
                e.model.resize(e.model.size() - 5);
            if (const RgbaImage* cached = objects_.thumbnail(e))
                return cached;
        }
    thumbs_[key] = img;
    return img.get();
}

const RgbaImage* BuildMode::dormerThumbnail(const BuildRoofProduct& d) {
    const uint32_t key = 0xD0E3E700u ^ uint32_t(d.id);
    if (auto it = thumbs_.find(key); it != thumbs_.end())
        return it->second.get();
    std::shared_ptr<RgbaImage> img;
    if (renderThumb && !d.defaultPattern.empty())
        if (auto r = renderThumb({{d.defaultPattern, {0, 0, 0}}}))
            img = std::make_shared<RgbaImage>(std::move(*r));
    thumbs_[key] = img;
    return img.get();
}

std::vector<BuildMode::Item> BuildMode::items() {
    std::vector<Item> out;
    auto patternItems = [&](bool floor, const char* const* names, size_t count) {
        const std::string sub = subsort_ >= 2 && size_t(subsort_ - 2) < count ? names[subsort_ - 2] : "";
        for (const BuildPattern* p : patterns_.bySubsort(floor, sub)) {
            Item it;
            it.tool = floor ? Tool::Floor : Tool::Wallpaper;
            it.pattern = p;
            it.name = p->name, it.description = p->description, it.price = p->price;
            out.push_back(std::move(it));
        }
    };
    auto objectItems = [&](int type, int bits) {
        for (const auto& e : objects_.entries())
            if (e.buildModeType == type && (e.buildModeSubsort & bits)) {
                Item it;
                it.tool = Tool::Object;
                it.object = &e;
                it.name = e.name, it.description = e.description, it.price = e.price;
                out.push_back(std::move(it));
            }
    };
    switch (category_) {
    case 1: {
        // Text/Build.package STR# 129 (catalogTextResource build 129): for
        // catalog index i, string 3i the price, 3i+1 the name, 3i+2 the text.
        const StringTable* t = res_->table(packageGroupHash("Build"), kBuildText);
        for (int k = 0; k < 2; ++k) {
            Item it;
            it.tool = k ? Tool::Room : Tool::Wall;
            it.price = kWallSectionPrice;
            it.image = res_->image({kTypeUiImage, kBuildThumbGroup, k ? kRoomToolThumb : kWallToolThumb});
            if (t) {
                it.price = std::atoi(t->get(size_t(3 * k), lang::EnglishUS).c_str());
                it.name = t->get(size_t(3 * k + 1));
                it.description = t->get(size_t(3 * k + 2));
            }
            if (!it.price)
                it.price = kWallSectionPrice;
            if (it.name.empty())
                it.name = k ? "Room Tool" : "Wall Tool";
            out.push_back(std::move(it));
        }
        break;
    }
    case 2: {
        // B200 foundations: catalog text "build" 138 (deckfoundations.txt
        // "foundation 0": catalogTextIndices 0 1), price 3i, name 3i+1, text 3i+2.
        // The decks: "deck 1" (catalogTextIndices 2 3, lattice) and "deck 2"
        // (4 5, columns): strings 3 x index.
        struct Kind {
            int text, price;
            const char* name;
            uint32_t key;
            const char* material;
        };
        const Kind kinds[3] = {{0, kFoundationTilePrice, "Foundation", 0xF0F0F0F0u, "floor_plywoodFoundation"},
                               {2, kLatticeDeckTilePrice, "Deck", 0xF0F0F0F1u, "wall_deckRedwoodAlpha_base"},
                               {4, kColumnDeckTilePrice, "Deck", 0xF0F0F0F2u, "floor_deckDarkRedwood"}};
        for (int k = 0; k < 3; ++k) {
            Item it;
            it.tool = Tool::Foundation;
            it.product = k;
            it.price = kinds[k].price;
            it.name = kinds[k].name;
            if (const StringTable* t = res_->table(packageGroupHash("Build"), 0x8A)) {
                const size_t i = size_t(3 * kinds[k].text);
                if (const int price = std::atoi(t->get(i, lang::EnglishUS).c_str()))
                    it.price = price;
                if (const std::string n = t->get(i + 1); !n.empty())
                    it.name = n;
                it.description = t->get(i + 2);
            }
            it.image = materialThumbnail(kinds[k].key, kinds[k].material);
            out.push_back(std::move(it));
        }
        break;
    }
    case 7: {
        // B720 sculpting: catalog text "build" 128 (strings 3i price, 3i+1
        // name, 3i+2 text; Raise 0, Lower 1, the proxy tool 2 and 3 =
        // flatten and smooth, inferred), thumbnails 0x499DB772/0xE9261349,
        // ...50, ...41 (userToolProxies.txt) and ...51 (inferred).
        // B730 the terrain paints (their texture as the swatch).
        if (subsort_ == 0 || subsort_ == 2) {
            const StringTable* t = res_->table(packageGroupHash("Build"), 0x80);
            static const uint32_t thumbs[4] = {0xE9261349, 0xE9261350, 0xE9261341, 0xE9261351};
            static const char* const fallback[4] = {"Raise Terrain", "Lower Terrain", "Flatten Terrain", "Smooth Terrain"};
            for (int k = 0; k < 4; ++k) {
                Item it;
                it.tool = Tool::Terrain;
                it.terrainOp = k;
                it.price = 1;
                it.name = fallback[k];
                if (t) {
                    if (const std::string n = t->get(size_t(3 * k + 1)); !n.empty())
                        it.name = n;
                    it.description = t->get(size_t(3 * k + 2));
                }
                it.image = res_->image({kTypeUiImage, kBuildThumbGroup, thumbs[k]});
                out.push_back(std::move(it));
            }
        }
        if (subsort_ == 0 || subsort_ == 4) { // the water tool (ponds; catalog text "build" 133, §1; button inferred)
            Item it;
            it.tool = Tool::Terrain;
            it.terrainOp = int(vm::BuildEditor::TerrainOp::Water);
            it.price = 1;
            it.name = "Water";
            if (const StringTable* t = res_->table(packageGroupHash("Build"), 0x85)) {
                if (const std::string n = t->get(1); !n.empty())
                    it.name = n;
                it.description = t->get(2);
            }
            it.image = materialThumbnail(0xF0F0F0F3u, "lotwater");
            out.push_back(std::move(it));
        }
        if (subsort_ == 0 || subsort_ == 3)
            for (const BuildTerrainPaint& p : patterns_.terrainPaints()) {
                Item it;
                it.tool = Tool::Terrain;
                it.paint = &p;
                it.name = p.name, it.description = p.description;
                it.image = textureThumbnail(p.guid, p.texture);
                out.push_back(std::move(it));
            }
        if (subsort_ == 0 || subsort_ == 5) // B751-B753 the brushes (build type 4, bit 0x10)
            objectItems(4, 0x10);
        break;
    }
    case 9: {
        // B920 roof types: roofs.txt "roof 0-5" (Lot::RoofType 1-6), catalog
        // text "build" 140 at their catalogTextIndex (0 1 3 2 4 5), price 0,
        // thumbnails 0x499DB772/0xE9261580, ...85, ...60, ...65, ...70, ...75.
        // B930 the roof patterns (their top texture as the swatch).
        if (subsort_ == 0 || subsort_ == 2) {
            const StringTable* t = res_->table(packageGroupHash("Build"), 0x8C);
            static const int text[6] = {0, 1, 3, 2, 4, 5};
            static const uint32_t thumbs[6] = {0xE9261580, 0xE9261585, 0xE9261560, 0xE9261565, 0xE9261570, 0xE9261575};
            static const char* const fallback[6] = {"Shed Gable Roof", "Shed Hip Roof", "Short Gable Roof",
                                                    "Long Gable Roof", "Hip Roof", "Mansard Roof"};
            for (int k = 0; k < 6; ++k) {
                Item it;
                it.tool = Tool::Roof;
                it.roofType = uint32_t(k + 1);
                it.name = fallback[k];
                if (t) {
                    if (const std::string n = t->get(size_t(3 * text[k] + 1)); !n.empty())
                        it.name = n;
                    it.description = t->get(size_t(3 * text[k] + 2));
                }
                it.image = res_->image({kTypeUiImage, kBuildThumbGroup, thumbs[k]});
                out.push_back(std::move(it));
            }
        }
        if (subsort_ == 0 || subsort_ == 4) // B940 dormers (roofs.txt "roof 6-11", catalog text "build" 140)
            for (const BuildRoofProduct& d : patterns_.roofProducts())
                if (d.id >= 6 && d.id <= 11) {
                    Item it;
                    it.tool = Tool::Dormer;
                    it.dormer = &d;
                    it.name = d.name, it.description = d.description, it.price = d.price;
                    it.image = dormerThumbnail(d);
                    out.push_back(std::move(it));
                }
        if (subsort_ == 0 || subsort_ == 3)
            for (const BuildRoofPattern& p : patterns_.roofPatterns()) {
                Item it;
                it.tool = Tool::Roof;
                it.roofPattern = &p;
                it.name = p.name, it.description = p.description;
                it.image = textureThumbnail(p.guid, p.texture);
                out.push_back(std::move(it));
            }
        break;
    }
    case 3: objectItems(8, doorSubsort(subsort_)); break;
    case 8: { // B820 trees, B830 bushes, B840 flowers, B850 all (build type 4)
        // (the build catalog's census: the trees in the bit 1, the bushes in
        // the bit 2, the flowers in the bit 4; the bit 0x10 are the garden
        // tools, the brushes of the terrain panel).
        const uint16_t bits = subsort_ == 2 ? 0x01 : subsort_ == 3 ? 0x02 : subsort_ == 4 ? 0x04 : 0x07;
        if (subsort_ == 0 || subsort_ >= 2)
            objectItems(4, bits);
        break;
    }
    case 6: // B630 staircases (build type 1, subsort 0x20; placed with the object tool)
        if (subsort_ == 0 || subsort_ == 3)
            objectItems(1, 0x20);
        if (subsort_ == 0 || subsort_ == 4) // B640 modular stairs (modularstairs.txt, price a step)
            for (const BuildStairs& b : patterns_.stairs()) {
                Item it;
                it.tool = Tool::Stairs;
                it.stairs = &b;
                it.name = b.name, it.description = b.description, it.price = b.price;
                it.image = stairsThumbnail(b);
                out.push_back(std::move(it));
            }
        break;
    case 4: patternItems(true, kFloorSubsorts, std::size(kFloorSubsorts)); break;
    case 5: patternItems(false, kWallSubsorts, std::size(kWallSubsorts)); break;
    case 10:
        if (subsort_ == 2)
            objectItems(1, 0x200); // BA20 columns (build type 1, subsort 0x200)
        if (subsort_ == 6)
            objectItems(8, 8); // gates
        if (subsort_ == 3) // BA30 fireplaces (fireplaces.txt, catalog text "build" 144)
            for (const BuildFireplace& fp : patterns_.fireplaces()) {
                Item it;
                it.tool = Tool::Fireplace;
                it.fireplace = &fp;
                it.name = fp.name, it.description = fp.description, it.price = fp.price;
                out.push_back(std::move(it));
            }
        if (subsort_ == 4) { // BA40 pools: the pool tool (catalog text "build" 138, index 6), then the pool objects
            Item it;
            it.tool = Tool::Pool;
            it.price = kPoolTilePrice;
            it.name = "Pool";
            if (const StringTable* t = res_->table(packageGroupHash("Build"), 0x8A)) {
                if (const int price = std::atoi(t->get(18, lang::EnglishUS).c_str()))
                    it.price = price;
                if (const std::string n = t->get(19); !n.empty())
                    it.name = n;
                it.description = t->get(20);
            }
            it.image = materialThumbnail(0xF0F0F0F1u, "floor_poolRoundedLipBlue");
            out.push_back(std::move(it));
            objectItems(1, 0x40);
        }
        if (subsort_ == 5) // BA50 fences, then the fence arch (fencearches.txt, catalog text "build" 143)
            for (const BuildFenceArch& a : patterns_.fenceArches()) {
                Item it;
                it.tool = Tool::FenceArch;
                it.arch = &a;
                it.name = a.name, it.description = a.description, it.price = a.price;
                out.push_back(std::move(it));
            }
        if (subsort_ == 5)
            for (const BuildFence& f : patterns_.fences()) {
                Item it;
                it.tool = Tool::Fence;
                it.fence = &f;
                it.name = f.name, it.description = f.description, it.price = f.price;
                out.push_back(std::move(it));
            }
        break;
    default: break;
    }
    return out;
}

void BuildMode::openCategory(int category, int subsort) {
    category_ = category, subsort_ = subsort, page_ = 0, dirty_ = true;
}

UiScreen BuildMode::screen(int width, int height) {
    UiScreen out;
    out.width = width, out.height = height;
    laidW_ = width, laidH_ = height;
    targets_.clear();
    solid_.clear();
    if (!ok_ || !active_)
        return out;
    std::map<const UiElement*, std::array<float, 4>> rects;
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
        t.attrs = {{"clsid", "GZWinText"}, {"caption", s},    {"font", "LiveModePanelPuck"},
                   {"forecolor", color},    {"align", align}, {"winflag_visible", "yes"},
                   {"area", area(0, 0, int(w), int(h))}};
        UiDrawHooks th;
        layoutUiElement(t, x, y, *res_, *fonts_, out, th);
    };

    UiElement& root = panel_.roots.front();
    UiElement* sortArea = root.child(kSortArea);
    const uint32_t openId = category_ == 0 ? kCategories : uint32_t(0xB000 + category_ * 0x100);
    const UiElement* open = sortArea ? sortArea->child(openId) : nullptr;
    const auto list = items();
    // B950 opens the auto-roof panel (0xAC) where the catalog goes.
    const bool autoRoofPanel = category_ == 9 && subsort_ == 5;
    const bool showCatalog = category_ != 0 && !autoRoofPanel;
    // The catalog follows the open sort panel (as in Buy mode; the
    // script's place for it, x 484, is where the category buttons end).
    if (sortArea && open) {
        const int right = sortArea->area()[0] + open->area()[2] - 1;
        if (UiElement* c = root.child(kCatalogArea)) {
            const auto a = c->area();
            c->set("area", area(right, a[1], right + (a[2] - a[0]), a[3]));
        }
        if (UiElement* c = root.child(kAutoRoof)) {
            const auto a = c->area();
            c->set("area", area(right, a[1], right + (a[2] - a[0]), a[3]));
        }
        if (UiElement* t = root.child(kBgTile)) {
            const auto a = t->area();
            const int end = showCatalog ? right : sortArea->area()[0] + open->area()[2];
            t->set("area", area(a[0], a[1], std::max(a[0], end), a[3]));
        }
        if (UiElement* e = root.child(kBgEndCap)) {
            const auto a = e->area();
            const int x = sortArea->area()[0] + open->area()[2];
            e->set("area", area(x, a[1], x + (a[2] - a[0]), a[3]));
        }
    }
    const UiElement& puck = puck_.roots.front();
    const auto pa = puck.area();
    // The pod as in Buy and Live: the client's left edge and 3 px above the
    // script's own reach to its foot (the pill's art at 3,396 at 800x600,
    // measured; the join below rides the same place; docs/buy-build-mac.md
    // §1.6).
    const float puckX = 0, puckY = float(height) - float(pa[3] - pa[1]) - 3;
    float x0 = puckX, y0 = puckY - 137;
    if (const UiElement* a = puck.child(0x4BDEFFAA))
        if (const UiElement* b = root.child(0x0000AAAA))
            x0 = puckX + float(a->area()[0] - b->area()[0]), y0 = puckY + float(a->area()[1] - b->area()[1]);

    UiDrawHooks hk;
    hk.rects = &rects;
    for (uint32_t id : {kBgNoSubsort, kBgCollections, kCollectionsBtn, kCollectionsWin})
        if (const UiElement* e = root.child(id))
            hk.hide.insert(e);
    if (const UiElement* e = root.child(kAutoRoof)) {
        (autoRoofPanel ? hk.show : hk.hide).insert(e);
        if (const UiElement* b = e->child(0xC400))
            hk.buttonState[b] = preserveRoofs_ ? 2 : 1;
    }
    if (const UiElement* e = root.child(kBgFunctions))
        hk.show.insert(e);
    if (const UiElement* e = root.child(kBgEndCap))
        (showCatalog ? hk.hide : hk.show).insert(e);
    if (const UiElement* t = root.child(kBgTile)) {
        const auto a = t->area();
        if (a[2] <= a[0])
            hk.hide.insert(t);
    }
    if (const UiElement* c = root.child(kCatalogArea)) {
        (showCatalog ? hk.show : hk.hide).insert(c);
        if (const UiElement* x = c->child(kExpanded))
            hk.hide.insert(x);
        if (const UiElement* x = c->child(kInfoText))
            hk.hide.insert(x);
    }
    if (sortArea)
        for (const auto& c : sortArea->children)
            (c.id() == openId ? hk.show : hk.hide).insert(&c);
    if (open && category_ != 0)
        for (const auto& b : open->children) {
            const int n = int(b.id() >> 4 & 0xF);
            if ((b.id() & 0xF00F) == 0xB000 && n == subsort_ && n != 1)
                hk.buttonState[&b] = 2;
        }
    if (sortArea)
        if (const UiElement* cats = sortArea->child(kCategories))
            for (const auto& b : cats->children)
                if (int(b.id() >> 8 & 0xF) == category_)
                    hk.buttonState[&b] = 2;
    if (const UiElement* e = root.child(kHand))
        hk.buttonState[e] = tool_ == Tool::None ? 2 : 1;
    if (const UiElement* e = root.child(kDesignBtn))
        hk.buttonState[e] = tool_ == Tool::Design ? 2 : 1;
    if (const UiElement* e = root.child(kTopDownBtn))
        hk.buttonState[e] = topDown && topDown() ? 2 : 1;
    if (const UiElement* e = root.child(kUndo))
        hk.buttonState[e] = !undo_.empty() || editor_->undoDepth() > editorDepth_ ? 1 : 0;
    if (const UiElement* e = root.child(kRedo))
        hk.buttonState[e] = !redo_.empty() ? 1 : 0;
    layoutUiElement(root, x0 - float(root.area()[0]), y0 - float(root.area()[1]), *res_, *fonts_, out, hk);

    // The grid: thumbnails of the current page with their prices.
    const size_t perPage = size_t(kColumns * kRows);
    const size_t pages = std::max<size_t>(1, (list.size() + perPage - 1) / perPage);
    page_ = std::min(page_, pages - 1);
    const int64_t funds = vm::noCharge(vm_) ? INT64_MAX : vm::funds(vm_); // no charge: nothing is too dear
    const UiElement* catalog = root.child(kCatalogArea);
    const UiElement* collapsed = catalog ? catalog->child(kCollapsed) : nullptr;
    if (auto r = rectOf(collapsed); r && showCatalog) {
        const float cw = (kGridW - kMarginX * (kColumns - 1)) / kColumns;
        const float ch = (kGridH - kMarginY * (kRows - 1)) / kRows;
        const float thumb = std::min(cw, ch - 12);
        for (size_t i = 0; i < perPage && page_ * perPage + i < list.size(); ++i) {
            Item& it = const_cast<Item&>(list[page_ * perPage + i]);
            const float cx = (*r)[0] + kGridX + float(i % kColumns) * (cw + kMarginX);
            const float cy = (*r)[1] + kGridY + float(i / kColumns) * (ch + kMarginY);
            const float tx = cx + (cw - thumb) / 2;
            const bool selected = (it.pattern && it.pattern == pattern_) || (it.object && it.object == object_) ||
                                  (it.fence && it.fence == fence_) || (it.paint && it.paint == paint_ && tool_ == Tool::Terrain) ||
                                  (it.tool == Tool::Terrain && !it.paint && tool_ == Tool::Terrain && !paint_ &&
                                   it.terrainOp == int(terrainOp_)) ||
                                  (it.tool == Tool::Roof && !it.roofPattern && tool_ == Tool::Roof && it.roofType == roofType_) ||
                                  (it.stairs && it.stairs == stairs_ && tool_ == Tool::Stairs) ||
                                  (it.dormer && it.dormer == dormer_ && tool_ == Tool::Dormer) ||
                                  (it.arch && tool_ == Tool::FenceArch) ||
                                  (it.fireplace && it.fireplace == fireplace_ && tool_ == Tool::Fireplace) ||
                                  (!it.pattern && !it.object && !it.fence && !it.paint && !it.stairs && !it.dormer && !it.arch &&
                                   !it.fireplace && it.tool != Tool::Terrain &&
                                   it.tool != Tool::Roof && it.tool == tool_);
            if (selected) {
                UiQuad q;
                q.x0 = tx - 2, q.y0 = cy - 2, q.x1 = tx + thumb + 2, q.y1 = cy + thumb + 2;
                q.color = {0.6f, 1, 0.6f, 0.7f};
                out.quads.push_back(q);
            }
            const RgbaImage* img = it.image;
            if (!img && it.pattern)
                img = patternThumbnail(*it.pattern);
            if (!img && it.fence)
                img = fenceThumbnail(*it.fence);
            if (!img && it.object)
                img = objects_.thumbnail(*it.object);
            if (img) {
                UiQuad q;
                q.image = img;
                q.x0 = tx, q.y0 = cy, q.x1 = tx + thumb, q.y1 = cy + thumb;
                if (it.price > funds)
                    q.color = {0.45f, 0.45f, 0.45f, 0.8f};
                out.quads.push_back(q);
            }
            text(money(it.price), tx - 8, cy + ch - 13, thumb + 16, 13, "center",
                 it.price > funds ? "(150,40,40)" : "(5,6,113)");
            targets_.push_back({{tx, cy, tx + thumb, cy + ch}, Action::Item, int(page_ * perPage + i)});
        }
        target(collapsed->child(kPrev), Action::Prev, 0);
        target(collapsed->child(kNext), Action::Next, 0);
    }
    target(root.child(kHand), Action::Hand, 0);
    if (const UiElement* e = root.child(kAutoRoof); e && autoRoofPanel) {
        static const std::pair<uint32_t, int> buttons[] = {{0xC100, 1}, {0xC200, 2}, {0xC500, 3}, {0xC600, 4},
                                                           {0xC700, 5}, {0xC300, 0}, {0xC400, -1}};
        for (const auto& [id, tool] : buttons)
            target(e->child(id), Action::AutoRoof, tool);
    }
    target(root.child(kDesignBtn), Action::Design, 0);
    target(root.child(kDayNightBtn), Action::DayNight, 0);
    target(root.child(kTopDownBtn), Action::TopDown, 0);
    target(root.child(kCategoriesBtn), Action::Back, 0);
    target(root.child(kUndo), Action::Undo, 0);
    target(root.child(kRedo), Action::Redo, 0);
    if (sortArea) {
        if (category_ == 0) {
            if (const UiElement* cats = sortArea->child(kCategories))
                for (const auto& b : cats->children)
                    target(&b, Action::Category, int(b.id() >> 8 & 0xF));
        } else if (open) {
            for (const auto& b : open->children)
                if ((b.id() & 0xF00F) == 0xB000) {
                    const int n = int(b.id() >> 4 & 0xF);
                    target(&b, n == 1 ? Action::Back : Action::Subsort, n);
                }
        }
    }
    for (const auto& [e, r] : rects)
        if (e->clsid() == "GZWinBMP" || e->clsid() == "GZWinGen" || e->clsid().rfind("0x", 0) == 0)
            solid_.push_back(r);

    // The catalog description of the item under the mouse (as Buy mode's).
    if (hovered_) {
        UiElement& dm = description_.roots.front();
        if (UiElement* n = dm.child(0x4000))
            n->set("area", "(158,36,456,120)"), n->set("wrapped", "yes");
        if (UiElement* n = dm.child(0x5000))
            n->set("winflag_visible", "no");
        const auto da = dm.area();
        const float dx = x0 + 205, dy = y0 + 96 - float(da[3] - da[1]) - 4;
        UiDrawHooks dh;
        if (const UiElement* n = dm.child(0x3000))
            dh.captions[n] = hovered_->name;
        if (const UiElement* n = dm.child(0x4000))
            dh.captions[n] = hovered_->description;
        if (const UiElement* n = dm.child(0x6000))
            dh.captions[n] = "";
        if (const UiElement* f = dm.child(0x8000))
            if (const UiElement* img = f->child(0x8001)) {
                const RgbaImage* t = hovered_->image;
                if (!t && hovered_->pattern)
                    t = patternThumbnail(*hovered_->pattern);
                if (!t && hovered_->fence)
                    t = fenceThumbnail(*hovered_->fence);
                if (!t && hovered_->object)
                    t = objects_.thumbnail(*hovered_->object);
                if (t)
                    dh.images[img] = t;
            }
        for (uint32_t id : {0x9000u, 0xA000u})
            if (const UiElement* g = dm.child(id))
                dh.hide.insert(g);
        dh.rects = &rects;
        layoutUiElement(dm, dx - float(da[0]), dy - float(da[1]), *res_, *fonts_, out, dh);
        if (const UiElement* pb = dm.child(0x0002))
            if (auto r = rectOf(pb))
                text(money(hovered_->price), (*r)[0], (*r)[1], (*r)[2] - (*r)[0], (*r)[3] - (*r)[1], "center");
    }
    // Why the last change was refused, by the mouse (the look is inferred).
    if (!message_.empty() && message_.rfind("cannot", 0) == 0) {
        // Only the game's text for the refusal: no "cannot:" prefix and no internal reason in brackets.
        std::string shown = message_.substr(0, message_.find(';'));
        shown.erase(0, std::min<size_t>(shown.size(), 8)); // "cannot: "
        if (size_t br = shown.find(" ("); br != std::string::npos)
            shown.resize(br);
        const simd_float2 m = mouse_.value_or(simd_make_float2(float(width) / 2, float(height) / 2));
        UiQuad q;
        const float boxW = 7 * float(shown.size()) + 12;
        q.x0 = std::max(4.0f, std::min(m.x + 14, float(width) - boxW - 4)); // never past the window's edge
        q.y0 = std::min(m.y + 14, float(height) - 26);
        q.x1 = q.x0 + boxW, q.y1 = q.y0 + 22;
        q.color = {178 / 255.0f, 193 / 255.0f, 1, 0.9f};
        out.quads.push_back(q);
        text(shown, q.x0, q.y0 + 3, q.x1 - q.x0, 18, "center");
    }
    design_->layout(out, width, height); // the object design dialog over everything
    windows_.capture(rects); // for the tutorial (controlRect)
    return out;
}

void BuildMode::layoutIfNeeded() {
    if (canvasW_ > 0 && canvasH_ > 0 && (laidW_ != canvasW_ || laidH_ != canvasH_ || targets_.empty() || dirty_))
        screen(canvasW_, canvasH_);
}

bool BuildMode::overPanel(simd_float2 p) const {
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

bool BuildMode::press(simd_float2 p) {
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
        case Action::Category:
            openCategory(t.value, 0);
            if (t.value == 1)
                selectWallTool(false);
            break;
        case Action::Subsort:
            subsort_ = t.value, page_ = 0;
            if (category_ == 6 && t.value == 2) // B620: the Railing Tool (tool 0xF)
                selectRailingTool();
            break;
        case Action::Back: openCategory(0, 0); break;
        case Action::Prev: page_ = page_ ? page_ - 1 : 0; break;
        case Action::Next: ++page_; break;
        case Action::Item: {
            const auto list = items();
            if (size_t(t.value) < list.size()) {
                const Item& it = list[size_t(t.value)];
                if (it.pattern)
                    selectPattern(*it.pattern);
                else if (it.fence)
                    selectFence(*it.fence);
                else if (it.object)
                    selectObject(*it.object);
                else if (it.tool == Tool::Foundation)
                    selectFoundationTool(it.product);
                else if (it.tool == Tool::Pool)
                    selectPoolTool();
                else if (it.stairs)
                    selectStairsTool(*it.stairs);
                else if (it.dormer)
                    selectDormerTool(*it.dormer);
                else if (it.fireplace)
                    selectFireplaceTool(*it.fireplace);
                else if (it.arch) {
                    cancel();
                    tool_ = Tool::FenceArch, arch_ = it.arch;
                    pattern_ = nullptr, object_ = nullptr, fence_ = nullptr, paint_ = nullptr;
                    say(it.arch->name + " (" + money(it.arch->price) + "): click a fence");
                }
                else if (it.roofPattern)
                    applied(editor_->setRoofPattern(it.roofPattern->guid));
                else if (it.tool == Tool::Roof)
                    selectRoofTool(it.roofType);
                else if (it.tool == Tool::Terrain)
                    selectTerrainTool(vm::BuildEditor::TerrainOp(std::max(it.terrainOp, 0)), it.paint);
                else
                    selectWallTool(it.tool == Tool::Room);
            }
            break;
        }
        case Action::Hand: cancel(), design_->cancel(), tool_ = Tool::None; break;
        case Action::Design: selectDesignTool(); break;
        case Action::AutoRoof:
            if (t.value < 0)
                preserveRoofs_ = !preserveRoofs_, say(preserveRoofs_ ? "auto roof: preserve existing roofs" : "auto roof: replace its roofs");
            else
                autoRoof(t.value);
            break;
        case Action::DayNight:
            if (onDayNight)
                onDayNight();
            break;
        case Action::TopDown:
            if (onTopDown)
                onTopDown();
            break;
        case Action::Undo: undo(); break;
        case Action::Redo: redo(); break;
        case Action::None: break;
        }
        std::printf("opensims2: build panel: %s %d (%zu items)\n",
                    t.action == Action::Item ? "item" : t.action == Action::Category ? "category"
                    : t.action == Action::Subsort ? "subsort" : "button",
                    t.value, items().size());
        std::fflush(stdout);
        dirty_ = true;
        return true;
    }
    for (const auto& r : solid_)
        if (inside(r, p))
            return true;
    return false;
}

void BuildMode::hover(simd_float2 p) {
    mouse_ = p;
    if (!active_)
        return;
    layoutIfNeeded();
    std::optional<Item> over;
    for (const Target& t : targets_)
        if (t.action == Action::Item && inside(t.rect, p)) {
            const auto list = items();
            if (size_t(t.value) < list.size())
                over = list[size_t(t.value)];
        }
    const bool changed = over.has_value() != hovered_.has_value() ||
                         (over && (over->pattern != hovered_->pattern || over->object != hovered_->object ||
                                   over->fence != hovered_->fence || over->paint != hovered_->paint ||
                                   over->terrainOp != hovered_->terrainOp || over->roofType != hovered_->roofType ||
                                   over->roofPattern != hovered_->roofPattern ||
                                   over->tool != hovered_->tool));
    if (changed)
        hovered_ = over, dirty_ = true;
}

#ifndef OS2_SDL_GPU
void BuildMode::attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat) {
    for (auto& l : layers_)
        l = std::make_unique<UiLayer>(device, colorFormat, depthFormat);
}

void BuildMode::encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale) {
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

std::optional<std::array<float, 4>> BuildMode::controlRect(uint32_t window, uint32_t control) {
    if (!active_)
        return std::nullopt;
    layoutIfNeeded();
    // The game's "Build Dynamic Menu" (0x502BBF02 with the room tool,
    // 0x502BBF03 with the deck tool: button 3) is made in code; here those
    // tools are items of the catalog: the room tool, and the first deck
    // (the lattice one, deckfoundations.txt "deck 1"; inferred).
    if ((window == 0x502BBF02 || window == 0x502BBF03) && control == 3) {
        const bool deck = window == 0x502BBF03;
        const auto list = items();
        for (const Target& t : targets_)
            if (t.action == Action::Item && size_t(t.value) < list.size()) {
                const Item& it = list[size_t(t.value)];
                if (deck ? it.tool == Tool::Foundation && it.product == 1 : it.tool == Tool::Room)
                    return t.rect;
            }
        return std::nullopt;
    }
    return windows_.find(window, control);
}

} // namespace os2
