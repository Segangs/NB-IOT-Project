---
name: commit-and-log
description: Use when finishing a task, updating project history or README, or preparing a concise Korean git commit when the user explicitly asks for git commit/sync/push.
---

# Commit And Log

1. Check `git status --short --branch`.
2. Update `project_history.md` at the top using the existing date-based format.
3. Update `README.md` with a concise completed-work summary, grouped under one section per date. If the date already exists, append the new work item inside that date section.
4. Review the diff for unrelated changes and avoid staging user changes accidentally.
5. Do not stage, commit, or push automatically at task completion.
6. Only when the user explicitly asks to commit/sync/push, commit with a short Korean conventional message, for example `docs: Codex 마이그레이션`.

Ask before push, deploy, merge, or destructive cleanup.
