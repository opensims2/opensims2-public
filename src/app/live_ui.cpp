#include "app/live_ui.hpp"

#include "app/live_panels.hpp"
#include "app/wants_panel.hpp"
#include "app/ui_panel_util.hpp"

#include "formats/dbpf.hpp"
#include "formats/font.hpp"
#include "formats/image.hpp"
#include "formats/strings.hpp"
#include "formats/ui_image.hpp"
#include "formats/ui_resources.hpp"
#ifndef OS2_SDL_GPU
#include "render/ui_layer.hpp"
#endif
#include "vm/animation.hpp"
#include "vm/interactions.hpp"
#include "vm/motives.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace os2 {

namespace {

constexpr uint32_t kPuckScript = 0x4905F85B, kLiveScript = 0x49060005, kMotivesScript = 0x49064905,
                   kBarScript = 0x4906500B, kMemberScript = 0x49060053, kIconScript = 0x0609A1C0,
                   kQueueConstants = 0xCCD02691;
constexpr uint32_t kMoodGradient = 0xE9100002; // docs/ui-live-mac.md §4.2
constexpr uint32_t kPauseScript = 0x8C364B93;  // cTSUIPauseIndicator
constexpr uint32_t kOptionsScript = 0x49060001; // the options panel
// The subpanel buttons in the button holder, by LivePanels::Panel.
constexpr std::array<uint32_t, 5> kSubButtons = {0x0D8AB291, 0x17DEFBF5, 0x57DEFBF4, 0x47DEFBF2, 0x8B85BA4E};
// Person data: aspiration (46, a bit: 1 romance, 2 family, 4 fortune, 16
// popularity, 32 knowledge, 64 grow up; SimPE's names) and its score (160).
constexpr int kPdAspiration = 46, kPdAspirationScore = 160;
// Age days left (91) and age duration (156): the age meter's data.
constexpr int kPdAgeDaysLeft = 91, kPdAgeDuration = 156;
constexpr uint32_t kAgesTooltip = 0xAD8A9F10; // the meter's tooltip button
// Live panel placements (inferred): the mood meter group after the front
// cap (316 + 13), the needs panel's background after the group (its mood
// box ends at +87), 347 px wide.
constexpr int kGroupX = 329, kSubPanelX = kGroupX + 87, kSubPanelWidth = 347;

// Live panel windows (0x49060005).
constexpr uint32_t kPause = 0x27D7258F, kSpeed2 = 0x57E01CC1, kSpeed3 = 0x37E01CC3, kSpeed4 = 0x47E01CC5;
constexpr uint32_t kClock = 0x37D82A93;
// Puck windows (0x4905F85B): mode buttons (live, buy, build, story, options) and funds.
constexpr std::array<uint32_t, 5> kModes = {0x37D97B25, 0x67D97B2A, 0x77D97B28, 0x27D97B2D, 0x67D97B2F};
constexpr uint32_t kFunds = 0x00003000;

// The needs panel's bars: child window -> motive (docs/ui-live-mac.md §3.1),
// and the caption in Live.package's STR# 0x82 (string number = entry + 1).
struct BarInfo {
    uint32_t window;
    int16_t motive;
    int caption;
};
constexpr std::array<BarInfo, 8> kBars = {{{0xA1, vm::motive::Hunger, 2},
                                           {0xA2, vm::motive::Comfort, 6},
                                           {0xA3, vm::motive::Bladder, 14},
                                           {0xA4, vm::motive::Energy, 4},
                                           {0xA5, vm::motive::Fun, 8},
                                           {0xA6, vm::motive::Social, 12},
                                           {0xA7, vm::motive::Hygiene, 10},
                                           {0xA8, vm::motive::Environment, 16}}};

const UiElement* findIn(const UiElement& e, uint32_t id) { return findWindow(e, id); }
UiElement* findIn(UiElement& e, uint32_t id) { return findWindow(e, id); }
std::string area(int x0, int y0, int x1, int y1) { return uiArea(x0, y0, x1, y1); }
bool inside(const std::array<float, 4>& r, simd_float2 p) { return insideRect(r, p.x, p.y); }

} // namespace

std::array<float, 4> moodColor(const RgbaImage& gradient, float mood) {
    if (gradient.height == 0 || gradient.width == 0)
        return {1, 1, 1, 1};
    const float m = std::clamp(std::trunc(mood), -100.0f, 100.0f);
    const int h = int(gradient.height);
    const int y = std::clamp(h - int(std::lround(float(h) * (m + 100) / 200)), 0, h - 1);
    const uint8_t* p = &gradient.pixels[size_t(y) * gradient.width * 4];
    return {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, 1};
}

void MotiveTrend::add(float value) {
    values_.push_back(int(std::trunc(value)));
    if (values_.size() > 10)
        values_.pop_front();
    float sum = 0;
    for (int v : values_)
        sum += float(v);
    averages_.push_back(int(std::lround(sum / float(values_.size()))));
    if (averages_.size() > 10)
        averages_.pop_front();
}

int MotiveTrend::arrows() const {
    if (averages_.size() < 2)
        return 0;
    const int d = averages_.back() - averages_.front();
    const int a = std::abs(d);
    const int n = a == 0 ? 0 : a <= 2 ? 1 : a <= 4 ? 2 : 3;
    return d < 0 ? -n : n;
}

LiveUi::LiveUi(vm::VM& vm, ResourceIndex& index, const std::filesystem::path& resDir)
    : vm_(&vm), res_(std::make_unique<UiResources>(index, resDir)),
      fonts_(std::make_unique<FontLibrary>(resDir / "UI/Fonts")) {
    auto load = [&](uint32_t id, UiScript& into) {
        if (auto s = res_->script(id)) {
            into = std::move(*s);
            return true;
        }
        std::fprintf(stderr, "opensims2: live UI: no UI script %08X\n", id);
        return false;
    };
    ok_ = true;
    for (auto [id, into] : {std::pair{kPuckScript, &puck_}, {kLiveScript, &live_}, {kMotivesScript, &motives_},
                            {kBarScript, &bar_}, {kMemberScript, &member_}, {kIconScript, &icon_}})
        ok_ = load(id, *into) && ok_;
    if (auto c = res_->script(kQueueConstants))
        queueConstants_ = uiConstants(*c);
    for (size_t i = 0; i < kBars.size(); ++i) {
        bars_[i] = bar_;
        for (auto& r : bars_[i].roots)
            if (r.attr("captionres"))
                r.set("captionres", "{7f96c284," + [&] {
                    char b[16];
                    std::snprintf(b, sizeof b, "0082%04x", kBars[i].caption);
                    return std::string(b);
                }() + "}");
    }
    try {
        thumbnails_ = std::make_unique<DbpfPackage>(resDir / "UserData/Thumbnails/ObjectThumbnails.package");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: live UI: %s\n", e.what());
    }

    panels_ = std::make_unique<LivePanels>(*res_);
    if (auto p = res_->script(kPauseScript))
        pause_ = std::move(*p);
    if (auto p = res_->script(kOptionsScript))
        options_ = std::move(*p);
    if (!ok_)
        return;
    // --- What the panel's code does once (inferred placements: TODO.md) ----
    // The live panel with the wants closed and the needs subpanel open:
    // the caps of the other states (the script keeps them side by side as a
    // palette) are hidden; the mood meter and the subpanel buttons follow
    // the front cap at the offset the expanded palette shows (13 px into
    // it), with no wants area of the script (app/wants_panel.hpp draws ours); the
    // subpanel's tile runs from there under the needs panel to its end cap.
    UiElement& root = live_.roots.front();
    auto hideId = [&](uint32_t id) {
        if (UiElement* e = findIn(root, id))
            e->set("winflag_visible", "no");
    };
    // The platinum aspiration glows (0x49064805/07, 0x4BDEFFA2) only show
    // past 74 aspiration points (docs/ui-live-mac.md §4.2); the score is
    // simulated now (vm/wants.hpp), but these windows are not placed right
    // yet (shown, they cover the mood meter): kept hidden.
    for (uint32_t id : {0xA94E97F1u, 0xA94E97E6u, 0xA94E97ECu, 0x49064802u, 0xA94E97F4u, 0xA94E97F2u, 0x49064805u,
                        0x49064807u, 0x4BDEFFA2u, 0xA94E97E5u})
        hideId(id);
    for (uint32_t id : {0x49064801u, 0xA94E97E8u}) // Mood SubPanel / ButtonHolder - minimized
        if (UiElement* e = findIn(root, id)) {
            const auto a = e->area();
            e->set("area", area(kGroupX, a[1], kGroupX + a[2] - a[0], a[3]));
        }
    if (UiElement* e = findIn(root, 0xA94E97F5)) // SubPanel Background - Tile
        e->set("area", area(kGroupX + 13, 136, kSubPanelX + kSubPanelWidth - 3, 283)), e->set("blttype", "tiled"),
            e->set("winflag_visible", "yes");
    if (UiElement* e = findIn(root, 0xA94E97EB)) // End Cap - Collapsed - SubPanel
        e->set("area", area(kSubPanelX + kSubPanelWidth - 3, 136, kSubPanelX + kSubPanelWidth + 32, 283));
    // The portrait's shadow is the portrait's silhouette (not drawn here).
    if (UiElement* e = findIn(root, 0x4BDEFFA4))
        if (UiElement* s = e->child(2))
            s->set("winflag_visible", "no");
    // Family thumbnails: no shadow (as above).
    if (UiElement* g = findIn(member_.roots.front(), 0x1000))
        if (UiElement* s = g->child(2))
            s->set("winflag_visible", "no");
}

