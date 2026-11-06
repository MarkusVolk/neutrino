/*
 * Lua binding of the web browser
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _LUABROWSER_H
#define _LUABROWSER_H

class CLuaInstBrowser
{
	public:
		static CLuaInstBrowser *getInstance();
		static void BrowserRegister(lua_State *L);

	private:
		static int BrowserAvailable(lua_State *L);
		static int BrowserOpen(lua_State *L);
};

#endif
