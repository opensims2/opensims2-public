#include "memories.hpp"
#include "engine.hpp"
#include "formats/log_once.hpp"
#include "errors.hpp"
#include "vm.hpp"
#include "placement.hpp"
#include "rooms.hpp"

#include "hood_bridge.hpp"
#include "interactions.hpp"
#include "go_here.hpp"
#include "kill.hpp"

#include "formats/binreader.hpp"
#include "formats/dbpf.hpp"
#include "formats/resindex.hpp"
#include "formats/strings.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace os2::vm {

namespace {

uint64_t key(uint32_t group, uint16_t instance) { return uint64_t(group) << 32 | instance; }

template <class T>
const T* loadCached(std::unordered_map<uint64_t, std::unique_ptr<T>>& cache, ResourceIndex* index, uint32_t type,
                    uint32_t group, uint16_t instance) {
    const auto k = key(group, instance);
    if (auto it = cache.find(k); it != cache.end())
        return it->second.get();
    std::unique_ptr<T> value;
    if (index)
        if (auto data = index->read(Tgi{type, group, instance, 0}))
            value = std::make_unique<T>(T::parse(*data));
    return cache.emplace(k, std::move(value)).first->second.get();
}

std::string treeName(const Frame& f) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "0x%08X/0x%04X", f.group, f.instance);
    return std::string(buf) + " \"" + f.behavior->fileName + "\"";
}

template <class V>
auto& slot(V& values, int idx, const char* what) {
    if (idx < 0 || size_t(idx) >= values.size())
        throw VMError(std::string(what) + " " + std::to_string(idx) + " out of range (" +
                      std::to_string(values.size()) + ")");
    return values[size_t(idx)];
}

// Temps: 0-7, else error 0x02 (owners 8, 0x0B, 0x0D, 0x1E, 0x1F, 0x30, 0x31).
static int16_t& temp(std::array<int16_t, kTempCount>& temps, int idx, const char* what) {
    if (idx < 0 || size_t(idx) >= temps.size())
        throw gameError(err::TempOutOfRange, std::string(what) + " " + std::to_string(idx));
    return temps[size_t(idx)];
}

// Attributes and semi-attributes: an index past the object's count is error
// 0x06 (InterpValue, BV 0xb15f2c owners 0, 1, 2, 5, 0x16, 0x32).
int16_t& attribute(std::vector<int16_t>& values, int idx, const char* what) {
    if (idx < 0 || size_t(idx) >= values.size())
        throw gameError(err::AttributeOutOfRange,
                        std::string(what) + " " + std::to_string(idx) + " of " + std::to_string(values.size()));
    return values[size_t(idx)];
}

// An object array's element by index: outside the elements, error 0x6D.
static int16_t& element(std::vector<int16_t>& values, int idx, const char* what) {
    if (idx < 0 || size_t(idx) >= values.size())
        throw gameError(err::NoArrayElement, std::string(what) + " " + std::to_string(idx) + " of " + std::to_string(values.size()));
    return values[size_t(idx)];
}

// Type attributes (owners 0x24/0x25): a fixed count, an index past it is
// error 0x40 (InterpValue BV 0xb15f2c; no base tree uses them).
static int16_t& typeAttribute(std::vector<int16_t>& values, int idx, const char* what) {
    if (idx < 0 || size_t(idx) >= values.size())
        throw gameError(err::TypeAttributeOutOfRange,
                        std::string(what) + " " + std::to_string(idx) + " of " + std::to_string(values.size()));
    return values[size_t(idx)];
}

Person& personOf(Object& o) {
    if (!o.person)
        throw VMError("object " + std::to_string(o.id) + " is not a person");
    return *o.person;
}

// Field of the editor's OBJD numbering: 0-1 are the u32 version, i >= 2 the
// field at file offset 0x40 + 2i.
int16_t definitionField(const ObjectDefinition& d, int idx) {
    if (idx < 0)
        throw VMError("definition field " + std::to_string(idx) + " out of range");
    if (idx < 2)
        return int16_t(d.version >> (16 * idx));
    return int16_t(d.field(0x40 + 2 * size_t(idx)));
}

int16_t definitionField(const Object& o, int idx) {
    if (!o.definition)
        throw VMError("object " + std::to_string(o.id) + " has no definition");
    return definitionField(*o.definition, idx);
}

} // namespace

uint32_t semiGlobalGroup(std::string_view name) { return (nameInstanceHash(name) & 0x00FFFFFF) | 0x7F000000; }

const Behavior* CodeLibrary::behavior(uint32_t group, uint16_t instance) {
    return loadCached(behaviors_, index_, kTypeBhav, group, instance);
}

const Constants* CodeLibrary::constants(uint32_t group, uint16_t instance) {
    return loadCached(constants_, index_, kTypeBcon, group, instance);
}

const ObjectDefinition* CodeLibrary::definition(uint32_t group, uint16_t instance) {
    return loadCached(definitions_, index_, kTypeObjd, group, instance);
}

const StringTable* CodeLibrary::strings(uint32_t group, uint16_t instance) {
    return loadCached(strings_, index_, kTypeStr, group, instance);
}

const Slots* CodeLibrary::slots(uint32_t group, uint16_t instance) {
    return loadCached(slots_, index_, kTypeSlot, group, instance);
}

const InteractionTable* CodeLibrary::interactions(uint32_t group, uint16_t instance) {
    try {
        return loadCached(interactions_, index_, kTypeTtab, group, instance);
    } catch (const std::exception& err) {
        os2::logOnce("interactions (TTAB)", std::to_string(group) + ":" + std::to_string(instance), err);
        // A few TTABs carry bytes the parser does not explain: no interactions.
        return interactions_.emplace(key(group, instance), nullptr).first->second.get();
    }
}

const StringTable* CodeLibrary::interactionStrings(uint32_t group, uint16_t instance) {
    return loadCached(interactionStrings_, index_, kTypeTtas, group, instance);
}

void CodeLibrary::add(uint32_t group, uint16_t instance, InteractionTable t) {
    interactions_[key(group, instance)] = std::make_unique<InteractionTable>(std::move(t));
}

void CodeLibrary::addInteractionStrings(uint32_t group, uint16_t instance, StringTable t) {
    interactionStrings_[key(group, instance)] = std::make_unique<StringTable>(std::move(t));
}

void CodeLibrary::add(uint32_t group, uint16_t instance, Slots sl) {
    slots_[key(group, instance)] = std::make_unique<Slots>(std::move(sl));
}

std::optional<uint16_t> CodeLibrary::behaviorByName(uint32_t group, const std::string& name) {    if (!namesScanned_ && index_) {
        namesScanned_ = true;
        for (const auto& loc : index_->ofType(kTypeBhav)) {
            const auto& tgi = loc.entry->tgi;
            const auto data = index_->read(loc);
            // The file name is the first field, a NUL-padded 64-byte string.
            const auto end = std::find(data.begin(), data.begin() + std::min<size_t>(data.size(), 64), 0);
            byName_[tgi.group].emplace(std::string(data.begin(), end), uint16_t(tgi.instance));
        }
    }
    auto g = byName_.find(group);
    if (g == byName_.end())
        return std::nullopt;
    auto it = g->second.find(name);
    return it == g->second.end() ? std::nullopt : std::optional<uint16_t>(it->second);
}

// Group of the resources a package keeps for itself (Sims' character files,
// lots): the index cannot address them, since they all share it.
constexpr uint32_t kPackageLocalGroup = 0xFFFFFFFF;

void CodeLibrary::scanLuaScripts() {
    if (luaScanned_)
        return;
    luaScanned_ = true;
    if (!index_)
        return;
    // The ObjectScripts resources of the EPs: "u32 0, u32 length, the name, then
    // the Lua 5.0 chunk" (docs/lua-scripts.md). 0x9012468B are the .objLua
    // scripts, 0x9012468A the .globalObjLua preludes.
    std::vector<std::pair<std::string, std::vector<uint8_t>>> preludes;
    for (const uint32_t type : {0x9012468Bu, 0x9012468Au}) {
        for (const auto& loc : index_->ofType(type)) {
            std::vector<uint8_t> bytes;
            try {
                bytes = index_->read(loc);
            } catch (const std::exception&) {
                continue;
            }
            static const uint8_t signature[4] = {0x1B, 'L', 'u', 'a'};
            const auto at = std::search(bytes.begin(), bytes.end(), signature, signature + 4);
            if (at == bytes.end() || at - bytes.begin() < 8)
                continue;
            std::string name(bytes.begin() + 8, at);
            if (name.empty())
                continue;
            std::vector<uint8_t> chunk(at, bytes.end());
            if (type == 0x9012468Au)
                preludes.emplace_back(std::move(name), std::move(chunk));
            else
                luaScripts_.emplace(std::move(name), std::move(chunk)); // the first of two wins, as everywhere
        }
    }
    // The preludes: Global first (the base classes and the scopes), then by name.
    std::sort(preludes.begin(), preludes.end(), [](const auto& a, const auto& b) {
        if ((a.first == "Global") != (b.first == "Global"))
            return a.first == "Global";
        return a.first < b.first;
    });
    for (auto& p : preludes) {
        luaPreludeChunks_.push_back(std::move(p.second));
        luaPreludeNames_.push_back(std::move(p.first));
    }
}

const std::vector<uint8_t>* CodeLibrary::luaScript(const std::string& name) {
    scanLuaScripts();
    const auto it = luaScripts_.find(name);
    return it == luaScripts_.end() ? nullptr : &it->second;
}

