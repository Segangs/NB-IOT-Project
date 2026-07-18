import json
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FIRMWARE_CONTRACT = ROOT / "contracts" / "g1" / "firmware_runtime_v1.json"
MQTT_DB_CONTRACT = ROOT / "contracts" / "g1" / "mqtt_db_v1.json"
NUMERIC_CSV = re.compile(
    r"^(?:0|[1-9][0-9]*|-[1-9][0-9]*)"
    r"(?:,(?:0|[1-9][0-9]*|-[1-9][0-9]*))*$"
)


def enum_contract_errors(actual, expected):
    errors = []
    if actual != expected:
        errors.append("exact_mapping")
    if len(actual.values()) != len(set(actual.values())):
        errors.append("numeric_uniqueness")
    return errors


EXPECTED_CROSS_CONTRACT_REFS = {
    "health_status": {
        "contract_id": "nb_iot.g1.firmware_runtime",
        "path": "sensor.health_codes",
    },
    "event_type": {
        "contract_id": "nb_iot.g1.firmware_runtime",
        "path": "event_types",
    },
    "power_incident_lifecycle": {
        "contract_id": "nb_iot.g1.firmware_runtime",
        "path": "power.incident_lifecycle",
    },
}


def resolve_contract_ref(ref, contracts):
    if not isinstance(ref, dict) or set(ref) != {"contract_id", "path"}:
        raise ValueError("invalid_contract_ref_shape")
    contract_id = ref["contract_id"]
    if contract_id not in contracts:
        raise ValueError("unresolved_contract_id")
    value = contracts[contract_id]
    path = ref["path"]
    if not isinstance(path, str) or not path:
        raise ValueError("invalid_contract_path")
    for part in path.split("."):
        if not isinstance(value, dict) or part not in value:
            raise ValueError("unresolved_contract_path")
        value = value[part]
    return value


