"""Builds the Android app (a Wear OS watch's; a phone's too): the game's own source, with the native test build's 3DS
stand-ins (tools/test/native/stub, native_stubs.cpp) and this build's display, input and sound (android/src), for
32-bit ARM (armeabi-v7a: a Galaxy Watch's Android is 32-bit only) with the Android NDK, over SDL3; then the APK with
Gradle (android/app).

    python android/build.py              # build/android/FractumMorrowind-debug.apk (rebuilds what changed)
    python android/build.py --release    # build/android/FractumMorrowind-android.apk, the release build (signed with
                                         # android/keystore.properties' key; MW3DS_VERSION: its version)
    python android/build.py --no-apk     # only the native libraries (build/android/jniLibs)
    MW3DS_ANDROID_ABI=x86_64 python android/build.py   # for the x86_64 emulator (Wear OS's has no 32-bit ARM); the
                                         # APK then carries both ABIs' libraries

Needs ANDROID_HOME (the SDK, with platform 35) and ANDROID_NDK_HOME (NDK 27), JDK 17, CMake, Ninja and git. SDL3, Mbed TLS
and zlib are downloaded into build/android once (pinned).
"""
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HERE = Path(__file__).parent
NATIVE = ROOT / "tools" / "test" / "native"
BUILD = ROOT / os.environ.get("MW3DS_ANDROID_BUILD", "build/android")
ABI = os.environ.get("MW3DS_ANDROID_ABI", "armeabi-v7a")
API = 28
TARGET = {"armeabi-v7a": f"--target=armv7a-linux-androideabi{API}", "x86_64": f"--target=x86_64-linux-android{API}"}.get(ABI) \
    or sys.exit(f"build.py: no build for the ABI {ABI} (armeabi-v7a, x86_64)")
# ARMv7 with NEON and VFPv4, which every armeabi-v7a device of Android 9 and later has; Thumb-2, the NDK's default
MARCH = ["-march=armv7-a", "-mfpu=neon-vfpv4", "-mfloat-abi=softfp", "-mthumb"] if ABI == "armeabi-v7a" else []
# (each other ABI's objects and SDL3 beside the 32-bit ARM ones)
SUFFIX = "" if ABI == "armeabi-v7a" else "-" + ABI
# SDL3 (as FractumSeraph/OpenCE's Android build pins it: tools/android_build.py)
SDL_TAG = "release-3.4.16"
SDL_COMMIT = "fa2c02bb6e21974a89ea9824bc53c9932abe5f9c"
SDL_URL = "https://github.com/libsdl-org/SDL.git"
SDL = BUILD / "SDL3"
SDL_BUILD = BUILD / ("sdl3-build" + SUFFIX)
# Mbed TLS for online play and the data download (mwmp/netsocket.cpp), the 3.6 long-term support release
# FractumSeraph/OpenCE vendors; the console's build has devkitPro's
MBEDTLS_VERSION = "3.6.7"
MBEDTLS_URL = f"https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-{MBEDTLS_VERSION}/mbedtls-{MBEDTLS_VERSION}.tar.bz2"
MBEDTLS_SHA256 = "a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6"
MBEDTLS = BUILD / "mbedtls"
ZLIB = BUILD / "zlib"
ZLIB_URL = "https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz"
ZLIB_SRC = ["adler32", "compress", "crc32", "deflate", "inffast", "inflate", "inftrees", "trees", "uncompr", "zutil"]
# what the native test build leaves out (tools/test/native/build.py SKIP): the console's entry, log, heaps and
# screenshots, which native_stubs.cpp and gles_gpu.cpp stand in for
SKIP = {"main.cpp", "components/debug/debuglog.cpp", "mwrender/screenshotmanager.cpp", "platform/linear.cpp",
        "platform/heaps.cpp"}
# The game as the native test build compiles it (no fused multiply-add: the 3DS rounds every operation), against the
# same stand-in headers, with the Android build's hooks in them (MW3DS_ANDROID)
COMMON = [TARGET, *MARCH, "-fPIC", "-O2", "-gline-tables-only", "-w", "-ffp-contract=off", "-fno-strict-aliasing",
          "-DMW3DS_ANDROID"]


