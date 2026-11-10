#!/usr/bin/env bash
# hvnetpp 构建脚本（仅 Linux/WSL 下运行）
#
# 用法: ./script/build.sh [选项]
#   -t, --type <Debug|Release|RelWithDebInfo>  构建类型（默认 Release）
#   -c, --clean          删除构建目录后全量重建
#   -j, --jobs <N>       并行编译数（默认 nproc）
#       --no-examples    不编译 examples/
#       --ubsan          UBSan trap 模式 + _GLIBCXX_ASSERTIONS（无需 libubsan 运行时）
#       --asan           AddressSanitizer + UBSan（需要系统装有 libasan）
#       --tsan           ThreadSanitizer（需要 libtsan；不能与 --asan 同用）
#       --test           构建后运行全部自测（unpack/loop/net_selftest）
#   -h, --help           显示本帮助
#
# 示例:
#   ./script/build.sh                     # Release 构建
#   ./script/build.sh -t Debug --test     # Debug 构建并跑自测
#   ./script/build.sh -c --asan           # 干净重建 ASan 版本
#   ./script/build.sh --tsan --test       # TSan 版本并跑自测（并发改动必跑）

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

BUILD_TYPE="Release"
BUILD_DIR="$PROJECT_ROOT/build"
CLEAN=0
JOBS="$(nproc 2>/dev/null || echo 4)"
BUILD_EXAMPLES=ON
RUN_TEST=0
EXTRA_CXX_FLAGS=""
SANITIZER=""

usage() {
    sed -n '2,19p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

err() {
    echo "[build.sh] 错误: $*" >&2
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        -t|--type)
            [ $# -ge 2 ] || err "$1 需要一个参数"
            BUILD_TYPE="$2"; shift 2 ;;
        -c|--clean)
            CLEAN=1; shift ;;
        -j|--jobs)
            [ $# -ge 2 ] || err "$1 需要一个参数"
            JOBS="$2"; shift 2 ;;
        --no-examples)
            BUILD_EXAMPLES=OFF; shift ;;
        --ubsan)
            SANITIZER="ubsan"; shift ;;
        --asan)
            SANITIZER="asan"; shift ;;
        --tsan)
            SANITIZER="tsan"; shift ;;
        --test)
            RUN_TEST=1; shift ;;
        -h|--help)
            usage; exit 0 ;;
        *)
            usage; err "未知选项: $1" ;;
    esac
done

case "$(uname -s)" in
    Linux) ;;
    *) err "hvnetpp 仅支持 Linux（Windows 下请在 WSL 中运行本脚本）" ;;
esac
command -v cmake > /dev/null || err "未找到 cmake"

case "$SANITIZER" in
    ubsan)
        EXTRA_CXX_FLAGS="-fsanitize=undefined -fsanitize-undefined-trap-on-error -fno-omit-frame-pointer -D_GLIBCXX_ASSERTIONS"
        BUILD_DIR="$PROJECT_ROOT/build-ubsan"
        ;;
    asan)
        EXTRA_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
        BUILD_DIR="$PROJECT_ROOT/build-asan"
        ;;
    tsan)
        EXTRA_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer"
        BUILD_DIR="$PROJECT_ROOT/build-tsan"
        ;;
esac

if [ "$CLEAN" -eq 1 ]; then
    echo "[build.sh] 清理 $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

echo "[build.sh] 构建类型=$BUILD_TYPE 目录=$BUILD_DIR 并行=$JOBS 示例=$BUILD_EXAMPLES ${SANITIZER:+sanitizer=$SANITIZER}"

CMAKE_ARGS=(
    -S "$PROJECT_ROOT"
    -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DHVNETPP_BUILD_EXAMPLES="$BUILD_EXAMPLES"
)
if [ -n "$EXTRA_CXX_FLAGS" ]; then
    CMAKE_ARGS+=(-DCMAKE_CXX_FLAGS="$EXTRA_CXX_FLAGS")
fi

cmake "${CMAKE_ARGS[@]}"
cmake --build "$BUILD_DIR" -j "$JOBS"

echo "[build.sh] 构建完成: $BUILD_DIR/libhvnetpp.a"
if [ "$BUILD_EXAMPLES" = "ON" ]; then
    echo "[build.sh] 示例位于: $BUILD_DIR/examples/"
fi

if [ "$RUN_TEST" -eq 1 ]; then
    if [ "$BUILD_EXAMPLES" != "ON" ]; then
        err "--test 需要编译示例（不要与 --no-examples 同用）"
    fi
    for t in unpack_selftest loop_selftest net_selftest; do
        echo "[build.sh] 运行 $t ..."
        "$BUILD_DIR/examples/$t"
    done
fi
