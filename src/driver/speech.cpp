/*
	Speech output: menus, lists and messages read aloud with eSpeak NG

	License: GPL

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License as published
	by the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "speech.h"

#include <alsa/asoundlib.h>
#include <espeak-ng/speak_lib.h>

#include <global.h>
#include <driver/abstime.h>
#include <eitd/sectionsd.h>
#include <gui/widget/remote_path.h>
#include <gui/widget/remote_ui.h>
#include <system/helpers.h>
#include <system/set_threadname.h>
#include <system/settings.h>
#include <zapit/getservices.h>
#include <zapit/zapit.h>

/* how much sound ALSA holds: a key cuts the sentence off after at most this */
#define SPEECH_LATENCY_US	100000
#define SPEECH_VOLUME_DELAY_MS	600

namespace
{

/* the thread that synthesises; the callback writes to it and stops when
 * the generation it started with is over */
struct playback
{
	snd_pcm_t *pcm;
	unsigned int generation;
	const std::atomic<unsigned int> *current;
};

int synth_callback(short *wav, int numsamples, espeak_EVENT *events)
{
	playback *p = (playback *)events->user_data;
	if (*p->current != p->generation)
		return 1;
	while (wav && numsamples > 0) {
		snd_pcm_sframes_t n = snd_pcm_writei(p->pcm, wav, numsamples);
		if (n < 0)
			n = snd_pcm_recover(p->pcm, n, 1);
		if (n < 0)
			return 1;
		wav += n;
		numsamples -= n;
		if (*p->current != p->generation)
			return 1;
	}
	return 0;
}

} /* namespace */

CSpeech *CSpeech::getInstance()
{
	static CSpeech *me = NULL;
	if (!me)
		me = new CSpeech();
	return me;
}

CSpeech::CSpeech()
	: due(0), generation(0), rate(0), quit(false), last_ui(NULL)
{
	languageChanged();
	thread = std::thread(&CSpeech::run, this);
}

CSpeech::~CSpeech()
{
	{
		std::lock_guard<std::mutex> g(lock);
		quit = true;
		generation++;
	}
	wake.notify_all();
	if (thread.joinable())
		thread.join();
}

void CSpeech::languageChanged()
{
	std::string lang = g_settings.language;
	std::string iso = Lang2ISO639_1(lang);
	std::lock_guard<std::mutex> g(lock);
	voice = iso.empty() ? "en" : iso;
	last_ui = NULL;
}

void CSpeech::say(const std::string &text, int delay_ms, bool always)
{
	if ((!g_settings.speech_enabled && !always) || text.empty())
		return;
	{
		std::lock_guard<std::mutex> g(lock);
		pending = text;
		due = time_monotonic_ms() + delay_ms;
		rate = 105 + 35 * g_settings.speech_rate;
		generation++;
	}
	wake.notify_all();
}

void CSpeech::stop()
{
	std::lock_guard<std::mutex> g(lock);
	pending.clear();
	generation++;
}

void CSpeech::key()
{
	stop();
}

/* a new window: its title and what is selected in it; the same window:
 * only what is selected, when that changed */
void CSpeech::settled()
{
	if (!g_settings.speech_enabled || remote_path::hidden())
		return;
	CRemoteUi *ui = CRemoteUi::top();
	if (!ui) {
		last_ui = NULL;
		return;
	}
	std::string title = ui->remoteSpeakTitle();
	std::string focus = ui->remoteSpeakFocus();
	if (ui != last_ui || title != last_title) {
		std::string text = title;
		if (!focus.empty())
			text += (text.empty() ? "" : ". ") + focus;
		say(text);
	} else if (focus != last_focus)
		say(focus);
	last_ui = ui;
	last_title = title;
	last_focus = focus;
}

void CSpeech::toggle()
{
	g_settings.speech_enabled = !g_settings.speech_enabled;
	last_ui = NULL;
	say(g_Locale->getText(g_settings.speech_enabled ? LOCALE_SPEECH_ON : LOCALE_SPEECH_OFF), 0, true);
}

void CSpeech::zapped()
{
	if (!g_settings.speech_enabled)
		return;
	CZapitChannel *channel = CServiceManager::getInstance()->FindChannel(CZapit::getInstance()->GetCurrentChannelID());
	if (!channel)
		return;
	std::string text = to_string(channel->number) + ", " + channel->getName();
	CSectionsdClient::CurrentNextInfo info;
	CEitManager::getInstance()->getCurrentNextServiceKey(channel->getEpgID(), info);
	if (info.flags & CSectionsdClient::epgflags::has_current)
		text += ". " + info.current_name;
	say(text);
}

void CSpeech::volume(int value)
{
	char text[100];
	snprintf(text, sizeof(text), g_Locale->getText(LOCALE_SPEECH_VOLUME), value);
	say(text, SPEECH_VOLUME_DELAY_MS);
}

void CSpeech::muted(bool on)
{
	say(g_Locale->getText(on ? LOCALE_SPEECH_MUTED : LOCALE_SPEECH_UNMUTED));
}

void CSpeech::run()
{
	set_threadname("n:speech");
	int sample_rate = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 0, NULL, 0);
	if (sample_rate <= 0) {
		printf("[speech] eSpeak NG did not start\n");
		return;
	}
	espeak_SetSynthCallback(synth_callback);
	std::string voice_set;

	std::unique_lock<std::mutex> g(lock);
	while (!quit) {
		if (pending.empty()) {
			wake.wait(g);
			continue;
		}
		uint64_t now = time_monotonic_ms();
		if (now < due) {
			wake.wait_for(g, std::chrono::milliseconds(due - now));
			continue;
		}
		std::string text = pending;
		pending.clear();
		unsigned int gen = generation;
		std::string v = voice;
		int r = rate;
		g.unlock();

		if (v != voice_set && espeak_SetVoiceByName(v.c_str()) == EE_OK)
			voice_set = v;
		espeak_SetParameter(espeakRATE, r, 0);

		snd_pcm_t *pcm = NULL;
		if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) == 0 &&
		    snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED, 1, sample_rate, 1, SPEECH_LATENCY_US) == 0) {
			playback p = { pcm, gen, &generation };
			espeak_Synth(text.c_str(), text.size() + 1, 0, POS_CHARACTER, 0, espeakCHARS_UTF8, NULL, &p);
			if (generation == gen)
				snd_pcm_drain(pcm);
			else
				snd_pcm_drop(pcm);
		} else
			printf("[speech] no sound device\n");
		if (pcm)
			snd_pcm_close(pcm);
		g.lock();
	}
	g.unlock();
	espeak_Terminate();
}
