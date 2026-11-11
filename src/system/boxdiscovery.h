/*
 * Boxes with reception in the network, found over SSDP
 *
 * A box whose tuners have received something announces itself; every box
 * looks for the others and plays their DVB channels as web channels.
 *
 * License: GPL
 */

#ifndef __boxdiscovery_h__
#define __boxdiscovery_h__

#include <pthread.h>
#include <time.h>

#include <map>
#include <string>
#include <vector>

class CBoxDiscovery
{
	public:
		static CBoxDiscovery *getInstance();
		void Start();
		void Stop();
		/* the web interfaces (host:port) of the other boxes with reception */
		std::vector<std::string> getBoxes();

	private:
		struct Box
		{
			std::string host;
			time_t seen;
		};

		CBoxDiscovery();
		static void *run(void *arg);
		void loop();
		bool joinGroup();
		void checkReception();
		bool announcing();
		void search();
		void notify(bool alive);
		void receive(int fd);
		void answer(int fd, const std::string &to_ip, int to_port);
		void found(const std::string &usn, const std::string &location);
		void lost(const std::string &usn);
		void expire();
		void boxesChanged();
		void loadState();
		void saveState();

		pthread_t thread;
		bool running;
		pthread_mutex_t mutex;
		int group_fd;
		int search_fd;
		bool joined;
		bool reception;
		std::string usn;
		std::map<std::string, Box> boxes;
};

#endif
