#!/bin/sh
# 遗留人工验收引导脚本（TODO"遗留人工验收"节；冷启动协议第 4 条：agent 只能
# 为人工验收准备脚本与操作说明，勾选权在真人）。逐项打印操作说明、代跑可自动
# 化的命令，然后请真人肉眼判定 PASS/FAIL/SKIP，最后汇总。不改 TODO.md。
#
# 用法：scripts/manual_acceptance_macos.sh <app-bundle-binary> [编号...]
#   app-bundle-binary 例：build/ParticleSaturn.macOS.app/Contents/MacOS/ParticleSaturn.macOS
#   编号缺省跑全部（1-9），可指定子集如 "6" 或 "2 6 9"。
#   前置：请在前台、无其他应用抢焦点、断言生效构建（本仓库 build/ 默认满足）下执行。

set -eu

app=$1
shift
items=${*:-"1 2 3 4 5 6 7 8 9"}
build_dir=$(dirname "$(dirname "$(dirname "$(dirname "$app")")")")

results=""

ask() {
    printf '\n>>> 判定 [p=PASS / f=FAIL / s=SKIP]: '
    read -r answer < /dev/tty
    case "$answer" in
    p|P) results="$results
  第 $1 项 PASS —— $2" ;;
    f|F) results="$results
  第 $1 项 FAIL —— $2" ;;
    *)   results="$results
  第 $1 项 SKIP —— $2" ;;
    esac
}

banner() {
    printf '\n============================================================\n'
    printf '第 %s 项：%s\n' "$1" "$2"
    printf '============================================================\n'
}

run_mode() {
    printf '\n[启动 %s 模式，肉眼检查后 Cmd+Q 退出]\n' "$1"
    case "$1" in
    vulkan-*) env PARTICLESATURN_GRAPHICS_API=vulkan PARTICLESATURN_VULKAN_DRIVER="${1#vulkan-}" "$app" || true ;;
    *)        env PARTICLESATURN_GRAPHICS_API="$1" "$app" || true ;;
    esac
}

for item in $items; do
    case "$item" in
    1)
        banner 1 "Metal 成为 macOS 参考路径（终验，§8）"
        cat <<'EOF'
说明：Metal 是对比模式的参考路径（D-001）。确认场景完整（星空+土星环+泛光+
面板 acrylic）、无闪烁/撕裂/黑帧，交互（拖拽旋转、滚轮缩放、F3 调试窗、B 切
换模糊）正常。对比模式量化数据已在 TODO P4 记录，此项是肉眼终验。
EOF
        run_mode metal
        ask 1 "Metal 参考路径终验"
        ;;
    2)
        banner 2 "MoltenVK / KosmicKrisp 运行验证（§10.6：画面、交互、设备丢失、重启）"
        cat <<'EOF'
说明：两个 Vulkan ICD 各验四件事。前两件（画面+交互）靠肉眼；后两件（设备丢
失恢复、重启）自动化冒烟已在 ctest 覆盖，这里代跑一遍给你看退出码。
EOF
        for driver in molten kosmic; do
            printf '\n--- %s：肉眼看画面与交互 ---\n' "$driver"
            run_mode "vulkan-$driver"
            printf '\n--- %s：设备丢失恢复冒烟（期望退出码 0）---\n' "$driver"
            env PARTICLESATURN_GRAPHICS_API=vulkan PARTICLESATURN_VULKAN_DRIVER=$driver \
                PARTICLESATURN_VULKAN_SMOKE_FRAMES=60 PARTICLESATURN_VULKAN_DEVICE_LOST_SMOKE=1 "$app" \
                && printf '%s 设备丢失恢复：退出码 0\n' "$driver" || printf '%s 设备丢失恢复：失败\n' "$driver"
            printf '\n--- %s：重启冒烟（期望退出码 0）---\n' "$driver"
            env PARTICLESATURN_GRAPHICS_API=vulkan PARTICLESATURN_VULKAN_DRIVER=$driver \
                PARTICLESATURN_VULKAN_RESTART_SMOKE=1 "$app" \
                && printf '%s 重启：退出码 0\n' "$driver" || printf '%s 重启：失败\n' "$driver"
        done
        ask 2 "MoltenVK/KosmicKrisp 四件套"
        ;;
    3)
        banner 3 "网格着色器对等路径实机验收（断言生效构建下重跑基线确认阈值）"
        printf '说明：代跑 gpu 层测试（含网格着色器对等基线），全绿即阈值成立。\n\n'
        ctest --test-dir "$build_dir" -L gpu --output-on-failure || true
        ask 3 "网格着色器对等基线"
        ;;
    4)
        banner 4 "MD3 界面迁移视觉验收"
        cat <<'EOF'
