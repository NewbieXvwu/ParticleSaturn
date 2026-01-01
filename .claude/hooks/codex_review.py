import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional, Tuple

MAX_DIFF_CHARS = 20000          # 防止一次塞太多 diff
POST_DEBOUNCE_SECONDS = 25      # 写文件后审阅节流
STOP_MAX_ROUNDS = 10            # 防止无限循环卡死
CODEX_TIMEOUT_SECONDS = 240     # 等 Codex 审阅的超时

STATUS_RE = re.compile(r"(?mi)^\s*STATUS\s*:\s*(APPROVED|CHANGES_REQUESTED)\s*$")

def run_cmd(args, cwd: Path, input_text: Optional[str] = None, timeout: int = 60) -> Tuple[int, str, str]:
    try:
        # 显式传递完整环境变量，确保 PATH 正确继承
        env = os.environ.copy()
        # Windows 上强制使用 UTF-8 编码
        env["PYTHONIOENCODING"] = "utf-8"
        p = subprocess.run(
            args,
            cwd=str(cwd),
            input=input_text,
            capture_output=True,
            timeout=timeout,
            shell=True,  # 使用 shell 以正确解析 PATH
            env=env,
            encoding="utf-8",
            errors="replace",
        )
        return p.returncode, p.stdout or "", p.stderr or ""
    except FileNotFoundError:
        return 127, "", f"command not found: {args[0] if isinstance(args, list) else args}"
    except subprocess.TimeoutExpired:
        return 124, "", f"timeout: {args if isinstance(args, str) else ' '.join(args)}"

def is_git_repo(project_dir: Path) -> bool:
    return (project_dir / ".git").exists()

def get_git_diff(project_dir: Path, file_path: Optional[str] = None) -> str:
    if not is_git_repo(project_dir):
        return ""
    args = ["git", "diff", "--no-color", "--unified=3"]
    if file_path:
        args += ["--", file_path]
    rc, out, err = run_cmd(args, cwd=project_dir, timeout=30)
    if rc != 0:
        return ""
    if len(out) > MAX_DIFF_CHARS:
        return out[:MAX_DIFF_CHARS] + "\n\n[diff truncated]\n"
    return out

def get_git_name_only(project_dir: Path) -> str:
    if not is_git_repo(project_dir):
        return ""
    rc, out, _ = run_cmd(["git", "diff", "--name-only"], cwd=project_dir, timeout=15)
    return out.strip() if rc == 0 else ""

def codex_available(project_dir: Path) -> Tuple[bool, str]:
    rc, out, err = run_cmd(["cping"], cwd=project_dir, timeout=10)
    if rc == 0:
        return True, (out.strip() or "cping ok")
    return False, (err.strip() or out.strip() or "cping failed")

def call_codex(project_dir: Path, prompt: str) -> Tuple[bool, str]:
    # 优先尝试：stdin 传入（更稳，避免参数里包含大量换行）
    rc, out, err = run_cmd(["cask-w"], cwd=project_dir, input_text=prompt, timeout=CODEX_TIMEOUT_SECONDS)
    if rc == 0 and (out.strip() or err.strip()):
        return True, (out.strip() or err.strip())

    # 备选：把 prompt 当成参数传入
    rc2, out2, err2 = run_cmd(["cask-w", prompt], cwd=project_dir, timeout=CODEX_TIMEOUT_SECONDS)
    if rc2 == 0 and (out2.strip() or err2.strip()):
        return True, (out2.strip() or err2.strip())

    msg = f"cask-w failed (rc={rc}, rc2={rc2}). stderr={err.strip()} / {err2.strip()}"
    return False, msg

def load_json(path: Path, default):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return default

def save_json(path: Path, data) -> None:
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")

