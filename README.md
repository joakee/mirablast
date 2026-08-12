# mirablast

Turns a Miracast / Wi-Fi Display sink into a real **extended** monitor on
XFCE/X11, instead of a mirror of your primary display.

`gnome-network-displays` (gnd) already does Miracast well, but on a
non-GNOME/X11 session it falls back to capturing the whole root window with a
bare `ximagesrc`, because the ScreenCast portal it normally uses for monitor
selection requires Mutter. That fallback can only ever mirror.

mirablast works around that with two independent pieces:

1. A small patch set for gnd that lets it crop its X11 capture to an
   arbitrary region (`$GND_X11_REGION`), plus two latency-related patches.
2. `evdi-virtual-display`, which attaches a synthesised EDID to an
   [evdi](https://github.com/DisplayLink/evdi) device, giving you a **real**
   RandR output for any resolution/refresh rate you ask for. `xrandr
   --setmonitor` was tried first and doesn't work for this: xfwm4 and the
   XFCE/arandr tooling compute usable desktop space from RandR 1.2 *outputs*,
   not RandR 1.5 *monitors*, and clamp windows to the physical panel edge if
   you use it.

`miracast-extend` glues the two together: it brings up the virtual output
positioned to the right of your primary display, then launches the patched
gnd pointed at just that region.

## Requirements

- XFCE on X11 (developed and tested there; other RandR/xrandr-based X11
  window managers likely work, but the display-profile handling below is
  XFCE-specific)
- The `evdi` kernel module, e.g. via `evdi-dkms`, and its `PyEvdi` Python
  bindings (ships alongside `evdi-dkms` on Arch)
- A Miracast/WFD sink to cast to
- For the packaged build: an Arch-family distro with `makepkg`. Elsewhere,
  apply `packaging/*.patch` to a `gnome-network-displays` checkout yourself
  and build with `meson` -- the patches are plain unified diffs against
  upstream `src/`, nothing Arch-specific is in them.

## Install

```
git clone <this repo> ~/Projects/mirablast
cd ~/Projects/mirablast
./install.sh
```

This symlinks `bin/evdi-virtual-display` and `bin/miracast-extend` into
`~/.local/bin`, and on Arch, builds and installs the patched
`gnome-network-displays-git` package via `makepkg -si` (which will prompt for
your password to install it).

If `evdi`/`PyEvdi` aren't set up yet, `install.sh` warns but doesn't fail --
get those from your distro's `evdi-dkms` package (or build evdi from source)
before trying to run anything.

## Usage

```
miracast-extend run 1920x1080      # bring up the virtual display, launch gnd, tear down on exit
miracast-extend on 1920x1080       # just bring up the virtual display
miracast-extend off                # tear it down
miracast-extend status             # show current outputs and the evdi helper's state
```

Then connect to your Miracast sink from the gnd window as usual; it will
appear as a plain second output in `xrandr`, arandr, and
`xfce4-display-settings` immediately, and only starts streaming once you
connect.

**Match the virtual display's resolution to what the sink actually
negotiates.** A mismatch makes gnd's `videoscale` rescale every frame, which
roughly doubled measured CPU use in testing. If you don't know the sink's
native resolution, start with `run 1920x1080` (most Miracast sinks land on
1080p) and check `miracast-extend status` / the gnd log after connecting.

### Environment variables

| Variable       | Effect                                                              | Default          |
|----------------|----------------------------------------------------------------------|-------------------|
| `PRIMARY`      | Output to extend from                                                | autodetected      |
| `REFRESH`      | Refresh rate (Hz) for the virtual display                            | `60`              |
| `HELPER`       | Path to `evdi-virtual-display`                                       | next to this script |
| `LOG`          | Where the evdi helper's stdout/stderr goes                           | `$XDG_RUNTIME_DIR/evdi-virtual-display.log` |
| `KEEP_RUNNING` | If `1`, refuse to kill an already-running gnd instead of restarting it | unset           |
| `GND_X11_DAMAGE` | Set to `0` to disable XDamage on the cropped capture (see below, don't) | unset (XDamage on) |
| `GND_GOP_SECONDS` | Keyframe interval in seconds, 1-60                                 | `1`               |
| `GND_LATENCY_MS`  | Pipeline latency in ms, 0-2000, overrides the encoder-based default | `500` for openh264, `100` otherwise |

`GND_X11_REGION` is computed and set by `miracast-extend` itself; there's
normally no reason to set it directly.

## How it works

- **`bin/evdi-virtual-display`** opens an evdi card, builds a 128-byte EDID
  with one detailed timing descriptor for the requested `WxH@refresh`, and
  holds the connection open until it receives SIGTERM. The timing is
  computed with VESA CVT-RB v1 (the same formula `cvt -r` and the kernel's
  `drm_cvt_mode(reduced=true)` use) rather than looked up from a table, so
  any resolution works, not just a hand-picked set.
- **`bin/miracast-extend`** starts that helper, waits for the resulting
  output to appear in `xrandr`, positions it to the right of `$PRIMARY`, and
  watches for a few seconds afterwards: xfsettingsd applies its active
  display profile whenever outputs change, and has been observed switching
  the *primary* panel off and leaving only the new virtual display active. If
  that happens, it's undone automatically. It then launches
  `gnome-network-displays` with `GND_X11_REGION` set to the virtual output's
  geometry, and tears everything down on exit.
- **`packaging/gnd-x11-region-crop.patch`** adds `$GND_X11_REGION` handling
  to gnd's X11 fallback path (`src/app/nd-window.c`), cropping the
  `ximagesrc` capture instead of grabbing the whole root window.
- **`packaging/gnd-gop-seconds.patch`** makes the H.264 keyframe interval
  configurable via `$GND_GOP_SECONDS`; upstream hardcodes 10 seconds.
- **`packaging/gnd-latency.patch`** stops charging every session a fixed
  500ms of pipeline latency. Upstream applies that unconditionally with a
  comment explaining it's specifically an `openh264enc` workaround for
  post-scene-change latency spikes; sessions using a hardware encoder
  (`vah264enc`) were paying it for nothing.

## Troubleshooting

- **Flickering / a cursor trail on the sink.** Almost certainly XDamage
  turned off (`GND_X11_DAMAGE=0`). Leave it at its default (on) -- an A/B
  test during development showed a damage-off crop of an *identical* region
  to an uncropped, damage-on capture flickering badly while the latter was
  clean. The reason isn't fully understood, but the fix reliably is.
  Un-cropped full-frame reads seem to interact badly with the rest of the
  pipeline when damage tracking is disabled.
- **Laptop panel goes blank after `on`/`run`.** `miracast-extend` already
  watches for this and re-enables `$PRIMARY` automatically, but if it keeps
  recurring, check `xfconf-query -c displays -lv` -- a saved XFCE display
  profile may have a stale `Active=false` for your primary output. Fix the
  profile directly, or disable `/AutoEnableProfiles`.
- **High CPU / choppy video.** Check the virtual display's resolution
  matches what the sink negotiated (see Usage above) -- a mismatch forces
  `videoscale` to rescale every frame.
- **`evdi output never came up`.** Check `$LOG` (default
  `$XDG_RUNTIME_DIR/evdi-virtual-display.log`).  A common cause is
  `sku_area_limit`/`--area-limit` rejecting the requested mode's pixel area;
  the default limit is 4K (3840x2160).

## License

MIT, see `LICENSE`. The bundled `packaging/PKGBUILD` is a derivative of the
`gnome-network-displays-git` AUR package (maintained by Mark Wagie) with the
patches in this repo applied via its `prepare()`; `gnome-network-displays`
itself is GPL-3.0-or-later.
