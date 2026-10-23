// Windows host: a real lot, its light map, its objects, Sims driven by the
// SimAntics VM, and one image from the game's own UI scripts.

#include "app/lot_audio.hpp"
#include "app/lot_vm.hpp"
#include "audio/decode.hpp"
#include "formats/image.hpp"
#include "formats/lighting_config.hpp"
#include "formats/lot.hpp"
#include "formats/movie.hpp"
#include "formats/neighborhood.hpp"
#include "formats/lot_objects.hpp"
#include "formats/lot_lightmap.hpp"
#include "formats/lot_model.hpp"
#include "formats/resindex.hpp"
#include "formats/sim.hpp"
#include "formats/sim_actor.hpp"
#include "formats/sim_censor.hpp"
#include "formats/sim_character.hpp"
#include "formats/skinning.hpp"
#include "vm/lot.hpp"
#include "formats/effects.hpp"
#include "app/live_ui.hpp"
#include "app/game_save.hpp"
#include "app/build_mode.hpp"
#include "app/buy_mode.hpp"
#include "app/cas_mode.hpp"
#include "app/game_flow.hpp"
#include "app/head_shot.hpp"
#include "app/hood_mode.hpp"
#include "app/game_cursors.hpp"
#include "app/loading_screen.hpp"
#include "app/main_menu.hpp"
#include "app/movie_host.hpp"
#include "app/scene_loader.hpp"
#include "app/tv_broadcast.hpp"
#include "app/options_panel.hpp"
#include "app/tutorial.hpp"
#include "app/user_data.hpp"
#include "render/camera.hpp"
#include "formats/camera_config.hpp"
#include "app/camera_follow.hpp"
#include "app/camera_input.hpp"
#include "app/wall_view.hpp"
#include "app/lot_effects.hpp"
#include "app/lot_lighting.hpp"
#include "app/vm_objects.hpp"
#include "app/pie_menu.hpp"
#include "formats/dbpf.hpp"
#include "vm/go_here.hpp"
#include "vm/interactions.hpp"
#include "vm/wants.hpp"
#include "vm/world.hpp"
#include "formats/ui_image.hpp"
#include "formats/character_write.hpp"
#include "formats/cas_catalog.hpp"
#include "formats/cas_face.hpp"
#include "vm/baby_character.hpp"
#include "vm/dialog.hpp"
#include "app/door_portals.hpp"
#include "port/files.hpp"
#include "formats/ui_resources.hpp"
#include "render/particle_system.hpp"
#include "formats/ui_script.hpp"
#include "render/gpu_renderer.hpp"

#include <SDL3/SDL.h>

#include <simd/simd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#endif

#ifdef _WIN32
namespace {

LONG WINAPI crashFilter(EXCEPTION_POINTERS* info) {
    std::FILE* out = std::fopen("build-win/crash.txt", "w");
    if (!out)
        out = std::fopen("crash.txt", "w");
    if (!out || !info || !info->ExceptionRecord || !info->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    std::fprintf(out, "exception %08lx at %p\n", static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode),
                 info->ExceptionRecord->ExceptionAddress);
    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(process, nullptr, TRUE);
    CONTEXT context = *info->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 48; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr))
            break;
        char buffer[sizeof(SYMBOL_INFO) + 256];
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol))
            std::fprintf(out, "%d %s+0x%llx\n", i, symbol->Name, static_cast<unsigned long long>(displacement));
        else
            std::fprintf(out, "%d %llx\n", i, static_cast<unsigned long long>(frame.AddrPC.Offset));
        if (frame.AddrReturn.Offset == 0)
            break;
    }
    std::fclose(out);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace
#endif

namespace {

constexpr uint32_t kTypeCres = 0xE519C933;

std::string lower(std::string s) {
    for (auto& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string cresOf(os2::ResourceIndex& index, const os2::Lot::Object& object, std::map<std::string, os2::Tgi>& definitions) {
    if (index.findByName(object.model + "_cres", kTypeCres))
        return object.model;
    if (definitions.empty())
        for (const auto& loc : index.ofType(0x4F424A44))
            definitions.emplace(lower(os2::ObjectDefinition::parse(index.read(loc)).fileName), loc.entry->tgi);
    std::string name = object.definition;
    if (name.ends_with("_objt"))
        name.resize(name.size() - 5);
    auto it = definitions.find(lower(name));
    const std::string cres = it == definitions.end() ? std::string() : os2::objectCresName(index, it->second);
    return cres.empty() ? object.model : cres;
}

simd_float4x4 placementOf(const os2::Lot::Object& object) {
    const auto& q = object.rotation;
    simd_float4x4 at = simd_matrix4x4(simd_quaternion(q[0], q[1], q[2], q[3]));
    at.columns[3] = simd_make_float4(object.position[0], object.position[1], object.position[2], 1);
    return at;
}

const os2::RgbaImage* firstUiImage(os2::UiResources& ui, os2::ResourceIndex& index, float& x, float& y, float& w, float& h) {
    for (const auto& loc : index.ofType(os2::kTypeUiScript)) {
        std::vector<uint8_t> bytes;
        try {
            bytes = index.read(loc);
        } catch (...) {
            continue;
        }
        if (os2::isReia(bytes))
            continue;
        const os2::UiScript script = os2::UiScript::parse(bytes);
        const os2::RgbaImage* found = nullptr;
        auto walk = [&](auto&& self, const os2::UiElement& element, int ox, int oy) -> bool {
            const auto area = element.area();
            const int x0 = ox + area[0], y0 = oy + area[1];
            const int width = std::max(0, area[2] - area[0]), height = std::max(0, area[3] - area[1]);
            if (const std::string* image = element.attr("image")) {
                if (const auto ref = os2::uiResourceRef(*image)) {
                    if (const os2::RgbaImage* picture = ui.image(*ref)) {
                        if (picture->width >= 64 && picture->height >= 64 && width >= 64 && height >= 64) {
                            found = picture;
                            x = float(x0);
                            y = float(y0);
                            w = float(width);
                            h = float(height);
                            return true;
                        }
                    }
                }
            }
            for (const auto& child : element.children)
                if (self(self, child, x0, y0))
                    return true;
            return false;
        };
        for (const auto& root : script.roots)
            if (walk(walk, root, 0, 0))
                return found;
    }
    return nullptr;
}

std::string idleFor(const os2::SkinnedModel& skin) {
    const std::string& name = skin.skeleton.name;
    if (!name.empty() && std::tolower(static_cast<unsigned char>(name[0])) == 'b')
        return "b-floor-idle-loopBreathe";
    if (!name.empty() && std::tolower(static_cast<unsigned char>(name[0])) == 'c')
        return "c-idle-neutral-armsSide-wtC-breatheLoop50";
    if (!name.empty() && std::tolower(static_cast<unsigned char>(name[0])) == 'p')
        return "p-idle-neutral-lookAround";
    return "a-idle-neutral-armsSide-wtC-breatheLoop";
}

struct Camera {
    simd_float3 pivot{20, 20, 0};
    float yaw = 0.8f;
    float pitch = 0.55f;
    float distance = 28.f;

    simd_float3 eye() const {
        const float horiz = distance * std::cos(pitch);
        return {pivot.x + std::cos(yaw) * horiz, pivot.y + std::sin(yaw) * horiz, pivot.z + distance * std::sin(pitch)};
    }

    void basis(simd_float3& right, simd_float3& up) const {
        const simd_float3 eyeAt = eye();
        const simd_float3 forward = simd_normalize(pivot - eyeAt);
        right = simd_normalize(simd_cross(forward, simd_float3{0, 0, 1}));
        up = simd_cross(right, forward);
    }

    simd_float4x4 viewProj(float aspect) const {
        const float horiz = distance * std::cos(pitch);
        const simd_float3 eye{pivot.x + std::cos(yaw) * horiz, pivot.y + std::sin(yaw) * horiz, pivot.z + distance * std::sin(pitch)};
        const simd_float3 f = simd_normalize(pivot - eye);
        const simd_float3 s = simd_normalize(simd_cross(f, simd_float3(0, 0, 1)));
        const simd_float3 u = simd_cross(s, f);
        const simd_float4x4 view = simd_matrix(simd_make_float4(s.x, u.x, -f.x, 0), simd_make_float4(s.y, u.y, -f.y, 0),
                                               simd_make_float4(s.z, u.z, -f.z, 0),
                                               simd_make_float4(-simd_dot(s, eye), -simd_dot(u, eye), simd_dot(f, eye), 1));
        const float fov = 50.f * 3.14159265f / 180.f;
        const float t = 1.f / std::tan(fov * 0.5f);
        const float ys = t * std::min(aspect, 4.f / 3.f);
        const float xs = ys / aspect;
        const float n = 0.1f, fa = 400.f;
        const float zs = fa / (n - fa);
        const simd_float4x4 projection = simd_matrix(simd_make_float4(xs, 0, 0, 0), simd_make_float4(0, ys, 0, 0),
                                                     simd_make_float4(0, 0, zs, -1), simd_make_float4(0, 0, zs * n, 0));
        return simd_mul(projection, view);
    }
};

std::filesystem::path exeDir() {
#ifdef _WIN32
    wchar_t modulePath[MAX_PATH];
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    return std::filesystem::path(modulePath).parent_path();
#else
    return SDL_GetBasePath(); // the macOS check build (OS2_WINDOWS_CHECK)
#endif
}

// The game folder when started without arguments (a double-click), as the Mac
// host's findGameDir: %OS2_DATA%, the exe's folder or one above it holding the
// data (itself, game\, data\game\ or the testers' opensims2-data\ beside the
// unzipped app), then the CD install's folders.
std::optional<std::filesystem::path> findGameDir(const std::filesystem::path& exe) {
    namespace fs = std::filesystem;
    constexpr const char* check = "TSData/Res/UserData/Cameras/cameras.txt"; // the Mac host's kCameraConfig
    std::error_code ec;
    auto in = [&](const fs::path& dir) -> std::optional<fs::path> {
        for (const char* sub : {"", "game", "data/game", "opensims2-data"})
            if (fs::exists(dir / sub / check, ec))
                return dir / sub;
        return std::nullopt;
    };
    if (const char* env = std::getenv("OS2_DATA"))
        if (auto found = in(env))
            return found;
    for (fs::path dir = exe; !dir.empty(); dir = dir.parent_path()) {
        if (auto found = in(dir))
            return found;
        if (dir == dir.parent_path())
            break;
    }
    for (const char* install : {"C:/Program Files (x86)/EA GAMES/The Sims 2", "C:/Program Files/EA GAMES/The Sims 2"})
        if (auto found = in(install))
            return found;
    return std::nullopt;
}

// The macOS virtual key codes the shared modes read (CasMode, HoodMode, the bins' text boxes).
unsigned short macKeyCode(SDL_Scancode code) {
    switch (code) {
    case SDL_SCANCODE_BACKSPACE: return 51;
    case SDL_SCANCODE_DELETE: return 117;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 36;
    case SDL_SCANCODE_TAB: return 48;
    case SDL_SCANCODE_ESCAPE: return 53;
    case SDL_SCANCODE_LEFT: return 123;
    case SDL_SCANCODE_RIGHT: return 124;
    case SDL_SCANCODE_DOWN: return 125;
    case SDL_SCANCODE_UP: return 126;
    case SDL_SCANCODE_HOME: return 115;
    case SDL_SCANCODE_END: return 119;
    case SDL_SCANCODE_Z: return 6;
    case SDL_SCANCODE_X: return 7;
    case SDL_SCANCODE_Y: return 16;
    case SDL_SCANCODE_COMMA: return 43;
    case SDL_SCANCODE_PERIOD: return 47;
    default: return 0xFFFF;
    }
}

// What the app shares across scenes (the Mac host's globals): the game's data,
// the user's folder and options, and the one window every scene draws into.
struct App {
    std::filesystem::path res, gameDir, exe;
    std::vector<std::string> args; // args[0] is the executable; the scene flags of the running scene
    std::optional<std::filesystem::path> shot;
    std::unique_ptr<os2::ResourceIndex> index;
    std::unique_ptr<os2::UserData> user;
    os2::GameSettings settings;
    SDL_Window* window = nullptr;
    std::optional<uint32_t> createdFamily;     // CAS -> the neighborhood that opened it
    std::optional<os2::LotCamera::Pose> hoodPose; // the neighborhood's camera across a reload
    bool introPlayed = false;                     // the start-up movies, before the first main menu

    os2::ResourceIndex& resources() {
        if (!index) {
            const std::vector<std::filesystem::path> roots = os2::installationResRoots(res);
            std::printf("opensims2: indexing %zu resource tree%s of %s\n", roots.size(), roots.size() == 1 ? "" : "s",
                        res.string().c_str());
            std::fflush(stdout);
            index = std::make_unique<os2::ResourceIndex>();
            for (const auto& root : roots)
                index->addRoot(root);
        }
        return *index;
    }
    std::optional<std::string> argument(std::string_view flag) const { return os2::argValue(args, std::string(flag)); }
    // --hood: a folder as given, else the user's copy of the shipped neighborhood (made on first use).
    std::filesystem::path hoodFolder(const std::string& query) const {
        std::error_code ec;
        if (std::filesystem::is_directory(query, ec))
            return query;
        return user->neighborhood(query);
    }
};

// The option windows' volumes and mutes on the mixer's groups (the Mac host's applyAudioSettings).
void applyAudioSettings(os2::GameAudio& audio, const os2::GameSettings& settings) {
    const os2::SoundGroup groups[4] = {os2::SoundGroup::Vox, os2::SoundGroup::Sfx, os2::SoundGroup::Music,
                                       os2::SoundGroup::Ambience};
    for (size_t g = 0; g < 4; ++g)
        audio.mixer().setGroupVolume(groups[g], settings.muted[g] ? 0.0f : settings.volume[g]);
}

// The game's mouse cursors (app/game_cursors.hpp) as SDL cursors, loaded on
// first use; the system arrow stands in for a missing file.
void setGameCursor(const std::filesystem::path& res, os2::GameCursor which) {
    static std::array<SDL_Cursor*, size_t(os2::GameCursor::Count)> cursors{};
    static SDL_Cursor *arrow = nullptr, *shown = nullptr;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
        const std::filesystem::path dir = res / "UI" / "Cursors";
        for (size_t i = 0; i < cursors.size(); ++i) {
            const std::filesystem::path f = os2::findCursorFile(dir, os2::gameCursorName(os2::GameCursor(i)));
            if (f.empty())
                continue;
            std::ifstream in(f, std::ios::binary);
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto img = os2::decodeCursor(bytes);
            if (!img)
                continue;
            if (SDL_Surface* s = SDL_CreateSurfaceFrom(img->width, img->height, SDL_PIXELFORMAT_RGBA32, img->rgba.data(),
                                                       img->width * 4)) {
                cursors[i] = SDL_CreateColorCursor(s, img->hotX, img->hotY);
                SDL_DestroySurface(s);
            }
        }
        std::printf("opensims2: %zu of %zu cursors\n",
                    size_t(std::count_if(cursors.begin(), cursors.end(), [](SDL_Cursor* c) { return c != nullptr; })),
                    cursors.size());
    }
    SDL_Cursor* c = cursors[size_t(which)] ? cursors[size_t(which)] : arrow;
    if (c && c != shown) {
        SDL_SetCursor(c);
        shown = c;
    }
}

// Plays what app/movie_host.hpp has started, in the window, until it ends
// (Esc, Space, Return or a click skip a movie). False: the window was closed.
bool playMoviesInWindow(os2::Renderer& renderer) {
    while (os2::moviePlaying()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                while (os2::moviePlaying()) {
                    os2::skipMovie();
                    os2::movieHostFrame();
                }
                return false;
            }
            const bool key = event.type == SDL_EVENT_KEY_DOWN &&
                             (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_SPACE || event.key.key == SDLK_RETURN);
            if (key || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                os2::skipMovie();
        }
        if (const os2::RgbaImage* picture = os2::movieHostFrame())
            renderer.drawMovie(*picture);
    }
    return true;
}

// A frame of the scene, or of the movie over it (the end of its loading screen).
bool drawFrame(os2::Renderer& renderer, const simd_float4x4& viewProj) {
    if (const os2::RgbaImage* picture = os2::movieHostFrame())
        return renderer.drawMovie(*picture), true;
    return renderer.draw(viewProj);
}

// The loading screen's case from the neighborhood's lot info (the Mac host's
// fillLotEntryFromFlags): the lot's family or name, the neighborhood's name,
// or the family made in Create-a-Sim.
os2::LotEntry lotEntryOf(const App& app, const os2::Scene& scene) {
    os2::LotEntry entry;
    if (scene.hood.empty() || (scene.kind != os2::Scene::Kind::Lot && scene.kind != os2::Scene::Kind::Neighborhood))
        return entry;
    try {
        const os2::hood::Neighborhood hood = os2::hood::Neighborhood::load(app.hoodFolder(scene.hood), false);
        const auto thumbs = hood.package.parent_path() / "Thumbnails" / (hood.prefix + "_FamilyThumbnails.package");
        if (scene.kind == os2::Scene::Kind::Neighborhood) {
            entry.hoodName = hood.name;
            entry.tutorial = hood.prefix == "Tutorial";
            if (app.createdFamily)
                if (const auto* f = hood.family(*app.createdFamily)) {
                    entry.withFamily = true, entry.familyId = f->id, entry.familyName = f->name;
                    entry.familyThumbs = thumbs;
                }
            return entry;
        }
        const os2::hood::Lot* lot = hood.lot(scene.lot);
        if (!lot)
            return entry;
        entry.known = true;
        entry.type = lot->info.type;
        entry.family = !lot->families.empty();
        entry.name = lot->name;
        entry.familyThumbs = thumbs;
        if (entry.family) {
            entry.familyId = lot->families.front();
            if (const auto* f = hood.family(entry.familyId))
                entry.familyName = f->name;
        }
    } catch (const std::exception&) {
    }
    return entry;
}

// A scene's loading screen (app/loading_screen.hpp). The Mac loads the scene
// on a worker while the main thread draws it; here the scene loads on this
// thread (SDL draws to the window from the thread that made it), so the
// screen is drawn at the load's steps, at most 30 times a second. Headless
// only with --loading-shot <file.png> [--loading-at <fraction>]: the screen
// saved once the load reaches that far (default 0.5).
class SceneLoading {
public:
    SceneLoading(App& app, os2::Renderer& renderer, const os2::Scene& scene, bool cas)
        : renderer_(renderer), start_(std::chrono::steady_clock::now()) {
        if (auto shot = app.argument("--loading-shot"))
            shot_ = *shot, shotAt_ = std::stof(app.argument("--loading-at").value_or("0.5"));
        else if (app.shot)
            return;
        os2::loading::begin();
        active_ = os2::playLoadingScreen(os2::loadingScreenFor(app.res, scene, cas, lotEntryOf(app, scene)),
                                         [] { return os2::loading::shown(); });
        step(0);
    }
    ~SceneLoading() { finish(); }

