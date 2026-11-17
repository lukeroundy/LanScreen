#!/usr/bin/env bash
# Build a LanScreen AppImage for the machine you run this on (x86_64 or aarch64).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
arch="$(uname -m)"
case "$arch" in
  x86_64|amd64) arch=x86_64 ;;
  aarch64|arm64) arch=aarch64 ;;
  *)
    echo "Unsupported architecture: $arch (need x86_64 or aarch64)" >&2
    exit 1
    ;;
esac

build_dir="${BUILD_DIR:-$root/build}"
work="${APPIMAGE_WORK:-$build_dir/appimage}"
tools="$work/tools"
appdir="$work/AppDir"
dist="${DIST_DIR:-$root/dist}"
version="${LANSCREEN_VERSION:-}"

if [[ -z "$version" ]]; then
  version="$(sed -n 's/.*project(lanscreen VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt" | head -1)"
fi
version="${version:-0.2.25}"

echo "LanScreen AppImage  version=$version  arch=$arch"

if ! command -v patchelf >/dev/null; then
  echo "patchelf is required (Arch: pacman -S patchelf; Debian: apt install patchelf)." >&2
  exit 1
fi

# Build the binary if needed.
if [[ ! -x "$build_dir/lanscreen" ]]; then
  echo "Building lanscreen first…"
  BUILD_DIR="$build_dir" "$root/scripts/build.sh"
fi
if [[ ! -x "$build_dir/lanscreen" ]]; then
  echo "No $build_dir/lanscreen binary." >&2
  exit 1
fi

mkdir -p "$tools" "$dist"
rm -rf "$appdir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/share/applications" "$appdir/usr/share/icons/hicolor/256x256/apps"

cp -f "$build_dir/lanscreen" "$appdir/usr/bin/lanscreen"
cp -f "$root/packaging/lanscreen.desktop" "$appdir/usr/share/applications/lanscreen.desktop"
cp -f "$root/packaging/lanscreen.desktop" "$appdir/lanscreen.desktop"
cp -f "$root/packaging/lanscreen.png" "$appdir/usr/share/icons/hicolor/256x256/apps/lanscreen.png"
cp -f "$root/packaging/lanscreen.png" "$appdir/lanscreen.png"

download() {
  local url="$1" dest="$2"
  if [[ -x "$dest" || -f "$dest" ]]; then
    return 0
  fi
  echo "Downloading $(basename "$dest")…"
  if command -v curl >/dev/null; then
    curl -L --fail --retry 3 -o "$dest" "$url"
  else
    wget -O "$dest" "$url"
  fi
}

run_appimage() {
  local bin="$1"
  shift
  if [[ "$bin" == *.AppImage ]]; then
    chmod +x "$bin"
    if APPIMAGE_EXTRACT_AND_RUN=1 "$bin" --appimage-help >/dev/null 2>&1 || true; then
      APPIMAGE_EXTRACT_AND_RUN=1 "$bin" "$@"
      return
    fi
    # Fallback: extract once and run the inner AppRun.
    local extdir
    extdir="$(dirname "$bin")/$(basename "$bin" .AppImage).extracted"
    if [[ ! -x "$extdir/AppRun" ]]; then
      rm -rf "$extdir"
      (cd "$(dirname "$bin")" && APPIMAGE_EXTRACT_AND_RUN=1 "$bin" --appimage-extract >/dev/null 2>&1) || true
      # linuxdeploy extracts to squashfs-root in cwd
      if [[ -d "$(dirname "$bin")/squashfs-root" && ! -d "$extdir" ]]; then
        mv "$(dirname "$bin")/squashfs-root" "$extdir"
      fi
    fi
    if [[ -x "$extdir/AppRun" ]]; then
      "$extdir/AppRun" "$@"
      return
    fi
    APPIMAGE_EXTRACT_AND_RUN=1 "$bin" "$@"
  else
    chmod +x "$bin"
    "$bin" "$@"
  fi
}

ld_url="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${arch}.AppImage"
qt_url="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-${arch}.AppImage"
gst_url="https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gstreamer/master/linuxdeploy-plugin-gstreamer.sh"

download "$ld_url" "$tools/linuxdeploy-${arch}.AppImage"
download "$qt_url" "$tools/linuxdeploy-plugin-qt-${arch}.AppImage"
download "$gst_url" "$tools/linuxdeploy-plugin-gstreamer.sh"
chmod +x "$tools/linuxdeploy-${arch}.AppImage" "$tools/linuxdeploy-plugin-qt-${arch}.AppImage" "$tools/linuxdeploy-plugin-gstreamer.sh"

# linuxdeploy looks for plugins next to itself or on PATH / LINUXDEPLOY_PLUGIN_DIR.
export LINUXDEPLOY="$tools/linuxdeploy-${arch}.AppImage"
# When using EXTRACT_AND_RUN, plugin discovery is happier if plugins are on PATH.
export PATH="$tools:$PATH"
cp -f "$tools/linuxdeploy-plugin-qt-${arch}.AppImage" "$tools/linuxdeploy-plugin-qt.AppImage"
chmod +x "$tools/linuxdeploy-plugin-qt.AppImage"

if command -v qmake6 >/dev/null; then
  export QMAKE="$(command -v qmake6)"
elif command -v qmake >/dev/null; then
  export QMAKE="$(command -v qmake)"
fi

