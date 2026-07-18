import json
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CONTRACT = ROOT / "contracts" / "g1" / "mqtt_db_v1.json"
CANONICAL_TOKEN_PATTERN = r"^(?:0|[1-9][0-9]*|-[1-9][0-9]*)$"
CANONICAL_TOKEN = re.compile(CANONICAL_TOKEN_PATTERN)
NUMERIC_CSV = re.compile(
    r"^(?:0|[1-9][0-9]*|-[1-9][0-9]*)"
    r"(?:,(?:0|[1-9][0-9]*|-[1-9][0-9]*))*$"
)

UINT32_MAX = 4_294_967_295


def longest_canonical_endpoint(spec):
    endpoints = (spec["minimum"], spec["maximum"])
    return max(
        endpoints,
        key=lambda value: (len(str(value)), value == spec["maximum"]),
    )


def crc32_iso_hdlc(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 if crc & 1 else 0)
    return (crc ^ 0xFFFFFFFF) & 0xFFFFFFFF


def is_config_no_change_response(request, payload):
    if request["page_index"] != 0 or not NUMERIC_CSV.fullmatch(payload):
        return False
    values = [int(token) for token in payload.split(",")]
    expected = [
        1,
        request["request_id"],
        request["known_config_version"],
        0,
        1,
        0,
        0,
        0,
        0,
        0,
        0,
    ]
    return values == expected


def is_command_none_response(request, payload):
    if not NUMERIC_CSV.fullmatch(payload):
        return False
    values = [int(token) for token in payload.split(",")]
    return values == [1, request["request_id"], 0, 0, 0, 0]


def journal_graph_errors(state_enum, recovery):
    known_states = set(state_enum)
    nonempty_states = known_states - {"empty"}
    expected_entry_fields = {"event", "action", "next_state", "reexecute_allowed"}
    errors = []

    tables = {
        "normal": recovery.get("normal_transitions", {}),
        "boot": recovery.get("boot_recovery_transitions", {}),
    }
    for table_name, table in tables.items():
        if set(table) != nonempty_states:
            errors.append(f"{table_name}_coverage")
        for state, entry in table.items():
            if set(entry) != expected_entry_fields:
                errors.append(f"{table_name}_{state}_shape")
                continue
            if entry["next_state"] not in known_states:
                errors.append(f"{table_name}_{state}_next_state")
            if not isinstance(entry["reexecute_allowed"], bool):
                errors.append(f"{table_name}_{state}_reexecute_type")
            elif entry["reexecute_allowed"]:
                errors.append(f"{table_name}_{state}_reexecute_enabled")

    normal = tables["normal"]
    state = "accepted_persisted"
    visited = set()
    while state != "empty" and state in normal and state not in visited:
        visited.add(state)
        state = normal[state].get("next_state")
    if state != "empty":
        errors.append("normal_path_not_terminal")

    clear_sources = set()
    for table in tables.values():
        for state, entry in table.items():
            if entry.get("action") == "clear_journal" or entry.get("next_state") == "empty":
                clear_sources.add(state)
    if clear_sources != {"final_receipted"}:
        errors.append("clear_source")

    execute_marked_boot = tables["boot"].get("execute_marked", {})
    if execute_marked_boot.get("reexecute_allowed") is not False:
        errors.append("execute_marked_boot_reexecute")
    if "dispatch" in execute_marked_boot.get("action", ""):
        errors.append("execute_marked_boot_dispatch")
    return errors


def mqtt_identity_outcome(binding, authenticated_imei, topic_imei):
    if authenticated_imei.encode("ascii") == topic_imei.encode("ascii"):
        return {
            "route": True,
            "p_imei": authenticated_imei,
            "rpc": True,
            "republish": True,
            "audit_increment": False,
        }
    mismatch = binding["mismatch"]
    return {
        "route": mismatch["action"] != "drop",
        "p_imei": None,
        "rpc": mismatch["rpc_allowed"],
        "republish": mismatch["response_republish_allowed"],
        "audit_increment": mismatch["security_audit_counter_increment"],
    }


def destructive_ttl_decision(checkpoint, current):
    if checkpoint["opcode"] != "none" and checkpoint["ttl_seconds"] <= 0:
        return {"decision": "invalid", "remaining_ttl_seconds": 0}
    if checkpoint["opcode"] == "none":
        return {
            "decision": "no_command" if checkpoint["ttl_seconds"] == 0 else "invalid",
            "remaining_ttl_seconds": 0,
        }

    remaining = checkpoint["remaining_ttl_seconds"]
    if current["boot_sequence"] == checkpoint["checkpoint_boot_sequence"]:
        elapsed = current["monotonic_seconds"] - checkpoint["checkpoint_monotonic_seconds"]
        if elapsed < 0:
            return {"decision": "expired", "remaining_ttl_seconds": 0}
    else:
        if not (
            checkpoint["checkpoint_clock_valid"]
            and current["clock_valid"]
            and current["unix_seconds"] >= checkpoint["checkpoint_unix_seconds"]
        ):
            return {"decision": "expired", "remaining_ttl_seconds": 0}
        elapsed = current["unix_seconds"] - checkpoint["checkpoint_unix_seconds"]

    remaining = max(0, remaining - elapsed)
    return {
        "decision": "dispatch" if remaining > 0 else "expired",
        "remaining_ttl_seconds": remaining,
    }


def dispatch_latch_action(dispatch_latched):
    if dispatch_latched:
        return "ignore_duplicate_no_dispatch"
    return "atomically_persist_latch_then_dispatch_once"


def enum_contract_errors(actual, expected):
    errors = []
    if actual != expected:
        errors.append("exact_mapping")
    if len(actual.values()) != len(set(actual.values())):
        errors.append("numeric_uniqueness")
    return errors


def event_schema_errors(actual, expected):
    errors = []
    if set(actual) != set(expected):
        errors.append("coverage")
    for event_name, expected_schema in expected.items():
        schema = actual.get(event_name)
        if schema != expected_schema:
            errors.append(event_name)
    return errors


def resolve_dotted_path(document, path):
    value = document
    for part in path.split("."):
        value = value[part]
    return value


def device_event_gateway_outcome(binding, schemas, event_type):
    policy = binding["device_event_route"]
    schema_route_matches = (
        schemas[event_type]["producer_route"]
        == policy["required_schema_producer_route"]
    )
    allowed = event_type in policy["allowed_event_types"] and schema_route_matches
    if allowed:
        return {"action": "route", "rpc_allowed": True}
    return policy["gateway_reject"]


def event_dedupe_key(idempotency_key, **values):
    return tuple(values[field] for field in idempotency_key)


def conversion_accepts(rule, value):
    if rule.get("input_mapping") is not None:
        return value in rule["input_mapping"]
    if rule.get("accepted_postgres_types") is not None:
        if isinstance(value, bool):
            value_type = "boolean"
        elif isinstance(value, int):
            value_type = "integer"
        else:
            value_type = "text"
        return value_type in rule["accepted_postgres_types"]
    return bool(CANONICAL_TOKEN.fullmatch(value)) if isinstance(value, str) else False

