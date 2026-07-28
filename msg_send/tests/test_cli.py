from __future__ import annotations

from io import StringIO
import json
from pathlib import Path
import tempfile
import unittest
from contextlib import redirect_stderr
from datetime import datetime, timezone

from msg_send.__main__ import main
from msg_send.domain import (
    DeliveryChannel,
    MessageJob,
    MessagePolicy,
    MessageStatus,
    ProviderSubmission,
    PushAction,
    PushApplication,
    SubmissionRegistration,
)
from msg_send.runtime import (
    ProductionRuntime,
    RuntimeRunResult,
)


def config_env() -> dict[str, str]:
    return {
        "SUPABASE_URL": "https://project-ref.supabase.co",
        "SUPABASE_SECRET_KEY": "sb_secret_test_only_not_a_real_key",
        "BIZPPURIO_ACCOUNT": "provider-account",
        "BIZPPURIO_PASSWORD": "provider-password",
        "BIZPPURIO_FROM": "0212345678",
        "BIZPPURIO_SENDERKEY": "sender-profile-key",
        "BIZPPURIO_AT_COST_KRW": "8.5000",
        "BIZPPURIO_SMS_COST_KRW": "12.0000",
    }


class FakeRuntime:
    def __init__(self) -> None:
        self.one_shot_ids: list[str] = []
        self.callback_calls = 0
        self.reconcile_ids: list[str] = []

    def run_once(self) -> RuntimeRunResult:
        return RuntimeRunResult(
            executed=False,
            success=True,
            reason="claim_disabled",
        )

    def one_shot(self, expected_message_id: str) -> RuntimeRunResult:
        self.one_shot_ids.append(expected_message_id)
        return RuntimeRunResult(
            executed=True,
            success=True,
            reason="completed",
        )

    def reconcile_callback(
        self,
        provider_request_id: str,
    ) -> RuntimeRunResult:
        self.reconcile_ids.append(provider_request_id)
        return RuntimeRunResult(
            executed=True,
            success=True,
            reason="report_requested",
        )

    def handle_callback_payload_non_sending(
        self,
        payload: dict[str, object],
    ) -> PushAction:
        del payload
        self.callback_calls += 1
        return PushAction.DELIVERED


class FallbackApplyDatabase:
    def __init__(self) -> None:
        self.rejections = 0
        self.started = 0
        self.waiting = 0

    def apply_push_result(
        self,
        *args: object,
        **kwargs: object,
    ) -> PushApplication:
        del args, kwargs
        return PushApplication(
            PushAction.SMS_FALLBACK,
            MessageJob(
                message_id="42",
                lease_token="12345678-1234-4abc-8def-1234567890ab",
                dedupe_key="dedupe-42",
                recipient="01012345678",
                template_code="template-reference",
                template_body="source body",
                history_url="https://zxcx.io/s/history_token_123456",
                settings_url="https://zxcx.io/s/settings_token_123456",
                sms_body="fallback body",
                attempt_count=2,
                policy=MessagePolicy(
                    consent_granted=True,
                    sms_fallback_approved=True,
                    sms_cost_class="sms",
                ),
                delivery_channel=DeliveryChannel.SMS,
                fallback_reason="7001",
                max_attempts=1,
                lock_owner="spaceship-msg-send-1",
            ),
        )

    def reject_sms_fallback(
        self,
        *args: object,
        **kwargs: object,
    ) -> None:
        del args, kwargs
        self.rejections += 1

    def mark_submission_started(
        self,
        job: MessageJob,
        channel: DeliveryChannel,
    ) -> None:
        del job, channel
        self.started += 1

    def mark_submission_waiting_result(
        self,
        job: MessageJob,
        provider_request_id: str,
        channel: DeliveryChannel,
        now: datetime,
    ) -> SubmissionRegistration:
        del job, provider_request_id, channel, now
        self.waiting += 1
        return SubmissionRegistration(
            applied=True,
            duplicate=False,
            current_status=MessageStatus.WAITING_RESULT,
        )


class CountingProvider:
    def __init__(self) -> None:
        self.calls = 0

    def submit(
        self,
        job: MessageJob,
        channel: DeliveryChannel,
    ) -> ProviderSubmission:
        del job, channel
        self.calls += 1
        return ProviderSubmission(
            accepted=True,
            request_id="provider-request-fallback",
        )


