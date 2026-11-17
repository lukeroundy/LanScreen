# LanScreen

Use a second Linux PC as an extra monitor over your local network. One machine
is the host: it can create a KDE/KWin virtual display, capture it, encode it,
and stream it. The other is the client: it decodes the stream and shows it
fullscreen.

The same app does both jobs. It builds on x86_64 and aarch64. Encoders are
detected at runtime, so you can copy the same binary between a Mesa box, an
NVIDIA box, and a Snapdragon / Adreno laptop without linking vendor SDKs.

This is version 0.2.25. The software encode/decode path (x264 over SRT) has
been compile-tested and run through a loopback self-test. On KDE Plasma
Wayland, the host can create a KWin virtual monitor automatically so the
second PC is a real extended screen, not a mirror. The client is display-only
in this version: it does not send mouse or keyboard input back to the host.
Hardware encoders are wired up and will be used automatically when GStreamer
can actually create them, but they have not been tried on real NVIDIA or
Adreno hardware yet.

## What you need

- Linux (Arch and Debian/Ubuntu are the ones called out below)
- Qt 6 (Core, Widgets, Network, DBus)
- GStreamer 1.x, including:
  - gst-plugins-base, good, bad
  - gst-plugins-ugly (for `x264enc`, the software H.264 fallback)
  - gst-libav (software decode)
  - SRT support (`srtsink` / `srtsrc`, usually in gst-plugins-bad)
  - optional: gst-plugin-pipewire, gstreamer-vaapi / gst-va
- For a virtual monitor on the host: KDE Plasma on Wayland, plus `krfb`
  (`krfb-virtualmonitor`) and `kscreen` (`kscreen-doctor`)

### Arch Linux (x86_64 or aarch64)

```bash
sudo pacman -S --needed base-devel cmake pkgconf qt6-base qt6-wayland \
  gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad gst-plugins-ugly \
  gst-libav gst-plugin-pipewire srt libevdev
```

On the host, also install `krfb` and `kscreen` if you want the automatic
virtual monitor.

Hardware-specific extras, only on the machines that have the hardware:

```bash
# Mesa / AMD / Intel (VAAPI)
sudo pacman -S --needed mesa libva libva-mesa-driver intel-media-driver libva-utils

# NVIDIA (NVENC / NVDEC). Needs the proprietary driver; the GStreamer nvcodec
# plugin lives in gst-plugins-bad and is already installed above.
sudo pacman -S --needed nvidia nvidia-utils

# Snapdragon / Adreno (V4L2 Venus): no extra userspace SDK. You need a kernel
# that exposes the Venus encoder as a /dev/video* node. v4l-utils is handy:
sudo pacman -S --needed v4l-utils
v4l2-ctl --list-devices
```

Do not install NVIDIA userspace on a Snapdragon machine, and do not expect a
Venus `/dev/video*` node on a typical x86 box. LanScreen just looks at what
GStreamer can actually open.

### Debian / Ubuntu

```bash
sudo apt install build-essential cmake pkgconf \
  qt6-base-dev qt6-wayland-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly \
  gstreamer1.0-libav gstreamer1.0-tools gstreamer1.0-x gstreamer1.0-gl \
  gstreamer1.0-pipewire gstreamer1.0-vaapi \
  libsrt-gnutls-dev libevdev-dev
```

On the host, also install `krfb` and `kscreen` if you want the automatic
virtual monitor.

Package names drift a bit between distro versions. If `pkg-config` cannot
find `Qt6Core` or `gstreamer-1.0`, the build script will tell you.

## Build

```bash
git clone https://github.com/lukeroundy/LanScreen.git lanscreen
cd lanscreen
./scripts/build.sh
```

That writes `build/lanscreen`. The script is the same on x86_64 and aarch64.
You can also do it by hand:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Useful checks after a build:

```bash
./build/lanscreen --probe          # what encoders/decoders GStreamer can open
./build/lanscreen --list-outputs   # displays, plus virtual-display notes
./build/lanscreen --selftest       # software H.264 over SRT, loopback
```

