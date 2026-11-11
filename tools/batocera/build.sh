#!/bin/sh
# Builds the portable Linux build Batocera runs, into build-batocera/, for
# deploy.sh and package.sh:
#
#   tools/batocera/build.sh
#
# The build links the C++ runtime statically. A toolchain with no static
# libstdc++ installed needs one unpacked somewhere, named by STATIC_LIB_DIR.
# On Fedora that's the libstdc++-static package; unpacked into
# build-batocera/portable-sysroot, it's found without STATIC_LIB_DIR.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
FEDORA_LIBDIR=$ROOT/build-batocera/portable-sysroot/usr/lib/gcc/x86_64-redhat-linux/16
LIBDIR=${STATIC_LIB_DIR:-}
if [ -z "$LIBDIR" ] && [ -f "$FEDORA_LIBDIR/libstdc++.a" ]; then
  LIBDIR=$FEDORA_LIBDIR
fi
if [ -n "$LIBDIR" ] && [ ! -f "$LIBDIR/libstdc++.a" ]; then
  echo "No static libstdc++ at $LIBDIR" >&2
  exit 1
fi

cmake -S . -B build-batocera -G "Ninja Multi-Config" \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DXENIA_BUILD_TESTS=OFF -DXENIA_BUILD_MISC=OFF -DXENIA_ENABLE_LTO=ON \
  -DXENIA_PORTABLE_LINUX=ON -DXENIA_PORTABLE_LINUX_STATIC_LIB_DIR="$LIBDIR"
# The version the window title and logs show, as xb writes it for build/.
python3 -c '
import importlib.util
spec = importlib.util.spec_from_file_location("xb", "xenia-build.py")
xb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(xb)
xb.generate_version_h("build-batocera")
'
cmake --build build-batocera --config Release
echo "Built $ROOT/build-batocera/bin/Linux/Release/xenia_edge"
