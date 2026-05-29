#!/bin/bash
# install_helper.sh — install tcmt_sensor_helper as a launchd daemon
# One-time sudo run:  sudo ./scripts/install_helper.sh

set -euo pipefail

HELPER_NAME="com.tcmt.sensorhelper"
HELPER_BIN="/Library/PrivilegedHelperTools/${HELPER_NAME}"
PLIST_PATH="/Library/LaunchDaemons/${HELPER_NAME}.plist"
LOG_PATH="/tmp/tcmt-sensorhelper.log"

# Resolve source binary relative to this script (repo root)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

if [ "$EUID" -ne 0 ]; then
    echo "ERROR: This script must be run as root (sudo)."
    echo "Usage: sudo ${SCRIPT_DIR}/install_helper.sh"
    exit 1
fi

# Unload any existing instance
echo "→ Removing previous instance..."
launchctl bootout system/"${HELPER_NAME}" 2>/dev/null || true
rm -f "${HELPER_BIN}" "${PLIST_PATH}"

# Copy helper binary
echo "→ Copying helper binary..."
cp "${REPO_DIR}/build/src/tcmt_sensor_helper" "${HELPER_BIN}"
chown root:wheel "${HELPER_BIN}"
chmod 755 "${HELPER_BIN}"

# NOTE: Binary must be signed BEFORE running this script (root doesn't have
# keychain access). Run as user:
#   codesign -f -s "Apple Development: ..." "${HELPER_BIN}"
# The CMake build already signs it, so this is usually already done.

# Create launchd plist (no MachServices — we use POSIX SHM, not XPC)
echo "→ Creating launchd plist..."
cat > "${PLIST_PATH}" << 'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
 "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.tcmt.sensorhelper</string>
    <key>KeepAlive</key>
    <true/>
    <key>RunAtLoad</key>
    <true/>
    <key>StandardOutPath</key>
    <string>/tmp/tcmt-sensorhelper.log</string>
    <key>StandardErrorPath</key>
    <string>/tmp/tcmt-sensorhelper.log</string>
</dict>
</plist>
PLIST
chown root:wheel "${PLIST_PATH}"
chmod 644 "${PLIST_PATH}"

# Load with launchd
echo "→ Loading daemon..."
launchctl bootstrap system "${PLIST_PATH}"

# Verify
sleep 1
PID=$(launchctl list "${HELPER_NAME}" | awk '{print $1}')
if [ "$PID" != "-" ] && [ "$PID" != "" ]; then
    echo "✓ ${HELPER_NAME} running (PID ${PID})"
    echo "  Log: ${LOG_PATH}"
else
    echo "✗ ${HELPER_NAME} failed to start — check ${LOG_PATH}"
    cat "${LOG_PATH}" 2>/dev/null || echo "(log empty)"
    exit 1
fi
