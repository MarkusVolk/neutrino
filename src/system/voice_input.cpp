/*
	Voice input for the assistant: a recording from an ALSA device and its
	transcription by a server with the OpenAI audio API

	License: GPL

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public
	License as published by the Free Software Foundation; either
	version 2 of the License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "voice_input.h"
#include "http_request.h"
#include "llm_client.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alsa/asoundlib.h>

#define RATE		16000
#define CHUNK		(RATE / 10)
/* in chunks */
#define MAX_LENGTH	300
#define MAX_WAIT	80
#define END_SILENCE	15
#define SKIP_START	3
/* what a tenth of a second of speech has at least, as RMS of 16 bit samples */
#define MIN_SPEECH	250
#define MAX_GAIN	16

CVoiceInput::CVoiceInput()
{
	pcm = NULL;
	running = false;
	stopping = false;
	speech = false;
	level = 0;
}

CVoiceInput::~CVoiceInput()
{
	stop();
}

bool CVoiceInput::start(const std::string &device, std::string &error)
{
	stop();
	snd_pcm_t *handle = NULL;
	int err = snd_pcm_open(&handle, device.empty() ? "default" : device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
	if (err >= 0)
	{
		err = snd_pcm_set_params(handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, RATE, 1, 200000);
		if (err < 0)
			snd_pcm_close(handle);
	}
	if (err < 0)
	{
		error = snd_strerror(err);
		return false;
	}
	pcm = handle;
	samples.clear();
	speech = false;
	stopping = false;
	level = 0;
	running = true;
	thread = std::thread(&CVoiceInput::record, this);
	return true;
}

/*
	Runs until stop(), until the speaker has been silent for a while or
	until nobody spoke at all. Speech is what is clearly louder than the
	quietest tenth of a second so far; the first chunks are left out of
	that, as a device may click when it starts.
*/
void CVoiceInput::record()
{
	snd_pcm_t *handle = (snd_pcm_t *)pcm;
	int16_t chunk[CHUNK];
	double floor = 0;
	int chunks = 0, silent = 0, peak = 0;

	while (!stopping && chunks < MAX_LENGTH)
	{
		snd_pcm_sframes_t n = snd_pcm_readi(handle, chunk, CHUNK);
		if (n < 0)
		{
			if (snd_pcm_recover(handle, n, 1) < 0)
				break;
			continue;
		}
		if (n == 0)
			continue;
		samples.insert(samples.end(), chunk, chunk + n);

		double sum = 0;
		for (snd_pcm_sframes_t i = 0; i < n; i++)
			sum += (double)chunk[i] * chunk[i];
		double rms = sqrt(sum / n);
		int percent = rms > 1 ? (int)(20 * log10(rms / 32768.0) + 60) * 100 / 60 : 0;
		level = percent < 0 ? 0 : (percent > 100 ? 100 : percent);

		if (++chunks <= SKIP_START)
			continue;
		if (rms > peak)
			peak = (int)rms;
		if (chunks == SKIP_START + 1 || rms < floor)
			floor = rms;

		double threshold = floor * 4 > MIN_SPEECH ? floor * 4 : MIN_SPEECH;
		if (rms > threshold)
		{
			speech = true;
			silent = 0;
		}
		else if (speech && ++silent >= END_SILENCE)
			break;
		else if (!speech && chunks >= MAX_WAIT)
			break;
	}
	/* tells a microphone that is muted or too quiet from a speaker who said nothing */
	printf("[voice] %.1f s recorded, loudest %d of 32767, quietest %d, %s\n", chunks / 10.0, peak, (int)floor,
	       speech ? "speech" : "no speech");
	level = 0;
	running = false;
}

void CVoiceInput::stop()
{
	stopping = true;
	if (thread.joinable())
		thread.join();
	running = false;
	if (pcm)
	{
		snd_pcm_close((snd_pcm_t *)pcm);
		pcm = NULL;
	}
}

static void put_le(std::string &out, uint32_t value, int bytes)
{
	for (int i = 0; i < bytes; i++)
		out += (char)((value >> (8 * i)) & 0xff);
}

std::string CVoiceInput::getWav()
{
	std::string wav;
	uint32_t size = samples.size() * 2;
	wav.reserve(44 + size);
	wav += "RIFF";
	put_le(wav, 36 + size, 4);
	wav += "WAVEfmt ";
	put_le(wav, 16, 4);
	put_le(wav, 1, 2);
	put_le(wav, 1, 2);
	put_le(wav, RATE, 4);
	put_le(wav, RATE * 2, 4);
	put_le(wav, 2, 2);
	put_le(wav, 16, 2);
	wav += "data";
	put_le(wav, size, 4);
	/* a quiet microphone is brought up to half of the range, the recogniser hears it better then */
	int peak = 1;
	for (size_t i = 0; i < samples.size(); i++)
		if (abs(samples[i]) > peak)
			peak = abs(samples[i]);
	int gain = peak < 16000 ? 16000 / peak : 1;
	if (gain > MAX_GAIN)
		gain = MAX_GAIN;
	for (size_t i = 0; i < samples.size(); i++)
		put_le(wav, (uint16_t)(int16_t)(samples[i] * gain), 2);
	return wav;
}

bool transcribe(const stt_config &config, const std::string &wav, std::string &text, std::string &error,
		const volatile bool *cancel)
{
	if (config.url.empty())
	{
		error = "no server";
		return false;
	}
	std::string url = config.url;
	while (!url.empty() && url[url.size() - 1] == '/')
		url.erase(url.size() - 1);
	url += "/audio/transcriptions";

	std::vector<std::string> headers;
	if (!config.key.empty())
		headers.push_back("Authorization: Bearer " + config.key);

	std::vector<http_form_field> fields;
	http_form_field field;
	field.name = "file";
	field.value = wav;
	field.filename = "speech.wav";
	field.content_type = "audio/wav";
	fields.push_back(field);
	field = http_form_field();
	field.name = "model";
	field.value = config.model;
	fields.push_back(field);
	field.name = "response_format";
	field.value = "json";
	fields.push_back(field);
	if (!config.language.empty())
	{
		field.name = "language";
		field.value = config.language;
		fields.push_back(field);
	}

	http_reply http;
	if (!http_post_form(url, headers, fields, http, cancel))
	{
		error = http.error;
		return false;
	}
	Json::Value root;
	bool parsed = json_parse(http.body, root) && root.isObject();
	if (http.status != 200 || !parsed || !root["text"].isString())
	{
		char status[32];
		snprintf(status, sizeof(status), "HTTP %ld", http.status);
		error = status;
		if (parsed && root["error"]["message"].isString())
			error = root["error"]["message"].asString();
		else if (parsed && root["error"].isString())
			error = root["error"].asString();
		return false;
	}
	text = root["text"].asString();
	size_t first = text.find_first_not_of(" \t\r\n");
	size_t last = text.find_last_not_of(" \t\r\n");
	text = first == std::string::npos ? "" : text.substr(first, last - first + 1);
	return true;
}
