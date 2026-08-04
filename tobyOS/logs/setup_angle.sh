#!/bin/bash
# Slice 96b (tier 3 Phase 0): stage ANGLE DLLs so QEMU's virglrenderer can
# initialize on this Windows host.
#
# QEMU 10.2's Windows build ships NO EGL/GLES libraries: `-display
# egl-headless` SEGFAULTS outright, and `sdl,gl=on` falls back to WGL,
# where the vendor ICD does not load in QEMU's process (GDI OpenGL 1.1 ->
# "Unable to create OpenGL context >= 3.0" -> virgl init fails) -- even on
# this RTX 5090 + Radeon iGPU box, in a Console (non-RDP) session.
#
# The fix measured in Phase 0: put Chromium-flavour ANGLE DLLs
# (libEGL.dll, libGLESv2.dll, d3dcompiler_47.dll) on PATH -- Windows' DLL
# search reaches PATH after the app dir and System32, so QEMU picks them
# up WITHOUT touching Program Files. With them, egl-headless initializes,
# virglrenderer comes up (ANGLE -> D3D11 -> real GPU), and the guest
# negotiates virgl=yes with a live control queue.
#
# Source: the local Edge install (redistributable ANGLE, always present
# and auto-updated on Windows). Staged into logs/angle/ (gitignored).
#
# Usage:
#   bash logs/setup_angle.sh
#   PATH="$(pwd)/logs/angle:$PATH" <qemu invocation with
#       -display egl-headless -device virtio-gpu-gl-pci>
#
# Known Phase-1 item: with virgl negotiated, SET_SCANOUT of a
# RESOURCE_CREATE_2D resource is rejected (resp 0x1203) -- the 2D-compat
# scanout path differs under virgl; the guest driver currently declines
# the device (probe rc=-13). See the ledger, slice 96b.
set -e
cd "$(dirname "$0")/.."
EDGE_APP="/c/Program Files (x86)/Microsoft/Edge/Application"
NEED="libEGL.dll libGLESv2.dll d3dcompiler_47.dll"
# Slice 106: pick the newest version that ACTUALLY CARRIES the DLLs, not just
# the newest version. Edge updated to 151.0.4129.59 on this box and that
# directory ships no ANGLE at all (150.0.4078.99 still does), so the old
# `sort -V | tail -1` picked a version where every cp failed -- and with
# `set -e` the whole GPU run then died at staging time.
VER=""
for v in $(ls "$EDGE_APP" | grep -E '^[0-9]+\.' | sort -V -r); do
    ok=1
    for f in $NEED; do [ -f "$EDGE_APP/$v/$f" ] || ok=0; done
    [ "$ok" = 1 ] && { VER="$v"; break; }
done
if [ -z "$VER" ]; then
    echo "no Edge version under $EDGE_APP carries ANGLE ($NEED)."
    echo "installed versions:"; ls "$EDGE_APP" | grep -E '^[0-9]+\.'
    echo "Any Chromium-flavour ANGLE will do -- point this at one."
    exit 1
fi
mkdir -p logs/angle
for f in $NEED; do
    cp "$EDGE_APP/$VER/$f" logs/angle/
done
echo "staged ANGLE from Edge $VER -> logs/angle/"
ls -la logs/angle/
