/*
 * A tray icon on the desktop
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <config.h>
#include <cstdio>
#include <cstdlib>
#include <string.h>

#include <dbus/dbus.h>

#include <global.h>
#include <neutrino.h>
#include <driver/rcinput.h>
#include <neutrinoMessages.h>

#include "trayicon.h"

#define ITEM_PATH	"/StatusNotifierItem"
#define MENU_PATH	"/StatusNotifierItem/Menu"
#define ITEM_IFACE	"org.kde.StatusNotifierItem"
#define MENU_IFACE	"com.canonical.dbusmenu"
#define WATCHER_NAME	"org.kde.StatusNotifierWatcher"
#define WATCHER_PATH	"/StatusNotifierWatcher"
#define DBUS_PROPS	"org.freedesktop.DBus.Properties"
#define DBUS_INTROSPECT	"org.freedesktop.DBus.Introspectable"

/* the menu: a toggle between standby and wakeup, a line, the exit */
#define MENU_ID_POWER	1
#define MENU_ID_LINE	2
#define MENU_ID_EXIT	3
static const int menu_ids[] = { MENU_ID_POWER, MENU_ID_LINE, MENU_ID_EXIT };

static const char *item_introspection =
	"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
	" \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
	"<node>\n"
	" <interface name=\"" DBUS_INTROSPECT "\">\n"
	"  <method name=\"Introspect\"><arg name=\"data\" type=\"s\" direction=\"out\"/></method>\n"
	" </interface>\n"
	" <interface name=\"" DBUS_PROPS "\">\n"
	"  <method name=\"Get\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/></method>\n"
	"  <method name=\"GetAll\"><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"out\"/></method>\n"
	" </interface>\n"
	" <interface name=\"" ITEM_IFACE "\">\n"
	"  <method name=\"Activate\"><arg type=\"i\" direction=\"in\"/><arg type=\"i\" direction=\"in\"/></method>\n"
	"  <method name=\"SecondaryActivate\"><arg type=\"i\" direction=\"in\"/><arg type=\"i\" direction=\"in\"/></method>\n"
	"  <method name=\"ContextMenu\"><arg type=\"i\" direction=\"in\"/><arg type=\"i\" direction=\"in\"/></method>\n"
	"  <method name=\"Scroll\"><arg type=\"i\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/></method>\n"
	"  <property name=\"Category\" type=\"s\" access=\"read\"/>\n"
	"  <property name=\"Id\" type=\"s\" access=\"read\"/>\n"
	"  <property name=\"Title\" type=\"s\" access=\"read\"/>\n"
	"  <property name=\"Status\" type=\"s\" access=\"read\"/>\n"
	"  <property name=\"WindowId\" type=\"i\" access=\"read\"/>\n"
	"  <property name=\"IconName\" type=\"s\" access=\"read\"/>\n"
	"  <property name=\"IconThemePath\" type=\"s\" access=\"read\"/>\n"
	"  <property name=\"ToolTip\" type=\"(sa(iiay)ss)\" access=\"read\"/>\n"
	"  <property name=\"ItemIsMenu\" type=\"b\" access=\"read\"/>\n"
	"  <property name=\"Menu\" type=\"o\" access=\"read\"/>\n"
	"  <signal name=\"NewIcon\"/>\n"
	"  <signal name=\"NewToolTip\"/>\n"
	"  <signal name=\"NewStatus\"><arg type=\"s\"/></signal>\n"
	" </interface>\n"
	"</node>\n";

