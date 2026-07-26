#!/bin/sh
# 从官方源码构建 DXC 并安装到 ~/.local/opt/dxc（macOS 无官方二进制、brew 无公式）。
# 单源着色器翻译（src/shaders/single，D-004）的构建前置。

set -eu

work=${1:-"$(mktemp -d /tmp/dxc-build.XXXXXX)"}
prefix="$HOME/.local/opt/dxc"

echo "== 构建目录: $work"
if [ ! -d "$work/DirectXShaderCompiler" ]; then
    git clone --depth 1 https://github.com/microsoft/DirectXShaderCompiler.git "$work/DirectXShaderCompiler"
fi
cd "$work/DirectXShaderCompiler"
git submodule update --init --depth 1
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -C cmake/caches/PredefinedParams.cmake
ninja -C build dxc

mkdir -p "$prefix/bin" "$prefix/lib"
cp build/bin/dxc "$prefix/bin/"
cp build/lib/libdxcompiler.dylib "$prefix/lib/"
"$prefix/bin/dxc" --version
echo "== 已安装到 $prefix"
