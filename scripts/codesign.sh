#!/bin/bash
# Conditionally sign the app bundle — skips if identity is not in keychain (CI-safe)
set -e

IDENTITY="6AD7D5B2E77AB209D2A6D8B038F297A22EAC8179"
APP_DIR="$1"
ENTITLEMENTS="$2"

# The pinned SHA goes stale whenever the development certificate is reissued;
# the build then quietly produced an ad-hoc bundle, and TCC keys those by
# cdhash, so Location Services was re-prompted after every rebuild. Fall back
# to whatever Apple Development identity is actually in the keychain.
if ! security find-identity -v -p codesigning | grep -q "$IDENTITY"; then
    IDENTITY=$(security find-identity -v -p codesigning \
        | sed -n 's/.*"\(Apple Development:.*\)"/\1/p' | head -1)
fi

if [ -n "$IDENTITY" ] && security find-identity -v -p codesigning | grep -q "$IDENTITY"; then
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
