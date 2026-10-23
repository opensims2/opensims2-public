#include "resindex.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <memory>
#include <thread>
#include <array>
#include <cctype>
#include <string>

namespace os2 {

namespace {

constexpr size_t kMaxOpenFiles = 128; // under the 256 a macOS app starts with

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// OpenPGP CRC-24 (poly 0x864CFB, init 0xB704CE), a byte at a time from a table
// built from the bitwise form.
const std::array<uint32_t, 256>& crc24Table() {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t b = 0; b < 256; ++b) {
            uint32_t crc = b << 16;
            for (int i = 0; i < 8; ++i) {
                crc <<= 1;
                if (crc & 0x1000000)
                    crc ^= 0x864CFB;
            }
            t[b] = crc & 0xFFFFFF;
        }
        return t;
    }();
    return table;
}

uint32_t crc24Lower(std::string_view s) {
    const auto& t = crc24Table();
    uint32_t crc = 0xB704CE;
    for (char c : s) {
        const unsigned char l = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(c)));
        crc = ((crc << 8) & 0xFFFFFF) ^ t[((crc >> 16) ^ l) & 0xFF];
    }
    return crc & 0xFFFFFF;
}

// MSB-first CRC-32 (poly 0x04C11DB7, init 0xFFFFFFFF, no final xor), likewise.
const std::array<uint32_t, 256>& crc32Table() {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t b = 0; b < 256; ++b) {
            uint32_t crc = b << 24;
            for (int i = 0; i < 8; ++i)
                crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : crc << 1;
            t[b] = crc;
        }
        return t;
    }();
    return table;
}

uint32_t crc32Lower(std::string_view s) {
    const auto& t = crc32Table();
    uint32_t crc = 0xFFFFFFFF;
    for (char c : s) {
        const unsigned char l = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(c)));
        crc = (crc << 8) ^ t[((crc >> 24) ^ l) & 0xFF];
    }
    return crc;
}

} // namespace

uint32_t nameInstanceHash(std::string_view name) { return crc24Lower(name) | 0xFF000000; }
uint32_t nameInstanceHiHash(std::string_view name) { return crc32Lower(name); }

ResourceIndex::ResourceIndex(const std::filesystem::path& root, const std::string& locale) { addRoot(root, locale); }

void ResourceIndex::addRoot(const std::filesystem::path& root, const std::string& locale) {
    const auto localeDir = root / "Locale";
    const bool localized = std::filesystem::is_directory(localeDir);
    if (localized && locale_.empty())
        locale_ = locale;
    std::vector<std::filesystem::path> paths, localePaths;
    for (auto it = std::filesystem::recursive_directory_iterator(root); it != std::filesystem::recursive_directory_iterator();
         ++it) {
        const auto& de = *it;
        if (localized && de.is_directory() && de.path().parent_path() == localeDir) {
            if (de.path().filename() != locale)
                it.disable_recursion_pending();
            continue;
        }
        if (!de.is_regular_file())
            continue;
        auto ext = lower(de.path().extension().string());
        if (ext != ".package")
            continue;
        const bool inLocale = localized && de.path().lexically_relative(localeDir).begin()->string() != "..";
        (inLocale ? localePaths : paths).push_back(de.path());
    }
    addPackages(std::move(localePaths));
    addPackages(std::move(paths));
}

ResourceIndex::ResourceIndex(const std::vector<std::filesystem::path>& packages, const ResourceIndex* fallback)
    : fallback_(fallback) {
    addPackages(packages);
}

