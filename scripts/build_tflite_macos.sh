#!/bin/sh

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tensorflow_root="$repo_root/HandTracker/libs/tensorflow"
tflite_source="$tensorflow_root/tensorflow/lite"
build_dir=${PARTICLESATURN_TFLITE_ARM64_DIR:-/tmp/particlesaturn-tflite-arm64}
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-$(sysctl -n hw.ncpu)}

if [ ! -d "$tflite_source/.git" ] && [ ! -f "$tensorflow_root/.git" ]; then
    printf '%s\n' "TensorFlow Lite 子模块未初始化：$tensorflow_root" >&2
    exit 1
fi

"$repo_root/scripts/apply_third_party_patch.sh" tensorflow-lite

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

# tensorflow-lite is a static archive. Build the XNNPACK delegate and the
# Abseil logging archives it references so consumers can link a real model
# runtime without depending on unbuilt implementation targets.
cmake --build "$build_dir" --target \
    tensorflow-lite \
    xnnpack-delegate \
    absl_log_internal_message \
    absl_log_internal_check_op \
    absl_log_internal_conditions \
    absl_log_internal_format \
    absl_log_internal_proto \
    --parallel "$jobs"

lipo -archs "$build_dir/libtensorflow-lite.a"
nm -gU "$build_dir/libxnnpack-delegate.a" | grep -q 'TfLiteXNNPackDelegateCreate'
nm -gU "$build_dir/_deps/xnnpack-build/libXNNPACK.a" | grep -q 'xnn_initialize'
test -f "$build_dir/_deps/abseil-cpp-build/absl/log/libabsl_log_internal_message.a"
printf '%s\n' "ARM64 TensorFlow Lite 与 XNNPACK 构建验证通过：$build_dir"
