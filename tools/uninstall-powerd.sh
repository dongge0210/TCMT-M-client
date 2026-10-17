#!/bin/bash
# uninstall-powerd.sh — remove tcmt-powerd LaunchDaemon and its files.
#
# Usage:  sudo tools/uninstall-powerd.sh
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "需要 root: sudo $0" >&2
    exit 1
fi

launchctl bootout system/com.tcmt.powerd 2>/dev/null || true
rm -f /Library/LaunchDaemons/com.tcmt.powerd.plist
rm -f /usr/local/libexec/tcmt-powerd
rm -f /var/log/tcmt-powerd.log /var/log/tcmt-powerd.out.log /var/log/tcmt-powerd.err.log
echo "OK — tcmt-powerd 已卸载"
