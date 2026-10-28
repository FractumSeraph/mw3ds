// The game data's download (FractumSeraph/mw3ds): see mwmp/datadownload.hpp. Driven a frame at a time like the
// self-updater (HttpRequest with a sink: a pack is unpacked as it streams in, never held whole).
#include "mwmp/datadownload.hpp"

#include <3ds.h>
#include <citro2d.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "components/debug/debuglog.hpp"
#include "components/files/dataversion.hpp"
#include "extern/cjson/cJSON.h"
#include "mwgui/loadingscreen.hpp"
#include "mwgui/widgets.hpp"
#include "mwinput/inputmanagerimp.hpp"
#include "mwmp/fetch.hpp"
#include "mwmp/netsocket.hpp"
#include "mwmp/online.hpp"
#include "mwmp/selfupdate.hpp"
#include "mwmp/sha256.hpp"
#include "platform/linear.hpp"

namespace
{
const char* const kBase = "https://halomaps.fractumseraph.net/mw3ds/";
const char* const kUrlPath = "sdmc:/3ds/mw3ds/data_url.txt";
const char* const kAnswerPath = "sdmc:/3ds/mw3ds/data_answer.txt";
const char* const kSdData = "sdmc:/3ds/mw3ds/data";
const char* const kSdDataPart = "sdmc:/3ds/mw3ds/data.part";
const char* const kSdDataOld = "sdmc:/3ds/mw3ds/data.old";
const char* const kProgress = "sdmc:/3ds/mw3ds/data.part/.download";
const int kTries = 3;                    // a pack's downloads before the screen says it failed

std::string firstLine(const char* path)
{
	FILE* f = fopen(path, "r");
	if (!f)
		return "";
	char line[512] = "";
	if (!fgets(line, sizeof(line), f))
		line[0] = 0;
	fclose(f);
	std::string s = line;
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
		s.pop_back();
	return s;
}

std::string mb(double bytes)
{
	char b[32];
	snprintf(b, sizeof(b), bytes >= 1024.0 * 1048576 ? "%.2f GB" : "%.0f MB",
		bytes >= 1024.0 * 1048576 ? bytes / (1024.0 * 1048576) : bytes / 1048576);
	return b;
}

struct Pack
{
	std::string file, sha256;
	size_t size = 0, bytes = 0;
};

struct Index
{
	int format = 0, data = 0, maxTex = 0, files = 0;
	std::string name, sha256;            // (sha256: of index.json itself, the progress file's first line)
	double bytes = 0;
	std::vector<Pack> packs;
};

bool readIndex(const std::string& text, Index& x, std::string& err)
{
	cJSON* j = cJSON_Parse(text.c_str());
	if (!j)
	{
		err = "The server's index.json could not be read.";
		return false;
	}
	auto num = [&](const char* k) { const cJSON* v = cJSON_GetObjectItem(j, k); return cJSON_IsNumber(v) ? v->valuedouble : 0.0; };
	x.format = (int)num("format");
	x.data = (int)num("data");
	x.maxTex = (int)num("maxTex");
	x.files = (int)num("files");
	x.bytes = num("bytes");
	const cJSON* name = cJSON_GetObjectItem(j, "name");
	x.name = cJSON_IsString(name) ? name->valuestring : "Morrowind";
	const cJSON* p;
	cJSON_ArrayForEach(p, cJSON_GetObjectItem(j, "packs"))
	{
		const cJSON* file = cJSON_GetObjectItem(p, "file");
		const cJSON* sha = cJSON_GetObjectItem(p, "sha256");
		const cJSON* size = cJSON_GetObjectItem(p, "size");
		const cJSON* bytes = cJSON_GetObjectItem(p, "bytes");
		if (!cJSON_IsString(file) || !cJSON_IsString(sha) || strlen(sha->valuestring) != 64 || !cJSON_IsNumber(size)
			|| strchr(file->valuestring, '/') || strstr(file->valuestring, ".."))
			continue;
		Pack k;
		k.file = file->valuestring;
		k.sha256 = sha->valuestring;
		k.size = (size_t)size->valuedouble;
		k.bytes = cJSON_IsNumber(bytes) ? (size_t)bytes->valuedouble : k.size;
		x.packs.push_back(k);
	}
	cJSON_Delete(j);
	if (x.format != 1 || x.packs.empty())
	{
		err = x.format != 1 ? "The server's data is packed in a way this version of FractumMorrowind can't read (pack format "
			+ std::to_string(x.format) + "). Update FractumMorrowind." : "The server's index.json lists no packs.";
		return false;
	}
	Sha256 s;
	s.add(text.data(), text.size());
	x.sha256 = s.hex();
	return true;
}

// A path a pack may write: relative, "/"-separated, nothing climbing out
bool safePath(const std::string& p)
{
	if (p.empty() || p[0] == '/' || p.find('\\') != std::string::npos || p.find(':') != std::string::npos)
		return false;
	size_t at = 0;
	while (at <= p.size())
	{
		size_t end = p.find('/', at);
		std::string seg = p.substr(at, end == std::string::npos ? std::string::npos : end - at);
		if (seg.empty() || seg == "." || seg == "..")
			return false;
		if (end == std::string::npos)
			break;
		at = end + 1;
	}
	return true;
}

// A pack as it streams in (tools/release/make_datapacks.py): "MWPK", u32 1, then per file u16 path length, the path,
// u32 size, the bytes; u16 0 ends it. Each file is written under root as its bytes arrive.
struct Unpacker
{
	enum Part { MAGIC, NAMELEN, NAME, SIZE, BODY, END };
	std::string root, acc, name, err;
	Part part = MAGIC;
	size_t want = 8, left = 0;
	FILE* out = nullptr;
	std::set<std::string> made;
	Sha256 sha;
	int files = 0;
	static char s_buf[64 << 10];

