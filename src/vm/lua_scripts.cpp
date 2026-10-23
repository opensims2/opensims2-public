// The 0x7E (Run Script)'s Lua 5.0 side: the game keeps its object scripts in
// TSData/Res/ObjectScripts as Lua 5.0 chunks (docs/lua-scripts.md), and the
// 0x7E runs one of them by name. The scripts call the engine through the API
// registered here (the PC build registers it in FUN_0090eeb0 and FUN_00911470,
// with the objects in FUN_0090e1c0/FUN_0090faa0/FUN_00911180); this file is
// the port of that API, over the VM's objects, neighbors and the running
// thread.
#include "lua_scripts.hpp"

#include "errors.hpp"
#include "formats/log_once.hpp"
#include "vm.hpp"

// Lua 5.0's headers have no extern "C": the library is C (third_party/lua50).
extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace os2::vm {

namespace {

int argInt(lua_State* L, int i) { return int(lua_tonumber(L, i)); }

// A 32-bit value as the engine pushes it: a signed int in the Lua number (the
// preludes' GUID tables hold the values that do not fit 31 bits as negatives).
void pushInt32(lua_State* L, uint32_t v) { lua_pushnumber(L, double(int32_t(v))); }

bool traceLua() {
    static const bool on = std::getenv("OS2_TRACE_LUA") != nullptr;
    return on;
}

} // namespace

// One Lua state per VM: the preludes (the game's `.globalObjLua` chunks) load
// once, and every 0x7E runs its script in it, as the game does.
class LuaRuntime {
public:
    explicit LuaRuntime(VM& vm);
    ~LuaRuntime();
    LuaRuntime(const LuaRuntime&) = delete;

    // Runs script `name` with `c` (the running instruction) and returns the
    // value SetScriptReturnValue left; false when the script is missing, fails
    // to load or errors.
    bool run(Context& c, const std::string& name);

    Context* context() const { return current_; }
    VM& vmm() { return vm_; }

    // The running object / stack object / person lookups the API uses.
    Object* object(int id);
    int16_t* personData(int16_t nid, int field);

private:
    VM& vm_;
    lua_State* L_ = nullptr;
    Context* current_ = nullptr;
    bool returned_ = false;

    static LuaRuntime* self(lua_State* L);

    void registerApi();
    void registerWorldTables();
    void loadPreludes();
    bool runChunk(const char* data, size_t size, const std::string& name);
    // A class table: `Class.new(...)` builds an instance whose metatable's
    // __index is the methods table; a method the port does not have answers
    // with a logging stub instead of erroring the script.
    void makeClass(const char* name, const luaL_reg* methods, lua_CFunction ctor);
    void pushInstance(lua_State* L, const char* className, int16_t nid);

    // --- the engine's functions (FUN_0090eeb0: threads and frames) ---
    static int setScriptReturnValue(lua_State* L);
    static int getStackObjectId(lua_State* L);
    static int getRunningObjectId(lua_State* L);
    static int getRunningOnObjectId(lua_State* L);
    static int getTemp(lua_State* L);
    static int setTemp(lua_State* L);
    static int getLocal(lua_State* L);
    static int setLocal(lua_State* L);
    static int getTreeParameter(lua_State* L);
    static int setTreeParameter(lua_State* L);
    static int getPrimitiveParameter(lua_State* L);

    // --- FUN_00911470: the objects, people and definitions ---
    static int getPersonData(lua_State* L);
    static int setPersonData(lua_State* L);
    static int getNeighborPersonData(lua_State* L);
    static int setNeighborPersonData(lua_State* L);
    static int getObjectData(lua_State* L);
    static int setObjectData(lua_State* L);
    static int getObjectAttribute(lua_State* L);
    static int setObjectAttribute(lua_State* L);
    static int getObjectSemiAttribute(lua_State* L);
    static int setObjectSemiAttribute(lua_State* L);
    static int getObjectDefinitionField(lua_State* L);
    static int getObjectArrayCopy(lua_State* L);
    static int setObjectArray(lua_State* L);
    static int getNumObjectArrays(lua_State* L);
    static int getObjectIdInSlot(lua_State* L);
    static int getConstant(lua_State* L);
    static int getNumConstants(lua_State* L);
    static int getNumConstantsWithGuid(lua_State* L);
    static int getConstantWithGuid(lua_State* L);
    static int getConstantsTableByGuid(lua_State* L);
    static int getSimulatorGlobal(lua_State* L);
    static int setSimulatorGlobal(lua_State* L);
    static int getNeighborRelationship(lua_State* L);
    static int setNeighborRelationship(lua_State* L);
    static int getUserDirectory(lua_State* L);
    static int createObjectGuid(lua_State* L);
    static int splitObjectGuid(lua_State* L);
    static int roundFloat(lua_State* L);
    static int mathRound(lua_State* L);
    static int bitwiseAnd(lua_State* L);
    static int bitwiseOr(lua_State* L);
    static int bitwiseXOr(lua_State* L);
    static int bitShiftLeft(lua_State* L);
    static int bitShiftRight(lua_State* L);
    static int setBit(lua_State* L);
    static int clearBit(lua_State* L);
    static int isBitSet(lua_State* L);

    // --- the C-defined objects (FUN_0090e1c0/FUN_0090faa0/FUN_00911180) ---
    static int neighborNew(lua_State* L);
    static int neighborGetPersonData(lua_State* L);
    static int neighborSetPersonData(lua_State* L);
    static int neighborGetGuid(lua_State* L);
    static int neighborGetName(lua_State* L);
    static int neighborGetSurname(lua_State* L);
    static int neighborGetFullName(lua_State* L);
    static int neighborGetOutfitProperty(lua_State* L);
    static int personNew(lua_State* L);
    static int personGetPersonData(lua_State* L);
    static int personSetPersonData(lua_State* L);
    static int personGetGuid(lua_State* L);
    static int personGetMotive(lua_State* L);
    static int managerNew(lua_State* L);
    static int managerGetPersonIdFromGUID(lua_State* L);
    static int managerGetObjectIdFromGUID(lua_State* L);
    static int managerGetObjectGUIDFromObjectId(lua_State* L);
    static int managerGetObjectIds(lua_State* L);
    static int managerGetNeighborIds(lua_State* L);
    static int managerGetPersonIds(lua_State* L);
    static int managerVerifyObjectId(lua_State* L);
    static int managerVerifyNeighborId(lua_State* L);
    static int neighborhoodNew(lua_State* L);
    static int neighborhoodGetNumOfType(lua_State* L);
    static int neighborhoodGetIds(lua_State* L);
    static int neighborhoodGetType(lua_State* L);
    static int inventoryNew(lua_State* L);
    static int tokenNew(lua_State* L);
    static int objectDataTableNew(lua_State* L);
    static int dataTableHas(lua_State* L);
    static int dataTableGetValue(lua_State* L);
    static int dataTableEmpty(lua_State* L);
    static int dataTableEmptyName(lua_State* L);
    static int neighborhoodClassNew(lua_State* L);
    static int neighborhoodIsValidLot(lua_State* L);
    static int neighborhoodGetLotType(lua_State* L);
    static int neighborhoodGetLotName(lua_State* L);
    static int neighborhoodGetLotIds(lua_State* L);
    static int worldGetObjectTile(lua_State* L);
    static int worldGetObjectLocation(lua_State* L);
    static int worldGetObjectsOnTile(lua_State* L);
    static int worldGetTerrainElevation(lua_State* L);
    static int worldMoveObjectToLocation(lua_State* L);
    static int dataTableGetObjectValue(lua_State* L);
    static int dataTableGetNeighborValue(lua_State* L);
    static int gameStateGetSaveLotEnabled(lua_State* L);
    static int gameStateGetTransitionsEnabled(lua_State* L);
    static int gameStateGetLotTripInfo(lua_State* L);
    static int gameStateIsEpInstalled(lua_State* L);
    static int gameStateGetInstalledEps(lua_State* L);
    static int gameStateGetCurrentState(lua_State* L);
    static int gameStateGetProperty(lua_State* L);
    static int unknownMethod(lua_State* L);
    static int stubFunction(lua_State* L);
    static int luaPrint(lua_State* L);
};

// --- the Lua state ---------------------------------------------------------

LuaRuntime::LuaRuntime(VM& vm) : vm_(vm) {
    L_ = lua_open();
    if (!L_)
        throw VMError("cannot create the Lua state");
    // The base library (Lua 5.0's standard set; the scripts use ipairs,
    // setmetatable and table.getn, never `#`).
    luaopen_base(L_);
    luaopen_table(L_);
    luaopen_string(L_);
    luaopen_math(L_);
    lua_settop(L_, 0);
    // The game's Lua adds math.round (Lua 5.0's math has no round; the
    // ObjectScripts use it, e.g. ShoppingSetValueByPercent).
    lua_getglobal(L_, "math");
    lua_pushstring(L_, "round");
    lua_pushcfunction(L_, &LuaRuntime::mathRound);
    lua_settable(L_, -3);
    lua_pop(L_, 1);
    // The runtime for the C functions.
    lua_pushstring(L_, "os2.lua.runtime");
    lua_pushlightuserdata(L_, this);
    lua_rawset(L_, LUA_REGISTRYINDEX);
    registerApi();
    registerWorldTables();
    // The engine's print goes to its log; here only with OS2_TRACE_LUA.
    lua_pushcfunction(L_, &LuaRuntime::luaPrint);
    lua_setglobal(L_, "print");
    loadPreludes();
}

LuaRuntime::~LuaRuntime() {
    if (L_)
        lua_close(L_);
}

LuaRuntime* LuaRuntime::self(lua_State* L) {
    lua_pushstring(L, "os2.lua.runtime");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* r = static_cast<LuaRuntime*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return r;
}

bool LuaRuntime::run(Context& c, const std::string& name) {
    const std::vector<uint8_t>* chunk = nullptr;
    try {
        chunk = vm_.code().luaScript(name);
    } catch (const std::exception& e) {
        logOnce("lua script", "scan " + name, e);
        return false;
    }
    if (!chunk) {
        logOnce("lua script", "no " + name, "0x7E: no ObjectScripts script \"" + name + "\"");
        return false;
    }
    // The context is the running 0x7E's: its frame's parameters, the thread's
    // temps and the frame's locals (GetTreeParameter and the rest). A script
    // may run another one through the VM, so it is saved and restored.
    Context* saved = current_;
    current_ = &c;
    returned_ = false;
    const bool result = runChunk(reinterpret_cast<const char*>(chunk->data()), chunk->size(), name);
    current_ = saved;
    if (traceLua())
        std::fprintf(stderr, "lua: \"%s\" -> %s\n", name.c_str(), result ? "true" : "false");
    return result;
}

bool LuaRuntime::runChunk(const char* data, size_t size, const std::string& name) {
    lua_settop(L_, 0);
    if (luaL_loadbuffer(L_, data, size, name.c_str()) != 0) {
        const char* msg = lua_tostring(L_, -1);
        logOnce("lua script", "load " + name, "0x7E: cannot load \"" + name + "\": " + (msg ? msg : "?"));
        lua_settop(L_, 0);
        return false;
    }
    int status = 0;
    try {
        status = lua_pcall(L_, 0, 0, 0);
    } catch (const std::exception& e) {
        // No API function throws on purpose; this is the last resort for a
        // lazy load under it (a malformed package). Throw no further.
        logOnce("lua script", "throw " + name, e);
        lua_settop(L_, 0);
        return false;
    }
    if (status != 0) {
        const char* msg = lua_tostring(L_, -1);
        logOnce("lua script", "run " + name, "0x7E: \"" + name + "\" failed: " + (msg ? msg : "?"));
        lua_settop(L_, 0);
        return false;
    }
    lua_settop(L_, 0);
    return returned_;
}

void LuaRuntime::registerApi() {
    static const luaL_reg api[] = {
        {"SetScriptReturnValue", &LuaRuntime::setScriptReturnValue},
        {"GetStackObjectId", &LuaRuntime::getStackObjectId},
        {"GetRunningObjectId", &LuaRuntime::getRunningObjectId},
        {"GetRunningOnObjectId", &LuaRuntime::getRunningOnObjectId},
        {"GetTemp", &LuaRuntime::getTemp},
        {"SetTemp", &LuaRuntime::setTemp},
        {"GetLocal", &LuaRuntime::getLocal},
        {"SetLocal", &LuaRuntime::setLocal},
        {"GetTreeParameter", &LuaRuntime::getTreeParameter},
        {"SetTreeParameter", &LuaRuntime::setTreeParameter},
        {"GetPrimitiveParameter", &LuaRuntime::getPrimitiveParameter},
        {"GetPersonData", &LuaRuntime::getPersonData},
        {"SetPersonData", &LuaRuntime::setPersonData},
        {"GetNeighborPersonData", &LuaRuntime::getNeighborPersonData},
        {"SetNeighborPersonData", &LuaRuntime::setNeighborPersonData},
        {"GetObjectData", &LuaRuntime::getObjectData},
        {"SetObjectData", &LuaRuntime::setObjectData},
        {"GetObjectAttribute", &LuaRuntime::getObjectAttribute},
        {"SetObjectAttribute", &LuaRuntime::setObjectAttribute},
        {"GetObjectSemiAttribute", &LuaRuntime::getObjectSemiAttribute},
        {"SetObjectSemiAttribute", &LuaRuntime::setObjectSemiAttribute},
        {"GetObjectDefinitionField", &LuaRuntime::getObjectDefinitionField},
        {"GetObjectArrayCopy", &LuaRuntime::getObjectArrayCopy},
        {"SetObjectArray", &LuaRuntime::setObjectArray},
        {"GetNumObjectArrays", &LuaRuntime::getNumObjectArrays},
        {"GetObjectIdInSlot", &LuaRuntime::getObjectIdInSlot},
        {"GetConstant", &LuaRuntime::getConstant},
        {"GetNumConstants", &LuaRuntime::getNumConstants},
        {"GetNumConstantsWithGuid", &LuaRuntime::getNumConstantsWithGuid},
        {"GetConstantWithGuid", &LuaRuntime::getConstantWithGuid},
        {"GetConstantsTableByGuid", &LuaRuntime::getConstantsTableByGuid},
        {"GetSimulatorGlobal", &LuaRuntime::getSimulatorGlobal},
        {"SetSimulatorGlobal", &LuaRuntime::setSimulatorGlobal},
        {"GetNeighborRelationship", &LuaRuntime::getNeighborRelationship},
        {"SetNeighborRelationship", &LuaRuntime::setNeighborRelationship},
        {"GetUserDirectory", &LuaRuntime::getUserDirectory},
        {"CreateObjectGUID", &LuaRuntime::createObjectGuid},
        {"SplitObjectGUID", &LuaRuntime::splitObjectGuid},
        {"RoundFloat", &LuaRuntime::roundFloat},
        {"BitwiseAnd", &LuaRuntime::bitwiseAnd},
        {"BitwiseOr", &LuaRuntime::bitwiseOr},
        {"BitwiseXOr", &LuaRuntime::bitwiseXOr},
        {"BitShiftLeft", &LuaRuntime::bitShiftLeft},
        {"BitShiftRight", &LuaRuntime::bitShiftRight},
        {"SetBit", &LuaRuntime::setBit},
        {"ClearBit", &LuaRuntime::clearBit},
        {"IsBitSet", &LuaRuntime::isBitSet},
        {nullptr, nullptr},
    };
    lua_pushvalue(L_, LUA_GLOBALSINDEX);
    luaL_openlib(L_, nullptr, api, 0);
    lua_pop(L_, 1);

    // The objects the game defines in C, not in a prelude (docs/lua-scripts.md:
    // CachedNeighbor, CachedPerson, ObjectManager).
    static const luaL_reg neighborMethods[] = {
        {"getPersonData", &LuaRuntime::neighborGetPersonData},
        {"setPersonData", &LuaRuntime::neighborSetPersonData},
        {"getGUID", &LuaRuntime::neighborGetGuid},
        {"getName", &LuaRuntime::neighborGetName},
        {"getSurname", &LuaRuntime::neighborGetSurname},
        {"getFullName", &LuaRuntime::neighborGetFullName},
        {"getOutfitProperty", &LuaRuntime::neighborGetOutfitProperty},
        {nullptr, nullptr},
    };
    makeClass("CachedNeighbor", neighborMethods, &LuaRuntime::neighborNew);

    static const luaL_reg personMethods[] = {
        {"getPersonData", &LuaRuntime::personGetPersonData},
        {"setPersonData", &LuaRuntime::personSetPersonData},
        {"getGUID", &LuaRuntime::personGetGuid},
        {"getMotive", &LuaRuntime::personGetMotive},
        {nullptr, nullptr},
    };
    makeClass("CachedPerson", personMethods, &LuaRuntime::personNew);

    static const luaL_reg managerMethods[] = {
        {"getPersonIdFromGUID", &LuaRuntime::managerGetPersonIdFromGUID},
        {"getObjectIdFromGUID", &LuaRuntime::managerGetObjectIdFromGUID},
        {"getObjectGUIDFromObjectId", &LuaRuntime::managerGetObjectGUIDFromObjectId},
        {"getObjectIds", &LuaRuntime::managerGetObjectIds},
        {"getNeighborIds", &LuaRuntime::managerGetNeighborIds},
        {"getPersonIds", &LuaRuntime::managerGetPersonIds},
        {"verifyObjectId", &LuaRuntime::managerVerifyObjectId},
        {"verifyNeighborId", &LuaRuntime::managerVerifyNeighborId},
        {nullptr, nullptr},
    };
    makeClass("ObjectManager", managerMethods, &LuaRuntime::managerNew);
    // The query and species constants of FUN_00911180 (the scripts pass them
    // to getObjectIds/getNeighborIds).
    const struct {
        const char* name;
        int value;
    } managerConstants[] = {
        {"kQuery_All", 0}, {"kQuery_ByGUID", 1}, {"kQuery_ForPartsofMultiTileObject", 2},
        {"kQuery_ByObjectDataField", 3}, {"kQuery_ByAccessibleRooms", 4},
        {"kQuery_ByAccessableRooms", 4}, {"kSpeciesFlag_Human", 1}, {"kSpeciesFlag_DogLarge", 2},
        {"kSpeciesFlag_DogSmall", 4}, {"kSpeciesFlag_Cat", 8}, {"kSpeciesMask_All", 0},
    };
    lua_getglobal(L_, "ObjectManager");
    for (const auto& c : managerConstants) {
        lua_pushstring(L_, c.name);
        lua_pushnumber(L_, c.value);
        lua_settable(L_, -3);
    }
    lua_pop(L_, 1);

    static const luaL_reg neighborhoodMethods[] = {
        {"getNumNeighborhoodsOfType", &LuaRuntime::neighborhoodGetNumOfType},
        {"getNeighborhoodIds", &LuaRuntime::neighborhoodGetIds},
        {"getNeighborhoodType", &LuaRuntime::neighborhoodGetType},
        {nullptr, nullptr},
    };
    makeClass("NeighborhoodManager", neighborhoodMethods, &LuaRuntime::neighborhoodNew);

    // Neighborhood: the lot queries of FUN_0090db40. The port knows the
    // neighborhood's lots (VM::lots: zoning, position, name); the apartment
    // ones (roommates, linked lots) it does not model yet, and the class
    // fallback logs them and answers 0.
    static const luaL_reg neighborhoodLotMethods[] = {
        {"isValidLot", &LuaRuntime::neighborhoodIsValidLot},
        {"getLotType", &LuaRuntime::neighborhoodGetLotType},
        {"getLotName", &LuaRuntime::neighborhoodGetLotName},
        {"getLotIds", &LuaRuntime::neighborhoodGetLotIds},
        {nullptr, nullptr},
    };
    makeClass("Neighborhood", neighborhoodLotMethods, &LuaRuntime::neighborhoodClassNew);

    // ObjectDataTable and NeighborDataTable: the engine's generic data tables
    // (FUN_00910cd0/FUN_00910f30), which the business and shop scripts use
    // through nDataTable. The port does not store them, so every table and
    // value is absent: has* false, getTableValue nil, the id/key/value lists
    // empty (the scripts test for those, e.g. ShoppingGetPrice).
    static const luaL_reg objectTableMethods[] = {
        {"hasTable", &LuaRuntime::dataTableHas},
        {"hasTableValue", &LuaRuntime::dataTableHas},
        {"getTableValue", &LuaRuntime::dataTableGetValue},
        {"getTableIds", &LuaRuntime::dataTableEmpty},
        {"getTableKeys", &LuaRuntime::dataTableEmpty},
        {"getTableValues", &LuaRuntime::dataTableEmpty},
        {"getTableName", &LuaRuntime::dataTableEmptyName},
        {nullptr, nullptr},
    };
    static const luaL_reg neighborTableMethods[] = {
        {"hasTable", &LuaRuntime::dataTableHas},
        {"hasTableValue", &LuaRuntime::dataTableHas},
        {"getTableValue", &LuaRuntime::dataTableGetValue},
        {"getTableIds", &LuaRuntime::dataTableEmpty},
        {"getTableKeys", &LuaRuntime::dataTableEmpty},
        {"getTableValues", &LuaRuntime::dataTableEmpty},
        {"getTableName", &LuaRuntime::dataTableEmptyName},
        {nullptr, nullptr},
    };
    makeClass("ObjectDataTable", objectTableMethods, &LuaRuntime::objectDataTableNew);
    makeClass("NeighborDataTable", neighborTableMethods, &LuaRuntime::objectDataTableNew);

    // Inventory and InventoryToken: the class's CONSTANTS are what the game's
    // preludes read (the "Tokens" prelude builds gInventoryTypes/gTokenFlags
    // from them); the methods over the port's inventory model are not
    // measured yet, so the class fallback logs them and answers 0.
    static const luaL_reg noMethods[] = {{nullptr, nullptr}};
    makeClass("Inventory", noMethods, &LuaRuntime::inventoryNew);
    makeClass("InventoryToken", noMethods, &LuaRuntime::tokenNew);
    const struct {
        const char* className;
        const char* field;
        int value;
    } inventoryConstants[] = {
        {"Inventory", "kTypeGlobal", 0},     {"Inventory", "kTypeLot", 1},
        {"Inventory", "kTypeFamily", 2},     {"Inventory", "kTypeNeighbor", 3},
        {"Inventory", "kTypeGameWide", 4},   {"InventoryToken", "kFlagVisible", tokenflag::Visible},
        {"InventoryToken", "kFlagMemory", tokenflag::Memory},
        {"InventoryToken", "kFlagMarkedForDeletion", tokenflag::MarkedForDeletion},
        {"InventoryToken", "kFlagShopping", tokenflag::Shopping},
    };
    for (const auto& c : inventoryConstants) {
        lua_getglobal(L_, c.className);
        lua_pushstring(L_, c.field);
        lua_pushnumber(L_, c.value);
        lua_settable(L_, -3);
        lua_pop(L_, 1);
    }
}

void LuaRuntime::registerWorldTables() {
    // nWorld: the world's queries (FUN_0090e7e0 registers the Lua "nWorld").
    lua_newtable(L_);
    const struct {
        const char* name;
        lua_CFunction fn;
    } world[] = {
        {"GetObjectTile", &LuaRuntime::worldGetObjectTile},
        {"GetObjectLocation", &LuaRuntime::worldGetObjectLocation},
        {"GetObjectsOnTile", &LuaRuntime::worldGetObjectsOnTile},
        {"GetTerrainElevationForObject", &LuaRuntime::worldGetTerrainElevation},
        {"MoveObjectToLocation", &LuaRuntime::worldMoveObjectToLocation},
    };
    for (const auto& f : world) {
        lua_pushstring(L_, f.name);
        lua_pushcfunction(L_, f.fn);
        lua_settable(L_, -3);
    }
    lua_setglobal(L_, "nWorld");

    // nGameState: the game's own switches.
    lua_newtable(L_);
    const struct {
        const char* name;
        lua_CFunction fn;
    } state[] = {
        {"GetSaveLotEnabled", &LuaRuntime::gameStateGetSaveLotEnabled},
        {"GetTransitionsEnabled", &LuaRuntime::gameStateGetTransitionsEnabled},
        {"GetLotTripInfo", &LuaRuntime::gameStateGetLotTripInfo},
        {"IsEPInstalled", &LuaRuntime::gameStateIsEpInstalled},
        {"GetInstalledEps", &LuaRuntime::gameStateGetInstalledEps},
        {"GetCurrentState", &LuaRuntime::gameStateGetCurrentState},
        {"GetProperty", &LuaRuntime::gameStateGetProperty},
        {"SetProperty", &LuaRuntime::gameStateGetProperty},
    };
    for (const auto& f : state) {
        lua_pushstring(L_, f.name);
        lua_pushcfunction(L_, f.fn);
        lua_settable(L_, -3);
    }
    lua_setglobal(L_, "nGameState");

    // nDataTable: the generic data tables of the objects and neighbors
    // (FUN_00911120: GetObjectTableValue/GetNeighborTableValue). The port
    // does not model them; a missing table/row is nil, as the engine's.
    lua_newtable(L_);
    lua_pushstring(L_, "GetObjectTableValue");
    lua_pushcfunction(L_, &LuaRuntime::dataTableGetObjectValue);
    lua_settable(L_, -3);
    lua_pushstring(L_, "GetNeighborTableValue");
    lua_pushcfunction(L_, &LuaRuntime::dataTableGetNeighborValue);
    lua_settable(L_, -3);
    lua_setglobal(L_, "nDataTable");
}

void LuaRuntime::makeClass(const char* name, const luaL_reg* methods, lua_CFunction ctor) {
    lua_newtable(L_);                          // [methods]
    luaL_openlib(L_, nullptr, methods, 0);     // fills [methods]
    // Its metatable: a method the port does not have logs and returns a stub
    // (the game's class tables are closed; this keeps a script from failing
    // on an API this port has not measured yet).
    lua_newtable(L_);                          // [methods][fallback]
    lua_pushstring(L_, "__index");
    lua_pushcfunction(L_, &LuaRuntime::unknownMethod);
    lua_settable(L_, -3);
    lua_setmetatable(L_, -2);                  // methods' metatable = fallback
    // The instances' metatable: __index = the methods table.
    lua_newtable(L_);                          // [methods][mt]
    lua_pushstring(L_, "__index");
    lua_pushvalue(L_, -3);                     // the methods table
    lua_settable(L_, -3);
    lua_pushstring(L_, "__metatable");
    lua_pushboolean(L_, 1);
    lua_settable(L_, -3);
    lua_pushstring(L_, name);                  // registry[name] = mt
    lua_pushvalue(L_, -2);
    lua_rawset(L_, LUA_REGISTRYINDEX);
    lua_pop(L_, 1);                            // [methods]
    lua_pop(L_, 1);                            // []
    // The class table: `new`.
    lua_newtable(L_);
    lua_pushstring(L_, "new");
    lua_pushcfunction(L_, ctor);
    lua_settable(L_, -3);
    lua_setglobal(L_, name);
}

void LuaRuntime::pushInstance(lua_State* L, const char* className, int16_t nid) {
    lua_newtable(L);
    lua_pushstring(L, "nid");
    lua_pushnumber(L, nid);
    lua_settable(L, -3);
    luaL_getmetatable(L, className);
    lua_setmetatable(L, -2);
}

void LuaRuntime::loadPreludes() {
    // The `.globalObjLua` chunks of the installation, Global first and then by
    // name (CodeLibrary::luaPreludes): the classes and the data-field tables
    // (PersonData, ObjDef, ObjData, NPCTypeToGUID, ...) the scripts use.
    const auto& preludes = vm_.code().luaPreludes();
    const auto& names = vm_.code().luaPreludeNames();
    if (preludes.empty())
        return;
    size_t loaded = 0;
    for (size_t i = 0; i < preludes.size(); ++i) {
        const auto& chunk = preludes[i];
        const std::string& name = i < names.size() ? names[i] : std::to_string(i);
        lua_settop(L_, 0);
        if (luaL_loadbuffer(L_, reinterpret_cast<const char*>(chunk.data()), chunk.size(), name.c_str()) != 0 ||
            lua_pcall(L_, 0, 0, 0) != 0) {
            const char* msg = lua_tostring(L_, -1);
            logOnce("lua prelude", name, "Lua prelude \"" + name + "\" failed: " + (msg ? msg : "?"));
            lua_settop(L_, 0);
            continue;
        }
        ++loaded;
    }
    if (traceLua())
        std::fprintf(stderr, "lua: %zu preludes loaded\n", loaded);
}

// --- the running thread and frame -----------------------------------------

int LuaRuntime::setScriptReturnValue(lua_State* L) {
    if (LuaRuntime* r = self(L))
        r->returned_ = lua_toboolean(L, 1) != 0;
    return 0; // the engine's FUN_008fc6b0 returns no values
}

int LuaRuntime::getStackObjectId(lua_State* L) {
    LuaRuntime* r = self(L);
    lua_pushnumber(L, r && r->current_ ? r->current_->frame.stackObject : 0);
    return 1;
}

int LuaRuntime::getRunningObjectId(lua_State* L) {
    LuaRuntime* r = self(L);
    lua_pushnumber(L, r && r->current_ ? r->current_->thread.me : 0);
    return 1;
}

int LuaRuntime::getRunningOnObjectId(lua_State* L) {
    LuaRuntime* r = self(L);
    lua_pushnumber(L, r && r->current_ ? r->current_->frame.stackObject : 0);
    return 1;
}

int LuaRuntime::getTemp(lua_State* L) {
    LuaRuntime* r = self(L);
    const int i = argInt(L, 1);
    int16_t v = 0;
    if (r && r->current_ && i >= 0 && size_t(i) < kTempCount)
        v = r->current_->thread.temps[size_t(i)];
    else
        logOnce("lua api", "GetTemp " + std::to_string(i), "Lua: temp " + std::to_string(i) + " out of range");
    lua_pushnumber(L, v);
    return 1;
}

int LuaRuntime::setTemp(lua_State* L) {
    LuaRuntime* r = self(L);
    const int i = argInt(L, 1);
    if (r && r->current_ && i >= 0 && size_t(i) < kTempCount)
        r->current_->thread.temps[size_t(i)] = int16_t(argInt(L, 2));
    else
        logOnce("lua api", "SetTemp " + std::to_string(i), "Lua: temp " + std::to_string(i) + " out of range");
    return 0;
}

int LuaRuntime::getLocal(lua_State* L) {
    LuaRuntime* r = self(L);
    const int i = argInt(L, 1);
    int16_t v = 0;
    if (r && r->current_) {
        const auto& locals = r->current_->frame.locals;
        if (i >= 0 && size_t(i) < locals.size())
            v = locals[size_t(i)];
        else
            logOnce("lua api", "GetLocal " + std::to_string(i), "Lua: local " + std::to_string(i) + " out of range");
    }
    lua_pushnumber(L, v);
    return 1;
}

int LuaRuntime::setLocal(lua_State* L) {
    LuaRuntime* r = self(L);
    const int i = argInt(L, 1);
    if (r && r->current_) {
        auto& locals = r->current_->frame.locals;
        if (i >= 0 && size_t(i) < locals.size())
            locals[size_t(i)] = int16_t(argInt(L, 2));
        else
            logOnce("lua api", "SetLocal " + std::to_string(i), "Lua: local " + std::to_string(i) + " out of range");
    }
    return 0;
}

int LuaRuntime::getTreeParameter(lua_State* L) {
    LuaRuntime* r = self(L);
    const int i = argInt(L, 1);
    int16_t v = 0;
    if (r && r->current_) {
        const auto& params = r->current_->frame.params;
        if (i >= 0 && size_t(i) < params.size())
            v = params[size_t(i)];
        else
            logOnce("lua api", "GetTreeParameter " + std::to_string(i),
                    "Lua: the tree has no parameter " + std::to_string(i));
    }
    lua_pushnumber(L, v);
    return 1;
}

int LuaRuntime::setTreeParameter(lua_State* L) {
    LuaRuntime* r = self(L);
    const int i = argInt(L, 1);
    if (r && r->current_) {
        auto& params = r->current_->frame.params;
        if (i >= 0 && size_t(i) < params.size())
            params[size_t(i)] = int16_t(argInt(L, 2));
    }
    return 0;
}

int LuaRuntime::getPrimitiveParameter(lua_State* L) {
    // The 0x7E's own operand parameters (the three (type, value) pairs the
    // handler logs); no script of the game reads them. An owner that needs a
    // stack object when the frame has none is 0, not an error.
    LuaRuntime* r = self(L);
    const int i = argInt(L, 1);
    int16_t v = 0;
    if (r && r->current_ && i >= 0 && i < 3) {
        const auto& o = r->current_->instruction.operands;
        const uint8_t owner = o[6 + 3 * size_t(i)];
        const int16_t data = int16_t(uint16_t(o[7 + 3 * size_t(i)]) | uint16_t(o[8 + 3 * size_t(i)]) << 8);
        try {
            v = r->current_->get(owner, data);
        } catch (const std::exception&) {
            v = 0;
        }
    }
    lua_pushnumber(L, v);
    return 1;
}

// --- objects, people and definitions ---------------------------------------

Object* LuaRuntime::object(int id) {
    Object* o = vm_.object(ObjectId(id));
    if (!o)
        logOnce("lua api", "object " + std::to_string(id), "Lua: no object " + std::to_string(id));
    return o;
}

int16_t* LuaRuntime::personData(int16_t nid, int field) {
    if (field < 0 || size_t(field) >= kPersonDataCount || nid <= 0)
        return nullptr;
    // The person of a Sim on the lot is his object's; a neighbor's is the
    // neighborhood's copy (hood_bridge.cpp personDataOf).
    if (Object* sim = vm_.neighborSim(nid); sim && sim->person)
        return &sim->person->data[size_t(field)];
    if (Neighbor* n = vm_.neighbor(nid))
        return &n->personData[size_t(field)];
    return nullptr;
}

int LuaRuntime::getPersonData(lua_State* L) {
    LuaRuntime* r = self(L);
    const int nid = argInt(L, 1);
    const int field = argInt(L, 2);
    int16_t v = 0;
    if (r) {
        if (int16_t* p = r->personData(int16_t(nid), field))
            v = *p;
        else
            logOnce("lua api", "GetPersonData " + std::to_string(nid) + "/" + std::to_string(field),
                    "Lua: no person data " + std::to_string(field) + " of neighbor " + std::to_string(nid));
    }
    lua_pushnumber(L, v);
    return 1;
}

int LuaRuntime::setPersonData(lua_State* L) {
    LuaRuntime* r = self(L);
    const int nid = argInt(L, 1);
    const int field = argInt(L, 2);
    if (r) {
        if (int16_t* p = r->personData(int16_t(nid), field))
            *p = int16_t(argInt(L, 3));
        else
            logOnce("lua api", "SetPersonData " + std::to_string(nid) + "/" + std::to_string(field),
                    "Lua: no person data " + std::to_string(field) + " of neighbor " + std::to_string(nid));
    }
    return 0;
}

int LuaRuntime::getNeighborPersonData(lua_State* L) { return getPersonData(L); }
int LuaRuntime::setNeighborPersonData(lua_State* L) { return setPersonData(L); }

int LuaRuntime::getObjectData(lua_State* L) {
    LuaRuntime* r = self(L);
    int16_t v = 0;
    if (r)
        if (Object* o = r->object(argInt(L, 1))) {
            const int field = argInt(L, 2);
            if (field >= 0 && size_t(field) < kObjectDataCount)
                v = o->data[size_t(field)];
            else
                logOnce("lua api", "GetObjectData " + std::to_string(field),
                        "Lua: object data " + std::to_string(field) + " out of range");
        }
    lua_pushnumber(L, v);
    return 1;
}

int LuaRuntime::setObjectData(lua_State* L) {
    LuaRuntime* r = self(L);
    if (r)
        if (Object* o = r->object(argInt(L, 1))) {
            const int field = argInt(L, 2);
            if (field >= 0 && size_t(field) < kObjectDataCount)
                o->data[size_t(field)] = int16_t(argInt(L, 3));
            else
                logOnce("lua api", "SetObjectData " + std::to_string(field),
                        "Lua: object data " + std::to_string(field) + " out of range");
        }
    return 0;
}

namespace {
template <typename Values>
int16_t readAt(const Values& v, int i, const char* what) {
    if (i < 0 || size_t(i) >= v.size()) {
        logOnce("lua api", std::string(what) + " " + std::to_string(i),
                "Lua: " + std::string(what) + " " + std::to_string(i) + " out of range");
        return 0;
    }
    return v[size_t(i)];
}
} // namespace

int LuaRuntime::getObjectAttribute(lua_State* L) {
    LuaRuntime* r = self(L);
    int16_t v = 0;
    if (r)
        if (Object* o = r->object(argInt(L, 1)))
            v = readAt(o->attributes, argInt(L, 2), "attribute");
    lua_pushnumber(L, v);
    return 1;
}

int LuaRuntime::setObjectAttribute(lua_State* L) {
    LuaRuntime* r = self(L);
    if (r)
        if (Object* o = r->object(argInt(L, 1))) {
            const int i = argInt(L, 2);
            if (i >= 0 && size_t(i) < o->attributes.size()) {
                o->attributes[size_t(i)] = int16_t(argInt(L, 3));
                // The Lua API writes attributes as the trees do; nothing else
                // reads them through an index.
            } else {
                logOnce("lua api", "SetObjectAttribute " + std::to_string(i),
                        "Lua: attribute " + std::to_string(i) + " out of range");
            }
        }
    return 0;
}

int LuaRuntime::getObjectSemiAttribute(lua_State* L) {
    LuaRuntime* r = self(L);
    int16_t v = 0;
    if (r)
        if (Object* o = r->object(argInt(L, 1)))
            v = readAt(o->semiAttributes, argInt(L, 2), "semi-attribute");
    lua_pushnumber(L, v);
    return 1;
}

int LuaRuntime::setObjectSemiAttribute(lua_State* L) {
    LuaRuntime* r = self(L);
    if (r)
        if (Object* o = r->object(argInt(L, 1))) {
            const int i = argInt(L, 2);
            if (i >= 0 && size_t(i) < o->semiAttributes.size())
                o->semiAttributes[size_t(i)] = int16_t(argInt(L, 3));
            else
                logOnce("lua api", "SetObjectSemiAttribute " + std::to_string(i),
                        "Lua: semi-attribute " + std::to_string(i) + " out of range");
        }
    return 0;
}

namespace {

// GetObjectDefinitionField's index: 0 and 1 are the two halves of the OBJD's
// version, 2..107 the labelled fields (label i is the field at 0x40 + 2i,
// formulas in simantics.hpp), and 201+ the definition's own values (the
// ObjectDefinition.globalObjLua table: version, guid, original guid, model
// guid, type attribute guid, the selectors, the proxy and job object GUIDs and
// the valid EP bit field). 204, 205 and 210 are inferred from that table's
// names and the OBJD labels; the rest is measured.
int32_t definitionFieldValue(const ObjectDefinition& d, int i) {
    if (i < 0)
        return 0;
    if (i < 2)
        return int16_t(d.version >> (16 * size_t(i)));
    if (i <= 107)
        return int16_t(d.field(0x40 + 2 * size_t(i)));
    switch (i) {
    case 201: return int32_t(d.version);
    case 202: return int32_t(d.guid());
    case 203: return int32_t(d.originalGuid());
    case 204: return int32_t(d.field32(0xCE)); // object model guid (labels 71/72)
    case 205: return int32_t(d.field32(0xF6)); // type attribute guid (labels 91/92)
    case 206: return int32_t(d.diagonalSelector());
    case 207: return int32_t(d.gridAlignedSelector());
    case 208: return int32_t(d.proxyGuid());
    case 209: return int32_t(d.field32(0xA8)); // job object guid (labels 52/53)
    case 210: return int32_t(d.field32(0xC0)); // valid EP flags 1/2 (labels 64/65)
    default: return 0;
    }
}

} // namespace

int LuaRuntime::getObjectDefinitionField(lua_State* L) {
    LuaRuntime* r = self(L);
    const int32_t guid = int32_t(argInt(L, 1));
    const int field = argInt(L, 2);
    int32_t v = 0;
    if (r) {
        // The OBJD scan reads and parses the package lazily: a malformed
        // definition must not throw through Lua's C frames.
        try {
            const auto loc = r->vm_.code().definitionByGuid(uint32_t(guid));
            const ObjectDefinition* def = loc ? r->vm_.code().definition(loc->first, loc->second) : nullptr;
            if (!def)
                logOnce("lua api", "definition " + std::to_string(guid),
                        "Lua: no object definition for GUID " + std::to_string(guid));
            else
                v = definitionFieldValue(*def, field);
        } catch (const std::exception& e) {
            logOnce("lua api", "definition " + std::to_string(guid), e);
        }
    }
    pushInt32(L, uint32_t(v));
    return 1;
}

int LuaRuntime::getObjectArrayCopy(lua_State* L) {
    LuaRuntime* r = self(L);
    if (!r) {
        lua_newtable(L);
        return 1;
    }
    Object* o = r->object(argInt(L, 1));
    if (!o) {
        lua_newtable(L);
        return 1;
    }
    const int array = argInt(L, 2);
    if (array < 0 || size_t(array) >= o->arrays.size()) {
        logOnce("lua api", "GetObjectArrayCopy " + std::to_string(array),
                "Lua: object array " + std::to_string(array) + " out of range");
        lua_newtable(L);
        return 1;
    }
    // The engine builds a Lua table, 1-based, of the array's values
    // (FUN_00900e30: `t[i] = value`); the scripts sort it and give it back.
    lua_newtable(L);
    int i = 1;
    for (int16_t v : o->arrays[size_t(array)].values) {
        lua_pushnumber(L, v);
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

int LuaRuntime::setObjectArray(lua_State* L) {
    LuaRuntime* r = self(L);
    if (!r)
        return 0;
    Object* o = r->object(argInt(L, 1));
    if (!o)
        return 0;
    const int array = argInt(L, 2);
    if (array < 0 || size_t(array) >= o->arrays.size()) {
        logOnce("lua api", "SetObjectArray " + std::to_string(array),
                "Lua: object array " + std::to_string(array) + " out of range");
        return 0;
    }
    if (!lua_istable(L, 3)) {
        logOnce("lua api", "SetObjectArray not a table", "Lua: SetObjectArray's values are not a table");
        return 0;
    }
    // FUN_0090d4a0: the array becomes the values of the table (1..n).
    auto& values = o->arrays[size_t(array)].values;
    values.clear();
    const int n = luaL_getn(L, 3);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 3, i);
        values.push_back(int16_t(lua_tonumber(L, -1)));
        lua_pop(L, 1);
    }
    return 0;
}

int LuaRuntime::getNumObjectArrays(lua_State* L) {
    LuaRuntime* r = self(L);
    Object* o = r ? r->object(argInt(L, 1)) : nullptr;
    lua_pushnumber(L, o ? int(o->arrays.size()) : 0);
    return 1;
}

int LuaRuntime::getObjectIdInSlot(lua_State* L) {
    LuaRuntime* r = self(L);
    int16_t id = 0;
    if (r)
        if (Object* o = r->object(argInt(L, 1)))
            id = r->vm_.containedIn(o->id, int16_t(argInt(L, 2)));
    lua_pushnumber(L, id);
    return 1;
}

int LuaRuntime::getConstant(lua_State* L) {
    // FUN_009010d0: the stack object's constant table `table` (the BCON that
    // its OBJD references by instance) at `index`. The object's private table
    // first, then its semi-global library's, then the global one.
    LuaRuntime* r = self(L);
    const int table = argInt(L, 1);
    const int index = argInt(L, 2);
    int16_t v = 0;
    if (r && r->current_) {
        try {
            const Object& so = r->current_->stackObject();
            const Constants* c = r->vm_.code().constants(so.group, uint16_t(table));
            if (!c && so.semiGroup)
                c = r->vm_.code().constants(so.semiGroup, uint16_t(table));
            if (!c)
                c = r->vm_.code().constants(kGlobalGroup, uint16_t(table));
            if (!c)
                logOnce("lua api", "GetConstant " + std::to_string(table),
                        "Lua: no constant table " + std::to_string(table));
            else if (index >= 0 && size_t(index) < c->values.size())
                v = c->values[size_t(index)];
            else
                logOnce("lua api", "GetConstant index " + std::to_string(index),
                        "Lua: constant " + std::to_string(index) + " out of range");
        } catch (const std::exception&) {
            v = 0; // no stack object
        }
    }
    lua_pushnumber(L, v);
    return 1;
}

namespace {
const Constants* constantsOfGuid(LuaRuntime& r, int32_t guid, int table) {
    const auto loc = r.vmm().code().definitionByGuid(uint32_t(guid));
    if (!loc)
        return nullptr;
    const ObjectDefinition* def = r.vmm().code().definition(loc->first, loc->second);
    if (!def)
        return nullptr;
    return r.vmm().code().constants(loc->first, uint16_t(table));
}
} // namespace

int LuaRuntime::getNumConstants(lua_State* L) {
    // FUN_00901200: the stack object's constant table's count.
    LuaRuntime* r = self(L);
    const int table = argInt(L, 1);
    int n = 0;
    if (r && r->current_) {
        try {
            const Object& so = r->current_->stackObject();
            const Constants* c = r->vm_.code().constants(so.group, uint16_t(table));
            if (!c && so.semiGroup)
                c = r->vm_.code().constants(so.semiGroup, uint16_t(table));
            if (!c)
                c = r->vm_.code().constants(kGlobalGroup, uint16_t(table));
            n = c ? int(c->values.size()) : 0;
        } catch (const std::exception&) {
            n = 0;
        }
    }
    lua_pushnumber(L, n);
    return 1;
}

int LuaRuntime::getNumConstantsWithGuid(lua_State* L) {
    LuaRuntime* r = self(L);
    const int32_t guid = int32_t(argInt(L, 1));
    const int table = argInt(L, 2);
    int n = 0;
    if (r) {
        try {
            if (const Constants* c = constantsOfGuid(*r, guid, table))
                n = int(c->values.size());
            else
                logOnce("lua api", "GetNumConstantsWithGuid " + std::to_string(guid) + "/" + std::to_string(table),
                        "Lua: no constant table " + std::to_string(table) + " for GUID " + std::to_string(guid));
        } catch (const std::exception& e) {
            logOnce("lua api", "GetNumConstantsWithGuid", e);
        }
    }
    lua_pushnumber(L, n);
    return 1;
}

int LuaRuntime::getConstantWithGuid(lua_State* L) {
    LuaRuntime* r = self(L);
    const int32_t guid = int32_t(argInt(L, 1));
    const int table = argInt(L, 2);
    const int index = argInt(L, 3);
    int16_t v = 0;
    if (r) {
        try {
            if (const Constants* c = constantsOfGuid(*r, guid, table)) {
                if (index >= 0 && size_t(index) < c->values.size())
                    v = c->values[size_t(index)];
                else
                    logOnce("lua api", "GetConstantWithGuid index " + std::to_string(index),
                            "Lua: constant " + std::to_string(index) + " out of range");
            }
        } catch (const std::exception& e) {
            logOnce("lua api", "GetConstantWithGuid", e);
        }
    }
    lua_pushnumber(L, v);
    return 1;
}

int LuaRuntime::getConstantsTableByGuid(lua_State* L) {
    // FUN_00901590: the whole table as a Lua table (1-based).
    LuaRuntime* r = self(L);
    const int32_t guid = int32_t(argInt(L, 1));
    const int table = argInt(L, 2);
    lua_newtable(L);
    if (r) {
        try {
            if (const Constants* c = constantsOfGuid(*r, guid, table))
                for (size_t i = 0; i < c->values.size(); ++i) {
                    lua_pushnumber(L, c->values[i]);
                    lua_rawseti(L, -2, int(i + 1));
                }
        } catch (const std::exception& e) {
            logOnce("lua api", "GetConstantsTableByGuid", e);
        }
    }
    return 1;
}

int LuaRuntime::getSimulatorGlobal(lua_State* L) {
    LuaRuntime* r = self(L);
    const int i = argInt(L, 1);
    int16_t v = 0;
    if (r && i >= 0 && size_t(i) < kGlobalCount)
        v = r->vm_.globals[size_t(i)];
    else
        logOnce("lua api", "GetSimulatorGlobal " + std::to_string(i),
                "Lua: simulator global " + std::to_string(i) + " out of range");
    lua_pushnumber(L, v);
    return 1;
}

int LuaRuntime::setSimulatorGlobal(lua_State* L) {
    LuaRuntime* r = self(L);
    const int i = argInt(L, 1);
    if (r && i >= 0 && size_t(i) < kGlobalCount)
        r->vm_.globals[size_t(i)] = int16_t(argInt(L, 2));
    else
        logOnce("lua api", "SetSimulatorGlobal " + std::to_string(i),
                "Lua: simulator global " + std::to_string(i) + " out of range");
    return 0;
}

int LuaRuntime::getNeighborRelationship(lua_State* L) {
    // FUN_008fd080: neighbor a's relationship variable `var` toward neighbor
    // b (the pair the Relation primitive uses: {neighbor, a} -> {neighbor, b}).
    LuaRuntime* r = self(L);
    const int16_t a = int16_t(argInt(L, 1));
    const int16_t b = int16_t(argInt(L, 2));
    const int var = argInt(L, 3);
    int16_t v = 0;
    if (r) {
        const VM::RelationshipPair pair{{true, a}, {true, b}};
        const auto it = r->vm_.relationships.find(pair);
        if (it != r->vm_.relationships.end() && var >= 0 && size_t(var) < it->second.size())
            v = it->second[size_t(var)];
    }
    lua_pushnumber(L, v);
    return 1;
}

int LuaRuntime::setNeighborRelationship(lua_State* L) {
    LuaRuntime* r = self(L);
    if (!r)
        return 0;
    const int16_t a = int16_t(argInt(L, 1));
    const int16_t b = int16_t(argInt(L, 2));
    const int var = argInt(L, 3);
    if (var < 0 || size_t(var) >= kMaxRelationshipVars) {
        logOnce("lua api", "SetNeighborRelationship " + std::to_string(var),
                "Lua: relationship variable " + std::to_string(var) + " out of range");
        return 0;
    }
    const VM::RelationshipPair pair{{true, a}, {true, b}};
    auto& entry = r->vm_.relationships[pair];
    if (entry.size() <= size_t(var))
        entry.resize(size_t(var) + 1, 0);
    entry[size_t(var)] = int16_t(argInt(L, 4));
    return 0;
}

int LuaRuntime::getUserDirectory(lua_State* L) {
    // The game's user data directory (mods, saves). The port keeps its own
    // (app/user_data.cpp), not this one; the only prelude that asks
    // ("LuaLoader") uses it to find mods, which the port does not load here.
    (void)L;
    const char* dir = std::getenv("OS2_USER_DIR");
    lua_pushstring(L, dir ? dir : "");
    return 1;
}

int LuaRuntime::createObjectGuid(lua_State* L) {
    // FUN_008fb6a0: (high word << 16) | (low word & 0xFFFF), signed.
    const uint32_t high = uint32_t(argInt(L, 1));
    const uint32_t low = uint32_t(argInt(L, 2));
    pushInt32(L, (high << 16) | (low & 0xFFFF));
    return 1;
}

int LuaRuntime::splitObjectGuid(lua_State* L) {
    // FUN_008fb700: the high word, then the low word.
    const uint32_t guid = uint32_t(argInt(L, 1));
    lua_pushnumber(L, guid >> 16);
    lua_pushnumber(L, guid & 0xFFFF);
    return 2;
}

int LuaRuntime::roundFloat(lua_State* L) {
    // FUN_008fb770: the nearest integer (half away from zero, as the game's).
    const double x = lua_tonumber(L, 1);
    lua_pushnumber(L, std::floor(x + 0.5));
    return 1;
}

int LuaRuntime::mathRound(lua_State* L) {
    const double x = lua_tonumber(L, 1);
    lua_pushnumber(L, std::floor(x + 0.5));
    return 1;
}

int LuaRuntime::inventoryNew(lua_State* L) {
    // Inventory.new(kind, id): the port's inventory model (VM::inventory).
    // The class methods are not measured yet; the class fallback logs them.
    LuaRuntime* r = self(L);
    if (r) {
        r->pushInstance(L, "Inventory", 0);
        lua_pushstring(L, "kind");
        lua_pushnumber(L, argInt(L, 1));
        lua_settable(L, -3);
        lua_pushstring(L, "id");
        lua_pushnumber(L, argInt(L, 2));
        lua_settable(L, -3);
    }
    return 1;
}

int LuaRuntime::tokenNew(lua_State* L) {
    // InventoryToken.new(...): a token copy; its methods are not measured yet.
    (void)L;
    if (LuaRuntime* r = self(L))
        r->pushInstance(L, "InventoryToken", 0);
    return 1;
}

int LuaRuntime::objectDataTableNew(lua_State* L) {
    // ObjectDataTable.new(objectId, tableId) / NeighborDataTable.new(nid, tableId):
    // the instance keeps the two arguments; the port stores no tables.
    LuaRuntime* r = self(L);
    if (r) {
        r->pushInstance(L, "ObjectDataTable", 0);
        lua_pushstring(L, "id");
        lua_pushnumber(L, argInt(L, 1));
        lua_settable(L, -3);
        lua_pushstring(L, "tableID");
        lua_pushnumber(L, argInt(L, 2));
        lua_settable(L, -3);
    }
    return 1;
}

int LuaRuntime::dataTableHas(lua_State* L) {
    (void)L;
    lua_pushboolean(L, 0);
    return 1;
}

int LuaRuntime::dataTableGetValue(lua_State* L) {
    (void)L;
    lua_pushnil(L);
    return 1;
}

int LuaRuntime::dataTableEmpty(lua_State* L) {
    lua_newtable(L);
    return 1;
}

int LuaRuntime::dataTableEmptyName(lua_State* L) {
    (void)L;
    lua_pushstring(L, "");
    return 1;
}

int LuaRuntime::bitwiseAnd(lua_State* L) {
    lua_pushnumber(L, double(int32_t(uint32_t(argInt(L, 1)) & uint32_t(argInt(L, 2)))));
    return 1;
}

int LuaRuntime::bitwiseOr(lua_State* L) {
    lua_pushnumber(L, double(int32_t(uint32_t(argInt(L, 1)) | uint32_t(argInt(L, 2)))));
    return 1;
}

int LuaRuntime::bitwiseXOr(lua_State* L) {
    lua_pushnumber(L, double(int32_t(uint32_t(argInt(L, 1)) ^ uint32_t(argInt(L, 2)))));
    return 1;
}

int LuaRuntime::bitShiftLeft(lua_State* L) {
    const uint32_t v = uint32_t(argInt(L, 1));
    lua_pushnumber(L, double(int32_t(v << (unsigned(argInt(L, 2)) & 31))));
    return 1;
}

int LuaRuntime::bitShiftRight(lua_State* L) {
    const uint32_t v = uint32_t(argInt(L, 1));
    lua_pushnumber(L, double(int32_t(v >> (unsigned(argInt(L, 2)) & 31))));
    return 1;
}

int LuaRuntime::setBit(lua_State* L) {
    // FUN_008fb810: bit (2nd arg) counted from 1.
    const uint32_t v = uint32_t(argInt(L, 1));
    const unsigned bit = unsigned(argInt(L, 2));
    lua_pushnumber(L, double(int32_t(v | (1u << ((bit - 1) & 31)))));
    return 1;
}

int LuaRuntime::clearBit(lua_State* L) {
    const uint32_t v = uint32_t(argInt(L, 1));
    const unsigned bit = unsigned(argInt(L, 2));
    lua_pushnumber(L, double(int32_t(v & ~(1u << ((bit - 1) & 31)))));
    return 1;
}

int LuaRuntime::isBitSet(lua_State* L) {
    const uint32_t v = uint32_t(argInt(L, 1));
    const unsigned bit = unsigned(argInt(L, 2));
    lua_pushnumber(L, (v >> ((bit - 1) & 31)) & 1u);
    return 1;
}

// --- CachedNeighbor, CachedPerson and the managers -------------------------

namespace {
int16_t instanceId(lua_State* L) {
    lua_pushstring(L, "nid");
    lua_rawget(L, 1);
    const int16_t id = int16_t(lua_tonumber(L, -1));
    lua_pop(L, 1);
    return id;
}

uint32_t neighborGuid(LuaRuntime& r, int16_t nid) {
    if (const Neighbor* n = r.vmm().neighbor(nid))
        return n->guid;
    return 0;
}
} // namespace

int LuaRuntime::neighborNew(lua_State* L) {
    if (LuaRuntime* r = self(L))
        r->pushInstance(L, "CachedNeighbor", int16_t(luaL_checknumber(L, 1)));
    return 1;
}

int LuaRuntime::neighborGetPersonData(lua_State* L) {
    LuaRuntime* r = self(L);
    int16_t v = 0;
    if (r) {
        const int16_t nid = instanceId(L);
        const int field = argInt(L, 2);
        if (int16_t* p = r->personData(nid, field))
            v = *p;
        else
            logOnce("lua api", "neighbor getPersonData " + std::to_string(nid) + "/" + std::to_string(field),
                    "Lua: CachedNeighbor(" + std::to_string(nid) + "):getPersonData(" + std::to_string(field) + ")");
    }
    lua_pushnumber(L, v);
    return 1;
}

int LuaRuntime::neighborSetPersonData(lua_State* L) {
    LuaRuntime* r = self(L);
    if (r) {
        const int16_t nid = instanceId(L);
        if (int16_t* p = r->personData(nid, argInt(L, 2)))
            *p = int16_t(argInt(L, 3));
    }
    return 0;
}

int LuaRuntime::neighborGetGuid(lua_State* L) {
    LuaRuntime* r = self(L);
    pushInt32(L, r ? neighborGuid(*r, instanceId(L)) : 0);
    return 1;
}

int LuaRuntime::neighborGetName(lua_State* L) {
    LuaRuntime* r = self(L);
    const Neighbor* n = r ? r->vm_.neighbor(instanceId(L)) : nullptr;
    lua_pushstring(L, n ? n->firstName.c_str() : "");
    return 1;
}

int LuaRuntime::neighborGetSurname(lua_State* L) {
    LuaRuntime* r = self(L);
    const Neighbor* n = r ? r->vm_.neighbor(instanceId(L)) : nullptr;
    lua_pushstring(L, n ? n->lastName.c_str() : "");
    return 1;
}

int LuaRuntime::neighborGetFullName(lua_State* L) {
    LuaRuntime* r = self(L);
    const Neighbor* n = r ? r->vm_.neighbor(instanceId(L)) : nullptr;
    const std::string name = n ? (n->firstName + (n->lastName.empty() ? "" : " " + n->lastName)) : "";
    lua_pushstring(L, name.c_str());
    return 1;
}

int LuaRuntime::neighborGetOutfitProperty(lua_State* L) {
    // LAB_009022e0: the neighbor's outfit property (hair/skin/eye color, ...)
    // of the item in arg 1. The port keeps the outfit keys (Neighbor::outfits)
    // but not the properties of each part yet.
    (void)L;
    logOnce("lua api", "getOutfitProperty", "Lua: CachedNeighbor:getOutfitProperty is not measured yet");
    lua_pushnumber(L, 0);
    return 1;
}

int LuaRuntime::personNew(lua_State* L) {
    if (LuaRuntime* r = self(L))
        r->pushInstance(L, "CachedPerson", int16_t(luaL_checknumber(L, 1)));
    return 1;
}

int LuaRuntime::personGetPersonData(lua_State* L) { return neighborGetPersonData(L); }
int LuaRuntime::personSetPersonData(lua_State* L) { return neighborSetPersonData(L); }
int LuaRuntime::personGetGuid(lua_State* L) { return neighborGetGuid(L); }

int LuaRuntime::personGetMotive(lua_State* L) {
    // A motive belongs to the Sim on the lot (its Person). Off the lot the
    // neighbor has no motives: 0.
    LuaRuntime* r = self(L);
    int16_t v = 0;
    if (r) {
        const int16_t nid = instanceId(L);
        const int i = argInt(L, 2);
        if (Object* sim = r->vm_.neighborSim(nid); sim && sim->person) {
            if (i >= 0 && size_t(i) < kMotiveCount)
                v = sim->person->motives[size_t(i)];
        } else {
            logOnce("lua api", "motive " + std::to_string(nid), "Lua: CachedPerson(" + std::to_string(nid) + ") is not on the lot");
        }
    }
    lua_pushnumber(L, v);
    return 1;
}

int LuaRuntime::managerNew(lua_State* L) {
    // The manager has no state of its own: its methods read the VM.
    if (LuaRuntime* r = self(L))
        r->pushInstance(L, "ObjectManager", 0);
    return 1;
}

int LuaRuntime::managerGetPersonIdFromGUID(lua_State* L) {
    // FUN_00904590: the neighbor ID of the live person object with this GUID,
    // 0 if no person of the lot has it.
    LuaRuntime* r = self(L);
    int16_t nid = 0;
    if (r) {
        for (const ObjectId id : r->vm_.objectsOfGuid(uint32_t(argInt(L, 2)))) {
            Object* o = r->vm_.object(id);
            if (o && o->person) {
                nid = o->person->data[persondata::NeighborId];
                break;
            }
        }
    }
    lua_pushnumber(L, nid);
    return 1;
}

int LuaRuntime::managerGetObjectIdFromGUID(lua_State* L) {
    // FUN_00904490: the first live object of that GUID.
    LuaRuntime* r = self(L);
    int16_t id = 0;
    if (r)
        for (const ObjectId candidate : r->vm_.objectsOfGuid(uint32_t(argInt(L, 2))))
            if (r->vm_.object(candidate)) {
                id = candidate;
                break;
            }
    lua_pushnumber(L, id);
    return 1;
}

int LuaRuntime::managerGetObjectGUIDFromObjectId(lua_State* L) {
    LuaRuntime* r = self(L);
    Object* o = r ? r->object(argInt(L, 2)) : nullptr;
    pushInt32(L, o ? o->guid() : 0);
    return 1;
}

namespace {
void pushIdTable(lua_State* L, const std::vector<int16_t>& ids) {
    lua_newtable(L);
    int i = 1;
    for (int16_t id : ids) {
        lua_pushnumber(L, id);
        lua_rawseti(L, -2, i++);
    }
}
} // namespace

int LuaRuntime::managerGetObjectIds(lua_State* L) {
    LuaRuntime* r = self(L);
    std::vector<int16_t> ids;
    if (r)
        for (const auto& [id, o] : r->vm_.objects()) {
            (void)o;
            ids.push_back(id);
        }
    pushIdTable(L, ids);
    return 1;
}

int LuaRuntime::managerGetNeighborIds(lua_State* L) {
    LuaRuntime* r = self(L);
    std::vector<int16_t> ids;
    if (r)
        for (const auto& [id, n] : r->vm_.neighbors) {
            (void)n;
            ids.push_back(id);
        }
    pushIdTable(L, ids);
    return 1;
}

int LuaRuntime::managerGetPersonIds(lua_State* L) {
    LuaRuntime* r = self(L);
    std::vector<int16_t> ids;
    if (r)
        for (const auto& [id, o] : r->vm_.objects())
            if (o.person)
                ids.push_back(o.person->data[persondata::NeighborId]);
    pushIdTable(L, ids);
    return 1;
}

int LuaRuntime::managerVerifyObjectId(lua_State* L) {
    LuaRuntime* r = self(L);
    const int id = argInt(L, 2);
    lua_pushnumber(L, r && r->vm_.object(ObjectId(id)) ? id : 0);
    return 1;
}

int LuaRuntime::managerVerifyNeighborId(lua_State* L) {
    LuaRuntime* r = self(L);
    const int id = argInt(L, 2);
    lua_pushnumber(L, r && r->vm_.neighbor(int16_t(id)) ? id : 0);
    return 1;
}

int LuaRuntime::neighborhoodNew(lua_State* L) {
    if (LuaRuntime* r = self(L))
        r->pushInstance(L, "NeighborhoodManager", 0);
    return 1;
}

int LuaRuntime::neighborhoodGetNumOfType(lua_State* L) {
    // The port has no downtown or university sub-neighborhoods yet
    // (docs/game-flow-mac.md): 0 is the count the game reports for a hood
    // without them, and GetValidDowntown answers false.
    (void)L;
    lua_pushnumber(L, 0);
    return 1;
}

int LuaRuntime::neighborhoodGetIds(lua_State* L) {
    (void)L;
    lua_newtable(L);
    return 1;
}

int LuaRuntime::neighborhoodGetType(lua_State* L) {
    (void)L;
    lua_pushstring(L, "");
    return 1;
}

int LuaRuntime::neighborhoodClassNew(lua_State* L) {
    if (LuaRuntime* r = self(L))
        r->pushInstance(L, "Neighborhood", 0);
    return 1;
}

int LuaRuntime::neighborhoodIsValidLot(lua_State* L) {
    LuaRuntime* r = self(L);
    const int16_t id = int16_t(argInt(L, 2));
    lua_pushnumber(L, r && r->vm_.lots.contains(id) ? 1 : 0);
    return 1;
}

int LuaRuntime::neighborhoodGetLotType(lua_State* L) {
    LuaRuntime* r = self(L);
    const int16_t id = int16_t(argInt(L, 2));
    int zoning = 0;
    if (r) {
        const auto it = r->vm_.lots.find(id);
        if (it != r->vm_.lots.end())
            zoning = it->second.zoning;
    }
    lua_pushnumber(L, zoning);
    return 1;
}

int LuaRuntime::neighborhoodGetLotName(lua_State* L) {
    LuaRuntime* r = self(L);
    const int16_t id = int16_t(argInt(L, 2));
    const std::string* name = nullptr;
    if (r) {
        const auto it = r->vm_.lots.find(id);
        if (it != r->vm_.lots.end())
            name = &it->second.name;
    }
    lua_pushstring(L, name ? name->c_str() : "");
    return 1;
}

int LuaRuntime::neighborhoodGetLotIds(lua_State* L) {
    LuaRuntime* r = self(L);
    lua_newtable(L);
    if (r) {
        int i = 1;
        for (const auto& [id, lot] : r->vm_.lots) {
            (void)lot;
            lua_pushnumber(L, id);
            lua_rawseti(L, -2, i++);
        }
    }
    return 1;
}

// --- nWorld and nGameState -------------------------------------------------

int LuaRuntime::worldGetObjectTile(lua_State* L) {
    LuaRuntime* r = self(L);
    Object* o = r ? r->object(argInt(L, 1)) : nullptr;
    if (!o) {
        lua_pushnil(L);
        return 1;
    }
    const TilePos t = VM::tileOf(*o);
    lua_pushnumber(L, t.x);
    lua_pushnumber(L, t.y);
    lua_pushnumber(L, t.level);
    lua_pushnumber(L, 0);
    return 4;
}

int LuaRuntime::worldGetObjectLocation(lua_State* L) {
    LuaRuntime* r = self(L);
    Object* o = r ? r->object(argInt(L, 1)) : nullptr;
    if (!o) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, o->position.x);
    lua_pushnumber(L, o->position.y);
    lua_pushnumber(L, o->position.level);
    return 3;
}

