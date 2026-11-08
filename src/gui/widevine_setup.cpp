/*
 * Widevine: switched on here, off by default; the CDM's state, and
 * installing, updating or removing it with neutrino-widevine from libstb-hal
 *
 * Copyright (C) 2026 Markus Volk
 *
 * License: GPL-2.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "widevine_setup.h"

#include <stdio.h>

#include <global.h>
#include <neutrino.h>
#include <neutrino_menue.h>
#include <driver/rcinput.h>
#include <gui/widget/hintbox.h>
#include <gui/widget/icons.h>
#include <gui/widget/menue_options.h>
#include <gui/widget/msgbox.h>

/* the value of a "key value" line of the tool's output */
static std::string field(const std::string &text, const std::string &key)
{
	size_t p = 0;
	while ((p = text.find(key + " ", p)) != std::string::npos && p > 0 && text[p - 1] != '\n')
		p++;
	if (p == std::string::npos)
		return "";
	p += key.size() + 1;
	size_t e = text.find('\n', p);
	return text.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

static bool newer(const std::string &a, const std::string &b)
{
	unsigned x[4] = { 0 }, y[4] = { 0 };
	sscanf(a.c_str(), "%u.%u.%u.%u", &x[0], &x[1], &x[2], &x[3]);
	sscanf(b.c_str(), "%u.%u.%u.%u", &y[0], &y[1], &y[2], &y[3]);
	for (int i = 0; i < 4; i++)
		if (x[i] != y[i])
			return x[i] > y[i];
	return false;
}

CWidevineSetup::CWidevineSetup()
	: state_item(NULL), google_item(NULL), remove_item(NULL)
{
}

/* the tool's output, stdout and stderr together */
std::string CWidevineSetup::run(const std::string &args)
{
	std::string out;
	std::string cmd = "neutrino-widevine " + args + " 2>&1";
	FILE *f = popen(cmd.c_str(), "r");
	if (!f)
		return "neutrino-widevine: cannot be started";
	char buf[256];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		out.append(buf, n);
	pclose(f);
	while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
		out.pop_back();
	return out;
}

void CWidevineSetup::act(const std::string &args)
{
	CHintBox hint(LOCALE_WIDEVINE_HEAD, g_Locale->getText(LOCALE_WIDEVINE_BUSY));
	hint.paint();
	std::string out = run(args);
	hint.hide();
	ShowMsg(LOCALE_WIDEVINE_HEAD, out, CMsgBox::mbrOk, CMsgBox::mbOk, NEUTRINO_ICON_INFO);
}

/* the state line and which actions there are, after any change */
void CWidevineSetup::refresh()
{
	std::string status = run("status");
	version = field(status, "version");
	std::string loads = field(status, "loads");
	std::string state;
	if (version.empty())
		state = g_Locale->getText(LOCALE_WIDEVINE_NOT_INSTALLED);
	else if (loads == "yes")
		state = version + " - " + g_Locale->getText(LOCALE_WIDEVINE_LOADS);
	else
		state = version + " - " + g_Locale->getText(LOCALE_WIDEVINE_DOES_NOT_LOAD) + " " +
			(loads.compare(0, 4, "no: ") ? loads : loads.substr(4));
	if (!state_item)
		return;
	bool on = g_settings.widevine;
	state_item->setName(state);
	google_item->setName(version.empty() ? LOCALE_WIDEVINE_INSTALL : LOCALE_WIDEVINE_UPDATE);
	google_item->setActive(on);
	remove_item->setActive(on && !version.empty());
}

/* switched on: without a CDM install one, with an older one update it */
void CWidevineSetup::offer()
{
	if (version.empty())
	{
		if (ShowMsg(LOCALE_WIDEVINE_HEAD, LOCALE_WIDEVINE_INSTALL_ASK, CMsgBox::mbrYes, CMsgBox::mbYes | CMsgBox::mbNo) == CMsgBox::mbrYes)
			act("install");
		return;
	}
	std::string latest;
	{
		CHintBox hint(LOCALE_WIDEVINE_HEAD, g_Locale->getText(LOCALE_WIDEVINE_BUSY));
		hint.paint();
		latest = field(run("latest"), "latest");
		hint.hide();
	}
	if (latest.empty() || !newer(latest, version))
		return;
	char text[256];
	snprintf(text, sizeof(text), g_Locale->getText(LOCALE_WIDEVINE_UPDATE_ASK), version.c_str(), latest.c_str());
	if (ShowMsg(LOCALE_WIDEVINE_HEAD, text, CMsgBox::mbrYes, CMsgBox::mbYes | CMsgBox::mbNo) == CMsgBox::mbrYes)
		act("update");
}

bool CWidevineSetup::changeNotify(const neutrino_locale_t OptionName, void *)
{
	if (OptionName == LOCALE_WIDEVINE_ENABLED && g_settings.widevine)
		offer();
	refresh();
	return true;
}

int CWidevineSetup::exec(CMenuTarget *parent, const std::string &actionKey)
{
	if (parent)
		parent->hide();

	if (actionKey == "google")
	{
		act(version.empty() ? "install" : "update");
		refresh();
		return menu_return::RETURN_REPAINT;
	}
	if (actionKey == "remove")
	{
		if (ShowMsg(LOCALE_WIDEVINE_HEAD, LOCALE_WIDEVINE_REMOVE_ASK, CMsgBox::mbrNo, CMsgBox::mbYes | CMsgBox::mbNo) == CMsgBox::mbrYes)
			run("remove");
		refresh();
		return menu_return::RETURN_REPAINT;
	}

	CMenuWidget m(LOCALE_MAINSETTINGS_MULTIMEDIA, NEUTRINO_ICON_SETTINGS, 50, MN_WIDGET_ID_WIDEVINESETUP);
	m.addIntroItems(LOCALE_WIDEVINE_HEAD);

	CMenuOptionChooser *on = new CMenuOptionChooser(LOCALE_WIDEVINE_ENABLED, &g_settings.widevine,
		OPTIONS_OFF0_ON1_OPTIONS, OPTIONS_OFF0_ON1_OPTION_COUNT, true, this);
	on->setHint(NEUTRINO_ICON_HINT_MOVIE, LOCALE_MENU_HINT_WIDEVINE_ENABLED);
	m.addItem(on);
	m.addItem(GenericMenuSeparatorLine);

	state_item = new CMenuForwarder("", false);
	state_item->setHint(NEUTRINO_ICON_HINT_MOVIE, LOCALE_MENU_HINT_WIDEVINE);
	m.addItem(state_item);
	google_item = new CMenuForwarder(LOCALE_WIDEVINE_INSTALL, true, NULL, this, "google", CRCInput::RC_red);
	google_item->setHint(NEUTRINO_ICON_HINT_MOVIE, LOCALE_MENU_HINT_WIDEVINE_GOOGLE);
	m.addItem(google_item);
	remove_item = new CMenuForwarder(LOCALE_WIDEVINE_REMOVE, true, NULL, this, "remove", CRCInput::RC_green);
	m.addItem(remove_item);
	refresh();

	int res = m.exec(NULL, "");
	state_item = google_item = remove_item = NULL;
	return res;
}
