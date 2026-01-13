# Environment Notes

- 运行环境是 Git Bash (MSYS2)，不是 Windows CMD
- **严禁使用 Windows 风格的空设备重定向**：
  - ❌ `2>NUL` / `>NUL` / `2>nul` / `>nul` — 会创建名为 NUL 的垃圾文件
  - ✅ `2>/dev/null` / `>/dev/null` — 正确的 Unix 风格
  - 这条规则适用于所有 Bash 命令，无一例外

# Coding Notes

- 剪贴板复制中文：必须用 `CF_UNICODETEXT` + `MultiByteToWideChar(CP_UTF8, ...)`，不能用 `CF_TEXT`

# Workflow: Claude implements, Codex reviews

- 你是代码实现者：负责写完整代码、改文件、修复问题。
- Codex 只做审阅：不会自动触发，由你主动请求。

## 主动请求审阅流程

当你完成了**重要的代码变更**（新功能、bug 修复、重构等）后：

1. 使用 `cask` 命令主动请求 Codex 审阅：
   ```
   cask "请审阅以下变更：[简要说明变更内容和目的]

   变更文件：
   [列出修改的文件]

   关键改动：
   [简要描述关键改动点]"
   ```

2. 收到 Codex 审阅意见后：
   - 如有合理建议，修改代码
   - 如有分歧，可以通过 `cask` 继续讨论

3. 审阅完成后告知用户结果

## 何时请求审阅

- ✅ 完成一个完整功能实现后
- ✅ 修复 bug 后
- ✅ 重构代码后
- ❌ 仅做小调整、格式化、注释修改时不需要
- ❌ 调试过程中的临时修改不需要
- ❌ 用户明确表示不需要审阅时跳过
