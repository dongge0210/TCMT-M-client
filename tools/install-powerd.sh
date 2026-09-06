#!/bin/bash
# install-powerd.sh — install tcmt-powerd as a root LaunchDaemon.
#
# tcmt-powerd reads IOReport energy as root (macOS 27+ restricts energy
# channels to privileged processes) and publishes CPU/GPU/ANE power (mW) to
# shared memory "tcmt-power"; the client is PowerMonitor::ReadShmPower.
#
# Usage:  sudo tools/install-powerd.sh
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "需要 root: sudo $0" >&2
    exit 1
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/build/src/tcmt-powerd"
DST=/usr/local/libexec/tcmt-powerd
PLIST=/Library/LaunchDaemons/com.tcmt.powerd.plist
TPL="$ROOT/src/helper/powerd/com.tcmt.powerd.plist"

if [[ ! -x "$SRC" ]]; then
    echo "找不到 $SRC — 先构建: cmake --build $ROOT/build -j8" >&2
    exit 1
fi

echo "→ 安装 $SRC → $DST"
install -m 755 "$SRC" "$DST"
echo "→ 安装 launchd plist → $PLIST"
install -m 644 "$TPL" "$PLIST"

launchctl bootout system/com.tcmt.powerd 2>/dev/null || true
# launchd can race an immediate rebootstrap after bootout (EIO); back off.
sleep 2
launchctl bootstrap system "$PLIST"
sleep 1

echo "→ 状态:"
launchctl print system/com.tcmt.powerd 2>&1 | head -12 || true
echo "OK — tcmt-powerd 已安装。卸载: sudo tools/uninstall-powerd.sh"