def run_synthetic_callback_apply(
    *,
    fallback_enabled: bool,
) -> tuple[int, StringIO, FallbackApplyDatabase, CountingProvider]:
    payload = {
        "CMSGID": "provider-request-private",
        "REFKEY": "1234567812344abc8def1234567890ab",
        "MSGID": "provider-result-private",
        "UNIXTIME": "1785038400",
        "RESULT": "7001",
        "MEDIA": "AT",
    }
    environment = config_env()
    environment["MSG_SEND_CLAIM_ENABLED"] = "true"
    environment["MSG_SEND_SEND_ENABLED"] = "true"
    environment["MSG_SEND_CALLBACK_APPLY_ENABLED"] = "true"
    environment["MSG_SEND_SMS_FALLBACK_ENABLED"] = (
        "true" if fallback_enabled else "false"
    )
    database = FallbackApplyDatabase()
    provider = CountingProvider()

    def factory(config: object) -> ProductionRuntime:
        return ProductionRuntime(
            config=config,  # type: ignore[arg-type]
            database=database,  # type: ignore[arg-type]
            provider=provider,
            clock=lambda: datetime(
                2026,
                7,
                26,
                12,
                30,
                tzinfo=timezone.utc,
            ),
        )

    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "callback.json"
        path.write_text(json.dumps(payload), encoding="utf-8")
        output = StringIO()
        code = main(
            [
                "synthetic-callback",
                "--input",
                str(path),
                "--apply",
            ],
            environ=environment,
            runtime_factory=factory,
            output=output,
        )
    return code, output, database, provider


