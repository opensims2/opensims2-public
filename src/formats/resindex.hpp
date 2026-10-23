#pragma once

#include "dbpf.hpp"
#include "locale.hpp"

#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace os2 {

// Scenegraph resources are found by name: group 0x1C0532FA (or the group
// encoded as "##0xGGGGGGGG!name"), instance = CRC24(name)|0xFF000000,
// instanceHi = CRC32(name), over the lowercased name.
constexpr uint32_t kLocalGroup = 0x1C0532FA;
uint32_t nameInstanceHash(std::string_view name);
uint32_t nameInstanceHiHash(std::string_view name);

// Every resource of every .package under a directory (normally TSData/Res),
// keyed by TGI. Packages keep their index in memory; file handles are opened
// on demand and only a few stay open at a time.
class ResourceIndex {
public:
    struct Location {
        DbpfPackage* package = nullptr;
        const DbpfEntry* entry = nullptr;
    };

    // Every package under `root`. When `root` has a Locale folder (Res), only
    // that of `locale` is indexed, and its packages come first, so they
    // replace the base game's (as the DBSegments of Sys/default.ini put
    // Res\Locale\%Language%\UI\ui.package over Res\UI\ui.package); the
    // other locales are left out (they used to win by sorting first: Czech's
    // ui.package, Dutch's Sims3D/Objects.package).
    explicit ResourceIndex(const std::filesystem::path& root, const std::string& locale = defaultLocale());
    // An index filled by later addRoot calls.
    ResourceIndex() = default;
    // Adds another root after the ones already indexed: what was added first
    // keeps winning duplicate TGIs. installationResRoots() returns an
    // installation's roots, the base game's first, each with the same Locale
    // treatment as the constructor's.
    void addRoot(const std::filesystem::path& root, const std::string& locale = defaultLocale());
    // An overlay: the resources of `packages`, then (for find/findByName)
    // those of `fallback`, e.g. a Sim's character file over the game's.
    ResourceIndex(const std::vector<std::filesystem::path>& packages, const ResourceIndex* fallback);

    size_t packageCount() const { return packages_.size(); }
    // The locale indexed (empty for a root without a Locale folder or an overlay).
    const std::string& locale() const { return locale_; }
    size_t resourceCount() const { return byTgi_.size(); }
    size_t duplicateCount() const { return duplicates_; }

    std::optional<Location> find(const Tgi& tgi) const;
    std::vector<uint8_t> read(const Location& loc);
    // read() that is safe from several threads at once (each call opens the
    // file itself): for loaders that decode many resources in parallel.
    static std::vector<uint8_t> readConcurrent(const Location& loc) { return loc.package->readConcurrent(*loc.entry); }
    std::optional<std::vector<uint8_t>> read(const Tgi& tgi);

    // Scenegraph lookup by resource name ("foo_txtr"; "##0x1C050000!foo_txtr" selects the group,
    // "#0x7F861BC1!foo_cres" is a resource of the same package, group 0xFFFFFFFF).
    std::optional<Location> findByName(std::string_view name, uint32_t type) const;

    // All resources of one type, in package order.
    std::vector<Location> ofType(uint32_t type) const;

private:
    std::vector<std::unique_ptr<DbpfPackage>> packages_;
    std::unordered_map<Tgi, Location, TgiHash> byTgi_;
    std::deque<DbpfPackage*> open_;
    const ResourceIndex* fallback_ = nullptr;
    void addPackages(std::vector<std::filesystem::path> paths);
    size_t duplicates_ = 0;
    std::string locale_;
};

// The TSData/Res trees of the installation `res` belongs to: `res` itself and
// its siblings, found walking up (the Ultimate Collection's grouped folders:
// <root>\Double Deluxe\Base\TSData\Res, <root>\Best of Business\EP3\TSData\
// Res, <root>\Seasons\TSData\Res, ...). `res` comes first so it wins
// duplicates, the siblings after it sorted; a `res` without siblings (a plain
// single-pack install) gives just {res}.
std::vector<std::filesystem::path> installationResRoots(const std::filesystem::path& res);

} // namespace os2
