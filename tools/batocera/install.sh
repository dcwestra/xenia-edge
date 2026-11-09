#!/bin/sh
# Installs Xenia Edge from the dcwestra/xenia-edge fork on a Batocera PC, as
# an extra Xbox 360 emulator, "xenia-edge", next to Batocera's own Xenia and
# Xenia Canary. Run it on the Batocera machine as root (log in over SSH as
# root, or open a terminal on it):
#
#   curl -fsSL https://github.com/dcwestra/xenia-edge/releases/latest/download/install-batocera.sh | sh
#
# To pass options when piping it, put them after "sh -s --", for example
# "... | sh -s -- --check". Options:
#
#   --check         download and check everything, but change nothing
#   --release TAG   install that release of the fork instead of the latest
#   --file ARCHIVE  install from a xenia-edge-batocera-x86_64.tar.xz on disk
#   --restart-es    restart EmulationStation when done, so xenia-edge shows up
#   --uninstall     remove it again (settings, saves and caches are kept)
#
# Running it again updates to the latest release, keeping the binary it
# replaces as xenia_edge.previous. Everything goes under /userdata, which
# Batocera updates keep. Batocera's own Xenia builds aren't touched, and saves
# are shared with them.
set -eu

REPO=${XENIA_EDGE_REPO:-dcwestra/xenia-edge}
NAME=xenia-edge-batocera-x86_64
# XENIA_EDGE_TEST_PREFIX puts what this installs under another directory, to
# try the installer out without touching the real installation.
PREFIX=${XENIA_EDGE_TEST_PREFIX:-}
D=$PREFIX/userdata/system/xenia-edge
ES=$PREFIX/userdata/system/configs/emulationstation
STORAGE=$PREFIX/userdata/system/configs/xenia-edge
STOCK_SYSTEMS=/usr/share/emulationstation/es_systems.cfg
SYSTEMS=$ES/es_systems_xbox360.cfg
FEATURES=$ES/es_features_xenia-edge.cfg
LAUNCHER=$D/emulatorlauncher-edge
# The files this installs into $D; --uninstall removes only these.
FILES="xenia_edge xenia_edge.previous emulatorlauncher-edge xeniaEdgeGenerator.py LICENSE VERSION"

say() { printf '%s\n' "$*"; }
die() { printf 'xenia-edge installer: %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
Installs the dcwestra/xenia-edge fork as the "xenia-edge" Xbox 360 emulator.
  --check         download and check everything, but change nothing
  --release TAG   install that release of the fork instead of the latest
  --file ARCHIVE  install from a xenia-edge-batocera-x86_64.tar.xz on disk
  --restart-es    restart EmulationStation when done, so xenia-edge shows up
  --uninstall     remove it again (settings, saves and caches are kept)
EOF
}

ours() { [ -f "$1" ] && grep -q 'emulatorlauncher-edge' "$1"; }

restart_es() {
  if [ "$restart" = 1 ]; then
    say "Restarting EmulationStation"
    batocera-es-swissknife --restart >/dev/null 2>&1 &
  else
    say "Restart EmulationStation to see the change:"
    say "  MAIN MENU > QUIT > RESTART EMULATIONSTATION"
  fi
}

# All of it in one function, run on the last line, so that a download cut off
# partway through runs nothing.
main() {
check=0 release='' archive='' restart=0 uninstall=0
while [ $# -gt 0 ]; do
  case $1 in
    --check) check=1 ;;
    --release) [ $# -ge 2 ] || die "--release needs a tag"; release=$2; shift ;;
    --file) [ $# -ge 2 ] || die "--file needs a path"; archive=$2; shift ;;
    --restart-es) restart=1 ;;
    --uninstall) uninstall=1 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option $1 (--help lists them)" ;;
  esac
  shift
done

[ "$(id -u)" = 0 ] || die "run this as root (Batocera's SSH login is root)"
[ -f /usr/share/batocera/batocera.version ] || die "this isn't a Batocera machine"
[ "$(uname -m)" = x86_64 ] || die "only x86_64 PCs are supported, not $(uname -m)"
bversion=$(cut -d' ' -f1 /usr/share/batocera/batocera.version)