LiveUi::~LiveUi() = default;

void LiveUi::setSim(vm::ObjectId id) {
    if (id == sim_)
        return;
    sim_ = id;
    for (auto& t : trends_)
        t.clear();
    dirty_ = true;
}

void LiveUi::setPortrait(vm::ObjectId id, RgbaImage portrait) {
    portraits_[id] = std::make_unique<RgbaImage>(std::move(portrait));
    icons_.clear();
    dirty_ = true;
}

const RgbaImage* LiveUi::image(uint32_t instance) {
    return res_->image({kTypeUiImage, kUiImageGroup, instance});
}

std::vector<vm::ObjectId> LiveUi::family() const {
    std::vector<vm::ObjectId> all, family;
    const int16_t current = vm_->globals[vm::global::CurrentFamily];
    for (const auto& [id, o] : vm_->objects()) {
        if (!o.person)
            continue;
        all.push_back(id);
        if (current && o.person->data[vm::persondata::FamilyNumber] == current)
            family.push_back(id);
    }
    return family.empty() ? all : family;
}

void LiveUi::tick(double seconds) {
    const bool shown = tipVisible();
    const bool popupShown = popup_.script && now_ - popup_.since >= popup_.delay;
    now_ = seconds;
    if (tipHovered_ >= 0 && shown != tipVisible())
        dirty_ = true;
    if (popup_.script && popupShown != (now_ - popup_.since >= popup_.delay))
        dirty_ = true;
    if (sliding_)
        dirty_ = true;
    if (lastSample_ >= 0 && seconds - lastSample_ < 0.5)
        return;
    lastSample_ = seconds;
    const vm::Object* sim = vm_->object(sim_);
    if (!sim || !sim->person)
        return;
    for (size_t i = 0; i < kBars.size(); ++i)
        trends_[i].add(float(sim->person->motives[size_t(kBars[i].motive)]));
}

const RgbaImage* LiveUi::objectThumbnail(const vm::Object& part) {
    // A multi-tile object's thumbnail is its lead's (GetObjectThumbnail).
    const vm::Object* lead = vm_->object(vm::multiTileGroup(part));
    const vm::Object& o = lead && lead->definition ? *lead : part;
    const uint32_t guid = o.guid();
    auto it = objectThumbs_.find(guid);
    if (it == objectThumbs_.end()) {
        std::unique_ptr<RgbaImage> img;
        // ObjectThumbnails.package (cTSUIThumbnailService::
        // GetObjectThumbnailGUIDAndModelName): instance = CRC32 of the
        // lower-cased "<OBJD group as signed decimal><model name>", the model
        // name being the OBJD's entry of its group's STR# 0x85.
        std::string model;
        if (o.definition)
            if (const StringTable* names = vm_->code().strings(o.group, 0x85))
                model = names->get(o.definition->modelIndex(), lang::EnglishUS);
        if (thumbnails_ && !model.empty() && model != "embedded") {
            if (thumbIndex_.empty())
                for (const auto& e : thumbnails_->entries())
                    thumbIndex_.emplace(e.tgi.instance, &e);
            if (auto e = thumbIndex_.find(objectThumbnailInstance(o.group, model)); e != thumbIndex_.end())
                if (auto d = decodeUiImage(thumbnails_->read(*e->second)))
                    img = std::make_unique<RgbaImage>(std::move(*d));
        }
        if (!img && std::getenv("OS2_LIVE_UI_DUMP"))
            std::fprintf(stderr, "opensims2: live UI: no thumbnail for %s (model \"%s\")\n",
                         o.definition ? o.definition->fileName.c_str() : "?", model.c_str());
        it = objectThumbs_.emplace(guid, std::move(img)).first;
    }
    return it->second.get();
}