static const char *menu_introspection =
	"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
	" \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
	"<node>\n"
	" <interface name=\"" DBUS_INTROSPECT "\">\n"
	"  <method name=\"Introspect\"><arg name=\"data\" type=\"s\" direction=\"out\"/></method>\n"
	" </interface>\n"
	" <interface name=\"" DBUS_PROPS "\">\n"
	"  <method name=\"Get\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/></method>\n"
	"  <method name=\"GetAll\"><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"out\"/></method>\n"
	" </interface>\n"
	" <interface name=\"" MENU_IFACE "\">\n"
	"  <method name=\"GetLayout\"><arg type=\"i\" direction=\"in\"/><arg type=\"i\" direction=\"in\"/><arg type=\"as\" direction=\"in\"/><arg type=\"u\" direction=\"out\"/><arg type=\"(ia{sv}av)\" direction=\"out\"/></method>\n"
	"  <method name=\"GetGroupProperties\"><arg type=\"ai\" direction=\"in\"/><arg type=\"as\" direction=\"in\"/><arg type=\"a(ia{sv})\" direction=\"out\"/></method>\n"
	"  <method name=\"GetProperty\"><arg type=\"i\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/></method>\n"
	"  <method name=\"Event\"><arg type=\"i\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/></method>\n"
	"  <method name=\"EventGroup\"><arg type=\"a(isvu)\" direction=\"in\"/><arg type=\"ai\" direction=\"out\"/></method>\n"
	"  <method name=\"AboutToShow\"><arg type=\"i\" direction=\"in\"/><arg type=\"b\" direction=\"out\"/></method>\n"
	"  <method name=\"AboutToShowGroup\"><arg type=\"ai\" direction=\"in\"/><arg type=\"ai\" direction=\"out\"/><arg type=\"ai\" direction=\"out\"/></method>\n"
	"  <property name=\"Version\" type=\"u\" access=\"read\"/>\n"
	"  <property name=\"TextDirection\" type=\"s\" access=\"read\"/>\n"
	"  <property name=\"Status\" type=\"s\" access=\"read\"/>\n"
	"  <property name=\"IconThemePath\" type=\"as\" access=\"read\"/>\n"
	"  <signal name=\"LayoutUpdated\"><arg type=\"u\"/><arg type=\"i\"/></signal>\n"
	"  <signal name=\"ItemsPropertiesUpdated\"><arg type=\"a(ia{sv})\"/><arg type=\"a(ias)\"/></signal>\n"
	" </interface>\n"
	"</node>\n";

static const char *item_props[] = {
	"Category", "Id", "Title", "Status", "WindowId", "IconName", "IconThemePath", "ToolTip", "ItemIsMenu", "Menu", NULL
};
static const char *menu_props[] = { "Version", "TextDirection", "Status", "IconThemePath", NULL };

static void appendVariant(DBusMessageIter *it, const char *type, const void *value)
{
	DBusMessageIter v;
	dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, type, &v);
	dbus_message_iter_append_basic(&v, type[0], value);
	dbus_message_iter_close_container(it, &v);
}

static void appendDictEntry(DBusMessageIter *a, const char *name, const char *type, const void *value)
{
	DBusMessageIter e;
	dbus_message_iter_open_container(a, DBUS_TYPE_DICT_ENTRY, NULL, &e);
	dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &name);
	appendVariant(&e, type, value);
	dbus_message_iter_close_container(a, &e);
}

