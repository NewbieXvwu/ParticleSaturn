#!/bin/sh

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

case "${1:-}" in
    diligent-volk-loader-path)
        source_root="$repo_root/libs/DiligentCore/ThirdParty/volk"
        patch_file="$repo_root/patches/diligent-volk-loader-path.patch"
        ;;
    tensorflow-lite-prune)
        source_root="$repo_root/HandTracker/libs/tensorflow"
        patch_file="$repo_root/patches/tflite-prune.patch"
        ;;
    imgui-md3)
        source_root="$repo_root/libs/imgui"
        patch_file="$repo_root/patches/imgui-md3.patch"
        ;;
    *)
        printf '%s\n' "用法：$0 {diligent-volk-loader-path|tensorflow-lite-prune|imgui-md3}" >&2
        exit 2
        ;;
esac

if [ ! -d "$source_root" ] || [ ! -f "$patch_file" ]; then
    printf '%s\n' "补丁源或目标目录不存在：$patch_file" >&2
    exit 1
fi

if git -C "$source_root" apply --reverse --check "$patch_file" >/dev/null 2>&1; then
    printf '%s\n' "补丁已应用：$(basename "$patch_file")"
elif git -C "$source_root" apply --check "$patch_file" >/dev/null 2>&1; then
    git -C "$source_root" apply "$patch_file"
    printf '%s\n' "已应用补丁：$(basename "$patch_file")"
else
    printf '%s\n' "补丁与当前源码不匹配：$patch_file" >&2
    exit 1
fi