int LuaRuntime::worldGetObjectsOnTile(lua_State* L) {
    LuaRuntime* r = self(L);
    std::vector<int16_t> ids;
    if (r) {
        TilePos t;
        t.x = argInt(L, 1);
        t.y = argInt(L, 2);
        t.level = lua_gettop(L) >= 3 ? argInt(L, 3) : 0;
        ids = r->vm_.objectsOnTile(t);
    }
    pushIdTable(L, ids);
    return 1;
}

int LuaRuntime::worldGetTerrainElevation(lua_State* L) {
    // The terrain's height at the object's tile: the port's terrain is flat
    // in the queries the scripts make (the driveway and garage rules are not
    // ported yet, docs/build-buy-mac.md).
    (void)L;
    lua_pushnumber(L, 0);
    return 1;
}

int LuaRuntime::worldMoveObjectToLocation(lua_State* L) {
    // The engine's MoveObjectToLocation (the garage and driveway scripts):
    // its placement rules (the driveway tiles, the car's heading) are not
    // measured yet. Answer false instead of moving something the game would
    // not.
    (void)L;
    logOnce("lua api", "MoveObjectToLocation", "Lua: MoveObjectToLocation is not measured yet");
    lua_pushboolean(L, 0);
    return 1;
}

int LuaRuntime::gameStateGetSaveLotEnabled(lua_State* L) {
    (void)L;
    lua_pushnumber(L, 1);
    return 1;
}

