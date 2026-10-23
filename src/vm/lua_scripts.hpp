#pragma once
// The 0x7E (Run Script)'s Lua 5.0 side: the game keeps its object scripts in
// TSData/Res/ObjectScripts as Lua 5.0 chunks, and the 0x7E runs one of them
// (docs/lua-scripts.md). The scripts call the engine through the API registered
// here, reading the running thread's temps/params and the objects' data.
#include <string>

namespace os2::vm {

class VM;
struct Context;

// Runs the ObjectScripts' script `name` with `c` as the context (the 0x7E's
// instruction) and returns the value SetScriptReturnValue left: false when the
// script is missing, fails to load or does not set it. The first call builds
// the Lua state and loads the game's preludes (*.globalObjLua).
bool runLuaScript(Context& c, const std::string& name);

} // namespace os2::vm