	void reset(const std::string& r)
	{
		close();
		root = r;
		acc.clear();
		err.clear();
		part = MAGIC;
		want = 8;
		left = 0;
		sha = Sha256();
		files = 0;
	}
	void close()
	{
		if (out)
			fclose(out);
		out = nullptr;
	}
	bool fail(const std::string& why)
	{
		err = why;
		close();
		return false;
	}
	static uint32_t le(const std::string& s, int n)
	{
		uint32_t v = 0;
		for (int i = n - 1; i >= 0; i--)
			v = v << 8 | (uint8_t)s[i];
		return v;
	}
	bool open()
	{
		for (size_t i = name.find('/'); i != std::string::npos; i = name.find('/', i + 1))
		{
			std::string dir = name.substr(0, i);
			if (made.insert(dir).second)
				mkdir((root + "/" + dir).c_str(), 0777);
		}
		out = fopen((root + "/" + name).c_str(), "wb");
		if (!out)
			return fail("Couldn't write " + name + " on the SD card.");
		setvbuf(out, s_buf, _IOFBF, sizeof(s_buf));
		return true;
	}
	bool fileDone()
	{
		FILE* f = out;
		out = nullptr;
		if (fclose(f) != 0)
			return fail("Writing " + name + " to the SD card failed (is it full?).");
		files++;
		part = NAMELEN;
		want = 2;
		return true;
	}
	bool feed(const char* p, size_t n)
	{
		sha.add(p, n);
		while (n)
		{
			if (part == BODY)
			{
				size_t take = n < left ? n : left;
				if (fwrite(p, 1, take, out) != take)
					return fail("Writing " + name + " to the SD card failed (is it full?).");
				p += take;
				n -= take;
				left -= take;
				if (!left && !fileDone())
					return false;
				continue;
			}
			if (part == END)
				return fail("The pack goes on past its end.");
			size_t take = want - acc.size() < n ? want - acc.size() : n;
			acc.append(p, take);
			p += take;
			n -= take;
			if (acc.size() < want)
				break;
			switch (part)
			{
			case MAGIC:
				if (acc.compare(0, 4, "MWPK") != 0 || le(acc.substr(4), 4) != 1)
					return fail("Not one of MW3DS's data packs.");
				part = NAMELEN;
				want = 2;
				break;
			case NAMELEN:
				want = le(acc, 2);
				part = want ? NAME : END;
				break;
			case NAME:
				name = acc;
				if (!safePath(name))
					return fail("The pack names a file outside the data folder.");
				part = SIZE;
				want = 4;
				break;
			case SIZE:
				left = le(acc, 4);
				if (!open())
					return false;
				if (!left && !fileDone())
					return false;
				if (left)
					part = BODY;
				break;
			default:
				break;
			}
			acc.clear();
		}
		return true;
	}
};
char Unpacker::s_buf[64 << 10];

enum Step { NO_WIFI, INDEX, ASK, BLOCKED, DOWNLOAD, PLACE, DONE, FAILED };

struct Downloader
{
	Step step = NO_WIFI;
	std::string need, message, indexUrl, base;
	bool isNew3ds = true, hasOld = false;  // hasOld: older data the player may go on with ("Not now")
	Index idx;
	Fetch fetch;
	Unpacker unp;
	size_t next = 0, doneBytes = 0;       // the first pack not yet in, the bytes of those that are
	int tries = 0;
	u64 startTick = 0;
	size_t startBytes = 0;

