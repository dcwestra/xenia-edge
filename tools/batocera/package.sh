#!/bin/sh
# Packs the portable Linux build (cmake -DXENIA_PORTABLE_LINUX=ON into
# build-batocera/) and the Batocera integration into the archive
# tools/batocera/install.sh installs, for a GitHub release of this fork:
#
#   tools/batocera/package.sh [output directory, default build-batocera/release]
#
# Writes xenia-edge-batocera-x86_64.tar.xz, its .sha256 and a copy of
# install.sh as install-batocera.sh. The binary must have been built from the
# commit that's checked out, with no uncommitted changes to the sources, so
# the release holds exactly what that commit has.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=$ROOT/build-batocera
BIN=$BUILD/bin/Linux/Release/xenia_edge
OUT=${1:-$BUILD/release}
NAME=xenia-edge-batocera-x86_64

if [ ! -x "$BIN" ]; then
  echo "No portable build at $BIN" >&2
  exit 1
fi

head=$(git -C "$ROOT" rev-parse HEAD)
built=$(sed -n 's/^#define XE_BUILD_COMMIT "\(.*\)"$/\1/p' "$BUILD/version.h")
branch=$(sed -n 's/^#define XE_BUILD_BRANCH "\(.*\)"$/\1/p' "$BUILD/version.h")
if [ "$built" != "$head" ]; then
  echo "$BUILD/version.h names commit $built, but $head is checked out" >&2
  exit 1
fi
if ! git -C "$ROOT" diff --quiet HEAD -- src third_party cmake CMakeLists.txt \
     assets; then
  echo "The sources have uncommitted changes, which the binary may hold" >&2
  exit 1
fi
if [ "$(find "$ROOT/build/data_repos/game-patches/patches" -name '*.toml' \
        2>/dev/null | wc -l)" -eq 0 ]; then
  echo "No game patches to bundle; run ./xb fetchdata and rebuild" >&2
  exit 1
fi

# Batocera 43 has glibc 2.40 and libstdc++ GLIBCXX_3.4.32.
max_glibc=$(objdump -T "$BIN" | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -uV | tail -1)
case "$max_glibc" in
  GLIBC_2.[0-9]|GLIBC_2.[0-3][0-9]|GLIBC_2.40) ;;
  *) echo "$BIN needs $max_glibc, newer than Batocera's glibc 2.40" >&2; exit 1 ;;
esac
if objdump -p "$BIN" | grep -qE 'NEEDED +libstdc\+\+'; then
  echo "$BIN links libstdc++ dynamically; build with XENIA_PORTABLE_LINUX" >&2
  exit 1
fi

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
P=$STAGE/$NAME
mkdir -p "$P" "$OUT"
# Debug info is most of the binary's size; the unstripped one stays in the
# build directory for symbolizing crash reports.
objcopy --strip-debug "$BIN" "$P/xenia_edge"
for f in emulatorlauncher-edge xeniaEdgeGenerator.py es_features_xenia-edge.cfg; do
  git -C "$ROOT" show "HEAD:tools/batocera/$f" > "$P/$f"
done
git -C "$ROOT" show HEAD:LICENSE > "$P/LICENSE"
chmod 755 "$P/xenia_edge" "$P/emulatorlauncher-edge"
chmod 644 "$P/xeniaEdgeGenerator.py" "$P/es_features_xenia-edge.cfg" "$P/LICENSE"
cat > "$P/VERSION" <<EOF
Xenia Edge, the edge-linux fork (https://github.com/dcwestra/xenia-edge)
branch $branch
commit $head
built $(date -u +%Y-%m-%d) for Batocera 43 (x86_64, glibc 2.40 or newer)
EOF

tar -C "$STAGE" --owner=0 --group=0 --numeric-owner -cJf "$OUT/$NAME.tar.xz" "$NAME"
(cd "$OUT" && sha256sum "$NAME.tar.xz" > "$NAME.tar.xz.sha256")
git -C "$ROOT" show HEAD:tools/batocera/install.sh > "$OUT/install-batocera.sh"
chmod 755 "$OUT/install-batocera.sh"
ls -la "$OUT"