const std::vector<std::vector<uint8_t>>& CodeLibrary::luaPreludes() {
    scanLuaScripts();
    return luaPreludeChunks_;
}

const std::vector<std::string>& CodeLibrary::luaPreludeNames() {
    scanLuaScripts();
    return luaPreludeNames_;
}

void CodeLibrary::indexDefinition(uint32_t group, uint16_t instance, const ObjectDefinition& d) {
    // Keeps the first OBJD of a GUID or name, like the rest of the index.
    if (byGuid_.emplace(d.guid(), std::pair{group, instance}).second) {
        // The definition list of Set to Next 38, in first-appearance order.
        // Fields 75 and 76 (offsets 0xD6 and 0xD8) are the selector category
        // and subcategory the Apartment Life content uses.
        definitionList_.push_back({d.guid(), int16_t(d.field(0xD6)), int16_t(d.field(0xD8))});
    }
    byFileName_.emplace(d.fileName, std::pair{group, instance});
    // OBJD field i is at file offset 0x40 + 2i: 9 "type" (0x52), 26 "Ignore
    // GlobalSim field in CAS Lot" (0x74), 42 "is global sim object" (0x94).
    constexpr uint16_t kTypeGlobalSim = 7;
    if (d.objectType() == kTypeGlobalSim && d.field(0x94) &&
        std::none_of(globalSims_.begin(), globalSims_.end(), [&](const auto& g) { return g.first.guid == d.guid(); }))
        globalSims_.push_back({{group, instance, d.guid(), d.field(0x74) != 0}, d.field(0x94)});
    if (d.masterId() && d.subIndex() == 0xFFFF)
        masters_.emplace(std::pair{group, d.masterId()}, instance);
    else if (d.masterId()) {
        auto& p = parts_[{group, d.masterId()}];
        if (std::find(p.begin(), p.end(), instance) == p.end()) {
            p.push_back(instance);
            std::sort(p.begin(), p.end());
        }
    }
}

void CodeLibrary::scanDefinitions() {
    if (guidsScanned_ || !index_)
        return;
    guidsScanned_ = true;
    for (const auto& loc : index_->ofType(kTypeObjd)) {
        const auto& tgi = loc.entry->tgi;
        if (tgi.group == kPackageLocalGroup)
            continue;
        indexDefinition(tgi.group, uint16_t(tgi.instance), ObjectDefinition::parse(index_->read(loc)));
    }
}

const std::vector<CodeLibrary::DefinitionRef>& CodeLibrary::definitions() {
    scanDefinitions();
    return definitionList_;
}

std::vector<uint16_t> CodeLibrary::parts(uint32_t group, uint16_t masterId) {
    scanDefinitions();
    auto it = parts_.find({group, masterId});
    return it == parts_.end() ? std::vector<uint16_t>{} : it->second;
}

std::optional<std::pair<uint32_t, uint16_t>> CodeLibrary::definitionByName(const std::string& fileName) {
    scanDefinitions();
    if (auto it = byFileName_.find(fileName); it != byFileName_.end())
        return it->second;
    return std::nullopt;
}

std::vector<uint16_t> CodeLibrary::addPackage(DbpfPackage& pkg, uint32_t group) {
    // Once per group: objects already made from it point at its definitions.
    if (auto it = packages_.find(group); it != packages_.end())
        return it->second;
    scanDefinitions(); // first, so the package's OBJDs take over their GUIDs
    std::vector<uint16_t> added;
    uint32_t semi = 0;
    for (const auto& e : pkg.entries()) {
        if (e.tgi.group != kPackageLocalGroup)
            continue;
        const auto instance = uint16_t(e.tgi.instance);
        switch (e.tgi.type) {
        case kTypeBhav: add(group, instance, Behavior::parse(pkg.read(e))); break;
        case kTypeBcon: add(group, instance, Constants::parse(pkg.read(e))); break;
        case kTypeStr: add(group, instance, StringTable::parse(pkg.read(e))); break;
        case kTypeObjf: add(group, instance, ObjectFunctions::parse(pkg.read(e))); break;
        case kTypeSlot: add(group, instance, Slots::parse(pkg.read(e))); break;
        case kTypeGlob: semi = semiGlobalGroup(SemiGlobal::parse(pkg.read(e)).semiGlobalName); break;
        case kTypeObjd: {
            auto d = ObjectDefinition::parse(pkg.read(e));
            const uint32_t guid = d.guid();
            // The package's definition takes over the GUID; the list keeps the
            // first-appearance place (the scan's) and takes the new fields.
            byGuid_[guid] = {group, instance};
            byFileName_[d.fileName] = {group, instance};
            bool listed = false;
            for (auto& r : definitionList_) {
                if (r.guid != guid)
                    continue;
                r.category = int16_t(d.field(0xD6));
                r.subcategory = int16_t(d.field(0xD8));
                listed = true;
                break;
            }
            if (!listed)
                definitionList_.push_back({guid, int16_t(d.field(0xD6)), int16_t(d.field(0xD8))});
            add(group, instance, std::move(d));
            added.push_back(instance);
            break;
        }
        default: break;
        }
    }
    setSemiGlobal(group, semi);
    size_t labels = 0;
    if (const StringTable* t = strings(group, 0x100))
        for (const auto& e : t->entries)
            labels += e.language == lang::EnglishUS;
    setAttributeLabelCount(group, labels);
    packages_[group] = added;
    return added;
}

std::optional<uint16_t> CodeLibrary::masterDefinition(uint32_t group, uint16_t masterId) {
    scanDefinitions();
    if (auto it = masters_.find({group, masterId}); it != masters_.end())
        return it->second;
    return std::nullopt;
}

const ObjectFunctions* CodeLibrary::functions(uint32_t group, uint16_t instance) {
    return loadCached(functions_, index_, kTypeObjf, group, instance);
}

void CodeLibrary::add(uint32_t group, uint16_t instance, ObjectFunctions f) {
    functions_[key(group, instance)] = std::make_unique<ObjectFunctions>(std::move(f));
}

std::vector<CodeLibrary::GlobalSimObject> CodeLibrary::globalSimObjects(uint16_t mask) {
    scanDefinitions();
    std::vector<GlobalSimObject> out;
    for (const auto& [g, field42] : globalSims_)
        if (field42 & mask)
            out.push_back(g);
    return out;
}

std::optional<std::pair<uint32_t, uint16_t>> CodeLibrary::definitionByGuid(uint32_t guid) {
    scanDefinitions();
    if (auto it = byGuid_.find(guid); it != byGuid_.end())
        return it->second;
    if (guidFallback_ && guid && guidFallbackTried_.insert(guid).second)
        return guidFallback_(*this, guid);
    return std::nullopt;
}

std::optional<std::pair<uint32_t, uint16_t>> CodeLibrary::characterDefinition(uint32_t guid) {
    return definitionByGuid(guid);
}

size_t CodeLibrary::attributeLabelCount(uint32_t group) {
    if (auto it = labelCounts_.find(group); it != labelCounts_.end())
        return it->second;
    size_t n = 0;
    if (index_)
        if (auto data = index_->read(Tgi{kTypeStr, group, 0x100, 0}))
            for (const auto& e : StringTable::parse(*data).entries)
                n += e.language == lang::EnglishUS;
    return labelCounts_[group] = n;
}

uint32_t CodeLibrary::semiGlobalFor(uint32_t group) {
    if (auto it = semiGlobals_.find(group); it != semiGlobals_.end())
        return it->second;
    uint32_t semi = 0;
    if (index_)
        for (uint32_t instance : {0x01u, 0x80u})
            if (auto data = index_->read(Tgi{kTypeGlob, group, instance, 0})) {
                semi = semiGlobalGroup(SemiGlobal::parse(*data).semiGlobalName);
                break;
            }
    semiGlobals_[group] = semi;
    return semi;
}

void CodeLibrary::add(uint32_t group, uint16_t instance, Behavior b) {
    byName_[group][b.fileName] = instance;
    behaviors_[key(group, instance)] = std::make_unique<Behavior>(std::move(b));
}

void CodeLibrary::add(uint32_t group, uint16_t instance, Constants c) {
    constants_[key(group, instance)] = std::make_unique<Constants>(std::move(c));
}

void CodeLibrary::add(uint32_t group, uint16_t instance, StringTable t) {
    strings_[key(group, instance)] = std::make_unique<StringTable>(std::move(t));
}

void CodeLibrary::add(uint32_t group, uint16_t instance, ObjectDefinition d) {
    indexDefinition(group, instance, d);
    definitions_[key(group, instance)] = std::make_unique<ObjectDefinition>(std::move(d));
}

bool CodeLibrary::renameDefinition(uint32_t group, uint16_t instance, const std::string& fileName) {
    auto it = definitions_.find(key(group, instance));
    if (it == definitions_.end() || !it->second)
        return false;
    it->second->fileName = fileName; // in place: the objects keep pointing at it
    byFileName_.emplace(fileName, std::pair{group, instance});
    return true;
}

Object::Array& Object::array(int idx) {
    if (idx < 0 || size_t(idx) >= arrays.size())
        throw gameError(err::BadObjectArray, "array " + std::to_string(idx) + " of " + std::to_string(arrays.size()));
    return arrays[size_t(idx)];
}

// --- Context ---------------------------------------------------------------

