import json
import unittest

from emqx_config_republish import (
    build_config_publish,
    build_emqx_auth_header,
    is_valid_imei,
    normalize_config_payload,
)


class EmqxConfigRepublishTest(unittest.TestCase):
    def test_rejects_invalid_imei(self):
        self.assertFalse(is_valid_imei(""))
        self.assertFalse(is_valid_imei("undefined"))
        self.assertFalse(is_valid_imei("354720510314300/other"))

    def test_builds_two_value_compact_json_in_sensor_order(self):
        body = build_config_publish(
            "354720510314300",
            [
                {"userSensorId": 2, "sensorCtgyType": "TMP", "setTmpUpLimit": -10.0},
                {"userSensorId": 1, "sensorCtgyType": "TMP", "setTmpUpLimit": -7},
            ],
        )
        self.assertEqual(body["payload"], "[-7,-10]")
        self.assertEqual(len(body["payload"].encode("ascii")), 8)

    def test_rejects_incomplete_duplicate_or_non_finite_limits(self):
        invalid = [
            [{"userSensorId": 1, "sensorCtgyType": "TMP", "setTmpUpLimit": -7}],
            [
                {"userSensorId": 1, "sensorCtgyType": "TMP", "setTmpUpLimit": -7},
                {"userSensorId": 1, "sensorCtgyType": "TMP", "setTmpUpLimit": -10},
            ],
            [
                {"userSensorId": 1, "sensorCtgyType": "TMP", "setTmpUpLimit": True},
                {"userSensorId": 2, "sensorCtgyType": "TMP", "setTmpUpLimit": -10},
            ],
            [
                {"userSensorId": 1, "sensorCtgyType": "TMP", "setTmpUpLimit": float("nan")},
                {"userSensorId": 2, "sensorCtgyType": "TMP", "setTmpUpLimit": -10},
            ],
            [
                {"userSensorId": 1, "sensorCtgyType": "TMP", "setTmpUpLimit": float("inf")},
                {"userSensorId": 2, "sensorCtgyType": "TMP", "setTmpUpLimit": -10},
            ],
            [
                {"userSensorId": 1, "sensorCtgyType": "TMP", "setTmpUpLimit": float("-inf")},
                {"userSensorId": 2, "sensorCtgyType": "TMP", "setTmpUpLimit": -10},
            ],
            [
                {"userSensorId": 1, "sensorCtgyType": "TMP", "setTmpUpLimit": "-7"},
                {"userSensorId": 2, "sensorCtgyType": "TMP", "setTmpUpLimit": -10},
            ],
        ]
        for config in invalid:
            with self.subTest(config=config):
                self.assertIsNone(build_config_publish("354720510314300", config))

    def test_rejects_limits_outside_finite_binary32_range(self):
        for limit in (1e39, -1e39, 10**400):
            with self.subTest(limit=limit):
                self.assertIsNone(
                    build_config_publish(
                        "354720510314300",
                        [
                            {
                                "userSensorId": 1,
                                "sensorCtgyType": "TMP",
                                "setTmpUpLimit": limit,
                            },
                            {
                                "userSensorId": 2,
                                "sensorCtgyType": "TMP",
                                "setTmpUpLimit": -10,
                            },
                        ],
                    )
                )

    def test_rejects_payload_over_modem_80_byte_limit(self):
        max_binary32_integer = (2**24 - 1) * 2**104
        config = [
            {
                "userSensorId": 1,
                "sensorCtgyType": "TMP",
                "setTmpUpLimit": max_binary32_integer,
            },
            {
                "userSensorId": 2,
                "sensorCtgyType": "TMP",
                "setTmpUpLimit": max_binary32_integer,
            },
        ]

        self.assertEqual(
            normalize_config_payload(config),
            [max_binary32_integer, max_binary32_integer],
        )
        oversized_payload = json.dumps(
            [max_binary32_integer, max_binary32_integer],
            separators=(",", ":"),
        )
        self.assertEqual(len(oversized_payload.encode("ascii")), 81)
        self.assertIsNone(build_config_publish("354720510314300", config))

    def test_rejects_empty_or_null_config_payloads(self):
        self.assertIsNone(build_config_publish("354720510314300", None))
        self.assertIsNone(build_config_publish("354720510314300", []))
        self.assertIsNone(build_config_publish("354720510314300", {}))
        self.assertIsNone(build_config_publish("354720510314300", "undefined"))

    def test_builds_basic_auth_header_from_api_key_secret(self):
        self.assertEqual(
            build_emqx_auth_header(api_key="key", api_secret="secret"),
            "Basic a2V5OnNlY3JldA==",
        )
        self.assertEqual(
            build_emqx_auth_header(auth_header="Bearer token", api_key="key", api_secret="secret"),
            "Bearer token",
        )


if __name__ == "__main__":
    unittest.main()
