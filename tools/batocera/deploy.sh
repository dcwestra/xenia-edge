#!/bin/sh
# Installs the portable Linux build (cmake -DXENIA_PORTABLE_LINUX=ON into
# build-batocera/) and the Batocera integration on a Batocera machine over SSH.
#
#   tools/batocera/deploy.sh [ssh host] [local Xenia storage root to copy
#                                        patches/ and config/ from]
#
# Patches and per-game configs from the local storage root are only added
# where the Batocera machine has none of that name yet, or only the stock
# patch file Batocera ships, as one copied there before may have been tuned
# there since. REPLACE_SETTINGS=1 replaces those too.
#
# Everything goes under /userdata, which Batocera updates keep. Files it would
# replace are kept as <name>.bak-<date> first, and Batocera's Wine Xenia setup
# isn't touched.
set -eu

HOST=${1:-batocera}
LOCAL_STORAGE=${2:-}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=$ROOT/build-batocera/bin/Linux/Release/xenia_edge
STAMP=$(date +%Y%m%d-%H%M%S)

if [ ! -x "$BIN" ]; then
  echo "No portable build at $BIN" >&2
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
echo "Binary needs $max_glibc at most, libstdc++ linked statically"

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/xenia-edge" "$STAGE/storage"
# Debug info is most of the binary's size; the unstripped one stays here for
# symbolizing crash reports.
objcopy --strip-debug "$BIN" "$STAGE/xenia-edge/xenia_edge"
cp "$ROOT/tools/batocera/emulatorlauncher-edge" \
   "$ROOT/tools/batocera/xeniaEdgeGenerator.py" "$STAGE/xenia-edge/"
chmod 755 "$STAGE/xenia-edge/xenia_edge" "$STAGE/xenia-edge/emulatorlauncher-edge"
cp "$ROOT/tools/batocera/es_systems_xbox360.cfg" "$STAGE/"
if [ -n "$LOCAL_STORAGE" ]; then
  for sub in patches config; do
    if [ -d "$LOCAL_STORAGE/$sub" ]; then
      mkdir -p "$STAGE/storage/$sub"
      find "$LOCAL_STORAGE/$sub" -maxdepth 1 -name '*.toml' \
        -exec cp {} "$STAGE/storage/$sub/" \;
    fi
  done
fi

rsync -a --info=stats1 "$STAGE/" "$HOST:/userdata/system/xenia-edge-staging/"

ssh "$HOST" STAMP="$STAMP" REPLACE_SETTINGS="${REPLACE_SETTINGS:-0}" sh -s <<'EOF'
set -eu
S=/userdata/system/xenia-edge-staging
D=/userdata/system/xenia-edge
ES=/userdata/system/configs/emulationstation
STORAGE=/userdata/system/configs/xenia-edge
mkdir -p "$D" "$ES" "$STORAGE/patches" "$STORAGE/config"

# Keep the previous emulator around until this one has been played with.
if [ -f "$D/xenia_edge" ]; then
  mv "$D/xenia_edge" "$D/xenia_edge.bak-$STAMP"
fi
cp -a "$S/xenia-edge/." "$D/"

if [ -f "$ES/es_systems_xbox360.cfg" ] &&
   ! cmp -s "$ES/es_systems_xbox360.cfg" "$S/es_systems_xbox360.cfg"; then
  cp -a "$ES/es_systems_xbox360.cfg" "$ES/es_systems_xbox360.cfg.bak-$STAMP"
fi
cp "$S/es_systems_xbox360.cfg" "$ES/"

# The community patches Batocera ships with Xenia Canary, without replacing
# any already here, which may have been edited.
if [ -d /usr/xenia-canary/patches ]; then
  cp -n /usr/xenia-canary/patches/*.toml "$STORAGE/patches/" 2>/dev/null || true
fi
# Patches and per-game configs copied from the development machine. A stock
# patch file of the same name is replaced (keeping a backup), but one this
# machine already had a copy of from here may have been tuned on it since, so
# it's only replaced with REPLACE_SETTINGS=1.
for sub in patches config; do
  [ -d "$S/storage/$sub" ] || continue
  for f in "$S/storage/$sub"/*.toml; do
    [ -e "$f" ] || continue
    name=$(basename "$f")
    dest="$STORAGE/$sub/$name"
    if [ -f "$dest" ] && ! cmp -s "$dest" "$f"; then
      stock="/usr/xenia-canary/patches/$name"
      if [ "$REPLACE_SETTINGS" != 1 ] &&
         ! { [ "$sub" = patches ] && [ -f "$stock" ] && cmp -s "$dest" "$stock"; }; then
        echo "Keeping $sub/$name as tuned here (REPLACE_SETTINGS=1 replaces it)"
        continue
      fi
      cp -a "$dest" "$dest.bak-$STAMP"
    fi
    cp "$f" "$dest"
  done
done

rm -rf "$S"
echo "Installed to $D:"
ls -la "$D"
# Not --help, which shows a message box on Batocera's display.
missing=$(ldd "$D/xenia_edge" 2>&1 | grep -E 'not found|version .* not found' || true)
if [ -n "$missing" ]; then
  echo "xenia_edge is missing libraries on this system:"
  echo "$missing"
  exit 1
fi
echo "All of xenia_edge's libraries are present"
EOF