int LuaRuntime::gameStateGetTransitionsEnabled(lua_State* L) {
    (void)L;
    lua_pushnumber(L, 1);
    return 1;
}

int LuaRuntime::gameStateGetLotTripInfo(lua_State* L) {
    // Not on a trip: the four results the script reads are 0.
    (void)L;
    for (int i = 0; i < 4; ++i)
        lua_pushnumber(L, 0);
    return 4;
}

int LuaRuntime::gameStateIsEpInstalled(lua_State* L) {
    // FUN_008fc670: bit `n` of the installed-EP mask (FUN_00cf5a70). The port
    // reads the data of the Ultimate Collection: the base (bit 0) and the
    // eight expansion packs (bits 1-8), plus Mansion & Garden (bit 9).
    constexpr uint32_t kInstalledEps = 0x3FF;
    const int n = argInt(L, 1);
    lua_pushnumber(L, (n >= 0 && n < 32 && (kInstalledEps & (1u << n))) ? 1 : 0);
    return 1;
}

int LuaRuntime::gameStateGetInstalledEps(lua_State* L) {
    constexpr uint32_t kInstalledEps = 0x3FF;
    (void)L;
    lua_pushnumber(L, double(kInstalledEps));
    return 1;
}

int LuaRuntime::gameStateGetCurrentState(lua_State* L) {
    // FUN_00799b65()->vfunc(0x14): the app's state. The scripts that read it
    // (the weather/season ones) are not run on a lot; 0 is the game's "in the
    // lot" state as far as the measured scripts compare.
    (void)L;
    lua_pushnumber(L, 0);
    return 1;
}