Object& Context::me() const {
    auto* o = vm.object(thread.me);
    if (!o)
        throw VMError("thread object " + std::to_string(thread.me) + " does not exist");
    return *o;
}

Object& Context::stackObject() const {
    auto* o = vm.object(frame.stackObject);
    if (!o)
        throw VMError("stack object " + std::to_string(frame.stackObject) + " does not exist");
    return *o;
}

// The neighbor whose ID is in the stack object (data owners 0x18, 0x20, 0x26);
// the message is the game's own (Sims2.exe).
static Neighbor& stackNeighbor(const Context& c) {
    Neighbor* n = c.vm.neighbor(c.frame.stackObject);
    if (!n)
        throw VMError("Missing neighbor for data access. (neighbor " + std::to_string(c.frame.stackObject) + ")");
    return *n;
}

// Person data of a neighbor: its Sim's while it is on the lot, else the neighborhood's copy.
static std::array<int16_t, kPersonDataCount>& neighborPersonData(const Context& c) {
    Neighbor& n = stackNeighbor(c);
    if (Object* sim = c.vm.neighborSim(c.frame.stackObject))
        return personOf(*sim).data;
    return n.personData;
}

// Neighbor in stack object (0x18), fields labelled in STR# 0xDD. Only those
// that do not need the neighborhood are implemented; "person age" is read
// from person data "Person Age" (inferred).
// A neighbor the neighborhood does not have reads 0, any field
// (cTSNeighborManager::GetNeighborData returns 0 without one; the binary's
// error 0x37 is only for owner 0x20, neighbor's person data). Lots keep IDs of
// neighbors deleted since (Controller - Chat on N001_Lot80: 144, 164, 170).
static int16_t neighborField(const Context& c, int16_t field) {
    if (!c.vm.neighbor(c.frame.stackObject))
        return 0;
    Neighbor& n = stackNeighbor(c);
    switch (field) {
    case 0: { // person instance id: the Sim's object, 0 when it is not on the lot
        const Object* sim = c.vm.neighborSim(c.frame.stackObject);
        return sim ? sim->id : 0;
    }
    case 2: return neighborPersonData(c)[persondata::PersonAge];
    case 6: return n.house;
    default: return neighborhoodNeighborField(c.vm, c.frame.stackObject, n, field);
    }
}

// Definition of a multi-tile object's master (the OBJD of its group with the
// same master ID and sub index -1); an object that is not multi-tile, or whose
// master is not found, is its own master (as FreeSO does).
static const ObjectDefinition& masterDefinitionOf(VM& vm, const Object& o) {
    if (!o.definition)
        throw VMError("object " + std::to_string(o.id) + " has no definition");
    if (const uint16_t master = o.definition->masterId())
        if (auto instance = vm.code().masterDefinition(o.group, master))
            if (const ObjectDefinition* d = vm.code().definition(o.group, *instance))
                return *d;
    return *o.definition;
}

// Type attributes of an object's type, as the object's selector finds them
// (cTSObjectSelector::CountTypeAttributes/GetTypeAttributes BV 0x51e5e8/
// 0x51e6c8): in the type attribute manager under its definition's proxy GUID
// (field 0xCD = editor field 29), or its GUID (0xCA = 14) without one. The
// manager has an entry for each catalog object whose "num type attributes"
// (field 90) is not 0, keyed by its GUID, with that many (cTSObjectFolder::
// PopulateTypeAttributesFromSelectors BV 0x4e4b0c); none: count 0.
static std::vector<int16_t>& typeAttributesOf(VM& vm, const Object& o) {
    if (!o.definition)
        throw VMError("object " + std::to_string(o.id) + " has no definition");
    const uint32_t key = o.definition->proxyGuid() ? o.definition->proxyGuid() : o.definition->guid();
    auto [it, added] = vm.typeAttributes.try_emplace(key);
    if (added) {
        const auto def = vm.code().definitionByGuid(key);
        const ObjectDefinition* d = def ? vm.code().definition(def->first, def->second) : nullptr;
        if (d)
            it->second.resize(uint16_t(definitionField(*d, 90)));
    }
    return it->second;
}

// A BCON of the code owner's, or error 0x2E ("Invalid constant.") without it.
static const Constants& constantsOf(VM& vm, const Object& owner, int scope, uint16_t table) {
    try {
        return vm.resolveConstants(owner, scope, table);
    } catch (const VMError& e) {
        throw gameError(err::InvalidConstant, e.what());
    }
}

// An entry of a BCON: outside it (or negative), error 0x2E.
static int16_t constantEntry(const Constants& c, int idx) {
    if (idx < 0 || size_t(idx) >= c.values.size())
        throw gameError(err::InvalidConstant, "entry " + std::to_string(idx) + " of " + std::to_string(c.values.size()));
    return c.values[size_t(idx)];
}

static int16_t& checkTreeAd(const Context& c, std::array<int16_t, kMotiveCount>& ads, int idx, const char* what) {
    if (!c.me().person)
        throw gameError(err::NotAPerson, what);
    if (idx < 0 || size_t(idx) >= ads.size())
        throw gameError(err::MotiveOutOfRange, std::string(what) + " " + std::to_string(idx));
    return ads[size_t(idx)];
}

// Storage behind a data owner, or null for the computed/read-only ones.
// Owners whose index comes from another variable read it first.
static int16_t* storage(const Context& c, uint8_t owner, int16_t data) {
    auto& t = c.thread;
    switch (owner) {
    case 0x00: return &attribute(c.me().attributes, data, "attribute");
    case 0x01: return &attribute(c.stackObject().attributes, data, "stack object attribute");
    case 0x02: return &attribute(c.me().semiAttributes, data, "semi-attribute");
    case 0x03: return &slot(c.me().data, data, "object data");
    case 0x04: return &slot(c.stackObject().data, data, "stack object data");
    case 0x05: return &attribute(c.stackObject().semiAttributes, data, "stack object semi-attribute");
    case 0x06: return &slot(c.vm.globals, data, "global");
    case 0x08: return &temp(t.temps, data, "temp");
    case 0x09: return &slot(c.frame.params, data, "param");
    case 0x0B: return &temp(t.temps, temp(t.temps, data, "temp"), "temp[temp]");
    case 0x0D: { // stack object's temps: the temps belong to the object in the game (cTSObject +0x8A),
                 // here to its thread: the running one if it is me, else its Main's
        const ObjectId so = c.stackObject().id;
        if (so == t.me)
            return &temp(t.temps, data, "stack object temp");
        auto it = c.vm.threads.find(so);
        if (it == c.vm.threads.end())
            throw VMError("stack object " + std::to_string(so) + " has no thread for its temps");
        return &temp(it->second.temps, data, "stack object temp");
    }
    // Check tree ads: only a Sim has them (else error 0x1C), 16 of each (a
    // motive number past them: error 0x1A; cTSPerson::InterpValue BV 0xad56ec).
    case 0x0C: return &checkTreeAd(c, t.adRange, data, "check tree ad range");
    case 0x1C: return &checkTreeAd(c, t.adPersonality, data, "check tree ad personality var");
    case 0x1D: return &checkTreeAd(c, t.adMin, data, "check tree ad min");
    case 0x0E: return &slot(personOf(c.me()).motives, data, "motive");
    case 0x0F: return &slot(personOf(c.stackObject()).motives, data, "stack object motive");
    case 0x11: return &slot(personOf(c.stackObject()).motives, temp(t.temps, data, "temp"), "stack object motive[temp]");
    case 0x12: return &slot(personOf(c.me()).data, data, "person data");
    case 0x13: return &slot(personOf(c.stackObject()).data, data, "stack object person data");
    case 0x16:
        return &attribute(c.stackObject().attributes, slot(c.frame.params, data, "param"), "stack object attribute[param]");
    case 0x19: return &slot(c.frame.locals, data, "local");
    case 0x24: return &typeAttribute(typeAttributesOf(c.vm, c.me()), data, "type attribute");
    case 0x25: return &typeAttribute(typeAttributesOf(c.vm, c.stackObject()), data, "stack object type attribute");
    case 0x20: return &slot(neighborPersonData(c), data, "neighbor's person data");
    case 0x1E: return &slot(personOf(c.me()).data, temp(t.temps, data, "temp"), "person data[temp]");
    case 0x1F:
        return &slot(personOf(c.stackObject()).data, temp(t.temps, data, "temp"), "stack object person data[temp]");
    case 0x32:
        return &attribute(c.stackObject().semiAttributes, slot(c.frame.params, data, "param"),
                        "stack object semi-attribute[param]");
    // Object arrays: `data` is the array (past the object's: error 0x6C); the
    // element is the one at its iterator index or at temp 0 (outside the
    // elements: error 0x6D; BV 0xb15f2c).
    case 0x29: return &c.me().array(data).iterator;
    case 0x2A: return &c.stackObject().array(data).iterator;
    case 0x2B: {
        auto& a = c.me().array(data);
        return &element(a.values, a.iterator, "my object array element");
    }
    case 0x2C: {
        auto& a = c.stackObject().array(data);
        return &element(a.values, a.iterator, "stack object's object array element");
    }
    case 0x2D: return &element(c.me().array(data).values, t.temps[0], "my object array element");
    case 0x2E: return &element(c.stackObject().array(data).values, t.temps[0], "stack object's object array element");
    default: return nullptr;
    }
}

// Fields of a temporary token (data owners 0x27/0x28), numbered as in TempToken.
static int16_t& tokenProperty(TempToken& t) {
    return slot(t.token.properties, t.propertyIndex, "temporary token property");
}

