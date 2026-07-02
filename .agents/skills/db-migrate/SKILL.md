---
name: db-migrate
description: Use when creating, reviewing, or applying Supabase/Postgres schema, RPC, RLS, policy, trigger, function, or data migration changes.
---

# DB Migrate

Database changes require explicit user approval.

1. Inspect current project, tables, logs, and advisors first.
2. Draft SQL and explain blast radius, rollback, and verification.
3. Ask for approval before applying anything to Supabase.
4. Prefer iterative `execute_sql` for inspection/testing; create a clean migration only when the final SQL is known.
5. After applying, verify with a targeted query and re-run advisors when relevant.
6. Update `project_history.md` and `README.md`.

Do not enable RLS without matching policies. Do not change public function grants blindly.
