/*
 * Lua binding of the web browser
 *
 * browser.available() - this box has the browser
 * browser.open{url=, name=, script=, resolver=, data=} - shows the page until it
 * ends, see CHbbtvPage; false if it cannot start
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <config.h>
#include <string>

#include <global.h>
#include <gui/hbbtv.h>

#include "luainstance.h"
#include "lua_browser.h"

CLuaInstBrowser *CLuaInstBrowser::getInstance()
{
	static CLuaInstBrowser instance;
	return &instance;
}

void CLuaInstBrowser::BrowserRegister(lua_State *L)
{
	luaL_Reg meth[] = {
		{ "available", CLuaInstBrowser::BrowserAvailable },
		{ "open", CLuaInstBrowser::BrowserOpen },
		{ NULL, NULL }
	};
	luaL_newmetatable(L, "browser");
	luaL_setfuncs(L, meth, 0);
	lua_pushvalue(L, -1);
	lua_setfield(L, -1, "__index");
	lua_setglobal(L, "browser");
}

int CLuaInstBrowser::BrowserAvailable(lua_State *L)
{
	lua_pushboolean(L, CHbbtv::available());
	return 1;
}

int CLuaInstBrowser::BrowserOpen(lua_State *L)
{
	lua_assert(lua_istable(L, 1));
	CHbbtvPage page;
	tableLookup(L, "url", page.url);
	tableLookup(L, "name", page.name);
	tableLookup(L, "script", page.script);
	tableLookup(L, "resolver", page.resolver);
	tableLookup(L, "data", page.data);
	lua_pushboolean(L, !page.url.empty() && CHbbtv::startPage(page));
	return 1;
}
