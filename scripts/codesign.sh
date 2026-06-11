#!/bin/bash
# Conditionally sign the app bundle — skips if identity is not in keychain (CI-safe)
set -e

IDENTITY="6AD7D5B2E77AB209D2A6D8B038F297A22EAC8179"
APP_DIR="$1"
ENTITLEMENTS="$2"

if security find-identity -v -p codesigning | grep -q "$IDENTITY"; then
    # Sign SMJobBless helper
    codesign --force --sign "$IDENTITY" \
        --options runtime \
        --identifier "com.tcmt.sensorhelper" \
        "$APP_DIR/TCMT-M.app/Contents/Library/LaunchServices/com.tcmt.sensorhelper"
    # Sign app bundle
    codesign --force --sign "$IDENTITY" \
        --options runtime \
        --entitlements "$ENTITLEMENTS" \
        "$APP_DIR/TCMT-M.app"
    echo "Signed TCMT-M.app with identity $IDENTITY"
else
    echo "Signing identity not in keychain, skipping codesign"
fi
