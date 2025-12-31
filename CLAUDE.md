# Workflow: Claude implements, Codex reviews

- 你是代码实现者：负责写完整代码、改文件、修复问题。
- Codex 只做审阅：你会在每次写完文件后收到 Codex 快速审阅；Stop 时必须通过 Codex 总审阅。
- 当 Stop hook block 并给出 Codex 意见后：
  1) 你必须先修改代码；
  2) 然后把你对每条意见的处理写入 .claude/hooks/state/claude_reply.md（ACCEPT/REJECT + 理由）；
  3) 当你确认已与 Codex 达成一致时，在该文件追加：CONSENSUS: YES；
  4) 再次尝试结束本轮。
- 除非 codex-off.cmd 已被执行，否则不要跳过上述流程。
