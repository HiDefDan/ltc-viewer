#!/usr/bin/env bash
# verify_graphics.sh — Remote inspection tool for CM5 LTC Viewer

TARGET_BIN="/usr/local/bin/ltc-timecode"
PROC_NAME="ltc-timecode"

printf "=== 1. SHARED LIBRARY LINKAGE ===\n"
if [ -f "$TARGET_BIN" ]; then
    ldd "$TARGET_BIN" | grep -E "gbm|EGL|GLES|drm"
else
    printf "[-] Target binary not found at %s. Checking local build...\n" "$TARGET_BIN"
    ldd ./ltc-timecode 2>/dev/null | grep -E "gbm|EGL|GLES|drm" || printf "[-] No compiled binary found.\n"
fi

printf "\n=== 2. KERNEL DRI DRIVER STATUS ===\n"
dmesg | grep -iE "dri|v3d|vc4" | tail -n 10

printf "\n=== 3. ACTIVE DRM CONNECTORS & STATUS ===\n"
# Count how many physical displays the kernel actually sees connected
for conn in /sys/class/drm/card1-*; do
    if [ -d "$conn" ]; then
        status=$(cat "$conn/status" 2>/dev/null)
        id=$(cat "$conn/connector_id" 2>/dev/null)
        printf "  %s (ID: %s) -> %s\n" "$(basename "$conn")" "$id" "$status"
    fi
done

printf "\n=== 4. OPEN GRAPHICS FILE DESCRIPTORS ===\n"
PID=$(pgrep -x "$PROC_NAME" | head -n 1)
if [ -n "$PID" ]; then
    printf "[+] Found running process %s (PID: %s)\n" "$PROC_NAME" "$PID"
    sudo lsof -p "$PID" | grep -E "card|render"
else
    printf "[-] Process %s is not running. Checking system-wide handles:\n" "$PROC_NAME"
    sudo lsof /dev/dri/card* /dev/dri/renderD* 2>/dev/null || printf "  No processes holding DRI handles.\n"
fi

printf "\n=== 5. GPU MEMORY ALLOCATIONS (V3D GEM OBJECTS) ===\n"
if [ -f /sys/kernel/debug/dri/1/v3d_gem_objects ]; then
    sudo cat /sys/kernel/debug/dri/1/v3d_gem_objects
elif [ -f /sys/kernel/debug/dri/0/v3d_gem_objects ]; then
    sudo cat /sys/kernel/debug/dri/0/v3d_gem_objects
else
    printf "[-] DebugFS node for v3d_gem_objects not found. Is vc4-kms-v3d active?\n"
fi

