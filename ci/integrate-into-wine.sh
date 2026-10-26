#!/bin/bash
# Drops DirectAudio into a clean (DirectAudio-unaware) Wine tree: by default a
# Wine 11 one (teldommm/proton-wine proton_11.0-2), or a Wine 9 / Proton 9 one
# with DA_WINE_MAJOR=9 (teldommm/wine proton-9.0-arm64ec). Run BEFORE autogen.sh
# so the configure.ac changes below are picked up by autoreconf.
#
#   [DA_WINE_MAJOR=9] ci/integrate-into-wine.sh <driver-repo-dir> <wine-tree-dir>
#
# Idempotent: re-running on an already integrated tree changes nothing.
set -euo pipefail

DRIVER=$(cd "$1" && pwd)
WINE=$(cd "$2" && pwd)
DST="$WINE/dlls/winedirectaudio.drv"
AC="$WINE/configure.ac"

# --- 1. driver sources ------------------------------------------------------
# DA_WINE_MAJOR picks the ABI flavour (default 11):
#   11  Wine 11: the .drv is a generic unixlib host, so the unix implementation
#       and its Makefile.in are the whole driver.
#   9   Wine 9 / Proton 9: mmdevapi still wants a PE module per driver, so the
#       wine9/ overlay (mmdevdrv.c, .spec, Makefile.in) goes in next to the same
#       directaudio.c, which is built with WINE_MMDEVAPI_NO_MIDI_GET_DRIVER.
mkdir -p "$DST"
case "${DA_WINE_MAJOR:-11}" in
    11)
        cp -v "$DRIVER/directaudio.c" "$DRIVER/Makefile.in" "$DST/"
        ;;
    9)
        cp -v "$DRIVER/directaudio.c" "$DST/"
        cp -v "$DRIVER/wine9/Makefile.in" "$DRIVER/wine9/mmdevdrv.c" \
              "$DRIVER/wine9/winedirectaudio.drv.spec" "$DST/"
        ;;
    *)
        echo "unsupported DA_WINE_MAJOR=${DA_WINE_MAJOR} (expected 9 or 11)" >&2
        exit 1
        ;;
esac

# --- 2. configure.ac --------------------------------------------------------
if grep -q 'dlls/winedirectaudio.drv' "$AC"; then
    echo "configure.ac already integrates DirectAudio"
    exit 0
fi

# The regex goes through ENVIRON, not awk -v: -v processes escapes, so gawk would
# turn \( into ( and reject the pattern.
# Insert $2 (a file) after the first line matching regex $1; fail if no match,
# so a moved anchor breaks the build loudly instead of silently skipping.
insert_after() {
    local pattern=$1 snippet=$2
    grep -qE "$pattern" "$AC" || { echo "anchor not found in configure.ac: $pattern" >&2; exit 1; }
    PAT=$pattern awk -v file="$snippet" '
        { print }
        !done && $0 ~ ENVIRON["PAT"] { while ((getline line < file) > 0) print line; done = 1 }
    ' "$AC" > "$AC.tmp" && mv "$AC.tmp" "$AC"
}
# Insert $2 before the first line matching regex $1.
insert_before() {
    local pattern=$1 snippet=$2
    grep -qE "$pattern" "$AC" || { echo "anchor not found in configure.ac: $pattern" >&2; exit 1; }
    PAT=$pattern awk -v file="$snippet" '
        !done && $0 ~ ENVIRON["PAT"] { while ((getline line < file) > 0) print line; done = 1 }
        { print }
    ' "$AC" > "$AC.tmp" && mv "$AC.tmp" "$AC"
}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# a. --with/--without-aaudio, next to the other sound backends.
cat > "$TMP/arg" <<'EOF'
AC_ARG_WITH(aaudio,    AS_HELP_STRING([--without-aaudio],[do not use the Android AAudio sound support]))
EOF
insert_after '^AC_ARG_WITH\(alsa,' "$TMP/arg"

# b. AAudio detection. No AAudio (non-Android host) = driver disabled.
cat > "$TMP/check" <<'EOF'
dnl **** Check for AAudio (Android native audio) for the DirectAudio driver ****
if test "x$with_aaudio" != "xno"
then
    AC_CHECK_HEADER([aaudio/AAudio.h],[AC_SUBST(AAUDIO_LIBS,"-laaudio")])
fi
test -n "$AAUDIO_LIBS" || enable_winedirectaudio_drv=${enable_winedirectaudio_drv:-no}

EOF
insert_before '^dnl \*\*\*\* Check for PulseAudio \*\*\*\*' "$TMP/check"

# c. Register the makefile, in alphabetical order (wined3d < winedirectaudio < winedmo).
cat > "$TMP/mk" <<'EOF'
WINE_CONFIG_MAKEFILE(dlls/winedirectaudio.drv)
EOF
insert_after '^WINE_CONFIG_MAKEFILE\(dlls/wined3d\)' "$TMP/mk"

echo "configure.ac: DirectAudio integrated"
grep -nE 'aaudio|AAUDIO|winedirectaudio' "$AC"