## AppImage

A portable x86_64 AppImage is attached to the [GitHub
releases](https://github.com/lukeroundy/LanScreen/releases). You can also
build one yourself for whatever architecture you run the script on (`x86_64`
or `aarch64`):

```bash
./scripts/build-appimage.sh
```

That produces `LanScreen-x86_64.AppImage` (or `LanScreen-aarch64.AppImage`)
and a copy under `dist/`. The script calls `scripts/build.sh` first, then
linuxdeploy with the Qt and GStreamer plugins.

It bundles Qt 6, GStreamer (base, good, bad, ugly, libav, SRT, PipeWire
plugin) and the usual runtime libraries. GPU encoder plugins (NVIDIA NVENC,
Mesa/Intel VAAPI, Snapdragon V4L2 Venus) are not something we can usefully
ship from another machine — those have to come from the host OS and its
driver. The AppImage still looks in the system's GStreamer plugin dirs so
those hardware plugins can load when they are installed.

`krfb-virtualmonitor` and `kscreen-doctor` are also left to the host OS, as
described below.

Run it like any other AppImage:

```bash
chmod +x LanScreen-x86_64.AppImage
./LanScreen-x86_64.AppImage
./LanScreen-x86_64.AppImage --probe
./LanScreen-x86_64.AppImage --host --virtual-display --virtual-res 1920x1080
```

If the AppImage fails to mount (no FUSE), try:

```bash
./LanScreen-x86_64.AppImage --appimage-extract-and-run
```

## Run

### Host (the main PC, the one whose desktop you want to extend)

```bash
./build/lanscreen
# open the "Share a screen (Host)" tab, set bitrate / encoder, Start sharing
```

Or from the command line:

```bash
./build/lanscreen --host --encoder auto --codec h264 --bitrate 8000 --fps 60
./build/lanscreen --host --source test --encoder software --headless
```

The host advertises itself on the LAN with a UDP beacon (port 47500). Control
is TCP (default 47510). Video defaults to SRT on 47511. Set a PIN if you do
not want anyone on the network walking in; the PIN is also used as the SRT
passphrase.

### Client (the second PC, the extra monitor)

```bash
./build/lanscreen
# open "Use as a monitor (Client)", pick the host or type its IP, Connect
```

```bash
./build/lanscreen --client --connect 192.168.1.10 --pin 1234 --fullscreen
```

The video window can go borderless. Escape or the Fullscreen button toggles
fullscreen. This version is display-only: mouse and keyboard on the client
are not sent back to the host.

### Encoder settings

Everything below is in the Host tab and as CLI flags:

| Setting            | What it does                                      | Typical value      |
|--------------------|---------------------------------------------------|--------------------|
| Encoder            | Auto, NVENC, VAAPI, V4L2, or software             | Auto               |
| Codec              | H.264, HEVC, or AV1 (if the encoder supports it)  | H.264              |
| Bitrate            | Slider + numeric field, in kbps                   | 8000               |
| Rate control       | CBR, VBR, or CQP (when the encoder allows it)     | CBR                |
| Framerate          | FPS                                               | 60                 |
| Resolution         | Scaled output, or 0×0 for native                  | 1920×1080          |
| Preset             | Ultra low latency, low latency, quality           | Low latency        |
| Keyframe interval  | Frames between keyframes                          | 60                 |
| Transport          | SRT, RTP/UDP, or TCP                              | SRT                |
| SRT latency        | Buffer / recovery window, in milliseconds         | 60                 |
| UDP FEC            | Optional, RTP only, still experimental            | off                |

Auto prefers NVENC, then VAAPI, then V4L2, then software, and only picks an
encoder GStreamer can actually put into the READY state.

## Encoder notes by GPU

**Mesa (AMD / Intel) — VAAPI.** LanScreen looks for `vah264enc` (newer
gst-va plugin) and `vaapih264enc` (older gstreamer-vaapi). HEVC and AV1 are
used when those elements exist too. Install `mesa` / `libva-mesa-driver` or
`intel-media-driver`, then check with `vainfo` and `lanscreen --probe`.

Bitrate units are messy across VA plugins. The gst-va elements (`vah264enc`)
take bits/sec; the older `vaapih264enc` takes kbps. LanScreen converts so the
number you type in the UI is always kbps.

**NVIDIA — NVENC.** Looks for `nvh264enc` / `nvh265enc` / `nvav1enc`, plus
the CUDA-prefixed and Jetson `nvv4l2*` names. You need the NVIDIA driver so
`libcuda.so.1` is present. If the nvcodec plugin loads but registers zero
elements, there is usually no NVIDIA GPU on that machine, which is the
expected runtime behavior.

**Qualcomm Snapdragon / Adreno — V4L2 Venus.** Looks for `v4l2h264enc` and
`v4l2h265enc`. Those elements only appear when the kernel exposes a Venus
encoder node under `/dev/video*`. Bitrate is passed as V4L2 `extra-controls`
in bits/sec (`video_bitrate`, `video_bitrate_mode`, `video_gop_size`). If
`--probe` does not list `v4l2h264enc` as ready, the encoder node is not
there (or the user cannot open it).

**Software fallback.** H.264 uses `x264enc` (then OpenH264). HEVC uses
`x265enc`. AV1 uses `svtav1enc`, then `av1enc`. This is the path that works
everywhere, including headless boxes with no GPU. It costs more CPU and adds
a little more latency than a hardware encoder.

## Virtual displays on KDE

If both machines are on KDE, this is the path you want. The host (your main
PC) should be logged into **Plasma on Wayland**. The client can be Wayland or
X11; it only has to decode and show the stream.

KWin does not expose an `addVirtualOutput` method on D-Bus. The official way
to add a virtual monitor is `krfb-virtualmonitor` from the `krfb` package.
Under the hood it talks to KWin over the `zkde_screencast_unstable_v1`
Wayland protocol (`stream_virtual_output`). KWin then creates the output,
names it `Virtual-<name>`, and removes it when the process exits.
`kscreen-doctor` (from the `kscreen` package) is used afterwards to place
that output to the right of your existing screens and, on Plasma 6.6+, to
set a custom refresh rate.

Install on the host:

```bash
# Arch
sudo pacman -S krfb kscreen

# Debian / Ubuntu
sudo apt install krfb kscreen
```

In the Host tab, leave **Create a virtual monitor when sharing starts**
checked (it is on by default when a Plasma Wayland session is detected),
pick a resolution and refresh rate, and hit Start sharing. Or from the CLI:

```bash
./build/lanscreen --host --virtual-display --virtual-res 1920x1080 --virtual-hz 60
```

`--no-virtual-display` turns this off. `lanscreen --list-outputs` prints
whether KWin, `krfb-virtualmonitor`, and `kscreen-doctor` were found.

What happens on Start:

1. LanScreen starts `krfb-virtualmonitor --name lanscreen --resolution WxH`.
   The new output shows up as `Virtual-lanscreen`.
2. `kscreen-doctor` places it to the right of your other monitors. If you
   asked for a refresh rate other than 60 Hz, it tries
   `output.Virtual-lanscreen.addCustomMode.…` and then `.mode.WxH@Hz`.
   That custom-mode command exists in Plasma 6.6+. On older Plasma the
   virtual monitor stays at 60 Hz and the log says so.
3. The encode pipeline captures that virtual output, not your main screen.
   On Wayland this goes through the desktop portal — if a share dialog
   appears, pick **Virtual-lanscreen**, not the laptop panel.
4. On Stop (or a clean exit) `krfb-virtualmonitor` is killed and KWin
   removes the virtual monitor.

`krfb-virtualmonitor` also starts a VNC listener because that is how the
tool is built. LanScreen gives it a random password and a high port
(5910–5989) and never uses that VNC session. Newer krfb can skip VNC if no
password is set; a password is still passed so older packages work.

This does **not** work on Plasma X11. `krfb-virtualmonitor` is Wayland-only.
If you are on X11, or `krfb` is missing, the Host tab explains why and you
can still use a dummy HDMI/DP dongle, pick an existing output, or a test
pattern.

Wayland vs X11, short version:

- **Host on Plasma Wayland:** virtual monitor works, as above.
- **Host on Plasma X11, or not KDE:** no automatic virtual monitor; use a
  dummy plug or an existing output. X11 capture uses `ximagesrc`.
- **Client:** any Linux session is fine.

`krfb-virtualmonitor` and `kscreen-doctor` are called from the *host OS*,
even if you run the AppImage. They have to talk to the KWin that is already
running, so they are not bundled inside the AppImage.

## Capture notes

- **X11:** `ximagesrc`, optionally cropped to a chosen screen. Works well,
  includes the pointer if `show-pointer` is on.
- **Wayland:** the xdg-desktop-portal ScreenCast path, then `pipewiresrc`.
  The desktop will prompt you to share a monitor the first time.
- **Test pattern:** always available, useful for checking bitrate and
  transport before you fight with capture.

## Input

In 0.2.25 the client is display-only. Source for a virtual mouse and
keyboard (`/dev/uinput`) is still in the tree, but the host does not create
those devices. A udev rule is in `scripts/99-lanscreen-uinput.rules` if you
are working on that path yourself; you do not need it for normal use.

## Security

- Discovery is a LAN broadcast. Anyone on the same network can see the host
  name and ports.
- An optional PIN is required before the control channel will hand out
  stream details. Five bad tries and the client is dropped.
- The same PIN is stretched into an SRT passphrase so the video itself is
  encrypted when you set one. Use a PIN if you are not on a network you trust.
- The host listens on all IPv4 interfaces by default (so localhost tests
  work) but advertises its private LAN address. You can bind to a specific
  interface in the UI.

## Flags

```
lanscreen --host|--client [options]
  --encoder auto|nvenc|vaapi|v4l2|software
  --decoder auto|hw|sw
  --codec h264|hevc|av1
  --bitrate KBPS
  --rate-control cbr|vbr|cqp
  --fps N  --width W  --height H
  --preset ultra-low-latency|low-latency|quality
  --keyint N
  --transport srt|udp|tcp
  --srt-latency MS
  --fec
  --bind ADDR  --port N  --media-port N
  --source test|screen|portal|output=NAME
  --virtual-display / --no-virtual-display
  --virtual-res WxH   --virtual-hz N
  --pin PIN  --name NAME
  --connect HOST[:PORT]
  --fullscreen
  --headless
  --probe  --list-outputs  --selftest
```

## What this version does not do yet

- Automatic virtual monitors are KDE Plasma Wayland only (via
  `krfb-virtualmonitor`). Other desktops still need a dummy plug or an
  existing output.
- Refresh rates other than 60 Hz on that virtual monitor need Plasma 6.6+
  (`kscreen-doctor addCustomMode`).
- On Wayland, the first capture of the virtual monitor may show a portal
  share dialog — pick `Virtual-lanscreen`.
- Only one client can be connected at a time.
- No audio, no clipboard, no file drop, no multi-monitor layout editor, and
  no mouse/keyboard control from the client.
- HEVC and AV1 depend on the encoder actually being there; H.264 is the one
  you can count on.
- UDP FEC is experimental.
- Hardware encode/decode has not been soak-tested on NVIDIA, Intel, AMD, or
  Snapdragon machines. If `--probe` says an encoder is ready and the stream
  still fails, the pipeline logs in the UI are the first place to look.

If something breaks on your box, the Host/Client log pane prints the exact
GStreamer pipeline that was started. That is usually enough to see whether
the encoder, the capture source, or the transport is the part that failed.

## License

MIT. See [LICENSE](LICENSE).