def main() -> int:
    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    payload = json.load(sys.stdin)

    project_dir = Path(os.environ.get("CLAUDE_PROJECT_DIR") or payload.get("cwd") or os.getcwd()).resolve()
    state_dir = project_dir / ".claude" / "hooks" / "state"
    state_dir.mkdir(parents=True, exist_ok=True)

    disabled_flag = state_dir / "codex.disabled"
    if disabled_flag.exists():
        print("{}")
        return 0

    loop_state_path = state_dir / "codex_loop.json"
    claude_reply_path = state_dir / "claude_reply.md"
    post_state_path = state_dir / "post_state.json"

    # 提取本次编辑的文件路径（PostToolUse 会带 tool_input.file_path）
    tool_input = payload.get("tool_input") or {}
    edited_file = tool_input.get("file_path") if isinstance(tool_input, dict) else None

    # 先检查是否有代码变更，无变更直接放行（设计/讨论阶段不触发审阅）
    changed_files = get_git_name_only(project_dir)
    diff = get_git_diff(project_dir, edited_file if mode == "post" else None).strip()

    if mode in ("post", "stop") and not diff and not changed_files:
        print("{}")
        return 0

    # PostToolUse：节流，避免每次小改都打爆额度
    if mode == "post":
        post_state = load_json(post_state_path, {"last_ts": 0})
        now = time.time()
        if now - float(post_state.get("last_ts", 0)) < POST_DEBOUNCE_SECONDS:
            print("{}")
            return 0
        post_state["last_ts"] = now
        save_json(post_state_path, post_state)

    # 有代码变更时才检查 Codex 可用性
    ok, why = codex_available(project_dir)
    if not ok:
        # Post：不强行 block，避免 Codex 不可用时影响 Claude 工作流
        if mode == "post":
            print("{}")
            return 0
        # Stop：block，提示用开关关闭协作
        print(json.dumps({
            "decision": "block",
            "reason": (
                "Codex 当前不可用（cping 失败）。\n"
                f"原因：{why}\n\n"
                "需要继续结束本轮：运行 codex-off.cmd（或创建 .claude/hooks/state/codex.disabled）后再结束。"
            )
        }, ensure_ascii=False))
        return 0

    # Stop：多轮辩论状态
    loop = load_json(loop_state_path, {"round": 0})
    if mode == "stop":
        # stop_hook_active 表示 Claude 已经被 Stop hook 续命过一次，用它限制死循环:contentReference[oaicite:15]{index=15}
        loop_round = int(loop.get("round", 0))
        if loop_round >= STOP_MAX_ROUNDS:
            print(json.dumps({
                "decision": "block",
                "reason": (
                    f"已连续触发 Codex 审阅 {loop_round} 轮，仍未达成一致。\n"
                    "为避免无限循环，请运行 codex-off.cmd 临时关闭协作后再结束，或先手动解决分歧再结束。"
                )
            }, ensure_ascii=False))
            return 0

    # Claude→Codex 的“辩论回信”文件：由 Claude 逐条写接受/反驳
    claude_reply = ""
    if claude_reply_path.exists():
        claude_reply = claude_reply_path.read_text(encoding="utf-8").strip()

    prompt = (
        "你是代码审阅者（Reviewer only）。你的代码审阅意见将回传给Claude，由它来实现你的修改或提出反对意见。不要修改任何文件，但可以读取任意文件。\n"
        "请严格按以下格式输出：\n"
        "STATUS: APPROVED 或 CHANGES_REQUESTED\n"
        "1) 关键问题（按优先级）\n"
        "2) 建议修改（可给 unified diff）\n"
        "3) 对 Claude 反驳/说明的回应（如有）\n\n"
        "---- 项目变更信息 ----\n"
        f"Changed files:\n{changed_files or '(unknown)'}\n\n"
        "Diff:\n"
        f"{diff}\n\n"
        "---- Claude 的逐条处理/反驳（如有）----\n"
        f"{claude_reply or '(none)'}\n"
    )

    success, codex_text = call_codex(project_dir, prompt)
    if not success:
        if mode == "post":
            print("{}")
            return 0
        print(json.dumps({
            "decision": "block",
            "reason": (
                "已尝试调用 Codex 进行审阅，但调用失败。\n"
                f"{codex_text}\n\n"
                "需要继续结束本轮：运行 codex-off.cmd 后再结束。"
            )
        }, ensure_ascii=False))
        return 0

    m = STATUS_RE.search(codex_text)
    codex_status = m.group(1) if m else "CHANGES_REQUESTED"

    # Stop：一致条件 = Codex APPROVED 且 Claude 在回信里写 CONSENSUS: YES
    if mode == "stop":
        consensus_yes = bool(re.search(r"(?mi)^\s*CONSENSUS\s*:\s*YES\s*$", claude_reply or ""))
        if codex_status == "APPROVED" and consensus_yes:
            # 清理状态，放行结束
            try:
                loop_state_path.unlink(missing_ok=True)
                # 不强制删除 claude_reply，留痕；需要可自行删
            except Exception:
                pass
            print("{}")
            return 0

        loop["round"] = int(loop.get("round", 0)) + 1
        loop["last_status"] = codex_status
        save_json(loop_state_path, loop)

        # 把 Codex 意见回传给 Claude，并要求 Claude 更新 claude_reply.md 再次 Stop
        if not claude_reply_path.exists():
            claude_reply_path.write_text(
                "逐条写你对 Codex 意见的处理：ACCEPT/REJECT + 理由。\n"
                "当你认为已与 Codex 达成一致时，追加一行：CONSENSUS: YES\n",
                encoding="utf-8"
            )

        print(json.dumps({
            "decision": "block",
            "reason": (
                "【Codex 审阅结果】\n"
                f"{codex_text}\n\n"
                "【下一步要求】\n"
                "1) Claude 负责按意见修改代码；\n"
                f"2) 把你对每条意见的处理写入：{claude_reply_path.as_posix()}\n"
                "   - ACCEPT/REJECT + 理由\n"
                "   - 达成一致时加：CONSENSUS: YES\n"
                "3) 然后再次尝试结束本轮（会触发下一轮 Codex 复审）。"
            )
        }, ensure_ascii=False))
        return 0

    # Post：把 Codex 快速审阅塞回 Claude（不要求一致，只是即时反馈）
    print(json.dumps({
        "decision": "block",
        "reason": "【Codex 快速审阅（写文件后自动触发）】\n" + codex_text
    }, ensure_ascii=False))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