// The icon of a queue entry: the target Sim's portrait or the object's
// thumbnail (the interaction's target, else its object), scaled to the icon
// window, cut by the variant's circular mask (child 3's image) and grey while
// waiting (docs/ui-live-mac.md §6.2).
const RgbaImage* LiveUi::queueIcon(const vm::QueuedInteraction& q, bool running, bool nested) {
    // The picture of an object or Sim: a person's portrait, an object's
    // thumbnail (null when it has none, like the invisible markers).
    auto pictureOf = [&](vm::ObjectId id) -> const RgbaImage* {
        if (const vm::Object* o = vm_->object(id)) {
            if (o->person) {
                if (auto p = portraits_.find(id); p != portraits_.end())
                    return p->second.get();
                return nullptr;
            }
            return objectThumbnail(*o);
        }
        return nullptr;
    };
    // The interaction's target, else its object; when neither has a picture
    // (an interaction on a marker such as "Come And See"), the object, then
    // the Sim the queue belongs to, instead of an empty blue frame.
    vm::ObjectId who = q.target ? q.target : q.object;
    const RgbaImage* src = pictureOf(who);
    if (!src && q.object && q.object != who)
        if (const RgbaImage* alt = pictureOf(q.object))
            src = alt, who = q.object;
    if (!src)
        if (const RgbaImage* own = pictureOf(sim_))
            src = own, who = sim_;
    const auto key = std::make_tuple(who, running, nested);
    if (auto it = icons_.find(key); it != icons_.end())
        return it->second.get();
    std::unique_ptr<RgbaImage> out;
    const UiElement* top = icon_.roots.size() > size_t(nested) ? &icon_.roots[size_t(nested)] : nullptr;
    const UiElement* maskWin = top ? top->child(3) : nullptr;
    const RgbaImage* mask = nullptr;
    if (maskWin)
        if (auto ref = uiResourceRef(maskWin->get("image")))
            mask = res_->image(*ref);
    if (src && mask) {
        out = std::make_unique<RgbaImage>(scaledImage(*src, mask->width, mask->height));
        for (size_t i = 0; i < out->pixels.size(); i += 4) {
            uint8_t* p = &out->pixels[i];
            if (!running) {
                const uint8_t l = uint8_t((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
                p[0] = p[1] = p[2] = l;
            }
            p[3] = uint8_t(unsigned(p[3]) * mask->pixels[i + 3] / 255);
        }
    }
    return (icons_[key] = std::move(out)).get();
}

uint64_t LiveUi::stamp() const {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    mix(uint64_t(sim_));
    mix(uint64_t(laidW_) << 16 | uint64_t(laidH_));
    const auto& g = vm_->globals;
    mix(uint64_t(uint16_t(g[vm::global::Hour])) << 32 | uint16_t(g[vm::global::Minute]) << 16 |
        uint16_t(g[vm::global::DayOfWeek]));
    mix(uint64_t(uint16_t(g[vm::global::Speed])) << 16 | uint16_t(g[vm::global::Paused]));
    const int16_t fam = g[vm::global::CurrentFamily];
    if (auto it = vm_->families.find(fam); it != vm_->families.end())
        mix(uint64_t(uint32_t(it->second.funds)));
    if (const vm::Object* sim = vm_->object(sim_)) {
        if (sim->person)
            for (int16_t m : sim->person->motives)
                mix(uint64_t(uint16_t(m)));
        mix(uint64_t(sim->activeInteraction + 1));
        for (const auto& q : sim->queue)
            mix(uint64_t(q.uid) << 2 | uint64_t(q.cancelled) << 1), mix(uint64_t(uint16_t(q.object)));
        for (const auto& q : sim->subQueue)
            mix(uint64_t(q.uid) << 3 | 4), mix(uint64_t(uint16_t(q.object)));
        if (sim->subAction)
            mix(uint64_t(sim->subAction->uid) << 3 | uint64_t(sim->subAction->cancelled) << 1 | 1);
    }
    for (const auto& t : trends_)
        mix(uint64_t(t.arrows() + 8));
    for (vm::ObjectId id : family()) {
        mix(uint64_t(uint16_t(id)) << 1 | uint64_t(simAway && simAway(id)));
        if (const vm::Object* o = vm_->object(id); o && o->person)
            mix(uint64_t(uint16_t(o->person->motives[vm::motive::Mood])));
    }
    mix(uint64_t(hoverIcon_ + 1));
    mix(uint64_t(mode_ + 1) | uint64_t(liveEnabled_) << 8);
    mix(uint64_t(uint16_t(g[vm::global::BudgetDiv10000])) << 16 | uint16_t(g[vm::global::BudgetMod10000]));
    return h;
}

UiScreen LiveUi::screen(int width, int height) {
    UiScreen out;
    out.width = width, out.height = height;
    laidW_ = width, laidH_ = height;
    targets_.clear();
    solid_.clear();
    frameImages_.clear();
    callbacks_.clear();
    tips_.clear();
    ageMeter_ = {};
    if (!ok_)
        return out;
    const vm::Object* sim = vm_->object(sim_);
    const auto& g = vm_->globals;
    const bool paused = g[vm::global::Paused] != 0;
    const int speed = g[vm::global::Speed] >= 2 && g[vm::global::Speed] <= 4 ? g[vm::global::Speed] : 2;
    std::map<const UiElement*, std::array<float, 4>> rects;
    auto place = [&](const UiElement& e, float x, float y, UiDrawHooks& hk) {
        const auto a = e.area();
        hk.rects = &rects;
        layoutUiElement(e, x - float(a[0]), y - float(a[1]), *res_, *fonts_, out, hk);
    };
    auto keep = [&](RgbaImage img) {
        frameImages_.push_back(std::make_unique<RgbaImage>(std::move(img)));
        return frameImages_.back().get();
    };
    auto rectOf = [&](const UiElement* e) -> std::optional<std::array<float, 4>> {
        if (!e)
            return std::nullopt;
        if (auto it = rects.find(e); it != rects.end())
            return it->second;
        return std::nullopt;
    };
    // Windows laid out from a local copy (the family column, the popup) leave
    // the rectangles before the copy goes: the tooltips below read every
    // window in them (the popup's stayed and crashed in UiElement::attr).
    std::function<void(const UiElement&)> forget = [&](const UiElement& e) {
        rects.erase(&e);
        for (const auto& k : e.children)
            forget(k);
    };
    auto target = [&](const UiElement* e, Action a, int value) {
        if (auto r = rectOf(e))
            targets_.push_back({*r, a, value, e});
    };
    // A gradient fill bar (docs/ui-live-mac.md §5): child 3's image is the
    // mask, child 4's the gradient; the output replaces child 3's image.
    auto fill = [&](const UiElement& bar, float value, UiDrawHooks& hk) {
        const UiElement* maskWin = bar.child(3);
        const UiElement* gradWin = bar.child(4);
        if (!maskWin || !gradWin)
            return;
        const auto mref = uiResourceRef(maskWin->get("image")), gref = uiResourceRef(gradWin->get("image"));
        const RgbaImage* mask = mref ? res_->image(*mref) : nullptr;
        const RgbaImage* grad = gref ? res_->image(*gref) : nullptr;
        if (!mask || !grad)
            return;
        // kFillType is not in these scripts; a bar taller than wide fills
        // by columns (the mood meter: inferred, the rows of its gradient
        // never reach the green).
        const auto a = maskWin->area();
        const bool vertical = (a[3] - a[1]) > (a[2] - a[0]);
        hk.images[maskWin] = keep(gradientFill(*grad, *mask, value, -100, 100, vertical));
        hk.hide.insert(gradWin);
    };

    // --- Placement (inferred) ------------------------------------------------
    // The puck at the client's left edge (measured at 800x600 and 1280x720,
    // docs/buy-build-mac.md §1.6); the live panel joined to it by their
    // anchors (the panel's hidden rectangle captioned "4bdeffaa:bl" over the
    // puck's window 0x4BDEFFAA); the needs subpanel after the mood meter.
    const UiElement& puck = puck_.roots.front();
    const UiElement& live = live_.roots.front();
    const auto pa = puck.area();
    // The original's pod sits 3 px above the script's own reach to the
    // client's foot (the pill's art at 516/396 over the 519/399 of the
    // script's place), so the panel rises with it (measured: the panel's own
    // background at height-289 at 800x600; docs/buy-build-mac.md §1.6).
    const float puckX = 0, puckY = float(height) - float(pa[3] - pa[1]) - 3;
    float liveX = puckX + 5, liveY = puckY - 86;
    if (const UiElement* a = puck.child(0x4BDEFFAA))
        if (const UiElement* b = live.child(0x0000AAAA)) {
            liveX = puckX + float(a->area()[0] - b->area()[0]);
            liveY = puckY + float(a->area()[1] - b->area()[1]);
        }

    // --- The live panel (only in live mode: Buy mode draws its own) ----------
    const bool showLive = mode_ == 0 && !optionsOpen_; // the options panel takes the live panel's place
    if (showLive) {
        UiDrawHooks hk;
        // Speed and pause: the marked one pressed (docs/ui-live-mac.md §1.3).
        const std::array<std::pair<uint32_t, int>, 3> speeds = {{{kSpeed2, 2}, {kSpeed3, 3}, {kSpeed4, 4}}};
        for (auto [id, v] : speeds)
            if (const UiElement* b = findIn(live, id))
                hk.buttonState[b] = !paused && speed == v ? 2 : 1;
        if (const UiElement* b = findIn(live, kPause))
            hk.buttonState[b] = paused ? 2 : 1;
        // The clock: the day's name and the time (24 h, as the Brazilian
        // Portuguese locale; the game asks the locale's format service).
        if (const UiElement* c = findIn(live, kClock)) {
            char t[16];
            std::snprintf(t, sizeof t, "%02d:%02d", g[vm::global::Hour], g[vm::global::Minute]);
            std::string day;
            const int dow = std::clamp<int>(g[vm::global::DayOfWeek], 0, 6);
            if (auto s = res_->string({0x7F96C284, 0x8A, uint32_t(7 + dow)}))
                day = *s + " ";
            hk.captions[c] = day + t;
        }
        if (sim && sim->person) {
            // The portrait.
            if (const UiElement* thumb = findIn(live, 0x4BDEFFA4))
                if (const UiElement* t = thumb->child(1))
                    if (auto p = portraits_.find(sim_); p != portraits_.end())
                        hk.images[t] = p->second.get();
            // The age: only the Sim's age shows.
            const std::string age = vm::agePrefix(*sim);
            const std::map<std::string, uint32_t> ages = {
                {"p", 0xAD8A9F0B}, {"c", 0xAD8A9F0C}, {"t", 0xAD8A9F0D}, {"a", 0xAD8A9F0E}, {"e", 0xAD8A9F0F}};
            for (const auto& [prefix, id] : ages)
                if (const UiElement* e = findIn(live, id))
                    (prefix == age ? hk.show : hk.hide).insert(e);
            // Its progress bar: the fill cut at the share of the stage lived.
            ageMeter_ = ageMeter(sim->person->data[vm::persondata::PersonAge],
                                 sim->person->data[kPdAgeDaysLeft], sim->person->data[kPdAgeDuration]);
            if (ageMeter_.shown)
                if (const UiElement* e = findIn(live, ages.at(age)))
                    if (const UiElement* bar = e->child(1))
                        if (const UiElement* fillWin = bar->child(3))
                            if (auto ref = uiResourceRef(fillWin->get("image")))
                                if (const RgbaImage* full = res_->image(*ref))
                                    hk.images[fillWin] = keep(progressFill(*full, ageMeter_.fraction));
            // The mood meter (motive 3, docs/ui-live-mac.md §4.1).
            if (const UiElement* m = findIn(live, 0x49064803))
                fill(*m, float(sim->person->motives[vm::motive::Mood]), hk);
        }
        // The open subpanel's button, pressed.
        const UiElement* holder = findIn(live, 0xA94E97E8);
        if (holder)
            for (size_t i = 0; i < kSubButtons.size(); ++i)
                if (const UiElement* b = holder->child(kSubButtons[i]))
                    hk.buttonState[b] = int(i) == subPanel_ ? 2 : 1;
        if (sim && sim->person) {
            // The aspiration's icon (CD8B8B11-16) and meter (inferred: the
            // score in -100..100 on the gradient fill bar 0x4BDEFFA1).
            const int asp = sim->person->data[kPdAspiration];
            const std::array<std::pair<uint32_t, int>, 6> icons = {
                {{0xCD8B8B11, 2}, {0xCD8B8B12, 32}, {0xCD8B8B13, 1}, {0xCD8B8B14, 4}, {0xCD8B8B15, 16}, {0xCD8B8B16, 64}}};
            for (auto [id, bit] : icons)
                if (const UiElement* e = findIn(live, id))
                    (asp == bit || (asp == 0 && bit == 64) ? hk.show : hk.hide).insert(e);
            if (const UiElement* bar = findIn(live, 0x4BDEFFA1))
                fill(*bar, float(sim->person->data[kPdAspirationScore]), hk);
        }
        place(live, liveX, liveY, hk);
        if (holder)
            for (size_t i = 0; i < kSubButtons.size(); ++i)
                if (panels_ && (i == 0 || panels_->ok(LivePanels::Panel(i))))
                    target(holder->child(kSubButtons[i]), Action::SubPanel, int(i));
        target(findIn(live, 0x4BDEFFA4), Action::Centre, sim_);
        // The age meter's tooltip: "Teenager (Becomes an Adult in %d days)".
        if (ageMeter_.shown && sim && sim->person)
            if (auto r = rectOf(findIn(live, kAgesTooltip)))
                if (auto fmt = res_->string({0x7F96C284, 0x83, ageMeter_.string}); fmt && !fmt->empty()) {
                    std::string text = *fmt;
                    if (auto at = text.find("%d"); at != std::string::npos)
                        text.replace(at, 2, std::to_string(ageMeter_.days));
                    tips_.push_back({*r, text});
                }
        // Clicking the mood meter shows the mood popup 500 ms later, on the
        // anchor window 0x4906480D of the collapsed variant (§4.1).
        if (const UiElement* mb = findIn(live, 0x49064809))
            if (auto r = rectOf(mb)) {
                std::array<float, 4> anchor = {(*r)[0], (*r)[1] - 4, (*r)[2], (*r)[1] - 4};
                if (const UiElement* an = findIn(live, 0x4906480D)) {
                    const auto aa = an->area(), ba = mb->area();
                    const float cx = (*r)[0] + float(aa[0] + aa[2]) / 2 - float(ba[0]);
                    const float y = (*r)[1] + float(aa[1] - ba[1]);
                    anchor = {cx, y, cx, y};
                }
                callbacks_.push_back([this, anchor] {
                    if (closedPopupKind_ != 1)
                        openPopup(0x49064908, anchor, {}, 0.5, 1);
                });
                targets_.push_back({*r, Action::Callback, int(callbacks_.size() - 1)});
            }
        for (auto [id, v] : speeds)
            target(findIn(live, id), Action::Speed, v);
        target(findIn(live, kPause), Action::Pause, 0);
        for (uint32_t id : {0xA94E97E9u, 0xA94E97F3u, 0xA94E97E5u, 0xA94E97F5u, 0xA94E97EBu})
            if (auto r = rectOf(findIn(live, id)))
                solid_.push_back(*r);
        if (auto r = rectOf(findIn(live, 0xA94E97E4))) // Background: its lower part (the top is the portrait's sky)
            solid_.push_back({(*r)[0], (*r)[1] + 60, (*r)[2], (*r)[3]});
    }

    // --- The other subpanels (app/live_panels.hpp) ----------------------------
    if (showLive && sim && sim->person && subPanel_ != 0 && panels_) {
        PanelContext c{*vm_, *res_, *fonts_, out, rects,
                       keep,
                       [&](std::array<float, 4> r, std::function<void()> f) {
                           callbacks_.push_back(std::move(f));
                           targets_.push_back({r, Action::Callback, int(callbacks_.size() - 1)});
                       },
                       [&](std::array<float, 4> r) { solid_.push_back(r); },
                       [&](int16_t id) { return portraitOfNeighbor(id); },
                       [&](const std::string& name) { sound(name); },
                       [&](std::array<float, 4> r, std::string t) { tips_.push_back({r, std::move(t)}); },
                       [this](uint32_t script, std::array<float, 4> anchor, std::vector<std::pair<uint32_t, std::string>> texts,
                              std::vector<std::pair<uint32_t, const RgbaImage*>> images) {
                           // 2: the relationship description (its box grows to the text), 3 another
                           const int kind = script == 0x4906502B ? 2 : 3;
                           if (closedPopupKind_ != kind) {
                               openPopup(script, anchor, std::move(texts), 0, kind);
                               popup_.images = std::move(images);
                           }
                       }};
        panels_->layout(LivePanels::Panel(subPanel_), c, sim_, liveX + float(kSubPanelX), liveY + 140 + 138);
    }

    // --- Wants & Fears (app/wants_panel.hpp): a strip above the subpanel -----
    if (showLive && sim && sim->person) {
        PanelContext c{*vm_, *res_, *fonts_, out, rects, keep,
                       [&](std::array<float, 4> r, std::function<void()> f) {
                           callbacks_.push_back(std::move(f));
                           targets_.push_back({r, Action::Callback, int(callbacks_.size() - 1)});
                       },
                       [&](std::array<float, 4> r) { solid_.push_back(r); },
                       [&](int16_t id) { return portraitOfNeighbor(id); },
                       [&](const std::string& name) { sound(name); },
                       [&](std::array<float, 4> r, std::string t) { tips_.push_back({r, std::move(t)}); },
                       nullptr};
        layoutWantsPanel(c, sim_, liveX + float(kSubPanelX) + 8, liveY + 140 - 76, float(kSubPanelWidth) - 16);
    }

    // --- The needs subpanel ------------------------------------------------
    if (showLive && sim && sim->person && subPanel_ == 0) {
        const UiElement& panel = motives_.roots.front();
        const std::string age = vm::agePrefix(*sim);
        const uint32_t group = age == "p" || age == "b" ? 0xA003 : age == "c" ? 0xA004 : 0xA005;
        UiDrawHooks hk;
        for (uint32_t id : {0xA003u, 0xA004u, 0xA005u})
            if (const UiElement* e = panel.child(id))
                (id == group ? hk.show : hk.hide).insert(e);
        const UiElement* bg = panel.child(0xA002);
        const float bgX = bg ? float(bg->area()[0]) : 8;
        place(panel, liveX + float(kSubPanelX) - bgX, liveY + 140, hk);
        if (bg)
            if (auto r = rectOf(bg))
                solid_.push_back(*r);
        if (const UiElement* grp = panel.child(group))
            for (size_t i = 0; i < kBars.size(); ++i) {
                auto r = rectOf(grp->child(kBars[i].window));
                if (!r)
                    continue;
                const float v = float(sim->person->motives[size_t(kBars[i].motive)]);
                const int arrows = trends_[i].arrows();
                for (const UiElement& e : bars_[i].roots) {
                    UiDrawHooks bh;
                    const uint32_t id = e.id();
                    if (id >= 1 && id <= 3) // up: children 3, 2, 1 in that order
                        (arrows >= int(4 - id) ? bh.show : bh.hide).insert(&e);
                    else if (id >= 4 && id <= 6) // down: 4, 5, 6
                        (-arrows >= int(id - 3) ? bh.show : bh.hide).insert(&e);
                    else if (id == 7)
                        fill(e, v, bh);
                    bh.rects = &rects;
                    layoutUiElement(e, (*r)[0], (*r)[1], *res_, *fonts_, out, bh);
                }
            }
    }

    // --- The options panel (puck button 4) -----------------------------------
    // Joined to the puck like the live panel: its hidden rectangle captioned
    // "4bdeffaa:bl" (child 0xAAAA of 0xA200) over the puck's window.
    if (optionsOpen_ && !options_.roots.empty()) {
        const UiElement& root = options_.roots.front();
        const UiElement* box = findIn(root, 0xA200);
        const UiElement* anchor = findIn(root, 0xAAAA);
        float ox = puckX + 5, oy = puckY - 40;
        if (const UiElement* a = puck.child(0x4BDEFFAA); a && box && anchor) {
            ox = puckX + float(a->area()[0]) - float(box->area()[0] + anchor->area()[0]);
            oy = puckY + float(a->area()[1]) - float(box->area()[1] + anchor->area()[1]);
        }
        UiDrawHooks hk;
        for (uint32_t id : {0xA211u, 0xA250u})
            if (const UiElement* e = findIn(root, id))
                hk.hide.insert(e);
        if (const UiElement* e = findIn(root, 0xA210))
            if (!canLeave_)
                hk.hide.insert(e);
        place(root, ox, oy, hk);
        for (uint32_t id : {0xA000u, 0xA100u})
            if (auto r = rectOf(findIn(root, id)))
                solid_.push_back(*r);
        auto button = [&](uint32_t id, std::function<void()> f) {
            const UiElement* b = findIn(root, id);
            if (auto r = rectOf(b)) {
                callbacks_.push_back([this, f = std::move(f), snd = uiButtonSound(*b)] {
                    if (!snd.empty())
                        sound(snd);
                    if (f)
                        f();
                });
                targets_.push_back({*r, Action::Callback, int(callbacks_.size() - 1)});
            }
        };
        button(0xA210, [this] {
            std::printf("opensims2: live UI: to the neighborhood\n");
            if (onNeighborhood)
                onNeighborhood();
        });
        button(0xA220, [this] {
            std::printf("opensims2: live UI: quit\n");
            if (onQuit)
                onQuit();
        });
        button(0xA230, [this] {
            std::printf("opensims2: live UI: save\n");
            if (onSave)
                onSave();
        });
        for (const auto& [id, window] : {std::pair{0xA241u, 3}, {0xA242u, 1}, {0xA243u, 2}, {0xA245u, 4}, {0xA246u, 5}}) // graphics, audio, game, camera, credits
            button(id, [this, window = window] {
                if (onOptionWindow)
                    onOptionWindow(window);
            });
    }

    // --- The puck ----------------------------------------------------------
    const std::vector<vm::ObjectId> members = family();
    {
        UiDrawHooks hk;
        for (size_t i = 0; i < kModes.size(); ++i)
            if (const UiElement* b = puck.child(kModes[i]))
                hk.buttonState[b] = (i == 0 && !liveEnabled_) ? 0 : optionsOpen_ ? (i == 4 ? 2 : 1) : (int(i) == mode_ ? 2 : 1); // the options panel lights only its button
        if (const UiElement* f = puck.child(kFunds)) {
            const int16_t fam = g[vm::global::CurrentFamily];
            const auto it = vm_->families.find(fam);
            const int32_t funds = it != vm_->families.end()
                                      ? it->second.funds
                                      : int32_t(g[vm::global::BudgetDiv10000]) * 10000 + g[vm::global::BudgetMod10000];
            hk.captions[f] = formatMoney(funds);
        }
        // The family column grows up from the puck by the members' thumbnails
        // (inferred); only while live — the original has none above the puck
        // in buy mode (measured at 1280x720, docs/buy-build-mac.md §1.6).
        UiScript column;
        if (const UiElement* c = puck.child(0x4BDEFFAB)) {
            hk.hide.insert(c);
            if (mode_ == 0) {
                column.roots.push_back(*c);
                UiElement& col = column.roots.front();
                const int n = int(members.size());
                const int mid = std::max(31, n * 43);
                const auto a = c->area();
                const int bottom = a[1] + 44; // the middle piece's end
                col.set("area", area(a[0], bottom - mid - 13, a[2], bottom));
                if (UiElement* top = col.child(1))
                    top->set("area", area(0, 0, 18, 13));
                if (UiElement* m = col.child(2))
                    m->set("area", area(0, 13, 18, 13 + mid));
            }
        }
        // The wall button shows the current mode; the fan opens over the camera controls.
        for (uint32_t id = 0x1100; id <= 0x1103; ++id)
            if (const UiElement* b = findIn(puck, id))
                (int(id - 0x1100) == wallMode_ ? hk.show : hk.hide).insert(b);
        if (const UiElement* fan = findIn(puck, 0x4000)) {
            if (!wallFan_)
                hk.hide.insert(fan);
            for (uint32_t i = 1; i <= 4; ++i)
                if (const UiElement* b = fan->child(i))
                    hk.buttonState[b] = int(i) - 1 == wallMode_ ? 2 : 1;
        }
        place(puck, puckX, puckY, hk);
        if (const UiElement* fan = findIn(puck, 0x4000); fan && wallFan_)
            for (uint32_t i = 1; i <= 4; ++i)
                target(fan->child(i), Action::Walls, int(i) - 1); // before the buttons under it
        target(findIn(puck, 0x1100 + uint32_t(wallMode_)), Action::WallFan, 0);
        for (size_t i = 0; i < kModes.size(); ++i)
            target(puck.child(kModes[i]), Action::Mode, int(i));
        target(findIn(puck, 0x2001), Action::Floor, 1);
        target(findIn(puck, 0x2000), Action::Floor, -1);
        target(findIn(puck, 0x1002), Action::Zoom, 1);
        target(findIn(puck, 0x1003), Action::Zoom, -1);
        target(findIn(puck, 0x1001), Action::Rotate, -1);
        target(findIn(puck, 0x1000), Action::Rotate, 1);
        if (const UiElement* bmp = puck.child(0))
            if (auto r = rectOf(bmp))
                solid_.push_back(*r);
        for (const auto& c : puck.children)
            if (c.clsid() == "GZWinBMP" && c.visible())
                if (auto r = rectOf(&c))
                    solid_.push_back(*r);
        if (!column.roots.empty()) {
            UiDrawHooks ch;
            const auto ca = column.roots.front().area();
            place(column.roots.front(), puckX + float(ca[0]), puckY + float(ca[1]), ch);
            // The column is a local copy: drop its windows from the rectangles
            // (the tooltips read them after this block).
            forget(column.roots.front());
            // The thumbnails, centred on the column's middle piece.
            const RgbaImage* moodGradient = image(kMoodGradient);
            const UiElement& m = member_.roots.front();
            const float mx = puckX + float(ca[0]) + 9 - float(m.area()[2] - m.area()[0]) / 2;
            float my = puckY + float(ca[1]) + 13;
            for (vm::ObjectId id : members) {
                const vm::Object* o = vm_->object(id);
                UiDrawHooks th;
                if (const UiElement* grp = m.child(0x1000)) {
                    if (const UiElement* t = grp->child(1)) {
                        if (auto p = portraits_.find(id); p != portraits_.end()) {
                            th.images[t] = p->second.get();
                            // Out of the world (at work): the portrait greyed, and not a target.
                            if (simAway && simAway(id)) {
                                auto& g = awayPortraits_[id];
                                if (!g || g->width != p->second->width || g->height != p->second->height) {
                                    g = std::make_unique<RgbaImage>(*p->second);
                                    for (size_t i = 0; i + 3 < g->pixels.size(); i += 4) {
                                        const unsigned l = (g->pixels[i] * 77u + g->pixels[i + 1] * 150u + g->pixels[i + 2] * 29u) >> 8;
                                        g->pixels[i] = g->pixels[i + 1] = g->pixels[i + 2] = uint8_t(l * 3 / 5 + 40);
                                    }
                                }
                                th.images[t] = g.get();
                            }
                        }
                        else
                            th.hide.insert(t);
                    }
                    if (const UiElement* bgw = grp->child(3); bgw && moodGradient && o && o->person)
                        th.colors[bgw] = moodColor(*moodGradient, o->person->motives[vm::motive::Mood]);
                }
                // 0x3000 marks the selected Sim, 0x2000 the others (inferred).
                if (const UiElement* b = m.child(0x2000))
                    (id == sim_ ? th.hide : th.show).insert(b);
                if (const UiElement* b = m.child(0x3000))
                    (id == sim_ ? th.show : th.hide).insert(b);
                const bool away = simAway && simAway(id);
                place(m, mx, my, th);
                if (!away)
                    targets_.push_back({{mx, my, mx + 40, my + 43}, Action::SelectSim, id});
                solid_.push_back({mx, my, mx + 40, my + 43});
                my += 43;
            }
        }
    }

    // --- The action queue (docs/ui-live-mac.md §6) ----------------------------
    hoverIcon_ = -1;
    if (mode_ == 0 && sim && icon_.roots.size() >= 2) {
        auto constant = [&](const char* k, float d) {
            auto it = queueConstants_.find(k);
            return it == queueConstants_.end() ? d : std::stof(it->second);
        };
        const float x0 = constant("kLocationX", 60), y0 = constant("kLocationY", 10),
                    margin = constant("kIconMarginX", 5);
        // cTSUIActionQueue::OnTick: the running interaction, the running
        // sub-interaction, the queue, then the sub-queue (at most 12 icons).
        struct Entry {
            const vm::QueuedInteraction* q;
            bool running, nested;
        };
        std::vector<Entry> order;
        if (sim->activeInteraction >= 0 && size_t(sim->activeInteraction) < sim->queue.size())
            order.push_back({&sim->queue[size_t(sim->activeInteraction)], true, false});
        if (sim->subAction)
            order.push_back({&*sim->subAction, true, true});
        for (size_t i = 0; i < sim->queue.size(); ++i)
            if (int(i) != sim->activeInteraction)
                order.push_back({&sim->queue[i], false, false});
        for (const auto& q : sim->subQueue)
            order.push_back({&q, false, true});
        if (order.size() > 12)
            order.resize(12);
        const UiElement& topWin = icon_.roots[0];
        const UiElement& subWin = icon_.roots[1];
        QueueMetrics qm;
        qm.x0 = x0, qm.y0 = y0, qm.margin = margin;
        qm.topWidth = float(topWin.area()[2] - topWin.area()[0]);
        qm.subHeight = float(subWin.area()[3] - subWin.area()[1]);
        qm.subOffset = float(subWin.area()[1] - topWin.area()[3]);
        std::vector<bool> nestedFlags;
        for (const Entry& e : order)
            nestedFlags.push_back(e.nested);
        const std::vector<QueueSlot> slots = layoutActionQueue(nestedFlags, qm);
        const float w = qm.topWidth, subH = qm.subHeight;
        std::string hoverName;
        std::array<float, 4> hoverRect{};
        // Icons slide to their slots in kAnimationSteps steps (script
        // 0xCCD02691; the step time is inferred); a new one starts at the
        // slot of the one before it (the first at the left of its slot) and
        // plays ui_queue (docs/ui-live-mac.md §6.4).
        const int steps = std::max(1, int(constant("kAnimationSteps", 4)));
        constexpr double kStepSeconds = 1.0 / 30;
        const bool firstLook = slidesSim_ != sim_;
        if (firstLook)
            slides_.clear(), slidesSim_ = sim_;
        std::set<uint32_t> present;
        bool added = false;
        sliding_ = false;
        std::vector<std::pair<float, float>> at(order.size());
        for (size_t k = 0; k < order.size(); ++k) {
            const uint32_t key = uint32_t(order[k].nested) << 16 | order[k].q->uid;
            present.insert(key);
            const float tx = slots[k].x, ty = slots[k].y;
            auto it = slides_.find(key);
            if (it == slides_.end()) {
                IconSlide sl;
                sl.toX = tx, sl.toY = ty, sl.since = now_;
                sl.fromX = firstLook ? tx : k > 0 ? slots[k - 1].x : tx - w - margin;
                sl.fromY = firstLook ? ty : k > 0 ? slots[k - 1].y : ty;
                it = slides_.emplace(key, sl).first;
                added = added || !firstLook;
            } else if (it->second.toX != tx || it->second.toY != ty) {
                IconSlide& sl = it->second;
                const double p = std::min(1.0, (now_ - sl.since) / (kStepSeconds * steps));
                const int done = std::min(steps, int(p * steps));
                sl.fromX += (sl.toX - sl.fromX) * float(done) / float(steps);
                sl.fromY += (sl.toY - sl.fromY) * float(done) / float(steps);
                sl.toX = tx, sl.toY = ty, sl.since = now_;
            }
            const IconSlide& sl = it->second;
            const int done = std::min(steps, int(std::floor((now_ - sl.since) / kStepSeconds)) + 1);
            if (done < steps && (sl.fromX != sl.toX || sl.fromY != sl.toY))
                sliding_ = true;
            at[k] = {sl.fromX + (sl.toX - sl.fromX) * float(done) / float(steps),
                     sl.fromY + (sl.toY - sl.fromY) * float(done) / float(steps)};
        }
        std::erase_if(slides_, [&](const auto& kv) { return !present.count(kv.first); });
        if (added) {
            std::printf("opensims2: live UI: queue icon added\n");
            sound("ui_queue");
        }
        for (size_t k = 0; k < order.size(); ++k) {
            const auto& [qp, running, nested] = order[k];
            const vm::QueuedInteraction& q = *qp;
            const UiElement& win = nested ? subWin : topWin;
            const float ix = at[k].first, iy = at[k].second, iw = float(win.area()[2] - win.area()[0]);
            const bool connected = slots[k].connected;
            const std::array<float, 4> r{ix, iy, ix + iw, iy + subH};
            const bool hovered = mouse_ && inside(r, *mouse_);
            if (hovered)
                hoverIcon_ = int(k), hoverName = q.name, hoverRect = r;
            UiDrawHooks hk;
            const RgbaImage* ic = queueIcon(q, running, nested);
            if (const UiElement* c = win.child(1))
                (connected ? hk.hide : hk.show).insert(c);
            if (const UiElement* c = win.child(2))
                (connected ? hk.show : hk.hide).insert(c);
            if (const UiElement* c = win.child(3)) {
                if (ic)
                    hk.images[c] = ic;
                else
                    hk.hide.insert(c);
            }
            // Child 7 shows only behind an embedded thumbnail (Refresh).
            if (const UiElement* c = win.child(7))
                hk.hide.insert(c);
            // The red X is the icon's own mark, set by a click on it (CancelAction: "if the
            // Sim accepted, the icon is marked cancelled", docs/ui-live-mac.md §6.3), not the
            // VM's cancelled flag, which the VM also raises by itself (a priority dropped to
            // 0, an object gone) and which put an X on the Sim's current action.
            const bool cancelled = cancelled_.count({sim_, q.uid}) > 0;
            if (const UiElement* c = win.child(4))
                (cancelled ? hk.show : hk.hide).insert(c);
            // The "active" frame on the running ones, unless highlighted.
            if (const UiElement* c = win.child(5))
                (running && !hovered ? hk.show : hk.hide).insert(c);
            if (const UiElement* c = win.child(6))
                (hovered ? hk.show : hk.hide).insert(c);
            place(win, ix, iy, hk);
            targets_.push_back({r, Action::Cancel, q.uid});
            solid_.push_back(r);
        }
        // The marks of interactions that are gone (uids are not reused for a long time, but
        // the set would grow for ever).
        std::erase_if(cancelled_, [&](const std::pair<vm::ObjectId, uint16_t>& k) {
            return k.first == sim_ && std::none_of(order.begin(), order.end(), [&](const auto& e) { return e.q->uid == k.second; });
        });
        // The interaction's name under the hovered icon (the game opens a
        // cTSWinCalloutBox; its look is inferred).
        if (!hoverName.empty()) {
            const std::string name = hoverName.substr(hoverName.rfind('/') == std::string::npos ? 0 : hoverName.rfind('/') + 1);
            UiElement t;
            t.tag = "LEGACY";
            t.attrs = {{"clsid", "GZWinText"},          {"caption", name},         {"font", "LiveModePanelPuck"},
                       {"forecolor", "(5,6,113)"},       {"opaque", "yes"},         {"bkgcolor", "(178,193,255)"},
                       {"align", "center"},              {"gutters", "(4,2)"},      {"winflag_visible", "yes"}};
            float ppem = 12;
            float tw = 120;
            if (const FontStyle* st = fonts_->style("LiveModePanelPuck"))
                if (const Font* f = fonts_->font(*st))
                    ppem = float(st->size), tw = measureText(*f, decodeUtf8(name), ppem).width + 12;
            const float tx = std::max(0.0f, hoverRect[0] + w / 2 - tw / 2), ty = hoverRect[3] + 2;
            t.attrs.emplace_back("area", area(0, 0, int(std::ceil(tw)), int(std::ceil(ppem * 1.6f))));
            UiDrawHooks th;
            layoutUiElement(t, tx, ty, *res_, *fonts_, out, th);
        }
    }

    // --- The pause indicator (cTSUIPauseIndicator::SetArea, BV 0x96ea30) -----
    // Its eight pieces frame the screen: the corners at the corners, the
    // edges stretched between them.
    if (paused && pause_.roots.size() >= 8) {
        const float W = float(width), H = float(height);
        auto piece = [&](size_t i) -> const UiElement& { return pause_.roots[i]; };
        auto wOf = [&](size_t i) { return float(piece(i).area()[2] - piece(i).area()[0]); };
        auto hOf = [&](size_t i) { return float(piece(i).area()[3] - piece(i).area()[1]); };
        // 0 top left, 1 top, 2 top right, 3 left, 4 right, 5 bottom left, 6 bottom, 7 bottom right.
        const std::array<std::array<float, 4>, 8> at = {{
            {0, 0, wOf(0), hOf(0)},
            {wOf(0), 0, W - wOf(2), hOf(1)},
            {W - wOf(2), 0, W, hOf(2)},
            {0, hOf(0), wOf(3), H - hOf(5)},
            {W - wOf(4), hOf(2), W, H - hOf(7)},
            {0, H - hOf(5), wOf(5), H},
            {wOf(5), H - hOf(6), W - wOf(7), H},
            {W - wOf(7), H - hOf(7), W, H},
        }};
        for (size_t i = 0; i < 8; ++i) {
            UiElement e = piece(i);
            e.set("area", area(0, 0, int(std::lround(at[i][2] - at[i][0])), int(std::lround(at[i][3] - at[i][1]))));
            UiDrawHooks ph;
            ph.buttonState[&e] = 1;
            layoutUiElement(e, at[i][0], at[i][1], *res_, *fonts_, out, ph);
        }
    }

    // --- The popup (over everything but the tooltip) --------------------------
    if (mode_ == 0 && popup_.script && now_ - popup_.since >= popup_.delay) {
        auto it = popupScripts_.find(popup_.script);
        if (it == popupScripts_.end()) {
            auto sc = res_->script(popup_.script);
            it = popupScripts_.emplace(popup_.script, sc ? std::move(*sc) : UiScript{}).first;
        }
        if (!it->second.roots.empty()) {
            UiElement rootCopy = it->second.roots.front();
            // The relationship popup's description is longer than its
            // two-line window: the box grows to wrap it (the type under it).
            if (popup_.kind == 2) {
                UiElement* d = findWindow(rootCopy, 0xA2);
                UiElement* ty = findWindow(rootCopy, 0xA3);
                UiElement* bx = findWindow(rootCopy, 1);
                UiElement* tail = findWindow(rootCopy, 2);
                if (d && ty && bx && tail) {
                    std::string text;
                    for (const auto& [id, t] : popup_.texts)
                        if (id == 0xA2)
                            text = t;
                    float ppem = 12, lineH = 16, tw = 0;
                    if (const FontStyle* st = fonts_->style(d->get("font", "LiveModePanelBody")))
                        if (const Font* f = fonts_->font(*st)) {
                            ppem = float(st->size);
                            const TextMetrics m = measureText(*f, U"", ppem);
                            lineH = std::round(m.ascent + m.descent);
                            tw = measureText(*f, decodeUtf8(text), ppem).width;
                        }
                    const auto da = d->area();
                    const float avail = float(da[2] - da[0]) - 2;
                    const int lines = std::max(1, int(std::ceil(tw / avail * 1.12f)));
                    const int dh = int(float(lines) * lineH) + 6;
                    const auto ta = ty->area(), ba = bx->area(), tl = tail->area();
                    const int tyY = da[1] + dh - 4, boxH = tyY + (ta[3] - ta[1]) + 14;
                    ty->set("forecolor", "(5,6,113)"); // the script's light blue is unreadable on the translucent box
                    d->set("area", area(da[0], da[1], da[2], da[1] + dh));
                    ty->set("area", area(ta[0], tyY, ta[2], tyY + (ta[3] - ta[1])));
                    bx->set("area", area(ba[0], ba[1], ba[2], boxH));
                    tail->set("area", area(tl[0], boxH - 1, tl[2], boxH - 1 + (tl[3] - tl[1])));
                    const auto ra0 = rootCopy.area();
                    rootCopy.set("area", area(ra0[0], ra0[1], ra0[2], ra0[1] + boxH - 1 + (tl[3] - tl[1])));
                }
            }
            const UiElement& root = rootCopy;
            const auto ra = root.area();
            const float pw = float(ra[2] - ra[0]), ph = float(ra[3] - ra[1]);
            float tailX = pw / 2, tailBottom = ph; // the tail: child 2 (the picture under the box)
            if (const UiElement* tail = root.child(2))
                tailX = float(tail->area()[0] + tail->area()[2]) / 2 - float(ra[0]), tailBottom = float(tail->area()[3] - ra[1]);
            float px = (popup_.anchor[0] + popup_.anchor[2]) / 2 - tailX, py = popup_.anchor[1] - tailBottom;
            px = std::clamp(px, 0.0f, std::max(0.0f, float(width) - pw));
            py = std::max(0.0f, py);
            UiDrawHooks hk;
            for (const auto& [id, text] : popup_.texts)
                if (const UiElement* e = findWindow(root, id))
                    hk.captions[e] = text;
            for (const auto& [id, img] : popup_.images)
                if (const UiElement* e = findWindow(root, id); e && img)
                    hk.images[e] = img;
            hk.rects = &rects;
            const UiElement* box = root.child(1);
            layoutUiElement(root, px - float(ra[0]), py - float(ra[1]), *res_, *fonts_, out, hk);
            if (auto r = rectOf(box))
                popupRect_ = *r, solid_.push_back(*r);
            if (!popup_.texts.empty()) // the picture under the box takes clicks too
                if (auto r = rectOf(root.child(2)))
                    solid_.push_back(*r);
            forget(rootCopy); // a local copy, like the family column
        }
    }

    // --- Tooltips: the windows' tip strings (tipres) under the mouse --------
    // (Bars and groups carry theirs as a "Title|Body" captionres.)
    for (const auto& [e, r] : rects) {
        const std::string* t = e->attr("tipres");
        const std::string cls = e->clsid();
        if (!t && (cls == "GZWinGen" || cls == "GZWinFlatRect"))
            t = e->attr("captionres");
        if (t)
            if (auto ref = uiStringRef(*t))
                if (auto text = res_->string(*ref); text && !text->empty() &&
                                                    (e->attr("tipres") || text->find('|') != std::string::npos))
                    tips_.push_back({r, *text});
    }
    tip_ = -1;
    if (mouse_) {
        float best = 1e30f;
        for (size_t i = 0; i < tips_.size(); ++i) {
            const auto& r = tips_[i].first;
            const float a = (r[2] - r[0]) * (r[3] - r[1]);
            if (inside(r, *mouse_) && a < best)
                best = a, tip_ = int(i);
        }
    }
    if (tip_ != tipHovered_) // the panel changed under a still mouse: count again
        tipHovered_ = tip_, tipSince_ = now_;
    if (tip_ >= 0 && tipVisible())
        drawTooltip(out, tips_[size_t(tip_)].second, *mouse_);
    // The windows drawn, by id, for the tutorial (controlRect, controlAt).
    windows_.clear();
    for (const auto& [e, r] : rects)
        if (const uint32_t id = e->attr("id") ? e->id() : 0)
            windows_.push_back({id, r});
    if (auto it = rects.find(&puck_.roots.front()); it != rects.end())
        windows_.push_back({kPuckRoot, it->second});
    for (const Target& t : targets_)
        if (t.action == Action::SelectSim) { // the first family thumbnail ("B - First Sim in Chooser")
            windows_.push_back({kChooserThumbnail, t.rect});
            break;
        }
    return out;
}

std::optional<std::array<float, 4>> LiveUi::controlRect(uint32_t window, uint32_t control) {
    layoutIfNeeded();
    if (window == kSimChooser && control == kFirstSimInChooser)
        control = kChooserThumbnail, window = kMainWindow;
    auto contains = [](const std::array<float, 4>& a, const std::array<float, 4>& b) {
        return a[0] <= b[0] + 0.5f && a[1] <= b[1] + 0.5f && a[2] >= b[2] - 0.5f && a[3] >= b[3] - 0.5f;
    };
    std::optional<std::array<float, 4>> anywhere;
    bool windowDrawn = false; // a window not drawn as one (the aspirations' 0xCD8B8B10) holds any such control
    for (const auto& [wid, wr] : windows_)
        windowDrawn |= wid == window;
    for (const auto& [id, r] : windows_) {
        if (id != control)
            continue;
        if (window == kMainWindow) {
            // The main window's buttons are the puck's (other scripts reuse
            // the same small ids: a family thumbnail has a 0x1000).
            for (const auto& [pid, pr] : windows_)
                if (pid == kPuckRoot && contains(pr, r))
                    return r;
            if (!anywhere)
                anywhere = r;
            continue;
        }
        if (!windowDrawn)
            return r;
        // Inside the window: one drawn with that id that holds it (the
        // window's children lie in its area).
        for (const auto& [wid, wr] : windows_)
            if (wid == window && contains(wr, r))
                return r;
    }
    return anywhere;
}

bool LiveUi::controlAt(simd_float2 p, uint32_t window, uint32_t control) {
    const auto r = controlRect(window, control);
    return r && inside(*r, p);
}

// A tooltip (inferred look: the game's cTSWinCalloutBox): "Title|Body|||..."
// drawn as the title, then the body wrapped at 260 px, under the mouse.
void LiveUi::drawTooltip(UiScreen& out, const std::string& text, simd_float2 at) {
    // The fields after the body ("|||0x4906501c", "|0xab211110|...") name
    // pictures and the callout's script: not drawn.
    std::string t = text.substr(0, std::min(text.find("|||"), text.find("|0x")));
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' '))
        t.pop_back();
    std::string title = t, body;
    if (auto bar = t.find('|'); bar != std::string::npos)
        title = t.substr(0, bar), body = t.substr(bar + 1);
    drawGameTooltip(out, *res_, *fonts_, title, body, at.x, at.y);
}

