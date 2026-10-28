#pragma once

// The self-updater (FractumSeraph/mw3ds). At the title screen, when the console is on a network, GitHub is asked for
// the fork's latest release; a newer one is offered there. Only the program comes down, never Morrowind's data:
// FractumMorrowind.cia (the CIA's program alone) for an installed copy, FractumMorrowind.3dsx for the Homebrew Launcher's. Each
// file is checked against the size and SHA-256 GitHub lists for it (its "digest") before anything is replaced.
//
// The CIA's update replaces the whole installed title, data and all, so a CIA that carries its data inside (the
// converter's) first has that data copied out to sdmc:/3ds/mw3ds/data, where the program-only CIA reads it; from then
// on an update is the program alone. The release's update.json says which data format it needs
// (components/files/dataversion.hpp): newer than the console's, the data's server is asked for it
// (mwmp/datadownload.hpp): there, the update installs and the new program downloads its data at its start; not there,
// the update is not installed and the screen says to convert again.
//
// sdmc:/3ds/mw3ds/update_off turns the automatic check off (the screen's Never); sdmc:/3ds/mw3ds/update_url.txt
// points it at another release (a test server's: the GitHub API's JSON for one release).
#include <citro2d.h>
#include <string>

void selfUpdateCheck();                  // starts the check (once a run; not when off or without Wi-Fi)
void selfUpdatePoll();                   // moves the check on: each title frame
const std::string& selfUpdateOffer();    // the newer release's version, once found ("" none)
void selfUpdateStop();                   // the game starts: a check still waiting is given up
// The update's own screen: "" back to the title, "quit" once the new program is installed (it starts next)
std::string selfUpdateScreen(C3D_RenderTarget* top, C3D_RenderTarget* bottom, const char* dataDir);
// Test hook: sdmc:/3ds/mw3ds/update_answer.txt ("yes", "no", "never") checks at the start and answers the screen;
// true when it ran (the program then quits)
bool selfUpdateTest(C3D_RenderTarget* top, C3D_RenderTarget* bottom, const char* dataDir);

// (shared with the data download) whether the console is on Wi-Fi; a folder and all in it removed; the SD card's free
// bytes (a large number when the console can't say)
bool onWifi();
void removeTree(const std::string& path);
unsigned long long sdFree();
