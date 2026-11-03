# The Android build (Wear OS watches)

FractumMorrowind on Android: the same game as the 3DS build (`source/`), for a Wear OS watch first (Samsung's Galaxy
Watch 8 Classic: a round 438 x 438 display, a turning bezel, and a 32-bit-only Android), and phones too. It plays the
converted game data the 3DS build plays, from the watch's storage.

It is the native test build (`tools/test/native`) made to play: the game's own source compiled against the same
stand-in 3DS headers (`tools/test/native/stub`, with the hooks this build needs inside `#ifdef MW3DS_ANDROID`) and
`native_stubs.cpp`'s files, log and memory, with what that build leaves out made real here (`android/src`):

| File | What it is |
| --- | --- |
| `main_android.cpp` | The entry: the hooks of `source/platform/platform.hpp`, then `appMain` (SDL3 calls it from its activity) |
| `gles_gpu.cpp` | citro3d and citro2d on OpenGL ES 3.0: render targets as framebuffers, the `.t3x` textures uploaded (ETC1 as it is, the rest decoded by `tools/test/native/pica_textures.hpp`), `shaders/cell.v.pica` and `skin.v.pica` in GLSL, the texture combiners as a fragment shader made for each set of stages, citro2d's drawing batched; at a frame's end the screen the controls choose, put on the display |
| `android_input.cpp` | The window and its context, the paths, and the console's buttons, pads and touch screen from the watch's touch screen, bezel and back button (and a gamepad): one finger's gestures, the ring of actions; the software keyboard as the system's |
| `android_audio.cpp` | The ndsp channels mixed and played through SDL (the native test build's WAV mixer, in real time) |
| `build.py` | The build: SDL3, Mbed TLS (online play and the data download: `mwmp/netsocket.cpp`'s TLS, as the console's) and zlib, the game, the APK |
| `app/` | The app: SDL3's activity with a round display, the bezel, the back button, a text dialog, the buzz and the system font drawn with the watch's typeface (`MorrowindActivity.java`); the updater |

## Playing on a watch

The display shows one of the console's screens at a time, as large as fits in its circle: the top screen (the world)
while the game plays, the bottom one (the touch screen: the title, the HUD's buttons, the menus, messages) otherwise. A
menu opening or closing picks again; the ring's **SCREEN** and the bottom screen's **WORLD** swap them.

The watch is on one hand, so one finger plays, and every press buzzes:

| On the top screen | |
| --- | --- |
| Slide | Walk, a stick where the finger landed (pushed far: run) |
| Tap | Weapon away: A (talk, open, take). Weapon out: a quick blow |
| Hold still | Weapon out: a blow wound up while held (a red ring under the finger, a buzz once it's wound up fully), struck when let go. Weapon away: nothing |
| The bezel | Turn, 15 degrees a click |
| **WALK** | Walk on by itself, the bezel steering, until it is pressed again, a menu opens or a fight begins |
| **MORE**, or the back button | The ring of actions |

| The ring | |
| --- | --- |
| **MENU** | START: the menu (save, rest, options) |
| **JUMP**, **READY**, **CAST** | Jump; ready or put away the weapon; cast the selected spell |
| **SNEAK** | Sneak on and off ("SNEAK" shows while on) |
| **VIEW** | First or third person |
| **LOOK** | A slide looks instead of walking ("LOOK" shows while on) |
| **SCREEN** | The bottom screen: the HUD's inventory, magic, map and stats buttons |

A tap outside the ring's actions, or the back button, closes it. On a phone a second finger's slide looks while the
first walks.

### Combat

The weapon decides what a touch does, so that no blow is struck by accident: put away, nothing a finger does is a blow.
The game's **Combat aids** (Options > Prefs > Combat aids, on by default on Android, off on the 3DS) do the rest:

| Combat aid | |
| --- | --- |
| Ready by itself | The weapon comes out (a buzz) when someone starts fighting you, and goes away 5 s after the last of them stops; put away by hand, it stays away for that fight |
| Safe strikes | A first blow at someone who isn't fighting you (peaceful, or fighting someone else) is held back: two buzzes and "Attack Fargoth? Strike again to attack." A new blow at them within 4 s lands. Melee only |
| Aim help | With the weapon out, the view turns towards the nearest foe in front of you (within 70 degrees and three reaches), 90 degrees a second at most |

Being hit buzzes long.

### Frame rate

Options > Video > **Frame rate** (in place of the 3DS's depth buffer choice, which only its 3D screen uses): 60, or 30,
which keeps a watch cooler and its battery going longer.

| On the bottom screen | |
| --- | --- |
| A finger | The stylus |
| **BACK**, **MENU**, **OK** | The console's B, START and A |
| The bezel | The menus' highlight up and down (a list's row, an item grid's row; **OK** chooses it) and a text pane's scroll |
| The back button | B |
| **1** **2** **3** **4** under Inventory and Magic | Make the highlighted item or spell that quick key (the 3DS's ZL + D-pad); the HUD's slots use them |

The game's hints and its tutorial name these controls on Android, not the 3DS's buttons.

The actions press what the button layout chosen in Options has them on (Xbox by default). A paired gamepad is the
console's buttons where Nintendo puts them (A on the right), its sticks the Circle Pad and the C-stick.

## Installing

Each release from FractumMorrowind 0.4 on carries the app, `FractumMorrowind-android.apk`, signed with FractumSeraph's
key. Install it with wireless debugging on (Settings > Developer options):

```sh
adb install FractumMorrowind-android.apk
```

A development build (`FractumMorrowind-debug.apk`, signed with the machine's own key) does not install over a release's,
nor a release's over it: uninstall the other first (`adb uninstall net.fractumseraph.morrowind`), which deletes its game
data and saves.

At its first start, with Wi-Fi on, the game downloads its data as the 3DS build does (`mwmp/datadownload.cpp`):
Morrowind, Tribunal and Bloodmoon converted for the New 3DS, 1.4 GB in 42 parts from halomaps.fractumseraph.net, a few
minutes on a watch's Wi-Fi. The app keeps the screen on meanwhile, so the watch keeps its Wi-Fi. A download that fails
(the network dropped while the watch's charging screen covered the game) tries again by itself every 15 s, 20 times,
and carries on where it stopped. The data goes into the
app's folder, `Android/data/net.fractumseraph.morrowind/files/3ds/mw3ds/data`, as `3ds/mw3ds/data` on a 3DS's SD card;
saves, settings and `log.txt` are beside it in `.../files/3ds/mw3ds`. The game's log also goes to
`adb logcat -s mw3ds`. Uninstalling the app deletes that folder, data and saves too.

Copying the data with adb instead works too, but a watch drops wireless debugging during a long copy. Let the app make
its folder first (start it once): one adb makes belongs to the shell, and the game can't write in it.

## Updates

A release's app looks for a newer release when the game starts, as the 3DS build does (`mwmp/selfupdate.cpp`: the
same releases, GitHub's latest). When there is one, it asks: **Update** downloads the new app, checks it against the
size and SHA-256 GitHub lists for it and hands it to Android's package installer, which asks to confirm (the first time,
also to allow FractumMorrowind to install apps), replaces the game and starts it again; the game data and saves stay.
**Not now** asks again at the next start, **Never** makes `3ds/mw3ds/update_off`, the 3DS build's switch too (delete it
to be asked again). `Updater.java` and `UpdateReceiver.java` are the updater; the game's own updater, which would fetch
the 3DS's program, is off on Android.

The release workflow (`.github/workflows/release.yml`, the android job) builds the app from the release's tag
(`mp-0.4.0`: version 0.4.0, versionCode 400) and signs it with the repository's secrets `ANDROID_KEYSTORE_BASE64`,
`ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS` and `ANDROID_KEY_PASSWORD`, the same key as FractumSeraph/OpenCE's apps.
Without them it releases no app: an unsigned one can't be installed, and one signed with any other key can't update.

## Building

```sh
python android/build.py              # build/android/FractumMorrowind-debug.apk
python android/build.py --release    # build/android/FractumMorrowind-android.apk, the release build
python android/build.py --no-apk     # only the native libraries (build/android/jniLibs)
MW3DS_ANDROID_ABI=x86_64 python android/build.py   # also for the x86_64 emulator
```

The Wear OS emulator is x86_64 with no 32-bit ARM, so the 32-bit build won't run there: `MW3DS_ANDROID_ABI=x86_64` builds
the game for it too (beside the ARM libraries, in their own `obj-x86_64` and `sdl3-build-x86_64`), and the APK then carries
both. Releases carry the 32-bit ARM build alone.

The release build is signed with the key `android/keystore.properties` names (storeFile, storePassword, keyAlias,
keyPassword; gitignored), as the release workflow writes it; without one it stops. `MW3DS_VERSION` is its version
(0.0.0-dev otherwise) and `MW3DS_RELEASE_BUILD=1` turns its updater on.

A development build is an app of its own, `net.fractumseraph.morrowind.dev` ("Morrowind dev"), so it installs beside a
release's without touching it (with its own game data). It needs `ANDROID_HOME` (the SDK, platform 35), `ANDROID_NDK_HOME` (NDK 27), JDK 17, CMake, Ninja and git, on Linux;
it downloads SDL3, Mbed TLS and zlib (pinned) into `build/android` once. The APK is signed with the machine's debug key.

## What it does not do yet

- The streaming thread: cells load on the game's thread, as the native test build has it (a pause at a cell's edge).
- Movies, gyro aim, the 3D slider, screenshots.