	void fail(const std::string& why)
	{
		logf("data: failed: %s", why.c_str());
		fetch.req.cancel();
		unp.close();
		message = why;
		step = FAILED;
	}

	void begin()
	{
		if (!onWifi())
		{
			step = NO_WIFI;
			return;
		}
		if (!netInit())
		{
			fail("The network couldn't be started.");
			return;
		}
		indexUrl = dataIndexUrl(kDataVersion);
		base = indexUrl.substr(0, indexUrl.rfind('/') + 1);
		logf("data: asking %s", indexUrl.c_str());
		fetch.req.sink = nullptr;
		fetch.start(indexUrl, false);
		step = INDEX;
	}

	void indexIn()
	{
		std::string err;
		idx = Index();
		if (!readIndex(fetch.req.body, idx, err))
		{
			fail(err);
			return;
		}
		logf("data: %s, format %d, %d files, %.0f bytes in %u packs", idx.name.c_str(), idx.data, idx.files, idx.bytes,
			(unsigned)idx.packs.size());
		if (idx.data != kDataVersion)
		{
			message = "The server has game data of format " + std::to_string(idx.data) + ", and this version of FractumMorrowind needs format "
				+ std::to_string(kDataVersion) + ".";
			step = BLOCKED;
			return;
		}
		if (!isNew3ds && idx.maxTex > kOld3dsMaxTex)
		{
			message = "The server's game data is made for a New 3DS (textures up to " + std::to_string(idx.maxTex)
				+ " texels; an Old 3DS takes " + std::to_string(kOld3dsMaxTex) + "). Convert Morrowind for an Old 3DS "
				"with the MW3DS Converter on a PC instead.";
			step = BLOCKED;
			return;
		}
		// What an earlier start already brought in, when it was this same index
		next = doneBytes = 0;
		FILE* f = fopen(kProgress, "r");
		if (f)
		{
			char line[80];
			bool same = fgets(line, sizeof(line), f) && !strncmp(line, idx.sha256.c_str(), 64);
			while (same && next < idx.packs.size() && fgets(line, sizeof(line), f))
			{
				if (strncmp(line, idx.packs[next].sha256.c_str(), 64) != 0)
					break;
				doneBytes += idx.packs[next].bytes;
				next++;
			}
			fclose(f);
			if (!same)
				next = doneBytes = 0;
		}
		if (next)
			logf("data: %u of %u packs already here", (unsigned)next, (unsigned)idx.packs.size());
		step = ASK;
	}

