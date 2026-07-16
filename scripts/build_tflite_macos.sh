#!/bin/sh

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tensorflow_root="$repo_root/HandTracker/libs/tensorflow"
tflite_source="$tensorflow_root/tensorflow/lite"
patch_file="$repo_root/scripts/tflite-prune.patch"
build_dir=${PARTICLESATURN_TFLITE_ARM64_DIR:-/tmp/particlesaturn-tflite-arm64}
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-$(sysctl -n hw.ncpu)}

if [ ! -d "$tflite_source/.git" ] && [ ! -f "$tensorflow_root/.git" ]; then
    printf '%s\n' "TensorFlow Lite 子模块未初始化：$tensorflow_root" >&2
    exit 1
fi

if git -C "$tensorflow_root" apply --reverse --check "$patch_file"; then
    printf '%s\n' "TensorFlow Lite 精简补丁已应用"
elif git -C "$tensorflow_root" apply --check "$patch_file"; then
    git -C "$tensorflow_root" apply "$patch_file"
    printf '%s\n' "已应用 TensorFlow Lite 精简补丁"
else
    printf '%s\n' "TensorFlow Lite 源码与 scripts/tflite-prune.patch 不一致" >&2
    exit 1
fi

cmake -S "$tflite_source" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DTENSORFLOW_SOURCE_DIR="$tensorflow_root" \
    -DBUILD_SHARED_LIBS=OFF \
    -DTFLITE_ENABLE_XNNPACK=ON \
    -DXNNPACK_ENABLE_KLEIDIAI=OFF \
    -DTFLITE_ENABLE_RUY=ON \
    -DTFLITE_ENABLE_GPU=OFF \
    -DTFLITE_ENABLE_MMAP=ON \
    -DTFLITE_ENABLE_NNAPI=OFF \
    -DTFLITE_ENABLE_RESOURCE=ON \
    -DTFLITE_ENABLE_EXTERNAL_DELEGATE=OFF \
    -DTFLITE_ENABLE_LABEL_IMAGE=OFF \
    -DTFLITE_KERNEL_TEST=OFF \
    -Wno-dev

cmake --build "$build_dir" --target tensorflow-lite --parallel "$jobs"

lipo -archs "$build_dir/libtensorflow-lite.a"
nm -gU "$build_dir/libxnnpack-delegate.a" | grep -q 'TfLiteXNNPackDelegateCreate'
nm -gU "$build_dir/_deps/xnnpack-build/libXNNPACK.a" | grep -q 'xnn_initialize'
printf '%s\n' "ARM64 TensorFlow Lite 与 XNNPACK 构建验证通过：$build_dir"