def ndk_bin(name):
    ndk = os.environ.get("ANDROID_NDK_HOME") or sys.exit("build.py: set ANDROID_NDK_HOME to the NDK (27)")
    return str(Path(ndk) / "toolchains" / "llvm" / "prebuilt" / "linux-x86_64" / "bin" / name)


def cxx_flags():
    return [*COMMON, "-std=c++17", "-I", str(NATIVE / "stub"), "-I", str(NATIVE), "-I", str(ROOT / "source"),
            "-I", str(ZLIB), "-I", str(SDL / "include"), "-I", str(MBEDTLS / "include"), "-I", str(HERE / "src")]


def fetch():
    if not (SDL / "CMakeLists.txt").exists():
        subprocess.run(["git", "clone", "-q", "--depth", "1", "--branch", SDL_TAG, SDL_URL, str(SDL)], check=True)
    commit = subprocess.run(["git", "-C", str(SDL), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    if commit != SDL_COMMIT:
        sys.exit(f"build.py: SDL3 {SDL_TAG} is {commit}, not the pinned {SDL_COMMIT}")
    if not (MBEDTLS / "library").exists():
        data = urllib.request.urlopen(MBEDTLS_URL, timeout=120).read()
        got = hashlib.sha256(data).hexdigest()
        if got != MBEDTLS_SHA256:
            sys.exit(f"build.py: Mbed TLS {MBEDTLS_VERSION}'s download is {got}, not the pinned {MBEDTLS_SHA256}")
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:bz2") as t:
            for m in t.getmembers():
                parts = m.name.split("/", 1)
                if m.isfile() and len(parts) == 2 and parts[1].startswith(("library/", "include/")):
                    out = MBEDTLS / parts[1]
                    out.parent.mkdir(parents=True, exist_ok=True)
                    out.write_bytes(t.extractfile(m).read())
    if not (ZLIB / "zlib.h").exists():
        ZLIB.mkdir(parents=True, exist_ok=True)
        data = urllib.request.urlopen(ZLIB_URL, timeout=60).read()
        with tarfile.open(fileobj=io.BytesIO(data)) as t:
            for m in t.getmembers():
                name = m.name.split("/", 1)[-1]
                if m.isfile() and "/" not in name and name.endswith((".c", ".h")):
                    (ZLIB / name).write_bytes(t.extractfile(m).read())


def build_sdl():
    lib = SDL_BUILD / "libSDL3.so"
    if lib.exists():
        return lib
    ndk = Path(os.environ["ANDROID_NDK_HOME"])
    subprocess.run(["cmake", "-S", str(SDL), "-B", str(SDL_BUILD), "-G", "Ninja",
                    f"-DCMAKE_TOOLCHAIN_FILE={ndk / 'build' / 'cmake' / 'android.toolchain.cmake'}",
                    f"-DANDROID_ABI={ABI}", f"-DANDROID_PLATFORM=android-{API}", "-DCMAKE_BUILD_TYPE=Release",
                    "-DSDL_SHARED=ON", "-DSDL_STATIC=OFF", "-DSDL_TEST_LIBRARY=OFF", "-DSDL_TESTS=OFF",
                    "-DSDL_EXAMPLES=OFF"], check=True)
    subprocess.run(["cmake", "--build", str(SDL_BUILD), "--target", "SDL3-shared"], check=True)
    return lib


_newest = None


def newest_header():
    global _newest
    if _newest is None:
        _newest = max(p.stat().st_mtime for d in (ROOT / "source", NATIVE, HERE / "src") for p in d.rglob("*.h*"))
    return _newest


def compile_one(src, obj, kind):
    # (zlib and Mbed TLS depend only on their own files; the rest on any of the game's and the stand-ins' headers)
    own = kind == "mbedtls" or (kind == "c" and src.is_relative_to(ZLIB))
    if obj.exists() and obj.stat().st_mtime > (src.stat().st_mtime if own else max(src.stat().st_mtime, newest_header())):
        return None
    if kind == "c":
        cmd = [ndk_bin("clang"), *COMMON, "-I", str(ZLIB), "-I", str(ROOT / "source"), "-DNO_FSEEKO"]
    elif kind == "mbedtls":
        cmd = [ndk_bin("clang"), *COMMON, "-I", str(MBEDTLS / "include"), "-I", str(MBEDTLS / "library")]
    else:
        cmd = [ndk_bin("clang++"), *cxx_flags()]
        # (the game's files and the stand-ins take native_compat.h first, as in the native test build: sdmc:/ paths,
        # the POSIX bits, libctru's fonts; this build's own files are plain)
        if kind == "game":
            cmd += ["-include", str(NATIVE / "stub" / "native_compat.h")]
        # (the version, as the console's Makefile gives it: MW3DS_VERSION, a release's tag)
        if src.name == "selfupdate.cpp" and os.environ.get("MW3DS_VERSION"):
            cmd += [f'-DMW3DS_VERSION_TEXT="{os.environ["MW3DS_VERSION"]}"']
    r = subprocess.run([*cmd, "-c", str(src), "-o", str(obj)], capture_output=True, text=True)
    return None if r.returncode == 0 else f"{src}:\n{r.stderr[:4000]}"


def build_native():
    fetch()
    sdl = build_sdl()
    objdir = BUILD / ("obj" + SUFFIX)
    # (the version is compiled into the updater's file: a different one rebuilds it)
    stamp, version = BUILD / ("version" + SUFFIX + ".txt"), os.environ.get("MW3DS_VERSION", "")
    if not stamp.exists() or stamp.read_text() != version:
        (objdir / "game" / "mwmp" / "selfupdate.o").unlink(missing_ok=True)
        stamp.parent.mkdir(parents=True, exist_ok=True)
        stamp.write_text(version)
    jobs = []
    src = ROOT / "source"
    for s in sorted(src.rglob("*.c*")):
        rel = s.relative_to(src).as_posix()
        if s.suffix in (".cpp", ".c") and rel not in SKIP:
            jobs.append((s, objdir / "game" / s.relative_to(src).with_suffix(".o"), "game" if s.suffix == ".cpp" else "c"))
    jobs.append((NATIVE / "native_stubs.cpp", objdir / "native_stubs.o", "game"))
    for s in sorted((HERE / "src").glob("*.cpp")):
        jobs.append((s, objdir / "android" / s.with_suffix(".o").name, "android"))
    for z in ZLIB_SRC:
        jobs.append((ZLIB / (z + ".c"), objdir / ("z_" + z + ".o"), "c"))
    for m in sorted((MBEDTLS / "library").glob("*.c")):
        jobs.append((m, objdir / "mbedtls" / m.with_suffix(".o").name, "mbedtls"))
    for _, obj, _ in jobs:
        obj.parent.mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        errors = [e for e in pool.map(lambda j: compile_one(*j), jobs) if e]
    if errors:
        print("\n".join(errors))
        sys.exit(1)
    jni = BUILD / "jniLibs" / ABI
    jni.mkdir(parents=True, exist_ok=True)
    lib = jni / "libmain.so"
    rsp = BUILD / ("link" + SUFFIX + ".rsp")
    rsp.write_text("\n".join(str(o) for _, o, _ in jobs))
    r = subprocess.run([ndk_bin("clang++"), TARGET, *MARCH, "-shared", "-o", str(lib), f"@{rsp}", "-L", str(sdl.parent),
                        "-lSDL3", "-lGLESv3", "-lEGL", "-llog", "-landroid", "-static-libstdc++", "-Wl,--no-undefined",
                        "-Wl,-z,max-page-size=16384"], capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[:8000])
        sys.exit(1)
    shutil.copy2(sdl, jni / "libSDL3.so")
    print("built", lib)


def build_apk(release):
    gradle = [str(HERE / "gradlew.bat")] if os.name == "nt" else ["sh", str(HERE / "gradlew")]
    kind = "release" if release else "debug"
    subprocess.run([*gradle, "--console=plain", "-q", "assembleRelease" if release else "assembleDebug"], cwd=HERE,
                   check=True)
    outputs = BUILD / "gradle" / "app" / "outputs" / "apk" / kind
    apk = outputs / f"app-{kind}.apk"
    if release and not apk.exists():
        sys.exit("build.py: the release app is unsigned (app-release-unsigned.apk): android/keystore.properties names "
                 "no key")
    out = BUILD / ("FractumMorrowind-android.apk" if release else "FractumMorrowind-debug.apk")
    shutil.copy2(apk, out)
    print("built", out)


if __name__ == "__main__":
    build_native()
    if "--no-apk" not in sys.argv:
        build_apk("--release" in sys.argv)
