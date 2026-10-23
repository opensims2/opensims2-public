#pragma once
// The game's data for a command that runs a lot: the Res tree of every pack of
// the installation, which is what the app indexes (src/app/win_main.cpp,
// installationResRoots). `res` goes first, so a resource it has still wins over
// another pack's; the other packs only fill in what it lacks. With a single
// root the AL pack alone ran without the base game's and the other EPs'
// animations ("a-heatstroke-...", even "o-door-open-CCW" were missing), and the
// wait loops that depend on an animation's length spun into the instruction
// budget (docs/performance.md).
#include "formats/resindex.hpp"

#include <filesystem>

inline void s2pkgAddInstallation(os2::ResourceIndex& index, const std::filesystem::path& res) {
    for (const auto& root : os2::installationResRoots(res))
        index.addRoot(root);
}
