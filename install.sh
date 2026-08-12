#!/usr/bin/env bash
# Installs mirablast for the current user:
#   1. symlinks bin/evdi-virtual-display and bin/miracast-extend into ~/.local/bin
#   2. on Arch, builds and installs the patched gnome-network-displays package
#
# This does not install evdi or PyEvdi -- there is no general-distro package for
# either. See README.md for how to get them.

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_DIR="${HOME}/.local/bin"

echo "==> checking prerequisites"
command -v python3 >/dev/null || { echo "python3 not found" >&2; exit 1; }
command -v xrandr >/dev/null || { echo "xrandr not found (needs an X11 session)" >&2; exit 1; }
python3 -c "import PyEvdi" 2>/dev/null ||
  echo "warning: PyEvdi is not importable yet -- see README.md before running miracast-extend" >&2
[[ -e /dev/dri ]] || echo "warning: /dev/dri does not exist -- is a GPU driver loaded?" >&2

mkdir -p "$BIN_DIR"
for f in evdi-virtual-display miracast-extend; do
  ln -sf "$REPO_DIR/bin/$f" "$BIN_DIR/$f"
  echo "linked $BIN_DIR/$f -> $REPO_DIR/bin/$f"
done

if command -v makepkg >/dev/null; then
  echo "==> building patched gnome-network-displays (compiles from source, may take a while)"
  (cd "$REPO_DIR/packaging" && makepkg -si)
else
  echo "makepkg not found -- this isn't an Arch-family system." >&2
  echo "Apply packaging/*.patch to a gnome-network-displays checkout and build" >&2
  echo "with meson yourself; see README.md for what each patch does." >&2
fi

case ":$PATH:" in
  *":$BIN_DIR:"*) ;;
  *) echo "note: $BIN_DIR is not on your PATH" >&2 ;;
esac

echo "==> done. try: miracast-extend run 1920x1080"