    void step(float progress) {
        if (!active_)
            return;
        os2::loading::set(progress);
        if (!shot_.empty() && progress >= shotAt_) {
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
            os2::writePng(shot_, os2::movieHostCapture(1280, 720, elapsed));
            std::printf("opensims2: wrote loading screen %s (%.2f s into the load, progress %.0f%%)\n",
                        shot_.string().c_str(), elapsed, os2::loading::shown() * 100);
            std::fflush(stdout);
            shot_.clear();
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - lastDrawn_ < std::chrono::milliseconds(33))
            return;
        lastDrawn_ = now;
        SDL_PumpEvents();
        SDL_FlushEvents(SDL_EVENT_KEY_DOWN, SDL_EVENT_MOUSE_REMOVED); // the scene's input waits for the scene
        if (const os2::RgbaImage* picture = os2::movieHostFrame())
            renderer_.drawMovie(*picture);
    }
    // The load is done: the rest plays out quickly over the scene's first frames.
    void finish() {
        if (!active_)
            return;
        active_ = false;
        os2::loading::end();
        os2::finishLoadingScreen();
    }

private:
    os2::Renderer& renderer_;
    std::chrono::steady_clock::time_point start_, lastDrawn_{};
    std::filesystem::path shot_;
    float shotAt_ = 0.5f;
    bool active_ = false;
};

// Where a scene goes when it ends: the next scene (none: quit), and how many
// scripted steps it used (the next scene gets the rest).
struct SceneExit {
    std::optional<os2::Scene> next;
    size_t stepsUsed = 0;
    int code = 0;
};

// --cas: Create-a-Sim (app/cas_mode.hpp), as the Mac host's prepareCas/finishCas.
// Without a neighborhood families are written under --cas-out (default:
// cas-families next to the exe, so the game's own folders are left alone).
SceneExit runCas(App& app, const os2::Scene& scene) {
    std::vector<std::string> args = app.args;
    auto argument = [&](std::string_view flag) { return os2::argValue(args, std::string(flag)); };
    const std::filesystem::path& res = app.res;
    const std::optional<std::filesystem::path>& shot = app.shot;
    if (!argument("--cas-out")) {
        args.push_back("--cas-out");
        args.push_back((app.exe / "cas-families").string());
    }
    uint32_t w = 1280, h = 720;
    if (auto size = argument("--cas-size"))
        std::sscanf(size->c_str(), "%ux%u", &w, &h);
    os2::Renderer renderer(app.exe / "shaders", int(w), int(h), app.window);
    std::printf("opensims2: GPU %s\n", renderer.backend().c_str());
    SceneLoading loading(app, renderer, scene, true);
    os2::ResourceIndex& index = app.resources();
    loading.step(0.3f);
    const os2::LightingConfig lighting = os2::loadLightingConfig(res / "Lights" / "Lighting.txt");
    if (const auto* state = lighting.state(argument("--lighting").value_or("CAS"))) {
        renderer.setLighting(*state);
        if (!argument("--lighting")) {
            // The studio lamps of GlobalLots/CAS!.package replace these once it loads; without
            // the studio its boom light stands in from the camera's side, above (as the Mac host).
            auto set = os2::Renderer::outdoorLights(*state, true);
            set.lights.push_back({simd_normalize(simd_make_float3(0.35f, 1.0f, 0.9f)), simd_make_float3(1.0f, 0.95f, 0.85f) * 1.25f * 0.6f});
            renderer.setLightSets({set, set});
        }
    }
    os2::GameAudio gameAudio(res, {});
    applyAudioSettings(gameAudio, app.settings);
    os2::CasMode cas(renderer, index, app.gameDir, args);
    loading.step(0.9f);
    bool running = true;
    SceneExit exit;
    // Opened from a neighborhood: Accept saves the family there and goes back
    // with it selected; Exit goes back without one.
    auto back = [&] {
        running = false;
        if (scene.returnHood.empty())
            return;
        os2::Scene next;
        next.kind = os2::Scene::Kind::Neighborhood;
        next.hood = scene.returnHood;
        exit.next = next;
    };
    cas.playSound = [&](const std::string& name) { gameAudio.playUi(name); };
    cas.onExit = back;
    cas.onFamilySaved = [&](uint32_t family) {
        std::printf("opensims2: cas: family %u saved\n", family);
        if (!scene.returnHood.empty()) {
            app.createdFamily = family;
            back();
        }
    };
    std::unique_ptr<os2::LotCamera> camera = cas.makeCamera();
    cas.setCamera(camera.get());
    const simd_float4 background = simd_make_float4(0.33f, 0.36f, 0.45f, 1.f);
    renderer.setClearColor(background);
    cas.setCanvas(int(w), int(h));
    camera->setViewport(float(w), float(h));
    auto showUi = [&] {
        os2::UiScreen screen;
        if (!cas.layout(int(w), int(h), screen))
            return false;
        renderer.setUiImage(os2::composeUi(screen, {0, 0, 0, 0}), 0, 0, float(w), float(h));
        return true;
    };
    auto viewProj = [&] { return simd_mul(camera->projection(float(w) / float(h)), camera->view()); };
    SDL_SetWindowTitle(renderer.window(), cas.title().c_str());
    cas.runScripted(args);

    if (shot) {
        if (exit.next)
            return exit; // the steps went back to the neighborhood: the next scene takes the shot
        camera->snap();
        cas.update(0.5);
        camera->snap();
        std::ofstream log(shot->parent_path() / "frame_cas.txt");
        for (const auto& line : cas.report()) {
            std::printf("opensims2: %s\n", line.c_str());
            log << line << "\n";
        }
        // The scene alone through the offscreen path portraits use, then the window with the UI.
        const os2::RgbaImage alone = renderer.capture(w, h, os2::FrameView{camera->view(), camera->projection(float(w) / float(h))}, background);
        log << "offscreen " << alone.width << "x" << alone.height << "\n";
        showUi();
        renderer.setEye(camera->eye());
        if (!renderer.capture(*shot, viewProj()))
            exit.code = 1;
        else
            std::printf("opensims2: wrote %s\n", shot->string().c_str());
        return exit;
    }

    SDL_StartTextInput(renderer.window());
    bool dragging = false;
    const auto start = std::chrono::steady_clock::now();
    auto last = start;
    loading.finish();
    setGameCursor(app.res, os2::GameCursor::Arrow);
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            const bool control = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
            switch (event.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.scancode == SDL_SCANCODE_ESCAPE)
                    running = false;
                else
                    cas.keyDown(macKeyCode(event.key.scancode), "", control);
                break;
            case SDL_EVENT_KEY_UP: cas.keyUp(macKeyCode(event.key.scancode)); break;
            case SDL_EVENT_TEXT_INPUT: cas.keyDown(0xFFFF, event.text.text, control); break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (event.button.button == SDL_BUTTON_LEFT) {
                    dragging = true;
                    cas.mouseDown(simd_make_float2(event.button.x, event.button.y));
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (dragging)
                    cas.mouseDragged(simd_make_float2(event.motion.x, event.motion.y));
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.button == SDL_BUTTON_LEFT) {
                    dragging = false;
                    cas.mouseUp(simd_make_float2(event.button.x, event.button.y));
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL: camera->wheel(event.wheel.y); break;
            default: break;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        camera->update(std::chrono::duration<double>(now - last).count());
        last = now;
        cas.update(std::chrono::duration<double>(now - start).count());
        showUi();
        renderer.setEye(camera->eye());
        if (!drawFrame(renderer, viewProj()))
            running = false;
    }
    SDL_StopTextInput(renderer.window());
    return exit;
}

// The puck's F-key accelerators and the camera keys of the neighborhood view.
constexpr unsigned short kMacF1 = 122, kMacF2 = 120, kMacF3 = 99, kMacF4 = 118, kMacF5 = 96;

unsigned short macFunctionKey(SDL_Scancode code) {
    switch (code) {
    case SDL_SCANCODE_F1: return kMacF1;
    case SDL_SCANCODE_F2: return kMacF2;
    case SDL_SCANCODE_F3: return kMacF3;
    case SDL_SCANCODE_F4: return kMacF4;
    case SDL_SCANCODE_F5: return kMacF5;
    default: return 0;
    }
}

// --hood <N001|folder>: the neighborhood view (app/hood_mode.hpp), as the Mac
// host's prepareHood/finishHood. With --screenshot the steps (--click x,y) run
// first; a step that leaves the scene hands the shot to the next one.
SceneExit runHood(App& app, const os2::Scene& scene) {
    const uint32_t w = 1280, h = 720;
    os2::Renderer renderer(app.exe / "shaders", int(w), int(h), app.window);
    std::printf("opensims2: GPU %s\n", renderer.backend().c_str());
    SceneLoading loading(app, renderer, scene, false);
    os2::ResourceIndex& index = app.resources();
    const std::filesystem::path folder = app.hoodFolder(scene.hood);
    loading.step(0.3f);
    os2::HoodMode hood(renderer, index, app.gameDir, folder.string(), app.args);
    for (const auto& line : hood.view().report())
        std::printf("opensims2: %s\n", line.c_str());
    loading.step(0.9f);

    SceneExit exit;
    bool running = true;
    std::unique_ptr<os2::LotCamera> camera = hood.makeCamera();
    auto leave = [&](const os2::Scene& next) {
        exit.next = next;
        running = false;
    };
    // Entering a lot, Create-a-Sim and the main menu switch scenes in place (app/game_flow.hpp).
    hood.onEnterLot = [&](const os2::HoodLot& lot) {
        os2::Scene next;
        next.kind = os2::Scene::Kind::Lot;
        next.hood = scene.hood;
        next.lot = lot.id;
        leave(next);
    };
    hood.onCreateFamily = [&](const std::filesystem::path& hoodFolder) {
        os2::Scene next;
        next.kind = os2::Scene::Kind::Cas;
        next.hood = hoodFolder.string();
        next.returnHood = scene.hood;
        leave(next);
    };
    // A lot placed, moved or binned: the view built again, seen from where the camera was.
    hood.onReload = [&] {
        app.hoodPose = camera->pose();
        leave(scene);
    };
    hood.onQuit = [&] { running = false; };
    hood.onMainMenu = [&] {
        os2::Scene next;
        next.kind = os2::Scene::Kind::MainMenu;
        next.fromNeighborhood = true;
        next.returnHood = scene.hood;
        leave(next);
    };
    if (auto z = app.argument("--zoom"))
        camera->zoomBy(std::stoi(*z));
    if (auto o = app.argument("--orientation"))
        camera->rotateBy(std::stoi(*o));
    camera->snap();
    if (app.hoodPose) {
        camera->setPose(*app.hoodPose);
        app.hoodPose.reset();
    }

    os2::GameAudio gameAudio(app.res, {});
    applyAudioSettings(gameAudio, app.settings);
    hood.ui().playSound = [&](const std::string& name) { gameAudio.playUi(name); };
    // Head shots for the characters whose files have none (app/head_shot.hpp).
    std::vector<os2::SimPart> parts;
    hood.ui().renderHead = [&](const std::filesystem::path& file) {
        if (parts.empty())
            parts = os2::loadSimParts(index);
        return os2::renderHeadShot(renderer, index, parts, file, 128, app.res);
    };
    // The Decorations catalog's pictures: effects have UI images, the rest are rendered.
    hood.ui().renderDecorThumb = [&](const os2::hood::DecorProduct& p) -> std::optional<os2::RgbaImage> {
        const int model = hood.view().decorationModel(p);
        if (model < 0)
            return std::nullopt;
        return os2::renderModelThumbnail(renderer, model, hood.view().decorationBounds(p), 94);
    };
    hood.ui().setForceRenderHeads(app.argument("--hood-render-heads").has_value());
    // The Options panel's option windows (audio, game, graphics, camera, credits).
    hood.ui().options().setSettings(app.settings);
    hood.ui().options().onChanged = [&](const os2::GameSettings& s) {
        app.settings = s;
        s.save(*app.user);
        app.settings.applyGraphics();
        applyAudioSettings(gameAudio, app.settings);
        hood.view().refreshLightMap();
    };
    hood.ui().options().playSound = [&](const std::string& name) { gameAudio.playUi(name); };
    renderer.setClearColor(simd_make_float4(0.42f, 0.55f, 0.72f, 1.f));
    SDL_SetWindowTitle(app.window, ("opensims2 - " + hood.view().displayName()).c_str());
    if (app.createdFamily) {
        hood.familyCreated(*app.createdFamily);
        app.createdFamily.reset();
    }
    camera->setViewport(float(w), float(h));
    auto viewProj = [&] { return simd_mul(camera->projection(float(w) / float(h)), camera->view()); };
    int uiWindows = 0, uiDrawn = 0;
    auto showUi = [&] {
        const os2::UiScreen screen = hood.ui().overlay(int(w), int(h));
        uiWindows = int(screen.windows), uiDrawn = int(screen.drawn);
        renderer.setUiImage(os2::composeUi(screen, {0, 0, 0, 0}), 0, 0, float(w), float(h));
    };
    std::fflush(stdout);

    if (app.shot) {
        hood.scriptedView(*camera, float(w), float(h));
        camera->snap();
        // Where the lots are on the 1280x720 canvas, for click steps.
        const simd_float4x4 m = viewProj();
        for (const auto& lot : hood.view().lots()) {
            const simd_float3 c = hood.view().lotCentre(lot);
            const simd_float4 q = simd_mul(m, simd_make_float4(c, 1));
            const float x = (q.x / q.w + 1) / 2 * float(w), y = (1 - q.y / q.w) / 2 * float(h);
            if (q.w > 0 && x >= 0 && x < float(w) && y >= 0 && y < float(h))
                std::printf("opensims2: lot %u \"%s\" at (%.0f, %.0f)\n", lot.id, lot.name.c_str(), x, y);
        }
        const auto steps = os2::flowSteps(app.args);
        for (size_t k = 0; k < steps.size() && running; ++k) {
            exit.stepsUsed = k + 1;
            if (steps[k].kind == os2::FlowStep::Kind::Click) {
                hood.mouseDown(*camera, simd_make_float2(steps[k].x, steps[k].y), float(w), float(h));
                hood.mouseUp(*camera, simd_make_float2(steps[k].x, steps[k].y), float(w), float(h));
            }
        }
        if (exit.next)
            return exit;
        showUi();
        std::printf("opensims2: neighborhood UI %d windows, %d drawn\n", uiWindows, uiDrawn);
        renderer.setEye(camera->eye());
        if (!renderer.capture(*app.shot, viewProj()))
            exit.code = 1;
        else
            std::printf("opensims2: wrote %s\n", app.shot->string().c_str());
        return exit;
    }

    SDL_StartTextInput(app.window);
    bool dragging = false;
    bool keys[4] = {}; // left, right, back, forward
    auto last = std::chrono::steady_clock::now();
    loading.finish();
    setGameCursor(app.res, os2::GameCursor::Arrow);
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            const bool control = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
            const bool typing = hood.ui().bins().textEntry() || hood.ui().storyEditing();
            switch (event.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_TEXT_INPUT:
                if (typing)
                    hood.typeKey(0xFFFF, event.text.text);
                break;
            case SDL_EVENT_KEY_DOWN: {
                const SDL_Scancode code = event.key.scancode;
                if (typing) { // the place-lot dialog and the story take the keys
                    if (macKeyCode(code) != 0xFFFF)
                        hood.typeKey(macKeyCode(code), "");
                    break;
                }
                if (const unsigned short f = macFunctionKey(code)) {
                    if (f == kMacF4)
                        hood.ui().toggleStory();
                    else
                        hood.ui().togglePanel(f == kMacF1   ? os2::HoodBins::Panel::Families
                                              : f == kMacF2 ? os2::HoodBins::Panel::Lots
                                              : f == kMacF3 ? os2::HoodBins::Panel::Decorations
                                                            : os2::HoodBins::Panel::Options);
                    break;
                }
                if (control && (code == SDL_SCANCODE_Z || code == SDL_SCANCODE_Y) &&
                    hood.ui().bins().panel() == os2::HoodBins::Panel::Decorations) {
                    hood.undoDecoration(code == SDL_SCANCODE_Y);
                    break;
                }
                if (hood.placingLot() && (code == SDL_SCANCODE_COMMA || code == SDL_SCANCODE_PERIOD)) {
                    hood.turnLot(code == SDL_SCANCODE_PERIOD ? 1 : -1);
                    break;
                }
                switch (code) {
                case SDL_SCANCODE_ESCAPE: hood.escape(); break;
                case SDL_SCANCODE_Z: case SDL_SCANCODE_KP_PLUS: camera->zoomBy(1); break;
                case SDL_SCANCODE_X: case SDL_SCANCODE_KP_MINUS: camera->zoomBy(-1); break;
                case SDL_SCANCODE_COMMA: case SDL_SCANCODE_Q: camera->rotateBy(-1); break;
                case SDL_SCANCODE_PERIOD: case SDL_SCANCODE_E: camera->rotateBy(1); break;
                case SDL_SCANCODE_A: case SDL_SCANCODE_LEFT: keys[0] = true; break;
                case SDL_SCANCODE_D: case SDL_SCANCODE_RIGHT: keys[1] = true; break;
                case SDL_SCANCODE_S: case SDL_SCANCODE_DOWN: keys[2] = true; break;
                case SDL_SCANCODE_W: case SDL_SCANCODE_UP: keys[3] = true; break;
                default: break;
                }
                break;
            }
            case SDL_EVENT_KEY_UP:
                switch (event.key.scancode) {
                case SDL_SCANCODE_A: case SDL_SCANCODE_LEFT: keys[0] = false; break;
                case SDL_SCANCODE_D: case SDL_SCANCODE_RIGHT: keys[1] = false; break;
                case SDL_SCANCODE_S: case SDL_SCANCODE_DOWN: keys[2] = false; break;
                case SDL_SCANCODE_W: case SDL_SCANCODE_UP: keys[3] = false; break;
                default: break;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (event.button.button == SDL_BUTTON_LEFT) {
                    dragging = true;
                    hood.mouseDown(*camera, simd_make_float2(event.button.x, event.button.y), float(w), float(h));
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (dragging)
                    hood.mouseDragged(*camera, simd_make_float2(event.motion.x, event.motion.y), float(w), float(h));
                else
                    hood.mouseMoved(*camera, simd_make_float2(event.motion.x, event.motion.y), float(w), float(h));
                if (event.motion.state & SDL_BUTTON_RMASK)
                    camera->rotateRequest(event.motion.xrel, event.motion.yrel);
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.button == SDL_BUTTON_LEFT) {
                    dragging = false;
                    hood.mouseUp(*camera, simd_make_float2(event.button.x, event.button.y), float(w), float(h));
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL: camera->wheel(event.wheel.y); break;
            default: break;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        const float right = float(keys[1]) - float(keys[0]), forward = float(keys[3]) - float(keys[2]);
        // The held keys as the camera's move request (0 = released), as app_view.mm.
        camera->setKeyMove(right, forward);
        camera->update(dt);
        showUi();
        renderer.setEye(camera->eye());
        if (!drawFrame(renderer, viewProj()))
            running = false;
    }
    SDL_StopTextInput(app.window);
    return exit;
}

// --menu (or no scene flag): the main menu, which chooses the neighborhood
// (app/main_menu.hpp), as the Mac host's prepareMainMenu/finishMainMenu.
SceneExit runMenu(App& app, const os2::Scene& scene) {
    const uint32_t w = 1280, h = 720;
    os2::Renderer renderer(app.exe / "shaders", int(w), int(h), app.window);
    std::printf("opensims2: GPU %s\n", renderer.backend().c_str());
    // The start-up movies before the first main menu (EA logo, then the intro; skippable).
    if (!app.introPlayed && !app.shot) {
        app.introPlayed = true;
        os2::playMovies(os2::introMovies());
        if (!playMoviesInWindow(renderer))
            return {};
    }
    SceneLoading loading(app, renderer, scene, false);
    os2::ResourceIndex& index = app.resources();
    loading.step(0.6f);
    std::vector<std::filesystem::path> dirs;
    for (const auto& prefix : app.user->shipped())
        if (prefix != "Tutorial")
            dirs.push_back(app.user->neighborhoodForReading(prefix));
    os2::MainMenu menu(index, app.res, os2::readMainMenuHoods(dirs), scene.fromNeighborhood);
    menu.setEpisodes(os2::loadTutorialEpisodes(
        app.res, app.user->hasCopy("Tutorial") ? app.user->neighborhoodsDir() / "Tutorial" : std::filesystem::path()));
    os2::GameAudio gameAudio(app.res, {});
    applyAudioSettings(gameAudio, app.settings);
    menu.setSoundPlayer([&](const std::string& name) { gameAudio.playUi(name); });
    renderer.setClearColor(simd_make_float4(0, 0, 0, 1));
    SDL_SetWindowTitle(app.window, "opensims2");
    menu.setCanvas(int(w), int(h));
    for (size_t i = 0; i < dirs.size(); ++i) {
        const auto r = menu.thumbnailRect(int(i));
        std::printf("opensims2: main menu: %s at (%.0f, %.0f)-(%.0f, %.0f)\n", dirs[i].filename().string().c_str(), r[0], r[1],
                    r[2], r[3]);
    }
    if (!menu.ok())
        std::fprintf(stderr, "opensims2: main menu: scripts missing\n");

    SceneExit exit;
    bool running = true;
    auto press = [&](simd_float2 p) {
        menu.setCanvas(int(w), int(h));
        os2::Scene next;
        switch (menu.press(p)) {
        case os2::MainMenu::Action::Tutorial:
            // PlayEpisode: a fresh Tutorial neighborhood (ResetTutorialNeighborhood) and straight into the episode's lot.
            if (const os2::TutorialEpisode* ep = menu.chosenEpisode()) {
                std::printf("opensims2: main menu: tutorial %d \"%s\": %s lot %u\n", ep->number, ep->title.c_str(),
                            ep->neighborhood.c_str(), ep->lot);
                try {
                    os2::resetTutorialNeighborhood(app.user->shippedNeighborhoodsDir() / ep->neighborhood,
                                                   app.user->neighborhoodsDir() / ep->neighborhood);
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "opensims2: tutorial: %s\n", e.what());
                }
                next.kind = os2::Scene::Kind::Lot;
                next.hood = ep->neighborhood;
                next.lot = ep->lot;
                exit.next = next, running = false;
            }
            break;
        case os2::MainMenu::Action::Play:
            std::printf("opensims2: main menu: play %s\n", menu.chosen().c_str());
            next.kind = os2::Scene::Kind::Neighborhood;
            next.hood = menu.chosen();
            exit.next = next, running = false;
            break;
        case os2::MainMenu::Action::Close:
            next.kind = os2::Scene::Kind::Neighborhood;
            next.hood = scene.returnHood;
            exit.next = next, running = false;
            break;
        case os2::MainMenu::Action::Quit:
            std::printf("opensims2: main menu: quit\n");
            running = false;
            break;
        default: break;
        }
        std::fflush(stdout);
    };
    auto showUi = [&] {
        renderer.setUiImage(os2::composeUi(menu.screen(int(w), int(h)), {0, 0, 0, 0}), 0, 0, float(w), float(h));
    };
    // The menu draws only its UI: the 3D pass sees nothing.
    simd_float4x4 viewProj = matrix_identity_float4x4;

    if (app.shot) {
        if (auto sel = app.argument("--menu-select"))
            menu.preview(std::stoi(*sel));
        if (auto ep = app.argument("--menu-learn"))
            menu.openLearn(std::stoi(*ep));
        const auto steps = os2::flowSteps(app.args);
        for (size_t k = 0; k < steps.size() && running; ++k) {
            exit.stepsUsed = k + 1;
            if (steps[k].kind == os2::FlowStep::Kind::Click)
                press(simd_make_float2(steps[k].x, steps[k].y));
        }
        if (exit.next || !running)
            return exit;
        showUi();
        if (!renderer.capture(*app.shot, viewProj))
            exit.code = 1;
        else
            std::printf("opensims2: wrote %s\n", app.shot->string().c_str());
        return exit;
    }

    loading.finish();
    setGameCursor(app.res, os2::GameCursor::Arrow);
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_MOUSE_MOTION: menu.hover(simd_make_float2(event.motion.x, event.motion.y)); break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (event.button.button == SDL_BUTTON_LEFT)
                    press(simd_make_float2(event.button.x, event.button.y));
                break;
            default: break;
            }
        }
        showUi();
        if (!drawFrame(renderer, viewProj))
            running = false;
    }
    return exit;
}

} // namespace

