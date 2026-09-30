#!/bin/bash
# TCMT-M macOS 发布脚本：构建 Release → 打包 → 签名 → GitHub Release。
# 用法: tools/release-mac.sh [版本号]   （默认取根 CMakeLists 里的版本）
# 前置: ~/.tcmt/updater/update_key.pem（Ed25519 私钥，勿入库）、gh CLI
#
# 这是**唯一**能进 releases 的路径 —— CI 每次推 dev 出的测试包走 workflow artifact，
# 不发 release（见 .github/workflows/build.yml）。更新器只认 releases。
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# 版本串 = 阶段标记 + 数字，两段都在根 CMakeLists 里，跟 CMake 自己用的是同一处：
#   set(TCMT_VERSION_DISPLAY "ALPHA_${PROJECT_VERSION}")   → ALPHA_
#   project(TCMT-M VERSION 0.0.79)                         → 0.0.79
# 不带 git 短 SHA：那是给「同一个版本号的不同构建」区分的，发布包跟版本对齐就够了。
VERSION_PREFIX="$(sed -n 's/^set(TCMT_VERSION_DISPLAY "\([A-Za-z_]*\)\${PROJECT_VERSION}.*/\1/p' CMakeLists.txt | head -1)"
VERSION_NUMBER="$(sed -n '/^project(TCMT-M/,/^)/p' CMakeLists.txt | sed -n 's/^ *VERSION \([0-9.]*\).*/\1/p' | head -1)"
VERSION="${1:-${VERSION_PREFIX}${VERSION_NUMBER}}"
if [ -z "$VERSION" ]; then echo "无法确定版本号，请显式传入"; exit 1; fi

# tag 只放数字（v0.0.79）—— 它是标识符，ALPHA_ 那个阶段标记是给人看的，
# 放在 release 标题、manifest 和文件名里就够了。更新器比的是 manifest 里的 version，
# tag 长什么样不影响它。
TAG="v${VERSION#${VERSION_PREFIX}}"

KEY="$HOME/.tcmt/updater/update_key.pem"
if [ ! -f "$KEY" ]; then echo "缺少签名私钥: $KEY"; exit 1; fi

echo "==> 构建 Release ($VERSION)"
cmake --build build -j8 >/dev/null

BIN="build/src/TCMT-M"

# 构建出来的二进制里那个版本串必须和要发的版本对得上 —— 不然发出去的是一个
# 自称别的版本的包，更新器拿到会拿去跟自己比，结果没人说得清。这条链从来没跑通过，
# 加一道便宜的检查比事后查省事。
BUILT_VERSION="$(strings "$BIN" | grep -m1 "^${VERSION}-" || true)"
if [ -z "$BUILT_VERSION" ]; then
  echo "构建产物里没有以 $VERSION 开头的版本串 —— 是不是没重新 configure？"
  echo "（CMAKE 缓存住的是上一次的 TCMT_VERSION_DISPLAY）"
  exit 1
fi
echo "    二进制自报版本: $BUILT_VERSION"

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

echo "==> 创建 GitHub Release $TAG（版本 $VERSION）"
gh release create "$TAG" --repo dongge0210/TCMT-M-client --title "TCMT-M $VERSION" --notes "自动更新包" "$TARBALL" manifest.json

# 上传签名文件
printf '%s' "$SIG_B64" > manifest.json.sig
gh release upload "$TAG" manifest.json.sig --repo dongge0210/TCMT-M-client --clobber

rm -f manifest.json manifest.json.sig
echo "==> 完成: $TAG ($VERSION)"
