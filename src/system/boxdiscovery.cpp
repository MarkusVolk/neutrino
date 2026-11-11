/*
 * Boxes with reception in the network, found over SSDP
 *
 * A box whose tuners have received something answers searches for
 * ST_NEUTRINO_TUNER and says so with NOTIFY; LOCATION is its API. Every box
 * searches for the others; the web channel loader asks for their DVB
 * channels (control/xmltv.m3u?dvb=1) and plays them from their streamts.
 *
 * License: GPL
 */

#include <config.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include <global.h>
#include <neutrino.h>
#include <driver/abstime.h>
#include <system/boxdiscovery.h>
#include <system/helpers.h>
#include <system/set_threadname.h>
#include <zapit/bouquets.h>
#include <zapit/femanager.h>
#include <zapit/zapit.h>

extern CBouquetManager *g_bouquetManager;

#define SSDP_IP			"239.255.255.250"
#define SSDP_PORT		1900
#define ST_NEUTRINO_TUNER	"urn:tuxbox-org:device:neutrino-tuner:1"
#define STATE_FILE		CONFIGDIR "/boxdiscovery.state"

/* a box answers a search every 10 minutes and notifies every 30; one that
   was not heard of for an hour is gone, a byebye takes it at once */
#define SEARCH_INTERVAL		600
#define NOTIFY_INTERVAL		1800
#define BOX_TIMEOUT		3600

CBoxDiscovery *CBoxDiscovery::getInstance()
{
	static CBoxDiscovery instance;
	return &instance;
}

/* The MAC the box is known by, from sysfs: the network may not be up when
   Neutrino starts, and an interface without an address is not listed by
   SIOCGIFCONF. eth0 first, otherwise the first interface with a MAC. */
static std::string boxMac()
{
	std::vector<std::string> names(1, "eth0");
	DIR *dir = opendir("/sys/class/net");
	if (dir) {
		std::vector<std::string> others;
		while (struct dirent *e = readdir(dir))
			if (e->d_name[0] != '.' && strcmp(e->d_name, "lo") && strcmp(e->d_name, "eth0"))
				others.push_back(e->d_name);
		closedir(dir);
		std::sort(others.begin(), others.end());
		names.insert(names.end(), others.begin(), others.end());
	}
	for (size_t i = 0; i < names.size(); i++) {
		std::ifstream f(("/sys/class/net/" + names[i] + "/address").c_str());
		std::string mac;
		if (!std::getline(f, mac) || mac.empty() || mac == "00:00:00:00:00:00")
			continue;
		mac.erase(std::remove(mac.begin(), mac.end(), ':'), mac.end());
		return mac;
	}
	return "";
}

CBoxDiscovery::CBoxDiscovery()
{
	running = false;
	pthread_mutex_init(&mutex, NULL);
	group_fd = search_fd = -1;
	joined = false;
	reception = false;
	usn = "uuid:neutrino-" + boxMac();
	/* the boxes of the last run, so their lists are there before the
	   first search is answered */
	loadState();
}

std::vector<std::string> CBoxDiscovery::getBoxes()
{
	std::vector<std::string> ret;
	pthread_mutex_lock(&mutex);
	for (std::map<std::string, Box>::iterator it = boxes.begin(); it != boxes.end(); ++it)
		ret.push_back(it->second.host);
	pthread_mutex_unlock(&mutex);
	return ret;
}