PAYLOAD_FIELDS = {
    "telemetry": ["schema_version", "sensor_id", "value_tenths", "sequence"],
    "boot": [
        "schema_version",
        "cimi",
        "voltage_mv",
        "boot_temp_tenths",
        "flash_status",
        "ram_status",
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
    "config_request": [
        "schema_version",
        "request_id",
        "known_config_version",
        "page_index",
    ],
    "config_response": [
        "schema_version",
        "request_id",
        "config_version",
        "page_index",
        "page_count",
        "sensor_slot",
        "sensor_id",
        "upper_tenths",
        "lower_tenths",
        "enabled",
        "checksum_u32",
    ],
    "command_request": ["schema_version", "request_id", "last_cmd_id"],
    "command_response": [
        "schema_version",
        "request_id",
        "cmd_id",
        "opcode",
        "job_ref",
        "ttl_seconds",
    ],
    "config_ack": [
        "schema_version",
        "request_id",
        "config_version",
        "result_code",
        "error_code",
    ],
    "command_ack": [
        "schema_version",
        "cmd_id",
        "phase",
        "result_code",
        "error_code",
        "unix_seconds",
        "clock_valid",
    ],
    "command_ack_receipt": [
        "schema_version",
        "cmd_id",
        "phase",
        "result_code",
        "receipt_code",
        "error_code",
    ],
    "event": [
        "schema_version",
        "event_type",
        "incident_id",
        "sequence",
        "state_code",
        "value0",
        "value1",
        "unix_seconds",
        "clock_valid",
    ],
}

TOPICS = {
    "boot": "devices/{imei}/boot",
    "telemetry": "devices/{imei}/telemetry",
    "config_request": "devices/{imei}/config/request",
    "config_response": "devices/{imei}/config/response",
    "command_request": "devices/{imei}/cmd/request",
    "command_response": "devices/{imei}/cmd/response",
    "config_ack": "devices/{imei}/config/ack",
    "command_ack": "devices/{imei}/cmd/ack",
    "command_ack_receipt": "devices/{imei}/cmd/ack/receipt",
    "event": "devices/{imei}/event",
}

RPC_SIGNATURES = {
    "device_auth": {
        "name": "auth_device",
        "arguments": ["username text", "password text"],
    },
    "telemetry_ingest": {
        "name": "t",
        "arguments": [
            "p_imei text",
            "p_schema_version integer",
            "p_user_sensor_id bigint",
            "p_value_tenths integer",
            "p_sequence bigint",
        ],
    },
    "boot_ingest": {
        "name": "b",
        "arguments": [
            "p_imei text",
            "p_cimi text",
            "p_schema_version integer",
            "p_voltage_mv integer",
            "p_boot_temp_tenths integer",
            "p_flash_status integer",
            "p_ram_status integer",
            "p_at_status integer",
            "p_cpin_status integer",
            "p_csq integer",
            "p_carrier_code integer",
            "p_temp1_status integer",
            "p_temp2_status integer",
            "p_mic1_status integer",
            "p_mic2_status integer",
            "p_boot_reason integer",
            "p_last_cmd_id bigint",
        ],
    },
    "config_fetch": {
        "name": "get_device_config_page",
        "arguments": [
            "p_imei text",
            "p_request_id bigint",
            "p_known_config_version bigint",
            "p_page_index integer",
        ],
    },
    "command_claim": {
        "name": "claim_device_command",
        "arguments": [
            "p_imei text",
            "p_request_id bigint",
            "p_last_cmd_id bigint",
            "p_lease_seconds integer",
        ],
    },
    "config_ack": {
        "name": "ack_device_config",
        "arguments": [
            "p_imei text",
            "p_request_id bigint",
            "p_config_version bigint",
            "p_result_code integer",
            "p_error_code integer",
        ],
    },
    "command_ack": {
        "name": "ack_device_command",
        "arguments": [
            "p_imei text",
            "p_cmd_id bigint",
            "p_phase integer",
            "p_result_code integer",
            "p_error_code integer",
            "p_unix_seconds bigint",
            "p_clock_valid boolean",
        ],
    },
    "event_ingest": {
        "name": "ingest_device_event",
        "arguments": [
            "p_imei text",
            "p_event_type integer",
            "p_incident_id bigint",
            "p_sequence bigint",
            "p_state_code integer",
            "p_value0 bigint",
            "p_value1 bigint",
            "p_unix_seconds bigint",
            "p_clock_valid boolean",
        ],
    },
}

EXPECTED_IDENTITY_BINDING = {
    "authenticated_username_identity": "imei_text",
    "topic_identity_placeholder": "{imei}",
    "topic_imei_must_equal_authenticated_imei": True,
    "comparison": "byte_for_byte",
    "p_imei_source": "authenticated_identity_after_topic_match",
    "unmatched_topic_p_imei_allowed": False,
    "mismatch": {
        "action": "drop",
        "rpc_allowed": False,
        "response_republish_allowed": False,
        "security_audit_counter_increment": True,
    },
    "acl": {
        "device_publish_scope": "own_device_topic_families_only",
        "device_subscribe_scope": "own_device_topic_families_only",
        "device_publish_topic_families": [
            "boot",
            "telemetry",
            "config_request",
            "command_request",
            "config_ack",
            "command_ack",
            "event",
        ],
        "device_subscribe_topic_families": [
            "config_response",
            "command_response",
            "command_ack_receipt",
        ],
    },
    "device_event_route": {
        "topic_family": "event",
        "allowed_event_types": [
            "temperature_high",
            "temperature_low",
            "temperature_recovered",
            "adapter_removed",
            "adapter_restored",
            "poweroff_dying",
            "sensor_fault",
            "fota_status",
            "ai_status",
        ],
        "required_schema_producer_route": "device_mqtt_event",
        "gateway_reject": {"action": "drop", "rpc_allowed": False},
    },
    "device_event_route_server_impersonation_allowed": False,
}

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

EXPECTED_SERVER_EVENT_ROUTE = {
    "device_offline": {
        "producer": "authorized_server_monitor",
        "transport": "direct_rpc",
        "mqtt_topic": None,
        "device_authenticated_mqtt_route_allowed": False,
        "authorization": "server_authorized_device_scope",
        "identity_source": "authorized_server_device_scope",
        "identity_conversion_ref": "server_authorized_imei_to_pg_text",
        "payload_semantics_ref": "event_semantics.schemas.device_offline",
        "rpc": "event_ingest",
        "live_sql_or_policy_frozen": False,
    }
}

EXPECTED_EVENT_SCHEMAS = {
    "temperature_high": {
        "owner": "temperature_alarm_service",
        "producer_route": "device_mqtt_event",
        "allowed_state_codes": ["active"],
        "time_source": "device_unix_seconds_with_clock_valid",
        "value0": {
            "meaning": "measured_temperature",
            "unit": "tenths_celsius",
            "minimum": -550,
            "maximum": 1250,
            "zero_rule": "measured_zero_is_valid",
        },
        "value1": {
            "meaning": "upper_alarm_threshold",
            "unit": "tenths_celsius",
            "minimum": -550,
            "maximum": 1250,
            "zero_rule": "zero_threshold_is_valid",
        },
    },
    "temperature_low": {
        "owner": "temperature_alarm_service",
        "producer_route": "device_mqtt_event",
        "allowed_state_codes": ["active"],
        "time_source": "device_unix_seconds_with_clock_valid",
        "value0": {
            "meaning": "measured_temperature",
            "unit": "tenths_celsius",
            "minimum": -550,
            "maximum": 1250,
            "zero_rule": "measured_zero_is_valid",
        },
        "value1": {
            "meaning": "lower_alarm_threshold",
            "unit": "tenths_celsius",
            "minimum": -550,
            "maximum": 1250,
            "zero_rule": "zero_threshold_is_valid",
        },
    },
    "temperature_recovered": {
        "owner": "temperature_alarm_service",
        "producer_route": "device_mqtt_event",
        "allowed_state_codes": ["inactive_or_cleared"],
        "time_source": "device_unix_seconds_with_clock_valid",
        "value0": {
            "meaning": "measured_temperature",
            "unit": "tenths_celsius",
            "minimum": -550,
            "maximum": 1250,
            "zero_rule": "measured_zero_is_valid",
        },
        "value1": {
            "meaning": "recovery_threshold",
            "unit": "tenths_celsius",
            "minimum": -550,
            "maximum": 1250,
            "zero_rule": "zero_threshold_is_valid",
        },
    },
    "adapter_removed": {
        "owner": "power_state_service",
        "producer_route": "device_mqtt_event",
        "allowed_state_codes": ["active"],
        "time_source": "device_unix_seconds_with_clock_valid",
        "value0": {
            "meaning": "adapter_present",
            "unit": "boolean_code",
            "minimum": 0,
            "maximum": 1,
            "zero_rule": "removed_requires_zero",
            "event_value": 0,
        },
        "value1": {
            "meaning": "unused",
            "unit": "unused_zero",
            "minimum": 0,
            "maximum": 0,
            "zero_rule": "required_unused_zero",
        },
    },
    "adapter_restored": {
        "owner": "power_state_service",
        "producer_route": "device_mqtt_event",
        "allowed_state_codes": ["inactive_or_cleared"],
        "time_source": "device_unix_seconds_with_clock_valid",
        "value0": {
            "meaning": "adapter_present",
            "unit": "boolean_code",
            "minimum": 0,
            "maximum": 1,
            "zero_rule": "restored_requires_one",
            "event_value": 1,
        },
        "value1": {
            "meaning": "unused",
            "unit": "unused_zero",
            "minimum": 0,
            "maximum": 0,
            "zero_rule": "required_unused_zero",
        },
    },
    "poweroff_dying": {
        "owner": "power_state_service",
        "producer_route": "device_mqtt_event",
        "allowed_state_codes": ["progress"],
        "time_source": "device_unix_seconds_with_clock_valid",
        "value0": {
            "meaning": "grace_elapsed_seconds",
            "unit": "seconds",
            "minimum": 0,
            "maximum": 300,
            "zero_rule": "initial_grace_elapsed",
        },
        "value1": {
            "meaning": "remaining_shutdown_seconds",
            "unit": "seconds",
            "minimum": 0,
            "maximum": 300,
            "zero_rule": "shutdown_deadline_reached",
        },
    },
    "device_offline": {
        "owner": "authorized_server_offline_monitor",
        "producer_route": "authorized_server_direct_event_ingest",
        "allowed_state_codes": ["active"],
        "time_source": "server_unix_seconds",
        "value0": {
            "meaning": "offline_age_seconds",
            "unit": "seconds",
            "minimum": 0,
            "maximum": 2147483647,
            "zero_rule": "zero_age_is_valid_at_detection_boundary",
        },
        "value1": {
            "meaning": "unused",
            "unit": "unused_zero",
            "minimum": 0,
            "maximum": 0,
            "zero_rule": "required_unused_zero",
        },
    },
    "sensor_fault": {
        "owner": "sensor_coordinator",
        "producer_route": "device_mqtt_event",
        "allowed_state_codes": ["active", "inactive_or_cleared"],
        "time_source": "device_unix_seconds_with_clock_valid",
        "value0": {
            "meaning": "sensor_slot",
            "unit": "sensor_slot",
            "minimum": 1,
            "maximum": 2,
            "zero_rule": "forbidden",
        },
        "value1": {
            "meaning": "consecutive_failure_count",
            "unit": "count",
            "minimum": 0,
            "maximum": 2147483647,
            "zero_rule": "clear_resets_count",
        },
    },
    "fota_status": {
        "owner": "fota_update_service",
        "producer_route": "device_mqtt_event",
        "allowed_state_codes": ["progress", "failed", "inactive_or_cleared"],
        "time_source": "device_unix_seconds_with_clock_valid",
        "value0": {
            "meaning": "progress_percent",
            "unit": "percent",
            "minimum": 0,
            "maximum": 100,
            "zero_rule": "zero_percent_is_valid",
        },
        "value1": {
            "meaning": "unused",
            "unit": "unused_zero",
            "minimum": 0,
            "maximum": 0,
            "zero_rule": "required_unused_zero",
        },
    },
    "ai_status": {
        "owner": "ai_inference_service",
        "producer_route": "device_mqtt_event",
        "allowed_state_codes": ["progress", "failed", "inactive_or_cleared"],
        "time_source": "device_unix_seconds_with_clock_valid",
        "value0": {
            "meaning": "unused",
            "unit": "unused_zero",
            "minimum": 0,
            "maximum": 0,
            "zero_rule": "required_unused_zero",
        },
        "value1": {
            "meaning": "unused",
            "unit": "unused_zero",
            "minimum": 0,
            "maximum": 0,
            "zero_rule": "required_unused_zero",
        },
    },
}

EXPECTED_POWER_INCIDENT_CONTINUITY = {
    "firmware_contract_ref": EXPECTED_CROSS_CONTRACT_REFS[
        "power_incident_lifecycle"
    ],
    "incident_scope": "authenticated_imei_context",
    "correlated_events": [
        "adapter_removed",
        "poweroff_dying",
        "device_offline",
        "adapter_restored",
    ],
    "same_open_incident_id_required": True,
    "server_offline_max_per_open_incident": 1,
    "adapter_restored_closes_open_incident": True,
    "no_open_power_fallback": {
        "incident_kind": "separate_offline_incident",
        "power_incident_label_allowed": False,
    },
}

EXPECTED_EVENT_IDEMPOTENCY_CONTEXT = {
    "key_fields_ref": "event_semantics.idempotency_key",
    "field_sources": {
        "authenticated_imei_context": {
            "device_mqtt_event": {
                "source_ref": "mqtt_identity_binding.p_imei_source",
                "conversion_rule_ref": "authenticated_imei_to_pg_text",
            },
            "authorized_server_direct_event_ingest": {
                "source_ref": (
                    "authorized_server_event_routes.device_offline.identity_source"
                ),
                "conversion_rule_ref": "server_authorized_imei_to_pg_text",
            },
        },
        "event_origin": {
            "source": "event_schema.producer_route",
            "allowed_values": [
                "device_mqtt_event",
                "authorized_server_direct_event_ingest",
            ],
        },
    },
    "producer_sequence_collision_prevention": {
        "scope_fields": [
            "authenticated_imei_context",
            "event_origin",
            "event_type",
            "incident_id",
        ],
        "sequence_field": "sequence",
        "same_sequence_different_origin_collides": False,
    },
    "no_open_power_fallback_binding": {
        "source_ref": (
            "event_semantics.power_incident_continuity.no_open_power_fallback"
        ),
        "event_origin": "authorized_server_direct_event_ingest",
        "incident_kind": "separate_offline_incident",
    },
}

EXPECTED_ROUTE_CONVERSION_OVERRIDES = {
    "authorized_server_event_routes.device_offline": {
        "rpc": "event_ingest",
        "input_argument_rule_refs": {
            "p_imei text": "server_authorized_imei_to_pg_text"
        },
    }
}


class MqttDbContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = json.loads(CONTRACT.read_text(encoding="utf-8"))

    def test_global_wire_rules(self):
        self.assertEqual(self.data["contract_id"], "nb_iot.g1.mqtt_db")
        self.assertEqual(self.data["schema_version"], 1)
        self.assertEqual(self.data["encoding"], "numeric_csv")
        self.assertEqual(self.data["wire_character_set"], "ascii")
        self.assertTrue(self.data["numeric_tokens_only"])
        self.assertEqual(
            self.data["numeric_token_grammar"], "canonical_decimal_integer"
        )
        self.assertEqual(
            self.data["numeric_token_regex"], CANONICAL_TOKEN_PATTERN
        )
        self.assertEqual(
            self.data["max_golden_selection_rule"],
            "longest_canonical_endpoint_tie_maximum",
        )
        self.assertEqual(self.data["max_payload_bytes"], 80)
        self.assertFalse(self.data["legacy_json_allowed"])
        self.assertFalse(self.data["legacy_array_allowed"])
        self.assertFalse(self.data["raw_binary_allowed"])
        self.assertFalse(self.data["dual_decoder_allowed"])

    def test_every_payload_family_has_an_explicit_producer_consumer_route(self):
        routes = self.data["producer_consumer"]
        self.assertEqual(set(routes), set(PAYLOAD_FIELDS))
        expected = {
            "telemetry": ("pico", "rpc.telemetry_ingest"),
            "boot": ("pico", "rpc.boot_ingest"),
            "config_request": ("pico", "rpc.config_fetch"),
            "config_response": ("rpc.config_fetch", "pico"),
            "command_request": ("pico", "rpc.command_claim"),
            "command_response": ("rpc.command_claim", "pico"),
            "config_ack": ("pico", "rpc.config_ack"),
            "command_ack": ("pico", "rpc.command_ack"),
            "command_ack_receipt": ("command_ack_receipt_gateway", "pico"),
            "event": ("pico", "rpc.event_ingest"),
        }
        for family, (producer, consumer) in expected.items():
            self.assertEqual(routes[family]["producer"], producer)
            self.assertEqual(routes[family]["broker"], "emqx")
            self.assertEqual(routes[family]["consumer"], consumer)

    def test_canonical_decimal_grammar_rejects_noncanonical_tokens(self):
        contract_pattern = re.compile(self.data["numeric_token_regex"])
        for token in ("0", "1", "9", "10", "-1", "-10", "4294967295"):
            self.assertRegex(token, CANONICAL_TOKEN)
            self.assertRegex(token, contract_pattern)

        for token in (
            "+1",
            "+0",
            "01",
            "00",
            "-0",
            "-01",
            "",
            " 1",
            "1 ",
            "--1",
        ):
            self.assertNotRegex(token, CANONICAL_TOKEN)
            self.assertNotRegex(token, contract_pattern)

        for payload in ("1,+2", "1,02", "1,-0", "1,", ",1", "1, 2"):
            self.assertNotRegex(payload, NUMERIC_CSV)

    def test_topics_and_pull_order(self):
        self.assertEqual(self.data["pull_order"], ["config", "command"])
        self.assertEqual(
            self.data["pull_triggers"],
            ["first_connect", "reconnect", "periodic_telemetry"],
        )
        self.assertEqual(self.data["topics"], TOPICS)
        self.assertEqual(set(self.data["retained"]), set(TOPICS))
        self.assertTrue(all(not value for value in self.data["retained"].values()))
        polling = self.data["polling"]
        self.assertTrue(polling["config_command_separate"])
        self.assertEqual(polling["max_normal_apply_latency_seconds"], 1200)
        self.assertTrue(polling["bounded_retry"])
        self.assertTrue(polling["last_known_config_preserved_on_error"])

    def test_exact_device_auth_signature_and_auth_boundary(self):
        self.assertEqual(self.data["rpc"]["device_auth"], RPC_SIGNATURES["device_auth"])
        self.assertEqual(
            self.data["device_credentials"],
            {"username": "imei", "password": "imsi_cimi"},
        )
        auth = self.data["auth_boundary"]
        self.assertEqual(auth["web_login_methods"], ["id_password", "google_oauth"])
        self.assertEqual(auth["id_password_ui_identifier"], "id")
        self.assertFalse(auth["google_only"])
        self.assertFalse(auth["email_input_required"])
        self.assertEqual(auth["web_session_authority"], "supabase_auth")
        self.assertEqual(auth["browser_key_role"], "anon_public")
        self.assertFalse(auth["browser_service_role_allowed"])
        self.assertTrue(auth["web_and_device_auth_separate"])
        self.assertEqual(
            auth["id_to_supabase_identity_mapping_status"],
            "not_frozen_until_auth_migration_security_tests",
        )
        self.assertEqual(
            auth["authorization_metadata_mapping_status"],
            "not_frozen_until_auth_migration_security_tests",
        )
        self.assertFalse(auth["user_metadata_authorization_allowed"])
        self.assertFalse(self.data["rpc_policy"]["device_auth_overloads_allowed"])
        self.assertTrue(
            self.data["rpc_policy"]["diagnostic_context_requires_separate_rpc_name"]
        )

    def test_imei_is_context_only_and_not_repeated_in_payloads(self):
        imei = self.data["context_fields"]["imei"]
        self.assertEqual(
            imei["source"], "authenticated_mqtt_username_after_exact_topic_match"
        )
        self.assertEqual(imei["rpc_injection"], "p_imei")
        self.assertEqual(imei["identity_binding_ref"], "mqtt_identity_binding")
        self.assertFalse(imei["payload_repetition_allowed"])
        for item in self.data["payloads"].values():
            self.assertNotIn("imei", item["fields"])

    def test_authenticated_mqtt_identity_binding_and_server_event_split(self):
        self.assertEqual(
            self.data.get("mqtt_identity_binding"), EXPECTED_IDENTITY_BINDING
        )
        self.assertEqual(
            self.data.get("authorized_server_event_routes"),
            EXPECTED_SERVER_EVENT_ROUTE,
        )

        binding = self.data.get("mqtt_identity_binding")
        if binding is None:
            return
        accepted = mqtt_identity_outcome(binding, "359759088888888", "359759088888888")
        self.assertEqual(
            accepted,
            {
                "route": True,
                "p_imei": "359759088888888",
                "rpc": True,
                "republish": True,
                "audit_increment": False,
            },
        )
        mismatch = mqtt_identity_outcome(binding, "359759088888888", "359759099999999")
        self.assertEqual(
            mismatch,
            {
                "route": False,
                "p_imei": None,
                "rpc": False,
                "republish": False,
                "audit_increment": True,
            },
        )
        self.assertEqual(
            self.data["producer_consumer"]["event"],
            {"producer": "pico", "broker": "emqx", "consumer": "rpc.event_ingest"},
        )

    def test_directional_topic_acl_and_device_event_gateway_policy(self):
        binding = self.data["mqtt_identity_binding"]
        acl = binding["acl"]
        self.assertEqual(
            acl.get("device_publish_topic_families"),
            EXPECTED_IDENTITY_BINDING["acl"]["device_publish_topic_families"],
        )
        self.assertEqual(
            acl.get("device_subscribe_topic_families"),
            EXPECTED_IDENTITY_BINDING["acl"]["device_subscribe_topic_families"],
        )
        publish = acl.get("device_publish_topic_families")
        subscribe = acl.get("device_subscribe_topic_families")
        if publish is None or subscribe is None:
            return
        self.assertFalse(set(publish) & set(subscribe))
        for family in publish + subscribe:
            self.assertIn(family, self.data["topics"])
            self.assertIn("{imei}", self.data["topics"][family])

        policy = binding.get("device_event_route")
        self.assertEqual(policy, EXPECTED_IDENTITY_BINDING["device_event_route"])
        if policy is None:
            return
        schemas = self.data["event_semantics"]["schemas"]
        device_schema_types = {
            event_type
            for event_type, schema in schemas.items()
            if schema["producer_route"] == "device_mqtt_event"
        }
        self.assertEqual(set(policy["allowed_event_types"]), device_schema_types)
        self.assertNotIn("device_offline", policy["allowed_event_types"])
        for event_type in policy["allowed_event_types"]:
            self.assertEqual(
                device_event_gateway_outcome(binding, schemas, event_type),
                {"action": "route", "rpc_allowed": True},
                event_type,
            )
        self.assertEqual(
            device_event_gateway_outcome(binding, schemas, "device_offline"),
            {"action": "drop", "rpc_allowed": False},
        )

        allowlist_mutation = json.loads(json.dumps(binding))
        allowlist_mutation["device_event_route"]["allowed_event_types"].append(
            "device_offline"
        )
        self.assertEqual(
            device_event_gateway_outcome(
                allowlist_mutation, schemas, "device_offline"
            ),
            {"action": "drop", "rpc_allowed": False},
        )

    def test_invalid_empty_null_and_stale_publication_is_forbidden(self):
        rules = self.data["publication_rules"]
        for key in (
            "invalid_publish_allowed",
            "undefined_publish_allowed",
            "empty_publish_allowed",
            "null_publish_allowed",
            "telemetry_invalid_publish_allowed",
            "telemetry_stale_publish_allowed",
            "temperature_sentinel_allowed",
        ):
            self.assertFalse(rules[key], key)
        self.assertEqual(
            rules["valid_zero_code_rows_only"],
            ["config_no_change", "command_none"],
        )

    def test_csv_field_order_specs_and_max_boundary_payloads(self):
        self.assertEqual(
            {name: item["fields"] for name, item in self.data["payloads"].items()},
            PAYLOAD_FIELDS,
        )
        for family, item in self.data["payloads"].items():
            fields = item["fields"]
            specs = item["field_specs"]
            payload = item["max_golden_payload"]
            encoded = payload.encode("ascii")
            parts = payload.split(",")

            self.assertEqual(list(specs), fields, family)
            self.assertRegex(payload, NUMERIC_CSV, family)
            self.assertEqual(len(parts), len(fields), family)
            self.assertEqual(item["max_golden_payload_bytes"], len(encoded), family)
            self.assertLessEqual(len(encoded), self.data["max_payload_bytes"], family)

            for field, raw in zip(fields, parts):
                spec = specs[field]
                self.assertEqual(spec["kind"], "integer", (family, field))
                self.assertIsInstance(spec["signed"], bool, (family, field))
                self.assertIsInstance(spec["minimum"], int, (family, field))
                self.assertIsInstance(spec["maximum"], int, (family, field))
                self.assertLessEqual(
                    spec["minimum"], spec["maximum"], (family, field)
                )
                if not spec["signed"]:
                    self.assertGreaterEqual(spec["minimum"], 0, (family, field))
                self.assertIsInstance(spec["unit"], str, (family, field))
                self.assertTrue(spec["unit"], (family, field))
                self.assertIsInstance(spec["zero_code"], str, (family, field))
                self.assertTrue(spec["zero_code"], (family, field))
                self.assertEqual(
                    raw,
                    str(longest_canonical_endpoint(spec)),
                    (family, field),
                )

        event = self.data["payloads"]["event"]
        event_parts = event["max_golden_payload"].split(",")
        self.assertEqual(event_parts[event["fields"].index("value0")], "-2147483648")
        self.assertEqual(event_parts[event["fields"].index("value1")], "-2147483648")

    def test_enum_references_are_machine_resolvable_and_exact_for_field_ranges(self):
        enums = self.data["wire_enums"]
        for family, payload in self.data["payloads"].items():
            for field, spec in payload["field_specs"].items():
                self.assertNotIn("enum", spec, (family, field))
                if "enum_ref" not in spec:
                    continue
                enum_name = spec["enum_ref"]
                self.assertIn(enum_name, enums, (family, field))
                values = list(enums[enum_name].values())
                self.assertTrue(values, (family, field))
                self.assertEqual(
                    (spec["minimum"], spec["maximum"]),
                    (min(values), max(values)),
                    (family, field),
                )

    def test_wire_enums_are_the_single_numeric_source(self):
        for forbidden_alias in (
            "ack_phase",
            "command_result",
            "config_result",
            "config_error",
            "command_error",
            "event_types",
            "event_state_codes",
        ):
            self.assertNotIn(forbidden_alias, self.data)

        self.assertEqual(
            self.data["config_semantics"]["enum_refs"],
            {"result_code": "config_result", "error_code": "config_error"},
        )
        self.assertEqual(
            self.data["command_semantics"]["enum_refs"],
            {
                "opcode": "command_opcode",
                "phase": "ack_phase",
                "result_code": "command_result",
                "error_code": "command_error",
            },
        )
        self.assertEqual(
            self.data["event_semantics"]["enum_refs"],
            {"event_type": "event_type", "state_code": "event_state"},
        )

    def test_wire_enum_uniqueness_boolean_codes_and_zero_refs(self):
        enums = self.data["wire_enums"]
        self.assertEqual(enums["boolean_code"], {"false": 0, "true": 1})
        self.assertEqual(
            enums["health_status"],
            {"unknown": 0, "pass": 1, "degraded": 2, "failed": 3},
        )
        self.assertEqual(
            self.data.get("cross_contract_refs"),
            EXPECTED_CROSS_CONTRACT_REFS,
        )
        for enum_name, enum in enums.items():
            self.assertEqual(
                len(enum.values()), len(set(enum.values())), enum_name
            )

        for family, payload in self.data["payloads"].items():
            for field, spec in payload["field_specs"].items():
                enum_ref = spec.get("enum_ref")
                if enum_ref is None:
                    continue
                zero_names = [
                    name for name, value in enums[enum_ref].items() if value == 0
                ]
                if zero_names:
                    self.assertEqual(len(zero_names), 1, (family, field))
                    self.assertEqual(
                        spec.get("enum_zero_ref"), zero_names[0], (family, field)
                    )
                else:
                    self.assertNotIn("enum_zero_ref", spec, (family, field))
                    self.assertEqual(spec["zero_code"], "forbidden", (family, field))

        boolean_swap = {"false": 1, "true": 0}
        self.assertIn(
            "exact_mapping",
            enum_contract_errors(boolean_swap, {"false": 0, "true": 1}),
        )
        boolean_duplicate = {"false": 0, "true": 0}
        self.assertIn(
            "numeric_uniqueness",
            enum_contract_errors(boolean_duplicate, {"false": 0, "true": 1}),
        )

    def test_temperature_units_ranges_and_no_sentinel(self):
        temperature_fields = [
            ("telemetry", "value_tenths"),
            ("boot", "boot_temp_tenths"),
            ("config_response", "upper_tenths"),
            ("config_response", "lower_tenths"),
        ]
        for family, field in temperature_fields:
            spec = self.data["payloads"][family]["field_specs"][field]
            self.assertTrue(spec["signed"])
            self.assertEqual(spec["minimum"], -550)
            self.assertEqual(spec["maximum"], 1250)
            self.assertEqual(spec["unit"], "tenths_celsius")
            self.assertEqual(spec["zero_code"], "measured_zero_celsius")

    def test_boot_preserves_fourteen_meanings_and_adds_version_and_last_command(self):
        boot = self.data["payloads"]["boot"]
        self.assertEqual(
            boot["preserved_boot_meaning_fields"],
            [
                "cimi",
                "voltage_mv",
                "boot_temp_tenths",
                "flash_status",
                "ram_status",
                "at_status",
                "cpin_status",
                "csq",
                "carrier_code",
                "temp1_status",
                "temp2_status",
                "mic1_status",
                "mic2_status",
                "boot_reason",
            ],
        )
        self.assertEqual(boot["g1_added_fields"], ["schema_version", "last_cmd_id"])
        self.assertEqual(
            boot["fields"],
            ["schema_version"]
            + boot["preserved_boot_meaning_fields"]
            + ["last_cmd_id"],
        )

    def test_unsigned_32_bit_identifiers_and_boolean_ranges(self):
        id_fields = {
            "telemetry": ["sensor_id", "sequence"],
            "boot": ["last_cmd_id"],
            "config_request": ["request_id", "known_config_version"],
            "config_response": ["request_id", "config_version", "sensor_id"],
            "command_request": ["request_id", "last_cmd_id"],
            "command_response": ["request_id", "cmd_id", "job_ref"],
            "config_ack": ["request_id", "config_version"],
            "command_ack": ["cmd_id"],
            "command_ack_receipt": ["cmd_id"],
            "event": ["incident_id", "sequence"],
        }
        for family, fields in id_fields.items():
            for field in fields:
                spec = self.data["payloads"][family]["field_specs"][field]
                self.assertFalse(spec["signed"], (family, field))
                self.assertEqual(spec["maximum"], UINT32_MAX, (family, field))

        for family, field in (
            ("config_response", "enabled"),
            ("command_ack", "clock_valid"),
            ("event", "clock_valid"),
        ):
            spec = self.data["payloads"][family]["field_specs"][field]
            self.assertEqual((spec["minimum"], spec["maximum"]), (0, 1))
            self.assertEqual(spec["unit"], "boolean_code")

    def test_valid_config_no_change_and_command_none_rows(self):
        rows = self.data["valid_zero_code_rows"]
        self.assertEqual(set(rows), {"config_no_change", "command_none"})

        config = rows["config_no_change"]
        self.assertEqual(config["payload_family"], "config_response")
        self.assertEqual(config["meaning"], "no_new_config")
        self.assertEqual(config["condition"], "known_config_version_equals_current")
        self.assertEqual(
            config["field_relations"],
            {
                "schema_version": "constant:1",
                "request_id": "echo:request.request_id",
                "config_version": "echo:request.known_config_version",
                "page_index": "constant:0",
                "page_count": "constant:1",
                "sensor_slot": "constant:0",
                "sensor_id": "constant:0",
                "upper_tenths": "constant:0",
                "lower_tenths": "constant:0",
                "enabled": "constant:0",
                "checksum_u32": "constant:0",
            },
        )
        self.assertEqual(config["request_precondition"], {"page_index": 0})
        self.assertTrue(config["unique_relation_required"])
        self.assertTrue(config["boundary_example_only"])

        command = rows["command_none"]
        self.assertEqual(command["payload_family"], "command_response")
        self.assertEqual(command["meaning"], "no_claimable_command")
        self.assertEqual(
            command["field_relations"],
            {
                "schema_version": "constant:1",
                "request_id": "echo:request.request_id",
                "cmd_id": "constant:0",
                "opcode": "constant:0",
                "job_ref": "constant:0",
                "ttl_seconds": "constant:0",
            },
        )
        self.assertTrue(command["unique_relation_required"])
        self.assertTrue(command["boundary_example_only"])

        self.assertGreaterEqual(len(config["examples"]), 2)
        self.assertGreaterEqual(len(command["examples"]), 2)
        self.assertGreaterEqual(
            len({item["request"]["request_id"] for item in config["examples"]}), 2
        )
        self.assertGreaterEqual(
            len({item["request"]["request_id"] for item in command["examples"]}), 2
        )
        for example in config["examples"] + [config["boundary_example"]]:
            self.assertTrue(
                is_config_no_change_response(example["request"], example["response"])
            )
        for example in command["examples"] + [command["boundary_example"]]:
            self.assertTrue(
                is_command_none_response(example["request"], example["response"])
            )

    def test_absence_rows_reject_every_single_field_relation_violation(self):
        config_request = {
            "request_id": 42,
            "known_config_version": 7,
            "page_index": 0,
        }
        config_base = [1, 42, 7, 0, 1, 0, 0, 0, 0, 0, 0]
        for index in range(len(config_base)):
            mutated = list(config_base)
            mutated[index] = mutated[index] + 1
            self.assertFalse(
                is_config_no_change_response(
                    config_request,
                    ",".join(str(value) for value in mutated),
                ),
                f"config field index {index}",
            )

        command_request = {"request_id": 42}
        command_base = [1, 42, 0, 0, 0, 0]
        for index in range(len(command_base)):
            mutated = list(command_base)
            mutated[index] = mutated[index] + 1
            self.assertFalse(
                is_command_none_response(
                    command_request,
                    ",".join(str(value) for value in mutated),
                ),
                f"command field index {index}",
            )

    def test_config_paging_checksum_and_result_contract(self):
        config = self.data["config_semantics"]
        self.assertTrue(config["one_sensor_per_page"])
        self.assertEqual(config["page_index_base"], 0)
        self.assertEqual(config["page_count_min"], 1)
        self.assertEqual(config["sensor_slot_values"], [1, 2])
        self.assertEqual(config["sensor_slot_zero_use"], "config_no_change_only")
        self.assertEqual(config["checksum_ref"], "config_checksum")
        self.assertEqual(config["response_echo_fields"], ["request_id", "page_index"])
        self.assertEqual(
            config["last_page_relation"],
            "page_index_equals_page_count_minus_one",
        )
        self.assertTrue(config["atomic_apply_after_all_pages_verified"])
        self.assertEqual(config["no_change_row"], "config_no_change")
        self.assertEqual(
            self.data["wire_enums"]["config_result"],
            {"applied": 1, "rejected": 2, "stale": 3},
        )
        self.assertEqual(
            self.data["wire_enums"]["config_error"],
            {
                "none": 0,
                "invalid_field": 1,
                "checksum": 2,
                "version": 3,
                "storage": 4,
            },
        )

    def test_config_request_page_coordinator_state_machine(self):
        coordinator = self.data["config_semantics"]["request_coordinator"]
        self.assertEqual(
            coordinator,
            {
                "owner": "pico_request_coordinator",
                "start_page_index": 0,
                "same_request_id_across_pages": True,
                "one_page_request_at_a_time": True,
                "next_page_rule": (
                    "request_page_index_plus_one_when_response_page_index_less_than_"
                    "page_count_minus_one"
                ),
                "response_echo_fields": ["request_id", "page_index"],
                "last_page_relation": (
                    "response.page_index_equals_response.page_count_minus_one"
                ),
                "timeout_action": "retry_same_request_id_same_page_index",
                "duplicate_action": (
                    "idempotent_accept_same_request_id_same_page_index_same_version"
                ),
                "out_of_order_action": (
                    "discard_assembly_new_request_id_page_zero"
                ),
                "version_change_action": (
                    "discard_assembly_new_request_id_page_zero"
                ),
                "before_all_pages_forbidden": [
                    "atomic_apply",
                    "config_ack",
                    "command_pull",
                ],
                "after_all_pages_order": [
                    "verify_checksum",
                    "atomic_apply",
                    "config_ack",
                    "command_pull",
                ],
            },
        )
        mapping = self.data["rpc_argument_mappings"]["config_fetch"]
        page_source = next(
            source
            for source in mapping["arguments"]
            if source["argument"] == "p_page_index integer"
        )
        self.assertEqual(
            page_source,
            {
                "argument": "p_page_index integer",
                "source_kind": "payload_field",
                "field": "page_index",
                "payload_index": 3,
            },
        )

    def test_crc32_iso_hdlc_parameters_and_hardcoded_known_answer_vectors(self):
        standard_input = b"123456789"
        config_stream = (
            b"1,7,1,101,-200,50,1\n"
            b"1,7,2,202,-300,40,1"
        )
        self.assertEqual(crc32_iso_hdlc(standard_input), 3421780262)
        self.assertEqual(crc32_iso_hdlc(config_stream), 3194237552)

        checksum = self.data["config_checksum"]
        self.assertEqual(checksum["name"], "CRC-32/ISO-HDLC")
        self.assertEqual(checksum["width_bits"], 32)
        self.assertEqual(checksum["polynomial_normal_u32"], 79764919)
        self.assertEqual(checksum["polynomial_reflected_u32"], 3988292384)
        self.assertEqual(checksum["init_u32"], 4294967295)
        self.assertTrue(checksum["refin"])
        self.assertTrue(checksum["refout"])
        self.assertEqual(checksum["xorout_u32"], 4294967295)
        self.assertEqual(
            checksum["semantic_row_fields"],
            [
                "schema_version",
                "config_version",
                "sensor_slot",
                "sensor_id",
                "upper_tenths",
                "lower_tenths",
                "enabled",
            ],
        )
        self.assertEqual(checksum["row_order"], "sensor_slot_ascending")
        self.assertEqual(checksum["encoding"], "canonical_decimal_csv_ascii")
        self.assertEqual(checksum["field_separator"], ",")
        self.assertEqual(checksum["row_separator"], "LF")
        self.assertFalse(checksum["trailing_lf"])
        self.assertEqual(
            checksum["excluded_payload_fields"],
            ["request_id", "page_index", "page_count", "checksum_u32"],
        )
        self.assertEqual(
            checksum["known_answer_vectors"],
            [
                {
                    "name": "standard_check",
                    "input_ascii": "123456789",
                    "crc32_u32": 3421780262,
                },
                {
                    "name": "two_sensor_config",
                    "input_ascii": (
                        "1,7,1,101,-200,50,1\n"
                        "1,7,2,202,-300,40,1"
                    ),
                    "crc32_u32": 3194237552,
                },
            ],
        )

    def test_command_ttl_opcode_claim_dedupe_and_reboot_journal(self):
        command = self.data["command_semantics"]
        self.assertEqual(
            self.data["wire_enums"]["command_opcode"],
            {
                "none": 0,
                "reboot": 1,
                "power_off": 2,
                "request_status": 3,
                "fota_prepare": 4,
            },
        )
        self.assertEqual(
            command["enum_refs"],
            {
                "opcode": "command_opcode",
                "phase": "ack_phase",
                "result_code": "command_result",
                "error_code": "command_error",
            },
        )
        self.assertEqual(command["claim_count_per_request"], 1)
        self.assertEqual(command["ttl_field"], "ttl_seconds")
        self.assertEqual(
            command["ttl_semantics"], "duration_seconds_from_device_receive"
        )
        self.assertFalse(command["absolute_expires_at_allowed"])
        self.assertFalse(command["response_clock_valid_allowed"])
        self.assertEqual(command["no_command_row"], "command_none")
        self.assertEqual(command["job_ref_zero_meaning"], "no_job_reference")
        self.assertEqual(command["dedupe_key"], "cmd_id")
        self.assertEqual(
            command["accepted_before_execute_opcodes"], ["reboot", "power_off"]
        )
        self.assertTrue(command["final_ack_recovered_on_next_boot"])
        self.assertTrue(command["reboot_journal_required"])
        self.assertEqual(
            command["database_state_flow"],
            ["queued", "delivered", "accepted", "executed_or_failed_or_expired"],
        )
        response_fields = self.data["payloads"]["command_response"]["fields"]
        self.assertNotIn("expires_at", response_fields)
        self.assertNotIn("clock_valid", response_fields)

    def test_command_ack_receipt_and_durable_recovery_sequence(self):
        enums = self.data["wire_enums"]
        self.assertEqual(
            enums["ack_receipt"],
            {"ingested": 1, "rejected": 2, "mismatch": 3},
        )
        self.assertEqual(
            enums["ack_receipt_error"],
            {
                "none": 0,
                "db_rejected": 1,
                "cmd_mismatch": 2,
                "phase_mismatch": 3,
                "result_mismatch": 4,
            },
        )
        self.assertEqual(
            enums["command_journal_state"],
            {
                "empty": 0,
                "accepted_persisted": 1,
                "accepted_publish_pending": 2,
                "accepted_puback": 3,
                "accepted_receipted": 4,
                "execute_marked": 5,
                "executed": 6,
                "final_persisted": 7,
                "final_publish_pending": 8,
                "final_puback": 9,
                "final_receipted": 10,
            },
        )

        recovery = self.data["command_ack_recovery"]
        journal = recovery["durable_journal"]
        self.assertEqual(
            journal["required_fields"],
            [
                "cmd_id",
                "opcode",
                "job_ref",
                "ttl_seconds",
                "phase",
                "result",
                "error",
                "state",
                "retry_count",
                "boot_sequence_before_execute",
                "expected_effect",
                "remaining_ttl_seconds",
                "ttl_checkpoint_monotonic_seconds",
                "ttl_checkpoint_unix_seconds",
                "ttl_checkpoint_clock_valid",
                "ttl_checkpoint_boot_sequence",
                "dispatch_latched",
            ],
        )
        self.assertEqual(
            journal["field_enum_refs"],
            {
                "opcode": "command_opcode",
                "phase": "ack_phase",
                "result": "command_result",
                "error": "command_error",
                "state": "command_journal_state",
            },
        )
        self.assertTrue(journal["persistent_before_publish_or_execute"])
        self.assertTrue(journal["retry_count_monotonic"])
        self.assertEqual(
            recovery["sequence"],
            [
                "persist_accepted",
                "persist_accepted_publish_pending",
                "publish_accepted_ack",
                "record_accepted_puback",
                "validate_and_persist_accepted_receipted",
                "persist_execute_marker",
                "execute_once",
                "persist_executed",
                "persist_final",
                "persist_final_publish_pending",
                "publish_final_ack",
                "record_final_puback",
                "validate_and_persist_final_receipted",
                "clear_journal",
            ],
        )
        self.assertEqual(
            recovery["ack_phase_result_error_matrix"],
            {
                "accepted": {"accepted": ["none"]},
                "final": {
                    "executed": ["none"],
                    "failed": [
                        "invalid_opcode",
                        "duplicate",
                        "execution",
                        "journal",
                    ],
                    "expired": ["expired"],
                },
            },
        )
        self.assertEqual(
            recovery["receipt_matrix"],
            {
                "ingested": {
                    "allowed_error_codes": ["none"],
                    "requires_exact_match": ["cmd_id", "phase", "result_code"],
                    "accepted_current_state": "accepted_puback",
                    "accepted_action": "validate_and_persist_accepted_receipted",
                    "accepted_next_state": "accepted_receipted",
                    "final_current_state": "final_puback",
                    "final_action": "validate_and_persist_final_receipted",
                    "final_next_state": "final_receipted",
                },
                "rejected": {
                    "allowed_error_codes": ["db_rejected"],
                    "journal_action": "retain_and_bounded_retry",
                },
                "mismatch": {
                    "allowed_error_codes": [
                        "cmd_mismatch",
                        "phase_mismatch",
                        "result_mismatch",
                    ],
                    "journal_action": "retain_and_bounded_retry",
                },
            },
        )
        self.assertEqual(
            recovery["clear_predicate_all"],
            ["journal_state_is_final_receipted"],
        )
        self.assertEqual(
            recovery["boot_recovery"],
            {
                "after": "mqtt_recovery",
                "pending_replay_precedes": ["config_pull", "command_pull"],
                "replay_phases": ["accepted", "final"],
                "same_cmd_reexecution_allowed": False,
            },
        )
        self.assertTrue(recovery["bounded_retry"])

        mapping = self.data["rpc_output_mappings"]["command_ack_receipt"]
        self.assertEqual(mapping["rpc"], "command_ack")
        self.assertTrue(mapping["actual_db_ingest_required_for_ingested_receipt"])
        self.assertEqual(
            [item["rpc_output"] for item in mapping["positions"]],
            PAYLOAD_FIELDS["command_ack_receipt"],
        )

    def test_command_journal_graph_is_closed_and_power_loss_safe(self):
        states = self.data["wire_enums"]["command_journal_state"]
        recovery = self.data["command_ack_recovery"]
        linear_states = [
            "empty",
            "accepted_persisted",
            "accepted_publish_pending",
            "accepted_puback",
            "accepted_receipted",
            "execute_marked",
            "executed",
            "final_persisted",
            "final_publish_pending",
            "final_puback",
            "final_receipted",
            "empty",
        ]
        self.assertEqual(recovery.get("normal_linear_states"), linear_states)
        self.assertEqual(
            recovery.get("create_transition"),
            {
                "event": "accept_destructive_command",
                "action": "persist_full_accepted_journal",
                "next_state": "accepted_persisted",
                "reexecute_allowed": False,
            },
        )
        self.assertEqual(journal_graph_errors(states, recovery), [])

        normal = recovery["normal_transitions"]
        boot = recovery["boot_recovery_transitions"]
        for source, target in zip(linear_states[1:-1], linear_states[2:]):
            self.assertEqual(normal[source]["next_state"], target, source)
        self.assertEqual(
            normal["final_puback"],
            {
                "event": "matching_final_db_receipt",
                "action": "validate_and_persist_final_receipted",
                "next_state": "final_receipted",
                "reexecute_allowed": False,
            },
        )
        self.assertEqual(
            boot["final_receipted"],
            {
                "event": "boot_recovery",
                "action": "clear_journal",
                "next_state": "empty",
                "reexecute_allowed": False,
            },
        )
        self.assertFalse(boot["execute_marked"]["reexecute_allowed"])
        self.assertEqual(boot["accepted_puback"]["next_state"], "accepted_puback")
        self.assertEqual(boot["final_puback"]["next_state"], "final_puback")
        self.assertIn("replay_accepted_ack", boot["accepted_puback"]["action"])
        self.assertIn("replay_final_ack", boot["final_puback"]["action"])
        self.assertEqual(
            recovery["execute_marked_boot_resolution"],
            {
                "inputs": [
                    "boot_sequence_before_execute",
                    "current_boot_sequence",
                    "opcode",
                    "expected_effect",
                    "reset_reason",
                    "power_state_evidence",
                ],
                "opcode_evidence": {
                    "reboot": "boot_sequence_advanced_and_reset_reason_matches_expected_effect",
                    "power_off": "boot_sequence_advanced_and_power_state_matches_expected_effect",
                },
                "success": {
                    "result": "executed",
                    "error": "none",
                    "next_state": "executed",
                },
                "failure": {
                    "result": "failed",
                    "error": "journal",
                    "next_state": "executed",
                },
                "reexecute_allowed": False,
            },
        )

        mutated = json.loads(json.dumps(recovery))
        mutated["boot_recovery_transitions"].pop("accepted_puback")
        self.assertIn("boot_coverage", journal_graph_errors(states, mutated))

    def test_destructive_ttl_checkpoint_expiry_and_single_dispatch(self):
        command = self.data["command_semantics"]
        self.assertEqual(command.get("non_none_ttl_min_seconds"), 1)
        self.assertEqual(command.get("zero_ttl_use"), "command_none_only")

        recovery = self.data["command_ack_recovery"]
        policy = recovery.get("ttl_enforcement")
        self.assertIsNotNone(policy)
        if policy is None:
            return
        pre_execute_states = [
            "accepted_persisted",
            "accepted_publish_pending",
            "accepted_puback",
            "accepted_receipted",
        ]
        self.assertEqual(policy["origin"], "device_command_response_receipt")
        self.assertEqual(policy["original_ttl_field"], "ttl_seconds")
        self.assertEqual(policy["non_none_ttl_min_seconds"], 1)
        self.assertEqual(policy["zero_ttl_use"], "command_none_only")
        self.assertEqual(policy["pre_execute_states"], pre_execute_states)
        self.assertEqual(
            policy["checkpoint_fields"],
            [
                "remaining_ttl_seconds",
                "ttl_checkpoint_monotonic_seconds",
                "ttl_checkpoint_unix_seconds",
                "ttl_checkpoint_clock_valid",
                "ttl_checkpoint_boot_sequence",
            ],
        )
        self.assertTrue(policy["checkpoint_on_each_durable_pre_execute_transition"])
        self.assertEqual(
            policy["same_boot_elapsed_source"], "monotonic_seconds_nondecreasing"
        )
        self.assertEqual(
            policy["reboot_elapsed_rule"],
            "subtract_trusted_nondecreasing_unix_elapsed_only_when_both_clocks_valid",
        )
        self.assertEqual(
            policy["reboot_unprovable_time_action"],
            "persist_final_expired_without_dispatch",
        )
        self.assertEqual(set(policy["pre_execute_expiry_transitions"]), set(pre_execute_states))
        for state, branch in policy["pre_execute_expiry_transitions"].items():
            self.assertEqual(
                branch,
                {
                    "condition": "remaining_ttl_zero_or_validity_unprovable",
                    "action": "persist_terminal_expired",
                    "next_state": "executed",
                    "result": "expired",
                    "error": "expired",
                    "dispatch_allowed": False,
                },
                state,
            )
        self.assertEqual(
            policy["immediate_pre_dispatch_order"],
            [
                "checkpoint_remaining_ttl",
                "if_expired_persist_terminal_expired",
                "atomically_persist_dispatch_latch_and_execute_marked",
                "verify_dispatch_latch_persisted",
                "dispatch_once",
            ],
        )
        self.assertEqual(
            policy["boot_pre_execute_order"],
            ["evaluate_expiry", "persist_expired_or_recover_state"],
        )
        self.assertEqual(
            policy["expired_terminal"],
            {
                "result": "expired",
                "error": "expired",
                "state": "executed",
                "dispatch_allowed": False,
                "continue_to": "final_persisted_then_existing_final_receipt_flow",
            },
        )

        single_dispatch = recovery["single_dispatch"]
        self.assertEqual(
            single_dispatch,
            {
                "latch_field": "dispatch_latched",
                "persist_operation": "atomic_persist_dispatch_latch_and_execute_marked",
                "verify_persisted_before_dispatch": True,
                "dispatch_count_max": 1,
                "duplicate_event_action": "ignore_duplicate_no_dispatch",
                "execute_marked_boot_dispatch_allowed": False,
            },
        )
        self.assertEqual(dispatch_latch_action(False), "atomically_persist_latch_then_dispatch_once")
        self.assertEqual(dispatch_latch_action(True), "ignore_duplicate_no_dispatch")

        checkpoint = {
            "opcode": "power_off",
            "ttl_seconds": 5,
            "remaining_ttl_seconds": 5,
            "checkpoint_monotonic_seconds": 100,
            "checkpoint_unix_seconds": 1000,
            "checkpoint_clock_valid": True,
            "checkpoint_boot_sequence": 7,
        }
        delayed_receipt = {
            "monotonic_seconds": 106,
            "unix_seconds": 1006,
            "clock_valid": True,
            "boot_sequence": 7,
        }
        self.assertEqual(
            destructive_ttl_decision(checkpoint, delayed_receipt)["decision"],
            "expired",
        )
        reset_valid = {
            "monotonic_seconds": 0,
            "unix_seconds": 1003,
            "clock_valid": True,
            "boot_sequence": 8,
        }
        self.assertEqual(
            destructive_ttl_decision(checkpoint, reset_valid),
            {"decision": "dispatch", "remaining_ttl_seconds": 2},
        )
        reset_invalid = dict(reset_valid, clock_valid=False)
        self.assertEqual(
            destructive_ttl_decision(checkpoint, reset_invalid)["decision"],
            "expired",
        )
        zero_ttl = dict(checkpoint, ttl_seconds=0, remaining_ttl_seconds=0)
        self.assertEqual(
            destructive_ttl_decision(zero_ttl, delayed_receipt)["decision"],
            "invalid",
        )

        for table_name in ("normal_transitions", "boot_recovery_transitions"):
            for state, entry in recovery[table_name].items():
                self.assertFalse(entry["reexecute_allowed"], (table_name, state))
        mutated = json.loads(json.dumps(recovery))
        mutated["boot_recovery_transitions"]["execute_marked"][
            "action"
        ] = "dispatch_command_again"
        mutated["boot_recovery_transitions"]["execute_marked"][
            "reexecute_allowed"
        ] = True
        mutation_errors = journal_graph_errors(
            self.data["wire_enums"]["command_journal_state"], mutated
        )
        self.assertIn("boot_execute_marked_reexecute_enabled", mutation_errors)
        self.assertIn("execute_marked_boot_dispatch", mutation_errors)

    def test_ack_and_time_contracts(self):
        self.assertEqual(
            self.data["wire_enums"]["ack_phase"],
            {"accepted": 1, "final": 2},
        )
        self.assertEqual(
            self.data["wire_enums"]["command_result"],
            {"none": 0, "accepted": 1, "executed": 2, "failed": 3, "expired": 4},
        )
        self.assertEqual(self.data["database_time_type"], "timestamptz")
        self.assertEqual(self.data["operations_timezone"], "Asia/Seoul")
        self.assertEqual(self.data["device_event_time"], ["unix_seconds", "clock_valid"])
        self.assertTrue(self.data["db_receive_time_required"])
        time_contract = self.data["time_contract"]
        self.assertEqual(
            time_contract["device_unix_clock_payloads"], ["command_ack", "event"]
        )
        self.assertEqual(
            time_contract["payloads_without_device_time"],
            [
                "telemetry",
                "boot",
                "config_request",
                "config_response",
                "command_request",
                "command_response",
                "config_ack",
                "command_ack_receipt",
            ],
        )
        self.assertEqual(time_contract["db_receive_time_source"], "database_server")
        self.assertTrue(time_contract["db_receive_time_separate_from_device_time"])

    def test_rpc_conversions_and_command_receipt_ownership_are_exact(self):
        conversions = self.data.get("rpc_conversions")
        self.assertIsNotNone(conversions)
        if conversions is None:
            return
        rules = conversions["rules"]
        expected_rule_names = {
            "mqtt_username_imei_to_pg_text",
            "mqtt_password_cimi_to_pg_text",
            "authenticated_imei_to_pg_text",
            "server_authorized_imei_to_pg_text",
            "wire_cimi_to_pg_text",
            "wire_integer_to_pg_integer",
            "wire_integer_to_pg_bigint",
            "wire_boolean_code_to_pg_boolean",
            "server_policy_integer_to_pg_integer",
            "pg_integer_to_wire_canonical_integer",
            "pg_bigint_to_wire_canonical_integer",
            "pg_boolean_to_wire_boolean_code",
        }
        self.assertEqual(set(rules), expected_rule_names)
        wire_bool = rules["wire_boolean_code_to_pg_boolean"]
        self.assertEqual(wire_bool["input_mapping"], {"0": False, "1": True})
        self.assertTrue(wire_bool["exact_range_validation_before_conversion"])
        self.assertFalse(conversion_accepts(wire_bool, "true"))
        self.assertFalse(conversion_accepts(wire_bool, "false"))
        self.assertTrue(conversion_accepts(wire_bool, "0"))
        self.assertTrue(conversion_accepts(wire_bool, "1"))

        pg_bool = rules["pg_boolean_to_wire_boolean_code"]
        self.assertEqual(pg_bool["accepted_postgres_types"], ["boolean"])
        self.assertEqual(pg_bool["output_mapping"], {"false": "0", "true": "1"})
        self.assertFalse(pg_bool["boolean_text_output_allowed"])
        self.assertTrue(conversion_accepts(pg_bool, True))
        self.assertFalse(conversion_accepts(pg_bool, "true"))

        cimi = rules["wire_cimi_to_pg_text"]
        self.assertTrue(cimi["preserve_exact_ascii"])
        self.assertFalse(cimi["numeric_intermediate_allowed"])
        self.assertTrue(conversion_accepts(cimi, "999999999999999"))
        precision_loss_mutation = dict(cimi, numeric_intermediate_allowed=True)
        self.assertNotEqual(precision_loss_mutation, cimi)

        for rule_name in (
            "wire_integer_to_pg_integer",
            "wire_integer_to_pg_bigint",
        ):
            self.assertEqual(
                rules[rule_name]["validation_order"],
                ["canonical_decimal_grammar", "declared_field_range", "postgres_range"],
            )
        for rule_name in (
            "pg_integer_to_wire_canonical_integer",
            "pg_bigint_to_wire_canonical_integer",
        ):
            self.assertEqual(
                rules[rule_name]["serialization"], "canonical_decimal_ascii"
            )
            self.assertTrue(rules[rule_name]["declared_field_range_validation"])

        input_refs = conversions["input_argument_rule_refs"]
        self.assertEqual(set(input_refs), set(self.data["rpc"]))
        for rpc_name, rpc in self.data["rpc"].items():
            self.assertEqual(set(input_refs[rpc_name]), set(rpc["arguments"]))
            for argument, rule_ref in input_refs[rpc_name].items():
                self.assertIn(rule_ref, rules, (rpc_name, argument))

        self.assertEqual(
            input_refs["device_auth"],
            {
                "username text": "mqtt_username_imei_to_pg_text",
                "password text": "mqtt_password_cimi_to_pg_text",
            },
        )
        for mapping_name, mapping in self.data["rpc_argument_mappings"].items():
            sources = {item["argument"]: item for item in mapping["arguments"]}
            for argument in self.data["rpc"][mapping["rpc"]]["arguments"]:
                source = sources[argument]
                if argument == "p_imei text":
                    expected_ref = "authenticated_imei_to_pg_text"
                elif argument == "p_cimi text":
                    expected_ref = "wire_cimi_to_pg_text"
                elif source["source_kind"] == "server_policy":
                    expected_ref = "server_policy_integer_to_pg_integer"
                elif argument.endswith(" boolean"):
                    expected_ref = "wire_boolean_code_to_pg_boolean"
                elif argument.endswith(" bigint"):
                    expected_ref = "wire_integer_to_pg_bigint"
                else:
                    expected_ref = "wire_integer_to_pg_integer"
                self.assertEqual(
                    input_refs[mapping["rpc"]][argument],
                    expected_ref,
                    (mapping_name, argument),
                )

        output_refs = conversions["output_position_rule_refs"]
        self.assertEqual(set(output_refs), set(self.data["rpc_output_mappings"]))
        for family, mapping in self.data["rpc_output_mappings"].items():
            self.assertEqual(len(output_refs[family]), len(mapping["positions"]))
            for position, rule_ref in zip(mapping["positions"], output_refs[family]):
                self.assertIn(rule_ref, rules, (family, position))
                field = position["payload_field"]
                spec = self.data["payloads"][family]["field_specs"][field]
                if field == "enabled":
                    expected_ref = "pg_boolean_to_wire_boolean_code"
                elif spec["minimum"] < -2147483648 or spec["maximum"] > 2147483647:
                    expected_ref = "pg_bigint_to_wire_canonical_integer"
                else:
                    expected_ref = "pg_integer_to_wire_canonical_integer"
                self.assertEqual(rule_ref, expected_ref, (family, field))

        self.assertEqual(
            conversions["invalid_conversion"],
            {
                "rpc_allowed": False,
                "response_republish_allowed": False,
                "last_known_state_preserved": True,
                "bounded_error_accounting": True,
            },
        )

        ownership = self.data["command_ack_recovery"].get("receipt_ownership")
        self.assertEqual(
            ownership,
            {
                "wire_receipts": {
                    "ingested": {
                        "owner": "rpc.command_ack_success_response",
                        "prerequisite": "actual_db_ingest_succeeded",
                    },
                    "rejected": {
                        "owner": "rpc.command_ack_logical_rejection_response",
                        "prerequisite": "actual_rpc_response_logical_rejection",
                    },
                    "mismatch": {
                        "owner": "command_ack_receipt_gateway_validation",
                        "prerequisite": "actual_rpc_response_cmd_phase_or_result_mismatch",
                    },
                },
                "local_timeout": {
                    "owner": "pico_receipt_timer",
                    "wire_publish_allowed": False,
                    "local_action": "retain_journal_and_bounded_retry",
                },
            },
        )
        self.assertNotIn("timeout", self.data["wire_enums"]["ack_receipt"])
        self.assertNotIn("timeout", self.data["wire_enums"]["ack_receipt_error"])
        self.assertNotIn("timeout", self.data["command_ack_recovery"]["receipt_matrix"])
        receipt_spec = self.data["payloads"]["command_ack_receipt"]["field_specs"]
        self.assertEqual(receipt_spec["receipt_code"]["maximum"], 3)
        self.assertEqual(receipt_spec["error_code"]["maximum"], 4)
        timeout_mutation = dict(self.data["wire_enums"]["ack_receipt"], timeout=4)
        self.assertIn(
            "exact_mapping",
            enum_contract_errors(
                timeout_mutation,
                {"ingested": 1, "rejected": 2, "mismatch": 3},
            ),
        )

    def test_server_direct_event_route_has_typed_conversion_override(self):
        conversions = self.data["rpc_conversions"]
        overrides = conversions.get("route_specific_input_argument_rule_overrides")
        self.assertEqual(overrides, EXPECTED_ROUTE_CONVERSION_OVERRIDES)
        if overrides is None:
            return

        rules = conversions["rules"]
        for route_path, override in overrides.items():
            route = resolve_dotted_path(self.data, route_path)
            self.assertEqual(override["rpc"], route["rpc"], route_path)
            rpc_arguments = self.data["rpc"][override["rpc"]]["arguments"]
            for argument, rule_ref in override["input_argument_rule_refs"].items():
                self.assertIn(argument, rpc_arguments, (route_path, argument))
                self.assertIn(rule_ref, rules, (route_path, rule_ref))
                postgres_type = argument.rsplit(" ", 1)[1]
                self.assertEqual(
                    rules[rule_ref]["target_postgres_type"],
                    postgres_type,
                    (route_path, argument),
                )
                if argument == "p_imei text":
                    self.assertEqual(route["identity_conversion_ref"], rule_ref)

        self.assertEqual(
            conversions["input_argument_rule_refs"]["event_ingest"]["p_imei text"],
            "authenticated_imei_to_pg_text",
        )
        self.assertNotEqual(
            overrides["authorized_server_event_routes.device_offline"]
            ["input_argument_rule_refs"]["p_imei text"],
            conversions["input_argument_rule_refs"]["event_ingest"]["p_imei text"],
        )
        confused_override = json.loads(json.dumps(overrides))
        confused_override["authorized_server_event_routes.device_offline"][
            "input_argument_rule_refs"
        ]["p_imei text"] = "authenticated_imei_to_pg_text"
        self.assertNotEqual(confused_override, EXPECTED_ROUTE_CONVERSION_OVERRIDES)

    def test_exact_rpc_signatures_and_proposal_scope(self):
        self.assertEqual(self.data["rpc"], RPC_SIGNATURES)
        self.assertEqual(self.data["rpc_policy"]["contract_status"], "proposal_only")
        proposal = self.data["proposal_scope"]
        self.assertFalse(proposal["sql_changed"])
        self.assertFalse(proposal["emqx_changed"])
        self.assertFalse(proposal["live_schema_changed"])
        self.assertEqual(proposal["implementation_gate"], "g2c_migration_proposal")
        self.assertEqual(
            proposal["migration_requirements"],
            ["caller_graph", "compatibility", "rollback", "user_approval"],
        )

    def test_rpc_argument_sources_are_exact_and_positional(self):
        mappings = self.data["rpc_argument_mappings"]
        expected_payload_rpc = {
            "telemetry": "telemetry_ingest",
            "boot": "boot_ingest",
            "config_request": "config_fetch",
            "command_request": "command_claim",
            "config_ack": "config_ack",
            "command_ack": "command_ack",
            "event": "event_ingest",
        }
        self.assertEqual(
            {item["payload"]: item["rpc"] for item in mappings.values()},
            expected_payload_rpc,
        )

        for mapping_name, mapping in mappings.items():
            family = mapping["payload"]
            fields = self.data["payloads"][family]["fields"]
            rpc_args = self.data["rpc"][mapping["rpc"]]["arguments"]
            sources = mapping["arguments"]
            self.assertEqual(
                [source["argument"] for source in sources], rpc_args, mapping_name
            )

            mapped_payload_fields = set()
            for source in sources:
                source_kind = source["source_kind"]
                if source_kind == "payload_field":
                    index = source["payload_index"]
                    field = source["field"]
                    self.assertEqual(fields[index], field, (mapping_name, source))
                    mapped_payload_fields.add(field)
                elif source_kind == "authenticated_identity_after_topic_match":
                    self.assertEqual(source["field"], "imei")
                    self.assertEqual(
                        source["identity_binding_ref"], "mqtt_identity_binding"
                    )
                elif source_kind == "server_policy":
                    self.assertEqual(source["field"], "command_claim_lease_seconds")
                else:
                    self.fail(f"unknown source kind in {mapping_name}: {source_kind}")

            gateway_fields = set(mapping["gateway_validated_payload_fields"])
            self.assertEqual(mapped_payload_fields | gateway_fields, set(fields))
            self.assertFalse(mapped_payload_fields & gateway_fields)

    def test_rpc_output_to_response_payload_mapping_is_exact(self):
        mappings = self.data["rpc_output_mappings"]
        self.assertEqual(
            set(mappings),
            {"config_response", "command_response", "command_ack_receipt"},
        )
        self.assertEqual(mappings["config_response"]["rpc"], "config_fetch")
        self.assertEqual(mappings["command_response"]["rpc"], "command_claim")
        self.assertEqual(mappings["command_ack_receipt"]["rpc"], "command_ack")

        for family, mapping in mappings.items():
            fields = self.data["payloads"][family]["fields"]
            positions = mapping["positions"]
            self.assertEqual(
                [item["payload_index"] for item in positions],
                list(range(len(fields))),
            )
            self.assertEqual(
                [item["rpc_output"] for item in positions], fields, family
            )
            self.assertEqual(
                [item["payload_field"] for item in positions], fields, family
            )
            self.assertFalse(mapping["null_output_publish_allowed"])
            self.assertFalse(mapping["empty_output_publish_allowed"])

    def test_event_enum_idempotency_and_clock_contract(self):
        expected = {
            "temperature_high": 1,
            "temperature_low": 2,
            "temperature_recovered": 3,
            "adapter_removed": 4,
            "adapter_restored": 5,
            "poweroff_dying": 6,
            "device_offline": 7,
            "sensor_fault": 8,
            "fota_status": 9,
            "ai_status": 10,
        }
        self.assertEqual(self.data["wire_enums"]["event_type"], expected)
        self.assertEqual(
            self.data["event_semantics"]["idempotency_key"],
            [
                "authenticated_imei_context",
                "event_origin",
                "event_type",
                "incident_id",
                "sequence",
            ],
        )
        event_type = self.data["payloads"]["event"]["field_specs"]["event_type"]
        self.assertEqual(event_type["enum_ref"], "event_type")
        self.assertNotIn("enum", event_type)
        self.assertEqual(
            self.data["wire_enums"]["event_state"],
            {"inactive_or_cleared": 0, "active": 1, "progress": 2, "failed": 3},
        )

    def test_event_idempotency_sources_and_collision_domains_are_exact(self):
        semantics = self.data["event_semantics"]
        context = semantics.get("idempotency_context")
        self.assertEqual(context, EXPECTED_EVENT_IDEMPOTENCY_CONTEXT)
        if context is None:
            return

        self.assertEqual(
            resolve_dotted_path(self.data, context["key_fields_ref"]),
            semantics["idempotency_key"],
        )
        rules = self.data["rpc_conversions"]["rules"]
        imei_sources = context["field_sources"]["authenticated_imei_context"]
        for origin, source in imei_sources.items():
            self.assertEqual(
                resolve_dotted_path(self.data, source["source_ref"]),
                {
                    "device_mqtt_event": "authenticated_identity_after_topic_match",
                    "authorized_server_direct_event_ingest": (
                        "authorized_server_device_scope"
                    ),
                }[origin],
            )
            self.assertIn(source["conversion_rule_ref"], rules, origin)

        schema_origins = {
            schema["producer_route"]
            for schema in semantics["schemas"].values()
        }
        self.assertEqual(
            set(context["field_sources"]["event_origin"]["allowed_values"]),
            schema_origins,
        )
        collision = context["producer_sequence_collision_prevention"]
        self.assertEqual(
            collision["scope_fields"], semantics["idempotency_key"][:-1]
        )
        self.assertEqual(
            collision["sequence_field"], semantics["idempotency_key"][-1]
        )
        self.assertFalse(collision["same_sequence_different_origin_collides"])

        shared = {
            "authenticated_imei_context": "359759088888888",
            "event_type": "device_offline",
            "incident_id": 77,
            "sequence": 3,
        }
        device_key = event_dedupe_key(
            semantics["idempotency_key"],
            event_origin="device_mqtt_event",
            **shared,
        )
        server_key = event_dedupe_key(
            semantics["idempotency_key"],
            event_origin="authorized_server_direct_event_ingest",
            **shared,
        )
        other_device_key = event_dedupe_key(
            semantics["idempotency_key"],
            authenticated_imei_context="359759099999999",
            event_origin="authorized_server_direct_event_ingest",
            event_type="device_offline",
            incident_id=77,
            sequence=3,
        )
        self.assertNotEqual(device_key, server_key)
        self.assertNotEqual(server_key, other_device_key)

        fallback = context["no_open_power_fallback_binding"]
        resolved_fallback = resolve_dotted_path(self.data, fallback["source_ref"])
        self.assertEqual(
            resolved_fallback,
            semantics["power_incident_continuity"]["no_open_power_fallback"],
        )
        self.assertEqual(fallback["incident_kind"], resolved_fallback["incident_kind"])
        collision_mutation = json.loads(json.dumps(context))
        collision_mutation["producer_sequence_collision_prevention"][
            "same_sequence_different_origin_collides"
        ] = True
        self.assertNotEqual(collision_mutation, EXPECTED_EVENT_IDEMPOTENCY_CONTEXT)

    def test_event_owner_service_ids_are_concrete(self):
        schemas = self.data["event_semantics"]["schemas"]
        owners = {
            event_type: schema["owner"] for event_type, schema in schemas.items()
        }
        self.assertEqual(
            owners,
            {
                event_type: schema["owner"]
                for event_type, schema in EXPECTED_EVENT_SCHEMAS.items()
            },
        )
        self.assertFalse(
            set(owners.values())
            & {
                "alarm_owner",
                "power_owner",
                "server_monitor_owner",
                "sensor_owner",
                "fota_owner",
                "ai_owner",
            }
        )

    def test_event_schemas_are_exact_and_power_incident_safe(self):
        semantics = self.data["event_semantics"]
        schemas = semantics.get("schemas")
        self.assertIsNotNone(schemas)
        if schemas is None:
            return
        self.assertEqual(
            set(schemas), set(self.data["wire_enums"]["event_type"])
        )
        self.assertEqual(event_schema_errors(schemas, EXPECTED_EVENT_SCHEMAS), [])
        self.assertEqual(
            semantics["schema_coverage"], "exact_wire_event_type_keys"
        )
        self.assertEqual(
            semantics["power_incident_continuity"],
            EXPECTED_POWER_INCIDENT_CONTINUITY,
        )
        event_fields = self.data["payloads"]["event"]["field_specs"]
        self.assertEqual(event_fields["value0"]["unit"], "event_schema_value0")
        self.assertEqual(event_fields["value1"]["unit"], "event_schema_value1")
        self.assertEqual(event_fields["value0"]["zero_code"], "per_event_schema")
        self.assertEqual(event_fields["value1"]["zero_code"], "per_event_schema")

        owner_mutation = json.loads(json.dumps(schemas))
        owner_mutation["device_offline"]["owner"] = "device_owner"
        self.assertIn(
            "device_offline",
            event_schema_errors(owner_mutation, EXPECTED_EVENT_SCHEMAS),
        )
        unit_mutation = json.loads(json.dumps(schemas))
        unit_mutation["temperature_high"]["value0"]["unit"] = "celsius_float"
        self.assertIn(
            "temperature_high",
            event_schema_errors(unit_mutation, EXPECTED_EVENT_SCHEMAS),
        )
        continuity_mutation = json.loads(
            json.dumps(semantics["power_incident_continuity"])
        )
        continuity_mutation["same_open_incident_id_required"] = False
        self.assertNotEqual(continuity_mutation, EXPECTED_POWER_INCIDENT_CONTINUITY)


if __name__ == "__main__":
    unittest.main()
