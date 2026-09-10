#!/bin/bash
# TCMT-M macOS 发布脚本：构建 Release → 打包 → 签名 → GitHub Release。
# 用法: tools/release-mac.sh [版本号]   （默认取 CMake PROJECT_VERSION）
# 前置: ~/.tcmt/updater/update_key.pem（Ed25519 私钥，勿入库）、gh CLI
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

VERSION="${1:-$(grep -m1 -A3 'project(TCMT-M' CMakeLists.txt | grep -o 'VERSION [0-9.]*' | awk '{print $2}')}"
if [ -z "$VERSION" ]; then echo "无法确定版本号，请显式传入"; exit 1; fi

KEY="$HOME/.tcmt/updater/update_key.pem"
if [ ! -f "$KEY" ]; then echo "缺少签名私钥: $KEY"; exit 1; fi

echo "==> 构建 Release ($VERSION)"
cmake --build build -j8 >/dev/null

BIN="build/src/TCMT-M"
TARBALL="TCMT-M-$VERSION.tar.gz"
rm -f "$TARBALL"
tar -czf "$TARBALL" -C "$(dirname "$BIN")" "$(basename "$BIN")"

SHA=$(shasum -a 256 "$TARBALL" | awk '{print $1}')

# manifest.json: 客户端先下载并验签，再按 asset 文件名从 release 资产里找下载地址
cat > manifest.json <<EOF
{"version":"$VERSION","asset":"$TARBALL","sha256":"$SHA"}
EOF

openssl pkeyutl -sign -rawin -inkey "$KEY" -in manifest.json -out manifest.sig
SIG_B64=$(base64 < manifest.sig | tr -d '\n')
rm -f manifest.sig

echo "==> 创建 GitHub Release v$VERSION"
# 先建 release（无 asset），拿到上传 URL 后填回 manifest 再重新签名上传
gh release create "v$VERSION" --repo dongge0210/TCMT-M-client --title "TCMT-M $VERSION" --notes "自动更新包" "$TARBALL" manifest.json

# 上传签名文件
printf '%s' "$SIG_B64" > manifest.json.sig
gh release upload "v$VERSION" manifest.json.sig --repo dongge0210/TCMT-M-client --clobber

rm -f manifest.json manifest.json.sig
echo "==> 完成: v$VERSION"
