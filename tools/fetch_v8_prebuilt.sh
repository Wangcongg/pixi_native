#!/usr/bin/env bash
# fetch_v8_prebuilt.sh — 拉取 Cocos 维护的预编译 V8(cocos-engine-external 仓库)供 Android 宿主链接。
#
# 只稀疏检出 Android arm64-v8a / x86_64 的 libv8_monolith.a(V8 11.6.189.22 的 16 KB 修复版,含 libplatform 与内嵌 snapshot,
# 64 位 ABI 带指针压缩、无 sandbox)与配套头文件,落到 third_party/v8-prebuilt(git 忽略)。
# CMake(android/app/src/main/cpp/CMakeLists.txt,PIXINATIVE_JS_ENGINE=v8)在目录缺失时自动调用本脚本;也可手动执行。
#
#   tools/fetch_v8_prebuilt.sh [目标目录]
#
# 版本钉在 commit(cocos-engine-external 的 tag v3.8.8-1,2025-09-09,#501 Android 16 KB 修复);升级时改 COMMIT 并重新核对
# tools/v8_prebuilt.sha256(存在时校验每个静态库的 sha256,不匹配即失败)。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${1:-$ROOT/third_party/v8-prebuilt}"
REPO="https://github.com/cocos/cocos-engine-external.git"
COMMIT="0be13f7e2e9034d123744935ea73b410b2085b15"
ABIS=(arm64-v8a x86_64)
PATHS=(/versions.txt /LICENSE /android/CMakeLists.txt)
for abi in "${ABIS[@]}"; do PATHS+=("/android/$abi/v8" "/android/$abi/include/v8"); done
SUMS="$ROOT/tools/v8_prebuilt.sha256"

complete() {
    [ -f "$DEST/.pinned" ] && [ "$(cat "$DEST/.pinned")" = "$COMMIT" ] || return 1
    for abi in "${ABIS[@]}"; do [ -s "$DEST/android/$abi/v8/libv8_monolith.a" ] || return 1; done
    return 0
}
verify() {
    [ -f "$SUMS" ] || { echo "[v8-prebuilt] 无 tools/v8_prebuilt.sha256,跳过校验(首次拉取后把下面的 sha256 记进去)"; return 0; }
    (cd "$DEST" && shasum -a 256 -c "$SUMS") || { echo "[v8-prebuilt] sha256 校验失败:$DEST 与 tools/v8_prebuilt.sha256 不一致" >&2; return 1; }
}

if complete; then
    echo "[v8-prebuilt] 已就绪 $DEST ($COMMIT)"
    verify
    exit 0
fi

echo "[v8-prebuilt] 拉取 $REPO@$COMMIT → $DEST(稀疏检出,约 130 MiB)"
rm -rf "$DEST"
mkdir -p "$DEST"
git -C "$DEST" init -q
git -C "$DEST" remote add origin "$REPO"
git -C "$DEST" sparse-checkout set --no-cone "${PATHS[@]}"
git -C "$DEST" fetch -q --depth 1 --filter=blob:none origin "$COMMIT"
git -C "$DEST" checkout -q FETCH_HEAD
echo "$COMMIT" > "$DEST/.pinned"

echo "[v8-prebuilt] versions.txt 里的 v8:"
awk '/^v8/{f=1;next} f&&/^[^ ]/{exit} f' "$DEST/versions.txt" | sed 's/^/    /'
for abi in "${ABIS[@]}"; do
    lib="$DEST/android/$abi/v8/libv8_monolith.a"
    [ -s "$lib" ] || { echo "[v8-prebuilt] 缺少 $lib" >&2; exit 1; }
done
echo "[v8-prebuilt] sha256(写入 tools/v8_prebuilt.sha256 可钉死):"
(cd "$DEST" && shasum -a 256 android/*/v8/libv8_monolith.a)
verify