if [ "$uninstall" = 1 ]; then
  [ "$check" = 0 ] || die "--check and --uninstall don't go together"
  if ours "$SYSTEMS"; then
    rm -f "$SYSTEMS"
    if [ -f "$SYSTEMS.before-xenia-edge" ]; then
      mv "$SYSTEMS.before-xenia-edge" "$SYSTEMS"
      say "Restored your own $SYSTEMS"
    fi
  fi
  rm -f "$FEATURES"
  for f in $FILES; do rm -f "$D/$f"; done
  rm -rf "$D/__pycache__"
  if [ -d "$D" ] && ! rmdir "$D" 2>/dev/null; then
    say "Left in $D, as this installer didn't put it there: $(ls -A "$D" | tr '\n' ' ')"
  fi
  say "Removed xenia-edge. Kept its settings ($STORAGE), shader cache"
  say "(/userdata/system/cache/xenia-edge), logs and saves (/userdata/saves/xbox360)."
  restart_es
  exit 0
fi

case ${bversion%%.*} in
  ''|*[!0-9]*) say "Warning: can't tell which Batocera this is ($bversion)" ;;
  4[3-9]|[5-9][0-9]) ;;
  *) say "Warning: made for Batocera 43, and this is $bversion. Checking it works here." ;;
esac

