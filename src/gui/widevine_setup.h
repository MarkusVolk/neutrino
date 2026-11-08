/*
 * Widevine: switched on here, off by default; the CDM's state, and
 * installing, updating or removing it with neutrino-widevine from libstb-hal
 *
 * Copyright (C) 2026 Markus Volk
 *
 * License: GPL-2.0-or-later
 */

#ifndef __widevine_setup_h__
#define __widevine_setup_h__

#include <gui/widget/menue.h>

#include <string>

class CWidevineSetup : public CMenuTarget, CChangeObserver
{
	private:
		std::string version;
		CMenuForwarder *state_item;
		CMenuForwarder *google_item;
		CMenuForwarder *remove_item;

		std::string run(const std::string &args);
		void act(const std::string &args);
		void refresh();
		void offer();

	public:
		CWidevineSetup();
		int exec(CMenuTarget *parent, const std::string &actionKey);
		bool changeNotify(const neutrino_locale_t OptionName, void *Data);
};

#endif