void CBoxDiscovery::Start()
{
	if (running)
		return;

	group_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	search_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (group_fd < 0 || search_fd < 0)
	{
		perror("[boxdiscovery] socket");
		Stop();
		return;
	}

	int on = 1;
	setsockopt(group_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(SSDP_PORT);
	if (bind(group_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		perror("[boxdiscovery] bind 1900");
		Stop();
		return;
	}

	unsigned char ttl = 2;
	setsockopt(search_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

	running = true;
	if (pthread_create(&thread, NULL, run, this))
	{
		running = false;
		Stop();
	}
}

void CBoxDiscovery::Stop()
{
	if (running)
	{
		running = false;
		pthread_join(thread, NULL);
	}
	if (group_fd >= 0)
		close(group_fd);
	if (search_fd >= 0)
		close(search_fd);
	group_fd = search_fd = -1;
	joined = false;
}

void *CBoxDiscovery::run(void *arg)
{
	set_threadname("n:boxdiscovery");
	((CBoxDiscovery *)arg)->loop();
	return NULL;
}

void CBoxDiscovery::loop()
{
	time_t now = time_monotonic();
	time_t next_join = now, next_check = now, next_search = now + 2, next_notify = now;
	int searches = 0;
	bool was_announcing = false;

	while (running)
	{
		now = time_monotonic();
		/* fails while the network is not up yet */
		if (!joined && now >= next_join)
		{
			joined = joinGroup();
			next_join = now + 30;
		}
		if (now >= next_check)
		{
			checkReception();
			next_check = now + 10;
		}
		bool is_announcing = announcing();
		if (is_announcing && (!was_announcing || now >= next_notify))
		{
			notify(true);
			next_notify = now + NOTIFY_INTERVAL;
		}
		was_announcing = is_announcing;
		if (now >= next_search)
		{
			search();
			/* the first ones close together, a box may miss one */
			next_search = now + (++searches < 3 ? 10 : SEARCH_INTERVAL);
		}
		expire();

		struct pollfd fds[2] = { { group_fd, POLLIN, 0 }, { search_fd, POLLIN, 0 } };
		if (poll(fds, 2, 1000) > 0)
			for (int i = 0; i < 2; i++)
				if (fds[i].revents & POLLIN)
					receive(fds[i].fd);
	}

	if (announcing())
		notify(false);
}

bool CBoxDiscovery::joinGroup()
{
	struct ip_mreq mreq;
	mreq.imr_multiaddr.s_addr = inet_addr(SSDP_IP);
	mreq.imr_interface.s_addr = htonl(INADDR_ANY);
	return setsockopt(group_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) == 0;
}

/*
 * A box has reception once a tuner locked. It loses it when it shows a DVB
 * channel and its tuner finds nothing; in standby and with a web channel on
 * screen the last state stays, it is kept across restarts.
 */
void CBoxDiscovery::checkReception()
{
	const char *simulate = getenv("SIMULATE_FE");
	if (simulate && *simulate && strcmp(simulate, "0") != 0)
		return;

	CFEManager *fem = CFEManager::getInstance();
	bool now_reception = reception;
	bool locked = false;
	for (int i = 0; i < fem->getFrontendCount(); i++)
	{
		CFrontend *fe = fem->getFE(i);
		if (fe && fe->tuned)
			locked = true;
	}
	if (locked)
		now_reception = true;
	else if (CNeutrinoApp::getInstance()->getMode() != NeutrinoModes::mode_standby)
	{
		t_channel_id live = CZapit::getInstance()->GetCurrentChannelID();
		CFrontend *live_fe = CZapit::getInstance()->GetLiveFrontend();
		static int misses = 0;
		/* twice in a row, a tune takes a few seconds */
		if (live && !IS_WEBCHAN(live) && live_fe && !live_fe->tuned)
			now_reception = (++misses < 2) && reception;
		else
			misses = 0;
	}

	if (now_reception != reception)
	{
		printf("[boxdiscovery] %s\n", now_reception ? "reception, announcing this box" : "no reception, not announcing this box");
		bool was = announcing();
		reception = now_reception;
		if (was && !announcing())
			notify(false);
		saveState();
	}
}

bool CBoxDiscovery::announcing()
{
	return reception && CFEManager::getInstance()->getFrontendCount() > 0;
}

/* the address this box has toward the given one */
static std::string localAddress(const struct sockaddr_in &to)
{
	std::string ret;
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return ret;
	struct sockaddr_in local;
	socklen_t len = sizeof(local);
	char ip[INET_ADDRSTRLEN];
	if (connect(fd, (const struct sockaddr *)&to, sizeof(to)) == 0 &&
	    getsockname(fd, (struct sockaddr *)&local, &len) == 0 &&
	    inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip)))
		ret = ip;
	close(fd);
	return ret;
}

static std::string location(const struct sockaddr_in &to)
{
	std::string ip = localAddress(to);
	if (ip.empty())
		return ip;
	return "http://" + ip + ":" + to_string(httpdPort()) + "/api/v1/system/info";
}

static struct sockaddr_in ssdpGroup()
{
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr(SSDP_IP);
	addr.sin_port = htons(SSDP_PORT);
	return addr;
}

void CBoxDiscovery::search()
{
	struct sockaddr_in group = ssdpGroup();
	std::string msg = "M-SEARCH * HTTP/1.1\r\n"
		"HOST: " SSDP_IP ":1900\r\n"
		"MAN: \"ssdp:discover\"\r\n"
		"MX: 2\r\n"
		"ST: " ST_NEUTRINO_TUNER "\r\n\r\n";
	sendto(search_fd, msg.c_str(), msg.size(), 0, (struct sockaddr *)&group, sizeof(group));
}

void CBoxDiscovery::notify(bool alive)
{
	struct sockaddr_in group = ssdpGroup();
	std::string loc = location(group);
	if (alive && loc.empty())
		return;
	std::string msg = "NOTIFY * HTTP/1.1\r\n"
		"HOST: " SSDP_IP ":1900\r\n"
		"NT: " ST_NEUTRINO_TUNER "\r\n"
		"USN: " + usn + "::" ST_NEUTRINO_TUNER "\r\n";
	if (alive)
		msg += "NTS: ssdp:alive\r\n"
			"CACHE-CONTROL: max-age=" + to_string(BOX_TIMEOUT) + "\r\n"
			"LOCATION: " + loc + "\r\n"
			"SERVER: Linux UPnP/1.1 Neutrino/1.0\r\n";
	else
		msg += "NTS: ssdp:byebye\r\n";
	msg += "\r\n";
	sendto(search_fd, msg.c_str(), msg.size(), 0, (struct sockaddr *)&group, sizeof(group));
}

void CBoxDiscovery::answer(int fd, const std::string &to_ip, int to_port)
{
	struct sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = inet_addr(to_ip.c_str());
	to.sin_port = htons(to_port);
	std::string loc = location(to);
	if (loc.empty())
		return;
	std::string msg = "HTTP/1.1 200 OK\r\n"
		"CACHE-CONTROL: max-age=" + to_string(BOX_TIMEOUT) + "\r\n"
		"EXT:\r\n"
		"LOCATION: " + loc + "\r\n"
		"SERVER: Linux UPnP/1.1 Neutrino/1.0\r\n"
		"ST: " ST_NEUTRINO_TUNER "\r\n"
		"USN: " + usn + "::" ST_NEUTRINO_TUNER "\r\n\r\n";
	sendto(fd, msg.c_str(), msg.size(), 0, (struct sockaddr *)&to, sizeof(to));
}

void CBoxDiscovery::receive(int fd)
{
	char buf[1536];
	struct sockaddr_in from;
	socklen_t len = sizeof(from);
	ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &len);
	if (n <= 0)
		return;
	buf[n] = 0;

	std::istringstream in(buf);
	std::string first, line;
	std::getline(in, first);
	std::map<std::string, std::string> hdr;
	while (std::getline(in, line))
	{
		if (!line.empty() && line[line.size() - 1] == '\r')
			line.erase(line.size() - 1);
		std::string::size_type colon = line.find(':');
		if (colon == std::string::npos)
			continue;
		std::string key = line.substr(0, colon);
		std::transform(key.begin(), key.end(), key.begin(), ::toupper);
		std::string val = line.substr(colon + 1);
		val.erase(0, val.find_first_not_of(" \t"));
		hdr[key] = val;
	}

	std::string their_usn = hdr["USN"].substr(0, hdr["USN"].find("::"));
	if (!strncasecmp(first.c_str(), "M-SEARCH", 8))
	{
		if ((hdr["ST"] == ST_NEUTRINO_TUNER || hdr["ST"] == "ssdp:all") && announcing())
		{
			char ip[INET_ADDRSTRLEN];
			if (inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip)))
				answer(group_fd, ip, ntohs(from.sin_port));
		}
	}
	else if (their_usn.empty() || their_usn == usn)
		return;
	else if (!strncasecmp(first.c_str(), "NOTIFY", 6) && hdr["NT"] == ST_NEUTRINO_TUNER)
	{
		if (hdr["NTS"] == "ssdp:byebye")
			lost(their_usn);
		else if (hdr["NTS"] == "ssdp:alive")
			found(their_usn, hdr["LOCATION"]);
	}
	else if (!strncasecmp(first.c_str(), "HTTP/1.1 200", 12) && hdr["ST"] == ST_NEUTRINO_TUNER)
		found(their_usn, hdr["LOCATION"]);
}