/* a property of the item; false when there is no such property */
void CTrayIcon::appendItemProperty(void *iter, const char *name)
{
	DBusMessageIter *it = (DBusMessageIter *)iter;
	const char *s;
	dbus_bool_t no = FALSE;
	dbus_int32_t zero = 0;

	if (!strcmp(name, "Category"))
		s = "ApplicationStatus";
	else if (!strcmp(name, "Id"))
		s = "neutrino";
	else if (!strcmp(name, "Title"))
		s = "Neutrino";
	else if (!strcmp(name, "Status"))
		s = "Active";
	else if (!strcmp(name, "IconName"))
		s = "neutrino-symbolic";
	else if (!strcmp(name, "IconThemePath"))
		s = "";
	else if (!strcmp(name, "Menu"))
	{
		s = MENU_PATH;
		appendVariant(it, DBUS_TYPE_OBJECT_PATH_AS_STRING, &s);
		return;
	}
	else if (!strcmp(name, "WindowId"))
	{
		appendVariant(it, DBUS_TYPE_INT32_AS_STRING, &zero);
		return;
	}
	else if (!strcmp(name, "ItemIsMenu"))
	{
		appendVariant(it, DBUS_TYPE_BOOLEAN_AS_STRING, &no);
		return;
	}
	else if (!strcmp(name, "ToolTip"))
	{
		/* (icon name, icon pixmaps, title, description) */
		DBusMessageIter v, st, a;
		const char *icon = "neutrino-symbolic", *title = "Neutrino";
		const char *desc = standby() ? "Standby" : "Running";
		dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "(sa(iiay)ss)", &v);
		dbus_message_iter_open_container(&v, DBUS_TYPE_STRUCT, NULL, &st);
		dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &icon);
		dbus_message_iter_open_container(&st, DBUS_TYPE_ARRAY, "(iiay)", &a);
		dbus_message_iter_close_container(&st, &a);
		dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &title);
		dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &desc);
		dbus_message_iter_close_container(&v, &st);
		dbus_message_iter_close_container(it, &v);
		return;
	}
	else
		return;
	appendVariant(it, DBUS_TYPE_STRING_AS_STRING, &s);
}

static void appendMenuProperty(DBusMessageIter *it, const char *name)
{
	dbus_uint32_t version = 3;
	const char *s;
	if (!strcmp(name, "Version"))
		appendVariant(it, DBUS_TYPE_UINT32_AS_STRING, &version);
	else if (!strcmp(name, "TextDirection") || !strcmp(name, "Status"))
	{
		s = !strcmp(name, "Status") ? "normal" : "ltr";
		appendVariant(it, DBUS_TYPE_STRING_AS_STRING, &s);
	}
	else if (!strcmp(name, "IconThemePath"))
	{
		DBusMessageIter v, a;
		dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "as", &v);
		dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "s", &a);
		dbus_message_iter_close_container(&v, &a);
		dbus_message_iter_close_container(it, &v);
	}
}

/* the a{sv} of one menu entry */
void CTrayIcon::appendMenuItem(void *iter, int id)
{
	DBusMessageIter a;
	dbus_message_iter_open_container((DBusMessageIter *)iter, DBUS_TYPE_ARRAY, "{sv}", &a);
	if (id == MENU_ID_LINE)
	{
		const char *type = "separator";
		appendDictEntry(&a, "type", DBUS_TYPE_STRING_AS_STRING, &type);
	}
	else
	{
		const char *label = id == MENU_ID_EXIT ? "Quit" : standby() ? "Wake up" : "Standby";
		dbus_bool_t yes = TRUE;
		appendDictEntry(&a, "label", DBUS_TYPE_STRING_AS_STRING, &label);
		appendDictEntry(&a, "enabled", DBUS_TYPE_BOOLEAN_AS_STRING, &yes);
		appendDictEntry(&a, "visible", DBUS_TYPE_BOOLEAN_AS_STRING, &yes);
	}
	dbus_message_iter_close_container((DBusMessageIter *)iter, &a);
}

/* (ia{sv}av): the entry and its children; only the root has children */
static void appendLayout(DBusMessageIter *it, int id)
{
	DBusMessageIter st, children;
	dbus_int32_t i = id;
	dbus_message_iter_open_container(it, DBUS_TYPE_STRUCT, NULL, &st);
	dbus_message_iter_append_basic(&st, DBUS_TYPE_INT32, &i);
	if (id == 0)
	{
		DBusMessageIter a;
		const char *type = "submenu";
		dbus_message_iter_open_container(&st, DBUS_TYPE_ARRAY, "{sv}", &a);
		appendDictEntry(&a, "children-display", DBUS_TYPE_STRING_AS_STRING, &type);
		dbus_message_iter_close_container(&st, &a);
	}
	else
		CTrayIcon::getInstance()->appendMenuItem(&st, id);
	dbus_message_iter_open_container(&st, DBUS_TYPE_ARRAY, "v", &children);
	if (id == 0)
	{
		for (unsigned k = 0; k < sizeof(menu_ids) / sizeof(menu_ids[0]); k++)
		{
			DBusMessageIter v;
			dbus_message_iter_open_container(&children, DBUS_TYPE_VARIANT, "(ia{sv}av)", &v);
			appendLayout(&v, menu_ids[k]);
			dbus_message_iter_close_container(&children, &v);
		}
	}
	dbus_message_iter_close_container(&st, &children);
	dbus_message_iter_close_container(it, &st);
}