	void start()
	{
		double left = idx.bytes - doneBytes;
		unsigned long long free = sdFree(), want = (unsigned long long)left + 16ull * 1048576;
		if (free < want)
		{
			fail("The SD card needs " + mb((double)want) + " free for the game data, and it has " + mb((double)free)
				+ ". Make room (or use a bigger card), then try again.");
			return;
		}
		if (!next)
		{
			// a fresh start: nothing of another index's packs kept
			removeTree(kSdDataPart);
			mkdir(kSdDataPart, 0777);
			FILE* f = fopen(kProgress, "w");
			if (!f)
			{
				fail("Couldn't write to the SD card (sdmc:/3ds/mw3ds).");
				return;
			}
			fprintf(f, "%s\n", idx.sha256.c_str());
			fclose(f);
		}
		tries = 0;
		startTick = svcGetSystemTick();
		startBytes = doneBytes;
		step = DOWNLOAD;
		packStart();
	}

	void packStart()
	{
		if (next >= idx.packs.size())
		{
			step = PLACE;
			message = "Putting the game data in place...";
			return;
		}
		const Pack& k = idx.packs[next];
		unp.reset(kSdDataPart);
		fetch.req.sink = [this](const char* d, size_t n) { return unp.feed(d, n); };
		fetch.start(base + k.file, false);
		if (fetch.failed)
			packFailed(fetch.err);
	}

	void packFailed(const std::string& why)
	{
		unp.close();
		logf("data: pack %s: %s (try %d of %d)", idx.packs[next].file.c_str(), why.c_str(), tries + 1, kTries);
		if (++tries < kTries)
		{
			packStart();
			return;
		}
		fail("The download stopped: " + why + ". Try again to carry on from where it stopped.");
	}

	void packDone()
	{
		const Pack& k = idx.packs[next];
		if (unp.part != Unpacker::END || unp.sha.total != k.size)
		{
			packFailed("pack " + k.file + " came incomplete");
			return;
		}
		std::string got = unp.sha.hex();             // (which pads it: total no longer the pack's)
		if (got != k.sha256)
		{
			logf("data: pack %s SHA-256 %s, the index lists %s", k.file.c_str(), got.c_str(), k.sha256.c_str());
			packFailed("pack " + k.file + " doesn't match its SHA-256");
			return;
		}
		FILE* f = fopen(kProgress, "a");
		bool noted = f && fprintf(f, "%s\n", k.sha256.c_str()) > 0;
		if (f)
			noted &= fclose(f) == 0;
		if (!noted)
		{
			fail("Couldn't write to the SD card (is it full?).");
			return;
		}
		doneBytes += k.bytes;
		next++;
		tries = 0;
		if (next % 8 == 0 || next == idx.packs.size())
			logf("data: %u of %u packs in (%s)", (unsigned)next, (unsigned)idx.packs.size(), mb(doneBytes).c_str());
		packStart();
	}

	// The new data in, older data out
	void place()
	{
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
		remove((std::string(kSdData) + "/.download").c_str());   // (the progress file, come along with the rest)
		removeTree(kSdDataOld);
		logf("data: in place, %d files (format %d)", idx.files, dataVersionOf(kSdData));
		step = DONE;
	}

	void stop()
	{
		fetch.req.cancel();
		unp.close();
		logf("data: stopped at pack %u of %u", (unsigned)next, (unsigned)idx.packs.size());
		step = ASK;
	}

