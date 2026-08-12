# `mirablast`

a bodged (at least for now) utility to use a miracast / p2p wi-fi display sink a a real extended monitor on x11! (currently targeting xfce4)

`gnome-network-displays` (`gnd`) can already handle miracst, but on a non-gnome/x11 session it falls back to capturing the whole root window via `ximagesrc`, since the screencast portal it uses for monitorselection requires Mutter, and the fallback will only mirror

mirablast aims to work around this in two pieces:

1. a minor patch set for `gnd` that:
    * lets it crop its x11 capture to an
    * arbitrary srea (`$GND_X11_REGION`) and a few tweaks pertaining to latency

2. `evdi-virtual-display`, which attaches a synthesised EDID to an [evdi](https://github.com/DisplayLink/evdi) device, giving you a RandR output for a specified resolution and refresh rate
    * **useful tip:** xfwm4 and the xfce/RandR tooling compute usable desktop space from RandR 1.2 outputs, not RandR 1.5 , and clamp windows to the physical panel edge if you use it. that was annoying to figure out lol

`miracast-extend` bridges the two:
  * it brings up the virtual output positioned to the right of your primary display,
      > as of right now it just positions the virtual output to the right of primary. though the virtual output is configurable in your display settings utility of choice, moving it from this positon is almost guaranteed to break things. for now i just have it "stuck" to the right of the primary display until the rest of the core functionality is more stable
  * then launches the patched gnd pointed at just that region.

## dependencies

* a. **runtime environment**
  * xfce on x11 
     > developed and tested on xfce only thus far other RandR/xrandr-based x11
    window managers *may* work, but the display-profile handling below is
    xfce-specific; worry not though! portability across x11 DEs/WMs will likely be explored in
    the future)
  * a miracast/WFD sink to cast to

    > currently only being tested on an [hp elite x3 lapdock](https://support.hp.com/au-en/product/product-specs/hp-elite-x3-lap-dock/12088822)

  <br>

* b. **`gnome-network-displays` build & runtime** (authoritative list is
`packaging/PKGBUILD`'s `depends=()`/`makedepends=()`; package names below are
arch's, other distros may name them differently)
  - `avahi`, `dnsmasq`
  - `gstreamer`, `gst-plugin-pipewire`, `gst-plugins-bad`, `gst-plugins-good`,
    `gst-plugins-ugly`, `gst-rtsp-server`
  - `gtk4`, `libadwaita`
  - `json-glib`, `libnm`, `libportal`, `libportal-gtk4`, `libpulse`, `libsoup3`
  - `networkmanager`, `protobuf-c`, `xdg-desktop-portal`
  - build-time only: `git`, `meson`, glib2 dev headers (`glib2-devel` on Arch)
  - optional: `gstreamer-vaapi`, for VA-API hardware encoding

* c. **virtual display**
  - The `evdi` kernel module, e.g. via `evdi-dkms` on arch linux (aur)
  - Its `PyEvdi` Python bindings (ships w/ `evdi-dkms` from the aur)

## build


### **patch `gnome-network-displays`**


apply the patches to an upstream checkout and build with `meson`:
```
git clone https://gitlab.gnome.org/GNOME/gnome-network-displays.git
cd gnome-network-displays
patch -Np1 -i ../packaging/gnd-x11-region-crop.patch
patch -Np1 -i ../packaging/gnd-gop-seconds.patch
patch -Np1 -i ../packaging/gnd-latency.patch
meson setup build --prefix=/usr/local
meson compile -C build
sudo meson install -C build
```

### **the scripts themselves**

`bin/evdi-virtual-display` and `bin/miracast-extend` are plain 'ol python and bash scripts respectively, put them on your `$PATH` e.g.:
```
ln -s "$PWD/bin/"* ~/.local/bin/
```
both scripts locate each other relative to their own path, so they still work regardless of wherever you choose to symlink them from

## usage

| command | action |
|---------|--------|
| `miracast-extend run 1920x1080` | bring up the virtual display, launch `gnd`, tear down on exit |
| `miracast-extend on 1920x1080`  | bring up the virtual display                                  |
| `miracast-extend off`           | tear down the virtual display                                 |
| `miracast-extend status`        | show current outputs and state of evdi helper                 |

connect to your miracast sink from the "network displays" window as usual. it will appear as a plain second output in `xrandr`, `arandr`, and `xfce4-display-settings`, etc. immediately, and will only commence streaming once connected.

it's also a good idea to match the virtual display's resolution to what the sink actually expects; a mismatch makes `gnd`'s `videoscale` rescale every frame, which often ended up doubling CPU use in testing

### environment variables

| variable           | effect                                                                      | default                                     |
|--------------------|---------------------------------------------------------------------------------|---------------------------------------------|
| `PRIMARY`          | output to extend from                                                           | autodetected                                |
| `REFRESH`          | refresh rate (Hz) for virtual display                                           | `60`                                        |
| `HELPER`           | path to `evdi-virtual-display`                                                  | `.`                         |
| `LOG`              | path for the evdi helper's stdout/stderr                                        | `$XDG_RUNTIME_DIR/evdi-virtual-display.log` |
| `KEEP_RUNNING`     | if `1`, refuse to kill an already-running `gnd` instead of restarting it          | unset                                       |
| `GND_X11_DAMAGE`   | set to `0` to disable XDamage on the cropped capture (see below, ***don't!***)     | unset (XDamage on)                          |
| `GND_GOP_SECONDS`  | keyframe interval in seconds, 1-60                                              | `1`                                         |
| `GND_LATENCY_MS`   | Pipeline latency in ms, 0-2000, overrides the encoder-based default             | `500` for openh264, `100` otherwise         |

`GND_X11_REGION` is set by `miracast-extend`; there's usually normally no reason to set it directly.

## how it works

- **`bin/evdi-virtual-display`** opens an evdi card, builds a 128-byte EDID
  with one timing descriptor for the `WxH@refresh`, and
  holds the connection open until terminated. timing is
  calculated with [VESA CVT-RB v1](https://glenwing.github.io/docs/VESA-CVT-1.2.pdf) (the same formula `cvt -r` and the kernel's
  `drm_cvt_mode(reduced=true)` use).

- **`bin/miracast-extend`** starts that helper, waits for the resulting
  output to appear in `xrandr`, positions right of `$PRIMARY`, and
  watches for a few seconds afterwards: 
    - `xfsettingsd` applies its active display profile whenever outputs change,  and might somethings switch
  the primary display off leaving only the new virtual display active. in the case that
  this happens, it should be undone automatically. (appalling scap job, i know)
  -  it then launches `gnome-network-displays` with `GND_X11_REGION` set to the virtual output's
  geometry, and tears everything down on exit.

- **`packaging/gnd-x11-region-crop.patch`** adds `$GND_X11_REGION` handling
  to gnd's X11 fallback path (`src/app/nd-window.c`), cropping the
  `ximagesrc` capture instead of grabbing the whole root window.

- **`packaging/gnd-gop-seconds.patch`** makes the H.264 keyframe interval
  configurable via `$GND_GOP_SECONDS` (upstream hardcoded at 10 seconds)

- **`packaging/gnd-latency.patch`** stops charging every session a fixed
  500ms of pipeline latency. upstream applies that unconditionally, 
  > * there is a comment stating it's actually an `openh264enc` workaround for 
  >   latency spikes occuring due to scene changes
  > * so, sessions using a hardware encoder (`vah264enc`) were being nerfed for no reason 
      (*at least, i think they are according to my not-so-scientific testing*)

## troubleshooting

- ### flickering / persistent cursor ghosting \ on the sink:
  high probability that XDamage is turned off (`GND_X11_DAMAGE=0`). 
  i don't quite fully understand the reason this is, but at least i understand the fix! /s.
  uncropped fullframe reads seem to interact very poorly with the rest of the pipeline when 
  damage tracking is disabled.

- ### laptop panel goes blank after `on`/`run`:
  `miracast-extend` already watches for this and re-enables `$PRIMARY` automatically, but if it keeps
    recurring, check `xfconf-query -c displays -lv` (a saved xfce display
    profile could possibly have a stale `Active=false` for the  primary output). 
    fix the profile manually, or disable `/AutoEnableProfiles`.

- ### high CPU / choppy video.
    check the virtual display's resolution matches what the sink negotiated (see Usage above):
    > *a mismatch forces `videoscale` to rescale every frame*

- ### `evdi output never came up`: 
  check `$LOG` (default `$XDG_RUNTIME_DIR/evdi-virtual-display.log`). a cause may be
  `sku_area_limit`/`--area-limit` rejecting the requested pixel area; the default limit is 3840x2160 (four-kay)

## license

MIT, see `LICENSE`. 

> * the bundled `packaging/PKGBUILD` is a derivative of the `gnome-network-displays-git` [AUR package](https://aur.archlinux.org/packages/gnome-network-displays-git) (maintained by yochananmarqos) with the patches in this repo applied via its `prepare()`; 
> * `gnome-network-displays` itself is GPL-3.0-or-later.
