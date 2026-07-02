---
name: supabase-inspect
description: Use when checking Supabase connectivity, OAuth token validity, project metadata, tables, logs, advisors, or database health without making schema changes.
---

# Supabase Inspect

Inspect only; do not change schema or data.

1. List projects and select `NB_IOT` unless the user specifies another project.
2. Record the project ref and API URL.
3. List relevant tables, using verbose output only when columns are needed.
4. Check logs/advisors when debugging or before risky DB work.
5. Surface security advisories plainly, especially disabled RLS or public `SECURITY DEFINER` functions.

Current project ref: `yzorfvgpmkwnjpdfyqsk`.