static DBusMessage *propertiesCall(DBusMessage *msg, const char *member, const char **names, bool item)
{
	DBusMessageIter it;
	DBusMessage *reply = NULL;
	if (!strcmp(member, "Get"))
	{
		const char *pi = NULL, *name = NULL;
		if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &pi, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID))
			return dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS, member);
		bool known = false;
		for (const char **n = names; *n; n++)
			known |= !strcmp(*n, name);
		if (!known)
			return dbus_message_new_error(msg, DBUS_ERROR_UNKNOWN_PROPERTY, name);
		reply = dbus_message_new_method_return(msg);
		dbus_message_iter_init_append(reply, &it);
		if (item)
			CTrayIcon::getInstance()->appendItemProperty(&it, name);
		else
			appendMenuProperty(&it, name);
	}
	else if (!strcmp(member, "GetAll"))
	{
		DBusMessageIter a, e;
		reply = dbus_message_new_method_return(msg);
		dbus_message_iter_init_append(reply, &it);
		dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &a);
		for (const char **n = names; *n; n++)
		{
			dbus_message_iter_open_container(&a, DBUS_TYPE_DICT_ENTRY, NULL, &e);
			dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, n);
			if (item)
				CTrayIcon::getInstance()->appendItemProperty(&e, *n);
			else
				appendMenuProperty(&e, *n);
			dbus_message_iter_close_container(&a, &e);
		}
		dbus_message_iter_close_container(&it, &a);
	}
	else
		reply = dbus_message_new_error(msg, DBUS_ERROR_PROPERTY_READ_ONLY, member);
	return reply;
}

