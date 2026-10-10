#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Standalone synthetic profile tests. Requires C compiler, GLib/GIO development
# files, pkg-config and Python 3. Never accesses a reader or private fixtures.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT HUP INT TERM
SRC="$ROOT/libfprint/drivers/goodix521d/chicago"
${CC:-cc} -shared -fPIC -O2 $(pkg-config --cflags gio-2.0 glib-2.0) \
  -I"$SRC" "$SRC/goodix-chicago-calibration.c" "$SRC/goodix-gf5298.c" \
  "$ROOT/tests/goodix521d-chicago-test-adapter.c" \
  $(pkg-config --libs gio-2.0 glib-2.0) -lm -o "$BUILD/profiles.so"
CHICAGO_TEST_LIBRARY="$BUILD/profiles.so" python3 "$ROOT/tests/goodix521d-chicago-profile.py"
