# The whole mw3ds toolchain in one Linux image, for building without MSYS2 (FractumSeraph/mw3ds):
# devkitPro's devkitARM, libctru, citro2d / citro3d, tex3ds and picasso, plus Python with the converter's
# packages (requirements.txt). Build:  docker build -t mw3ds-build -f tools/build/docker/Dockerfile .
# Use:   sh tools/build/docker/run.sh <command>   (the repo at /work, your Morrowind folder at /morrowind, read only)
FROM devkitpro/devkitarm:latest
RUN apt-get update && apt-get install -y --no-install-recommends python3-pip python3-venv git \
    && rm -rf /var/lib/apt/lists/*
COPY requirements.txt /tmp/requirements.txt
RUN python3 -m venv /opt/venv && /opt/venv/bin/pip install --no-cache-dir -r /tmp/requirements.txt
ENV PATH="/opt/venv/bin:${PATH}"
