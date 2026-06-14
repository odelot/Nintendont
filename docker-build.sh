#!/bin/bash
#
# docker-build.sh - Build Nintendont (incl. the RA kernel module) using a
# pinned devkitPro Docker image.
#
# WHY DOCKER / WHY PINNED:
#   This Nintendont source needs an OLDER toolchain. Building on a current
#   (gcc 14+) devkitPro fails for two reasons:
#     1. gcc 14 promotes -Wincompatible-pointer-types / -Wimplicit-function-
#        declaration to ERRORS (multidol/apploader.c etc).
#     2. Recent libogc removed the internal symbols the PPC loader uses
#        (__lwp_thread_closeall, __lwp_thread_stopmultitasking,
#        ogc/lwp_threads.h) -> link/compile errors, and the Nintendont README
#        explicitly warns "compiling on latest dkARM/dkPPC/libOGC ... will
#        crash when attempting to return to Nintendont menu".
#
#   devkitpro/devkitppc:20230417 bundles devkitARM r60 + devkitPPC r42
#   (both gcc 12.2.0) + libogc 2.4.0, which still ships lwp_threads.h and the
#   __lwp_* symbols. It builds this source cleanly with ZERO source hacks, so
#   the loader stays bit-identical to upstream and the only diff vs stock is
#   our kernel/ra_module.* addition (good for a control-vs-RA comparison).
#
# USAGE:
#   ./docker-build.sh          # incremental build  -> nintendont/boot.dol
#   ./docker-build.sh clean    # clean + full rebuild
#
set -e

IMAGE=devkitpro/devkitppc:20230417

cd "$(dirname "${BASH_SOURCE[0]}")"
# Windows path for the Docker bind mount when run from Git Bash; falls back to
# the POSIX path (WSL / Linux).
MOUNT="$(pwd -W 2>/dev/null || pwd)"

command -v docker >/dev/null 2>&1 || { echo "ERROR: docker not found in PATH"; exit 1; }
docker info >/dev/null 2>&1     || { echo "ERROR: docker daemon not running"; exit 1; }

# Pull the image if missing. Docker Hub's CDN (cloudfront) can TLS-timeout on
# the large blobs; completed layers are cached, so retries converge.
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	ok=0
	for i in $(seq 1 12); do
		echo "Pulling $IMAGE (attempt $i)..."
		if docker pull "$IMAGE"; then ok=1; break; fi
		sleep 3
	done
	[ "$ok" = 1 ] || { echo "ERROR: could not pull $IMAGE after retries"; exit 1; }
fi

if [ "$1" = "clean" ]; then
	BUILD_CMD="make forced"
else
	BUILD_CMD="make"
fi

echo "============================================="
echo "Building Nintendont in $IMAGE"
echo "  mount : $MOUNT -> /build"
echo "  cmd   : $BUILD_CMD"
echo "============================================="

MSYS_NO_PATHCONV=1 docker run --rm -v "$MOUNT:/build" -w /build "$IMAGE" bash -c "$BUILD_CMD"

echo ""
echo "Done. Deployable artifact: nintendont/boot.dol"
echo "Copy it to your SD card at /apps/Nintendont/boot.dol"