class G1CrossContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.firmware = json.loads(FIRMWARE_CONTRACT.read_text(encoding="utf-8"))
        cls.mqtt = json.loads(MQTT_DB_CONTRACT.read_text(encoding="utf-8"))
        cls.contracts = {cls.firmware["contract_id"]: cls.firmware}

    def test_contract_schema_versions_match(self):
        self.assertEqual(self.firmware["schema_version"], 1)
        self.assertEqual(self.mqtt["schema_version"], 1)

    def test_event_name_to_code_mapping_matches_exactly(self):
        ref = self.mqtt["cross_contract_refs"]["event_type"]
        self.assertIsInstance(ref, dict)
        firmware_events = resolve_contract_ref(ref, self.contracts)
        mqtt_events = self.mqtt["wire_enums"]["event_type"]
        event_field = self.mqtt["payloads"]["event"]["field_specs"]["event_type"]
        self.assertEqual(mqtt_events, firmware_events)
        self.assertEqual(event_field["enum_ref"], "event_type")
        self.assertNotIn("enum", event_field)
        self.assertEqual(set(mqtt_events.values()), set(firmware_events.values()))

    def test_event_enum_has_no_duplicate_mqtt_alias(self):
        self.assertNotIn("event_types", self.mqtt)
        self.assertEqual(
            self.mqtt["event_semantics"]["enum_refs"]["event_type"],
            "event_type",
        )

    def test_sensor_health_mapping_matches_exactly_and_rejects_mutations(self):
        ref = self.mqtt["cross_contract_refs"]["health_status"]
        self.assertIsInstance(ref, dict)
        firmware_health = resolve_contract_ref(ref, self.contracts)
        mqtt_health = self.mqtt["wire_enums"]["health_status"]
        self.assertEqual(mqtt_health, firmware_health)
        self.assertEqual(enum_contract_errors(mqtt_health, firmware_health), [])

        swapped = dict(mqtt_health, **{"pass": 2, "degraded": 1})
        self.assertIn("exact_mapping", enum_contract_errors(swapped, firmware_health))
        duplicated = dict(mqtt_health, **{"degraded": mqtt_health["pass"]})
        self.assertIn(
            "numeric_uniqueness", enum_contract_errors(duplicated, firmware_health)
        )

    def test_power_incident_lifecycle_reference_and_continuity_match(self):
        ref = self.mqtt["cross_contract_refs"]["power_incident_lifecycle"]
        self.assertIsInstance(ref, dict)
        firmware_lifecycle = resolve_contract_ref(ref, self.contracts)
        mqtt_continuity = self.mqtt["event_semantics"]["power_incident_continuity"]
        self.assertEqual(mqtt_continuity["firmware_contract_ref"], ref)
        self.assertEqual(
            mqtt_continuity["correlated_events"],
            firmware_lifecycle["correlated_events"],
        )
        self.assertEqual(
            mqtt_continuity["same_open_incident_id_required"],
            firmware_lifecycle["open_incident_id_reuse_required"],
        )
        self.assertEqual(
            mqtt_continuity["server_offline_max_per_open_incident"], 1
        )
        self.assertEqual(
            firmware_lifecycle["server_offline"]["cardinality_per_power_incident"],
            "once",
        )
        self.assertTrue(firmware_lifecycle["server_offline"]["duplicate_suppressed"])
        self.assertEqual(
            mqtt_continuity["no_open_power_fallback"],
            firmware_lifecycle["server_offline"]["without_open_power_incident"],
        )
        self.assertTrue(
            firmware_lifecycle["sequence"]["producer_origin_required_for_dedupe"]
        )

        mutation = json.loads(json.dumps(mqtt_continuity))
        mutation["same_open_incident_id_required"] = False
        self.assertNotEqual(
            mutation["same_open_incident_id_required"],
            firmware_lifecycle["open_incident_id_reuse_required"],
        )

    def test_cross_contract_refs_resolve_and_reject_bad_mutations(self):
        refs = self.mqtt.get("cross_contract_refs")
        self.assertEqual(refs, EXPECTED_CROSS_CONTRACT_REFS)
        if refs != EXPECTED_CROSS_CONTRACT_REFS:
            return
        self.assertEqual(
            resolve_contract_ref(refs["health_status"], self.contracts),
            self.firmware["sensor"]["health_codes"],
        )
        self.assertEqual(
            resolve_contract_ref(refs["event_type"], self.contracts),
            self.firmware["event_types"],
        )
        self.assertEqual(
            resolve_contract_ref(refs["power_incident_lifecycle"], self.contracts),
            self.firmware["power"]["incident_lifecycle"],
        )

        wrong_contract = dict(
            refs["health_status"], contract_id="nb_iot.g1.unknown"
        )
        with self.assertRaisesRegex(ValueError, "unresolved_contract_id"):
            resolve_contract_ref(wrong_contract, self.contracts)
        wrong_path = dict(refs["health_status"], path="sensor.missing_codes")
        with self.assertRaisesRegex(ValueError, "unresolved_contract_path"):
            resolve_contract_ref(wrong_path, self.contracts)

    def test_power_and_sensor_event_owners_match_firmware_services(self):
        schemas = self.mqtt["event_semantics"]["schemas"]
        lifecycle_ref = self.mqtt["cross_contract_refs"][
            "power_incident_lifecycle"
        ]
        self.assertIsInstance(lifecycle_ref, dict)
        power_owner = resolve_contract_ref(lifecycle_ref, self.contracts)["owner"]
        for event_type in (
            "adapter_removed",
            "adapter_restored",
            "poweroff_dying",
        ):
            self.assertEqual(schemas[event_type]["owner"], power_owner, event_type)
        self.assertEqual(
            schemas["sensor_fault"]["owner"],
            self.firmware["ownership"]["one_wire_i2s"],
        )

    def test_every_payload_starts_with_schema_version(self):
        for family, payload in self.mqtt["payloads"].items():
            self.assertEqual(payload["fields"][0], "schema_version", family)
            schema = payload["field_specs"]["schema_version"]
            self.assertEqual((schema["minimum"], schema["maximum"]), (1, 1))

    def test_topics_and_rpc_names_have_no_duplicates(self):
        topics = list(self.mqtt["topics"].values())
        rpc_names = [item["name"] for item in self.mqtt["rpc"].values()]
        self.assertEqual(len(topics), len(set(topics)))
        self.assertEqual(len(rpc_names), len(set(rpc_names)))

    def test_golden_csv_field_counts_and_byte_limits_match_contract(self):
        for family, payload in self.mqtt["payloads"].items():
            golden = payload["max_golden_payload"]
            encoded = golden.encode("ascii")
            self.assertRegex(golden, NUMERIC_CSV, family)
            self.assertEqual(len(golden.split(",")), len(payload["fields"]), family)
            self.assertEqual(payload["max_golden_payload_bytes"], len(encoded), family)
            self.assertLessEqual(len(encoded), self.mqtt["max_payload_bytes"], family)


if __name__ == "__main__":
    unittest.main()
