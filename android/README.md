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
| `android_input.cpp` | The window and its context, the paths, and the console's buttons, pads and touch screen from the watch's touch screen, bezel and back button (and a gamepad); the software keyboard as the system's |
| `android_audio.cpp` | The ndsp channels mixed and played through SDL (the native test build's WAV mixer, in real time) |
| `build.py` | The build: SDL3, Mbed TLS (online play and the data download: `mwmp/netsocket.cpp`'s TLS, as the console's) and zlib, the game, the APK |
| `app/` | The app: SDL3's activity with a round display, the bezel, the back button, a text dialog and the system font drawn with the watch's typeface (`MorrowindActivity.java`) |

## Playing on a watch

The display shows one of the console's screens at a time, as large as fits in its circle: the top screen (the world)
while the game plays, the bottom one (the touch screen: the title, the HUD's buttons, the menus) otherwise. A menu
opening or closing picks again; the **v** / **^** button swaps them.

| On the top screen | |
| --- | --- |
| A finger on the left half | The Circle Pad: a stick where it lands (pushed far: run) |
| A finger on the right half | Slide: turn and look, as far as it slides (Options' stylus speed). Tap: A (talk, open, take) |
| **=** | START: the menu (save, rest, options) |
| **X** / **Y** | Ready or put away the weapon / jump |
| **L** / **R** | Cast / attack (held: wind up) |
| **S** / **P** | Sneak on and off / first or third person |
| The bezel | Turn, 15 degrees a click |
| The back button | START |

| On the bottom screen | |
| --- | --- |
| A finger | The stylus |
| **B**, **=**, **A** | The console's B, START and A |
| The bezel | The D-pad's up and down |
| The back button | B |

The buttons press what the button layout chosen in Options has those actions on (Xbox by default). A paired gamepad is
the console's buttons where Nintendo puts them (A on the right), its sticks the Circle Pad and the C-stick.

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
minutes on a watch's Wi-Fi. The app keeps the screen on meanwhile, so the watch keeps its Wi-Fi. The data goes into the
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
```

The release build is signed with the key `android/keystore.properties` names (storeFile, storePassword, keyAlias,
keyPassword; gitignored), as the release workflow writes it; without one it stops. `MW3DS_VERSION` is its version
(0.0.0-dev otherwise) and `MW3DS_RELEASE_BUILD=1` turns its updater on.

It needs `ANDROID_HOME` (the SDK, platform 35), `ANDROID_NDK_HOME` (NDK 27), JDK 17, CMake, Ninja and git, on Linux;
it downloads SDL3, Mbed TLS and zlib (pinned) into `build/android` once. The APK is signed with the machine's debug key.

## What it does not do yet

- The streaming thread: cells load on the game's thread, as the native test build has it (a pause at a cell's edge).
- Movies, gyro aim, the 3D slider, screenshots.