void CBoxDiscovery::found(const std::string &their_usn, const std::string &loc)
{
	/* http://host:port/... */
	if (loc.compare(0, 7, "http://"))
		return;
	std::string host = loc.substr(7, loc.find('/', 7) - 7);
	if (host.empty())
		return;

	pthread_mutex_lock(&mutex);
	std::map<std::string, Box>::iterator it = boxes.find(their_usn);
	bool known = it != boxes.end() && it->second.host == host;
	if (known)
		it->second.seen = time_monotonic();
	pthread_mutex_unlock(&mutex);
	if (known)
		return;

	/* a new box or a new address: it must answer before its lists are asked for */
	std::string url = loc, reply;
	if (!getUrl(url, reply, AGENT, 3))
		return;
	printf("[boxdiscovery] found %s at %s\n", their_usn.c_str(), host.c_str());

	pthread_mutex_lock(&mutex);
	Box box = { host, time_monotonic() };
	boxes[their_usn] = box;
	pthread_mutex_unlock(&mutex);
	boxesChanged();
}

void CBoxDiscovery::lost(const std::string &their_usn)
{
	pthread_mutex_lock(&mutex);
	bool erased = boxes.erase(their_usn) > 0;
	pthread_mutex_unlock(&mutex);
	if (erased)
	{
		printf("[boxdiscovery] %s left\n", their_usn.c_str());
		boxesChanged();
	}
}

