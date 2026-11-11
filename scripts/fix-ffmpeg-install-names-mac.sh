#!/usr/bin/env bash
# Make the vendored ffmpeg Mach-Os relocatable AND leaf-name-consistent.
#
#   fix-ffmpeg-install-names-mac.sh [<ffmpeg-prefix>]   (default: external/ffmpeg)
#
# Called by build-external-ffmpeg-mac.sh after `make install`; idempotent,
# so it can also be re-run on an existing external/ffmpeg without the
# ~10 min rebuild.
#
# Invariant this script establishes (and --check verifies on any dir):
#
#   every load command that names one of our dylibs is
#   `@rpath/<file>` where <file> is the REAL versioned file
#   (libavutil.61.1.101.dylib) and that file's install name (LC_ID_DYLIB)
#   is the very same `@rpath/<file>`.
#
# Why: ffmpeg links its own libs/binaries against the major-version
# symlink name (libavutil.61.dylib). We used to ship that name as a
# second copy of the file whose install name said `.61.1.101.dylib`.
# macOS 27.2 dyld refuses that for hardened-runtime processes:
#   "found dylib at '…/libavutil.61.dylib' with install name
#    '@rpath/libavutil.61.1.101.dylib' and security level requires its
#    leaf name to match requested 'libavutil.61.dylib'"
# → UFB 1.4.1 died at launch (Library missing). One name per library,
# used by the file, its id, and every reference, satisfies every dyld.
#
#   fix-ffmpeg-install-names-mac.sh --check <dir> [<macho>...]
#
# Verifies instead of patching: every `@rpath/<leaf>` dependency of the
# Mach-Os in <dir> (plus any extra <macho> args) that resolves to a file
# in <dir> must have an install name with the same leaf. Exit 1 on any
# mismatch. Used as a release gate on UFB.app/Contents/Frameworks.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

install_leaf() {  # leaf of a dylib's LC_ID_DYLIB, empty if none
    local id
    id="$(otool -D "$1" 2>/dev/null | sed -n '2p')"
    [[ -n "$id" ]] && basename "$id"
    return 0
}

if [[ "${1:-}" == "--check" ]]; then
    dir="${2:?usage: --check <dir> [<macho>...]}"
    shift 2
    fail=0
    targets=()
    while IFS= read -r -d '' f; do targets+=("$f"); done \
        < <(find "$dir" -maxdepth 1 -type f -name '*.dylib' -print0)
    targets+=("$@")
    for t in "${targets[@]}"; do
        [[ -f "$t" ]] || continue
        while IFS= read -r dep; do
            leaf="${dep#@rpath/}"
            [[ "$leaf" == */* ]] && continue      # frameworks etc.
            [[ -e "$dir/$leaf" ]] || continue     # not one of ours
            got="$(install_leaf "$dir/$leaf")"
            if [[ "$got" != "$leaf" ]]; then
                echo "  FAIL $(basename "$t") wants @rpath/$leaf but that file's install name is '$got'" >&2
                fail=1
            fi
        done < <(otool -L "$t" 2>/dev/null | awk 'NR>1 && $1 ~ /^@rpath\// {print $1}')
    done
    exit "$fail"
fi

PREFIX="${1:-$SCRIPT_DIR/../external/ffmpeg}"
LIB_DIR="$PREFIX/lib"
[[ -d "$LIB_DIR" ]] || { echo "[ffmpeg-names] no $LIB_DIR" >&2; exit 1; }
LIB_DIR="$(cd "$LIB_DIR" && pwd -P)"

# Rewrite every dependency that names a file in LIB_DIR — whether still
# an absolute build-prefix path or an already-rewritten @rpath/<symlink
# name> — to @rpath/<real file name>. Driven off `otool -L` so we catch
# each reference exactly as the linker wrote it.
patch_macho() {
    local target="$1" dep leaf real
    [[ -f "$target" ]] || return 0
    while IFS= read -r dep; do
        leaf="$(basename "$dep")"
        [[ -e "$LIB_DIR/$leaf" ]] || continue
        case "$dep" in
            @rpath/"$leaf"|*/lib/"$leaf") ;;
            *) continue ;;
        esac
        real="$(basename "$(cd "$LIB_DIR" && realpath "$leaf")")"
        [[ "$dep" == "@rpath/$real" ]] && continue
        install_name_tool -change "$dep" "@rpath/$real" "$target" 2>/dev/null
    done < <(otool -L "$target" 2>/dev/null | awk 'NR>1 {print $1}')
}

echo "[ffmpeg-names] dylib ids + cross-deps → @rpath/<real name>"
for lib in "$LIB_DIR"/*.dylib; do
    [[ -L "$lib" ]] && continue
    install_name_tool -id "@rpath/$(basename "$lib")" "$lib" 2>/dev/null
    patch_macho "$lib"
done

# ffmpeg/ffprobe: same dependency rewrite, plus a single LC_RPATH of
# @executable_path/../Frameworks (they sit in UFB.app/Contents/MacOS/).
echo "[ffmpeg-names] binary load paths + rpath"
for bin in "$PREFIX/bin/ffmpeg" "$PREFIX/bin/ffprobe"; do
    [[ -x "$bin" ]] || continue
    patch_macho "$bin"
    while IFS= read -r old; do
        [[ -z "$old" ]] && continue
        install_name_tool -delete_rpath "$old" "$bin" 2>/dev/null || true
    done < <(otool -l "$bin" 2>/dev/null \
                | awk '/cmd LC_RPATH/{f=1; next} f && /path /{print $2; f=0}')
    install_name_tool -add_rpath "@executable_path/../Frameworks" "$bin"
done

# install_name_tool invalidates the linker's ad-hoc signature when it
# has to grow the load commands; arm64 won't load an unsigned dylib.
for f in "$LIB_DIR"/*.dylib "$PREFIX/bin/ffmpeg" "$PREFIX/bin/ffprobe"; do
    [[ -f "$f" && ! -L "$f" ]] || continue
    codesign --force --sign - "$f" 2>/dev/null || true
done

bins=()
for bin in "$PREFIX/bin/ffmpeg" "$PREFIX/bin/ffprobe"; do
    [[ -x "$bin" ]] && bins+=("$bin")
done
"${BASH_SOURCE[0]}" --check "$LIB_DIR" ${bins[@]+"${bins[@]}"}
echo "[ffmpeg-names] OK"
