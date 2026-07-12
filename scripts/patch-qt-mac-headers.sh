#!/usr/bin/env bash
# patch-qt-mac-headers.sh
#
# Applies the upstream Qt fix for QTBUG-145239 to a local Qt 6.11.0
# install on macOS, so cxx-qt's C++ TUs compile against Apple Clang
# (Xcode 16+ / clang 21+) without the
#   "implicitly declaring library function '__yield'" error.
#
# Background: Qt 6.11.0's `qyieldcpu.h` does
#     #if __has_builtin(__yield)
#         __yield();
# Apple Clang answers true to `__has_builtin(__yield)` because
# `__yield` is an inline in `<arm_acle.h>`, but Qt's header doesn't
# include that file, so the call site references an undeclared symbol.
# Modern Clang on macOS 26 promotes that to a hard error during cxx-qt's
# C++ build.
#
# The upstream fix (in Qt 6.11.1, scheduled May 8 2026, and Qt's `dev`
# branch) reorders the preprocessor chain so `__has_builtin(__builtin_arm_yield)`
# (a real Clang/GCC builtin that needs no header) is checked BEFORE
# the broken `__yield` path. Apple Silicon hits the working branch first
# and emits the YIELD instruction directly. No header includes, no flag
# pollution.
#
# This script applies the same reorder to the local Qt install. Idempotent:
#   • If the file is already patched (marker comment present), exit 0.
#   • If the file doesn't match the broken pattern (Qt 6.11.1+ with the
#     upstream fix already shipped), exit 0.
#   • Otherwise apply the patch and add the marker.
#
# Run via `just mac-setup-external` after a fresh Qt install.

set -euo pipefail

MARKER="UFB-PATCHED-QTBUG-145239"

resolve_qt_root() {
    # 1. $QT_MAC override (set by `just`).
    if [[ -n "${QT_MAC:-}" && -d "$QT_MAC/lib/QtCore.framework/Headers" ]]; then
        echo "$QT_MAC"; return 0
    fi
    # 2. $QMAKE → up two levels.
    if [[ -n "${QMAKE:-}" && -x "$QMAKE" ]]; then
        local root
        root="$(cd "$(dirname "$QMAKE")/.." && pwd)"
        if [[ -d "$root/lib/QtCore.framework/Headers" ]]; then
            echo "$root"; return 0
        fi
    fi
    # 3. Standard locations.
    for candidate in \
        "$HOME/Qt/6.11.0/macos" \
        "$HOME/Qt/6.11.1/macos" \
        "$HOME/Qt/6.12.0/macos" \
        "/Applications/Qt/6.11.0/macos" \
        "/opt/homebrew/opt/qt/lib/qt"*; do
        if [[ -d "$candidate/lib/QtCore.framework/Headers" ]]; then
            echo "$candidate"; return 0
        fi
    done
    return 1
}

QT_ROOT="$(resolve_qt_root)" || {
    echo "[patch-qt] could not locate a Qt 6.11+ macos install."
    echo "[patch-qt] set QT_MAC or QMAKE, or install Qt under ~/Qt/<version>/macos/"
    exit 1
}

HEADER="$QT_ROOT/lib/QtCore.framework/Headers/qyieldcpu.h"

if [[ ! -f "$HEADER" ]]; then
    echo "[patch-qt] expected $HEADER but it doesn't exist"
    exit 1
fi

# Idempotence check #1: already patched by us.
if grep -q "$MARKER" "$HEADER"; then
    echo "[patch-qt] $HEADER already patched — skipping"
    exit 0
fi

# Idempotence check #2: file doesn't have the broken pattern (e.g. Qt
# 6.11.1+ shipped the upstream fix). The broken pattern is the literal
# `#if __has_builtin(__yield)` at the start of the qYieldCpu() body.
# The upstream-fixed file leads with `#if __has_builtin(__builtin_arm_yield)`
# instead.
if ! grep -q '^#if __has_builtin(__yield)' "$HEADER"; then
    echo "[patch-qt] $HEADER doesn't match the broken Qt 6.11.0 pattern"
    echo "[patch-qt] (probably Qt 6.11.1+ with the upstream fix already) — skipping"
    exit 0
fi

# Apply the patch. We use Python for the multiline regex — sed gymnastics
# would be fragile on the edge cases.
python3 - "$HEADER" "$MARKER" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
marker = sys.argv[2]
src = path.read_text()

# Sanity: confirm the broken pattern is present once.
if src.count("#if __has_builtin(__yield)") != 1:
    sys.exit(f"[patch-qt] unexpected occurrence count of the broken pattern in {path}")

# Build the replacement. We:
#   1. Replace the first chain entry so __has_builtin(__builtin_arm_yield)
#      is checked first (matches Apple Silicon without needing arm_acle.h).
#   2. Demote the original __builtin_arm_yield block (now redundant) to
#      a comment so the diff is obvious in any future inspection.
#   3. Insert a marker comment for idempotence.

old_head = "#if __has_builtin(__yield)\n    __yield();              // Generic\n"
new_head = (
    f"// {marker}: reordered chain so __builtin_arm_yield (no-header builtin) is\n"
    f"// checked before __yield (Apple Clang answers __has_builtin(__yield) true\n"
    f"// without including arm_acle.h, breaking the call site). Upstream fix in\n"
    f"// Qt 6.11.1 / dev. Re-running scripts/patch-qt-mac-headers.sh is a no-op\n"
    f"// once Qt 6.11.1+ ships the same change.\n"
    f"#if __has_builtin(__builtin_arm_yield)\n"
    f"    __builtin_arm_yield();\n"
    f"#elif __has_builtin(__yield)\n"
    f"    __yield();              // Generic\n"
)

old_arm_block = (
    "\n#elif __has_builtin(__builtin_arm_yield)\n"
    "    __builtin_arm_yield();"
)

if old_head not in src:
    sys.exit("[patch-qt] couldn't locate the head replacement anchor")
if old_arm_block not in src:
    sys.exit("[patch-qt] couldn't locate the arm_yield block to remove")

patched = src.replace(old_head, new_head, 1)
patched = patched.replace(old_arm_block, "", 1)

# Write atomically: temp file in same dir, then rename.
tmp = path.with_suffix(path.suffix + ".ufb-tmp")
tmp.write_text(patched)
tmp.replace(path)
print(f"[patch-qt] patched {path}")
PY

echo "[patch-qt] done."