	// A frame's share of the work: the network read over and over for up to ~14 ms
	void update()
	{
		if (step == INDEX)
		{
			fetch.update();
			if (fetch.failed)
				fail("The server couldn't be reached (" + fetch.err + "): " + indexUrl);
			else if (fetch.done)
				indexIn();
			return;
		}
		if (step == PLACE)
		{
			place();
			return;
		}
		if (step != DOWNLOAD)
			return;
		u64 t0 = svcGetSystemTick();
		while (step == DOWNLOAD && svcGetSystemTick() - t0 < SYSCLOCK_ARM11 / 70)
		{
			fetch.update();
			if (fetch.failed)
				packFailed(unp.err.empty() ? fetch.err : unp.err);
			else if (fetch.done)
				packDone();
			else
				svcSleepThread(500000);
		}
	}

	double received() const
	{
		return (double)doneBytes + (step == DOWNLOAD ? (double)fetch.req.received() * (next < idx.packs.size()
			? (double)idx.packs[next].bytes / (idx.packs[next].size ? idx.packs[next].size : 1) : 1.0) : 0.0);
	}
};
}

std::string dataIndexUrl(int format)
{
	// (data_url.txt's address for this format: its "data-v<n>" made this format's)
	std::string url = firstLine(kUrlPath);
	size_t at = url.find("data-v");
	if (at == std::string::npos)
		return url.empty() ? std::string(kBase) + "data-v" + std::to_string(format) + "/index.json" : url;
	size_t end = at + 6;
	while (end < url.size() && isdigit((unsigned char)url[end]))
		end++;
	return url.substr(0, at + 6) + std::to_string(format) + url.substr(end);
}

std::string dataDownloadNeed(const char* dataDir)
{
	struct stat st;
	if (stat((std::string(dataDir) + "/game.json").c_str(), &st) != 0)
		return "This console has no game data yet.";
	int have = dataVersionOf(dataDir);
	if (strncmp(dataDir, "romfs:", 6) != 0 && have < kDataVersion)
		return "The game data on the SD card is format " + std::to_string(have) + ", older than this MW3DS's ("
			+ std::to_string(kDataVersion) + "): online, other players' things and people won't line up with yours.";
	return "";
}