TMP=$(mktemp -d /userdata/system/.xenia-edge-install.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

if [ -n "$archive" ]; then
  [ -f "$archive" ] || die "no such file: $archive"
  cp "$archive" "$TMP/$NAME.tar.xz"
  [ ! -f "$archive.sha256" ] || cp "$archive.sha256" "$TMP/$NAME.tar.xz.sha256"
else
  if [ -n "$release" ]; then
    url=https://github.com/$REPO/releases/download/$release
  else
    url=https://github.com/$REPO/releases/latest/download
  fi
  say "Downloading $url/$NAME.tar.xz"
  curl -fL --retry 3 --progress-bar -o "$TMP/$NAME.tar.xz" "$url/$NAME.tar.xz" ||
    die "download failed"
  curl -fsSL --retry 3 -o "$TMP/$NAME.tar.xz.sha256" "$url/$NAME.tar.xz.sha256" ||
    die "couldn't download the checksum"
fi
if [ -f "$TMP/$NAME.tar.xz.sha256" ]; then
  (cd "$TMP" && sha256sum -c --quiet "$NAME.tar.xz.sha256") ||
    die "the download is damaged (checksum mismatch); try again"
else
  say "Warning: no $archive.sha256 next to the archive, so it isn't verified"
fi
tar -C "$TMP" -xJf "$TMP/$NAME.tar.xz" || die "couldn't unpack the archive"
P=$TMP/$NAME
for f in xenia_edge emulatorlauncher-edge xeniaEdgeGenerator.py es_features_xenia-edge.cfg; do
  [ -f "$P/$f" ] || die "the archive has no $f"
done
sed -n '1,4p' "$P/VERSION" 2>/dev/null || true

# Everything the emulator needs from the system has to be here: its
# libraries, and every function it calls in them. Only GTK's Wayland
# functions may be missing, as Batocera's GTK has no Wayland backend and
# they're only called on Wayland.
missing=$(ldd -r "$P/xenia_edge" 2>&1 | grep -E 'not found|undefined symbol' |
          grep -v 'undefined symbol: gdk_wayland_' || true)
[ -z "$missing" ] || die "this Batocera ($bversion) lacks libraries xenia_edge needs:
$missing"
# The generator and launcher plug into Batocera's own launcher (configgen),
# whose insides can change between Batocera versions.
python3 -B - "$P" <<'EOF' || die "this Batocera ($bversion)'s launcher doesn't fit xenia-edge's generator"
import sys
sys.path.insert(0, sys.argv[1])
from configgen import emulatorlauncher
from configgen.generators import get_generator  # noqa: F401
from xeniaEdgeGenerator import XeniaEdgeGenerator  # noqa: F401
for name in ('launch', 'get_generator'):
    if not hasattr(emulatorlauncher, name):
        sys.exit(f'configgen.emulatorlauncher has no {name}')
EOF

# The xbox360 system, as this Batocera defines it, with xenia-edge added. A
# version of it already in $ES that this installer didn't write is kept, and
# built on.
base=$STOCK_SYSTEMS
if [ -f "$SYSTEMS" ] && ! ours "$SYSTEMS"; then
  base=$SYSTEMS
fi
python3 -B - "$base" "$TMP/es_systems_xbox360.cfg" "$LAUNCHER" <<'EOF' || die "couldn't add xenia-edge to the xbox360 system"
import sys
import xml.etree.ElementTree as ET

src, dst, launcher = sys.argv[1:4]
system = next((s for s in ET.parse(src).getroot().iter('system')
               if s.findtext('name') == 'xbox360'), None)
if system is None:
    sys.exit(f'{src} has no xbox360 system')
command = system.find('command')
words = (command.text or '').split(None, 1)
if len(words) != 2 or words[0].rsplit('/', 1)[-1] not in (
        'emulatorlauncher', 'emulatorlauncher-edge'):
    sys.exit(f'unexpected xbox360 command in {src}: {command.text}')
command.text = f'{launcher} {words[1]}'
emulators = system.find('emulators')
if emulators is None:
    emulators = ET.SubElement(system, 'emulators')
if not any(e.get('name') == 'xenia-edge' for e in emulators.findall('emulator')):
    emulator = ET.SubElement(emulators, 'emulator', name='xenia-edge')
    ET.SubElement(ET.SubElement(emulator, 'cores'), 'core').text = 'xenia-edge'
root = ET.Element('systemList')
root.append(system)
ET.indent(root, '  ')
with open(dst, 'w', encoding='utf-8') as out:
    out.write('<?xml version="1.0"?>\n'
              '<!-- Batocera\'s xbox360 system with the native Xenia Edge build\n'
              '     added as "xenia-edge", written by its install-batocera.sh.\n'
              '     emulatorlauncher-edge hands every other emulator to\n'
              '     Batocera\'s own launcher unchanged. -->\n')
    out.write(ET.tostring(root, encoding='unicode'))
    out.write('\n')
EOF

if [ "$check" = 1 ]; then
  say ""
  say "Check passed: xenia-edge would install and run on this Batocera ($bversion)."
  say "The xbox360 system it would write to $SYSTEMS:"
  cat "$TMP/es_systems_xbox360.cfg"
  exit 0
fi

mkdir -p "$D" "$ES" "$STORAGE/patches" "$STORAGE/config"
if [ -f "$SYSTEMS" ] && ! ours "$SYSTEMS"; then
  cp -a "$SYSTEMS" "$SYSTEMS.before-xenia-edge"
fi
if [ -f "$D/xenia_edge" ]; then
  mv -f "$D/xenia_edge" "$D/xenia_edge.previous"
fi
for f in xenia_edge emulatorlauncher-edge xeniaEdgeGenerator.py LICENSE VERSION; do
  [ -f "$P/$f" ] && cp -f "$P/$f" "$D/$f"
done
chmod 755 "$D/xenia_edge" "$D/emulatorlauncher-edge"
rm -rf "$D/__pycache__"
cp -f "$P/es_features_xenia-edge.cfg" "$FEATURES"
cp -f "$TMP/es_systems_xbox360.cfg" "$SYSTEMS"

say ""
say "Installed xenia-edge to $D."
say "To play a game with it: in EmulationStation, hold A on the game, open"
say "ADVANCED GAME OPTIONS and set EMULATOR to XENIA-EDGE. For every Xbox 360"
say "game: MAIN MENU > GAME SETTINGS > PER SYSTEM ADVANCED CONFIGURATION >"
say "XBOX 360 > EMULATOR."
say "For a Kinect game, also set KINECT to ENABLED in its advanced options (and"
say "XBOX AVATARS to ENABLED for Kinect Sports and Kinect Adventures!)."
say "Settings: $STORAGE, logs: /userdata/system/logs/xenia-edge/"
[ ! -f "$D/xenia_edge.previous" ] ||
  say "The binary it replaced is kept as $D/xenia_edge.previous."
restart_es
}

main "$@"
