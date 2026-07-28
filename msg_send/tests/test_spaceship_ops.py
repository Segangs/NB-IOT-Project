from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class SpaceshipOperationsContractTests(unittest.TestCase):
    def test_single_flock_cycle_drains_callback_before_sending(
        self,
    ) -> None:
        script = (
            ROOT / "ops/spaceship/msg-send-run-once.sh"
        ).read_text(encoding="utf-8")

        drain = script.index("drain-callback-spool")
        send = script.index("run-once", drain + 1)
        self.assertLess(drain, send)
        self.assertIn(
            "/home/yjijjnuzbr/project/msg_send_webhook_spool",
            script,
        )
        self.assertNotIn("|| true", script)

    def test_callback_drain_requires_explicit_apply_gate(self) -> None:
        script = (
            ROOT / "ops/spaceship/msg-send-run-once.sh"
        ).read_text(encoding="utf-8")

        self.assertIn(
            'if [ "${MSG_SEND_CALLBACK_APPLY_ENABLED:-false}" '
            '= "true" ]; then',
            script,
        )

    def test_runtime_defaults_to_callback_without_enabling_apply(
        self,
    ) -> None:
        script = (
            ROOT / "ops/spaceship/run_msg_send.sh"
        ).read_text(encoding="utf-8")

        self.assertIn(
            'MSG_SEND_RESULT_MODE="${MSG_SEND_RESULT_MODE:-callback}"',
            script,
        )
        self.assertIn(
            'MSG_SEND_CALLBACK_APPLY_ENABLED="${'
            'MSG_SEND_CALLBACK_APPLY_ENABLED:-false}"',
            script,
        )

    def test_cron_keeps_one_shared_flock(self) -> None:
        cron = (
            ROOT / "ops/spaceship/msg-send.cron.example"
        ).read_text(encoding="utf-8")

        self.assertEqual(cron.count("/usr/bin/flock -n"), 1)
        self.assertIn("msg-send-run-once.sh", cron)
        self.assertTrue(cron.startswith("* * * * * "))

    def test_runtime_exports_bounded_sequential_drain_defaults(
        self,
    ) -> None:
        script = (
            ROOT / "ops/spaceship/run_msg_send.sh"
        ).read_text(encoding="utf-8")

        self.assertIn(
            'MSG_SEND_MAX_MESSAGES_PER_RUN="${'
            'MSG_SEND_MAX_MESSAGES_PER_RUN:-20}"',
            script,
        )
        self.assertIn(
            'MSG_SEND_MAX_CYCLE_SECONDS="${'
            'MSG_SEND_MAX_CYCLE_SECONDS:-50}"',
            script,
        )


if __name__ == "__main__":
    unittest.main()
