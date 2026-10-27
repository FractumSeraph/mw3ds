#pragma once

// The converted data's format (FractumSeraph/mw3ds): the converter writes it to data_version.txt in the data folder
// (tools/convert/level.py reads this line), the program says which it needs. Raise it when the program needs data a
// converter older than this commit can't make; the self-updater (mwmp/selfupdate.cpp) won't install a release whose
// update.json asks for newer data than the console has, since the game would then run on data it can't read.
//   0  before the stamp
//   1  each reference's RefNum ("rn", the online mode's shared objects), the werewolf's first-person arms
static const int kDataVersion = 1;

// The data folder's data_version.txt (0 when it has none)
int dataVersionOf(const char* dataDir);
