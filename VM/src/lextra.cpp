// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details

// Fork-owned: mlua's Luau bindings link `lua_getmetatablepointer` and
// `lua_gcdump`, which upstream Luau ships in `Custom/src/lextra.cpp` and which
// LÖVR's build does not include. Ported here so the fork's VM provides the
// whole mlua-visible surface without pulling in the Custom component.
#include "lua.h"
#include "lapi.h"
#include "lgc.h"
#include "lobject.h"
#include "lstate.h"

extern "C" const void* lua_getmetatablepointer(lua_State* L, int objindex)
{
    const TValue* obj = luaA_toobject(L, objindex);
    if (!obj)
        return NULL;

    switch (ttype(obj))
    {
    case LUA_TTABLE:
        return hvalue(obj)->metatable;
    case LUA_TUSERDATA:
        return uvalue(obj)->metatable;
    default:
        return NULL;
    }
}

extern "C" void lua_gcdump(lua_State* L, void* file, const char* (*categoryName)(lua_State* L, uint8_t memcat))
{
    luaC_dump(L, file, categoryName);
}