int LuaRuntime::gameStateGetProperty(lua_State* L) {
    // FUN_0090cc90: the game state's property bag. Not measured; the scripts
    // of the ObjectScripts that reach it are the weather ones.
    (void)L;
    logOnce("lua api", "nGameState.GetProperty", "Lua: nGameState.GetProperty is not measured yet");
    lua_pushnumber(L, 0);
    return 1;
}

int LuaRuntime::dataTableGetObjectValue(lua_State* L) {
    // No generic data table is stored by the port: the row is absent (nil),
    // which is what the scripts test for (ShoppingGetPrice).
    (void)L;
    logOnce("lua api", "nDataTable.GetObjectTableValue", "Lua: nDataTable has no stored tables yet");
    lua_pushnil(L);
    return 1;
}

int LuaRuntime::dataTableGetNeighborValue(lua_State* L) {
    (void)L;
    logOnce("lua api", "nDataTable.GetNeighborTableValue", "Lua: nDataTable has no stored tables yet");
    lua_pushnil(L);
    return 1;
}

// --- the fallbacks ---------------------------------------------------------

int LuaRuntime::unknownMethod(lua_State* L) {
    // __index of a class's methods: any name gets a logging stub.
    const char* name = luaL_checkstring(L, 2);
    logOnce("lua api", std::string("method ") + name, std::string("Lua: the API has no \"") + name + "\" yet");
    lua_pushcfunction(L, &LuaRuntime::stubFunction);
    return 1;
}

int LuaRuntime::stubFunction(lua_State* L) {
    lua_pushnumber(L, 0);
    return 1;
}

int LuaRuntime::luaPrint(lua_State* L) {
    if (!traceLua())
        return 0;
    std::string out;
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        if (i > 1)
            out += '\t';
        const char* s = lua_tostring(L, i);
        out += s ? s : "?";
    }
    std::fprintf(stderr, "lua print: %s\n", out.c_str());
    return 0;
}

// --- the VM's side ---------------------------------------------------------

LuaRuntime& VM::lua() {
    if (!lua_)
        lua_ = new LuaRuntime(*this);
    return *lua_;
}

VM::~VM() { delete lua_; }

bool runLuaScript(Context& c, const std::string& name) {
    return c.vm.lua().run(c, name);
}

} // namespace os2::vm