static int16_t tokenField(TempToken& t, int16_t field) {
    switch (field) {
    case 0: return int16_t(t.kind);
    case 1: return t.inventoryId;
    case 2: return t.counted;
    case 3: return t.index;
    case 4:
    case 5:
    case 6:
    case 7: return t.copied[size_t(field - 4)]; // raw copies (TempToken)
    case 8: return t.propertyIndex;
    case 9: return tokenProperty(t);
    default: throw VMError("temporary token field " + std::to_string(field) + " out of range");
    }
}

static void setTokenField(TempToken& t, int16_t field, int16_t value) {
    switch (field) {
    case 0:
        if (value < 0 || value > 3)
            throw VMError("temporary token: inventory kind " + std::to_string(value) + " out of range");
        t.kind = InventoryKind(value);
        return;
    case 1: t.inventoryId = value; return;
    case 2: t.counted = value != 0; return;
    case 3: t.index = value; return;
    case 4:
    case 5:
    case 6:
    case 7: t.copied[size_t(field - 4)] = value; return; // raw copies: the token is not changed
    case 8: t.propertyIndex = value; return;
    case 9: tokenProperty(t) = value; return;
    default: throw VMError("temporary token field " + std::to_string(field) + " out of range");
    }
}

// Container slots: the type 0 entries of the object's SLOT (NumObjectSlots).
static size_t containmentSlotCount(VM& vm, const Object& o) {
    size_t n = 0;
    if (o.definition)
        if (const Slots* t = vm.code().slots(o.group, o.definition->slotId()))
            for (const auto& x : t->slots)
                n += x.type == 0;
    return n;
}

static int16_t slotContent(VM& vm, const Object& o, int16_t slot) {
    if (slot < 0 || size_t(slot) >= containmentSlotCount(vm, o))
        throw gameError(err::SlotOutOfRange, "slot " + std::to_string(slot) + " of object " + std::to_string(o.id));
    return vm.containedIn(o.id, slot);
}

int16_t Context::get(uint8_t owner, int16_t data) const {
    if (int16_t* v = storage(*this, owner, data))
        return *v;
    switch (owner) {
    case 0x07: return data; // literal
    case 0x0A: return frame.stackObject;
    case 0x27: return tokenField(me().tempToken, data);
    case 0x28: return tokenField(stackObject().tempToken, data);
    case 0x15: return definitionField(stackObject(), data);
    case 0x33: return definitionField(masterDefinitionOf(vm, stackObject()), data);
    case 0x17: return roomValue(vm, thread.temps[0], data); // room [temp 0] (rooms.cpp)
    case 0x23: { // stack object's function: the action tree of its OBJf entry, 0 if none
        const Object& o = stackObject();
        const ObjectFunctions* f = vm.code().functions(o.group, o.definitionInstance);
        if (data < 0)
            throw VMError("object function " + std::to_string(data) + " out of range");
        return f && size_t(data) < f->functions.size() ? int16_t(f->functions[size_t(data)].action) : int16_t(0);
    }
    case 0x2F: {
        // Constants [temp] (cEdithLanguage BV 0x4b2764): bits 13-15 pick the
        // private, semi-global or global tables (else error 0x2E), bits
        // 3-12 the table and bits 0-2 the temp holding the entry ("G -
        // Logarithms Table" 0x140/0x141 are 0x4200 and 0x4208 in "Get in
        // Temp 0 - Logarithmic Modifier").
        auto* owner = vm.object(frame.codeOwner);
        if (!owner)
            throw VMError("code owner does not exist");
        const int scope = uint16_t(data) >> 13;
        if (scope > 2)
            throw gameError(err::InvalidConstant, "constant [temp] " + hexString(uint16_t(data)));
        const auto& c = constantsOf(vm, *owner, scope, uint16_t((data >> 3) & 0x3FF));
        return constantEntry(c, thread.temps[size_t(data & 7)]);
    }
    // Slots: the object in a slot of mine / of the stack object, 0 if the slot
    // is empty; the slot is `data` or the temp it names. A slot past the
    // object's container slots (object data 67, its SLOT's type 0 slots) is
    // error 0x1B (cTSObject::GetObjectSlot gives none; InterpValue BV 0xb15f2c).
    case 0x10: return slotContent(vm, stackObject(), data);
    case 0x14: return slotContent(vm, me(), data);
    case 0x30: return slotContent(vm, me(), temp(thread.temps, data, "temp"));
    case 0x31: return slotContent(vm, stackObject(), temp(thread.temps, data, "temp"));
    case 0x18: return neighborField(*this, data);
    case 0x22: // neighborhood data: cTSObject::InterpValue (base 0xb2d890, BV 0xb15f2c) reads it by
               // cTSNeighborhood's getter (+0x14 + 2i) and only for 1-15, else error 0x3C; it gives
               // no address, so it cannot be written (docs/save-mac.md §9.4)
        if (data < 1 || data > 15)
            throw gameError(err::NeighborhoodDataOutOfRange, "neighborhood data " + std::to_string(data));
        return vm.neighborhoodData[size_t(data)];
    case 0x21: // job data: the game has none (InterpValue, base 1.0 and BV: always error 0x3A)
        throw gameError(err::JobDataNotFound, "job data " + std::to_string(data));
    case 0x26: {
        auto def = vm.code().characterDefinition(stackNeighbor(*this).guid);
        const ObjectDefinition* d = def ? vm.code().definition(def->first, def->second) : nullptr;
        if (!d) // the game's error 0x23 (InterpValue BV 0xb15f2c)
            throw gameError(0x23, "neighbor " + std::to_string(frame.stackObject) + " has no definition");
        return definitionField(*d, data);
    }
    case 0x1A: {
        // Constant (cEdithLanguage BV 0x4b2704): bits 13-14 pick the private
        // (0x1000), semi-global (0x2000) or global (0x100) tables (3: error
        // 0x2E), bits 7-12 and bit 15 (as 0x40) the table, bits 0-6 the
        // entry. The tables are the code owner's (the stack element's
        // cTSBehavior, cTSBehavior::GetConstants BV 0x489ed4), also in
        // semi-global and global code.
        auto* owner = vm.object(frame.codeOwner);
        if (!owner)
            throw VMError("code owner does not exist");
        const int scope = (uint16_t(data) >> 13) & 3;
        if (scope == 3)
            throw gameError(err::InvalidConstant, "constant " + hexString(uint16_t(data)));
        const uint16_t table = uint16_t(((uint16_t(data) >> 9) & 0x40) | ((uint16_t(data) >> 7) & 0x3F));
        return constantEntry(constantsOf(vm, *owner, scope, table), data & 0x7F);
    }
    default:
        ++vm.missingOwners[owner];
        throw gameError(err::IllegalOwner, "data owner " + hexString(owner)); // past 0x33 (InterpValue)
    }
}

void Context::set(uint8_t owner, int16_t data, int16_t value) const {
    if (int16_t* v = storage(*this, owner, data)) {
        *v = value;
        if ((owner == 0x03 || owner == 0x04) && data == objdata::ContainerId) // my / the stack object's container
            vm.noteContainer(owner == 0x03 ? me() : stackObject());
        if ((owner == 0x03 || owner == 0x04) && data == objdata::Room) // my / the stack object's room
            vm.noteRoomChanged(owner == 0x03 ? me().id : stackObject().id);
        return;
    }
    switch (owner) {
    case 0x0A: frame.stackObject = value; return;
    case 0x27: setTokenField(me().tempToken, data, value); return;
    case 0x28: setTokenField(stackObject().tempToken, data, value); return;
    case 0x21: throw gameError(err::JobDataNotFound, "job data " + std::to_string(data));
    case 0x22: // a value without an address: the Expression's write is error 0x13 (TryExpression,
               // base 0xb67204, BV 0xe4bb64)
        throw gameError(err::ReadOnly, "neighborhood data " + std::to_string(data));
    case 0x07:
    case 0x10: // slots change by moving objects, not by writing to them
    case 0x14:
    case 0x30:
    case 0x31:
    case 0x15: // the editor marks only some definition fields read-only; writes are not supported yet
    case 0x17:
    case 0x18:
    case 0x1A:
    case 0x23:
    case 0x26:
    case 0x2F:
    case 0x33: throw VMError("data owner " + hexString(owner) + " is read-only");
    default:
        ++vm.missingOwners[owner];
        throw gameError(err::IllegalOwner, "data owner " + hexString(owner)); // past 0x33 (InterpValue)
    }
}

// --- VM --------------------------------------------------------------------

VM::VM(CodeLibrary& code) : code_(code) { registerPrimitives(*this); }

