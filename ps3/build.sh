#!/bin/sh
# Builds spelunkr.pkg in a container that has the PS3 toolchain (ps3toolchain + PSL1GHT).
set -e
cd "$(dirname "$0")"
IMAGE="${PS3DEV_IMAGE:-flipacholas/ps3devextra:latest}"
docker run --rm -v "$PWD":/src -w /src -u "$(id -u):$(id -g)" "$IMAGE" sh -c 'make && make pkg'
ls -la spelunkr*.pkg