void LiveUi::openPopup(uint32_t script, std::array<float, 4> anchor,
                       std::vector<std::pair<uint32_t, std::string>> texts, double delay, int kind) {
    popup_ = {script, anchor, std::move(texts), now_, delay, kind};
    sound("ui_text_description_open");
    dirty_ = true;
}

void LiveUi::closePopup() {
    if (!popup_.script)
        return;
    closedPopupKind_ = popup_.kind;
    popup_ = {};
    sound("ui_text_description_close");
    dirty_ = true;
}

const RgbaImage* LiveUi::portraitOfNeighbor(int16_t id) {
    auto it = neighborPortraits_.find(id);
    if (it == neighborPortraits_.end()) {
        std::unique_ptr<RgbaImage> img;
        // A Sim on the lot: its portrait; else the character file's.
        for (const auto& [oid, o] : vm_->objects())
            if (o.person && o.person->data[vm::persondata::NeighborId] == id)
                if (auto p = portraits_.find(oid); p != portraits_.end())
                    img = std::make_unique<RgbaImage>(*p->second);
        if (!img && neighborPortrait)
            if (auto n = vm_->neighbors.find(id); n != vm_->neighbors.end())
                if (auto p = neighborPortrait(n->second.guid))
                    img = std::make_unique<RgbaImage>(std::move(*p));
        it = neighborPortraits_.emplace(id, std::move(img)).first;
    }
    return it->second.get();
}

