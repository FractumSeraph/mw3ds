// The self-updater (FractumSeraph/mw3ds): see mwmp/selfupdate.hpp. Driven a frame at a time like the online mode's
// requests (HttpRequest), so the title screen keeps drawing while GitHub answers.
#include "mwmp/selfupdate.hpp"

#include <3ds.h>
#include <citro2d.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "components/debug/debuglog.hpp"
#include "components/files/dataversion.hpp"
#include "extern/cjson/cJSON.h"
#include "mwgui/loadingscreen.hpp"
#include "mwgui/widgets.hpp"
#include "mwinput/inputmanagerimp.hpp"
#include "mwmp/netsocket.hpp"
#include "mwmp/online.hpp"
#include "mwmp/websocket.hpp"
#include "mwsound/soundmanagerimp.hpp"
#include "platform/linear.hpp"

#ifdef __3DS__
extern int __system_argc;
extern char** __system_argv;
#endif

#ifndef MW3DS_VERSION_TEXT
#define MW3DS_VERSION_TEXT "dev"
#endif

// This program's version (the PC test build: MW3DS_UPDATE_SELF_VERSION, to stand in for a release's)
static const char* ownVersion()
{
#ifndef __3DS__
	if (const char* v = getenv("MW3DS_UPDATE_SELF_VERSION"))
		return v;
#endif
	return MW3DS_VERSION_TEXT;
}

int dataVersionOf(const char* dataDir)
{
	std::string path = std::string(dataDir) + "/data_version.txt";
	FILE* f = fopen(path.c_str(), "r");
	if (!f)
		return 0;
	int v = 0;
	if (fscanf(f, "%d", &v) != 1)
		v = 0;
	fclose(f);
	return v;
}