说明：任一模式启动后打开设置面板，检查 MD3 视觉：圆角卡片、开关/滑条样式、
深浅色切换（面板内主题开关）、acrylic 背景模糊、图表区弱模糊。与迁移前截图
（若有）对照；无参照则按"无明显视觉破绽"判定。
EOF
        run_mode metal
        ask 4 "MD3 视觉验收"
        ;;
    5)
        banner 5 "窗口行为对齐；Retina 与外接显示器；睡眠唤醒（§16 阶段 10）"
        cat <<'EOF'
说明（需要外接显示器与手动睡眠，脚本无法代劳）：
  a. 任一模式下拖动窗口跨 Retina/外接显示器，画面缩放与 DPI 正确、无模糊/错位；
  b. 全屏进出、最小化还原、窗口边缘拖拽调整大小，四模式行为一致；
  c. 合盖或菜单睡眠→唤醒，画面恢复、无黑屏/设备丢失残留。
按需自行启动各模式：PARTICLESATURN_GRAPHICS_API=metal|opengl41|vulkan（后者配
PARTICLESATURN_VULKAN_DRIVER=molten|kosmic）。
EOF
        ask 5 "窗口/Retina/睡眠唤醒"
        ;;
    6)
        banner 6 "全屏恢复 smoke 真人复核（TODO 第 73 行）"
        cat <<'EOF'
说明：请保持本终端前台、期间不要操作其他应用。代跑 4 个 FullscreenRestore
smoke；如仍失败，请肉眼确认窗口是否真的进出了全屏——若窗口可见地完成了转换
而测试仍报超时，是检测逻辑问题；若窗口根本没动，是真 bug。
EOF
        ctest --test-dir "$build_dir" -R FullscreenRestore --output-on-failure || true
        ask 6 "FullscreenRestore 前台复核"
        ;;
    7)
        banner 7 "手势输入端到端验证（真实镜头）"
        cat <<'EOF'
说明：接好摄像头后任一模式启动，面板里选择摄像头并授权。验证：手入画出现关
键点、捏合缩放场景、平移旋转、手移出画面后场景不失控回弹。
EOF
        run_mode metal
        ask 7 "手势端到端"
        ;;
    8)
        banner 8 "摄像头异常状态交互验收（拔线恢复、权限流程）"
        cat <<'EOF'
说明：a) 运行中拔掉外接摄像头→应用不崩溃、面板状态正确，重新插入可恢复；
b) 在系统设置里撤销摄像头权限后启动→权限请求流程正确、拒绝后应用可用。
EOF
        run_mode metal
        ask 8 "摄像头异常交互"
        ;;
    9)
        banner 9 "四种 macOS 模式总验收（构建/启动/呈现/交互 + 设置持久化等）"
        cat <<'EOF'
说明：依次启动四模式，每个都检查：呈现正常、面板交互、主题/材质/垂直同步切
换生效、快捷键（F3/B/Cmd+Q）、退出后重启设置仍在（持久化）、LOD 行为一致。
EOF
        for mode in metal opengl41 vulkan-molten vulkan-kosmic; do
            run_mode "$mode"
        done
        ask 9 "四模式总验收"
        ;;
    *)
        printf '未知编号 %s（有效 1-9）\n' "$item" >&2
        ;;
    esac
done

printf '\n============================================================\n'
printf '人工验收结果汇总（请据此亲手更新 TODO.md 勾选状态）：%s\n' "$results"
printf '============================================================\n'
