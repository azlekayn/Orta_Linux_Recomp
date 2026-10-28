#!/usr/bin/env bash
# orta_package.sh -- assemble a runnable folder: the executable, settings.ini,
# engine.ini, README.txt and the game's files. POSIX sibling of
# orta_package.ps1.
#
#   bash orta_package.sh [output-dir]
#
# Default output is ../"Panzer Dragoon Orta PC".
set -u

ORTA="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLKIT="$(cd "$ORTA/../xboxrecomp-main" && pwd)"
PKG="${1:-$(cd "$ORTA/.." && pwd)/Panzer Dragoon Orta PC}"
REPORT="$ORTA/package_report.txt"

{
    echo "started $(date -Is)"
    echo '=== Building the release executable ==='
    cmake -S "$ORTA" -B "$ORTA/build64" -DCMAKE_BUILD_TYPE=Release || exit 1
    if ! cmake --build "$ORTA/build64" -j; then
        echo 'BUILD FAILED'
        exit 1
    fi

    echo '=== Assembling the package ==='
    mkdir -p "$PKG"
    cp -f "$ORTA/build64/orta_recomp" "$PKG/Panzer Dragoon Orta"
    for f in engine.ini settings.ini README.txt; do
        cp -f "$ORTA/package/$f" "$PKG/$f"
    done

    echo '=== Copying the game files (a few minutes) ==='
    mkdir -p "$PKG/game_files"
    # Everything except the user's saves and the recompiler's seed JSON.
    ( cd "$TOOLKIT/game_files" && \
      find . -type d \( -name UDATA -o -name TDATA \) -prune -o \
             -type f ! -name '*.json' -print0 | \
      tar --null -cf - -T - ) | ( cd "$PKG/game_files" && tar -xf - )

    n=$(find "$PKG" -type f | wc -l)
    gb=$(du -sh "$PKG" | cut -f1)
    echo "OK: $n files, $gb in $PKG"
    [ -f "$PKG/game_files/default.xbe" ] && echo 'default.xbe present'
    echo "finished $(date -Is)"
} | tee "$REPORT"