namespace
{
const char* const kLatest = "https://api.github.com/repos/FractumSeraph/mw3ds/releases/latest";
const char* const kOffPath = "sdmc:/3ds/mw3ds/update_off";
const char* const kUrlPath = "sdmc:/3ds/mw3ds/update_url.txt";
const char* const kSdData = "sdmc:/3ds/mw3ds/data";
const char* const kSdDataPart = "sdmc:/3ds/mw3ds/data.part";
const char* const kSdDataOld = "sdmc:/3ds/mw3ds/data.old";
const char* const kCiaAsset = "mw3ds-program.cia";
const char* const kHbAsset = "mw3ds.3dsx";
std::string g_answer;                    // the test hook's answer to the screen ("" a player's)

// ---- SHA-256 (FIPS 180-4), for the downloads' check against GitHub's digest
struct Sha256
{
	uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	uint8_t block[64];
	size_t used = 0;
	uint64_t total = 0;
	static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
	void compress(const uint8_t* p)
	{
		static const uint32_t k[64] = {
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
			0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
			0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
			0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
			0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
			0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
			0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
		uint32_t w[64];
		for (int i = 0; i < 16; i++)
			w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
		for (int i = 16; i < 64; i++)
			w[i] = w[i - 16] + (ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7]
				+ (ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10));
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
		for (int i = 0; i < 64; i++)
		{
			uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
			uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
			hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
	}
	void add(const void* data, size_t n)
	{
		const uint8_t* p = (const uint8_t*)data;
		total += n;
		while (n)
		{
			size_t take = 64 - used < n ? 64 - used : n;
			memcpy(block + used, p, take);
			used += take;
			p += take;
			n -= take;
			if (used == 64)
			{
				compress(block);
				used = 0;
			}
		}
	}
	std::string hex()
	{
		uint64_t bits = total * 8;
		uint8_t pad = 0x80;
		add(&pad, 1);
		pad = 0;
		while (used != 56)
			add(&pad, 1);
		uint8_t len[8];
		for (int i = 0; i < 8; i++)
			len[i] = (uint8_t)(bits >> (56 - i * 8));
		add(len, 8);
		char out[65];
		for (int i = 0; i < 8; i++)
			snprintf(out + i * 8, 9, "%08lx", (unsigned long)h[i]);
		return out;
	}
};

std::string lowerText(std::string s)
{
	for (auto& c : s)
		c = (char)tolower((unsigned char)c);
	return s;
}

std::string sha256Hex(const std::string& data)
{
	Sha256 s;
	s.add(data.data(), data.size());
	return s.hex();
}

// ---- versions: "mp-0.2.0", "0.2.0", a build past a tag "0.1.0-25-g3ecab94" (ahead of 0.1.0, behind 0.1.1)
struct Version
{
	int n[3] = { 0, 0, 0 };
	int ahead = 0;
	bool ok = false;
	explicit Version(const std::string& s)
	{
		// (the fork's tags are mp-X.Y.Z, the original's X.Y.Z)
		const char* p = s.c_str();
		if (!strncmp(p, "mp-", 3))
			p += 3;
		if (*p == 'v' || *p == 'V')
			p++;
		int a, b, c, used = 0;
		if (sscanf(p, "%d.%d.%d%n", &a, &b, &c, &used) != 3)
			return;
		n[0] = a; n[1] = b; n[2] = c;
		ok = true;
		if (p[used] == '-')
			ahead = atoi(p + used + 1);
	}
	bool newerThan(const Version& o) const
	{
		for (int i = 0; i < 3; i++)
			if (n[i] != o.n[i])
				return n[i] > o.n[i];
		return ahead > o.ahead;
	}
};

// One GET, its redirects followed (GitHub's release files are served from another host)
struct Fetch
{
	HttpRequest req;
	std::string url;
	int redirects = 0;
	bool failed = false, done = false;
	std::string err;
	void start(const std::string& u, bool api)
	{
		url = u;
		redirects = 0;
		failed = done = false;
		err.clear();
		begin(api);
	}
	void begin(bool api)
	{
		req.sendOrigin = false;
		std::vector<std::string> h;
		if (api)
		{
			h.push_back("Accept: application/vnd.github+json");
			h.push_back("X-GitHub-Api-Version: 2022-11-28");
		}
		else
			h.push_back("Accept: application/octet-stream");
		if (!req.start("GET", url, h, ""))
		{
			failed = true;
			err = req.error();
		}
	}
	void update()
	{
		if (failed || done)
			return;
		HttpRequest::State s = req.update();
		if (s == HttpRequest::FAILED)
		{
			failed = true;
			err = req.error();
		}
		else if (s == HttpRequest::DONE)
		{
			int st = req.status;
			if ((st == 301 || st == 302 || st == 303 || st == 307 || st == 308) && redirects < 5)
			{
				std::string loc = req.header("location");
				if (loc.empty())
				{
					failed = true;
					err = "a redirect without a Location";
					return;
				}
				if (loc[0] == '/')
				{
					NetUrl u;
					u.parse(url);
					loc = (u.tls ? "https://" : "http://") + u.host
						+ (u.port != (u.tls ? 443 : 80) ? ":" + std::to_string(u.port) : "") + loc;
				}
				url = loc;
				redirects++;
				begin(false);
			}
			else if (st != 200)
			{
				failed = true;
				char b[48];
				snprintf(b, sizeof(b), "the server answered %d", st);
				err = b;
			}
			else
				done = true;
		}
	}
};

struct Asset
{
	std::string url, sha256;
	size_t size = 0;
	bool ok() const { return !url.empty(); }
};

struct Release
{
	std::string version, notes;
	Asset cia, hb, info;
	int dataNeeded = 0;
};

enum CheckState { CHECK_IDLE, CHECK_RELEASE, CHECK_INFO, CHECK_DONE };
struct Checker
{
	CheckState state = CHECK_IDLE;
	bool started = false;
	Fetch fetch;
	Release rel;
	std::string offer, err;
	bool ourNet = false;                 // the check started the socket service (the title gives it back after)
} g_check;

std::string trimLine(std::string s)
{
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
		s.pop_back();
	return s;
}

std::string latestUrl()
{
	if (FILE* f = fopen(kUrlPath, "r"))
	{
		char line[512] = "";
		bool got = fgets(line, sizeof(line), f) != nullptr;
		fclose(f);
		std::string u = trimLine(line);
		if (got && !u.empty())
			return u;
	}
	return kLatest;
}

bool onWifi()
{
#ifdef __3DS__
	u32 wifi = 0;
	if (R_FAILED(acInit()))
		return false;
	Result rc = ACU_GetWifiStatus(&wifi);
	acExit();
	return R_SUCCEEDED(rc) && wifi != 0;
#else
	return getenv("MW3DS_UPDATE_OFFLINE") == nullptr;
#endif
}

void readAsset(const cJSON* a, Asset& out)
{
	const cJSON* url = cJSON_GetObjectItem(a, "browser_download_url");
	const cJSON* size = cJSON_GetObjectItem(a, "size");
	const cJSON* digest = cJSON_GetObjectItem(a, "digest");
	if (!cJSON_IsString(url))
		return;
	out.url = url->valuestring;
	out.size = cJSON_IsNumber(size) ? (size_t)size->valuedouble : 0;
	if (cJSON_IsString(digest) && strncmp(digest->valuestring, "sha256:", 7) == 0)
		out.sha256 = digest->valuestring + 7;
}

// The release's JSON: its version, notes and the three files (update.json, the program-only CIA, the 3dsx)
bool readRelease(const std::string& body, Release& r, std::string& err)
{
	cJSON* j = cJSON_Parse(body.c_str());
	if (!j)
	{
		err = "GitHub's answer could not be read";
		return false;
	}
	const cJSON* tag = cJSON_GetObjectItem(j, "tag_name");
	const cJSON* notes = cJSON_GetObjectItem(j, "body");
	r.version = cJSON_IsString(tag) ? tag->valuestring : "";
	r.notes = cJSON_IsString(notes) ? notes->valuestring : "";
	const cJSON* a;
	cJSON_ArrayForEach(a, cJSON_GetObjectItem(j, "assets"))
	{
		const cJSON* name = cJSON_GetObjectItem(a, "name");
		if (!cJSON_IsString(name))
			continue;
		if (!strcmp(name->valuestring, kCiaAsset))
			readAsset(a, r.cia);
		else if (!strcmp(name->valuestring, kHbAsset))
			readAsset(a, r.hb);
		else if (!strcmp(name->valuestring, "update.json"))
			readAsset(a, r.info);
	}
	cJSON_Delete(j);
	if (r.version.empty())
	{
		err = "the release has no version";
		return false;
	}
	return true;
}

void checkFinished()
{
	g_check.state = CHECK_DONE;
	Version mine(ownVersion()), theirs(g_check.rel.version);
	if (!g_check.err.empty())
		logf("update: check failed: %s", g_check.err.c_str());
	else if (!mine.ok)
		logf("update: this build is %s (no release version): not offered %s", ownVersion(),
			g_check.rel.version.c_str());
	else if (theirs.ok && theirs.newerThan(mine) && (g_check.rel.cia.ok() || g_check.rel.hb.ok()))
	{
		g_check.offer = g_check.rel.version;
		logf("update: %s is out (this is %s; it needs data %d)", g_check.offer.c_str(), ownVersion(),
			g_check.rel.dataNeeded);
	}
	else
		logf("update: %s is the latest (this is %s)", g_check.rel.version.c_str(), ownVersion());
	// (the socket service back, unless the online mode has come to use it meanwhile)
	if (g_check.ourNet && !g_online)
		netExit();
	g_check.ourNet = false;
}
}

void selfUpdateCheck()
{
	if (g_check.started)
		return;
	g_check.started = true;
	struct stat st;
	if (stat(kOffPath, &st) == 0)
	{
		logf("update: checks are off (%s)", kOffPath);
		return;
	}
	if (!onWifi())
	{
		logf("update: no Wi-Fi, not checked");
		return;
	}
	g_check.ourNet = !netReady();
	if (!netInit())
		return;
	std::string url = latestUrl();
	logf("update: asking %s", url.c_str());
	g_check.state = CHECK_RELEASE;
	g_check.fetch.start(url, true);
}

void selfUpdatePoll()
{
	if (g_check.state != CHECK_RELEASE && g_check.state != CHECK_INFO)
		return;
	Fetch& f = g_check.fetch;
	f.update();
	if (f.failed)
	{
		g_check.err = f.err;
		checkFinished();
		return;
	}
	if (!f.done)
		return;
	if (g_check.state == CHECK_RELEASE)
	{
		if (!readRelease(f.req.body, g_check.rel, g_check.err))
		{
			checkFinished();
			return;
		}
		if (g_check.rel.info.ok())
		{
			g_check.state = CHECK_INFO;
			f.start(g_check.rel.info.url, false);
			return;
		}
	}
	else if (cJSON* j = cJSON_Parse(f.req.body.c_str()))
	{
		const cJSON* d = cJSON_GetObjectItem(j, "data");
		if (cJSON_IsNumber(d))
			g_check.rel.dataNeeded = d->valueint;
		cJSON_Delete(j);
	}
	checkFinished();
}

const std::string& selfUpdateOffer()
{
	return g_check.offer;
}

void selfUpdateStop()
{
	if (g_check.state != CHECK_RELEASE && g_check.state != CHECK_INFO)
		return;
	g_check.fetch.req.cancel();
	g_check.err = "the game started first";
	checkFinished();
}

// ---- the update itself
namespace
{
enum Step { ASK, BLOCKED, DOWNLOAD, COPY_LIST, COPY, INSTALL, DONE, FAILED };

struct CopyJob
{
	std::vector<std::string> dirs;        // relative to the data folder, parents first
	std::vector<std::pair<std::string, size_t>> files;
	size_t total = 0, done = 0, next = 0;
	FILE* in = nullptr;
	FILE* out = nullptr;
	std::vector<char> buf;
};

void listTree(const std::string& root, const std::string& rel, CopyJob& job)
{
	DIR* d = opendir((root + rel).c_str());
	if (!d)
		return;
	std::vector<std::string> subs;
	while (dirent* e = readdir(d))
	{
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		std::string r = rel + "/" + e->d_name;
		struct stat st;
		if (stat((root + r).c_str(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
			subs.push_back(r);
		else
		{
			job.files.push_back({ r, (size_t)st.st_size });
			job.total += st.st_size;
		}
	}
	closedir(d);
	for (auto& s : subs)
	{
		job.dirs.push_back(s);
		listTree(root, s, job);
	}
}

void removeTree(const std::string& path)
{
	if (DIR* d = opendir(path.c_str()))
	{
		std::vector<std::string> names;
		while (dirent* e = readdir(d))
			if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
				names.push_back(e->d_name);
		closedir(d);
		for (auto& n : names)
		{
			std::string p = path + "/" + n;
			struct stat st;
			if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
				removeTree(p);
			else
				remove(p.c_str());
		}
	}
	rmdir(path.c_str());
}

// Free space on the SD card (bytes; a large number when the console can't say)
unsigned long long sdFree()
{
#ifdef __3DS__
	FS_ArchiveResource r;
	if (R_SUCCEEDED(FSUSER_GetArchiveResource(&r, SYSTEM_MEDIATYPE_SD)))
		return (unsigned long long)r.freeClusters * r.clusterSize;
#endif
	return ~0ull;
}

// Where this copy runs from: a CIA (installed), or a .3dsx the Homebrew Launcher started (its path)
bool runsAsHomebrew(std::string& path)
{
#ifdef __3DS__
	if (!envIsHomebrew())
		return false;
	if (__system_argc > 0 && __system_argv && __system_argv[0] && strncmp(__system_argv[0], "sdmc:/", 6) == 0)
		path = __system_argv[0];
	return true;
#else
	if (const char* p = getenv("MW3DS_UPDATE_3DSX"))
	{
		path = p;
		return true;
	}
	return false;
#endif
}

// The data inside the CIA (romfs), which the program-only CIA won't have
bool dataInsideCia(const char* dataDir)
{
#ifndef __3DS__
	if (getenv("MW3DS_UPDATE_FAKE_ROMFS"))
		return true;
#endif
	return !strncmp(dataDir, "romfs:", 6);
}

std::string mb(size_t bytes)
{
	char b[32];
	snprintf(b, sizeof(b), "%.1f MB", bytes / 1048576.0);
	return b;
}

struct Updater
{
	const char* dataDir;
	Step step = ASK;
	std::string message, hbPath;
	bool homebrew = false;
	Asset asset;
	Fetch fetch;
	std::string file;                     // the downloaded program, checked
	CopyJob copy;
	std::string copyFrom;
	size_t installed = 0;
#ifdef __3DS__
	Handle cia = 0;
#endif

	void fail(const std::string& why)
	{
		logf("update: failed: %s", why.c_str());
		message = why;
		step = FAILED;
		if (copy.in) { fclose(copy.in); copy.in = nullptr; }
		if (copy.out) { fclose(copy.out); copy.out = nullptr; }
	}

	void block(const std::string& why)
	{
		logf("update: not installed: %s", why.c_str());
		message = why;
		step = BLOCKED;
	}

	void begin()
	{
		const Release& r = g_check.rel;
		homebrew = runsAsHomebrew(hbPath);
		asset = homebrew ? r.hb : r.cia;
		if (!asset.ok())
		{
			block(std::string("This release has no ") + (homebrew ? kHbAsset : kCiaAsset) + ".");
			return;
		}
		if (homebrew && hbPath.empty())
		{
			block("The Homebrew Launcher didn't say where mw3ds.3dsx is. Copy the new one to the SD card yourself.");
			return;
		}
		if (asset.sha256.size() != 64)
		{
			block("GitHub lists no SHA-256 for this release's files, so it can't be checked. Not installed.");
			return;
		}
		int have = dataVersionOf(dataDir);
		if (r.dataNeeded > have)
		{
			block("MW3DS " + r.version + " needs game data from a newer converter (data format "
				+ std::to_string(r.dataNeeded) + "; this console's is " + std::to_string(have) + "). Convert your "
				"Morrowind again with the MW3DS Converter " + r.version + " on your PC and install the CIA it makes.");
			return;
		}
		if (!netInit())
		{
			fail("The network couldn't be started.");
			return;
		}
		logf("update: downloading %s (%u bytes)", asset.url.c_str(), (unsigned)asset.size);
		fetch.start(asset.url, false);
		step = DOWNLOAD;
		message = "Downloading MW3DS " + r.version + "...";
	}

	void downloaded()
	{
		file = std::move(fetch.req.body);
		fetch.req.body.clear();
		if (asset.size && file.size() != asset.size)
		{
			fail("The download was " + std::to_string(file.size()) + " bytes, not " + std::to_string(asset.size)
				+ ". Nothing was changed.");
			return;
		}
		std::string got = sha256Hex(file);
		if (lowerText(got) != lowerText(asset.sha256))
		{
			logf("update: SHA-256 %s, the release lists %s", got.c_str(), asset.sha256.c_str());
			fail("The download's SHA-256 doesn't match the release's. Nothing was changed.");
			return;
		}
		logf("update: %u bytes, SHA-256 matches", (unsigned)file.size());
		if (homebrew)
		{
			installHomebrew();
			return;
		}
		if (dataInsideCia(dataDir))
		{
			step = COPY_LIST;
			message = "Moving the game data to the SD card (once: later updates are the program alone)...";
		}
		else
			startInstall();
	}

	// ---- the .3dsx: the new file beside the old, then swapped in
	void installHomebrew()
	{
		std::string fresh = hbPath + ".new", old = hbPath + ".old";
		FILE* f = fopen(fresh.c_str(), "wb");
		if (!f)
		{
			fail("Couldn't write " + fresh + ".");
			return;
		}
		bool ok = fwrite(file.data(), 1, file.size(), f) == file.size();
		ok &= fclose(f) == 0;
		if (!ok)
		{
			remove(fresh.c_str());
			fail("Couldn't write " + fresh + " (is the SD card full?). Nothing was changed.");
			return;
		}
		remove(old.c_str());
		if (rename(hbPath.c_str(), old.c_str()) != 0 || rename(fresh.c_str(), hbPath.c_str()) != 0)
		{
			rename(old.c_str(), hbPath.c_str());
			fail("Couldn't replace " + hbPath + ".");
			return;
		}
		remove(old.c_str());
		logf("update: %s replaced", hbPath.c_str());
		step = DONE;
		message = "MW3DS " + g_check.rel.version + " is installed. Start it again from the Homebrew Launcher.";
	}

	// ---- the data, out of the CIA onto the card
	void listData()
	{
		copyFrom = dataDir;
#ifndef __3DS__
		if (getenv("MW3DS_UPDATE_FAKE_ROMFS"))
			copyFrom = getenv("MW3DS_UPDATE_FAKE_ROMFS");
#endif
		copy = CopyJob();
		listTree(copyFrom, "", copy);
		logf("update: the data: %u files, %u folders, %s", (unsigned)copy.files.size(), (unsigned)copy.dirs.size(),
			mb(copy.total).c_str());
		unsigned long long free = sdFree(), need = copy.total + 16ull * 1048576;
		if (free < need)
		{
			fail("The SD card needs " + mb(need) + " free to hold the game data (it has " + mb((size_t)free)
				+ "). Nothing was changed.");
			return;
		}
		removeTree(kSdDataPart);
		mkdir(kSdDataPart, 0777);
		for (auto& d : copy.dirs)
			mkdir((std::string(kSdDataPart) + d).c_str(), 0777);
		copy.buf.resize(1 << 20);
		step = COPY;
	}

	// A frame's share of the copy (about 30 ms)
	void copyData()
	{
		u64 start = svcGetSystemTick();
		while ((svcGetSystemTick() - start) < SYSCLOCK_ARM11 / 33)
		{
			if (!copy.in)
			{
				if (copy.next >= copy.files.size())
				{
					finishCopy();
					return;
				}
				const std::string& rel = copy.files[copy.next].first;
				copy.in = fopen((copyFrom + rel).c_str(), "rb");
				copy.out = fopen((std::string(kSdDataPart) + rel).c_str(), "wb");
				if (!copy.in || !copy.out)
				{
					fail("Couldn't copy " + rel + " to the SD card. The game is as it was.");
					return;
				}
			}
			size_t n = fread(copy.buf.data(), 1, copy.buf.size(), copy.in);
			if (n && fwrite(copy.buf.data(), 1, n, copy.out) != n)
			{
				fail("Writing the game data to the SD card failed (is it full?). The game is as it was.");
				return;
			}
			copy.done += n;
			if (n < copy.buf.size())
			{
				fclose(copy.in);
				copy.in = nullptr;
				if (fclose(copy.out) != 0)
				{
					copy.out = nullptr;
					fail("Writing the game data to the SD card failed. The game is as it was.");
					return;
				}
				copy.out = nullptr;
				copy.next++;
			}
		}
	}

	void finishCopy()
	{
		copy.buf = std::vector<char>();
		// (a copy an older build left on the card goes: this one is the data the CIA carried)
		removeTree(kSdDataOld);
		struct stat st;
		if (stat(kSdData, &st) == 0 && rename(kSdData, kSdDataOld) != 0)
		{
			fail("Couldn't move the old sdmc:/3ds/mw3ds/data aside.");
			return;
		}
		if (rename(kSdDataPart, kSdData) != 0)
		{
			rename(kSdDataOld, kSdData);
			fail("Couldn't put the game data in place.");
			return;
		}
		removeTree(kSdDataOld);
		logf("update: the data is on the SD card (%s)", mb(copy.total).c_str());
		startInstall();
	}

	// ---- the program-only CIA, through the console's title installer
	void startInstall()
	{
		installed = 0;
#ifdef __3DS__
		if (R_FAILED(amInit()))
		{
			fail("The console's installer (AM) couldn't be opened.");
			return;
		}
		Result rc = AM_StartCiaInstall(MEDIATYPE_SD, &cia);
		if (R_FAILED(rc))
		{
			amExit();
			char b[96];
			snprintf(b, sizeof(b), "The console refused to start the install (0x%08lx).", (unsigned long)rc);
			fail(b);
			return;
		}
#endif
		step = INSTALL;
		message = "Installing MW3DS " + g_check.rel.version + "...";
	}

	void install()
	{
		size_t n = file.size() - installed < (256u << 10) ? file.size() - installed : (256u << 10);
#ifdef __3DS__
		u32 written = 0;
		Result rc = FSFILE_Write(cia, &written, installed, file.data() + installed, n, 0);
		if (R_FAILED(rc) || written != n)
		{
			AM_CancelCIAInstall(cia);
			amExit();
			char b[96];
			snprintf(b, sizeof(b), "The install failed (0x%08lx). The installed game is as it was.", (unsigned long)rc);
			fail(b);
			return;
		}
#endif
		installed += n;
		if (installed < file.size())
			return;
#ifdef __3DS__
		Result rc2 = AM_FinishCiaInstall(cia);
		amExit();
		if (R_FAILED(rc2))
		{
			char b[96];
			snprintf(b, sizeof(b), "The install couldn't be finished (0x%08lx).", (unsigned long)rc2);
			fail(b);
			return;
		}
		aptSetChainloaderToSelf();           // (the new program starts when this one quits)
#else
		// the PC test build: the CIA lands on the card, for the test to compare
		if (FILE* f = fopen("sdmc:/3ds/mw3ds/installed-program.cia", "wb"))
		{
			fwrite(file.data(), 1, file.size(), f);
			fclose(f);
		}
#endif
		logf("update: %s installed", g_check.rel.version.c_str());
		file = std::string();
		step = DONE;
		message = "MW3DS " + g_check.rel.version + " is installed. Press A to start it.";
	}

	void update()
	{
		switch (step)
		{
		case DOWNLOAD:
			fetch.update();
			if (fetch.failed)
				fail("The download failed: " + fetch.err + ". Nothing was changed.");
			else if (fetch.done)
				downloaded();
			break;
		case COPY_LIST: listData(); break;
		case COPY: copyData(); break;
		case INSTALL: install(); break;
		default: break;
		}
	}

	float progress() const
	{
		switch (step)
		{
		case DOWNLOAD: return asset.size ? fminf(1.0f, (float)fetch.req.received() / asset.size) : 0.0f;
		case COPY: return copy.total ? (float)copy.done / copy.total : 0.0f;
		case INSTALL: return file.empty() ? 1.0f : (float)installed / file.size();
		default: return -1.0f;
		}
	}
};
}

std::string selfUpdateScreen(C3D_RenderTarget* top, C3D_RenderTarget* bottom, const char* dataDir)
{
	Updater u;
	u.dataDir = dataDir;
	UiScroll scroll;
	bool wasTouching = false, dragged = false;
	int lastX = 0, lastY = 0, pressX = 0, pressY = 0;
	u64 lastTick = svcGetSystemTick();
	const Release& r = g_check.rel;
	std::string notes = r.notes.empty() ? "(no notes)" : r.notes;
	while (aptMainLoop())
	{
		audioUpdate();
		hidScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld();
		UiInput ui = {};
		touchPosition tp;
		hidTouchRead(&tp);
		if (held & KEY_TOUCH)
		{
			ui.touching = true;
			ui.touchX = tp.px;
			ui.touchY = tp.py;
			if (!wasTouching)
			{
				dragged = false;
				pressX = tp.px;
				pressY = tp.py;
			}
			else
			{
				ui.dragDX = tp.px - lastX;
				ui.dragDY = tp.py - lastY;
				if (abs(tp.px - pressX) + abs(tp.py - pressY) > kTapSlop)
					dragged = true;
			}
			lastX = tp.px;
			lastY = tp.py;
		}
		else if (wasTouching && !dragged)
		{
			ui.tapped = true;
			ui.tapX = lastX;
			ui.tapY = lastY;
		}
		wasTouching = held & KEY_TOUCH;
		ui.down = down;
		ui.held = held;
		u64 tick = svcGetSystemTick();
		ui.dt = fminf((float)(tick - lastTick) / SYSCLOCK_ARM11, 0.1f);
		lastTick = tick;
		uiBeginFrame(ui);
		u.update();
		if (g_answer == "yes" && u.step == ASK)
			u.begin();
		bool busy = u.step == DOWNLOAD || u.step == COPY_LIST || u.step == COPY || u.step == INSTALL;

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		linearRetire();
		C2D_Prepare();
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
		clearTop(top, 0x000000FF);
		C2D_SceneBegin(top);
		bool art = uiArt("art/menu_morrowind.t3x", 400, 240, 0, 0, 400, 240);
		uiRect(0, art ? 150 : 60, 400, 90, 0xB0000000);
		uiTextCentered(200, art ? 156 : 70, 0.6f, col::header, "MW3DS " + r.version);
		uiTextCentered(200, art ? 180 : 94, 0.45f, col::textDim, std::string("This is ") + ownVersion());
		float p = u.progress();
		if (p >= 0.0f)
			uiBar(60, art ? 206 : 120, 280, 10, p, col::header);

		C2D_TargetClear(bottom, C2D_Color32(10, 8, 6, 255));
		C2D_SceneBegin(bottom);
		uiRect(0, 0, 320, 20, col::panelLight);
		uiRect(0, 20, 320, 1, col::border);
		uiText(6, 2, 0.55f, col::header, "Update");
		std::string result = "\x01";
		if (u.step == ASK)
		{
			uiTextBox(scroll, 6, 26, 308, 140, "A new version of MW3DS is out. Only the program is downloaded; your "
				"saves, settings and game data stay.\n\n" + notes, 0.45f, true);
			if (uiButton(4, 174, 100, 28, "Never") || g_answer == "never")
			{
				if (FILE* f = fopen(kOffPath, "w"))
				{
					fputs("Delete this file to have MW3DS check for updates again.\n", f);
					fclose(f);
				}
				logf("update: checks turned off");
				g_check.offer.clear();
				result = "";
			}
			if (uiButton(4, 208, 154, 28, "Not now") || (down & KEY_B) || g_answer == "no")
				result = "";
			if (uiButton(162, 208, 154, 28, "Update", true) || (down & KEY_A))
				u.begin();
		}
		else if (busy)
		{
			uiTextBox(scroll, 6, 40, 308, 120, u.message, 0.5f, false);
			if (u.step == COPY)
				uiTextCentered(160, 170, 0.45f, col::textDim, mb(u.copy.done) + " of " + mb(u.copy.total));
			else if (u.step == DOWNLOAD)
				uiTextCentered(160, 170, 0.45f, col::textDim, mb(u.fetch.req.received()));
			uiTextCentered(160, 214, 0.45f, col::textDim, "Keep the console on until it is done.");
		}
		else
		{
			uiTextBox(scroll, 6, 40, 308, 150, u.message, 0.5f, false);
			if (u.step == DONE)
			{
				if (uiButton(84, 208, 152, 28, u.homebrew ? "Quit" : "Start it", true) || (down & KEY_A) || !g_answer.empty())
					result = "quit";
			}
			else if (uiButton(216, 208, 100, 28, "Back") || (down & (KEY_B | KEY_A)) || !g_answer.empty())
				result = "";
		}
		C3D_FrameEnd(0);
		if (result != "\x01")
		{
			if (!g_online && !busy)
				netExit();
			return result;
		}
	}
	return "quit";
}

bool selfUpdateTest(C3D_RenderTarget* top, C3D_RenderTarget* bottom, const char* dataDir)
{
	FILE* f = fopen("sdmc:/3ds/mw3ds/update_answer.txt", "r");
	if (!f)
		return false;
	char line[32] = "";
	if (!fgets(line, sizeof(line), f))
		line[0] = 0;
	fclose(f);
	g_answer = trimLine(line);
	if (g_answer.empty())
		g_answer = "no";
	logf("update test: answering %s", g_answer.c_str());
	selfUpdateCheck();
	u64 start = svcGetSystemTick();
	while ((g_check.state == CHECK_RELEASE || g_check.state == CHECK_INFO)
		&& svcGetSystemTick() - start < 30ull * SYSCLOCK_ARM11)
	{
		selfUpdatePoll();
		svcSleepThread(5000000);
	}
	if (g_check.offer.empty())
		logf("update test: nothing offered");
	else
		logf("update test: the screen said %s", selfUpdateScreen(top, bottom, dataDir) == "quit" ? "quit" : "back");
	return true;
}