void LiveUi::selectSubPanel(int panel) {
    if (panel < 0 || panel >= LivePanels::Count || panel == subPanel_)
        return;
    subPanel_ = panel;
    if (panels_)
        panels_->resetPages();
    std::printf("opensims2: live UI: subpanel %d\n", panel);
    dirty_ = true;
}

void LiveUi::layoutIfNeeded() {
    if (canvasW_ > 0 && canvasH_ > 0 && (laidW_ != canvasW_ || laidH_ != canvasH_ || targets_.empty() || dirty_))
        screen(canvasW_, canvasH_);
}

void LiveUi::setSpeed(int speed) {
    // SetSimulatorSpeed: already at v and paused only unpauses.
    vm_->globals[vm::global::Speed] = int16_t(std::clamp(speed, 2, 4)), vm_->globals[vm::global::Paused] = 0;
    std::printf("opensims2: live UI: speed %d\n", speed);
    dirty_ = true;
}

void LiveUi::togglePause() {
    auto& paused = vm_->globals[vm::global::Paused];
    paused = paused ? 0 : 1;
    std::printf("opensims2: live UI: %s\n", paused ? "paused" : "unpaused");
    dirty_ = true;
}

bool LiveUi::overModeButton(simd_float2 p) {
    if (!ok_)
        return false;
    layoutIfNeeded();
    for (const Target& t : targets_)
        if ((t.action == Action::Mode || t.action == Action::WallFan || t.action == Action::Walls ||
             t.action == Action::Zoom || t.action == Action::Rotate || t.action == Action::Floor) &&
            inside(t.rect, p))
            return true;
    return false;
}

