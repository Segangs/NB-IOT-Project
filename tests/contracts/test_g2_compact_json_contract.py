import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CONTRACT = ROOT / "contracts" / "g2" / "mqtt_compact_json_v1.json"


class G2CompactJsonContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = json.loads(CONTRACT.read_text(encoding="utf-8"))

    def test_latest_decision_supersedes_csv_without_rewriting_g1_history(self):
        self.assertEqual(self.data["contract_id"], "nb_iot.g2.mqtt_compact_json")
        self.assertEqual(self.data["schema_version"], 1)
        self.assertEqual(self.data["encoding"], "compact_json")
        self.assertEqual(
            self.data["supersedes"],
            {
                "contract_id": "nb_iot.g1.mqtt_db",
                "scope": "wire_encoding_and_payload_shapes_only",
                "reason": "2026-07-25_user_decision_keep_current_compact_json",
            },
        )
        self.assertFalse(self.data["numeric_csv_allowed"])
        self.assertFalse(self.data["dual_decoder_allowed"])
        self.assertFalse(self.data["raw_binary_allowed"])

    def test_existing_payload_shapes_are_unchanged(self):
        existing = self.data["existing_payloads"]
        self.assertEqual(
            existing["telemetry"]["fields"],
            ["sensor_id", "value_celsius"],
        )
        self.assertEqual(
            existing["boot"]["fields"],
            [
                "voltage",
                "pico_temperature",
                "flash_integrity",
                "reserved_zero",
                "at_status",
                "cpin_status",
                "csq",
                "carrier_code",
                "temp1_status",
                "temp2_status",
                "mic1_status",
                "mic2_status",
                "boot_reason",
                "last_cmd_id",
            ],
        )
        self.assertEqual(existing["config_request"]["fixed_payload"], "{}")
        self.assertEqual(
            existing["config_response"]["fields"],
            ["temp1_upper_celsius", "temp2_upper_celsius"],
        )

    def test_command_and_ack_payload_shapes_are_exact(self):
        payloads = self.data["command_payloads"]
        expected = {
            "command_request": ["request_id", "last_cmd_id"],
            "command_response": [
                "request_id",
                "cmd_id",
                "opcode",
                "job_id",
                "ttl_seconds",
            ],
            "command_ack": [
                "cmd_id",
                "phase",
                "result",
                "error",
                "unix_seconds",
                "clock_valid",
            ],
            "command_ack_receipt": [
                "cmd_id",
                "phase",
                "result",
                "receipt",
                "error",
            ],
        }
        self.assertEqual(set(payloads), set(expected))
        for family, fields in expected.items():
            self.assertEqual(payloads[family]["fields"], fields, family)

    def test_all_golden_payloads_are_minified_numeric_json_and_within_limit(self):
        max_bytes = self.data["max_payload_bytes"]
        for group_name in ("existing_payloads", "command_payloads"):
            for family, payload in self.data[group_name].items():
                if family == "config_request":
                    encoded = payload["fixed_payload"].encode("ascii")
                    self.assertEqual(json.loads(encoded), {})
                else:
                    golden = payload["max_golden_values"]
                    encoded = json.dumps(
                        golden, separators=(",", ":"), ensure_ascii=True
                    ).encode("ascii")
                    self.assertEqual(
                        encoded.decode("ascii"),
                        payload["max_golden_payload"],
                        family,
                    )
                    self.assertIsInstance(json.loads(encoded), list)
                    self.assertTrue(
                        all(
                            isinstance(value, (int, float))
                            and not isinstance(value, bool)
                            for value in golden
                        ),
                        family,
                    )
                self.assertNotIn(b" ", encoded, family)
                self.assertLessEqual(len(encoded), max_bytes, family)

    def test_topics_are_separate_and_non_retained(self):
        topics = self.data["topics"]
        self.assertEqual(
            topics["command_request"], "devices/{imei}/cmd/request"
        )
        self.assertEqual(
            topics["command_response"], "devices/{imei}/cmd/response"
        )
        self.assertEqual(topics["command_ack"], "devices/{imei}/cmd/ack")
        self.assertEqual(
            topics["command_ack_receipt"],
            "devices/{imei}/cmd/ack/receipt",
        )
        self.assertEqual(set(self.data["retained"]), set(topics))
        self.assertTrue(
            all(retained is False for retained in self.data["retained"].values())
        )

    def test_command_enums_and_state_flow_are_exact(self):
        enums = self.data["command_enums"]
        self.assertEqual(
            enums["opcode"],
            {"none": 0, "reboot": 1, "power_off": 2,
             "request_status": 3, "fota_prepare": 4},
        )
        self.assertEqual(enums["phase"], {"accepted": 1, "final": 2})
        self.assertEqual(
            enums["result"],
            {"none": 0, "accepted": 1, "executed": 2,
             "failed": 3, "expired": 4},
        )
        self.assertEqual(
            self.data["database_state_flow"],
            ["queued", "delivered", "accepted",
             "executed_or_failed_or_expired"],
        )
        self.assertEqual(self.data["dedupe_key"], "cmd_id")
        self.assertEqual(self.data["claim_count_per_request"], 1)

    def test_safety_rules_keep_config_and_command_separate(self):
        safety = self.data["safety"]
        self.assertTrue(safety["imei_from_authenticated_context"])
        self.assertTrue(safety["config_command_separate"])
        self.assertTrue(safety["accepted_before_destructive_execute"])
        self.assertTrue(safety["single_dispatch_latch"])
        self.assertTrue(safety["final_receipt_before_journal_clear"])
        self.assertTrue(safety["stale_sensor_telemetry_forbidden"])
        self.assertTrue(safety["invalid_numeric_sentinel_forbidden"])


if __name__ == "__main__":
    unittest.main()