export GSTREAMER_INCLUDE_BAD_PLUGINS=1
export GSTREAMER_VERSION=1.0
# Debian vs Arch plugin locations
if [[ -d /usr/lib/${arch}-linux-gnu/gstreamer-1.0 ]]; then
  export GSTREAMER_PLUGINS_DIR=/usr/lib/${arch}-linux-gnu/gstreamer-1.0
elif [[ -d /usr/lib/gstreamer-1.0 ]]; then
  export GSTREAMER_PLUGINS_DIR=/usr/lib/gstreamer-1.0
elif [[ -d /usr/lib64/gstreamer-1.0 ]]; then
  export GSTREAMER_PLUGINS_DIR=/usr/lib64/gstreamer-1.0
fi
if [[ -d /usr/lib/${arch}-linux-gnu/gstreamer1.0/gstreamer-1.0 ]]; then
  export GSTREAMER_HELPERS_DIR=/usr/lib/${arch}-linux-gnu/gstreamer1.0/gstreamer-1.0
fi

export LINUXDEPLOY_OUTPUT_VERSION="$version"
export LDAI_OUTPUT="$dist/LanScreen-${arch}.AppImage"
export ARCH="$arch"
export APPIMAGE_EXTRACT_AND_RUN=1

cd "$work"

# Populate AppDir (Qt + GStreamer), then patch the GStreamer hook so host-OS
# hardware encoder plugins can still load, then wrap as an AppImage.
run_appimage "$tools/linuxdeploy-${arch}.AppImage" \
  --appdir "$appdir" \
  --executable "$appdir/usr/bin/lanscreen" \
  --desktop-file "$appdir/lanscreen.desktop" \
  --icon-file "$appdir/lanscreen.png" \
  --plugin qt \
  --plugin gstreamer

hook="$appdir/apprun-hooks/linuxdeploy-plugin-gstreamer.sh"
mkdir -p "$appdir/apprun-hooks"
if [[ -f "$hook" ]]; then
  # Keep bundled plugins first, then also search the host OS so NVENC / VAAPI /
  # V4L2 Venus plugins from the installed driver can be used.
  cat >> "$hook" << 'HOOK'

# Host-OS GStreamer plugins (NVIDIA, Mesa/Intel VAAPI, Snapdragon V4L2, …)
for _gst_dir in \
    /usr/lib/$(uname -m)-linux-gnu/gstreamer-1.0 \
    /usr/lib64/gstreamer-1.0 \
    /usr/lib/gstreamer-1.0; do
  if [ -d "$_gst_dir" ]; then
    GST_PLUGIN_SYSTEM_PATH_1_0="${GST_PLUGIN_SYSTEM_PATH_1_0}:$_gst_dir"
  fi
done
export GST_PLUGIN_SYSTEM_PATH_1_0
# Do not reuse a stale registry from a different machine / driver set.
export GST_REGISTRY="${XDG_CACHE_HOME:-$HOME/.cache}/lanscreen/gstreamer-1.0/registry.bin"
mkdir -p "$(dirname "$GST_REGISTRY")"
HOOK
else
  echo "Warning: GStreamer AppRun hook was not installed; writing our own." >&2
  cat > "$hook" << 'HOOK'
export GST_REGISTRY_REUSE_PLUGIN_SCANNER=no
export GST_PLUGIN_PATH_1_0="${APPDIR}/usr/lib/gstreamer-1.0"
export GST_PLUGIN_SYSTEM_PATH_1_0="${APPDIR}/usr/lib/gstreamer-1.0"
for _gst_dir in \
    /usr/lib/$(uname -m)-linux-gnu/gstreamer-1.0 \
    /usr/lib64/gstreamer-1.0 \
    /usr/lib/gstreamer-1.0; do
  if [ -d "$_gst_dir" ]; then
    GST_PLUGIN_SYSTEM_PATH_1_0="${GST_PLUGIN_SYSTEM_PATH_1_0}:$_gst_dir"
  fi
done
export GST_PLUGIN_SYSTEM_PATH_1_0
if [ -x "${APPDIR}/usr/lib/gstreamer1.0/gstreamer-1.0/gst-plugin-scanner" ]; then
  export GST_PLUGIN_SCANNER_1_0="${APPDIR}/usr/lib/gstreamer1.0/gstreamer-1.0/gst-plugin-scanner"
fi
HOOK
fi

run_appimage "$tools/linuxdeploy-${arch}.AppImage" \
  --appdir "$appdir" \
  --output appimage

# linuxdeploy / the appimage plugin may write to LDAI_OUTPUT, $work, or $PWD.
shopt -s nullglob
produced=(
  "$dist"/LanScreen-*.AppImage
  "$LDAI_OUTPUT"
  "$work"/LanScreen*.AppImage
  "$work"/lanscreen*.AppImage
  "$work"/*.AppImage
)
shopt -u nullglob
out="$dist/LanScreen-${arch}.AppImage"
picked=""
for f in "${produced[@]:-}"; do
  if [[ -n "$f" && -f "$f" ]]; then
    picked="$f"
    break
  fi
done
if [[ -z "$picked" ]]; then
  echo "linuxdeploy finished but no AppImage was found" >&2
  ls -la "$work" "$dist" >&2 || true
  exit 1
fi
mkdir -p "$dist"
if [[ "$picked" != "$out" ]]; then
  cp -f "$picked" "$out"
fi
chmod +x "$out"
# Also drop a copy next to the source tree (gitignored).
cp -f "$out" "$root/LanScreen-${arch}.AppImage"

echo
echo "AppImage: $out"
ls -lh "$out"
