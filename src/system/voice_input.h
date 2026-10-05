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

#ifndef __voice_input_h__
#define __voice_input_h__

#include <stdint.h>
#include <string>
#include <thread>
#include <vector>

class CVoiceInput
{
	private:
		void *pcm;
		std::thread thread;
		std::vector<int16_t> samples;
		volatile bool running;
		volatile bool stopping;
		volatile bool speech;
		volatile int level;

		void record();

	public:
		CVoiceInput();
		~CVoiceInput();

		bool start(const std::string &device, std::string &error);
		void stop();
		/* false once the speaker has finished or nothing was said */
		bool isRunning() { return running; }
		bool heardSpeech() { return speech; }
		/* 0 to 100 */
		int getLevel() { return level; }
		/* the recording as a WAV file, to be asked for after stop() */
		std::string getWav();
};

struct stt_config
{
	/* up to and including /v1 */
	std::string url;
	std::string key;
	std::string model;
	/* ISO 639-1, or empty to let the server find out */
	std::string language;
};

bool transcribe(const stt_config &config, const std::string &wav, std::string &text, std::string &error,
		const volatile bool *cancel);

#endif