// A lot: its terrain, walls, objects and Sims driven by the VM, with the live
// UI and the Buy and Build modes. `scene` names its neighborhood when the game
// flow opened it (Esc goes back there).
SceneExit runLot(App& app, const std::filesystem::path& lotFile, const os2::Scene& scene) {
    try {
        const std::filesystem::path& res = app.res;
        const std::filesystem::path shot = app.shot.value_or("frame.png");
        const bool exitAfterShot = app.shot.has_value();
        os2::ResourceIndex& index = app.resources();
        os2::DbpfPackage package(lotFile);
        os2::Lot lot = os2::Lot::load(package);
        os2::vm::dropLeftoverCObjects(lot, package, index);
        os2::PatternCatalog patterns(index, res);
        std::printf("opensims2: lot %ux%u, building the house\n", lot.width, lot.height);
        std::fflush(stdout);
        const char* vmOnly = std::getenv("OS2_VM_ONLY");
        if (vmOnly && vmOnly[0] == '1') {
            std::fprintf(stderr, "opensims2: VM only, starting SimAntics\n");
            std::fflush(stderr);
            os2::LotSimulation simulation(index, lot, package, lotFile);
            std::printf("opensims2: %s\n", simulation.summary().c_str());
            std::fflush(stdout);
            int people = 0;
            for (const auto& [id, object] : simulation.machine().objects()) {
                if (!object.person || !object.position.inWorld())
                    continue;
                ++people;
                if (people <= 12)
                    std::fprintf(stderr, "opensims2: vm sim %d '%s' at %.2f %.2f\n", id,
                                 object.definition ? object.definition->fileName.c_str() : "?", object.position.x,
                                 object.position.y);
            }
            std::fprintf(stderr, "opensims2: sims in world %d\n", people);
            std::fflush(stderr);
            for (int i = 0; i < 30; ++i)
                simulation.update(i * 0.042);
            std::printf("opensims2: ticks %llu\n", (unsigned long long)simulation.ticks());
            std::fflush(stdout);
            return {};
        }
        os2::Renderer renderer(app.exe / "shaders", 1280, 720, app.window);
        SDL_SetWindowTitle(app.window, ("opensims2 - " + lotFile.stem().string()).c_str());
        std::printf("opensims2: GPU %s\n", renderer.backend().c_str());
        SceneLoading loading(app, renderer, scene, false);
        // The live camera of the game's camera configuration, held to the lot
        // and on the selected Sim's floor (app/camera_follow.hpp); the lot is
        // entered with the walls cut away and the levels above it hidden.
        const auto cameraDefs = os2::loadCameraConfig(res / "UserData" / "Cameras" / "cameras.txt");
        const os2::CameraDef* liveCamera = os2::findCamera(cameraDefs, "Live");
        if (!liveCamera)
            throw std::runtime_error("no \"Live\" camera in cameras.txt");
        os2::LotCamera camera(*liveCamera);
        camera.setViewport(1280, 720);
        camera.snap();
        os2::LotCameraFollow follow(camera, lot);
        os2::CameraMouseInput cameraMouse;
        os2::WallView wallView;
        wallView.study(lot);
        auto lookOf = [&] {
            const simd_float3 d = camera.pivot() - camera.eye();
            return std::array<float, 2>{d.x, d.y};
        };
        os2::LotViewOptions view = wallView.options(lookOf(), wallView.shownLevel(follow.level()));
        os2::LotLightMapLayout layout;
        os2::LotModelCache cache;
        const os2::ObjectModel house = os2::buildLotModel(lot, index, patterns, view, &layout, &cache);
        for (const auto& warning : house.warnings)
            std::fprintf(stderr, "opensims2: %s\n", warning.c_str());

        std::printf("opensims2: house has %zu parts, uploading\n", house.parts.size());
        std::fflush(stdout);
        loading.step(0.2f);
        renderer.setGroundClip(os2::lotSkirtBottom(lot));
        const int houseModel = renderer.addObject(house, index);
        renderer.addInstance(houseModel, matrix_identity_float4x4);

        // Fence rails and posts: a model per CRES, an instance per piece; called
        // again after Build mode changed the fences (extra instances hidden).
        std::map<std::string, int> fenceModels;
        std::vector<int> fenceInstances;
        auto placeFences = [&](int level) {
            size_t used = 0;
            for (const auto& piece : os2::lotFencePieces(lot, patterns)) {
                if (!os2::WallView::visible(lot, level, piece.transform[12], piece.transform[13], piece.transform[14]))
                    continue;
                auto it = fenceModels.find(piece.model);
                if (it == fenceModels.end()) {
                    int id = -1;
                    try {
                        id = renderer.addObject(os2::loadObjectModel(index, piece.model), index);
                    } catch (const std::exception& e) {
                        std::fprintf(stderr, "opensims2: fence %s: %s\n", piece.model.c_str(), e.what());
                    }
                    it = fenceModels.emplace(piece.model, id).first;
                }
                if (it->second < 0)
                    continue;
                simd_float4x4 at;
                std::memcpy(&at, piece.transform.data(), sizeof at);
                if (used < fenceInstances.size()) {
                    renderer.setInstanceModel(fenceInstances[used], it->second);
                    renderer.setInstanceTransform(fenceInstances[used], at);
                } else {
                    fenceInstances.push_back(renderer.addInstance(it->second, at));
                }
                ++used;
            }
            simd_float4x4 away = matrix_identity_float4x4;
            away.columns[3] = simd_make_float4(0, 0, -1000, 1);
            for (size_t i = used; i < fenceInstances.size(); ++i)
                renderer.setInstanceTransform(fenceInstances[i], away);
            return used;
        };
        std::printf("opensims2: %zu fence pieces\n", placeFences(view.level));

        const os2::LightingConfig lighting = os2::loadLightingConfig(res / "Lights" / "Lighting.txt");
        // Redraws the lot after Build mode changed it: the model, the fences and the light map.
        int lotRebuilds = 0;
        std::unique_ptr<os2::LotLighting> lotLighting;
        std::function<os2::LotViewOptions()> wantedView; // set once the live UI and the pie menu are up
        auto rebuildLot = [&] {
            wallView.study(lot);
            if (wantedView)
                view = wantedView();
            cache.geometryChanged();
            layout = os2::LotLightMapLayout{};
            const os2::ObjectModel model = os2::buildLotModel(lot, index, patterns, view, &layout, &cache);
            renderer.replaceObject(houseModel, model, index);
            renderer.setGroundClip(os2::lotSkirtBottom(lot));
            placeFences(view.level);
            if (lotLighting)
                lotLighting->setLightMapLayout(layout);
            else if (const auto* state = lighting.state("day"))
                renderer.setLightMap(os2::computeLotLightMap(layout, *state, {}, os2::LightMapGlobals::from(lighting)).image);
            ++lotRebuilds;
        };
        if (const auto* state = lighting.state("day")) {
            const os2::RgbaImage lightMap =
                os2::computeLotLightMap(layout, *state, {}, os2::LightMapGlobals::from(lighting)).image;
            renderer.setLightMap(lightMap);
            renderer.setLighting(*state);
            std::printf("opensims2: light map %ux%u, lights %d objects / %d Sims\n", lightMap.width, lightMap.height,
                        renderer.objectLightCount(), renderer.simLightCount());
        }
        loading.step(0.3f);
        simd_float3 shadowTravel{0, 0, -1};
        simd_float3 shadowColor{0.55f, 0.55f, 0.62f};
        if (const auto* state = lighting.state("day")) {
            for (const auto& light : state->lights) {
                std::string name = light.name;
                for (char& c : name)
                    c = char(std::tolower(static_cast<unsigned char>(c)));
                if (name != "sun")
                    continue;
                const simd_float3 direction{light.direction[0], light.direction[1], light.direction[2]};
                if (simd_length(direction) > 1e-4f && direction.z > 0)
                    shadowTravel = -simd_normalize(direction);
            }
            const float strength = state->projectiveShadowStrength >= 0 ? state->projectiveShadowStrength : 0.73f;
            const float kept = std::max(0.f, 1.f - strength);
            shadowColor = simd_float3{kept, kept, std::min(1.f, kept + 0.15f)};
        }

        std::map<std::string, os2::Tgi> definitions;
        std::map<std::string, int> models;
        std::map<std::string, os2::Skeleton> skeletons;
        std::map<std::string, std::pair<simd_float3, simd_float3>> bounds;      // model space, by model key
        std::map<std::string, std::shared_ptr<const os2::PickMesh>> pickMeshes; // by model key
        struct PickBoxSpec {
            bool contained;
            uint32_t instance;
            std::string key;
            simd_float4x4 at;
        };
        std::vector<PickBoxSpec> pickBoxes;
        struct PlacedObject {
            std::string cres;
            std::map<std::string, std::string> materials;
            std::optional<os2::Xform> saved; // nothing: saved in a slot
            std::string definition;
            int instance = -1;
            std::vector<os2::Xform> pose;
        };
        std::map<int16_t, PlacedObject> placed;
        size_t objects = 0;
        for (const auto& object : lot.objects) {
            if (object.model.empty() || object.person || object.model == "nullModel" || object.model == "noModel" ||
                object.model == "null" || object.model == "placeholder")
                continue;
            const bool contained = object.position[0] == 0 && object.position[1] == 0;
            std::string key = cresOf(index, object, definitions);
            for (const auto& [subset, material] : object.materials)
                key += "|" + subset + "=" + material;
            if (!models.count(key)) {
                try {
                    const os2::ObjectModel model = os2::loadObjectModel(index, cresOf(index, object, definitions), object.materials);
                    skeletons[key] = model.skeleton;
                    models[key] = renderer.addObject(model, index);
                    bounds[key] = os2::modelBounds(model);
                    pickMeshes[key] = os2::makePickMesh(model, &index);
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "opensims2: %s: %s\n", object.model.c_str(), e.what());
                    models[key] = -1;
                }
            }
            int model = models[key];
            std::string drawnKey = key;
            std::vector<os2::Xform> savedPose;
            if (!object.nodes.empty() && skeletons.count(key)) {
                if (auto pose = os2::savedPose(skeletons[key], object.nodes)) {
                    const std::string posedKey = key + "@posed";
                    if (!models.count(posedKey)) {
                        try {
                            const os2::ObjectModel posed =
                                os2::posedModel(os2::loadObjectModel(index, cresOf(index, object, definitions), object.materials), *pose);
                            models[posedKey] = renderer.addObject(posed, index);
                            bounds[posedKey] = os2::modelBounds(posed);
                            pickMeshes[posedKey] = os2::makePickMesh(posed, &index);
                        } catch (...) {
                            models[posedKey] = -1;
                        }
                    }
                    if (models[posedKey] >= 0) {
                        model = models[posedKey];
                        drawnKey = posedKey;
                        savedPose = std::move(*pose);
                    }
                }
            }
            if (model < 0)
                continue;
            simd_float4x4 at = placementOf(object);
            if (contained)
                at.columns[3] = simd_make_float4(0, 0, -1000, 1);
            else
                ++objects;
            const int instance = renderer.addInstance(model, at);
            pickBoxes.push_back({contained, object.instance, drawnKey, at});
            // Stale cObjects share IDs with live ones: the first (or one in the
            // world) keeps the ID; app/vm_objects.cpp checks it against the VM.
            const int16_t id = int16_t(object.instance & 0xFFFF);
            if (!placed.count(id) || (!placed[id].saved && !contained)) {
                PlacedObject p{cresOf(index, object, definitions), object.materials, std::nullopt, object.definition,
                               instance, std::move(savedPose)};
                if (!contained)
                    p.saved = os2::Xform{os2::Quat{object.rotation[0], object.rotation[1], object.rotation[2], object.rotation[3]},
                                         {object.position[0], object.position[1], object.position[2]}};
                placed[id] = std::move(p);
            }
        }

        std::fprintf(stderr, "opensims2: placed %zu objects, starting SimAntics\n", objects);
        std::fflush(stderr);
        // Babies born in play and the NPCs the VM makes get character files
        // (vm/baby_character.hpp), kept with the session's until the game is saved.
        static std::map<int16_t, std::filesystem::path> bornCharacters;
        bornCharacters.clear();
        std::filesystem::path lotCharactersDir;
        try {
            lotCharactersDir = os2::vm::charactersDirOf(lotFile);
            os2::vm::installBabyCharacterWriter(index, lotCharactersDir, os2::sessionCharactersDir(), &bornCharacters);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: babies' characters: %s\n", e.what());
        }
        os2::LotSimulation simulation(index, lot, package, lotFile);
        std::printf("opensims2: %s\n", simulation.summary().c_str());
        loading.step(0.5f);
        const auto parts = os2::loadSimParts(index);
        struct DrawnSim {
            std::unique_ptr<os2::SimActor> actor;
            int model = -1;
            int16_t id = 0;
            int instance = -1;
            std::filesystem::path file; // the character file
        };
        std::vector<DrawnSim> sims;
        // A Sim's actor from its character file, standing at `at` facing
        // `heading`, its idle cued at `cueAt` (scene clock), driven by the VM.
        auto spawnSim = [&](const std::filesystem::path& file, int16_t id, simd_float3 at, float heading, double cueAt,
                            const std::string& label) {
            try {
                auto actor = std::make_unique<os2::SimActor>(os2::loadCharacterSim(index, parts, file));
                actor->position = {at.x, at.y, at.z};
                actor->heading = heading;
                const std::string idle = idleFor(actor->skin);
                actor->cue(cueAt, actor->animation(index, idle));
                actor->update(cueAt + 0.05);
                const int model = renderer.addObject(actor->skin.model, index, true);
                actor->model = model;
                const int instance = renderer.addInstance(model, matrix_identity_float4x4);
                renderer.setInstanceLightSet(instance, 1);
                renderer.updateObject(model, actor->posed);
                os2::DbpfPackage characterPackage(file);
                simulation.attach(*actor, id, os2::readCharacter(characterPackage, index), idle);
                std::printf("opensims2: Sim %s (%s) at (%.2f, %.2f, %.2f), %zu parts, idle %s\n", file.stem().string().c_str(),
                            label.c_str(), at.x, at.y, at.z, actor->skin.model.parts.size(), idle.c_str());
                sims.push_back({std::move(actor), model, id, instance, file});
                return true;
            } catch (const std::exception& e) {
                std::fprintf(stderr, "opensims2: Sim %s: %s\n", label.c_str(), e.what());
                return false;
            }
        };
        os2::LotObjects saved = os2::LotObjects::load(package);
        std::set<uint32_t> inWorld;
        for (const auto& state : saved.objects)
            if (state.person && state.x >= 0)
                inWorld.insert(uint32_t(uint16_t(state.id)));
        for (const auto& object : lot.objects) {
            if (!object.person || !inWorld.count(object.instance & 0xFFFF))
                continue;
            const std::string stem = object.definition.substr(0, object.definition.find(" - "));
            spawnSim(lotFile.parent_path().parent_path() / "Characters" / (stem + ".package"), int16_t(object.instance & 0xFFFF),
                     simd_make_float3(object.position[0], object.position[1], object.position[2]),
                     2 * std::atan2(object.rotation[2], object.rotation[3]), 0, object.definition);
        }
        // The Sims the first day's "reset lot" made (a family moving in).
        constexpr float kEighthTurn = 3.14159265f / 4;
        for (const auto& [id, file] : simulation.arrivedSims())
            if (const os2::vm::Object* o = simulation.machine().object(id))
                spawnSim(file, id,
                         simd_make_float3(o->position.x, o->position.y,
                                          simulation.floorHeight(o->position.level, o->position.x, o->position.y)),
                         float(o->data[os2::vm::objdata::Direction]) * kEighthTurn, 0, "arrived");
        if (sims.empty())
            std::printf("opensims2: no Sims on the lot\n");

        os2::VmObjectView objectView(renderer, index, simulation);
        // TV channels and arcade screens (app/tv_broadcast.hpp): a broadcast's material shows its movies.
        std::unique_ptr<os2::TvBroadcast> tv;
        try {
            tv = std::make_unique<os2::TvBroadcast>(renderer, res);
            objectView.adjustModel = [&](os2::ObjectModel& m, const std::map<std::string, std::string>& materials) {
                tv->adjust(m, materials);
            };
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: tv: %s\n", e.what());
        }
        simulation.seats.slot = [&](int16_t id, double now) { return objectView.slotPlacement(id, now); };
        simulation.objectBone = [&](int16_t id, uint32_t crc) { return objectView.boneByHash(id, crc); };
        // Look At's slot point: the slot's bone by name, else its offset
        // (LotSimulation::lookTarget).
        simulation.objectSlotPoint = [&](int16_t id, const std::array<float, 3>& offset,
                                         const std::string& bone) -> std::optional<os2::Vec3> {
            if (!bone.empty())
                if (const auto w = objectView.nodePlacement(id, bone))
                    return os2::Vec3{w->t[0], w->t[1], w->t[2]};
            if (const auto at = objectView.placement(id))
                return at->apply(os2::Vec3{offset[0], offset[1], offset[2]});
            return std::nullopt;
        };
        int following = 0;
        for (const auto& [id, p] : placed)
            following += objectView.addPlaced(id, p.definition, p.cres, p.instance, p.saved, p.materials, p.pose) ? 1 : 0;
        for (auto& sim : sims)
            objectView.addSim(sim.id, sim.actor.get());
        std::printf("opensims2: object view %zu placed, %d follow the VM\n", placed.size(), following);

        auto drawnSim = [&](int16_t id) -> DrawnSim* {
            for (auto& s : sims)
                if (s.id == id)
                    return &s;
            return nullptr;
        };
        // A Sim's body again from its character file (a new outfit, a new age).
        auto rebuildSim = [&](DrawnSim& s, uint32_t category) {
            os2::SimActor& actor = *s.actor;
            actor.skin = os2::loadCharacterSim(index, parts, s.file, category);
            actor.player.setSkeleton(&actor.skin.skeleton);
            const int model = renderer.addObject(actor.skin.model, index, true);
            renderer.retargetInstances(actor.model, model);
            actor.model = s.model = model;
        };
        // A Sim that changes clothes gets the outfit of its character file.
        simulation.onOutfitChange = [&](int16_t id, os2::SimActor&, uint32_t category) {
            DrawnSim* s = drawnSim(id);
            if (!s)
                return;
            try {
                rebuildSim(*s, category);
                std::printf("opensims2: Sim %d now wears outfit category 0x%03X\n", id, category);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "opensims2: Sim %d: outfit 0x%03X: %s\n", id, category, e.what());
            }
        };
        // The dialogs' $AspirationString and the newborns' names read Text/*.package (vm/dialog.hpp).
        static std::unique_ptr<os2::UiResources> vmTexts;
        os2::vm::textTables() = [&index, res](uint32_t group, uint32_t instance) -> const os2::StringTable* {
            try {
                if (!vmTexts)
                    vmTexts = std::make_unique<os2::UiResources>(index, res);
                return vmTexts->table(group, instance);
            } catch (const std::exception&) {
                return nullptr;
            }
        };
        // A Sim that grows up (the Age primitive) gets its character file grown
        // into the new age (cas::writeAgedCharacter), written beside the
        // session's other files, and is rebuilt from it.
        simulation.onAgeChange = [&](int16_t id, os2::SimActor&, uint32_t ageBit) {
            DrawnSim* s = drawnSim(id);
            if (!s)
                return;
            try {
                static std::unique_ptr<os2::cas::CasCatalog> catalog;
                static std::unique_ptr<os2::cas::FaceBaker> baker;
                if (!catalog) {
                    catalog = std::make_unique<os2::cas::CasCatalog>(os2::cas::CasCatalog::load(index));
                    baker = std::make_unique<os2::cas::FaceBaker>(index, *catalog);
                }
                // The save copies it to the neighborhood; an NPC made in play
                // stays in the session's NPCs folder (vm/baby_character.hpp).
                const std::filesystem::path session = os2::sessionCharactersDir();
                const std::filesystem::path dir = s->file.parent_path() == session / "NPCs" ? session / "NPCs" : session;
                const std::filesystem::path out = dir / (s->file.stem().string() + ".package");
                const std::filesystem::path tmp = dir / (s->file.stem().string() + ".new.package");
                os2::cas::writeAgedCharacter(index, *catalog, *baker, s->file, ageBit, tmp, uint32_t(id) * 7919u + ageBit);
                os2::replaceFile(tmp, out);
                s->file = out;
                rebuildSim(*s, 1);
                std::printf("opensims2: Sim %d grew into age 0x%02X (%s)\n", id, ageBit, out.string().c_str());
            } catch (const std::exception& e) {
                std::fprintf(stderr, "opensims2: Sim %d: age 0x%02X: %s\n", id, ageBit, e.what());
            }
        };
        // Objects the VM animates (doors, fridges, phones, ...) swap their static
        // model for a skinned one of the same CRES, posed by the animation (null: could not).
        std::map<int16_t, std::unique_ptr<os2::SimActor>> objectActors;
        auto objectActor = [&](int16_t id) -> os2::SimActor* {
            auto it = objectActors.find(id);
            if (it == objectActors.end()) {
                const auto p = placed.find(id);
                // Out of the world (saved at (0, 0) in no slot): an actor would be drawn at the lot's corner.
                if (p == placed.end() || !objectView.hasModel(id))
                    return nullptr;
                try {
                    const os2::ObjectModel model = os2::loadObjectModel(index, p->second.cres, objectView.materialsOf(id));
                    os2::SkinnedModel skin(model.skeleton);
                    skin.add(model);
                    auto actor = std::make_unique<os2::SimActor>(std::move(skin));
                    // From the pose the lot saved (a chair pushed in), and kept where its animations leave it.
                    if (p->second.pose.size() == actor->skin.skeleton.bones.size())
                        actor->player.setBasePose(p->second.pose);
                    actor->player.setHoldPose(true);
                    if (p->second.saved) {
                        const os2::Xform& at = *p->second.saved;
                        actor->position = at.t;
                        actor->heading = 2 * std::atan2(at.q.z, at.q.w);
                    }
                    actor->model = renderer.addObject(actor->skin.model, index, true);
                    renderer.addInstance(actor->model, matrix_identity_float4x4);
                    objectView.setActor(id, actor.get()); // hides the static instance
                    it = objectActors.emplace(id, std::move(actor)).first;
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "opensims2: object %d: %s\n", id, e.what());
                    it = objectActors.emplace(id, nullptr).first;
                }
            }
            return it->second.get();
        };
        // Once per name: the base game's trees ask for some the data lacks.
        std::set<std::string> missingAnimations;
        auto reportMissing = [&](int16_t id, const std::string& name, const std::exception& e) {
            if (missingAnimations.insert(name).second)
                std::fprintf(stderr, "opensims2: object %d: %s: %s\n", id, name.c_str(), e.what());
        };
        simulation.onObjectAnimation = [&](int16_t id, const std::string& name, double at) {
            os2::SimActor* actor = objectActor(id);
            if (!actor)
                return;
            try {
                // Mirrored as the VM says (a chair pulled out to the side the Sim gets in from).
                const os2::vm::Object* vo = simulation.machine().object(id);
                const bool mirror = vo && !vo->animation.overlays.empty() && vo->animation.overlays.back().name == name &&
                                    vo->animation.overlays.back().flipped;
                actor->cue(at, actor->animation(index, name), {.fade = 0.1f, .loop = false, .mirror = mirror});
            } catch (const std::exception& e) {
                reportMissing(id, name, e);
            }
        };
        // What the VM plays on an object, overlay by overlay (VM::objectPose):
        // each overlay its own layer, mirrored as the VM plays it, and taken
        // off when the VM takes it off, the object keeping that pose.
        simulation.onObjectOverlay = [&](int16_t id, const std::string& name, bool flipped, uint32_t overlay, bool loop, double at) {
            os2::SimActor* actor = objectActor(id);
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
        simulation.onObjectOverlayEnd = [&](int16_t id, uint32_t overlay, double at) {
            const auto it = objectActors.find(id);
            if (it != objectActors.end() && it->second)
                it->second->cueStop(at, 1 + int(overlay & 0x3FFFFFFF), 0);
        };
        simulation.actorOf = [&](int16_t id) -> const os2::SimActor* {
            const auto it = objectActors.find(id);
            return it != objectActors.end() ? it->second.get() : nullptr;
        };
        // An animated object whose material the VM changes (the plumbob's
        // colour) gets its skinned model again with the new materials.
        objectView.onActorMaterials = [&](int16_t id) {
            const auto it = objectActors.find(id);
            if (it == objectActors.end() || !it->second)
                return;
            os2::SimActor& actor = *it->second;
            try {
                const os2::ObjectModel model = os2::loadObjectModel(index, objectView.cresOf(id), objectView.materialsOf(id));
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
        // Doors open and close as the Sims walk through them (app/door_portals.hpp).
        os2::DoorPortals doors(
            [&](int16_t id, const std::string& name, double at) { simulation.onObjectAnimation(id, name, at); },
            [&](int16_t id) -> std::optional<os2::Xform> {
                if (objectView.hasModel(id))
                    return objectView.placement(id);
                const auto p = placed.find(id); // a door the VM does not have: where it was saved
                return p != placed.end() ? p->second.saved : std::nullopt;
            });
        for (const auto& [id, p] : placed)
            if (p.definition.rfind("Door", 0) == 0 && p.saved)
                doors.addDoor(id);
        loading.step(0.7f);
        try {
            lotLighting = std::make_unique<os2::LotLighting>(renderer, index, simulation, objectView, lighting,
                                                             os2::loadLightOverrides(res / "Lights" / "LotLights.txt"));
            for (auto& sim : sims)
                lotLighting->addSim(sim.id, sim.actor.get());
            lotLighting->setLightMapLayout(layout);
            if (const char* state = std::getenv("OS2_LIGHTING"))
                lotLighting->forceState(state);
            // Test aid (main.mm --lamps): lamps the VM has not switched shine at this level.
            if (const char* lamps = std::getenv("OS2_LAMPS"))
                lotLighting->setDefaultLampLevel(std::atoi(lamps));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: lighting: %s\n", e.what());
            lotLighting.reset();
        }

        os2::GameAudio gameAudio(res, {});
        applyAudioSettings(gameAudio, app.settings);
        os2::LotAudio lotAudio(gameAudio, index, lot, {});
        for (auto& sim : sims)
            lotAudio.attachSim(*sim.actor, sim.id, {});
        std::printf("opensims2: audio device %s\n", gameAudio.hasDevice() ? "on" : "off");
        std::fflush(stdout);

        // The lot given on its own (not from a neighborhood) is the port's test
        // bed: a movie frame and a particle effect are checked on it.
        const bool diagnostics = scene.hood.empty();
        std::string movieLabel;
        int movieW = 0, movieH = 0;
        long movieFrame = -1;
        try {
            const std::filesystem::path moviePath = diagnostics ? os2::findMovie(res, "ealogo_audio") : std::filesystem::path();
            if (!moviePath.empty()) {
                os2::Movie movie = os2::Movie::load(moviePath);
                os2::MovieDecoder decoder(movie);
                os2::RgbaImage picture;
                const double times[] = {0.4, 0.8, 1.2, std::max(0.1, movie.duration() * 0.5)};
                double shownAt = 0;
                for (double t : times) {
                    decoder.seek(t);
                    decoder.picture(picture);
                    uint64_t sum = 0;
                    const size_t n = picture.width * picture.height;
                    for (size_t i = 0; i + 2 < picture.pixels.size(); i += 4)
                        sum += picture.pixels[i] + picture.pixels[i + 1] + picture.pixels[i + 2];
                    shownAt = t;
                    if (n > 0 && double(sum) / double(n * 3) > 12)
                        break;
                }
                movieW = int(picture.width);
                movieH = int(picture.height);
                movieFrame = decoder.current();
                movieLabel = moviePath.filename().string();
                const auto movieShot = shot.parent_path() / "frame_movie.png";
                if (renderer.captureMovie(movieShot, picture))
                    std::printf("opensims2: movie %s %dx%d frame %ld at %.2fs -> %s\n", movieLabel.c_str(), movieW, movieH,
                                movieFrame, shownAt, movieShot.string().c_str());
                if (movie.hasSound()) {
                    auto pcm = std::make_shared<const os2::Pcm>(movie.decodeSound());
                    if (!pcm->samples.empty()) {
                        gameAudio.mixer().play(pcm, {});
                        std::printf("opensims2: movie sound %u Hz %u ch %.2fs\n", pcm->sampleRate, pcm->channels, pcm->seconds());
                    }
                }
            } else if (diagnostics) {
                std::fprintf(stderr, "opensims2: ealogo_audio.movie not found\n");
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: movie: %s\n", e.what());
        }

        std::unique_ptr<os2::LiveUi> liveUi;
        std::unique_ptr<os2::OptionsPanel> lotOptions; // the option windows of the live panel's options
        std::unique_ptr<os2::BuyMode> buy;
        std::unique_ptr<os2::BuildMode> build;
        std::unique_ptr<os2::PieMenuHost> pie;
        std::unique_ptr<os2::TutorialOverlay> tutorial; // over a lot of the Tutorial neighborhood
        int liveWindows = 0, liveDrawn = 0, buyWindows = 0, buyDrawn = 0, buildWindows = 0, buildDrawn = 0;
        auto showLiveUi = [&] {
            if (!liveUi || !liveUi->ok())
                return false;
            if (pie && pie->sim())
                liveUi->setSim(pie->sim());
            else if (!sims.empty())
                liveUi->setSim(sims.front().id);
            // The tutorial's arrow finds its control in the panels' layout, which
            // may lay them out again: before their screens are taken.
            os2::UiScreen tutorialScreen;
            if (tutorial) {
                tutorial->setCanvas(1280, 720);
                tutorialScreen = tutorial->screen(1280, 720);
            }
            os2::UiScreen liveScreen = liveUi->screen(1280, 720);
            liveWindows = int(liveScreen.windows);
            liveDrawn = int(liveScreen.drawn);
            // Under the puck, as Buy mode's panel (the window's tab rises behind the puck).
            os2::UiScreen screen;
            if (lotOptions && !liveUi->optionsOpen() && lotOptions->active())
                lotOptions->closeWindow(); // the options panel closed
            if (lotOptions && lotOptions->active())
                screen = lotOptions->screen(1280, 720);
            screen.width = liveScreen.width, screen.height = liveScreen.height;
            screen.quads.insert(screen.quads.end(), liveScreen.quads.begin(), liveScreen.quads.end());
            for (auto& text : liveScreen.textImages)
                screen.textImages.push_back(std::move(text));
            os2::UiScreen buyScreen;
            if (buy && buy->active()) {
                buy->update();
                buyScreen = buy->screen(1280, 720);
                buyWindows = int(buyScreen.windows);
                buyDrawn = int(buyScreen.drawn);
                screen.quads.insert(screen.quads.end(), buyScreen.quads.begin(), buyScreen.quads.end());
            }
            os2::UiScreen buildScreen;
            if (build && build->active()) {
                build->update();
                buildScreen = build->screen(1280, 720);
                buildWindows = int(buildScreen.windows);
                buildDrawn = int(buildScreen.drawn);
                screen.quads.insert(screen.quads.end(), buildScreen.quads.begin(), buildScreen.quads.end());
            }
            // The tutorial's window and arrow over the panels.
            screen.quads.insert(screen.quads.end(), tutorialScreen.quads.begin(), tutorialScreen.quads.end());
            // The pie menu and the dialogs over everything.
            os2::UiScreen pieScreen;
            if (pie) {
                pieScreen = pie->screen(1280, 720);
                screen.quads.insert(screen.quads.end(), pieScreen.quads.begin(), pieScreen.quads.end());
            }
            renderer.setUiImage(os2::composeUi(screen, {0, 0, 0, 0}), 0, 0, 1280, 720);
            return true;
        };
        loading.step(0.8f);
        try {
            liveUi = std::make_unique<os2::LiveUi>(simulation.machine(), index, res);
            if (showLiveUi())
                std::printf("opensims2: live UI %d windows, %d drawn\n", liveWindows, liveDrawn);
            else
                std::fprintf(stderr, "opensims2: live UI scripts missing\n");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: live UI: %s\n", e.what());
        }
        if (!liveUi || !liveUi->ok()) {
            os2::UiResources ui(index, res);
            float ux = 16, uy = 16, uw = 128, uh = 128;
            if (const os2::RgbaImage* image = firstUiImage(ui, index, ux, uy, uw, uh)) {
                const float scale = 1280.f / 1024.f;
                renderer.setUiImage(*image, ux * scale, uy * scale, std::max(uw * scale, 64.f), std::max(uh * scale, 64.f));
                std::printf("opensims2: UI image %ux%u\n", image->width, image->height);
            } else {
                std::fprintf(stderr, "opensims2: no UI image found\n");
            }
        }

        // The lot's particle effects (app/lot_effects.hpp): what the VM starts,
        // and --effect <name>[@<object id>[:<bone index>] | @x,y,z] (repeatable) by hand.
        std::unique_ptr<os2::LotEffects> lotEffects;
        std::string effectName;
        loading.step(0.85f);
        try {
            lotEffects = std::make_unique<os2::LotEffects>(renderer, index, res);
            for (size_t i = 1; i + 1 < app.args.size(); ++i) {
                if (app.args[i] != "--effect")
                    continue;
                std::string spec = app.args[i + 1], name = spec;
                int16_t object = 0;
                int slot = -1;
                os2::fx::V3 at{};
                if (auto a = spec.find('@'); a != std::string::npos) {
                    name = spec.substr(0, a);
                    const std::string where = spec.substr(a + 1);
                    if (std::sscanf(where.c_str(), "%f,%f,%f", &at[0], &at[1], &at[2]) != 3) {
                        object = int16_t(std::stoi(where));
                        if (auto c = where.find(':'); c != std::string::npos)
                            slot = std::stoi(where.substr(c + 1));
                    }
                }
                if (lotEffects->addManual(name, object, at, slot))
                    effectName = name;
                else
                    std::fprintf(stderr, "opensims2: --effect: no effect named %s\n", name.c_str());
            }
            // The diagnostic run starts one that shows particles over the first Sim.
            if (effectName.empty() && !sims.empty() && diagnostics) {
                const os2::fx::EffectsLibrary& library = lotEffects->library();
                auto trial = std::make_unique<os2::fx::EffectSystem>(library);
                auto* effectSystem = trial.get();
                const auto* effectLibrary = &library;
                os2::fx::Placement where;
                where.position = {sims.front().actor->position[0], sims.front().actor->position[1],
                                  sims.front().actor->position[2] + 1.2f};
                auto consider = [&](const std::string& name) {
                    if (!effectName.empty())
                        return;
                    const int handle = effectSystem->start(name, where);
                    if (handle < 0)
                        return;
                    effectSystem->update(0.4f);
                    std::vector<os2::fx::ParticleQuad> quads;
                    effectSystem->collect(quads);
                    if (!quads.empty())
                        effectName = name;
                    else
                        effectSystem->stop(handle, true);
                };
                for (const auto& [name, id] : effectLibrary->names) {
                    (void)id;
                    std::string lower = name;
                    for (char& c : lower)
                        c = char(std::tolower(static_cast<unsigned char>(c)));
                    if (lower.find("smoke") != std::string::npos || lower.find("steam") != std::string::npos ||
                        lower.find("heart") != std::string::npos || lower.find("fire") != std::string::npos)
                        consider(name);
                }
                if (effectName.empty()) {
                    int tried = 0;
                    for (const auto& [name, id] : effectLibrary->names) {
                        (void)id;
                        consider(name);
                        if (!effectName.empty() || ++tried >= 60)
                            break;
                    }
                }
                if (!effectName.empty()) {
                    std::vector<os2::fx::ParticleQuad> quads;
                    effectSystem->collect(quads);
                    std::printf("opensims2: effect %s, %zu particles\n", effectName.c_str(), quads.size());
                    lotEffects->addManual(effectName, 0, where.position);
                } else {
                    std::printf("opensims2: effects loaded, no particles from the first names\n");
                }
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: effects: %s\n", e.what());
            lotEffects.reset();
        }
        // The game's hidden PlumbBobs' glow goes with the app's plumbob.
        std::set<int16_t> hiddenPlumbobs;
        std::optional<os2::Xform> appPlumbobAt;
        if (lotEffects) {
            lotEffects->placementOverride = [&](int16_t id) -> std::optional<os2::Xform> {
                if (appPlumbobAt && hiddenPlumbobs.count(id))
                    return appPlumbobAt;
                return std::nullopt;
            };
        }

        if (!sims.empty())
            camera.setPivot(simd_make_float3(sims.front().actor->position[0], sims.front().actor->position[1],
                                             sims.front().actor->position[2]));
        else
            camera.setPivot(simd_make_float3(float(lot.width) / 2, float(lot.height) / 2,
                                             lot.elevation(follow.level(), int(lot.width / 2), int(lot.height / 2))));
        // --zoom <levels> / --orientation <steps> / --tilt <degrees> as on the
        // Mac; --pivot <x,y[,z]>: look at that point instead (tiles).
        if (auto z = app.argument("--zoom"))
            camera.zoomBy(std::stoi(*z));
        if (auto o = app.argument("--orientation"))
            camera.rotateBy(std::stoi(*o));
        if (auto t = app.argument("--tilt"))
            camera.tilt(std::stof(*t));
        if (auto p = app.argument("--pivot")) {
            simd_float3 pivot = camera.pivot();
            float px, py, pz;
            const int n = std::sscanf(p->c_str(), "%f,%f,%f", &px, &py, &pz);
            if (n >= 2)
                pivot.x = px, pivot.y = py;
            if (n == 3)
                pivot.z = pz;
            camera.setPivot(pivot);
        }
        camera.snap();
        // Page Up / Page Down and the puck's floor buttons: in Buy mode the
        // floor objects go on (the camera follows it), in Build mode the
        // floor built on, else the floor the camera shows (a level shown
        // with the one below, a foundation's top, is stepped over).
        auto changeFloor = [&](int delta) {
            if (buy && buy->active()) {
                if (buy->changeLevel(delta))
                    follow.setLevel(buy->level() - 1);
            } else if (build && build->active()) {
                if (build->level() - 1 + delta >= std::min(0, build->level() - 1) && follow.setLevel(build->level() - 1 + delta))
                    build->setLevel(follow.level() + 1);
            } else {
                const int now = follow.level(), shown = wallView.shownLevel(now);
                const int lowest = std::max(follow.lowestLevel(), std::min(0, now));
                int to = delta > 0 ? std::min(std::max(now + delta, shown + 1), std::max(wallView.topLevel(), now)) : now + delta;
                while (delta < 0 && to > lowest && wallView.shownLevel(to) == shown)
                    --to;
                follow.setLevel(std::max(to, std::min(lowest, now)));
            }
        };
        // The level the camera shows, whose walls follow the wall mode: Build
        // and Buy mode's level, else the camera's (the selected Sim's or the
        // one Page Up/Down chose).
        auto viewLevel = [&] {
            if (build && build->active())
                return wallView.shownLevel(build->level() - 1);
            if (buy && buy->active())
                return wallView.shownLevel(buy->level() - 1);
            return wallView.shownLevel(follow.level());
        };
        // Whether an object hangs on a wall the lot model cut down: a cut wall
        // of its level within 0.6 tiles, across its front (perpendicular to its +Y).
        auto onCutWall = [&](const os2::Xform& at) {
            const os2::Vec3 f = at.rotate({0, 1, 0});
            for (const auto& c : cache.cut) {
                const float floorZ = lot.elevationAt(int(c[4]), std::clamp(at.t[0], 0.0f, float(lot.width)),
                                                     std::clamp(at.t[1], 0.0f, float(lot.height)));
                if (at.t[2] < floorZ - 0.2f || at.t[2] > floorZ + 2.5f)
                    continue;
                float dx = c[2] - c[0], dy = c[3] - c[1];
                const float len = std::hypot(dx, dy);
                if (len < 1e-4f)
                    continue;
                dx /= len, dy /= len;
                if (std::abs(dx * f[0] + dy * f[1]) > 0.3f)
                    continue;
                const float s = std::clamp((at.t[0] - c[0]) * dx + (at.t[1] - c[1]) * dy, 0.0f, len);
                if (std::hypot(c[0] + dx * s - at.t[0], c[1] + dy * s - at.t[1]) <= 0.6f)
                    return true;
            }
            return false;
        };
        // Windows, doors, curtains and what hangs on walls: hidden with the
        // wall they are on when it is cut down; and nothing above the level shown.
        std::map<int16_t, bool> wallHung;
        for (const auto& b : pickBoxes)
            if (bounds[b.key].first.z > 0.5f)
                wallHung[int16_t(b.instance & 0xFFFF)] = true;
        objectView.visible = [&](int16_t id, const os2::Xform& at) {
            const int level = viewLevel();
            if (!os2::WallView::visible(lot, level, at.t[0], at.t[1], at.t[2], wallView.lowestCut(level)))
                return false;
            auto it = wallHung.find(id);
            if (it == wallHung.end()) {
                const os2::vm::Object* o = simulation.machine().object(id);
                std::string def = o && o->definition ? o->definition->fileName : "";
                std::transform(def.begin(), def.end(), def.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                const bool hung = def.starts_with("window") || def.starts_with("door") || def.starts_with("curtain") ||
                                  def.starts_with("blinds") || def.starts_with("drapes");
                it = wallHung.emplace(id, hung).first;
            }
            return !it->second || !onCutWall(at);
        };
        if (liveUi && liveUi->ok()) {
            liveUi->setCanvas(1280, 720);
            liveUi->playSound = [&](const std::string& name) { gameAudio.playUi(name); };
            liveUi->onZoom = [&](int steps) { camera.zoomBy(steps); };
            liveUi->onRotate = [&](int steps) { camera.rotateBy(steps); };
            liveUi->onCentreSim = [&](os2::vm::ObjectId id) { follow.centre(simulation.machine(), id); };
            liveUi->onFloor = changeFloor;
            liveUi->onWallMode = [&](int mode) {
                wallView.setMode(mode);
                liveUi->setWallMode(wallView.mode());
            };
            liveUi->setWallMode(wallView.mode());
            loading.step(0.9f);
            try {
                buy = std::make_unique<os2::BuyMode>(simulation, objectView, renderer, index, res);
                buy->setCanvas(1280, 720);
                buy->playSound = [&](const std::string& name) { gameAudio.playUi(name); };
                std::printf("opensims2: buy mode %s, %zu catalog entries\n", buy->ok() ? "ready" : "unavailable",
                            buy->catalog().entries().size());
            } catch (const std::exception& e) {
                std::fprintf(stderr, "opensims2: buy mode: %s\n", e.what());
                buy.reset();
            }
            try {
                build = std::make_unique<os2::BuildMode>(simulation, objectView, renderer, index, res, lot);
                build->setCanvas(1280, 720);
                build->onGeometryChanged = rebuildLot;
                build->playSound = [&](const std::string& name) { gameAudio.playUi(name); };
                std::printf("opensims2: build mode %s\n", build->ok() ? "ready" : "unavailable");
            } catch (const std::exception& e) {
                std::fprintf(stderr, "opensims2: build mode: %s\n", e.what());
                build.reset();
            }
            liveUi->onMode = [&](int mode) {
                if (mode == 1 && buy && buy->ok()) {
                    if (build)
                        build->exit();
                    buy->enter();
                    liveUi->setMode(1);
                } else if (mode == 2 && build && build->ok()) {
                    if (buy)
                        buy->exit();
                    build->enter();
                    liveUi->setMode(2);
                } else if (mode == 0) {
                    if (buy)
                        buy->exit();
                    if (build)
                        build->exit();
                    liveUi->setMode(0);
                }
            };
        }
        SceneExit exit;
        bool running = true;

        // Pie menus over the lot's VM (app/pie_menu.hpp), picked as the Mac host picks.
        loading.step(0.95f);
        try {
            pie = std::make_unique<os2::PieMenuHost>(simulation.machine(), res / "UI" / "Fonts");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: pie menu: %s\n", e.what());
        }
        // --vm-queue <object>:<entry>: test aid, the selected Sim's (--sim-id)
        // pie entry on the first object whose id or file name matches, queued.
        if (auto spec = app.argument("--vm-queue")) {
            os2::vm::VM& m = simulation.machine();
            auto lower = [](std::string s) {
                for (char& c : s)
                    c = char(std::tolower(static_cast<unsigned char>(c)));
                return s;
            };
            const auto colon = spec->find(':');
            const std::string what = lower(spec->substr(0, colon));
            const std::string entry = colon == std::string::npos ? "" : lower(spec->substr(colon + 1));
            os2::vm::ObjectId target = 0;
            for (const auto& [id, o] : m.objects())
                if (!target && !o.person && o.definition &&
                    (what == std::to_string(id) || lower(o.definition->fileName).find(what) != std::string::npos))
                    target = id;
            const os2::vm::ObjectId sim = os2::vm::findSim(m, app.argument("--sim-id").value_or(""));
            os2::vm::PieOptions opt;
            opt.language = os2::lang::EnglishUS;
            bool queued = false;
            std::string offered;
            if (target && sim)
                for (const auto& e : os2::vm::pieMenu(m, sim, target, opt)) {
                    offered += (offered.empty() ? "" : ", ") + e.name;
                    if (!queued && lower(e.name) == entry)
                        if (auto q = os2::vm::interactionFromPie(m, e)) {
                            os2::vm::addAction(m, *m.object(sim), *q);
                            queued = true;
                        }
                }
            std::printf("opensims2: --vm-queue %s: %s (object %d %s, Sim %d; entries: %s)\n", spec->c_str(),
                        queued ? "queued" : "no such entry", target,
                        target ? m.object(target)->definition->fileName.c_str() : "-", sim, offered.c_str());
        }
        // A lot of the Tutorial neighborhood: its tutorial controllers (created if
        // the lot has none) and the tutorial manager's UI over the live panel.
        size_t tutorialRequestsSeen = 0;
        if (scene.hood == "Tutorial" && liveUi && liveUi->ok()) {
            try {
                os2::vm::VM& vm = simulation.machine();
                const int created = os2::vm::ensureTutorialControllers(vm);
                // The tutorial lots are saved paused; the episode plays in time, so it
                // starts unpaused and the controller's first press of Buy mode pauses it.
                vm.globals[os2::vm::global::Paused] = 0;
                tutorialRequestsSeen = vm.engineRequests.size();
                tutorial = std::make_unique<os2::TutorialOverlay>(vm, index, res);
                tutorial->setCanvas(1280, 720);
                // The controls: Buy's and Build's panels while they are up, the live panel and the puck.
                tutorial->locate = [&](uint32_t window, uint32_t control) -> std::optional<std::array<float, 4>> {
                    // The main window's controls are the puck's (the panels reuse small ids).
                    if (window == os2::LiveUi::kMainWindow)
                        if (auto r = liveUi->controlRect(window, control))
                            return r;
                    if (buy && buy->active())
                        if (auto r = buy->controlRect(window, control))
                            return r;
                    if (build && build->active())
                        if (auto r = build->controlRect(window, control))
                            return r;
                    return liveUi->controlRect(window, control);
                };
                tutorial->pressControl = [&](uint32_t window, uint32_t control) {
                    const auto r = tutorial->locate(window, control);
                    if (!r)
                        return;
                    const simd_float2 p = simd_make_float2(((*r)[0] + (*r)[2]) / 2, ((*r)[1] + (*r)[3]) / 2);
                    tutorial->clicked(p); // as a click: the control's listeners, then the control
                    if (liveUi->overModeButton(p) && liveUi->press(p))
                        return;
                    if (buy && buy->active() && buy->press(p))
                        return;
                    if (build && build->active() && build->press(p))
                        return;
                    liveUi->press(p);
                };
                auto allows = [&](simd_float2 p) { return !tutorial || tutorial->allows(p); };
                liveUi->allowPress = allows;
                if (buy)
                    buy->allowPress = allows;
                if (build)
                    build->allowPress = allows;
                std::printf("opensims2: tutorial: %d controller(s) created\n", created);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "opensims2: tutorial: %s\n", e.what());
                tutorial.reset();
            }
        }
        // Whether a Sim is in the lot's world: it, or what holds it (a carried
        // baby, a passenger in a car), stands in it.
        auto simInTheWorld = [&](os2::vm::ObjectId id) {
            os2::vm::VM& m = simulation.machine();
            const os2::vm::Object* o = m.object(id);
            for (int depth = 0; o && depth < 8; ++depth) {
                if (o->position.inWorld())
                    return true;
                const int16_t c = o->data[os2::vm::objdata::ContainerId];
                if (!c || c == o->id)
                    return false;
                o = m.object(c);
            }
            return false;
        };
        // The small portrait: IMG 0x10, 0x08 or 0x02 by the Sim's age.
        auto simPortrait = [&](os2::vm::ObjectId id) -> std::optional<os2::RgbaImage> {
            for (const auto& s : sims) {
                if (s.id != id)
                    continue;
                try {
                    os2::DbpfPackage character(s.file);
                    for (const auto& e : character.entries())
                        if (e.tgi.type == os2::kTypeUiImage && e.tgi.instance < 0x100)
                            return os2::decodeUiImage(character.read(e));
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "opensims2: portrait: %s\n", e.what());
                }
            }
            return std::nullopt;
        };
        // The live panel's portraits (its family bar and the selected Sim's
        // head): the same IMG the pie menu's head uses.
        if (liveUi && liveUi->ok())
            for (const auto& s : sims)
                if (auto p = simPortrait(s.id))
                    liveUi->setPortrait(s.id, std::move(*p));
        // Whether the floor of `lotLevel` at `at` is drawn when the camera
        // shows `top`: not, in a level shown with the one below, the parts a
        // story over the ground, which the next floor up shows.
        auto drawnFloor = [&](int top, int lotLevel, simd_float3 at) {
            return lotLevel != top || wallView.lowestCut(top) >= top ||
                   !os2::storyOverGround(lot, lotLevel, int(std::floor(at.x)), int(std::floor(at.y)));
        };
        // Where the mouse ray meets a floor, from the level the camera shows
        // down: a walkable or pool tile (Go Here), or any tile with a floor
        // (the pick stops there). The point, the ray's parameter and the VM level.
        struct GroundHit {
            simd_float2 at;
            float t;
            int level;
        };
        auto groundUnderRay = [&](simd_float3 eye, simd_float3 dir, bool walkable) -> std::optional<GroundHit> {
            if (std::fabs(dir.z) < 1e-5f || !simulation.machine().world)
                return std::nullopt;
            const os2::vm::World& w = *simulation.machine().world;
            int top = viewLevel();
            if (top == os2::LotViewOptions::kAllLevels)
                top = layout.minLevel + layout.levels - 1;
            for (int lotLevel = top; lotLevel >= layout.minLevel; --lotLevel) {
                const int level = lotLevel + 1;
                float z = simulation.floorHeight(level, float(w.width()) / 2, float(w.height()) / 2);
                simd_float3 hit{};
                float t = 0;
                bool ok = true;
                for (int i = 0; i < 3 && ok; ++i) {
                    t = (z - eye.z) / dir.z;
                    ok = t > 0;
                    hit = eye + dir * t;
                    z = simulation.floorHeight(level, hit.x, hit.y);
                }
                const os2::vm::TilePos tile{int(std::floor(hit.x)), int(std::floor(hit.y)), level};
                if (ok && w.contains(tile) && (walkable ? w.walkable(tile) || w.pool(tile) : w.hasFloor(tile)) &&
                    drawnFloor(top, lotLevel, hit))
                    return GroundHit{simd_make_float2(hit.x, hit.y), t, level};
            }
            return std::nullopt;
        };
        if (pie) {
            pie->setCanvas(1280, 720);
            // A go-here destination goes when its menu closes, unless an interaction on it was queued.
            pie->onClose = [&](os2::vm::ObjectId id) { os2::vm::releaseDestination(pie->vm(), id); };
            pie->simAway = [&](os2::vm::ObjectId sim) { return !simInTheWorld(sim); };
            for (const auto& b : pickBoxes) {
                const auto& [lo, hi] = bounds[b.key];
                if (b.contained)
                    pie->addFollowingPickBox(b.instance, lo, hi, pickMeshes[b.key]);
                else
                    pie->addPickBox(b.instance, lo, hi, b.at, pickMeshes[b.key]);
            }
            // Objects in slots are picked where the VM view draws them.
            pie->setPlacementSource([&](os2::vm::ObjectId id) -> std::optional<simd_float4x4> {
                if (!objectView.hasModel(id))
                    return std::nullopt;
                const auto x = objectView.placement(id);
                if (!x)
                    return std::nullopt;
                simd_float4x4 m = simd_matrix4x4(simd_quaternion(x->q.x, x->q.y, x->q.z, x->q.w));
                m.columns[3] = simd_make_float4(x->t[0], x->t[1], x->t[2], 1);
                return m;
            });
            pie->drawn = [&](os2::vm::ObjectId id) { return objectView.shown(id); };
            // The levels above the camera's are not drawn: their objects are not picked.
            pie->pickable = [&](simd_float3 lo, simd_float3) {
                const int level = viewLevel();
                return os2::WallView::visible(lot, level, lo.x, lo.y, lo.z, wallView.lowestCut(level));
            };
            // A floor stops the ray (a Sim under it is not picked).
            pie->floorHit = [&](simd_float3 eye, simd_float3 dir) -> std::optional<float> {
                if (const auto g = groundUnderRay(eye, dir, false))
                    return g->t;
                return std::nullopt;
            };
            os2::UiResources ui(index, res);
            if (auto script = ui.script(0x090617B7))
                pie->setStyle(os2::PieMenuStyle::fromConstants(os2::uiConstants(*script)));
            if (auto p = simPortrait(pie->sim()))
                pie->setPortrait(std::move(*p));
            pie->setResources(index, res); // the dialog window's scripts
            pie->setSoundPlayer([&](const std::string& name) { gameAudio.playUi(name); });
            pie->setShowQueue(!(liveUi && liveUi->ok())); // the live panel draws the queue
            if (liveUi)
                liveUi->onSelectSim = [&](os2::vm::ObjectId id) {
                    pie->setSim(id);
                    if (auto p = simPortrait(id))
                        pie->setPortrait(std::move(*p));
                };
            if (buy)
                buy->pickObject = [&](simd_float3 eye, simd_float3 dir) { return pie->pick(eye, dir); };
            if (build)
                build->pickObject = [&](simd_float3 eye, simd_float3 dir) { return pie->pick(eye, dir); };
        }
        // The selected Sim's point for the cutaway (cWallCutawayManager,
        // docs/ui-live-mac.md 8d): where it stands while it is on the level
        // shown; worked out again only when it changes room (another tile
        // outdoors), or the camera or the view changed: between those times
        // the point stays. None unless the Sim is on the level shown.
        struct {
            bool stored = false;
            int level = 0;
            float x = 0, y = 0;
            std::optional<std::array<float, 3>> used;
            std::array<float, 6> camera{};
            int mode = -1, viewLevel = 0;
        } cutawaySim;
        auto cutawaySimPoint = [&](const os2::LotViewOptions& v) -> std::optional<std::array<float, 3>> {
            auto& c = cutawaySim;
            const os2::vm::Object* o = pie ? simulation.machine().object(pie->sim()) : nullptr;
            bool changed = false;
            if (o && o->position.inWorld() && o->position.level - 1 == v.level) {
                const auto& p = o->position;
                if (!c.stored) {
                    changed = true;
                } else {
                    const uint16_t was = os2::lotRoomAt(lot, c.level, c.x, c.y);
                    changed = was ? was != os2::lotRoomAt(lot, v.level, p.x, p.y)
                                  : (std::floor(c.x) != std::floor(p.x) || std::floor(c.y) != std::floor(p.y) ||
                                     c.level != v.level);
                }
                c.stored = true, c.level = v.level, c.x = p.x, c.y = p.y;
            }
            const simd_float3 e = camera.eye(), f = camera.pivot();
            const std::array<float, 6> cam{e.x, e.y, e.z, f.x, f.y, f.z};
            if (changed || cam != c.camera || c.mode != wallView.mode() || c.viewLevel != v.level || !c.used) {
                c.camera = cam, c.mode = wallView.mode(), c.viewLevel = v.level;
                if (c.stored)
                    c.used = std::array<float, 3>{c.x, c.y, lot.elevationAt(c.level, c.x, c.y)};
            }
            if (!c.stored || c.level != v.level)
                return std::nullopt;
            return c.used;
        };
        // The view the camera asks for; in cutaway the walls between the
        // camera and the selected Sim on the level shown come down too.
        wantedView = [&] {
            os2::LotViewOptions v = wallView.options(lookOf(), viewLevel());
            const simd_float3 e = camera.eye(); // cutaway tests the walls against the camera's position
            v.camera = std::array<float, 3>{e.x, e.y, e.z};
            if (v.walls == os2::LotViewOptions::Walls::Cutaway && pie && v.level != os2::LotViewOptions::kAllLevels)
                v.simPoint = cutawaySimPoint(v);
            return v;
        };
        // Keeps the lot model drawn for the current view: rebuilt (the
        // geometry; the cache keeps the materials and holes) when the wall
        // mode, the roof, the level or the cutaway side of the walls changed
        // (the cutaway compared apart: lotCutawayWalls follows the camera).
        std::vector<bool> cutaway;
        int lotViews = 0;
        auto updateLotView = [&] {
            const os2::LotViewOptions want = wantedView();
            const std::vector<bool> cut = os2::lotCutawayWalls(lot, want);
            if (!os2::WallView::sameCut(view, want) || cut != cutaway) {
                os2::LotLightMapLayout fresh;
                const os2::ObjectModel model = os2::buildLotModel(lot, index, patterns, want, &fresh, &cache);
                renderer.replaceObject(houseModel, model, index);
                if (want.level != view.level)
                    placeFences(want.level);
                if (fresh.walls.size() != layout.walls.size() || fresh.height != layout.height) {
                    layout = std::move(fresh);
                    if (lotLighting)
                        lotLighting->setLightMapLayout(layout);
                }
                view = want;
                cutaway = cut;
                ++lotViews;
            }
            if (liveUi)
                liveUi->setWallMode(wallView.mode());
        };
        // Sims above the level shown, or out of the world: out of sight and not picked.
        std::set<int16_t> simsHidden;
        auto showSims = [&] {
            simd_float4x4 away = matrix_identity_float4x4;
            away.columns[3] = simd_make_float4(0, 0, -1000, 1);
            simsHidden.clear();
            for (const auto& s : sims) {
                // A carried Sim is shown where its carrier stands.
                os2::Vec3 p = s.actor->transform ? os2::Vec3{s.actor->transform->t[0], s.actor->transform->t[1],
                                                             s.actor->transform->t[2]}
                                                 : s.actor->position;
                if (const os2::vm::Object* o = simulation.machine().object(s.id); o && o->person)
                    if (const DrawnSim* by = drawnSim(o->data[os2::vm::objdata::ContainerId]))
                        p = by->actor->position;
                const bool shown = simInTheWorld(s.id) &&
                                   os2::WallView::visible(lot, view.level, p[0], p[1], p[2], view.lowestCut);
                renderer.setInstanceTransform(s.instance, shown ? matrix_identity_float4x4 : away);
                if (!shown)
                    simsHidden.insert(s.id);
            }
        };
        // A right click on a Sim of the family, or the space bar for the next one, selects it.
        auto selectFamilySim = [&](os2::vm::ObjectId id) {
            if (!liveUi || !id || id == liveUi->sim())
                return false;
            const auto family = liveUi->family();
            if (std::find(family.begin(), family.end(), id) == family.end() || !simInTheWorld(id))
                return false;
            std::printf("opensims2: select Sim %d\n", id);
            if (liveUi->onSelectSim)
                liveUi->onSelectSim(id);
            liveUi->setSim(id);
            return true;
        };
        auto selectNextFamilySim = [&] {
            if (!liveUi)
                return;
            const auto family = liveUi->family();
            if (family.size() < 2)
                return;
            auto it = std::find(family.begin(), family.end(), liveUi->sim());
            selectFamilySim(it == family.end() || std::next(it) == family.end() ? family.front() : *std::next(it));
        };
        // The Sims' boxes as posed now, and the objects the VM made after the
        // lot loaded (bills, dishes, things bought), picked where they are drawn.
        std::map<int16_t, std::string> madePickBoxes;
        std::map<std::string, std::shared_ptr<const os2::PickMesh>> madeMeshes; // by CRES (null: none)
        auto updatePicks = [&] {
            if (!pie)
                return;
            for (const auto& s : sims) {
                if (!simInTheWorld(s.id) || simsHidden.count(s.id)) {
                    pie->setMovingBox(s.id, simd_make_float3(0, 0, 0), simd_make_float3(0, 0, 0));
                    continue;
                }
                simd_float3 lo = simd_make_float3(1e9f, 1e9f, 1e9f), hi = simd_make_float3(-1e9f, -1e9f, -1e9f);
                for (const auto& b : s.actor->bones) {
                    lo = simd_min(lo, simd_make_float3(b.t[0], b.t[1], b.t[2]));
                    hi = simd_max(hi, simd_make_float3(b.t[0], b.t[1], b.t[2]));
                }
                const auto& p = s.actor->position;
                if (s.actor->bones.empty()) // not posed yet: a standing adult's box
                    lo = simd_make_float3(p[0] - 0.15f, p[1] - 0.15f, p[2]), hi = simd_make_float3(p[0] + 0.15f, p[1] + 0.15f, p[2] + 1.7f);
                lo = lo - simd_make_float3(0.15f, 0.15f, 0.05f);
                hi = hi + simd_make_float3(0.15f, 0.15f, 0.15f);
                const bool posed = !s.actor->bones.empty() && !s.actor->posed.parts.empty();
                pie->setMovingBox(s.id, lo, hi, posed ? &s.actor->posed : nullptr);
            }
            std::set<int16_t> drawnNow;
            objectView.forEachDrawn([&](int16_t id, int, os2::SimActor*) { drawnNow.insert(id); });
            for (auto it = madePickBoxes.begin(); it != madePickBoxes.end();) {
                if (drawnNow.count(it->first) && objectView.cresOf(it->first) == it->second) {
                    ++it;
                    continue;
                }
                pie->removePickBox(it->first);
                it = madePickBoxes.erase(it);
            }
            for (const int16_t id : drawnNow) {
                if (madePickBoxes.count(id) || pie->hasPickBox(id))
                    continue;
                const std::string cres = objectView.cresOf(id);
                if (cres.empty())
                    continue;
                auto m = madeMeshes.find(cres);
                if (m == madeMeshes.end()) {
                    std::shared_ptr<const os2::PickMesh> mesh;
                    try {
                        mesh = os2::makePickMesh(os2::loadObjectModel(index, cres, {}), &index);
                    } catch (const std::exception&) {
                    }
                    m = madeMeshes.emplace(cres, std::move(mesh)).first;
                }
                if (!m->second || m->second->corners.empty())
                    continue;
                pie->addFollowingPickBox(id, m->second->lo, m->second->hi, m->second);
                if (pie->hasPickBox(id))
                    madePickBoxes[id] = cres;
            }
        };
        // A click on the ground: the selected Sim's menu on a go-here destination there (vm/go_here.hpp).
        auto openGoHere = [&](simd_float3 eye, simd_float3 dir, simd_float2 at) {
            if (!pie || !pie->sim())
                return false;
            const auto g = groundUnderRay(eye, dir, true);
            if (!g)
                return false;
            os2::vm::VM& m = pie->vm();
            const os2::vm::ObjectId dest = os2::vm::createGoHereDestination(m, pie->sim(), g->at.x, g->at.y, g->level);
            if (!dest)
                return false;
            if (!pie->open(dest, at)) {
                os2::vm::releaseDestination(m, dest);
                return false;
            }
            std::printf("opensims2: go here: destination %d at (%.2f, %.2f, %d)\n", dest, g->at.x, g->at.y, g->level);
            return true;
        };
        // A click that picked object `id`: its menu; a rug's merges a go-here
        // destination at the floor under the mouse (vm::objectClick).
        auto openClicked = [&](os2::vm::ObjectId id, simd_float3 eye, simd_float3 dir, simd_float2 at) {
            if (!pie || !pie->sim())
                return;
            os2::vm::VM& m = pie->vm();
            if (os2::vm::isGoHereRug(m, id)) {
                std::optional<os2::vm::Position> ground;
                if (const auto g = groundUnderRay(eye, dir, true))
                    ground = os2::vm::Position{g->at.x, g->at.y, g->level};
                bool has = false;
                for (const auto& e : os2::vm::pieMenu(m, pie->sim(), id))
                    has = has || (e.available() && !e.debug);
                const auto menu = os2::vm::objectClick(m, pie->sim(), id, ground, has);
                if (menu.object && pie->open(menu.object, at, menu.destination))
                    return;
                for (const os2::vm::ObjectId d : {menu.object, menu.destination})
                    if (d && d != id)
                        os2::vm::releaseDestination(m, d);
                return;
            }
            if (!pie->open(id, at))
                std::printf("opensims2: object %d: no interactions available\n", id);
        };

        // The "Salvar?" questions (UIText STR# 0x99: 3/4 leaving for the
        // neighborhood, 5/6 quitting with 24-26 as buttons; Sim/Não/Cancelar
        // from STR# 0x8E), put up as a dialog of no object that the pie menu draws.
        static constexpr os2::vm::ObjectId kFlowDialog = -2;
        enum class FlowDialog { None, Leave, Quit };
        FlowDialog flowDialog = FlowDialog::None;
        std::unique_ptr<os2::UiResources> texts;
        auto uiText = [&](uint32_t instance, uint32_t line, const char* fallback) -> std::string {
            try {
                if (!texts)
                    texts = std::make_unique<os2::UiResources>(index, res);
                if (auto t = texts->string(os2::UiStringRef{0x7fa7dc7c, instance, line}); t && !t->empty())
                    return *t;
            } catch (const std::exception&) {
            }
            return fallback;
        };
        auto openFlowDialog = [&](FlowDialog kind) {
            if (simulation.machine().dialogs.count(kFlowDialog))
                return;
            os2::vm::Dialog d;
            d.me = kFlowDialog;
            d.blocking = true;
            d.icon = 1;
            if (kind == FlowDialog::Leave) {
                d.title = uiText(0x99, 3, "Save?");
                d.message = uiText(0x99, 4, "Do you want to save your game before exiting to the Neighborhood?");
                d.buttons = {uiText(0x8E, 2, "Yes"), uiText(0x8E, 3, "No"), uiText(0x8E, 1, "Cancel")};
            } else {
                d.title = uiText(0x99, 5, "Quit?");
                d.message = uiText(0x99, 6, "Do you want to save before quitting?");
                d.buttons = {uiText(0x99, 24, "Save & Quit"), uiText(0x99, 25, "Quit without Saving"),
                             uiText(0x99, 26, "Don't Quit")};
            }
            simulation.machine().dialogs[kFlowDialog] = std::move(d);
            flowDialog = kind;
            std::printf("opensims2: dialog \"%s\" open\n", simulation.machine().dialogs[kFlowDialog].title.c_str());
        };
        // Saves the lot and its neighborhood to the user's folder (app/game_save.hpp).
        auto saveGameNow = [&] {
            // What the buy or build tool holds is not on the lot.
            if (buy && buy->held())
                buy->cancel();
            if (build && build->active())
                build->cancel();
            const auto began = std::chrono::steady_clock::now();
            const os2::GameSaveResult r =
                os2::saveGame(simulation.machine(), lotFile, res.parent_path().parent_path(), os2::sessionCharactersDir());
            for (const auto& l : r.lines)
                std::printf("opensims2: save: %s\n", l.c_str());
            if (r.ok)
                std::printf("opensims2: saved %s (%.2f s)\n", lotFile.string().c_str(),
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count());
            else
                std::fprintf(stderr, "opensims2: save failed: %s\n", r.error.c_str());
            return r.ok;
        };
        // Acts on the answer to the open "Salvar?" dialog, if it has one.
        auto pollFlowDialog = [&] {
            if (flowDialog == FlowDialog::None)
                return;
            auto& dialogs = simulation.machine().dialogs;
            auto it = dialogs.find(kFlowDialog);
            if (it == dialogs.end()) {
                flowDialog = FlowDialog::None;
                return;
            }
            const int answer = it->second.answer;
            if (answer < 0)
                return;
            dialogs.erase(it);
            const FlowDialog kind = flowDialog;
            flowDialog = FlowDialog::None;
            if (answer == 2)
                return; // cancel: stay
            if (answer == 0 && !saveGameNow())
                return; // the save failed: stay on the lot
            if (kind == FlowDialog::Leave) {
                os2::Scene back;
                back.kind = os2::Scene::Kind::Neighborhood;
                back.hood = scene.hood;
                exit.next = back;
            }
            running = false;
        };
        // Leaving the lot as the game does (ExitLotBegin): asks whether to save first.
        auto leaveLot = [&] {
            if (!scene.hood.empty())
                openFlowDialog(FlowDialog::Leave);
        };
        if (liveUi) {
            liveUi->setCanLeave(!scene.hood.empty());
            liveUi->onNeighborhood = leaveLot;
            liveUi->onQuit = [&] { openFlowDialog(FlowDialog::Quit); };
            liveUi->onSave = [&] { saveGameNow(); };
            try {
                lotOptions = std::make_unique<os2::OptionsPanel>(index, res, app.settings);
                lotOptions->onChanged = [&](const os2::GameSettings& s) {
                    app.settings = s;
                    if (app.user)
                        s.save(*app.user);
                    app.settings.applyGraphics(); // for what loads next
                    applyAudioSettings(gameAudio, app.settings);
                };
                lotOptions->playSound = [&](const std::string& name) { gameAudio.playUi(name); };
                liveUi->onOptionWindow = [&](int window) { lotOptions->toggleWindow(window); };
            } catch (const std::exception& e) {
                std::fprintf(stderr, "opensims2: options panel: %s\n", e.what());
                lotOptions.reset();
            }
        }
        // A click on an open option window (the options panel itself is the
        // live panel's): whatever the window covers is its own.
        auto pressOptions = [&](simd_float2 p) {
            if (!lotOptions)
                return false;
            if (liveUi && !liveUi->optionsOpen() && lotOptions->active())
                lotOptions->closeWindow();
            if (!lotOptions->active())
                return false;
            lotOptions->setCanvas(1280, 720);
            return lotOptions->press(p) != os2::OptionsPanel::Action::None || lotOptions->covers(p);
        };
        // Esc: the open pie menu first, else back to the neighborhood (the lot
        // given on its own just closes).
        auto escape = [&] {
            if (pie && pie->isOpen()) {
                pie->close();
                return;
            }
            if (pie && pie->dialogOpen())
                return;
            if (scene.hood.empty())
                running = false;
            else
                leaveLot();
        };
        // The tutorial each frame: its UI Effects, and the end of an episode
        // (Generic Sims Call 0 "Exit Lot", from "Simulation - Exit Tutorial"):
        // back to the main menu it was played from.
        auto pollTutorial = [&](double now) {
            if (!tutorial)
                return;
            tutorial->update(now);
            auto& requests = simulation.machine().engineRequests;
            if (tutorialRequestsSeen > requests.size())
                tutorialRequestsSeen = 0;
            for (; tutorialRequestsSeen < requests.size(); ++tutorialRequestsSeen)
                if (requests[tutorialRequestsSeen].call == 0 && !exit.next) {
                    const auto& t = requests[tutorialRequestsSeen].temps;
                    std::printf("opensims2: tutorial: episode over (state %d), exit lot%s\n",
                                simulation.machine().globals[26], t[2] ? ", saving" : "");
                    std::fflush(stdout);
                    // Exit Lot's temp 2: save the lot first (its SIMI keeps global 26
                    // "Tutorial State", which the episode list reads).
                    if (t[2])
                        saveGameNow();
                    os2::Scene next;
                    next.kind = os2::Scene::Kind::MainMenu;
                    exit.next = next;
                    running = false;
                }
        };

        // The mouse ray (world space) through a canvas point.
        auto viewProj = [&] { return simd_mul(camera.projection(1280.f / 720.f), camera.view()); };
        auto rayAt = [&](simd_float2 p) {
            const simd_float4x4 m = viewProj();
            const simd_float4 q = simd_mul(simd_inverse(m), simd_make_float4(2 * p.x / 1280.f - 1, 1 - 2 * p.y / 720.f, 0.5f, 1));
            return simd_normalize(simd_make_float3(q.x / q.w, q.y / q.w, q.z / q.w) - camera.eye());
        };
        // A press, in the Mac host's order: the dialog, the open pie menu, the
        // puck's modes, the options panel, Buy/Build's panels, the live panel,
        // then the world (Buy/Build's tools, else the pie menu of what the
        // mouse is on, else Go Here). `whole`: a scripted click (press and release).
        bool rightOnWorld = false; // the last right press went to the lot (the camera drag)
        auto mouseDown = [&](simd_float2 p, bool right, bool whole, bool control, bool shift) {
            if (pie && pie->dialogOpen() && pie->click(p)) {
                pollFlowDialog();
                return;
            }
            if (tutorial && !right) { // the tutorial's window (top right)
                tutorial->setCanvas(1280, 720);
                liveUi->setCanvas(1280, 720); // (its click trees find the controls laid out)
                if (tutorial->press(p))
                    return;
            }
            if (pie && pie->isOpen() && pie->press(p))
                return;
            if (!right && pressOptions(p)) // the option windows float over everything
                return;
            if (liveUi && !right) {
                if (liveUi->overModeButton(p) && liveUi->press(p))
                    return;
                if (liveUi->optionsOpen() && liveUi->press(p))
                    return;
            }
            const bool optionsOver = liveUi && liveUi->optionsOpen();
            if (!right && !optionsOver && buy && buy->active() && buy->press(p))
                return;
            if (!right && !optionsOver && build && build->active() && build->press(p))
                return;
            if (liveUi && liveUi->press(p, right)) {
                if (lotOptions && !liveUi->optionsOpen())
                    lotOptions->closeWindow();
                return;
            }
            if (!right && pressOptions(p)) // under the puck
                return;
            if (right) {
                rightOnWorld = true;
                if (pie && !(buy && buy->active()) && !(build && build->active()))
                    selectFamilySim(pie->pick(camera.eye(), rayAt(p)));
                return;
            }
            const simd_float3 eye = camera.eye(), dir = rayAt(p);
            if (buy && buy->active()) {
                if (whole)
                    buy->click(eye, dir, p);
                else
                    buy->mouseDown(eye, dir, p);
                return;
            }
            if (build && build->active()) {
                build->mouseDown(eye, dir, control, shift);
                if (whole)
                    build->mouseUp(eye, dir);
                return;
            }
            if (!pie)
                return;
            updatePicks();
            const os2::vm::ObjectId id = pie->pick(eye, dir);
            const os2::vm::Object* o = pie->vm().object(id);
            std::printf("opensims2: click (%g, %g) picked object %d (%s)\n", p.x, p.y, id,
                        o && o->definition ? o->definition->fileName.c_str() : "nothing");
            if (id)
                openClicked(id, eye, dir, p);
            else
                openGoHere(eye, dir, p);
            for (size_t e = 0; e < pie->entries().size(); ++e)
                std::printf("opensims2: pie [%zu] %s\n", e, pie->entries()[e].name.c_str());
        };
        auto mouseUp = [&](simd_float2 p) {
            if (pie && pie->isOpen()) {
                pie->release(p);
                return;
            }
            if (build && build->active())
                build->mouseUp(camera.eye(), rayAt(p));
            if (buy && buy->active())
                buy->mouseUp();
        };
        auto mouseMoved = [&](simd_float2 p, bool dragging) {
            if (pie)
                pie->hover(p);
            if (liveUi && !dragging)
                liveUi->hover(p);
            if (buy && buy->active()) {
                if (dragging) {
                    buy->mouseDrag(camera.eye(), rayAt(p));
                } else {
                    buy->hover(p);
                    buy->pointAt(camera.eye(), rayAt(p));
                }
            }
            if (build && build->active()) {
                if (dragging) {
                    build->mouseDrag(camera.eye(), rayAt(p));
                } else {
                    build->hover(p);
                    build->pointAt(camera.eye(), rayAt(p));
                }
            }
        };

        const auto start = std::chrono::steady_clock::now();
        double headlessClock = -1; // the clock of scripted runs (--wait), else real time
        auto seconds = [&] {
            if (headlessClock >= 0)
                return headlessClock;
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        };
        // Sims the VM makes while the lot plays (a baby born, an NPC, a
        // visitor): a person object the lot did not load gets an actor from
        // its character file (a newborn's from the baby writer; anyone
        // else's by its OBJD GUID in the session's or the neighborhood's
        // Characters folder). Persons without a character file stay undrawn.
        std::map<std::filesystem::path, std::map<uint32_t, std::filesystem::path>> characterFiles;
        auto characterFileOf = [&](uint32_t guid) -> std::optional<std::filesystem::path> {
            for (const std::filesystem::path& dir : {os2::sessionCharactersDir(), lotCharactersDir}) {
                if (dir.empty())
                    continue;
                auto& files = characterFiles[dir];
                std::error_code ec;
                for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                    if (e.path().extension() != ".package" ||
                        std::any_of(files.begin(), files.end(), [&](const auto& f) { return f.second == e.path(); }))
                        continue;
                    try {
                        os2::DbpfPackage pkg(e.path());
                        for (const auto& x : pkg.entries())
                            if (x.tgi.type == 0x4F424A44) { // OBJD
                                files[os2::ObjectDefinition::parse(pkg.read(x)).guid()] = e.path();
                                break;
                            }
                    } catch (const std::exception&) {
                    }
                }
                if (auto it = files.find(guid); it != files.end())
                    return it->second;
            }
            return std::nullopt;
        };
        // Balloon icons the app draws: a Sim's portrait (0x10), a want's or fear's icon (0x2000).
        if (lotEffects)
            lotEffects->iconImage = [&](const os2::vm::ActiveEffect& e) -> std::optional<os2::RgbaImage> {
                os2::vm::VM& m = simulation.machine();
                if (e.iconSim) {
                    // The variable holds the Sim's neighbor ID (the memories'
                    // and socials' balloons), else its object ID.
                    for (const auto& [id, o] : m.objects())
                        if (o.person && os2::vm::neighborIdOf(o) == e.iconSim)
                            if (auto p = simPortrait(id))
                                return p;
                    if (const os2::vm::Neighbor* n = m.neighbor(e.iconSim); n && n->guid)
                        if (auto file = characterFileOf(n->guid))
                            try {
                                os2::DbpfPackage character(*file);
                                for (const auto& x : character.entries())
                                    if (x.tgi.type == os2::kTypeUiImage && x.tgi.instance < 0x100)
                                        return os2::decodeUiImage(character.read(x));
                            } catch (const std::exception&) {
                            }
                    return simPortrait(e.iconSim);
                }
                if (e.wantSim) {
                    const os2::vm::Object* sim = m.object(e.wantSim);
                    os2::vm::WantSystem* ws = os2::vm::wantSystem(m);
                    const os2::vm::SimWants* w = sim && ws ? os2::vm::simWants(m, os2::vm::neighborIdOf(*sim)) : nullptr;
                    const auto& list = w ? (e.wantFear ? w->fears : w->wants) : std::vector<os2::vm::WantPtr>{};
                    if (e.wantSlot >= 0 && size_t(e.wantSlot) < list.size() && list[size_t(e.wantSlot)])
                        if (const auto* d = ws->library->definition(list[size_t(e.wantSlot)]->data.guid))
                            if (uint32_t icon = d->primaryIcon ? d->primaryIcon : d->secondaryIcon)
                                if (auto bytes = index.read(os2::Tgi{os2::kTypeUiImage, os2::kUiImageGroup, icon, 0}))
                                    return os2::decodeUiImage(*bytes);
                }
                return std::nullopt;
            };
        std::set<int16_t> simsLookedAt; // person objects already adopted or given up on
        for (const auto& s : sims)
            simsLookedAt.insert(s.id);
        std::map<uint32_t, std::optional<std::filesystem::path>> embeddedCharacters;
        // Actors of Sims gone from the lot: kept (other views may still point at them), out of sight.
        std::vector<std::unique_ptr<os2::SimActor>> goneActors;
        std::set<const os2::SimActor*> carriedSims; // drawn at a carrier's bone (last frame)
        double sceneNow = 0;
        auto adoptVmSims = [&] {
            os2::vm::VM& m = simulation.machine();
            for (int16_t id : simulation.goneSims()) {
                simulation.detach(id);
                for (auto it = sims.begin(); it != sims.end();)
                    if (it->id == id) {
                        simd_float4x4 away = matrix_identity_float4x4;
                        away.columns[3] = simd_make_float4(0, 0, -1000, 1);
                        renderer.setInstanceTransform(it->instance, away);
                        carriedSims.erase(it->actor.get());
                        goneActors.push_back(std::move(it->actor));
                        it = sims.erase(it);
                    } else {
                        ++it;
                    }
                objectView.removeSim(id);
                if (lotLighting)
                    lotLighting->removeSim(id);
                if (pie)
                    pie->setMovingBox(os2::vm::ObjectId(id), simd_make_float3(0, 0, 0), simd_make_float3(0, 0, 0));
                simsLookedAt.erase(id); // a Sim given the ID later is adopted anew
                std::printf("opensims2: Sim %d leaves the lot (its object is gone)\n", id);
            }
            std::vector<int16_t> fresh;
            for (const auto& [id, o] : m.objects())
                if (o.person && (o.position.inWorld() || o.data[os2::vm::objdata::ContainerId] != 0) && !simsLookedAt.count(id))
                    fresh.push_back(id);
            for (int16_t id : fresh) {
                simsLookedAt.insert(id);
                if (std::any_of(sims.begin(), sims.end(), [&](const DrawnSim& s) { return s.id == id; }))
                    continue;
                const os2::vm::Object& o = *m.object(id);
                const int16_t neighbor = o.person->data[os2::vm::persondata::NeighborId];
                std::optional<std::filesystem::path> file;
                if (auto it = bornCharacters.find(neighbor); it != bornCharacters.end())
                    file = it->second;
                else if (const os2::vm::Neighbor* n = m.neighbor(neighbor))
                    file = characterFileOf(n->guid);
                // The game's unique NPCs (the Grim Reaper, the Pollination
                // Technician) keep their character in their object's group.
                if (const os2::vm::Neighbor* n = m.neighbor(neighbor); !file && n && n->guid) {
                    auto [it, first] = embeddedCharacters.try_emplace(n->guid);
                    if (first)
                        try {
                            char stem[32];
                            std::snprintf(stem, sizeof stem, "Unique_%08X.package", n->guid);
                            const std::filesystem::path out = os2::sessionCharactersDir() / "NPCs" / stem;
                            if (os2::cas::writeEmbeddedCharacter(index, n->guid, out))
                                it->second = out;
                        } catch (const std::exception& e) {
                            std::fprintf(stderr, "opensims2: Sim %d (neighbor %d): %s\n", id, neighbor, e.what());
                        }
                    file = it->second;
                }
                if (!file) {
                    std::fprintf(stderr, "opensims2: Sim %d (neighbor %d): no character file, not drawn\n", id, neighbor);
                    continue;
                }
                const simd_float3 at = simd_make_float3(o.position.x, o.position.y,
                                                        simulation.floorHeight(o.position.level, o.position.x, o.position.y));
                if (!spawnSim(*file, id, at, -float(o.data[os2::vm::objdata::Direction]) * kEighthTurn, sceneNow, "joins"))
                    continue;
                os2::SimActor* actor = sims.back().actor.get();
                objectView.addSim(id, actor);
                if (lotLighting)
                    lotLighting->addSim(id, actor);
                lotAudio.attachSim(*actor, id, {});
                std::printf("opensims2: Sim %d (neighbor %d) joins the lot at tick %llu: %s, skeleton %s\n", id, neighbor,
                            (unsigned long long)m.ticks, file->filename().string().c_str(), actor->skin.skeleton.name.c_str());
                std::fflush(stdout);
            }
        };
        // The plumbob over the selected Sim: the game's PlumbBob (a VM object
        // in the Sim's head slots holder) when it is over that Sim; otherwise
        // the app's own, spinning once every 2.316 s ("o-plumbob-spin") over
        // the top of the posed mesh in the game's one's colour, and the
        // game's ones hidden, so that there is one plumbob.
        constexpr float kPlumbobGap = 0.2f, kPlumbobSpinSeconds = 2.316f;
        int plumbob = -1;
        float plumbobBottom = 0; // the model's lowest point (modelled about 1.24 over its origin)
        try {
            const os2::ObjectModel model = os2::loadObjectModel(index, "plumbob");
            plumbob = renderer.addInstance(renderer.addObject(model, index), matrix_identity_float4x4);
            plumbobBottom = os2::modelBounds(model).first.z;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: plumbob: %s\n", e.what());
        }
        bool vmPlumbob = false;
        std::map<std::string, int> plumbobModels; // per core material
        std::string plumbobMaterial;
        auto choosePlumbob = [&] {
            if (!pie)
                return;
            vmPlumbob = false;
            std::string material;
            for (const auto& [id, o] : simulation.machine().objects()) {
                if (o.person || !o.definition || o.definition->fileName != "PlumbBob")
                    continue;
                const bool over = objectView.hasModel(id) && objectView.outermostContainer(id) == pie->sim();
                objectView.setHidden(id, !over || vmPlumbob);
                if (!over || vmPlumbob)
                    hiddenPlumbobs.insert(id);
                else
                    hiddenPlumbobs.erase(id);
                if (over && !vmPlumbob)
                    vmPlumbob = true;
                if (material.empty() || over) {
                    if (auto it = o.materials.find("core"); it != o.materials.end())
                        material = it->second;
                    else if (!o.materials.empty())
                        material = o.materials.begin()->second;
                }
            }
            if (vmPlumbob || material == plumbobMaterial || plumbob < 0)
                return;
            auto it = plumbobModels.find(material);
            if (it == plumbobModels.end()) {
                int m = -1;
                try {
                    m = renderer.addObject(os2::loadObjectModel(index, "plumbob", {{"core", material}}), index);
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "opensims2: plumbob %s: %s\n", material.c_str(), e.what());
                }
                it = plumbobModels.emplace(material, m).first;
            }
            if (it->second >= 0)
                renderer.setInstanceModel(plumbob, it->second);
            plumbobMaterial = material;
        };
        auto placePlumbob = [&](double seconds) {
            appPlumbobAt.reset();
            if (plumbob < 0)
                return;
            simd_float4x4 at = matrix_identity_float4x4;
            at.columns[3] = simd_make_float4(0, 0, -1000, 1); // hidden
            for (const auto& s : sims) {
                // Not over a Sim out of the world (at work, at school).
                if (!pie || s.id != pie->sim() || vmPlumbob || !simInTheWorld(s.id) || simsHidden.count(s.id))
                    continue;
                float top = s.actor->position[2];
                for (const auto& p : s.actor->posed.parts)
                    for (const auto& v : p.mesh.positions)
                        top = std::max(top, v[2]);
                const float angle = float(std::fmod(seconds / kPlumbobSpinSeconds, 1.0) * 2 * 3.14159265358979);
                at = simd_matrix4x4(simd_quaternion(angle, simd_make_float3(0, 0, 1)));
                at.columns[3] = simd_make_float4(s.actor->position[0], s.actor->position[1], top + kPlumbobGap - plumbobBottom, 1);
                appPlumbobAt = os2::Xform{os2::quatFromEulerXYZ(0, 0, angle),
                                          {s.actor->position[0], s.actor->position[1], top + kPlumbobGap - plumbobBottom}};
            }
            renderer.setInstanceTransform(plumbob, at);
        };
        // Runs the lot's VM `ticks` ticks headless (a --wait step).
        auto runTicks = [&](long ticks) {
            const uint64_t target = simulation.ticks() + uint64_t(std::max(0L, ticks));
            // Paused with a tutorial running: its controller goes on (pollTutorial),
            // so a wait is that many ticks of real time.
            const bool pausedTutorial = tutorial && simulation.machine().globals[os2::vm::global::Paused];
            for (long guard = 0; simulation.ticks() < target && guard < (pausedTutorial ? ticks * 30 / 23 + 1 : ticks * 40 + 100);
                 ++guard) {
                headlessClock += 1.0 / 30;
                const double sceneTime = simulation.update(headlessClock);
                if (lotLighting && simulation.ticks() % 30 == 0)
                    lotLighting->update(sceneTime);
                if (liveUi)
                    liveUi->tick(headlessClock);
                pollTutorial(headlessClock);
            }
            adoptVmSims();
            std::printf("opensims2: waited to VM tick %llu\n", (unsigned long long)simulation.ticks());
        };
        // Held keys: pan left, right, back, forward; zoom in, out; rotate left, right.
        bool keys[8]{};
        double lastFrame = -1;
        auto frame = [&] {
            const double now = seconds();
            const double dt = lastFrame < 0 ? 1.0 / 60 : std::clamp(now - lastFrame, 0.0, 0.1);
            lastFrame = now;
            if (liveUi)
                liveUi->tick(now);
            const double scene = simulation.update(now);
            sceneNow = scene;
            pollTutorial(now);
            // The animated objects first: the Sims' hands reach for their bones.
            for (auto& [id, a] : objectActors)
                if (a)
                    a->update(scene);
            adoptVmSims();
            {
                // Shift: "Move ... Faster" (twice the speed).
                const float fast = (SDL_GetModState() & SDL_KMOD_SHIFT) ? 2.0f : 1.0f;
                // The held keys as the camera's requests, as app_view.mm's applyHeldKeys
                // (docs/camera-ui-mac.md 1.11): move, zoom and rotate (0 = released).
                camera.setViewport(1280, 720);
                camera.setKeyMove((float(keys[1]) - float(keys[0])) * fast, (float(keys[3]) - float(keys[2])) * fast);
                camera.setKeyZoom(int(keys[4]) - int(keys[5]));
                camera.setKeyRotate(int(keys[7]) - int(keys[6]));
                // The window's edge (DoWindowEdgeScrolling, once a frame): the step requests.
                cameraMouse.config().edgeScroll = app.settings.edgeScrolling;
                const os2::CameraMouseInput::EdgeSteps edge = cameraMouse.edgeSteps(1280, 720);
                if (edge.sendSideways)
                    camera.stepSideways(edge.sideways);
                if (edge.sendForward)
                    camera.stepForward(edge.forward);
                // Right button held: the anchor's offset x 0.25 as the move request.
                if (cameraMouse.anchored()) {
                    const auto [dragX, dragY] = cameraMouse.dragMove();
                    camera.moveRequest(dragX, dragY);
                }
                if (pie)
                    follow.follow(simulation.machine(), pie->sim());
                camera.update(dt);
                updateLotView();
            }
            {
                std::vector<os2::Vec3> feet;
                for (const auto& s : sims)
                    feet.push_back(s.actor->position);
                doors.update(simulation.machine(), feet, scene);
            }
            // Body shape from person data 81 "Body Flags" and 58 age.
            for (auto& s : sims)
                if (const auto* o = simulation.machine().object(s.id); o && o->person)
                    s.actor->bodyMorphs = os2::bodyShapeMorphs(o->person->data[81], o->person->data[58]);
            // A carried baby or toddler (in a Sim's slot, "Pickup - Do Carry
            // Sim") is drawn at the carrier's slot bone, after the carrier is posed.
            std::map<os2::SimActor*, std::pair<const os2::SimActor*, std::string>> carried;
            for (const auto& s : sims) {
                const auto* o = simulation.machine().object(s.id);
                const int16_t c = o && o->person ? o->data[os2::vm::objdata::ContainerId] : 0;
                const auto* co = c ? simulation.machine().object(c) : nullptr;
                const DrawnSim* carrier = co && co->person ? drawnSim(c) : nullptr;
                if (carrier) {
                    std::string bone = objectView.containerSlotBone(c, o->data[os2::vm::objdata::SlotNumber]);
                    if (bone.empty())
                        bone = "BellyTarget_surface"; // the carry animations' grip (inferred)
                    carried[s.actor.get()] = {carrier->actor.get(), bone};
                    carriedSims.insert(s.actor.get());
                } else if (carriedSims.erase(s.actor.get())) {
                    s.actor->transform.reset();
                }
            }
            for (auto& sim : sims)
                if (!carried.count(sim.actor.get()))
                    sim.actor->update(scene);
            for (auto& [a, by] : carried) {
                const int b = by.first->skin.skeleton.find(by.second);
                a->transform = b >= 0 && size_t(b) < by.first->bones.size() ? by.first->bones[size_t(b)] : by.first->placement();
                a->update(scene);
            }
            // The carriers again: their hands reach the carried Sim's bones by IK.
            {
                std::set<const os2::SimActor*> again;
                for (const auto& [a, by] : carried)
                    again.insert(by.first);
                for (auto& sim : sims)
                    if (again.count(sim.actor.get()))
                        sim.actor->update(scene);
            }
            showSims();
            choosePlumbob();
            objectView.update();
            updatePicks();
            if (lotLighting)
                lotLighting->update(scene);
            for (auto& [id, a] : objectActors)
                if (a) {
                    a->update(scene);
                    renderer.updateObject(a->model, a->posed);
                }
            for (auto& sim : sims)
                renderer.updateObject(sim.actor->model, sim.actor->posed);
            placePlumbob(scene);
            if (lotEffects) {
                // The effect events of the animations playing (Sims, animated objects).
                std::vector<const os2::SimActor*> animated;
                for (const auto& s : sims)
                    animated.push_back(s.actor.get());
                for (const auto& [id, a] : objectActors)
                    if (a)
                        animated.push_back(a.get());
                lotEffects->animationEffects(animated, scene);
                lotEffects->update(scene, &simulation.machine(), &objectView, [&](int16_t id) -> const os2::SimActor* {
                    const DrawnSim* s = drawnSim(id);
                    return s ? s->actor.get() : nullptr;
                });
                std::vector<os2::fx::EffectSound> sounds; // fire, flies, sparks, ...
                lotEffects->collectSounds(sounds);
                lotAudio.effectSounds(sounds);
            }
            os2::LotAudio::Listener listener;
            {
                const simd_float3 p = camera.pivot(), look = p - camera.eye();
                listener.x = p.x, listener.y = p.y, listener.z = std::max(0.0f, p.z - 1);
                listener.rightX = look.y, listener.rightY = -look.x; // forward x up
                listener.zoom = std::clamp(camera.zoom() / 5.0f, 0.0f, 1.0f);
            }
            lotAudio.update(&simulation.machine(), scene, listener);
            if (tv) {
                tv->update(scene);
                // A channel's sound comes out of its first receiver (the lowest
                // object ID showing it), as the Mac host does; the other TVs on it are silent.
                std::vector<os2::LotAudio::TvSource> on;
                std::set<std::string> heard;
                for (const auto& [id, o] : simulation.machine().objects()) // (by ID)
                    for (const auto& [group, material] : o.materials)
                        if (const auto* s = tv->isChannel(material) ? tv->sound(material) : nullptr) {
                            if (heard.insert(material).second)
                                on.push_back({id, s->serial, s->pcm, std::max(0.0, tv->now() - s->started)});
                            break;
                        }
                lotAudio.tvSounds(simulation.machine(), on);
            }
            std::vector<os2::GpuRenderer::ShadowCaster> casters;
            for (const auto& sim : sims) {
                if (lotLighting)
                    break; // LotLighting casts the Sims' shadows
                os2::GpuRenderer::ShadowCaster caster;
                caster.instance = sim.instance;
                caster.feet = simd_make_float3(sim.actor->position[0], sim.actor->position[1], sim.actor->position[2]);
                caster.heading = sim.actor->heading;
                int level = layout.minLevel;
                for (int l = layout.minLevel; l < layout.minLevel + layout.levels; ++l)
                    if (std::fabs(layout.elevationAt(l, caster.feet.x, caster.feet.y) - caster.feet.z) <
                        std::fabs(layout.elevationAt(level, caster.feet.x, caster.feet.y) - caster.feet.z))
                        level = l;
                caster.outside = layout.outdoor(level, layout.roomAt(level, caster.feet.x, caster.feet.y));
                caster.direction = caster.outside ? shadowTravel : simd_float3{0, 0, -1};
                caster.color = shadowColor;
                casters.push_back(caster);
            }
            if (!lotLighting)
                renderer.setShadowCasters(casters);
            std::vector<os2::GpuRenderer::CensorBox> censor;
            const char* censorOff = std::getenv("OS2_NO_CENSOR");
            const char* censorForce = std::getenv("OS2_CENSOR");
            if (!(censorOff && censorOff[0] == '1')) {
                for (const auto& sim : sims) {
                    uint16_t flags = 0;
                    bool female = false;
                    if (const os2::vm::Object* object = simulation.machine().object(sim.id); object && object->person) {
                        flags = uint16_t(object->person->data[size_t(os2::censor::kPersonData)]);
                        female = object->person->data[65] == 1;
                    }
                    if (censorForce && censorForce[0] == '1' && flags == 0)
                        flags = os2::censor::FullBody;
                    for (const auto& box : os2::censorBoxes(*sim.actor, flags, female)) {
                        os2::GpuRenderer::CensorBox gpu;
                        gpu.lo = simd_make_float3(box.lo[0], box.lo[1], box.lo[2]);
                        gpu.hi = simd_make_float3(box.hi[0], box.hi[1], box.hi[2]);
                        censor.push_back(gpu);
                    }
                }
            }
            renderer.setCensorBoxes(censor);
            static bool censorLogged = false;
            if (!censorLogged) {
                censorLogged = true;
                std::fprintf(stderr, "opensims2: censor boxes %zu\n", censor.size());
            }
            renderer.setEye(camera.eye());
            renderer.setView(camera.view());
            return drawFrame(renderer, viewProj());
        };

        // A few ticks so the capture shows the VM's animation, not the rest pose.
        for (int i = 0; i < 30; ++i)
            frame();
        camera.snap();
        updateLotView();
        loading.finish();
        // A lot reached through the game flow runs the script's remaining
        // steps (--click, --right-click, --wait, --key escape; app/game_flow.hpp);
        // a step that leaves the lot leaves the rest to the next scene.
        if (exitAfterShot && !diagnostics) {
            headlessClock = seconds();
            if (tutorial) { // the tutorial presses and points at controls laid out at the shot's size
                showLiveUi();
                pollTutorial(headlessClock);
            }
            const auto steps = os2::flowSteps(app.args);
            for (size_t k = 0; k < steps.size() && running; ++k) {
                exit.stepsUsed = k + 1;
                const os2::FlowStep& st = steps[k];
                if (st.kind == os2::FlowStep::Kind::Wait) {
                    runTicks(st.ticks);
                } else if (st.kind == os2::FlowStep::Kind::TutorialAuto) { // --tutorial-auto <turns>[:<step>]
                    // Each turn: a click on what the arrow points at or Next;
                    // else what a step waiting on the Sim waits for (a Go
                    // Here, an object bought, a job: vm::tutorialAutoAct);
                    // else time passes. Stops after ~3000 idle ticks.
                    int idle = 0;
                    // The trees' own dialogs (the job offers) get their first button.
                    int& autoAnswer = simulation.machine().autoDialogAnswer;
                    const int answerBefore = autoAnswer;
                    if (autoAnswer < 0)
                        autoAnswer = 0;
                    for (long turn = 0; tutorial && turn < st.ticks && running && idle < 50; ++turn) {
                        const auto& progress = simulation.machine().tutorial.progress;
                        if (st.entry > 0 && progress.visible && progress.step >= st.entry)
                            break;
                        showLiveUi();
                        pollTutorial(headlessClock);
                        if (tutorial->autoClick()) {
                            idle = 0;
                            runTicks(40);
                            continue;
                        }
                        if (idle % 10 == 1)
                            if (const std::string done = os2::vm::tutorialAutoAct(simulation.machine(), &index); !done.empty())
                                std::printf("opensims2: tutorial: %s\n", done.c_str());
                        ++idle;
                        runTicks(60);
                    }
                    autoAnswer = answerBefore;
                } else if (st.kind == os2::FlowStep::Kind::Click || st.kind == os2::FlowStep::Kind::RightClick) {
                    const simd_float2 p = simd_make_float2(st.x, st.y);
                    const bool right = st.kind == os2::FlowStep::Kind::RightClick;
                    showLiveUi(); // the panels laid out as the last frame left them
                    mouseDown(p, right, true, false, false);
                    if (!right)
                        mouseUp(p);
                } else if (st.kind == os2::FlowStep::Kind::Key && st.text == "escape") {
                    escape();
                }
                pollFlowDialog();
                std::fflush(stdout);
            }
            if (exit.next || !running)
                return exit;
            camera.snap();
            for (int i = 0; i < 5; ++i) {
                headlessClock += 1.0 / 30;
                frame();
            }
            camera.snap();
            showLiveUi();
            renderer.setEye(camera.eye());
            renderer.setView(camera.view());
            if (!renderer.capture(shot, viewProj()))
                exit.code = 1;
            else
                std::printf("opensims2: wrote %s (%llu ticks, %zu Sims, %zu objects animated, %zu doors open, %zu TV channels)\n",
                            shot.string().c_str(), (unsigned long long)simulation.ticks(), sims.size(),
                            size_t(std::count_if(objectActors.begin(), objectActors.end(), [](const auto& a) { return a.second != nullptr; })),
                            doors.openCount(), tv ? tv->channelsInUse() : size_t(0));
            std::printf("opensims2: sound: %s\n", lotAudio.summary().c_str());
            return exit;
        }
        bool pauseClick = false;
        if (liveUi && liveUi->ok()) {
            if (const auto rect = liveUi->controlRect(os2::LiveUi::kMainWindow, 0x27D7258F)) {
                const simd_float2 at{((*rect)[0] + (*rect)[2]) * 0.5f, ((*rect)[1] + (*rect)[3]) * 0.5f};
                pauseClick = liveUi->press(at);
            }
        }
        showLiveUi();
        renderer.setEye(camera.eye());
        renderer.setView(camera.view());
        if (!renderer.capture(shot, viewProj()))
            throw std::runtime_error("screenshot failed");
        const int censorOnFrame = renderer.censorCount();
        const os2::LotCamera::Pose homePose = camera.pose();
        simd_float3 mirrorCenter, mirrorNormal;
        if (renderer.mirrorAnchor(mirrorCenter, mirrorNormal)) {
            Camera mirrorCamera; // level, 1.6 tiles in front of the glass
            mirrorCamera.pivot = mirrorCenter;
            mirrorCamera.yaw = std::atan2(mirrorNormal.y, mirrorNormal.x);
            mirrorCamera.pitch = 0.f;
            mirrorCamera.distance = 1.6f;
            renderer.setEye(mirrorCamera.eye());
            const auto mirrorShot = shot.parent_path() / "frame_mirror.png";
            if (renderer.capture(mirrorShot, mirrorCamera.viewProj(1280.f / 720.f)))
                std::printf("opensims2: mirror at (%.2f, %.2f, %.2f) normal (%.2f, %.2f, %.2f) -> %s\n", mirrorCenter.x,
                            mirrorCenter.y, mirrorCenter.z, mirrorNormal.x, mirrorNormal.y, mirrorNormal.z,
                            mirrorShot.string().c_str());
        } else {
            std::printf("opensims2: no mirror in the lot\n");
        }
        std::ofstream log(shot.parent_path() / "frame_vm.txt");
        if (shot.parent_path().empty())
            log = std::ofstream("frame_vm.txt");
        log << simulation.summary() << "\n";
        log << "ticks " << simulation.ticks() << "\n";
        log << "shadows " << renderer.shadowCount() << "\n";
        if (lotLighting) {
            size_t points = 0;
            for (const auto& set : renderer.lightSets())
                points += set.points.size();
            log << "lighting " << lotLighting->stateName() << "; light sets " << renderer.lightSets().size() << "; lamps "
                << lotLighting->lamps().size() << "; point lights " << points << "\n";
        } else {
            log << "lighting static day\n";
        }
        log << "censor " << censorOnFrame << "\n";
        log << "live-ui " << liveWindows << " windows " << liveDrawn << " drawn\n";
        log << "ui-pause " << (pauseClick ? "clicked" : "miss") << " vm "
            << simulation.machine().globals[os2::vm::global::Paused] << "\n";
        std::string saveLine = "save skipped";
        try {
            const auto originalStamp = std::filesystem::last_write_time(lotFile);
            const auto hoodDir = lotFile.parent_path().parent_path();
            const auto hoodName = hoodDir.filename().string() + "_Neighborhood.package";
            const auto originalHood = hoodDir / hoodName;
            const bool haveHood = std::filesystem::is_regular_file(originalHood);
            const auto hoodStamp = haveHood ? std::filesystem::last_write_time(originalHood) : std::filesystem::file_time_type{};
            const auto saveDir = shot.parent_path().empty() ? std::filesystem::path("save-test")
                                                            : shot.parent_path() / "save-test";
            const auto saveLot = saveDir / hoodDir.filename() / "Lots" / lotFile.filename();
            std::filesystem::create_directories(saveLot.parent_path());
            std::filesystem::copy_file(lotFile, saveLot, std::filesystem::copy_options::overwrite_existing);
            if (haveHood)
                std::filesystem::copy_file(originalHood, saveLot.parent_path().parent_path() / hoodName,
                                           std::filesystem::copy_options::overwrite_existing);
            os2::LotObjects before;
            {
                os2::DbpfPackage beforePackage(saveLot);
                before = os2::LotObjects::load(beforePackage);
            }
            const os2::GameSaveResult written =
                os2::saveGame(simulation.machine(), saveLot, res.parent_path().parent_path(), os2::sessionCharactersDir());
            const bool untouched = std::filesystem::last_write_time(lotFile) == originalStamp;
            if (!written.ok) {
                saveLine = "save failed: " + written.error;
            } else {
                os2::LotObjects after;
                {
                    os2::DbpfPackage afterPackage(saveLot);
                    after = os2::LotObjects::load(afterPackage);
                }
                int moved = 0;
                for (const auto& now : after.objects) {
                    if (!now.person || now.x < 0)
                        continue;
                    for (const auto& was : before.objects)
                        if (was.id == now.id && (std::fabs(now.x - was.x) > 0.01f || std::fabs(now.y - was.y) > 0.01f))
                            ++moved;
                }
                std::string hoodLine;
                for (const auto& line : written.lines)
                    if (line.rfind("neighborhood ", 0) == 0)
                        hoodLine = line;
                const bool hoodUntouched = !haveHood || std::filesystem::last_write_time(originalHood) == hoodStamp;
                saveLine = (written.lines.empty() ? "save ok" : written.lines.front()) + "; sims moved " + std::to_string(moved) +
                           (hoodLine.empty() ? "; no neighborhood" : "; " + hoodLine) +
                           (untouched ? "; original untouched" : "; ORIGINAL CHANGED") +
                           (hoodUntouched ? "; hood original untouched" : "; HOOD ORIGINAL CHANGED");
            }
            std::printf("opensims2: %s\n", saveLine.c_str());
        } catch (const std::exception& e) {
            saveLine = std::string("save failed: ") + e.what();
            std::fprintf(stderr, "opensims2: %s\n", saveLine.c_str());
        }
        log << "save " << saveLine << "\n";
        std::string buyLine = "buy skipped";
        if (buy && buy->ok() && liveUi && liveUi->ok()) {
            camera.setPose(homePose);
            bool buyClick = false;
            if (const auto rect = liveUi->controlRect(os2::LiveUi::kMainWindow, 0x67D97B2A)) {
                const simd_float2 at{((*rect)[0] + (*rect)[2]) * 0.5f, ((*rect)[1] + (*rect)[3]) * 0.5f};
                buyClick = liveUi->press(at);
            }
            std::string placedLine = "nothing placed";
            if (buy->active()) {
                const size_t before = simulation.machine().objects().size();
                int tried = 0;
                bool bought = false;
                for (const auto& entry : buy->catalog().entries()) {
                    if (bought || tried >= 8)
                        break;
                    if (entry.multiTile || entry.price <= 0 || entry.price > 400 || !buy->select(entry))
                        continue;
                    ++tried;
                    const int cx = int(camera.pivot().x), cy = int(camera.pivot().y);
                    for (int r = 1; r < 10 && !bought; ++r)
                        for (int dx = -r; dx <= r && !bought; ++dx)
                            for (int dy = -r; dy <= r && !bought; ++dy)
                                if ((std::abs(dx) == r || std::abs(dy) == r) && buy->placeAt(cx + dx, cy + dy, 2)) {
                                    bought = true;
                                    camera.setPivot(simd_make_float3(float(cx + dx) + 0.5f, float(cy + dy) + 0.5f, camera.pivot().z));
                                }
                    if (!bought)
                        buy->cancel();
                }
                placedLine = buy->message() + "; vm objects " + std::to_string(before) + " -> " +
                             std::to_string(simulation.machine().objects().size());
                for (int i = 0; i < 5; ++i)
                    frame();
                camera.snap();
                showLiveUi();
                renderer.setEye(camera.eye());
                renderer.setView(camera.view());
                const auto buyShot = shot.parent_path() / "frame_buy.png";
                renderer.capture(buyShot, viewProj());
            }
            buyLine = std::string(buyClick ? "clicked" : "miss") + (buy->active() ? " active" : " inactive") + " panel " +
                      std::to_string(buyWindows) + " windows " + std::to_string(buyDrawn) + " drawn; catalog " +
                      std::to_string(buy->catalog().entries().size()) + "; " + placedLine;
            std::printf("opensims2: buy %s\n", buyLine.c_str());
        }
        log << "buy " << buyLine << "\n";
        std::string buildLine = "build skipped";
        if (build && build->ok() && liveUi && liveUi->ok()) {
            camera.setPose(homePose);
            bool buildClick = false;
            if (const auto rect = liveUi->controlRect(os2::LiveUi::kMainWindow, 0x77D97B28)) {
                const simd_float2 at{((*rect)[0] + (*rect)[2]) * 0.5f, ((*rect)[1] + (*rect)[3]) * 0.5f};
                buildClick = liveUi->press(at);
            }
            std::string roomLine = "no room drawn";
            if (build->active()) {
                const size_t wallsBefore = lot.walls.size();
                const int rebuildsBefore = lotRebuilds;
                const int cx = int(camera.pivot().x), cy = int(camera.pivot().y);
                std::string drawn;
                for (int r = 2; r < 12 && drawn.empty(); ++r)
                    for (int dx = -r; dx <= r && drawn.empty(); dx += 2)
                        for (int dy = -r; dy <= r && drawn.empty(); dy += 2) {
                            if (std::abs(dx) != r && std::abs(dy) != r)
                                continue;
                            const int x0 = cx + dx, y0 = cy + dy;
                            if (x0 < 1 || y0 < 1 || x0 + 3 >= int(lot.width) || y0 + 3 >= int(lot.height))
                                continue;
                            const std::string area = std::to_string(x0) + "," + std::to_string(y0) + "," +
                                                     std::to_string(x0 + 3) + "," + std::to_string(y0 + 3);
                            build->runSteps({"", "--room", area});
                            if (lotRebuilds > rebuildsBefore) {
                                drawn = area;
                                camera.setPivot(simd_make_float3(float(x0) + 2.f, float(y0) + 2.f, camera.pivot().z));
                            }
                        }
                roomLine = drawn.empty() ? "no room drawn"
                                         : "room " + drawn + "; walls " + std::to_string(wallsBefore) + " -> " +
                                               std::to_string(lot.walls.size()) + "; lot rebuilt " +
                                               std::to_string(lotRebuilds - rebuildsBefore);
                for (int i = 0; i < 5; ++i)
                    frame();
                camera.snap();
                showLiveUi();
                renderer.setEye(camera.eye());
                renderer.setView(camera.view());
                renderer.capture(shot.parent_path() / "frame_build.png", viewProj());
            }
            buildLine = std::string(buildClick ? "clicked" : "miss") + (build->active() ? " active" : " inactive") +
                        " panel " + std::to_string(buildWindows) + " windows " + std::to_string(buildDrawn) +
                        " drawn; " + roomLine;
            std::printf("opensims2: build %s\n", buildLine.c_str());
        }
        log << "build " << buildLine << "\n";
        log << "movie " << movieLabel << " " << movieW << "x" << movieH << " frame " << movieFrame << "\n";
        log << "particles " << renderer.particleCount() << " effect " << effectName << "\n";
        log << "audio " << (gameAudio.hasDevice() ? "on" : "off") << " frames " << gameAudio.mixer().framesRendered()
            << " voices " << gameAudio.mixer().voiceCount() << "\n";
        log << lotAudio.summary() << "\n";
        if (os2::haveMp3Decoder()) {
            try {
                bool found = false;
                for (const auto& loc : index.ofType(os2::type::AUDIO)) {
                    const auto bytes = index.read(loc);
                    if (os2::detectAudioFormat(bytes) != os2::AudioFormat::Mp3)
                        continue;
                    const os2::Pcm pcm = os2::decodeMp3(bytes);
                    std::printf("opensims2: mp3 %u Hz %u ch %.2fs peak %d\n", pcm.sampleRate, pcm.channels, pcm.seconds(),
                                pcm.peak());
                    log << "mp3 " << pcm.sampleRate << " Hz " << pcm.channels << " ch " << pcm.seconds() << " s peak "
                        << pcm.peak() << "\n";
                    found = true;
                    break;
                }
                if (!found)
                    std::fprintf(stderr, "opensims2: no MP3 resource\n");
            } catch (const std::exception& e) {
                std::fprintf(stderr, "opensims2: mp3: %s\n", e.what());
            }
        } else {
            std::fprintf(stderr, "opensims2: MP3 decoder unavailable\n");
        }
        log << "objects drawn " << objects << "\n";
        log << "sims drawn " << sims.size() << "\n";
        for (const auto& [id, object] : simulation.machine().objects()) {
            if (!object.person)
                continue;
            log << "sim " << id << " " << (object.definition ? object.definition->fileName : "?") << " at " << object.position.x
                << " " << object.position.y << " anim '" << object.animation.name << "'\n";
        }
        log.close();
        std::printf("opensims2: wrote %s (%zu objects, %zu Sims, %llu ticks)\n", shot.string().c_str(), objects, sims.size(),
                    (unsigned long long)simulation.ticks());
        if (exitAfterShot)
            return {};

        SDL_StartTextInput(app.window);
        bool leftDown = false, rotating = false;
        // The cursor for what is under the mouse (the game's arrow over the
        // UI, Buy mode's hands over the lot), as the Mac host's refreshGameCursor.
        auto refreshCursor = [&](simd_float2 p) {
            using Ctx = os2::CursorContext;
            Ctx c;
            c.mode = buy && buy->active()       ? Ctx::Mode::Buy
                     : build && build->active() ? Ctx::Mode::Build
                     : liveUi                   ? Ctx::Mode::Live
                                                : Ctx::Mode::Other;
            c.overUi |= pie && (pie->isOpen() || pie->dialogOpen());
            c.overUi |= liveUi && liveUi->overPanel(p);
            if (buy && buy->active()) {
                c.overUi |= buy->overPanel(p);
                c.buyTool = Ctx::BuyTool(int(buy->tool()));
                c.holding = buy->held() != 0;
                c.pressing = buy->pressing();
                c.turned = buy->turned();
                c.overObject = buy->overObject();
            }
            setGameCursor(app.res, os2::chooseCursor(c));
        };
        refreshCursor(simd_make_float2(-1, -1));
        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT)
                    running = false;
                // An open dialog with a text box (naming a baby) takes the keys.
                if (pie && pie->dialogOpen() && pie->dialogHasTextBox()) {
                    if (event.type == SDL_EVENT_TEXT_INPUT) {
                        pie->dialogKey(0xFFFF, event.text.text);
                        continue;
                    }
                    if (event.type == SDL_EVENT_KEY_DOWN && macKeyCode(event.key.scancode) != 0xFFFF) {
                        pie->dialogKey(macKeyCode(event.key.scancode), "");
                        continue;
                    }
                }
                if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
                    const bool down = event.type == SDL_EVENT_KEY_DOWN;
                    switch (event.key.scancode) {
                    case SDL_SCANCODE_A: case SDL_SCANCODE_LEFT: keys[0] = down; break;
                    case SDL_SCANCODE_D: case SDL_SCANCODE_RIGHT: keys[1] = down; break;
                    case SDL_SCANCODE_S: case SDL_SCANCODE_DOWN: keys[2] = down; break;
                    case SDL_SCANCODE_W: case SDL_SCANCODE_UP: keys[3] = down; break;
                    case SDL_SCANCODE_EQUALS: keys[4] = down; break;
                    case SDL_SCANCODE_MINUS: keys[5] = down; break;
                    case SDL_SCANCODE_COMMA: case SDL_SCANCODE_Q: keys[6] = down; break;
                    case SDL_SCANCODE_PERIOD: case SDL_SCANCODE_E: keys[7] = down; break;
                    case SDL_SCANCODE_Z: case SDL_SCANCODE_KP_PLUS: if (down) camera.zoomBy(1); break;
                    case SDL_SCANCODE_X: case SDL_SCANCODE_KP_MINUS: if (down) camera.zoomBy(-1); break;
                    case SDL_SCANCODE_PAGEUP: if (down) changeFloor(1); break;
                    case SDL_SCANCODE_PAGEDOWN: if (down) changeFloor(-1); break;
                    // "Paredes ... - Origem/Fim": Home and End step through the wall modes.
                    case SDL_SCANCODE_HOME: if (down) wallView.cycle(1); break;
                    case SDL_SCANCODE_END: if (down) wallView.cycle(-1); break;
                    case SDL_SCANCODE_SPACE: if (down) selectNextFamilySim(); break;
                    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER:
                        if (down && pie)
                            if (auto p = follow.simPoint(simulation.machine(), pie->sim()))
                                camera.moveTo(*p);
                        break;
                    case SDL_SCANCODE_ESCAPE:
                        if (down)
                            escape();
                        break;
                    // The speed buttons' accelerators (their tips: "Pausa - P",
                    // "Velocidade Normal - 1", ...), as the Mac host's.
                    case SDL_SCANCODE_P: if (down && liveUi && liveUi->ok()) liveUi->togglePause(); break;
                    case SDL_SCANCODE_1: if (down && liveUi && liveUi->ok()) liveUi->setSpeed(2); break;
                    case SDL_SCANCODE_2: if (down && liveUi && liveUi->ok()) liveUi->setSpeed(3); break;
                    case SDL_SCANCODE_3: if (down && liveUi && liveUi->ok()) liveUi->setSpeed(4); break;
                    default: break;
                    }
                }
                if (event.type == SDL_EVENT_MOUSE_WHEEL)
                    camera.wheel(event.wheel.y);
                if (event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE)
                    cameraMouse.mouseAt(0, 0, false);
                if (event.type == SDL_EVENT_MOUSE_MOTION) {
                    if (rotating) {
                        camera.rotateRequest(event.motion.xrel, event.motion.yrel);
                    } else {
                        cameraMouse.mouseAt(event.motion.x, event.motion.y);
                        mouseMoved(simd_make_float2(event.motion.x, event.motion.y), leftDown);
                    }
                }
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    const SDL_Keymod mods = SDL_GetModState();
                    const bool control = (mods & SDL_KMOD_CTRL) != 0, alt = (mods & SDL_KMOD_ALT) != 0;
                    const simd_float2 p = simd_make_float2(event.button.x, event.button.y);
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        leftDown = true;
                        mouseDown(p, false, false, control, (mods & SDL_KMOD_SHIFT) != 0);
                    } else if (event.button.button == SDL_BUTTON_RIGHT) {
                        rightOnWorld = false;
                        mouseDown(p, true, false, control, (mods & SDL_KMOD_SHIFT) != 0);
                        // Control (the game's "CTRL + botão DIREITO") or Alt: rotate and tilt; else drag the view.
                        if (rightOnWorld) {
                            rotating = os2::CameraMouseInput::rotatePress(os2::CameraMouseInput::Button::Right, control, alt);
                            if (!rotating)
                                cameraMouse.beginAnchor();
                        }
                    } else if (event.button.button == SDL_BUTTON_MIDDLE) {
                        rotating = true;
                    }
                    // While rotating the cursor stays where it was clicked; the deltas keep coming.
                    if (rotating)
                        SDL_SetWindowRelativeMouseMode(app.window, true);
                }
                if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        leftDown = false;
                        mouseUp(simd_make_float2(event.button.x, event.button.y));
                    } else if (event.button.button == SDL_BUTTON_RIGHT || event.button.button == SDL_BUTTON_MIDDLE) {
                        cameraMouse.endAnchor();
                        if (rotating)
                            SDL_SetWindowRelativeMouseMode(app.window, false);
                        rotating = false;
                    }
                }
            }
            pollFlowDialog();
            if (!rotating) {
                float mx = 0, my = 0;
                SDL_GetMouseState(&mx, &my);
                refreshCursor(simd_make_float2(mx, my));
            }
            showLiveUi();
            if (!frame())
                running = false;
        }
        return exit;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: %s\n", e.what());
        SceneExit failed;
        failed.code = 1;
        return failed;
    }
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(crashFilter);
#endif
    App app;
    if (argc >= 2) {
        app.res = argv[1];
    } else if (auto game = findGameDir(exeDir())) {
        app.res = *game / "TSData" / "Res"; // no scene flag: the main menu
    } else {
        std::fprintf(stderr, "usage: opensims2_win <TSData/Res> [lot.package] [--menu | --hood N001 [--hood-lot id] | --cas] "
                             "[--screenshot file.png]\n");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "opensims2",
                                 "The Sims 2 game data was not found.\n\n"
                                 "Put the opensims2 folder inside your The Sims 2 folder (the one that contains TSData), "
                                 "or start it with the data's path:\n"
                                 "opensims2_win.exe \"C:\\...\\The Sims 2\\TSData\\Res\"",
                                 nullptr);
        return 2;
    }
    app.gameDir = app.res.parent_path().parent_path();
    app.exe = exeDir();
    app.args.push_back(argv[0]);
    std::optional<std::filesystem::path> lotFile; // the old form: a lot package as the second argument
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (i == 2 && !a.starts_with("--"))
            lotFile = a;
        else
            app.args.push_back(a);
    }
    if (auto shot = app.argument("--screenshot"))
        app.shot = *shot;
    // --movie <name> --screenshot out.png [--frames n] [--time s] [--sound-wav mix.wav]: the movie's
    // frames headless (app/movie_host.hpp); without --screenshot it plays in the window.
    if (auto movie = app.argument("--movie"); movie && app.shot)
        return os2::movieScreenshots(app.res, *movie, app.shot->string(), std::stoi(app.argument("--frames").value_or("1")),
                                     std::stod(app.argument("--time").value_or("0")), 1280, 720,
                                     app.argument("--sound-wav").value_or(""));
    try {
        // The user's folder (app/user_data.hpp): --user-dir, %OS2_USER_DIR%, else %LOCALAPPDATA%\opensims2.
        std::filesystem::path user = os2::defaultUserDir();
        if (const char* env = std::getenv("OS2_USER_DIR"))
            user = env;
        if (auto dir = app.argument("--user-dir"))
            user = *dir;
        app.user = std::make_unique<os2::UserData>(app.gameDir, user);
        app.settings = os2::GameSettings::load(*app.user);
        app.settings.applyGraphics();
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            std::fprintf(stderr, "opensims2: SDL: %s\n", SDL_GetError());
            return 1;
        }
        // One window for every scene; the UI canvas is its size, so it is not resized.
        app.window = SDL_CreateWindow("opensims2", 1280, 720, 0);
        if (!app.window) {
            std::fprintf(stderr, "opensims2: window: %s\n", SDL_GetError());
            return 1;
        }
        os2::movieHostInit({app.res, std::find(app.args.begin(), app.args.end(), "--no-audio") == app.args.end()});
        if (auto movie = app.argument("--movie")) {
            os2::Renderer renderer(app.exe / "shaders", 1280, 720, app.window);
            os2::playMovie(*movie);
            playMoviesInWindow(renderer);
            SDL_DestroyWindow(app.window);
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return 0;
        }
        int code = 0;
        if (lotFile) {
            setGameCursor(app.res, os2::GameCursor::Hourglass);
            code = runLot(app, *lotFile, os2::Scene{}).code;
        } else {
            os2::Scene scene = os2::sceneOf(app.args);
            if (scene.kind == os2::Scene::Kind::Other)
                scene.kind = os2::Scene::Kind::MainMenu;
            for (;;) {
                std::printf("opensims2: scene %s\n", scene.kind == os2::Scene::Kind::MainMenu       ? "main menu"
                                                     : scene.kind == os2::Scene::Kind::Neighborhood ? "neighborhood"
                                                     : scene.kind == os2::Scene::Kind::Lot          ? "lot"
                                                                                                    : "cas");
                std::fflush(stdout);
                setGameCursor(app.res, os2::GameCursor::Hourglass); // each scene's loop puts the arrow back
                SceneExit r;
                switch (scene.kind) {
                case os2::Scene::Kind::Neighborhood: r = runHood(app, scene); break;
                case os2::Scene::Kind::Cas: r = runCas(app, scene); break;
                case os2::Scene::Kind::Lot:
                    r = runLot(app, os2::hoodLotPackage(app.res, app.hoodFolder(scene.hood).string(), scene.lot), scene);
                    break;
                default: r = runMenu(app, scene); break;
                }
                code = r.code;
                if (!r.next)
                    break;
                app.args = os2::sceneArguments(app.args, *r.next, r.stepsUsed);
                scene = *r.next;
            }
        }
        SDL_DestroyWindow(app.window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return code;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: %s\n", e.what());
        return 1;
    }
}
