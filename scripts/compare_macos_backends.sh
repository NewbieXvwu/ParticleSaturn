#!/bin/sh
# 对比模式驱动（TODO P4，D-001 测量做成功能）：同一确定性帧状态
# （PARTICLESATURN_CAPTURE_BASELINE：固定种子、固定几何、暂停场景、锁 LOD）
# 依次送各后端渲染捕获，再以 Metal（拟定参考路径）为基准输出
# 逐后端指标、差异热力图与并排图。
#
# 用法：scripts/compare_macos_backends.sh <app-bundle-binary> <compare-tool> <output-dir> [backends...]
#   backends 缺省为 "metal opengl41 vulkan-molten"；kosmic 可用 "vulkan-kosmic" 追加。

set -eu

app=$1
tool=$2
out=$3
shift 3
backends=${*:-"metal opengl41 vulkan-molten"}

mkdir -p "$out"

capture() {
    backend=$1
    ppm="$out/$backend.ppm"
    case "$backend" in
    vulkan-*)
        driver=${backend#vulkan-}
        env PARTICLESATURN_GRAPHICS_API=vulkan PARTICLESATURN_VULKAN_DRIVER="$driver" \
            PARTICLESATURN_CAPTURE_BASELINE="$ppm" "$app"
        ;;
    *)
        env PARTICLESATURN_GRAPHICS_API="$backend" PARTICLESATURN_CAPTURE_BASELINE="$ppm" "$app"
        ;;
    esac
    [ -s "$ppm" ] || { echo "capture failed for $backend" >&2; exit 1; }
}

for backend in $backends; do
    echo "== capturing $backend"
    capture "$backend"
done

reference=""
for backend in $backends; do
    if [ -z "$reference" ]; then
        reference=$backend
        echo "== reference path: $backend"
        continue
    fi
    echo "== $backend vs $reference"
    "$tool" "$out/$reference.ppm" "$out/$backend.ppm" "$out/$backend-vs-$reference"
done
echo "== outputs in $out"
