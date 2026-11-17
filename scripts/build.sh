#!/usr/bin/env bash
# Build LanScreen on x86_64 or aarch64 Linux.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-$root/build}"
jobs="$(nproc 2>/dev/null || echo 4)"
build_type="${CMAKE_BUILD_TYPE:-Release}"

mkdir -p "$build_dir"
cd "$build_dir"

if ! command -v cmake >/dev/null || ! command -v pkg-config >/dev/null; then
  echo "Need cmake and pkg-config on PATH." >&2
  exit 1
fi

for req in Qt6Core Qt6Widgets Qt6Network Qt6DBus gstreamer-1.0 gstreamer-video-1.0; do
  if ! pkg-config --exists "$req"; then
    echo "Missing pkg-config package: $req" >&2
    echo "See the README for dependencies on Arch and Debian/Ubuntu." >&2
    exit 1
  fi
done

cmake "$root" -DCMAKE_BUILD_TYPE="$build_type" "$@"
cmake --build . -j"$jobs"

echo
echo "Built: $build_dir/lanscreen"
echo "Try:   $build_dir/lanscreen --probe"
echo "       $build_dir/lanscreen --selftest"
