#!/usr/bin/env bash
# Install the PipeWire filter-chain preset for the anechoic suppressor.
set -euo pipefail

PRESET="$(cd "$(dirname "$0")" && pwd)/99-anechoic-ladspa.conf"
DEST="${XDG_CONFIG_HOME:-$HOME/.config}/pipewire/pipewire.conf.d"

mkdir -p "$DEST"
cp -f "$PRESET" "$DEST/99-anechoic-ladspa.conf"

echo "Installed preset to $DEST"
echo ""
echo "Validate with:  pw-config supported '$DEST/99-anechoic-ladspa.conf'"
echo "Restart with:   systemctl --user restart pipewire pipewire-pulse"
echo ""
echo "Check the virtual source with:  pactl list short sources | grep anechoic"