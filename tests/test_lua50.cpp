// Lua 5.0.3 in the port's build (third_party/lua50): the game's ObjectScripts
// are Lua 5.0 bytecode (docs/lua-scripts.md), so the 0x7E (Run Script) needs
// the VM to load a precompiled chunk and run it. This checks the library on the
// port's compiler: a source chunk, its dump, and the dump loaded back (the path
// the game's scripts take).
// Lua 5.0's headers have no __cplusplus guard (5.1 added one): the C library's
// names are demanded from C++ through this block.
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static int writer(lua_State*, const void* p, size_t size, void* data) {
    auto* out = static_cast<std::string*>(data);
    out->append(static_cast<const char*>(p), size);
    return 0;
}

int main() {
    lua_State* L = lua_open();
    luaopen_base(L);
    luaopen_table(L);
    luaopen_string(L);
    luaopen_math(L);

    // A source chunk runs (the parser is in the library as well).
    const char* src = "return 21 * 2";
    check(luaL_loadbuffer(L, src, std::strlen(src), "test") == 0, "a source chunk loads");
    check(lua_pcall(L, 0, 1, 0) == 0, "a source chunk runs");
    check(lua_tonumber(L, -1) == 42.0, "a source chunk's result");
    lua_pop(L, 1);

    // Its dump, loaded back: the game's path (a precompiled chunk).
    check(luaL_loadbuffer(L, src, std::strlen(src), "test") == 0, "the chunk loads again");
    std::string bytecode;
    // lua_dump returns 1 on success in 5.0 (lapi.c sets status = 1).
    const int dumped = lua_dump(L, writer, &bytecode);
    check(dumped == 1 && bytecode.size() > 20, "the chunk dumps");
    check(bytecode.compare(0, 4, "\033Lua") == 0, "the dump is a Lua chunk");
    lua_pop(L, 1);
    check(luaL_loadbuffer(L, bytecode.data(), bytecode.size(), "dump") == 0, "the dump loads");
    check(lua_pcall(L, 0, 1, 0) == 0, "the dump runs");
    check(lua_tonumber(L, -1) == 42.0, "the dump's result");
    lua_pop(L, 1);

    // Tables and the standard library the scripts use (ipairs/pairs/table/math).
    // (Lua 5.0 has no `#` operator: the length is table.getn.)
    const char* lib = "local t = {3, 1, 2}; local n = 0; for i, v in ipairs(t) do n = n + v end;"
                      " table.sort(t); return n, t[1], math.floor(3.7), table.getn(t)";
    const int libLoaded = luaL_loadbuffer(L, lib, std::strlen(lib), "lib");
    check(libLoaded == 0, "the library chunk loads");
    check(lua_pcall(L, 0, 4, 0) == 0, "the library chunk runs");
    check(lua_tonumber(L, -4) == 6.0 && lua_tonumber(L, -3) == 1.0 && lua_tonumber(L, -2) == 3.0 &&
              lua_tonumber(L, -1) == 3.0,
          "ipairs, table.sort, math.floor and #");
    lua_pop(L, 4);

    lua_close(L);
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("lua 5.0: all tests passed\n");
    return failures ? 1 : 0;
}