bool dataDownloadScreen(C3D_RenderTarget* top, C3D_RenderTarget* bottom, const std::string& need, bool isNew3ds)
{
	Downloader d;
	d.need = need;
	d.isNew3ds = isNew3ds;
	struct stat st;
	d.hasOld = stat((std::string(kSdData) + "/game.json").c_str(), &st) == 0;
	std::string answer = firstLine(kAnswerPath);
	if (!answer.empty())
		logf("data test: answering %s", answer.c_str());
	logf("data: %s", need.c_str());
	d.begin();
	UiScroll scroll;
	bool wasTouching = false, dragged = false;
	int lastX = 0, lastY = 0, pressX = 0, pressY = 0;
	u64 lastTick = svcGetSystemTick();
	int result = -1;                      // 1 the data is in, 0 quit, 2 on with the old data
	while (result < 0 && aptMainLoop())
	{
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
		d.update();
		if (d.step == DONE)
			result = 1;
		if (answer == "yes" && d.step == ASK)
			d.start();

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		linearRetire();
		C2D_Prepare();
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
		clearTop(top, 0x000000FF);
		C2D_SceneBegin(top);
		uiTextCentered(200, 60, 0.9f, col::header, "FractumMorrowind");
		uiTextCentered(200, 96, 0.5f, col::textDim, "Morrowind's game data");
		double total = d.idx.bytes > 0 ? d.idx.bytes : 1.0;
		if (d.step == DOWNLOAD || d.step == PLACE || (d.step == ASK && d.next))
		{
			double got = d.received();
			uiBar(60, 140, 280, 12, (float)fmin(1.0, got / total), col::header);
			uiTextCentered(200, 160, 0.5f, col::text, mb(got) + " of " + mb(d.idx.bytes));
			double secs = (double)(svcGetSystemTick() - d.startTick) / SYSCLOCK_ARM11;
			if (d.step == DOWNLOAD && secs > 5 && got > d.startBytes)
			{
				double rate = (got - d.startBytes) / secs;
				int left = (int)((d.idx.bytes - got) / rate);
				char b[96];
				snprintf(b, sizeof(b), "%.2f MB/s, about %d min left", rate / 1048576, left / 60 + 1);
				uiTextCentered(200, 184, 0.45f, col::textDim, b);
			}
		}

		C2D_TargetClear(bottom, C2D_Color32(10, 8, 6, 255));
		C2D_SceneBegin(bottom);
		uiRect(0, 0, 320, 20, col::panelLight);
		uiRect(0, 20, 320, 1, col::border);
		uiText(6, 2, 0.55f, col::header, "Game data");
		switch (d.step)
		{
		case NO_WIFI:
			uiTextBox(scroll, 6, 26, 308, 140, d.need + "\n\nFractumMorrowind downloads it over Wi-Fi, and the console isn't on a "
				"network. Turn Wi-Fi on (or move nearer the router), then try again.\n\nOr install the CIA the FractumMorrowind "
				"Converter makes from your own Morrowind on a PC: it carries the data inside.", 0.45f, true);
			if (uiButton(4, 208, 100, 28, d.hasOld ? "Not now" : "Quit") || (down & KEY_B) || answer == "no")
				result = d.hasOld ? 2 : 0;
			if (uiButton(162, 208, 154, 28, "Try again", true) || (down & KEY_A))
				d.begin();
			break;
		case INDEX:
			uiTextBox(scroll, 6, 40, 308, 120, "Asking the server what there is...\n\n" + d.indexUrl, 0.45f, false);
			break;
		case ASK:
		{
			std::string text = d.need + "\n\nDownload " + d.idx.name + ", converted for the 3DS: " + mb(d.idx.bytes)
				+ " (" + std::to_string(d.idx.files) + " files), from " + d.base + ".";
			if (d.next)
				text += " " + mb(d.doneBytes) + " of it is here already: it carries on from there.";
			text += "\n\nThe SD card has " + mb((double)sdFree()) + " free. It takes a while: keep the console open and "
				"charging. Your saves stay as they are.";
			uiTextBox(scroll, 6, 26, 308, 176, text, 0.45f, true);
			if (uiButton(4, 208, 100, 28, d.hasOld ? "Not now" : "Quit") || (down & KEY_B) || answer == "no")
				result = d.hasOld ? 2 : 0;
			if (uiButton(162, 208, 154, 28, d.next ? "Carry on" : "Download", true) || (down & KEY_A))
				d.start();
			break;
		}
		case DOWNLOAD:
		case PLACE:
		{
			std::string text = d.step == PLACE ? d.message : "Downloading part " + std::to_string(d.next + 1) + " of "
				+ std::to_string(d.idx.packs.size()) + "...";
			uiTextBox(scroll, 6, 40, 308, 120, text, 0.5f, false);
			uiTextCentered(160, 170, 0.45f, col::textDim, "Keep the console open until it is done.");
			if (d.step == DOWNLOAD && (uiButton(216, 208, 100, 28, "Stop") || (down & KEY_B)))
				d.stop();
			break;
		}
		case DONE:
			break;
		case BLOCKED:
		case FAILED:
		default:
			uiTextBox(scroll, 6, 26, 308, 176, d.message, 0.45f, false);
			if (uiButton(4, 208, 100, 28, d.hasOld ? "Not now" : "Quit") || (down & KEY_B) || !answer.empty())
				result = d.hasOld ? 2 : 0;
			if (d.step == FAILED && (uiButton(162, 208, 154, 28, "Try again", true) || (down & KEY_A)))
				d.begin();
			break;
		}
		C3D_FrameEnd(0);
	}
	d.fetch.req.cancel();
	d.unp.close();
	if (!g_online)
		netExit();
	logf("data: the screen %s", result == 1 ? "has the data in place" : result == 2 ? "goes on with the old data" : "quits");
	return result >= 1;
}