void ResourceIndex::addPackages(std::vector<std::filesystem::path> paths) {
    std::sort(paths.begin(), paths.end());
    // The packages' headers and indexes are read in parallel (hundreds of
    // files); they are added in path order, so the first of two duplicates
    // is the same one as when reading them one after the other.
    std::vector<std::unique_ptr<DbpfPackage>> built(paths.size());
    std::vector<std::exception_ptr> failed(paths.size());
    std::atomic<size_t> next{0};
    auto work = [&] {
        for (size_t i; (i = next++) < paths.size();) {
            try {
                built[i] = std::make_unique<DbpfPackage>(paths[i]);
                built[i]->closeFile();
            } catch (...) { // any type: rethrown below on the calling thread (one escaping a worker terminates)
                failed[i] = std::current_exception();
            }
        }
    };
    const size_t threads = paths.size() < 8 ? 0 : std::min<size_t>(8, std::max(1u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (size_t t = 1; t < threads; ++t)
        pool.emplace_back(work);
    work();
    for (auto& t : pool)
        t.join();
    for (size_t i = 0; i < paths.size(); ++i) {
        if (failed[i])
            std::rethrow_exception(failed[i]);
        auto pkg = std::move(built[i]);
        for (const auto& e : pkg->entries()) {
            if (e.tgi.type == type::DIR)
                continue;
            if (!byTgi_.emplace(e.tgi, Location{pkg.get(), &e}).second)
                ++duplicates_;
        }
        packages_.push_back(std::move(pkg));
    }
}

std::optional<ResourceIndex::Location> ResourceIndex::find(const Tgi& tgi) const {
    auto it = byTgi_.find(tgi);
    if (it == byTgi_.end())
        return fallback_ ? fallback_->find(tgi) : std::nullopt;
    return it->second;
}

std::vector<uint8_t> ResourceIndex::read(const Location& loc) {
    if (!loc.package->fileOpen()) {
        if (open_.size() >= kMaxOpenFiles) {
            open_.front()->closeFile();
            open_.pop_front();
        }
        open_.push_back(loc.package);
    }
    return loc.package->read(*loc.entry);
}

std::optional<std::vector<uint8_t>> ResourceIndex::read(const Tgi& tgi) {
    auto loc = find(tgi);
    if (!loc)
        return std::nullopt;
    return read(*loc);
}

std::optional<ResourceIndex::Location> ResourceIndex::findByName(std::string_view name, uint32_t type) const {
    uint32_t group = kLocalGroup;
    if (name.starts_with("#0x")) {
        // "#0xGGGGGGGG!name": a resource of the same package (character
        // files), stored with group 0xFFFFFFFF and hashed without the prefix.
        if (auto bang = name.find('!'); bang != std::string_view::npos) {
            name = name.substr(bang + 1);
            group = 0xFFFFFFFF;
        }
    } else if (name.starts_with("##0x")) {
        if (auto bang = name.find('!'); bang != std::string_view::npos) {
            group = uint32_t(std::stoul(std::string(name.substr(4, bang - 4)), nullptr, 16));
            name = name.substr(bang + 1);
        }
    }
    Tgi k{type, group, nameInstanceHash(name), nameInstanceHiHash(name)};
    if (auto loc = find(k))
        return loc;
    k.instanceHi = 0; // older resources leave the high instance empty
    if (auto loc = find(k))
        return loc;
    // "Scope!name" (a lot's imposter: "N001_Lot11!imposter_gmnd"): a resource
    // of the same package, group 0xFFFFFFFF, hashed without the scope.
    if (group == kLocalGroup && !name.starts_with("#"))
        if (auto bang = name.find('!'); bang != std::string_view::npos && bang + 1 < name.size())
            return findByName("#0xFFFFFFFF!" + std::string(name.substr(bang + 1)), type);
    return std::nullopt;
}

std::vector<ResourceIndex::Location> ResourceIndex::ofType(uint32_t type) const {
    std::vector<Location> out;
    for (const auto& pkg : packages_)
        for (const auto& e : pkg->entries())
            if (e.tgi.type == type)
                out.push_back(Location{pkg.get(), &e});
    return out;
}

namespace {

// The TSData/Res trees within `depth` levels under `dir` (never descending
// into a TSData: that is what marks a root).
void collectResRoots(const std::filesystem::path& dir, int depth, std::vector<std::filesystem::path>& out) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code de;
        if (!it->is_directory(de))
            continue;
        const std::filesystem::path path = it->path();
        if (path.filename() == "TSData") {
            if (std::filesystem::is_directory(path / "Res", de))
                out.push_back(path / "Res");
        } else if (depth > 1) {
            collectResRoots(path, depth - 1, out);
        }
    }
}

} // namespace

std::vector<std::filesystem::path> installationResRoots(const std::filesystem::path& res) {
    const std::filesystem::path first = res.lexically_normal();
    std::vector<std::filesystem::path> best{first};
    std::filesystem::path dir = first;
    // Up to four levels up; the highest ancestor that finds at least one root
    // beside `res` wins (the Ultimate Collection's root: four up from the
    // base game's Res, where its collections and packs sit).
    for (int up = 0; up < 4 && dir.has_parent_path() && dir.parent_path() != dir; ++up) {
        dir = dir.parent_path();
        std::vector<std::filesystem::path> found;
        collectResRoots(dir, 4, found);
        if (found.size() >= 2 && found.size() >= best.size())
            best = std::move(found);
    }
    std::vector<std::filesystem::path> out{first};
    for (auto& p : best)
        if (p != first)
            out.push_back(std::move(p));
    std::sort(out.begin() + 1, out.end());
    return out;
}

} // namespace os2
