#!/bin/sh
# Builds spelunkr.pkg in a container that has the PS3 toolchain (ps3toolchain + PSL1GHT).
# The whole repo is mounted because the app's shared code lives in ../core.
set -e
cd "$(dirname "$0")/.."
IMAGE="${PS3DEV_IMAGE:-flipacholas/ps3devextra:latest}"
docker run --rm -v "$PWD":/src -w /src/ps3 -u "$(id -u):$(id -g)" "$IMAGE" sh -c 'make && make pkg'
ls -la ps3/spelunkr*.pkg