static DBusHandlerResult handleItem(DBusConnection *c, DBusMessage *msg, void *)
{
	if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	const char *iface = dbus_message_get_interface(msg);
	const char *member = dbus_message_get_member(msg);
	if (!iface || !member)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	DBusMessage *reply = NULL;
	if (!strcmp(iface, DBUS_INTROSPECT) && !strcmp(member, "Introspect"))
	{
		reply = dbus_message_new_method_return(msg);
		dbus_message_append_args(reply, DBUS_TYPE_STRING, &item_introspection, DBUS_TYPE_INVALID);
	}
	else if (!strcmp(iface, DBUS_PROPS))
		reply = propertiesCall(msg, member, item_props, true);
	else if (!strcmp(iface, ITEM_IFACE))
	{
		if (!strcmp(member, "Activate") || !strcmp(member, "SecondaryActivate"))
			CTrayIcon::getInstance()->activate();
		reply = dbus_message_new_method_return(msg);
	}
	else
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	dbus_connection_send(c, reply, NULL);
	dbus_message_unref(reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult handleMenu(DBusConnection *c, DBusMessage *msg, void *)
{
	if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	const char *iface = dbus_message_get_interface(msg);
	const char *member = dbus_message_get_member(msg);
	if (!iface || !member)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	DBusMessage *reply = NULL;
	DBusMessageIter it;
	if (!strcmp(iface, DBUS_INTROSPECT) && !strcmp(member, "Introspect"))
	{
		reply = dbus_message_new_method_return(msg);
		dbus_message_append_args(reply, DBUS_TYPE_STRING, &menu_introspection, DBUS_TYPE_INVALID);
	}
	else if (!strcmp(iface, DBUS_PROPS))
		reply = propertiesCall(msg, member, menu_props, false);
	else if (!strcmp(iface, MENU_IFACE) && !strcmp(member, "GetLayout"))
	{
		dbus_int32_t parent = 0;
		DBusMessageIter in;
		if (dbus_message_iter_init(msg, &in) && dbus_message_iter_get_arg_type(&in) == DBUS_TYPE_INT32)
			dbus_message_iter_get_basic(&in, &parent);
		dbus_uint32_t revision = CTrayIcon::getInstance()->layoutRevision();
		reply = dbus_message_new_method_return(msg);
		dbus_message_iter_init_append(reply, &it);
		dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &revision);
		appendLayout(&it, parent);
	}
	else if (!strcmp(iface, MENU_IFACE) && !strcmp(member, "GetGroupProperties"))
	{
		DBusMessageIter in, ids, a, st;
		reply = dbus_message_new_method_return(msg);
		dbus_message_iter_init_append(reply, &it);
		dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "(ia{sv})", &a);
		if (dbus_message_iter_init(msg, &in) && dbus_message_iter_get_arg_type(&in) == DBUS_TYPE_ARRAY)
		{
			dbus_message_iter_recurse(&in, &ids);
			while (dbus_message_iter_get_arg_type(&ids) == DBUS_TYPE_INT32)
			{
				dbus_int32_t id;
				dbus_message_iter_get_basic(&ids, &id);
				if (id > 0)
				{
					dbus_message_iter_open_container(&a, DBUS_TYPE_STRUCT, NULL, &st);
					dbus_message_iter_append_basic(&st, DBUS_TYPE_INT32, &id);
					CTrayIcon::getInstance()->appendMenuItem(&st, id);
					dbus_message_iter_close_container(&a, &st);
				}
				dbus_message_iter_next(&ids);
			}
		}
		dbus_message_iter_close_container(&it, &a);
	}
	else if (!strcmp(iface, MENU_IFACE) && !strcmp(member, "GetProperty"))
		reply = dbus_message_new_error(msg, DBUS_ERROR_UNKNOWN_PROPERTY, member);
	else if (!strcmp(iface, MENU_IFACE) && !strcmp(member, "Event"))
	{
		dbus_int32_t id = 0;
		const char *event = NULL;
		DBusMessageIter in;
		if (dbus_message_iter_init(msg, &in) && dbus_message_iter_get_arg_type(&in) == DBUS_TYPE_INT32)
		{
			dbus_message_iter_get_basic(&in, &id);
			if (dbus_message_iter_next(&in) && dbus_message_iter_get_arg_type(&in) == DBUS_TYPE_STRING)
				dbus_message_iter_get_basic(&in, &event);
		}
		if (event && !strcmp(event, "clicked"))
			CTrayIcon::getInstance()->menuEvent(id);
		reply = dbus_message_new_method_return(msg);
	}
	else if (!strcmp(iface, MENU_IFACE) && !strcmp(member, "AboutToShow"))
	{
		dbus_bool_t changed = FALSE;
		reply = dbus_message_new_method_return(msg);
		dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &changed, DBUS_TYPE_INVALID);
	}
	else if (!strcmp(iface, MENU_IFACE))
		reply = dbus_message_new_method_return(msg);
	else
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	dbus_connection_send(c, reply, NULL);
	dbus_message_unref(reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

/* the watcher comes and goes with the shell; register again when it is back */
static DBusHandlerResult watchWatcher(DBusConnection *, DBusMessage *msg, void *arg)
{
	if (dbus_message_is_signal(msg, "org.freedesktop.DBus", "NameOwnerChanged"))
	{
		const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
		if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old_owner,
					  DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID) &&
		    !strcmp(name, WATCHER_NAME) && new_owner && *new_owner)
			((CTrayIcon *)arg)->Start(); /* only registers again, it runs already */
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

CTrayIcon *CTrayIcon::getInstance()
{
	static CTrayIcon *instance = NULL;
	if (!instance)
		instance = new CTrayIcon();
	return instance;
}

CTrayIcon::CTrayIcon()
{
	conn = NULL;
	running = false;
	shown_standby = false;
	revision = 1;
}

bool CTrayIcon::standby()
{
	return CNeutrinoApp::getInstance()->getMode() == NeutrinoModes::mode_standby;
}

void CTrayIcon::activate()
{
	g_RCInput->postMsg(standby() ? NeutrinoMessages::STANDBY_OFF : NeutrinoMessages::STANDBY_ON, 0);
}

void CTrayIcon::menuEvent(int id)
{
	if (id == MENU_ID_POWER)
		activate();
	else if (id == MENU_ID_EXIT)
		g_RCInput->postMsg(NeutrinoMessages::EXIT, 0);
}

void CTrayIcon::registerItem()
{
	const char *name = dbus_bus_get_unique_name(conn);
	DBusMessage *call = dbus_message_new_method_call(WATCHER_NAME, WATCHER_PATH, WATCHER_NAME, "RegisterStatusNotifierItem");
	if (!call)
		return;
	dbus_message_append_args(call, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
	dbus_message_set_no_reply(call, TRUE);
	dbus_connection_send(conn, call, NULL);
	dbus_message_unref(call);
}

void CTrayIcon::Start()
{
	if (running)
	{
		registerItem();
		return;
	}
	/* the tray is a desktop thing */
	if (!getenv("WAYLAND_DISPLAY") && !getenv("DISPLAY"))
		return;

	DBusError err;
	dbus_error_init(&err);
	conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
	if (!conn)
	{
		dbus_error_free(&err);
		return;
	}
	dbus_connection_set_exit_on_disconnect(conn, FALSE);

	static const DBusObjectPathVTable item_vtable = { NULL, handleItem, NULL, NULL, NULL, NULL };
	static const DBusObjectPathVTable menu_vtable = { NULL, handleMenu, NULL, NULL, NULL, NULL };
	if (!dbus_connection_register_object_path(conn, ITEM_PATH, &item_vtable, this) ||
	    !dbus_connection_register_object_path(conn, MENU_PATH, &menu_vtable, this))
	{
		printf("[trayicon] object paths not taken\n");
		dbus_connection_close(conn);
		dbus_connection_unref(conn);
		conn = NULL;
		return;
	}
	dbus_bus_add_match(conn, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
			   "member='NameOwnerChanged',arg0='" WATCHER_NAME "'", NULL);
	dbus_connection_add_filter(conn, watchWatcher, this, NULL);

	shown_standby = standby();
	running = true;
	if (pthread_create(&thread, NULL, run, this))
	{
		running = false;
		dbus_connection_close(conn);
		dbus_connection_unref(conn);
		conn = NULL;
		return;
	}
	registerItem();
}

void CTrayIcon::Stop()
{
	if (!running)
		return;
	running = false;
	pthread_join(thread, NULL);
	dbus_connection_close(conn);
	dbus_connection_unref(conn);
	conn = NULL;
}

void *CTrayIcon::run(void *arg)
{
	CTrayIcon *self = (CTrayIcon *)arg;
	while (self->running && dbus_connection_read_write_dispatch(self->conn, 500))
		self->stateChanged();
	return NULL;
}

/* the tooltip and the first menu entry follow the standby state */
void CTrayIcon::stateChanged()
{
	bool now = standby();
	if (now == shown_standby)
		return;
	shown_standby = now;

	DBusMessage *sig = dbus_message_new_signal(ITEM_PATH, ITEM_IFACE, "NewToolTip");
	if (sig)
	{
		dbus_connection_send(conn, sig, NULL);
		dbus_message_unref(sig);
	}
	sig = dbus_message_new_signal(MENU_PATH, MENU_IFACE, "LayoutUpdated");
	if (sig)
	{
		dbus_uint32_t rev = ++revision;
		dbus_int32_t parent = 0;
		dbus_message_append_args(sig, DBUS_TYPE_UINT32, &rev, DBUS_TYPE_INT32, &parent, DBUS_TYPE_INVALID);
		dbus_connection_send(conn, sig, NULL);
		dbus_message_unref(sig);
	}
}
