# 风格模板

把 `.clang-format` 与 `.editorconfig` 复制到工程根目录。

- `.clang-format` 只负责**格式**：Tab 缩进、K&R 花括号、指针右对齐、100 列。
- **命名**无法由 clang-format 强制，按 `references/layers-and-naming.md` §5 执行；
  审查时按 §8 输出偏差清单。
- 新工程直接使用本模板。
- 改造现有工程：若已有 `.clang-format`，**保留现有格式**，避免全文件重排；
  命名仍按本 skill 标准，新代码不跟随错误命名。
- 工具链没有 clang-format 时，至少保持 `.editorconfig` 的缩进与换行，
  并在提交前人工对照 §5 检查命名。
