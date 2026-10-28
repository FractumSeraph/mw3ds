#pragma once

// The game data's download (FractumSeraph/mw3ds). The program-only CIA (and a .3dsx) carries none of Morrowind; when
// the console has no data of this program's format (no romfs:/data, no sdmc:/3ds/mw3ds/data, or older data there), a
// screen at the start offers to fetch it from the server that hosts it beside openmw-web's own files:
// <base>/data-v<format>/index.json (tools/release/make_datapacks.py) names packs of about 32 MB, each checked by its
// SHA-256 and unpacked into sdmc:/3ds/mw3ds/data.part as it streams in. The packs done are noted there, so a download
// cut off (no Wi-Fi, the console shut) carries on from the pack it was in. Once every pack is in, data.part becomes
// sdmc:/3ds/mw3ds/data (older data there is replaced; saves are kept apart, in sdmc:/3ds/mw3ds/saves).
//
// sdmc:/3ds/mw3ds/data_url.txt: another index.json's address (a test server's); data_answer.txt ("yes"/"no") answers
// the screen for a scripted run.
#include <citro2d.h>
#include <string>

// What the console lacks: "" nothing (dataDir's data is this program's), else why the data must come down
std::string dataDownloadNeed(const char* dataDir);
// The index.json the console asks for data of this format
std::string dataIndexUrl(int format);
// The screen: true once the data is in sdmc:/3ds/mw3ds/data, false when the player quits
bool dataDownloadScreen(C3D_RenderTarget* top, C3D_RenderTarget* bottom, const std::string& need, bool isNew3ds);