class CommandLineSafetyTests(unittest.TestCase):
    def test_callback_spool_drain_requires_explicit_apply_gate(
        self,
    ) -> None:
        factory_calls = 0

        def factory(config: object) -> FakeRuntime:
            nonlocal factory_calls
            del config
            factory_calls += 1
            return FakeRuntime()

        with tempfile.TemporaryDirectory() as directory:
            for name in ("inbox", "processing", "quarantine"):
                (Path(directory) / name).mkdir()
            with redirect_stderr(StringIO()):
                with self.assertRaises(SystemExit):
                    main(
                        [
                            "drain-callback-spool",
                            "--spool-root",
                            directory,
                        ],
                        environ=config_env(),
                        runtime_factory=factory,  # type: ignore[arg-type]
                        output=StringIO(),
                    )
        self.assertEqual(factory_calls, 0)

    def test_callback_spool_drain_emits_sanitized_counts(
        self,
    ) -> None:
        environment = config_env()
        environment["MSG_SEND_CALLBACK_APPLY_ENABLED"] = "true"
        runtime = FakeRuntime()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("inbox", "processing", "quarantine"):
                (root / name).mkdir()
            path = root / "inbox" / (("a" * 64) + ".json")
            path.write_text(
                json.dumps(
                    {
                        "version": 1,
                        "attempts": 0,
                        "payload": {
                            "CMSGID": "PRIVATE_REQUEST",
                            "REFKEY":
                                "1234567812344abc8def1234567890ab",
                            "MSGID": "PRIVATE_RESULT",
                            "UNIXTIME": "1785038400",
                            "RESULT": "7000",
                            "MEDIA": "AT",
                        },
                    }
                ),
                encoding="utf-8",
            )
            output = StringIO()
            code = main(
                [
                    "drain-callback-spool",
                    "--spool-root",
                    directory,
                ],
                environ=environment,
                runtime_factory=lambda config: runtime,  # type: ignore[arg-type]
                output=output,
            )

        self.assertEqual(code, 0)
        self.assertEqual(runtime.callback_calls, 1)
        self.assertEqual(
            json.loads(output.getvalue()),
            {
                "processed": 1,
                "quarantined": 0,
                "retried": 0,
                "success": True,
            },
        )
        self.assertNotIn("PRIVATE_REQUEST", output.getvalue())
        self.assertNotIn("PRIVATE_RESULT", output.getvalue())

    def test_one_shot_requires_explicit_confirmation(self) -> None:
        factory_calls = 0

        def factory(config: object) -> FakeRuntime:
            nonlocal factory_calls
            del config
            factory_calls += 1
            return FakeRuntime()

        with redirect_stderr(StringIO()):
            with self.assertRaises(SystemExit):
                main(
                    ["one-shot", "--expected-message-id", "42"],
                    environ=config_env(),
                    runtime_factory=factory,  # type: ignore[arg-type]
                    output=StringIO(),
                )
        self.assertEqual(factory_calls, 0)

    def test_locked_run_once_and_confirmed_one_shot_emit_sanitized_json(
        self,
    ) -> None:
        runtime = FakeRuntime()
        output = StringIO()
        code = main(
            ["run-once"],
            environ=config_env(),
            runtime_factory=lambda config: runtime,  # type: ignore[arg-type]
            output=output,
        )
        self.assertEqual(code, 0)
        self.assertEqual(
            json.loads(output.getvalue()),
            {
                "executed": False,
                "reason": "claim_disabled",
                "success": True,
            },
        )

        output = StringIO()
        code = main(
            [
                "one-shot",
                "--expected-message-id",
                "42",
                "--confirm-one-shot",
            ],
            environ=config_env(),
            runtime_factory=lambda config: runtime,  # type: ignore[arg-type]
            output=output,
        )
        self.assertEqual(code, 0)
        self.assertEqual(runtime.one_shot_ids, ["42"])
        rendered = output.getvalue()
        self.assertNotIn(config_env()["SUPABASE_SECRET_KEY"], rendered)
        self.assertNotIn(config_env()["BIZPPURIO_PASSWORD"], rendered)

    def test_synthetic_callback_defaults_to_parse_only_redacted_summary(
        self,
    ) -> None:
        payload = {
            "CMSGID": "provider-request-private",
            "REFKEY": "1234567812344abc8def1234567890ab",
            "MSGID": "provider-result-private",
            "UNIXTIME": "1785038400",
            "RESULT": "7000",
            "MEDIA": "AT",
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "callback.json"
            path.write_text(
                json.dumps(payload),
                encoding="utf-8",
            )
            runtime = FakeRuntime()
            output = StringIO()
            code = main(
                ["synthetic-callback", "--input", str(path)],
                environ=config_env(),
                runtime_factory=lambda config: runtime,  # type: ignore[arg-type]
                output=output,
            )
        self.assertEqual(code, 0)
        summary = json.loads(output.getvalue())
        self.assertEqual(summary["channel"], "alimtalk")
        self.assertTrue(summary["delivered"])
        self.assertNotIn("CMSGID", output.getvalue())
        self.assertNotIn("provider-request-private", output.getvalue())
        self.assertEqual(runtime.callback_calls, 0)

    def test_synthetic_callback_apply_never_sends_when_fallback_disabled(
        self,
    ) -> None:
        code, output, database, provider = run_synthetic_callback_apply(
            fallback_enabled=False
        )

        self.assertEqual(code, 0)
        self.assertEqual(
            json.loads(output.getvalue()),
            {"action": "failed"},
        )
        self.assertEqual(database.rejections, 1)
        self.assertEqual(provider.calls, 0)

    def test_synthetic_callback_apply_never_sends_when_fallback_enabled(
        self,
    ) -> None:
        code, output, database, provider = run_synthetic_callback_apply(
            fallback_enabled=True
        )

        self.assertEqual(provider.calls, 0)
        self.assertEqual(code, 0)
        self.assertEqual(
            json.loads(output.getvalue()),
            {"action": "failed"},
        )
        self.assertEqual(database.rejections, 1)
        self.assertEqual(database.started, 0)
        self.assertEqual(database.waiting, 0)

    def test_reconcile_callback_command_emits_only_sanitized_outcome(
        self,
    ) -> None:
        runtime = FakeRuntime()
        output = StringIO()

        code = main(
            [
                "reconcile-callback",
                "--provider-request-id",
                "PRIVATE_PROVIDER_REQUEST_SENTINEL",
            ],
            environ=config_env(),
            runtime_factory=lambda config: runtime,  # type: ignore[arg-type]
            output=output,
        )

        self.assertEqual(code, 0)
        self.assertEqual(
            runtime.reconcile_ids,
            ["PRIVATE_PROVIDER_REQUEST_SENTINEL"],
        )
        self.assertEqual(
            json.loads(output.getvalue()),
            {
                "executed": True,
                "reason": "report_requested",
                "success": True,
            },
        )
        self.assertNotIn(
            "PRIVATE_PROVIDER_REQUEST_SENTINEL",
            output.getvalue(),
        )


if __name__ == "__main__":
    unittest.main()