Object& VM::createObject(uint32_t group, size_t attributeCount, ObjectId id) {
    if (id <= 0) {
        id = 1;
        while (objects_.count(id)) {
            // Object IDs are positive 16-bit numbers: with all of them in use
            // a new object is an error (the thread is reset) rather than a
            // wrapped-around ID.
            if (id == std::numeric_limits<ObjectId>::max())
                throw VMError("no free object ID (32767 objects)");
            ++id;
        }
    } else if (objects_.count(id)) {
        throw VMError("object " + std::to_string(id) + " already exists");
    }
    Object& o = objects_[id];
    if (size_t(uint16_t(id)) >= byId_.size())
        byId_.resize(size_t(uint16_t(id)) + 1 + 64, nullptr);
    byId_[uint16_t(id)] = &o;
    ++tableGeneration_;
    o.id = id;
    o.serial = ++objectSerial_;
    o.listOrder = --listFront_;
    o.appendOrder = (int64_t(1) << 40) + int64_t(o.serial);
    objectList_[o.listOrder] = id;
    o.group = group;
    o.semiGroup = code_.semiGlobalFor(group);
    o.attributes.resize(attributeCount);
    if (o.semiGroup)
        o.semiAttributes.resize(code_.attributeLabelCount(o.semiGroup));
    o.data[objdata::ObjectId] = id;
    o.data[objdata::LeadTileObjectId] = id;
    o.data[objdata::SlotNumber] = -1;
    noteRoomChanged(id); // a new object is told its room (the pass that used to scan for it)
    o.data[objdata::SemiAttributeCount] = int16_t(o.semiAttributes.size());
    // Object data 45-47, 24 and 21 "birth year/month/day/hour/minute": when
    // the object came to be (a saved object's own come back with its state).
    // Inferred from the labels and the food: "Function - Init - Food" copies
    // them into the meal's age and "Get in Temps - Age of Meal" subtracts
    // them from the clock; at 0 every new meal was 22,000 hours old, spoiled
    // at once, and a Sim threw away the frozen dinner baking in the oven.
    o.data[45] = int16_t(clock.year), o.data[46] = int16_t(clock.month), o.data[47] = int16_t(clock.day);
    o.data[24] = int16_t(clock.hour), o.data[21] = int16_t(clock.minute);
    return o;
}

Object& VM::instantiate(uint32_t group, uint16_t definitionInstance) {
    const ObjectDefinition* def = code_.definition(group, definitionInstance);
    if (!def)
        throw VMError("OBJD " + hexString(group) + "/" + hexString(definitionInstance) + " not found");
    if (!def->masterId() || def->subIndex() != 0xFFFF)
        return instantiatePart(group, definitionInstance);
    const auto parts = code_.parts(group, def->masterId());
    if (parts.empty())
        throw VMError("multi-tile OBJD " + hexString(group) + "/" + hexString(definitionInstance) + " has no parts");
    // The lead is the part whose OBJD "multi-tile lead object" (label 63,
    // offset 0xBE) is set, else the one with sub index 0, else the first: the
    // saved lots lead a double bed by its "E - Head" (sub index 0x0002, the
    // flagged part), which the game created first (the lowest ID).
    uint16_t lead = parts.front();
    bool flagged = false;
    for (uint16_t p : parts)
        if (const ObjectDefinition* d = code_.definition(group, p); d && d->field(0xBE) != 0) {
            lead = p, flagged = true;
            break;
        }
    for (uint16_t p : parts)
        if (const ObjectDefinition* d = code_.definition(group, p); !flagged && d && d->subIndex() == 0) {
            lead = p;
            break;
        }
    // When the 0,0 part has no Main, the lead is the part that leads the
    // interaction group (OBJD interaction group < 0), else the first part
    // with a Main: the vehicles' 0,5 / 0,8 parts, whose semi-global
    // "Functional - Main" fetches the riders while the others' sleeps on its
    // "am I the lead?" test (inferred; the saved gates and wedding arches
    // lead from 0,0, whose Main they have). The master's interactions are
    // the parts' (interactionTable: tree table 0xFFFF).
    auto hasMain = [&](uint16_t instance) {
        const ObjectFunctions* f = code_.functions(group, instance);
        return f && f->functions.size() > 1 && f->functions[1].action;
    };
    if (!hasMain(lead)) {
        std::optional<uint16_t> withMain, groupLead;
        for (uint16_t p : parts) {
            if (!withMain && hasMain(p))
                withMain = p;
            if (const ObjectDefinition* d = code_.definition(group, p); !groupLead && d && int16_t(d->interactionGroup()) < 0)
                groupLead = p;
        }
        if (groupLead || withMain)
            lead = groupLead ? *groupLead : *withMain;
    }
    Object& first = instantiatePart(group, lead);
    const ObjectId leadId = first.id;
    for (uint16_t p : parts)
        if (p != lead)
            instantiatePart(group, p).data[objdata::LeadTileObjectId] = leadId;
    return *object(leadId);
}

Object& VM::instantiatePart(uint32_t group, uint16_t definitionInstance, ObjectId id) {
    const ObjectDefinition* def = code_.definition(group, definitionInstance);
    if (!def)
        throw VMError("OBJD " + hexString(group) + "/" + hexString(definitionInstance) + " not found");
    Object& o = createObject(group, std::max<size_t>(def->numAttributes(), kMinAttributes), id);
    o.arrays.resize(std::min<size_t>(uint16_t(def->field(0x40 + 2 * 0x3B)), kMaxObjectArrays));
    o.definition = def;
    o.definitionInstance = definitionInstance;
    if (const uint32_t g = o.guid()) { // objectsOfGuid
        auto& list = byGuid_[g];
        list.insert(std::upper_bound(list.begin(), list.end(), o.id), o.id);
    }
    // Engine-maintained, as the lots' saved states show (600 of the 665 objects
    // of N001_Lot11; the rest are multi-tile parts and leftovers): the
    // number of container slots (type 0) of the object's SLOT.
    if (const Slots* sl = code_.slots(group, def->slotId()))
        o.data[objdata::SlotCount] = int16_t(std::count_if(sl->slots.begin(), sl->slots.end(),
                                                           [](const Slots::Slot& x) { return x.type == 0; }));
    // Object data 58 "object version" starts as the definition's (OBJD label
    // 49): a saved object whose version differs runs its load tree
    // (lot_resume.cpp). Inferred: the lots' states hold the definition's.
    o.data[58] = int16_t(def->field(0x40 + 2 * 49));
    if (def->objectType() == kObjectTypePerson)
        o.person.emplace();
    return o;
}

Object* VM::instantiateGuid(uint32_t guid) {
    auto def = code_.definitionByGuid(guid);
    return def ? &instantiate(def->first, def->second) : nullptr;
}

VM::RelationshipKey VM::relationshipKey(ObjectId id) {
    const Object* o = object(id);
    if (!o)
        throw VMError("object " + std::to_string(id) + " does not exist");
    if (o->person && o->person->data[persondata::NeighborId])
        return {true, o->person->data[persondata::NeighborId]};
    return {false, id};
}

Object* VM::neighborSim(int16_t id) {
    if (id == 0)
        return nullptr;
    for (Object* o : byId_) // by ID, as objects_ is
        if (o && o->person && o->person->data[persondata::NeighborId] == id)
            return o;
    return nullptr;
}

int16_t VM::neighborForGuid(uint32_t guid) {
    if (neighbors.empty() || guid == 0)
        return 0;
    for (const auto& [id, n] : neighbors)
        if (n.guid == guid)
            return id;
    const int16_t id = int16_t(neighbors.rbegin()->first + 1);
    Neighbor& n = neighbors[id];
    n.guid = guid;
    n.personData[persondata::NeighborId] = id;
    return id;
}

ObjectId VM::containedIn(ObjectId container, int16_t slot) const {
    if (container == 0 || slot < 0)
        return 0;
    checkContained();
    // An object already on the kill queue (Remove Object Instance) is not in
    // the slot any more: "Chess - Reset Sim" removes what the Sim holds and
    // asks again until the hand is empty, all in one tick (it spun out the
    // instruction budget in N002_Lot10; inferred).
    for (const ObjectId id : contained_.ids) { // by ID, as objects_ is
        const Object* o = uint16_t(id) < byId_.size() ? byId_[uint16_t(id)] : nullptr;
        if (o && o->data[objdata::ContainerId] == container && o->data[objdata::SlotNumber] == slot && o->id != container &&
            !dyingObjects.contains(o->id))
            return o->id;
    }
    return 0;
}

void VM::IdList::add(ObjectId id) {
    const size_t k = uint16_t(id);
    if (k >= in.size())
        in.resize(k + 1 + 64, 0);
    in[k] = 1;
    ids.insert(std::upper_bound(ids.begin(), ids.end(), id), id);
}

void VM::IdList::drop(ObjectId id) {
    if (!has(id))
        return;
    in[uint16_t(id)] = 0;
    ids.erase(std::lower_bound(ids.begin(), ids.end(), id));
}

void VM::checkContained() const {
    static const bool check = std::getenv("OS2_CHECK_CONTAINED") != nullptr;
    if (!check)
        return;
    for (const Object* o : byId_)
        if (o && o->data[objdata::ContainerId] != 0 &&
            !(contained_.has(o->id) && std::binary_search(contained_.ids.begin(), contained_.ids.end(), o->id))) {
            std::fprintf(stderr, "opensims2: object %d is in container %d but not in the containment index\n", o->id,
                         o->data[objdata::ContainerId]);
            std::abort();
        }
    for (const Object* o : byId_)
        if (o && ((!o->timers.empty() && !timed_.has(o->id)) ||
                  ((o->syntheticSlot || o->managedDestination) && !destinations_.has(o->id)))) {
            std::fprintf(stderr, "opensims2: object %d has timers or is a destination but is not listed\n", o->id);
            std::abort();
        }
    size_t byGuid = 0;
    for (const auto& [g, list] : byGuid_)
        byGuid += list.size();
    size_t withGuid = 0;
    for (const Object* o : byId_)
        if (o && o->guid()) {
            ++withGuid;
            const auto it = byGuid_.find(o->guid());
            if (it == byGuid_.end() || !std::binary_search(it->second.begin(), it->second.end(), o->id)) {
                std::fprintf(stderr, "opensims2: object %d is not listed under its GUID\n", o->id);
                std::abort();
            }
        }
    if (byGuid != withGuid) {
        std::fprintf(stderr, "opensims2: %zu objects listed by GUID, %zu with a GUID\n", byGuid, withGuid);
        std::abort();
    }
}

