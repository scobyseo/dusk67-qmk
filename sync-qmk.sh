#!/bin/bash
# Copy the keyboard from this repo into the pinned QMK submodule and verify.
#
# QMK only discovers keyboards inside its own tree, and a submodule checkout
# wipes anything placed there, so this has to be re-run after `git submodule
# update`. It is the *only* mechanical step between the repo and a build.
set -euo pipefail
REPO="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
SRC="$REPO/keyboards/ydkb/unicore_f1"
DST="$REPO/qmk_firmware/keyboards/ydkb/unicore_f1"

[ -d "$REPO/qmk_firmware/.git" ] || [ -f "$REPO/qmk_firmware/.git" ] || {
    echo "error: $REPO/qmk_firmware is not a git checkout; run:" >&2
    echo "  git submodule update --init --recursive" >&2
    exit 1
}

mkdir -p "$(dirname "$DST")"
rm -rf "$DST"
cp -r "$SRC" "$DST"

# Prove the copy is faithful -- a silent partial copy would build the wrong
# firmware rather than fail.
if diff -rq "$SRC" "$DST" >/dev/null; then
    echo "synced: keyboards/ydkb/unicore_f1 -> qmk_firmware/keyboards/ydkb/unicore_f1"
else
    echo "error: copy differs from source" >&2
    diff -rq "$SRC" "$DST" >&2
    exit 1
fi

echo "pinned QMK: $(git -C "$REPO/qmk_firmware" rev-parse --short HEAD)"
echo
echo "build with:"
echo "  export PATH=~/work/toolchain-arm-gnu-13.2/bin:\$PATH"
echo "  cd $REPO/qmk_firmware && make ydkb/unicore_f1:dusk67_via"