bool LiveUi::overPanel(simd_float2 p) {
    if (!ok_)
        return false;
    layoutIfNeeded();
    for (const Target& t : targets_)
        if (inside(t.rect, p))
            return true;
    for (const auto& r : solid_)
        if (inside(r, p))
            return true;
    return false;
}

bool LiveUi::press(simd_float2 p, bool right) {
    if (!ok_)
        return false;
    layoutIfNeeded();
    // A control the tutorial turned off takes the click and does nothing.
    if (!right && allowPress && !allowPress(p)) {
        for (const Target& t : targets_)
            if (inside(t.rect, p))
                return true;
    }
    // An open popup closes on any click; a click on the popup itself does nothing else.
    closedPopupKind_ = 0;
    if (popup_.script) {
        const bool shown = now_ - popup_.since >= popup_.delay;
        const bool onPopup = shown && inside(popupRect_, p);
        closePopup();
        if (onPopup)
            return true;
    }
    // The wall fan closes on any click; a click on one of its buttons picks it.
    if (wallFan_) {
        wallFan_ = false;
        dirty_ = true;
        bool onFan = false;
        for (const Target& t : targets_)
            onFan |= (t.action == Action::Walls || t.action == Action::WallFan) && inside(t.rect, p);
        if (!onFan) {
            std::printf("opensims2: live UI: wall fan closed\n");
            std::fflush(stdout);
        }
        for (const Target& t : targets_) {
            if (t.action == Action::WallFan && inside(t.rect, p))
                return true; // the wall button closes its own fan
            if (t.action != Action::Walls || !inside(t.rect, p) || right)
                continue;
            if (const std::string snd = uiButtonSound(*t.element); !snd.empty())
                sound(snd);
            static const char* const kNames[] = {"down", "cutaway", "up", "up with the roof"};
            std::printf("opensims2: live UI: walls %s\n", kNames[t.value]);
            if (onWallMode)
                onWallMode(t.value);
            std::fflush(stdout);
            return true;
        }
    }
    for (const Target& t : targets_) {
        if (!inside(t.rect, p))
            continue;
        if (t.action == Action::Walls)
            continue; // the fan is closed
        if (right && t.action == Action::SelectSim) {
            // cTSWinProcSimChooserThumbnail: the right button follows the
            // Sim, or stops following it.
            followed_ = followed_ == vm::ObjectId(t.value) ? 0 : vm::ObjectId(t.value);
            std::printf("opensims2: live UI: camera %s Sim %d\n", followed_ ? "follows" : "stops following", t.value);
            if (onFollowSim)
                onFollowSim(followed_);
            std::fflush(stdout);
            return true;
        }
        if (right && t.action != Action::Cancel) // the queue cancels with either button
            return true;
        if (t.action == Action::Mode && t.value == 0 && !liveEnabled_) { // HandleLiveModeButton: no Sims, no Live
            std::printf("opensims2: live UI: live button disabled (no Sims on the lot)\n");
            std::fflush(stdout);
            return true;
        }
        switch (t.action) {
        case Action::Speed:
        case Action::Pause:
        case Action::SubPanel:
        case Action::SelectSim:
        case Action::Centre:
        case Action::Mode:
        case Action::Floor:
        case Action::Zoom:
        case Action::Rotate:
        case Action::WallFan:
            // The button's own click sound (btnclicksnd, §7.7).
            if (t.element)
                if (const std::string snd = uiButtonSound(*t.element); !snd.empty())
                    sound(snd);
            break;
        default: break;
        }
        switch (t.action) {
        case Action::Centre:
            std::printf("opensims2: live UI: centre on Sim %d\n", t.value);
            if (onCentreSim)
                onCentreSim(vm::ObjectId(t.value));
            break;
        case Action::Speed: setSpeed(t.value); break;
        case Action::Pause: togglePause(); break;
        case Action::SubPanel: selectSubPanel(t.value); break;
        case Action::Callback:
            if (t.value >= 0 && size_t(t.value) < callbacks_.size()) {
                const auto f = callbacks_[size_t(t.value)];
                f();
            }
            break;
        case Action::SelectSim:
            if (vm::ObjectId(t.value) != sim_) {
                std::printf("opensims2: live UI: select Sim %d\n", t.value);
                if (onSelectSim)
                    onSelectSim(vm::ObjectId(t.value));
                setSim(vm::ObjectId(t.value));
            } else {
                // The selected Sim's thumbnail centres the camera on it and stops
                // following a Sim (cTSWinProcSimChooserThumbnail::onBtnClickedID, BV
                // 0x298a44: CenterOnSim, then AutotrackSim(null, false)).
                std::printf("opensims2: live UI: centre on Sim %d\n", t.value);
                if (onCentreSim)
                    onCentreSim(sim_);
                if (followed_ != 0) {
                    followed_ = 0;
                    if (onFollowSim)
                        onFollowSim(0);
                }
            }
            break;
        case Action::Cancel:
            if (vm::Object* sim = vm_->object(sim_); sim && !cancelled_.count({sim_, uint16_t(t.value)})) {
                const uint16_t uid = uint16_t(t.value);
                bool ok = vm::cancelInteraction(*vm_, *sim, uid);
                // A sub-interaction: a waiting one leaves the sub-queue, the
                // running one is told to stop (its Idle for Input returns).
                if (!ok && sim->subAction && sim->subAction->uid == uid)
                    sim->subAction->cancelled = ok = true;
                if (!ok)
                    ok = std::erase_if(sim->subQueue, [&](const vm::QueuedInteraction& x) { return x.uid == uid; }) > 0;
                std::printf("opensims2: live UI: cancel queue entry %d: %s\n", t.value, ok ? "ok" : "refused");
                if (ok) {
                    cancelled_.insert({sim_, uint16_t(t.value)});
                    sound("ui_queue_delete");
                }
            }
            break;
        case Action::Zoom:
            if (onZoom)
                onZoom(t.value);
            break;
        case Action::Rotate:
            if (onRotate)
                onRotate(t.value);
            break;
        case Action::WallFan:
            wallFan_ = true;
            std::printf("opensims2: live UI: wall fan open\n");
            break;
        case Action::Walls: break;
        case Action::Floor:
            std::printf("opensims2: live UI: floor %s\n", t.value > 0 ? "up" : "down");
            if (onFloor)
                onFloor(t.value);
            break;
        case Action::Mode:
            if (t.value == 4) { // options: opens and closes its panel
                optionsOpen_ = !optionsOpen_;
                std::printf("opensims2: live UI: options panel %s\n", optionsOpen_ ? "open" : "closed");
                break;
            }
            optionsOpen_ = false;
            std::printf("opensims2: live UI: mode button %d%s\n", t.value,
                        t.value == 0 ? " (live)" : !onMode ? " (inert)" : t.value == 1 ? " (buy)" : t.value == 2 ? " (build)" : "");
            if (onMode)
                onMode(t.value);
            break;
        case Action::None: break;
        }
        std::fflush(stdout);
        dirty_ = true;
        return true;
    }
    for (const auto& r : solid_)
        if (inside(r, p))
            return true;
    return false;
}

