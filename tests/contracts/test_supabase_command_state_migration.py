import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERSION = "20260725054445"
UP = ROOT / "supabase" / "migrations" / f"{VERSION}_device_command_state.sql"
DOWN = ROOT / "supabase" / "rollbacks" / f"{VERSION}_device_command_state_down.sql"
PRECHECK = ROOT / "supabase" / "prechecks" / f"{VERSION}_device_command_state_precheck.sql"
VERIFY = ROOT / "supabase" / "verifications" / f"{VERSION}_device_command_state_verify.sql"
EXPECTED = ROOT / "supabase" / "device_command_state_expected.md"


def sql(path: Path) -> str:
    return path.read_text(encoding="utf-8")


class SupabaseCommandStateMigrationContractTest(unittest.TestCase):
    def test_all_review_artifacts_exist(self):
        for path in (UP, DOWN, PRECHECK, VERIFY, EXPECTED):
            with self.subTest(path=path):
                self.assertTrue(path.is_file(), f"missing migration artifact: {path}")

    def test_up_is_additive_and_preserves_legacy_command_path(self):
        source = sql(UP)
        lowered = source.lower()
        self.assertIn("create table public.device_command_state", lowered)
        self.assertIn('references public."devicecmds" ("cmdid")', lowered)
        self.assertIn("create trigger trg_sync_device_command_state", lowered)
        self.assertNotRegex(lowered, r"drop\s+(table|trigger|function).*(devicecmds|assign_device_command)")
        self.assertNotRegex(lowered, r"alter\s+table\s+public\.\"devicecmds\"\s+rename")

    def test_claim_is_single_row_locked_and_bounded(self):
        source = sql(UP).lower()
        claim = source[source.index("create function public.claim_device_command") :]
        claim = claim[: claim.index("create function public.ack_device_command")]
        self.assertIn("into strict v_device_id", claim)
        self.assertIn("for update of s, c skip locked", claim)
        self.assertIn("limit 1", claim)
        self.assertIn("delivery_attempts < 5", claim)
        self.assertIn("p_last_cmd_id", claim)
        self.assertIn("lease_expires_at", claim)
        self.assertIn("jsonb_build_array", claim)

    def test_claim_is_blocked_until_the_legacy_delivery_trigger_is_removed(self):
        source = sql(UP).lower()
        claim = source[source.index("create function public.claim_device_command") :]
        claim = claim[: claim.index("create function public.ack_device_command")]
        self.assertIn("trg_assign_device_command", claim)
        self.assertIn("legacy command path remains active", claim)
        expected = EXPECTED.read_text(encoding="utf-8")
        self.assertIn("신규 command writer·consumer 활성화 차단", expected)
        self.assertIn("trg_assign_device_command", expected)

    def test_nullable_rpc_inputs_are_explicitly_rejected(self):
        source = sql(UP).lower()
        claim = source[source.index("create function public.claim_device_command") :]
        claim = claim[: claim.index("create function public.ack_device_command")]
        for token in (
            "p_request_id is null",
            "p_last_cmd_id is null",
            "p_lease_seconds is null",
        ):
            self.assertIn(token, claim)

        ack = source[source.index("create function public.ack_device_command") :]
        for token in (
            "p_cmd_id is null",
            "p_cmd_id > 4294967295",
            "p_phase is null",
            "p_result is null",
            "p_error is null",
            "p_unix_seconds is null",
            "p_clock_valid is null",
        ):
            self.assertIn(token, ack)

    def test_request_retry_replays_the_accepted_command(self):
        source = sql(UP).lower()
        claim = source[source.index("create function public.claim_device_command") :]
        claim = claim[: claim.index("create function public.ack_device_command")]
        self.assertIn("s.state in ('delivered', 'accepted')", claim)

    def test_ack_has_exact_phases_idempotence_and_receipt(self):
        source = sql(UP).lower()
        ack = source[source.index("create function public.ack_device_command") :]
        self.assertIn("p_phase = 1", ack)
        self.assertIn("p_phase = 2", ack)
        self.assertIn("accepted_result", ack)
        self.assertIn("final_result", ack)
        self.assertRegex(ack, r"return\s+public\.device_command_receipt\(")
        self.assertIn("receipt_mismatch", ack)
        receipt = source[
            source.index("create function public.device_command_receipt") :
            source.index("insert into public.device_command_state")
        ]
        self.assertIn("jsonb_build_array", receipt)

    def test_ack_retry_identity_does_not_depend_on_retry_timestamp(self):
        source = sql(UP).lower()
        accepted_duplicate = source[
            source.index("if v_state.accepted_result is not null") :
            source.index("if v_state.state <> 'delivered'")
        ]
        final_duplicate = source[
            source.index("if v_state.final_result is not null") :
            source.index("if v_state.state <> 'accepted'")
        ]
        for block in (accepted_duplicate, final_duplicate):
            with self.subTest(block=block[:40]):
                self.assertNotIn("_unix_seconds =", block)
                self.assertNotIn("_clock_valid =", block)

    def test_new_table_and_rpc_are_not_publicly_callable(self):
        source = sql(UP).lower()
        compact = re.sub(r"\s+", " ", source)
        self.assertIn("enable row level security", source)
        self.assertIn(
            "revoke all on table public.device_command_state from public, anon, authenticated",
            compact,
        )
        for signature in (
            "public.claim_device_command(text,bigint,bigint,integer)",
            "public.ack_device_command(text,bigint,smallint,smallint,smallint,bigint,boolean)",
        ):
            with self.subTest(signature=signature):
                self.assertIn(
                    f"revoke all on function {signature} from public, anon, authenticated",
                    compact,
                )
                self.assertIn(
                    f"grant execute on function {signature} to service_role",
                    compact,
                )
        self.assertGreaterEqual(source.count("security definer"), 3)
        self.assertGreaterEqual(source.count("set search_path = ''"), 4)

    def test_ack_and_receipt_accept_the_wire_uint32_command_id_domain(self):
        source = sql(UP).lower()
        self.assertIn(
            "create function public.device_command_receipt(\n    p_cmd_id bigint",
            source,
        )
        self.assertIn(
            "create function public.ack_device_command(\n"
            "    p_imei text,\n"
            "    p_cmd_id bigint",
            source,
        )
        expected = EXPECTED.read_text(encoding="utf-8")
        self.assertIn("uint32", expected)
        self.assertIn("legacy `deviceCmds.cmdId`", expected)

    def test_precheck_and_verify_guard_the_observed_baseline(self):
        precheck = sql(PRECHECK).lower()
        verify = sql(VERIFY).lower()
        self.assertFalse(precheck.lstrip().startswith("\\"))
        self.assertFalse(verify.lstrip().startswith("\\"))
        self.assertIn('public."devicecmds"', precheck)
        self.assertIn("assign_device_command()", precheck)
        self.assertIn(
            "t.tgfoid = 'public.assign_device_command()'::regprocedure",
            precheck,
        )
        self.assertIn("de97b37ad246c5e509ae06f821a10338", precheck)
        self.assertIn("raise exception", precheck)
        self.assertIn("relrowsecurity", verify)
        self.assertIn("trg_sync_device_command_state", verify)
        self.assertIn("has_function_privilege", verify)
        self.assertIn("has_table_privilege", verify)
        self.assertIn("prosecdef", verify)
        self.assertIn("idx_device_command_state_claim", verify)
        self.assertIn("uq_device_command_state_active_request", verify)
        self.assertIn("device_command_state_terminal_check", verify)
        self.assertIn("s.expires_at <> s.created_at + interval '1 day'", verify)
        self.assertIn("backfill state/timestamp mapping mismatch", verify)
        self.assertIn("when object_not_in_prerequisite_state then", verify)
        self.assertIn("legacy claim guard did not reject", verify)
        self.assertNotIn("has_function_privilege('public'", verify)
        self.assertIn("raise exception", verify)

    def test_backfill_uses_one_stable_statement_timestamp(self):
        source = sql(UP).lower()
        backfill = source[
            source.index("insert into public.device_command_state") :
            source.index("create function public.sync_device_command_state")
        ]
        self.assertIn("statement_timestamp()", backfill)
        self.assertNotIn("clock_timestamp()", backfill)
        sync = source[
            source.index("create function public.sync_device_command_state") :
            source.index("create trigger trg_sync_device_command_state")
        ]
        self.assertIn("v_now timestamp with time zone", sync)
        self.assertIn("v_now := clock_timestamp()", sync)

    def test_rollback_removes_only_new_objects_in_reverse_order(self):
        source = sql(DOWN).lower()
        expected_order = [
            "drop trigger trg_sync_device_command_state",
            "drop function public.sync_device_command_state()",
            "drop function public.ack_device_command",
            "drop function public.claim_device_command",
            "drop function public.device_command_receipt",
            "drop table public.device_command_state",
        ]
        offsets = [source.index(token) for token in expected_order]
        self.assertEqual(offsets, sorted(offsets))
        self.assertNotIn('drop table public."devicecmds"', source)
        self.assertNotIn("drop function public.assign_device_command", source)

    def test_expected_result_declares_no_live_apply_and_data_loss_boundary(self):
        source = EXPECTED.read_text(encoding="utf-8")
        self.assertIn("live apply: 0", source)
        self.assertIn("기존 `deviceCmds` 행 보존", source)
        self.assertIn("신규 companion 상태 이력 삭제", source)
        self.assertIn("별도 승인", source)


if __name__ == "__main__":
    unittest.main()