uint16_t VM::functionTree(const Object& o, size_t function) {
    const ObjectFunctions* f = code_.functions(o.group, o.definitionInstance);
    return f && function < f->functions.size() ? f->functions[function].action : 0;
}

std::optional<Thread> VM::runFunction(ObjectId id, size_t function, size_t maxTurns, std::span<const int16_t> args,
                                      ObjectId stackObject) {
    const Object* o = object(id);
    if (!o)
        throw VMError("object " + std::to_string(id) + " does not exist");
    const uint16_t tree = functionTree(*o, function);
    // An OBJf entry naming a tree its object does not have (the vehicle
    // masters' Init 0x1005) runs nothing (inferred).
    if (!tree || !resolveTree(*o, tree).second)
        return std::nullopt;
    Thread t = start(id, tree, args, stackObject);
    for (size_t turn = 0; t.status == Thread::Status::Running && turn < maxTurns; ++turn)
        run(t, instructionsPerTurn);
    dropThreadRecords(*this, t); // records.cpp: a thread run apart
    return t;
}

void VM::initialize(ObjectId id, const std::vector<ObjectId>& existing) {
    constexpr int kMaxInitDepth = 8;
    if (initDepth_ >= kMaxInitDepth)
        throw VMError("objects created by Inits nest too deep");
    const Object* o = object(id);
    if (!o)
        return;
    const ObjectId lead = o->data[objdata::LeadTileObjectId] ? o->data[objdata::LeadTileObjectId] : id;
    std::vector<ObjectId> parts;
    for (const auto& [other, x] : objects_)
        if ((other == id || x.data[objdata::LeadTileObjectId] == lead) &&
            !std::binary_search(existing.begin(), existing.end(), other))
            parts.push_back(other);
    ++initDepth_;
    try {
        for (ObjectId p : parts)
            if (auto t = runFunction(p, 0))
                createdInits.push_back(std::move(*t));
    } catch (...) { // any type: the depth is put back, then it goes on (throw;)
        --initDepth_;
        throw;
    }
    --initDepth_;
    for (ObjectId p : parts)
        startMain(p);
}

bool VM::startMain(ObjectId id) {
    const Object* o = object(id);
    const uint16_t tree = o ? functionTree(*o, 1) : 0;
    if (!tree)
        return false;
    threads.insert_or_assign(id, start(id, tree));
    return true;
}

void VM::setClockGlobals() {
    globals[global::Hour] = int16_t(clock.hour);
    globals[global::Minute] = int16_t(clock.minute);
    globals[global::Second] = int16_t(clock.tick * 60 / std::max(clock.ticksPerMinute, 1));
    globals[global::DayOfMonth] = int16_t(clock.day);
    globals[global::Month] = int16_t(clock.month);
    globals[global::Year] = int16_t(clock.year);
    globals[global::TimeOfDay] = timeOfDay(clock.hour);
}

// cTSSimulator's time of day for an hour (Mac binary, FUN_004984d4, STR#
// 0xE1 "times of day"): 0 day 7-17, 1 dusk 18, 2 night 19-5, 3 dawn 6. The
// simulator sets global 4 from it whenever the hour changes.
int16_t timeOfDay(int hour) {
    if (hour == 6)
        return 3;
    if (hour >= 7 && hour < 18)
        return 0;
    if (hour == 18)
        return 1;
    return 2;
}

void VM::tick() {
    // A nested tick (there is none today: the app's loop, the autonomy loop
    // and startLotObjects are the only callers) must not drop the dead
    // threads: the iterators of the outer tick would dangle.
    const bool outer = !ticking;
    ticking = true;
    struct Guard {
        bool& flag;
        ~Guard() { flag = false; }
    } guard{ticking};
    // The calendar is 30-day months, as FreeSO's clock (not confirmed).
    if (++clock.tick >= clock.ticksPerMinute) {
        clock.tick = 0;
        if (++clock.minute == 60) {
            clock.minute = 0;
            if (++clock.hour == 24) {
                // A new day (cTSSimulator::SimulateOneTick): Days Running
                // counts up, Day Of Week = Days Running % 7, and a new month
                // sets Maximum Day Number to 30.
                clock.hour = 0;
                ++globals[global::DaysRunning];
                globals[global::DayOfWeek] = int16_t(globals[global::DaysRunning] % 7);
                dayPassed(*this); // the objects depreciate (placement.cpp)
                // cTSPerson::DayPassed: the memories fade (memories.cpp).
                for (Object* o : objectTable())
                    if (o && o->person)
                        updateMemories(*this, *o, true);
                if (++clock.day > 30) {
                    clock.day = 1;
                    globals[global::MaxDayNumber] = 30;
                    if (++clock.month > 12)
                        clock.month = 1, ++clock.year;
                }
            }
        }
    }
    setClockGlobals();
    ++ticks;
    if (dialogClockIsTicks) // dialog.hpp: headless, game time at speed 1 is the clock
        autoTimeOutDialogs(*this, double(ticks) / clock.ticksPerSecond);
    tickMotives(*this); // motives.cpp (and the memories every 60 ticks)
    tickWants(*this);   // wants.cpp
    handleTimers();
    tickHurry(*this); // animation.hpp: object data 85 hurries the requests (OnPreSimulatorTick)
    // Objects created or removed during the tick: the threads are read first
    // (the map's nodes are stable, so the iterators stay valid while the
    // tick runs); new objects get their turn from the next tick on, and the
    // threads of the removed ones go at the end of it (deadThreads), so that
    // walking the map does not look every thread up again.
    std::vector<std::map<ObjectId, Thread>::iterator> its;
    its.reserve(threads.size());
    for (auto it = threads.begin(); it != threads.end(); ++it)
        its.push_back(it);
    for (auto it : its) {
        const ObjectId id = it->first;
        Thread& t = it->second;
        if (t.status == Thread::Status::Finished) {
            const Object* o = object(id);
            const uint16_t tree = o ? functionTree(*o, 1) : 0;
            if (!tree)
                continue;
            const size_t done = t.instructions;
            t = start(id, tree);
            t.instructions = done;
        }
        if (t.status == Thread::Status::Running)
            run(t, instructionsPerTurn);
        if (t.status == Thread::Status::Failed && resetOnError)
            resetAfterError(id, t);
    }
    // The threads of the objects removed during the tick go now (the ID and
    // the serial: the ID may belong to a new object by now). Only the
    // outermost tick drops them: a nested one would leave the iterators of
    // the outer tick dangling.
    if (outer) {
        for (const auto& [id, serial] : deadThreads)
            if (auto it = threads.find(id); it != threads.end() && it->second.serial == serial)
                threads.erase(it);
        deadThreads.clear();
    }
    tickOverlayEvents(*this); // animation.hpp
    flushKillQueue(*this); // kill.hpp
    flushDestinations(*this); // go_here.hpp
    followContainers();
    notifyRoomChanges(*this); // rooms.hpp: OnRoomChanged
    // The simulator's tick counts up once every object has run
    // (cTSSimulator::SimulateOneTick, Mac BV 0x49a814).
    ++simulatorTicks;
    // OS2_DEBUG_RECORDS=1: an object whose data 62 "use count" is not the
    // number of the persons' object records on it, as it first differs.
    if (static const bool checkRecords = std::getenv("OS2_DEBUG_RECORDS") != nullptr; checkRecords) {
        static std::set<std::pair<ObjectId, int>> told;
        std::map<ObjectId, int> uses;
        for (const Object* o : objectTable())
            if (o && o->person)
                for (const auto& r : o->person->objectRecords)
                    ++uses[r.object];
        if (const char* at = std::getenv("OS2_DEBUG_RECORDS_AT"); at && ticks % uint64_t(std::max(1, std::atoi(at))) == 0)
            for (const Object* o : objectTable())
                if (o && o->person)
                    for (const auto& r : o->person->objectRecords) {
                        const Object* x = object(r.object);
                        std::fprintf(stderr, "t%llu record: %d -> %d (%s) depth %d\n", (unsigned long long)ticks, o->id,
                                     r.object, x && x->definition ? x->definition->fileName.c_str() : "?", r.depth);
                    }
        for (const Object* o : objectTable())
            if (o && o->data[62] != uses[o->id] && told.insert({o->id, o->data[62] - uses[o->id]}).second)
                std::fprintf(stderr, "t%llu records: object %d (%s) data 62 = %d, %d records\n", (unsigned long long)ticks,
                             o->id, o->definition ? o->definition->fileName.c_str() : "?", o->data[62], uses[o->id]);
    }
}