void CBoxDiscovery::expire()
{
	time_t now = time_monotonic();
	bool erased = false;
	pthread_mutex_lock(&mutex);
	for (std::map<std::string, Box>::iterator it = boxes.begin(); it != boxes.end();)
	{
		if (now - it->second.seen > BOX_TIMEOUT)
		{
			printf("[boxdiscovery] %s not heard of for an hour\n", it->first.c_str());
			boxes.erase(it++);
			erased = true;
		}
		else
			++it;
	}
	pthread_mutex_unlock(&mutex);
	if (erased)
		boxesChanged();
}

/* the web channels are built again with the lists of the boxes found now,
   and the guides their lists name are read */
void CBoxDiscovery::boxesChanged()
{
	saveState();
	if (!g_bouquetManager || !g_Zapit)
		return;
	g_bouquetManager->setWebchannelsReloadReason("boxdiscovery");
	g_Zapit->reinitChannels();
	CNeutrinoApp::getInstance()->xmltv_xml_auto_readepg();
}

void CBoxDiscovery::loadState()
{
	std::ifstream in(STATE_FILE);
	std::string key, a, b;
	time_t now = time_monotonic();
	while (in >> key)
	{
		if (key == "reception" && in >> a)
			reception = a == "1";
		else if (key == "box" && in >> a >> b)
		{
			Box box = { b, now };
			boxes[a] = box;
		}
		else
			in.ignore(4096, '\n');
	}
}

void CBoxDiscovery::saveState()
{
	std::string s = "reception " + std::string(reception ? "1" : "0") + "\n";
	pthread_mutex_lock(&mutex);
	for (std::map<std::string, Box>::iterator it = boxes.begin(); it != boxes.end(); ++it)
		s += "box " + it->first + " " + it->second.host + "\n";
	pthread_mutex_unlock(&mutex);
	std::string tmp = STATE_FILE ".tmp";
	std::ofstream out(tmp.c_str());
	out << s;
	out.close();
	if (out)
		rename(tmp.c_str(), STATE_FILE);
}
