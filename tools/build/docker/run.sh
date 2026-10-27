#!/bin/sh
# Runs a command in the mw3ds-build image (tools/build/docker/Dockerfile), building the image the first time.
# The repo is mounted at /work and MW3DS_MORROWIND_DIR (from the environment or .env) read-only at /morrowind;
# inside, MW3DS_MORROWIND_DIR=/morrowind.   sh tools/build/docker/run.sh python3 tools/build/make.py
set -e
cd "$(dirname "$0")/../../.."
if ! docker image inspect mw3ds-build >/dev/null 2>&1; then
	docker build -t mw3ds-build -f tools/build/docker/Dockerfile .
fi
MW=${MW3DS_MORROWIND_DIR:-$(sed -n 's/^MW3DS_MORROWIND_DIR=//p' .env 2>/dev/null | tr -d '\r')}
REPO=$(pwd -W 2>/dev/null || pwd)
MSYS_NO_PATHCONV=1 docker run --rm ${MW3DS_DOCKER_FLAGS} -v "$REPO:/work" ${MW:+-v "$MW:/morrowind:ro"} \
	-e MW3DS_MORROWIND_DIR=/morrowind -e HOME=/tmp -w /work mw3ds-build "$@"