// The object's reset after a SimAntics error (cTSObject::HandleError, BV
// 0xb05510, outside the debugger): its interactions are unlinked (vtable
// 0x534), then Cleanup(nobody) (vtable 0x320, BV 0xb09838):
// - a person (cTSPerson::OnCleanup, BV 0xadcbcc) drops its stack and queue,
//   its tree sim is reset to the Main, its cleanup tree (OBJf entry 3) runs,
//   person data 0 and 59 and object data 34 are cleared and the objects of
//   its object records are reset with their users (records.cpp);
// - an object with a cleanup tree runs it and its tree sim is reset to the
//   Main, its data stay;
// - an object without one is Reset (cTSObject::Reset(1, true, false, false),
//   BV 0xb0ab9c): the object data are zeroed but for direction, container
//   and slot, room, lead tile, object ID, the birth date, current value,
//   target slot count, slot count, the semi-attribute count and the object
//   links (79-81; 105-107 are past the VM's data); render flags 71 := 0,
//   57 and the render alpha and colours 72-75 := 255, rotation notches 23 :=
//   2, object version 58 and look at score 78 := the definition's (labels
//   49, 13), and with "use default placement flags" (label 12) the height,
//   wall adjacency, wall placement and placement flags (4, 5, 13, 42) := the
//   definition's defaults (labels 6, 3, 5, 4); the niceness score 7 := OBJD
//   field 45 (the GetDefaultNicenessMultiplier virtual, vtable 0x578).
//   The attributes stay. Its Init runs (not
//   for a multi-tile part: its group's lead handles it), then the Main
//   starts over.
// The game also writes an ObjectError log. The Main starts from its first
// node the next tick.
void VM::resetAfterError(ObjectId id, Thread& t) {
    // On stderr once per error and tree (the game writes its ObjectError log;
    // s2pkg prints errorResets, the app did not).
    {
        std::string where = t.error.substr(0, t.error.find('\n', t.error.find('\n') + 1));
        if (const size_t nl = where.find("\n  at "); nl != std::string::npos)
            where.replace(nl, 6, " at ");
        const Object* o = object(id);
        os2::logOnce("SimAntics error", where,
                     "object " + std::to_string(id) +
                         (o && o->definition && !o->definition->fileName.empty() ? " (" + o->definition->fileName + ")" : "") +
                         " reset");
    }
    ErrorReset& r = errorResets[id];
    if (r.count++ == 0)
        r.first = t.error;
    r.last = t.error;
    r.lastTick = ticks;
    resetObject(id, t);
}

// Cleanup(nobody) (cTSObject::Cleanup, BV 0xb09838), as above, without the
// error: also what an object's removal does to a person whose current
// interaction object or route target it was (kill.cpp).
void VM::resetObject(ObjectId id, Thread& t, bool cascade) {
    Object* o = object(id);
    const uint16_t main = o ? functionTree(*o, 1) : 0;
    if (!o || !main || cleaningUp.contains(id))
        return;
    // The guard of cTSObject::Cleanup (flag 0x40000) and cTSPerson::OnCleanup
    // (0x800): a cascade of resets (records.cpp) does not come back here.
    cleaningUp.insert(id);
    struct Guard {
        std::set<ObjectId>& set;
        ObjectId id;
        ~Guard() { set.erase(id); }
    } guard{cleaningUp, id};
    if (o->person) {
        // cTSPerson::OnCleanup (BV 0xadcbcc): the object records go, each
        // with its frames (data 62 - 1), and unless the person is in the
        // kill queue (flag 0x1000) each record's object is reset with its
        // other users (records.cpp). The running interactions' links are
        // undone as their frames go (StackJustPopped's end of the action);
        // the waiting entries leave by DeleteTopAction / DeleteTopSubAction:
        // ActionSkipped, OBJf entry 4 on their objects.
        cleanupRecords(*this, id, cascade && !dying(*this, id));
        o = object(id);
        if (!o)
            return;
        std::vector<QueuedInteraction> running, waiting;
        for (size_t i = 0; i < o->queue.size(); ++i)
            (int(i) <= o->activeInteraction ? running : waiting).push_back(o->queue[i]);
        if (o->subAction)
            running.push_back(*o->subAction);
        for (const auto& q : o->subQueue)
            waiting.push_back(q);
        o->queue.clear();
        o->activeInteraction = -1;
        o->subAction.reset();
        o->subQueue.clear();
        // SetCurrentAction(the empty interaction): its priority, 0, and a
        // GUID for it (person data 109).
        o->person->data[persondata::Priority] = kPriorityIdle;
        becomeEmptyAction(*this, *o);
        syncQueueData(*o);
        setInteractionData(*o);
        for (const auto& q : running)
            unlinkInteraction(*this, id, q);
        for (const auto& q : waiting)
            actionSkipped(*this, id, q);
        o = object(id);
        if (!o)
            return;
        syncQueueData(*o);
    }
    // The router's OnCleanup (BV 0x5727c8) empties the route stack
    // (ClearRouteStack: PopRoute until none is left), the last one setting
    // person data 39 "route status" to 0 (docs/primitives-mac.md §4j; it is 1
    // only while a route is on the stack).
    if (o->person)
        o->person->data[persondata::RouteStatus] = 0;
    o->route.reset();
    o->outerRoutes.clear();
    const size_t done = t.instructions;
    t = start(id, main);
    t.instructions = done;
    if (!o->person && !functionTree(*o, 3) && o->definition) {
        const auto old = o->data;
        o->data.fill(0);
        for (int k : {1, 2, 3, 11, 19, 21, 24, 29, 41, 45, 46, 47, 55, 67, 79, 80, 81})
            o->data[size_t(k)] = old[size_t(k)];
        noteContainer(*o);
        // GetDefaultNicenessMultiplier (vtable 0x578): OBJD field 45.
        o->data[7] = int16_t(definitionField(*o, 45));
        o->data[71] = 0;
        for (int k : {57, 72, 73, 74, 75})
            o->data[size_t(k)] = 0xFF;
        o->data[23] = 2;
        o->data[58] = definitionField(*o, 49);
        o->data[78] = definitionField(*o, 13);
        if (definitionField(*o, 12)) {
            o->data[4] = definitionField(*o, 6);
            o->data[5] = definitionField(*o, 3);
            o->data[13] = definitionField(*o, 5);
            o->data[42] = definitionField(*o, 4);
        }
        if (!o->definition->masterId())
            runFunction(id, 0); // Init
    } else if (o->person) {
        // cTSPerson::OnCleanup: out of the world and back in a free place
        // near; person data 0 "Sitting?" and 59 "Social Menu Obj ID" and
        // object data 34 "Hidden Flags" := 0 (SetPersonData 0 / 0x3b,
        // SetObjectData 0x22: BV 0xadcbcc, base 1.0 0xb85f74-0xb85fc0; was
        // person data 34, inferred); the cleanup tree.
        placeNearFree(*o);
        o->person->data[0] = 0;
        o->person->data[59] = 0;
        o->data[34] = 0;
        if (functionTree(*o, 3))
            runFunction(id, 3);
    } else if (functionTree(*o, 3)) {
        runFunction(id, 3); // the cleanup tree
    }
}

void VM::followContainers() {
    // Contained objects are where their container is (as the saved lots keep
    // them and Snap Into puts them): what a Sim carries goes along with it.
    // By ID, as objects_ is; those out of their container leave the list.
    checkContained();
    contained_.filter([&](ObjectId id) {
        const Object* o = object(id);
        return o && o->data[objdata::ContainerId] != 0;
    });
    for (const ObjectId id : contained_.ids) {
        Object& o = *object(id);
        const ObjectId in = o.data[objdata::ContainerId];
        if (in == o.id)
            continue;
        const Object* top = object(in);
        if (!top)
            continue;
        for (int depth = 0; depth < 8 && top->data[objdata::ContainerId]; ++depth) {
            const Object* up = object(top->data[objdata::ContainerId]);
            if (!up || up == top)
                break;
            top = up;
        }
        setPosition(o, top->position);
        o.data[objdata::Room] = top->data[objdata::Room];
        noteRoomChanged(o.id);
    }
}

// The family tree (cTSFamilyRelationshipArray, BV 0x544028/0x5442e8).
void VM::removeTies(int16_t a, int16_t b) {
    for (auto [x, y] : {std::pair{a, b}, std::pair{b, a}}) {
        auto& t = familyTree[x];
        t.parents.erase(y), t.children.erase(y), t.spouses.erase(y), t.siblings.erase(y);
    }
}

// AddRelationship(child, parent, 4): the parent becomes parent 0 or 1 of the
// child (none if it has two: false), after the ties between the two are
// cleared, and the child a sibling of the parent's other children.
bool VM::addChildTie(int16_t child, int16_t parent) {
    if (child == parent)
        return false;
    if (familyTree[child].parents.size() >= 2 && !familyTree[child].parents.count(parent))
        return false;
    removeTies(child, parent);
    for (int16_t other : std::set<int16_t>(familyTree[parent].children))
        if (other != child) {
            removeTies(child, other);
            familyTree[child].siblings.insert(other);
            familyTree[other].siblings.insert(child);
        }
    familyTree[parent].children.insert(child);
    familyTree[child].parents.insert(parent);
    return true;
}

// AddRelationship(a, b, 2): each leaves its former spouse first.
void VM::addSpouseTie(int16_t a, int16_t b) {
    if (a == b)
        return;
    removeTies(a, b);
    for (int16_t x : {a, b})
        for (int16_t old : std::set<int16_t>(familyTree[x].spouses))
            removeTies(x, old);
    familyTree[a].spouses.insert(b);
    familyTree[b].spouses.insert(a);
}

Object* VM::object(ObjectId id) {
    const size_t k = uint16_t(id);
    return k < byId_.size() ? byId_[k] : nullptr;
}

