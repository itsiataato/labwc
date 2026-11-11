#!/usr/bin/env bash
# LabWC + monowl-derived TopBar + WiFi manager.
# Compositor = system labwc (unchanged). TopBar + wifi = monopanel (layer-shell client).
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PANEL="$SCRIPT_DIR/../panel/monopanel"

# Build panel if missing
if [ ! -x "$PANEL" ]; then
  echo "building panel..."
  make -C "$SCRIPT_DIR/../panel" >/dev/null 2>&1 || { echo "panel build failed"; exit 1; }
fi

# Build full GTK wifi manager if missing (panel falls back to the fuzzel
# menu when this is absent; the GTK manager is the intended one).
WIFI_GUI="$SCRIPT_DIR/../tools/wifi-gui/wifi-gui"
if [ ! -x "$WIFI_GUI" ]; then
  echo "building wifi-gui..."
  make -C "$SCRIPT_DIR/../tools/wifi-gui" >/dev/null 2>&1 || echo "wifi-gui build failed (panel will use fuzzel fallback)"
fi

export XDG_CURRENT_DESKTOP=labwc
export MOZ_ENABLE_WAYLAND=1 GDK_BACKEND=wayland QT_QPA_PLATFORM=wayland
export ELECTRON_OZONE_PLATFORM_HINT=auto

# Start top bar in background (layer-shell, survives compositor)
"$PANEL" >/tmp/opencode/monopanel.log 2>&1 &
PANEL_PID=$!
echo "monopanel pid=$PANEL_PID" >> /tmp/opencode/labwc-topbar.log

exec labwc
