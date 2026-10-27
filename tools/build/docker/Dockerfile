# The whole mw3ds toolchain in one Linux image, for building without MSYS2 (FractumSeraph/mw3ds):
# devkitPro's devkitARM, libctru, citro2d / citro3d, tex3ds and picasso, makerom, plus Python with the converter's
# packages (requirements.txt). Build:  docker build -t mw3ds-build -f tools/build/docker/Dockerfile .
# Use:   sh tools/build/docker/run.sh <command>   (the repo at /work, your Morrowind folder at /morrowind, read only)
FROM devkitpro/devkitarm:latest
RUN apt-get update && apt-get install -y --no-install-recommends python3-pip python3-venv git ca-certificates \
    && rm -rf /var/lib/apt/lists/*
# makerom for the CIA, built from Project_CTR's tagged source (its prebuilt Linux one needs a newer glibc than this
# Debian 12 image has); tools/build/setup.py fetches the same release prebuilt on Windows
RUN apt-get update && apt-get install -y --no-install-recommends build-essential && rm -rf /var/lib/apt/lists/* \
    && git clone --depth 1 -b makerom-v0.19.0 https://github.com/3DSGuy/Project_CTR /tmp/ctr \
    && make -C /tmp/ctr/makerom deps && make -C /tmp/ctr/makerom -j4 \
    && mkdir -p /opt/ctr \
    && cp "$(find /tmp/ctr/makerom -type f -name makerom -perm -u+x | head -1)" /opt/ctr/makerom \
    && rm -rf /tmp/ctr
ENV MW3DS_CTR_TOOLS_DIR=/opt/ctr
COPY requirements.txt /tmp/requirements.txt
RUN python3 -m venv /opt/venv && /opt/venv/bin/pip install --no-cache-dir -r /tmp/requirements.txt
ENV PATH="/opt/venv/bin:${PATH}"