std::pair<uint32_t, const Behavior*> VM::resolveTree(const Object& owner, uint16_t treeId) {
    uint32_t group = 0;
    if (treeId >= 0x100 && treeId < 0x1000)
        group = kGlobalGroup;
    else if (treeId >= 0x1000 && treeId < 0x2000)
        group = owner.group;
    else if (treeId >= 0x2000)
        group = owner.semiGroup;
    const Behavior* b = group ? code_.behavior(group, treeId) : nullptr;
    return {b ? group : 0, b};
}

const Constants& VM::resolveConstants(const Object& owner, uint16_t table) {
    if (table < 64)
        return resolveConstants(owner, 0, table);
    if (table < 128)
        return resolveConstants(owner, 1, uint16_t(table - 64));
    return resolveConstants(owner, 2, uint16_t(table - 128));
}

const Constants& VM::resolveConstants(const Object& owner, int scope, uint16_t table) {
    // Private and semi-global tables resolve against the code owner, also
    // when the running code is semi-global or global.
    uint32_t group;
    uint16_t instance;
    if (scope == 0)
        group = owner.group, instance = uint16_t(0x1000 + table);
    else if (scope == 1)
        group = owner.semiGroup, instance = uint16_t(0x2000 + table);
    else
        group = kGlobalGroup, instance = uint16_t(0x100 + table);
    const Constants* c = group ? code_.constants(group, instance) : nullptr;
    if (!c)
        throw VMError("BCON " + hexString(group) + "/" + hexString(instance) + " not found");
    return *c;
}

Thread VM::start(ObjectId me, uint16_t treeId, std::span<const int16_t> args, ObjectId stackObject,
                 ObjectId codeOwner) {
    Thread t;
    t.me = me;
    t.serial = ++threadSerials;
    if (const Object* o = object(me))
        t.meSerial = o->serial;
    try {
        push(t, codeOwner ? codeOwner : me, treeId, args);
        t.stack.back().stackObject = stackObject;
    } catch (const std::exception& e) {
        fail(t, e.what());
    }
    return t;
}

void VM::push(Thread& t, ObjectId codeOwner, uint16_t treeId, std::span<const int16_t> args) {
    const Object* owner = object(codeOwner);
    if (!owner)
        throw VMError("code owner " + std::to_string(codeOwner) + " does not exist");
    auto [group, behavior] = resolveTree(*owner, treeId);
    if (!behavior)
        throw gameError(0x11, "tree " + hexString(treeId) + " not found");
    // cTSTreeSim::Gosub (BV 0xe1a428) refuses past 1000 frames (0x452) and a
    // tree without nodes; DoNodeAction then throws "Stack overflow" (1000)
    // for a tree that exists.
    if (t.stack.size() > kMaxStackDepth)
        throw gameError(0x3E8, "stack overflow: max stack depth exceeded");
    Frame f;
    f.behavior = behavior;
    f.group = group;
    f.instance = treeId;
    f.codeOwner = codeOwner;
    if (!t.stack.empty())
        f.stackObject = t.stack.back().stackObject;
    f.params.assign(std::max<size_t>({behavior->argCount, args.size(), 4}), 0);
    std::copy(args.begin(), args.end(), f.params.begin());
    f.locals.assign(behavior->localCount, 0);
    if (behavior->instructions.empty())
        throw gameError(0x3E8, "tree " + hexString(treeId) + " has no instructions");
    t.stack.push_back(std::move(f));
}

void VM::call(Thread& t, ObjectId codeOwner, uint16_t treeId, std::array<int16_t, 4> args, bool changeIcon) {
    push(t, codeOwner, treeId, args);
    gosubObjectTree(*this, t, codeOwner, changeIcon); // records.cpp
}

void VM::fail(Thread& t, const std::string& message) {
    t.status = Thread::Status::Failed;
    t.error = message;
    constexpr size_t kShownFrames = 8;
    for (size_t i = 0; i < t.stack.size() && i < kShownFrames; ++i) {
        const Frame& f = t.stack[t.stack.size() - 1 - i];
        t.error += "\n  at " + treeName(f) + " #" + std::to_string(f.pc);
    }
    if (t.stack.size() > kShownFrames)
        t.error += "\n  ... " + std::to_string(t.stack.size() - kShownFrames) + " more frames";
}

bool VM::branch(Thread& t, Behavior::Target target) {
    for (;;) {
        switch (target.kind) {
        case Behavior::TargetKind::Instruction:
            t.stack.back().pc = target.index;
            t.stack.back().primitiveState = 0;
            return true;
        case Behavior::TargetKind::Error:
            fail(t, "error branch taken");
            return false;
        case Behavior::TargetKind::True:
        case Behavior::TargetKind::False: {
            const bool value = target.kind == Behavior::TargetKind::True;
            const bool interaction = t.stack.back().interaction;
            const bool foundAction = t.stack.back().foundAction;
            const bool subInteraction = t.stack.back().subInteraction;
            const std::optional<bool> forced = t.stack.back().callerResult;
            const bool rerun = t.stack.back().rerunCaller;
            t.stack.pop_back();
            stackJustPopped(*this, t); // a person's object records (records.cpp)
            if (t.stack.empty()) {
                t.status = Thread::Status::Finished;
                t.result = value;
                return false;
            }
            if (rerun) { // the primitive that ran it goes on
                t.rerunResult = value;
                return true;
            }
            if (subInteraction)
                endSubInteraction(*this, t); // and its Idle for Input branches on the result
            if (interaction) {
                // A queued interaction's action tree: the Idle for Input that ran it runs again.
                endInteraction(*this, t);
                if (!foundAction)
                    return true;
                t.interactionReturned = 0; // Gosub Found Action branches instead
                foundActionReturned(*this, t, value);
            }
            // The caller's call instruction branches on the returned value.
            const Frame& caller = t.stack.back();
            const auto& call = caller.behavior->instructions[caller.pc];
            target = forced.value_or(value) ? call.onTrue : call.onFalse;
            break;
        }
        }
    }
}

void VM::run(Thread& t, size_t maxInstructions) {
    ++treeRuns;
    t.turn = treeRuns;
    for (size_t budget = maxInstructions; t.status == Thread::Status::Running;) {
        if (budget-- == 0) {
            fail(t, "instruction budget exhausted (" + std::to_string(maxInstructions) + ")");
            return;
        }
        // A removed object's thread is killed, also when its ID went to a
        // new object in the meantime (the removal leaves vm.threads alone;
        // N003_Lot192: a school bus controller's Main went on as the bus
        // made with its ID, "tree 0x1008 not found" every tick).
        if (const Object* o = object(t.me); !o || (t.meSerial && o->serial != t.meSerial)) {
            t.status = Thread::Status::Killed;
            return;
        }
        Frame& frame = t.stack.back();
        const auto& in = frame.behavior->instructions[frame.pc];
        if (trace)
            trace(t, frame, in);
        ++t.instructions;
        try {
            Context ctx{*this, t, frame, in};
            if (in.opcode >= 0x100) {
                // Call (cTSTreeSim::DoNodeAction BV 0xe1b18c, Gosub
                // 0xe1a428): as many arguments as the callee takes, at most
                // 8: with bit 0 of byte 12, (owner u8, data u16) pairs at
                // 0, 3, 6, 9 (a callee of 1-3 arguments gets that many,
                // of 4-8 four, the rest 0; of more, all 0); with all
                // of bytes 0-7 0xFF the temps; else the literals, u16 at
                // 2 x i (the 5th to 8th from bytes 8-15: the operands are
                // the argument array). Node version 0 keeps only bit 0 of
                // byte 12 (RunPrivateTreeParam::Read; the rest is stale:
                // "Run Accepter Anim" has 0x69 there), so the BV's "my
                // parameters" bit 1 does not come up.
                const auto& o = in.operands;
                const Object* owner = object(frame.codeOwner);
                const Behavior* callee = owner ? resolveTree(*owner, in.opcode).second : nullptr;
                const size_t count = callee ? callee->argCount : 0;
                std::array<int16_t, 8> args{};
                if (o[12] & 1) {
                    const size_t n = count >= 1 && count <= 8 ? std::min<size_t>(count, 4) : 0;
                    for (size_t k = 0; k < n; ++k)
                        args[k] = ctx.get(o[k * 3], int16_t(le16(&o[k * 3 + 1])));
                } else if (std::all_of(o.begin(), o.begin() + 8, [](uint8_t b) { return b == 0xFF; })) {
                    std::copy_n(t.temps.begin(), std::min(args.size(), t.temps.size()), args.begin());
                } else {
                    for (size_t k = 0; k < args.size(); ++k)
                        args[k] = int16_t(le16(&o[k * 2]));
                }
                const std::span<const int16_t> passed(args.data(), std::clamp<size_t>(count, 4, args.size()));
                push(t, frame.codeOwner, in.opcode, passed);
                continue;
            }
            auto it = primitives_.find(in.opcode);
            if (it == primitives_.end()) {
                ++missingPrimitives[in.opcode];
                fail(t, "primitive " + hexString(in.opcode) + " (" + primitiveName(in.opcode) + ") not implemented");
                return;
            }
            switch (it->second(ctx)) {
            case Exit::Yield: return;
            case Exit::Call: break;
            case Exit::Error: fail(t, "primitive " + primitiveName(in.opcode) + " returned error"); return;
            case Exit::True: branch(t, in.onTrue); break;
            case Exit::False: branch(t, in.onFalse); break;
            }
        } catch (const std::exception& e) { // VMError, or a resource that failed to parse
            fail(t, e.what());
        }
    }
}

} // namespace os2::vm
