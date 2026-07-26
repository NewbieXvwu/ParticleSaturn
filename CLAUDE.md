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

# Codebase Documentation Protocol

五份常设文档各司其职（体系见 DECISIONS D-016，不得增设新常设文档）：
`TODO.md`（现行待办）· `docs/CODEMAP.md`（地图：能力→位置）· `docs/DECISIONS.md`（决策与已废止方案）· `docs/MIGRATION_LOG.md`（冻结迁移史+进展批注附录）· `docs/AUDIT_2026-07.md`（技术债快照）

- **写代码前**：需要接口/服务/工具函数/脚本时，先查 `docs/CODEMAP.md`，再全库 grep；确认不存在才新建。严禁凭印象重新发明已有能力——本仓库的三份 MD3、双份 AppState 都是这么来的。
- **新增/移动/删除模块、接口、入口、脚本**：同一提交更新 CODEMAP。
- **架构方向**以 `docs/DECISIONS.md` 为准，不得重新引入已废止方案；要推翻决策就追加新决策条目，不许默默绕过。
- **迁移/替换必须在同一提交系列删除旧实现**；暂不能删的登记 CODEMAP 冻结区。冻结区内文件勿修改、勿扩展、勿模仿。
- **无第二个真实消费者不新建抽象接口。**
- **进展批注**追加到 `docs/MIGRATION_LOG.md` 文末"归档后进展"节；`TODO.md` 只维护勾选状态与一行备注。
