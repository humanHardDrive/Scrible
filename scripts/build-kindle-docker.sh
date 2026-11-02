#!/usr/bin/env bash
# Runs scripts/build-kindle.sh inside a Linux container with the Kindle toolchain.
# Works on macOS (Docker Desktop, OrbStack, colima) and Linux.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE=xournal-scribe-kindle

docker build --platform linux/amd64 -t "$IMAGE" -f "$ROOT/docker/kindle.Dockerfile" "$ROOT/docker"
docker run --rm --platform linux/amd64 -v "$ROOT:/src" -w /src "$IMAGE" scripts/build-kindle.sh