void LiveUi::hover(simd_float2 p) {
    mouse_ = p;
    layoutIfNeeded();
    int over = -1, k = 0;
    for (const Target& t : targets_)
        if (t.action == Action::Cancel) {
            if (inside(t.rect, p))
                over = k;
            ++k;
        }
    if (over != hoverIcon_)
        dirty_ = true;
    int tip = -1;
    float best = 1e30f;
    for (size_t i = 0; i < tips_.size(); ++i) {
        const auto& r = tips_[i].first;
        const float a = (r[2] - r[0]) * (r[3] - r[1]);
        if (inside(r, p) && a < best)
            best = a, tip = int(i);
    }
    if (tip != tipHovered_) // another control: the count starts again
        tipHovered_ = tip, tipSince_ = now_;
    if (tip != tip_ || tip >= 0) // a tooltip follows the mouse
        dirty_ = true;
}

#ifndef OS2_SDL_GPU
void LiveUi::attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat) {
    for (auto& l : layers_)
        l = std::make_unique<UiLayer>(device, colorFormat, depthFormat);
}

void LiveUi::encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale) {
    if (!layers_[0] || width == 0 || height == 0 || !ok_)
        return;
    const int cw = int(std::lround(float(width) / std::max(scale, 0.01f)));
    const int ch = int(std::lround(float(height) / std::max(scale, 0.01f)));
    if (cw != canvasW_ || ch != canvasH_)
        canvasW_ = cw, canvasH_ = ch, dirty_ = true;
    if (const uint64_t s = stamp(); s != stamp_)
        stamp_ = s, dirty_ = true;
    if (dirty_) {
        layer_ = (layer_ + 1) % layers_.size();
        const UiScreen sc = screen(cw, ch);
        if (const char* dump = std::getenv("OS2_LIVE_UI_DUMP")) // debug: the screen drawn on the CPU
            writePng(dump, composeUi(sc, {0, 0, 0, 0}));
        layers_[layer_]->setScreen(sc);
        stamp_ = stamp();
        dirty_ = false;
    }
    layers_[layer_]->encode(enc, width, height);
}
#endif

} // namespace os2
