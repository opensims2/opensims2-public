#pragma once

#include <string>

// s2pkg lua: the Lua 5.0 scripts of the game (ObjectScripts/*.objLua): the
// chunk's functions, constants and disassembly (docs/lua-scripts.md).
int scriptCommand(const std::string& cmd, int argc, char** argv);
