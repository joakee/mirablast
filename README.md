# `mirablast`

a bodged (at least for now) utility to use a miracast / p2p wi-fi display sink as a real extended monitor on x11! (currently targeting xfce4)

`gnome-network-displays` (`gnd`) can already handle miracast, but on a non-gnome/x11 session it falls back to capturing the whole root window via `ximagesrc`, since the screencast portal it uses for monitor selection requires Mutter, and the fallback will only mirror

mirablast is now a **single tray application** (`mirablast-app`) that does the whole job in one process:

1. it spawns `evdi-virtual-display`, which attaches a synthesised EDID to an [evdi](https://github.com/DisplayLink/evdi) device, giving you a real RandR output at a chosen resolution and refresh rate
    * **useful tip:** xfwm4 and the xfce/RandR tooling compute usable desktop space from RandR 1.2 outputs, not RandR 1.5, and clamp windows to the physical panel edge if you use it. that was annoying to figure out lol
2. it positions that output to the right of your primary display
    > the virtual output is configurable in your display settings utility of choice, but moving it from this position is almost guaranteed to break things. for now it's "stuck" to the right of the primary display until the rest of the core functionality is more stable
3. it runs the WFD/miracast backend **in-process** — the relevant parts of `gnome-network-displays` are vendored into `app/vendor/`, so there's no external `gnd` to install or patch anymore — with the `ximagesrc` capture cropped to exactly the virtual output's region

everything is driven from a tray icon: pick a sink, cast, disconnect, tweak settings, quit.

> the older script-based workflow (`miracast-extend` + a patched upstream `gnd`) still exists in this repo, see [legacy](#legacy-the-script--patch-workflow) at the bottom

## dependencies

* a. **runtime environment**
  * xfce on x11
     > developed and tested on xfce only thus far. other RandR/xrandr-based x11
    window managers *may* work, but the display-profile handling below is
    xfce-specific; worry not though! portability across x11 DEs/WMs will likely be explored in
    the future)
  * a status-notifier/appindicator tray (xfce4-panel's "status notifier plugin" is fine)
  * a miracast/WFD sink to cast to

    > currently only being tested on an [hp elite x3 lapdock](https://support.hp.com/au-en/product/product-specs/hp-elite-x3-lap-dock/12088822)

  <br>

* b. **`mirablast-app` build & runtime** (package names below are arch's, other
distros may name them differently)
  - `gtk3`, `libayatana-appindicator`
  - `glib2`, `libnm`, `networkmanager`, `libpulse`
  - `gstreamer`, `gst-plugins-base`, `gst-plugins-good` (`ximagesrc`),
    `gst-plugins-bad` (`intervideosink`/`intervideosrc`, `h264parse`,
    `mpegtsmux`, `vah264enc`), `gst-plugins-ugly` (`x264enc`), `gst-libav`
    (`avenc_aac`), `gst-rtsp-server`
  - `dnsmasq` — NetworkManager's shared-mode DHCP on the p2p link
  - build-time only: `meson`, a C compiler, `pkgconf`, glib2 dev headers
    (`glib2-devel` on arch, for `glib-mkenums`)
  - optional: `firewalld`, if present the backend asks it for a WFD zone;
    absent, it just warns and carries on
  - optional: `gstreamer-vaapi`, only for the older `vaapih264enc` path —
    `vah264enc` from `gst-plugins-bad` is preferred and needs nothing extra

* c. **virtual display**
  - the `evdi` kernel module, e.g. via `evdi-dkms` on arch linux (aur)
  - its `PyEvdi` python bindings (ships w/ `evdi-dkms` from the aur)

## build

the app is a meson project rooted at `app/`:

```
cd app
meson setup build
meson compile -C build
```

that gives you `app/build/mirablast-app`, which runs straight out of the tree:

```
./build/mirablast-app
```

rebuilds are just `meson compile -C build` — it re-runs `meson setup` itself if
`meson.build` changed.

installing is optional (it puts `mirablast-app` and the `.desktop` file on the
system, default prefix `/usr/local`):

```
sudo meson install -C build
```

### the evdi helper

`bin/evdi-virtual-display` is a plain 'ol python script and is **not** installed
by meson. running from the build tree, the app finds it automatically
(`app/build/../../bin/`). otherwise put it on your `$PATH`:

```
ln -s "$PWD/bin/evdi-virtual-display" ~/.local/bin/
```

the search order is: the path configured in Settings → next to the app binary
(repo layout) → `~/.local/bin` → `$PATH`.

## usage

launch `mirablast-app` (or "Mirablast" from your application menu, if
installed). it has no main window — everything hangs off the tray icon:

| menu entry                             | action                                                                        |
|----------------------------------------|---------------------------------------------------------------------------------|
| *(top line)*                           | current status; not clickable                                                     |
| `Cast to…`                             | list discovered sinks; clicking one connects and starts streaming. brings the virtual display up first if it isn't already, so this alone is enough to go from nothing to casting |
| `Disconnect`                           | stop streaming, keep the virtual display up                                       |
| `Start` / `Stop virtual display`       | one entry that toggles: spawn the evdi helper and place the output right of primary, or tear it back down |
| `Settings…`                            | the settings dialog below                                                         |
| `Quit`                                 | tears the display down on the way out                                             |

the output appears in `xrandr`, `arandr`, and `xfce4-display-settings` as a
plain second monitor as soon as the display is started, and only commences
streaming once you connect to a sink. `evdi_open` is slow, so expect **~8-12s**
between "Start virtual display" and the output showing up.

`Quit`, `SIGINT` and `SIGTERM` all tear the display down cleanly.

### settings

`Settings…` writes `~/.config/mirablast/mirablast.conf` (a plain keyfile under a
`[mirablast]` group), which you can also edit by hand:

| key              | effect                                                                     | default     | range          |
|------------------|----------------------------------------------------------------------------|-------------|----------------|
| `resolution`     | size of the virtual display, `WxH`                                           | `1920x1080` | 64-4094 per axis |
| `refresh`        | refresh rate of the virtual display, Hz                                      | `60`        | 1-240          |
| `latency-ms`     | pipeline latency; lower is snappier, raise it if the picture stutters        | `100`       | 0-2000         |
| `gop-seconds`    | seconds between H.264 keyframes                                              | `1`         | 1-60           |
| `use-damage`     | XDamage tracking on the capture — **leave this on**, see troubleshooting     | `true`      |                |
| `auto-reconnect` | re-establish the cast when a display move resizes the desktop (~20s outage)  | `true`      |                |
| `primary`        | output to extend from, e.g. `eDP-1`; empty = whatever X calls primary        | autodetect  |                |
| `helper`         | path to `evdi-virtual-display`; empty = the search order above               | autodiscover |               |

`resolution` and `refresh` apply at the next display start; everything else at
the next connect.

it's a good idea to match the resolution to what the sink actually expects; a
mismatch makes `videoscale` rescale every frame, which often ended up doubling
CPU use in testing.

### environment variables

these are development/test hooks, there's normally no reason to set them:

| variable           | effect                                                                                  |
|--------------------|-------------------------------------------------------------------------------------------|
| `MIRABLAST_AUTO`   | `display` starts the virtual display at launch; `cast:<substring>` also connects to the first discovered sink whose name matches |
| `MIRABLAST_DUMMY`  | `1` registers the dummy sink provider (same as the `--dummy` flag), for testing without hardware |
| `GND_GOP_SECONDS`  | keyframe interval in seconds; **set by the app** from `gop-seconds`, read by the vendored factory |
| `GND_LATENCY_MS`   | pipeline latency in ms; **set by the app** from `latency-ms`                                |

## how it works

- **`bin/evdi-virtual-display`** opens an evdi card, builds a 128-byte EDID
  with one timing descriptor for the `WxH@refresh`, and
  holds the connection open until terminated. timing is
  calculated with [VESA CVT-RB v1](https://glenwing.github.io/docs/VESA-CVT-1.2.pdf) (the same formula `cvt -r` and the kernel's
  `drm_cvt_mode(reduced=true)` use). its stdout/stderr go to
  `$XDG_RUNTIME_DIR/mirablast-evdi.log`.

- **`app/src/mb-session.c`** owns the whole lifecycle: it spawns that helper,
  waits for the resulting output to appear in `xrandr`, positions it right of
  the primary, and watches for a few seconds afterwards:
    - `xfsettingsd` applies its active display profile whenever outputs change, and might sometimes switch
  the primary display off leaving only the new virtual display active. in the case that
  this happens, it should be undone automatically. (appalling scap job, i know)
  - it then builds the capture bin — `ximagesrc` cropped to the virtual
  output's geometry, `videoscale`, and an `intervideosink` — and hands the
  matching `intervideosrc` to the WFD backend as its source. when the desktop
  geometry changes underneath a live cast, it reconnects (if `auto-reconnect`
  is on).

- **`app/vendor/`** is the vendored `gnome-network-displays` backend: sink
  discovery over NetworkManager wi-fi p2p, the RTSP/WFD server, and the
  encoder pipeline. it's carried as source rather than patched at build time,
  which is what retired the three patches below. `wfd-media-factory.c` reads
  `$GND_GOP_SECONDS` and `$GND_LATENCY_MS`, which the app sets from your
  settings — that's the one seam left between the app and the vendored code.

## troubleshooting

- ### flickering / persistent cursor ghosting on the sink:
  high probability that XDamage is turned off (`use-damage=false`).
  i don't quite fully understand the reason this is, but at least i understand the fix! /s.
  uncropped fullframe reads seem to interact very poorly with the rest of the pipeline when
  damage tracking is disabled.

- ### laptop panel goes blank after starting the display:
  the app already watches for this and re-enables the primary automatically, but if it keeps
    recurring, check `xfconf-query -c displays -lv` (a saved xfce display
    profile could possibly have a stale `Active=false` for the primary output).
    fix the profile manually, or disable `/AutoEnableProfiles`.

- ### high CPU / choppy video:
    check the virtual display's resolution matches what the sink negotiated (see settings above):
    > *a mismatch forces `videoscale` to rescale every frame*

- ### `evdi-virtual-display exited early`:
  check `$XDG_RUNTIME_DIR/mirablast-evdi.log`. two usual causes: starting again
  too soon after a stop (the device is still held, wait ~15s), or
  `sku_area_limit`/`--area-limit` rejecting the requested pixel area — the
  default limit is 3840x2160 (four-kay).

- ### no tray icon:
  the app is tray-resident and holds itself open with no window, so if your
  panel has no status-notifier/appindicator support there's nothing to click.
  on xfce, add the "Status Notifier Plugin" to the panel.

## legacy: the script + patch workflow

before the app existed, mirablast was `bin/miracast-extend` driving a **patched
upstream `gnd`**. that path still works and the pieces are still in the repo,
but it isn't where development happens anymore:

- `bin/miracast-extend` — `run` / `on` / `off` / `status`, e.g.
  `miracast-extend run 1920x1080`. brings the virtual display up, launches
  `gnome-network-displays` with `GND_X11_REGION` set to the output's geometry,
  and tears everything down on exit. tuned with the `PRIMARY`, `REFRESH`,
  `HELPER`, `LOG`, `KEEP_RUNNING`, `GND_X11_DAMAGE`, `GND_GOP_SECONDS` and
  `GND_LATENCY_MS` environment variables.
- `packaging/gnd-x11-region-crop.patch` — adds `$GND_X11_REGION` handling to
  gnd's X11 fallback path (`src/app/nd-window.c`), cropping the `ximagesrc`
  capture instead of grabbing the whole root window.
- `packaging/gnd-gop-seconds.patch` — makes the H.264 keyframe interval
  configurable via `$GND_GOP_SECONDS` (upstream hardcoded at 10 seconds).
- `packaging/gnd-latency.patch` — stops charging every session a fixed 500ms of
  pipeline latency. upstream applies that unconditionally,
  > * there is a comment stating it's actually an `openh264enc` workaround for
  >   latency spikes occuring due to scene changes
  > * so, sessions using a hardware encoder (`vah264enc`) were being nerfed for no reason
      (*at least, i think they are according to my not-so-scientific testing*)
- `packaging/PKGBUILD` builds patched gnd as an arch package.

to use it: clone upstream gnd, `patch -Np1 -i` the three patches, `meson setup
build --prefix=/usr/local && meson compile -C build && sudo meson install -C
build`, then symlink `bin/*` onto your `$PATH`. the app supersedes all of this —
the same three changes now live in `app/vendor/`.

## license

per directory, see `LICENSE`:

> * `app/` is GPL-3.0-or-later (see `app/LICENSE`); it incorporates source code from
>   `gnome-network-displays`, which is GPL-3.0-or-later
> * everything else (`bin/`, `packaging/`) is MIT
> * the bundled `packaging/PKGBUILD` is a derivative of the `gnome-network-displays-git` [AUR package](https://aur.archlinux.org/packages/gnome-network-displays-git) (maintained by yochananmarqos) with the patches in this repo applied via its `prepare()`
