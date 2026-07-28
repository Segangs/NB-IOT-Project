import copy
import inspect
import json
import re
import unittest
from pathlib import Path
from typing import NamedTuple


ROOT = Path(__file__).resolve().parents[2]
CONTRACT = ROOT / "contracts" / "g1" / "firmware_runtime_v1.json"
FLASH_CAMEL_BOUNDARY = re.compile(
    r"(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])"
)
FLASH_NON_ALNUM = re.compile(r"[^A-Za-z0-9]+")
FLASH_HEX_LITERAL = re.compile(r"0x[0-9a-f]+", re.IGNORECASE)
FLASH_DECIMAL_STRING = re.compile(r"[+-]?\d+")
FLASH_LOCATION_TOKENS = frozenset(
    {
        "address",
        "addr",
        "base",
        "offset",
        "origin",
        "start",
        "end",
        "location",
        "locations",
    }
)
KNOWN_NON_FLASH_LOCATION_PATHS = {"flash_fota.private_key_location"}
KNOWN_NOT_FROZEN_FLASH_LOCATION_STATUS_PATHS = {
    "flash_fota.partition_address_status"
}
EXPECTED_FLASH_FOTA_KEYS = {
    "anti_rollback_required",
    "artifact_download_transport",
    "artifacts_immutable",
    "asymmetric_signature_required",
    "automatic_install_schedule_status",
    "automatic_rollout_status",
    "boot_diagnostic_history",
    "boot_failure_recovery_history_required",
    "boot_metadata",
    "bootloader_application_fota_writable",
    "bootloader_immutable",
    "bootloader_responsibilities",
    "central_service_required",
    "confirm_deadline_ms",
    "confirm_optional_degraded_allowed",
    "confirm_required_local_gates",
    "confirmed_slot_writable",
    "device_key_material",
    "download_target",
    "dual_journal_required",
    "dual_journal_required_fields",
    "erase_program_commit_power_cut_tests_required",
    "failed_image_marked_bad",
    "firmware_ab_required",
    "firmware_model_activation_independent",
    "first_release_scope",
    "flash_safe_execute_required",
    "hash_algorithm",
    "key_id_required",
    "key_management_drills_required",
    "lock_order_required",
    "log_entry_alignment_bytes",
    "manifest_required_fields",
    "manifest_serialization",
    "manifest_signed_bytes",
    "manifest_target_slot_is_logical",
    "manifest_target_slot_values",
    "model_activation_checks",
    "model_atomic_activation_required",
    "model_download_target",
    "model_failure_rollback_target",
    "partition_address_status",
    "partition_size_status",
    "physical_supply_chain_security_claimed",
    "private_key_location",
    "record_layout_status",
    "release_pipeline_audit_required",
    "rollback_target",
    "runtime_v2_stability_gate_required",
    "security_claim_scope",
    "service_recovery_methods",
    "shared_artifact_pipeline",
    "signature_algorithm",
    "signature_encoding_status",
    "slot_validation_tests_required",
    "trial_boot_max",
    "trial_policy_provisional",
}
EXPECTED_POWER_INCIDENT_LIFECYCLE = {
    "owner": "power_state_service",
    "scope": "device",
    "identity": {
        "storage": "durable",
        "persistence": "until_closed",
        "open_incident_id_source": "persisted_open_power_incident_id",
        "clear_after_close": True,
    },
    "correlated_events": [
        "adapter_removed",
        "poweroff_dying",
        "device_offline",
        "adapter_restored",
    ],
    "event_transitions": {
        "adapter_removed": "open",
        "poweroff_dying": "progress",
        "device_offline": "correlate_if_open",
        "adapter_restored": "close",
    },
    "open_incident_id_reuse_required": True,
    "restore_before_commit": {
        "shutdown_transition": "cancel",
        "incident_transition": "close",
        "close_incident_id_source": "persisted_open_power_incident_id",
    },
    "restore_after_commit": {
        "shutdown_transition": "never_cancel",
        "incident_transition": "remain_open",
        "close_trigger": "next_successful_boot_or_restore",
        "close_incident_id_source": "persisted_open_power_incident_id",
    },
    "server_offline": {
        "cardinality_per_power_incident": "once",
        "duplicate_suppressed": True,
        "open_power_incident_id_source": "persisted_open_power_incident_id",
        "without_open_power_incident": {
            "incident_kind": "separate_offline_incident",
            "power_incident_label_allowed": False,
        },
    },
    "sequence": {
        "scope": "per_producer_per_incident",
        "monotonic": True,
        "producer_origin_required_for_dedupe": True,
    },
    "required_fields_ref": "power.incident_required_fields",
}
EXPECTED_COMMIT_MARKER_SEMANTICS = {
    "state_meanings": {
        "erased": "uncommitted",
        "programmed": "committed",
        "partial": "invalid",
        "torn": "invalid",
        "unknown": "invalid",
    },
    "valid_committed_state": "programmed",
    "invalid_states": ["erased", "partial", "torn", "unknown"],
    "exact_bit_pattern_status": "not_frozen_until_record_layout_approval",
    "program_transition": {
        "from_state": "erased",
        "to_state": "programmed",
        "write_unit": "flash_program_unit",
        "allowed_after_step": "readback_verify_fields_and_crc",
        "last_program_operation_required": True,
    },
    "old_valid_copy_preserved_until": "new_copy_fully_committed_and_verified",
}
EXPECTED_SLOT_STATE_INVARIANTS = {
    "representation": "logical_a_b_or_none_only",
    "logical_firmware_slots": ["logical_a", "logical_b"],
    "none_value": "none",
    "numeric_slot_codes_allowed": False,
    "physical_layout_values_allowed": False,
    "active_slot": {
        "allowed_values": ["logical_a", "logical_b"],
        "firmware_validation_required": True,
    },
    "confirmed_slot": {
        "allowed_values": ["logical_a", "logical_b"],
        "firmware_validation_required": True,
        "role": "rollback_target",
        "must_not_equal_field": "bad_slot",
    },
    "pending_slot": {
        "allowed_values": ["none", "logical_a", "logical_b"],
        "non_none_firmware_validation_required": True,
        "non_none_must_not_equal_field": "active_slot",
        "trial_must_not_equal_field": "confirmed_slot",
    },
    "bad_slot": {
        "allowed_values": ["none", "logical_a", "logical_b"],
        "non_none_requires_failed_candidate": True,
        "must_not_equal_fields": [
            "active_slot",
            "confirmed_slot",
            "pending_slot_when_non_none",
        ],
    },
    "trial_count": {
        "type": "integer_not_boolean",
        "minimum": 0,
        "maximum_ref": "flash_fota.trial_boot_max",
        "range_inclusive": True,
        "out_of_range_result": "record_invalid",
    },
    "next_boot_actions": {
        "confirmed_no_pending_no_bad": {
            "requires": {
                "active_slot": "valid_firmware_logical_slot",
                "pending_slot": "none",
                "bad_slot": "none",
                "active_distinct_from_bad": True,
            },
            "action": "boot_active",
        },
        "valid_pending_trial": {
            "requires": {
                "pending_slot": "valid_firmware_logical_slot",
                "pending_distinct_from": [
                    "active_slot",
                    "confirmed_slot",
                    "bad_slot",
                ],
                "trial_count_relation": "less_than_trial_boot_max",
                "durable_pre_jump_increment_required": True,
            },
            "action": "durably_increment_trial_count_then_boot_pending",
        },
        "trial_exhausted_pending": {
            "requires": {
                "pending_slot": "valid_firmware_logical_slot",
                "pending_distinct_from": [
                    "active_slot",
                    "confirmed_slot",
                    "bad_slot",
                ],
                "trial_count_relation": "equal_to_trial_boot_max",
            },
            "action": "mark_pending_bad_then_rollback_to_confirmed",
        },
        "failed_bad_candidate": {
            "requires": {
                "bad_slot": "failed_candidate",
                "confirmed_slot": "valid_firmware_logical_slot_distinct_from_bad",
            },
            "action": "rollback_to_confirmed",
        },
        "impossible_or_trial_overflow": {
            "record_result": "invalid",
            "copy_selection_fallback": "older_valid_copy",
            "zero_valid_copies": "service_recovery_without_image_write",
        },
    },
    "impossible_combination_result": "record_invalid",
}
EXPECTED_COPY_TOPOLOGY = {
    "logical_copy_ids": ["copy_a", "copy_b"],
    "snapshot_cardinality": "at_most_one_record_per_copy_id",
    "validation_order": "before_record_filtering_and_selection",
    "unknown_copy_id_result": "service_recovery_without_image_write",
    "duplicate_copy_id_result": "service_recovery_without_image_write",
    "one_surviving_known_copy_allowed": True,
    "journal_target": "other_logical_copy_id",
}
EXPECTED_LOGICAL_VALIDATION_ENVELOPE = {
    "abstraction": {
        "normative_semantics": {
            "language_scope": "language_neutral",
            "physical_record_layout_claimed": False,
            "production_container_abi_claimed": False,
        },
        "host_executable_model": {
            "language": "python",
            "representation_scope": "executable_model_only",
            "representation_labels_normative_for_production": False,
            "production_firmware_evidence": False,
        },
        "production_binding": {
            "language_binding": "implementation_specific",
            "container_binding": "implementation_specific",
            "equivalent_semantics_required": True,
            "python_representation_required": False,
        },
    },
    "snapshot": {
        "normative_kind": "finite_ordered_sequence",
        "executable_model_representations": [
            "python_list",
            "python_tuple",
        ],
        "finite": True,
        "ordered": True,
        "cardinality": {
            "minimum": 0,
            "maximum_ref": "flash_fota.boot_metadata.copy_count",
            "range_inclusive": True,
        },
        "element_semantics": "keyed_logical_validation_envelope",
        "validation_order": (
            "container_mapping_and_copy_identity_before_topology_then_"
            "record_fields_before_selection"
        ),
        "malformed_result": "service_recovery_without_image_write",
        "copy_identity_boundary": {
            "required_field": "copy_id",
            "type": "string",
            "missing_or_wrong_type_result": (
                "service_recovery_without_image_write"
            ),
        },
    },
    "record": {
        "normative_kind": "keyed_logical_validation_envelope",
        "executable_model_representations": ["python_dict"],
        "required_fields": [
            "copy_id",
            "format_version",
            "length",
            "sequence",
            "state",
            "supported_format",
            "approved_length",
            "crc_matches",
            "commit_marker_state",
            "active_slot",
            "confirmed_slot",
            "pending_slot",
            "bad_slot",
            "trial_count",
            "version",
            "hash",
            "rollback_reason",
            "valid_firmware_slots",
            "failed_candidate_slots",
        ],
        "unknown_fields": "reject_record",
        "invalid_record_result": (
            "record_invalid_with_other_valid_copy_fallback"
        ),
        "untrusted_claim_fields": [
            "candidate_verified",
            "confirmed_valid",
        ],
        "untrusted_claim_result": "record_invalid",
        "field_schemas": {
            "copy_id": {"type": "string"},
            "format_version": {
                "type": "integer_not_boolean",
                "minimum": 1,
                "range_validation_phase": "canonical_semantic_projection",
            },
            "length": {
                "type": "integer_not_boolean",
                "minimum": 1,
                "range_validation_phase": "canonical_semantic_projection",
            },
            "sequence": {
                "type": "integer_not_boolean",
                "minimum": 0,
                "maximum_status": "not_frozen_until_record_layout_approval",
                "range_validation_phase": "canonical_semantic_projection",
            },
            "state": {
                "type": "string",
                "allowed_values": [
                    "committed",
                    "torn",
                    "uncommitted",
                    "crc_invalid",
                ],
            },
            "supported_format": {"type": "exact_boolean"},
            "approved_length": {"type": "exact_boolean"},
            "crc_matches": {"type": "exact_boolean"},
            "commit_marker_state": {
                "type": "string",
                "allowed_values_ref": (
                    "flash_fota.boot_metadata.commit_marker_semantics."
                    "state_meanings"
                ),
            },
            "active_slot": {
                "type": "string",
                "allowed_values_ref": (
                    "flash_fota.boot_metadata.slot_state_invariants."
                    "logical_firmware_slots"
                ),
            },
            "confirmed_slot": {
                "type": "string",
                "allowed_values_ref": (
                    "flash_fota.boot_metadata.slot_state_invariants."
                    "logical_firmware_slots"
                ),
            },
            "pending_slot": {
                "type": "string",
                "allowed_values_ref": (
                    "flash_fota.boot_metadata.slot_state_invariants."
                    "pending_slot.allowed_values"
                ),
            },
            "bad_slot": {
                "type": "string",
                "allowed_values_ref": (
                    "flash_fota.boot_metadata.slot_state_invariants."
                    "bad_slot.allowed_values"
                ),
            },
            "trial_count": {
                "type": "integer_not_boolean",
                "minimum": 0,
                "maximum_ref": "flash_fota.trial_boot_max",
                "range_inclusive": True,
                "range_validation_phase": "slot_state_invariants",
            },
            "version": {
                "type": "string",
                "semantic_ref": (
                    "flash_fota.boot_metadata.candidate_trial_transitions."
                    "individual_committed_record_identity.version"
                ),
            },
            "hash": {
                "type": "string",
                "semantic_ref": (
                    "flash_fota.boot_metadata.candidate_trial_transitions."
                    "individual_committed_record_identity.hash"
                ),
            },
            "rollback_reason": {
                "type": "string",
                "allowed_values_ref": (
                    "flash_fota.boot_metadata.candidate_trial_transitions."
                    "individual_committed_record_state."
                    "rollback_reason_domain"
                ),
            },
            "valid_firmware_slots": {
                "type": "finite_slot_collection",
                "normative_collection_semantics": "finite_mathematical_set",
                "executable_model_representations": [
                    "python_list",
                    "python_tuple",
                    "python_set",
                    "python_frozenset",
                ],
                "element_type": "string",
                "allowed_values_ref": (
                    "flash_fota.boot_metadata.slot_state_invariants."
                    "logical_firmware_slots"
                ),
                "duplicate_semantics": "mathematical_set",
            },
            "failed_candidate_slots": {
                "type": "finite_slot_collection",
                "normative_collection_semantics": "finite_mathematical_set",
                "executable_model_representations": [
                    "python_list",
                    "python_tuple",
                    "python_set",
                    "python_frozenset",
                ],
                "element_type": "string",
                "allowed_values_ref": (
                    "flash_fota.boot_metadata.slot_state_invariants."
                    "logical_firmware_slots"
                ),
                "duplicate_semantics": "mathematical_set",
            },
        },
    },
    "field_roles": {
        "coverage": {
            "required_fields_ref": (
                "flash_fota.boot_metadata.logical_validation_envelope."
                "record.required_fields"
            ),
            "membership_rule": "exactly_one_role_per_required_field",
            "union_rule": "equals_required_field_set",
        },
        "roles": {
            "journal_semantic_fields": {
                "fields_ref": (
                    "flash_fota.boot_metadata.crc.semantic_coverage_fields"
                ),
                "field_order": "ordered_reference",
                "source": "committed_boot_metadata_journal_semantics",
                "logical_persistence": True,
                "crc_covered": True,
                "authority_alone": False,
                "physical_byte_layout_claimed": False,
                "physical_byte_layout_status_ref": (
                    "flash_fota.boot_metadata.crc.byte_layout_status"
                ),
            },
            "storage_copy_reader_binding": {
                "fields": ["copy_id"],
                "source": "trusted_logical_storage_copy_reader_binding",
                "record_payload_authority": False,
                "logical_persistence": False,
                "persisted_in_journal_payload": False,
                "crc_covered": False,
                "authority_alone": False,
                "physical_copy_binding_status": (
                    "address_and_value_not_frozen"
                ),
            },
            "decoder_validation_annotations": {
                "fields": [
                    "state",
                    "supported_format",
                    "approved_length",
                    "crc_matches",
                ],
                "role": "bootloader_decoder_validation_annotation",
                "source": "bootloader_record_decoder_and_validator",
                "logical_persistence": False,
                "persisted_in_journal_payload": False,
                "crc_covered": False,
                "authority_alone": False,
                "physical_byte_layout_claimed": False,
            },
            "commit_marker_logical_annotation": {
                "fields": ["commit_marker_state"],
                "role": "decoded_underlying_commit_marker_annotation",
                "source": "decoded_underlying_commit_marker",
                "logical_persistence": False,
                "persisted_in_journal_payload": False,
                "added_journal_semantic_field": False,
                "crc_covered": False,
                "authority_alone": False,
                "physical_byte_layout_claimed": False,
                "underlying_marker": {
                    "persisted": True,
                    "crc_covered": False,
                    "exact_bit_pattern_status_ref": (
                        "flash_fota.boot_metadata.commit_marker_semantics."
                        "exact_bit_pattern_status"
                    ),
                },
            },
            "host_executable_model_validation_context": {
                "fields": [
                    "valid_firmware_slots",
                    "failed_candidate_slots",
                ],
                "role": (
                    "host_executable_model_validation_context_not_"
                    "physical_journal_fields"
                ),
                "representation_scope": "executable_model_only",
                "field_sources": {
                    "valid_firmware_slots": (
                        "recomputed_bootloader_slot_validation"
                    ),
                    "failed_candidate_slots": (
                        "committed_bad_candidate_and_failure_validation_"
                        "context"
                    ),
                },
                "logical_persistence": False,
                "persisted_in_journal_payload": False,
                "crc_covered": False,
                "authority_alone": False,
                "physical_byte_layout_claimed": False,
                "external_revalidation_separation": {
                    "current_pending_and_confirmed_revalidation_"
                    "separately_required": True,
                    "ref": (
                        "flash_fota.boot_metadata.boot_decision."
                        "external_revalidation"
                    ),
                },
            },
        },
    },
    "malformed_results": {
        "unidentifiable_snapshot_or_topology": (
            "service_recovery_without_image_write"
        ),
        "identified_invalid_record": (
            "record_invalid_with_other_valid_copy_fallback"
        ),
        "zero_remaining_valid_records": (
            "service_recovery_without_image_write"
        ),
        "decision_service_recovery_shape": "null_source_next_and_action",
        "classifier": "invalid",
        "transition_validator": False,
        "semantic_projection": "none",
        "individual_validator": False,
    },
}
EXPECTED_EXTERNAL_REVALIDATION_INPUT_SCHEMA = {
    "container": {
        "normative_collection_semantics": "finite_mathematical_set",
        "executable_model_representations": [
            "python_list",
            "python_tuple",
            "python_set",
            "python_frozenset",
        ],
        "arbitrary_iterables_allowed": False,
        "invalid_element_invalidates_whole_container": True,
        "duplicate_semantics": "mathematical_set",
    },
    "omitted_or_none": {
        "pending_candidate_evidence": "empty_exact_identity_set",
        "confirmed_bootable_slot_evidence": "empty_exact_slot_set",
    },
    "pending_candidate_evidence": {
        "element_semantics": "exact_ordered_identity_product",
        "executable_model_representation": "python_tuple",
        "arity": 3,
        "field_order": ["pending_slot", "version", "hash"],
        "pending_slot": {
            "type": "string",
            "allowed_values_ref": (
                "flash_fota.boot_metadata.slot_state_invariants."
                "logical_firmware_slots"
            ),
            "none_allowed": False,
        },
        "version": {
            "type": "non_blank_string",
            "normalization_allowed": False,
        },
        "hash": {"format": "lowercase_hex_64"},
    },
    "confirmed_bootable_slot_evidence": {
        "element_semantics": "exact_logical_slot_string",
        "allowed_values_ref": (
            "flash_fota.boot_metadata.slot_state_invariants."
            "logical_firmware_slots"
        ),
    },
    "malformed_non_none_result": {
        "malformed_is_absence": False,
        "decision": "service_recovery_without_image_write_with_null_outputs",
        "classifier": "invalid",
        "transition_validator": False,
    },
    "historical_structural_replay": {
        "journal_commit_authorization": False,
        "ephemeral_evidence_required": False,
    },
}
EXPECTED_INDIVIDUAL_RECORD_STATE = {
    "required_check": "rollback_reason_state_relationships",
    "applies_to": "every_committed_record_including_sole_or_base_copy",
    "rollback_reason_domain": [
        "none",
        "explicit_trial_failure",
        "trial_exhausted",
        "pending_revalidation_failed",
    ],
    "pending_state": {
        "pending_slot": "non_none",
        "bad_slot": "none",
        "rollback_reason": "none",
        "trial_count": "zero_to_trial_boot_max_inclusive",
    },
    "confirmed_state": {
        "pending_slot": "none",
        "bad_slot": "none",
        "active_equals_confirmed": True,
        "trial_count": 0,
        "rollback_reason": "none",
    },
    "failed_state": {
        "pending_slot": "none",
        "bad_slot": "non_none",
        "trial_count": 0,
        "rollback_reason": [
            "explicit_trial_failure",
            "trial_exhausted",
            "pending_revalidation_failed",
        ],
    },
    "invalid_relationship_result": "record_invalid",
}
EXPECTED_CANDIDATE_TRIAL_TRANSITIONS = {
    "candidate_identity_fields": ["pending_slot", "version", "hash"],
    "bad_identity_fields": ["bad_slot", "version", "hash"],
    "identity_comparison": "exact_tuple",
    "version_source": "verified_manifest.version",
    "hash_source": "verified_manifest.sha256",
    "hash_semantics": "verified_artifact_sha256",
    "individual_committed_record_identity": {
        "required_check": "candidate_identity_shape",
        "applies_to": "every_committed_candidate_including_sole_or_base_copy",
        "version": {
            "type": "non_blank_string",
            "source": "verified_manifest.version",
            "normalization_allowed": False,
        },
        "hash": {
            "format": "lowercase_hex_64",
            "source": "verified_manifest.sha256",
            "semantics": "verified_artifact_sha256",
        },
        "field_ownership_ref": (
            "flash_fota.boot_metadata.candidate_trial_transitions.field_ownership"
        ),
        "incoherent_ownership_result": "record_invalid",
    },
    "individual_committed_record_state": EXPECTED_INDIVIDUAL_RECORD_STATE,
    "verification_evidence": {
        "source": "bootloader_revalidated_manifest_and_artifact",
        "metadata_boolean_trusted": False,
        "identity_fields": ["pending_slot", "version", "hash"],
        "match_rule": "exact_tuple",
        "input_schema_ref": (
            "flash_fota.boot_metadata.boot_decision.external_revalidation."
            "input_schema"
        ),
        "journal_commit_authorization_transitions": [
            "stage_verified_candidate",
            "restage_same_candidate",
        ],
        "current_boot_pending_revalidation": True,
        "recomputed_checks": [
            "manifest_signature",
            "manifest_version_binding",
            "artifact_sha256",
            "anti_rollback",
            "hardware_revision",
            "target_slot",
        ],
        "missing_or_mismatched_result": (
            "pending_revalidation_failed_boot_decision"
        ),
    },
    "field_ownership": {
        "pending_non_none": "pending_candidate_identity",
        "bad_non_none": "bad_candidate_identity",
        "pending_none_bad_none": "active_confirmed_identity",
        "pending_and_bad_non_none_allowed": False,
    },
    "transition_validation": {
        "source_record_required": True,
        "destination_record_required": True,
        "destination_shape_alone_sufficient": False,
        "simple_field_mutation_allowed": False,
        "invalid_result": "record_invalid",
        "commit_mechanism": "new_boot_metadata_journal_record",
        "atomicity_fields_share_one_committed_record": True,
        "classifier_ref": (
            "flash_fota.boot_metadata.candidate_trial_transitions."
            "transition_classifier"
        ),
        "selection_acceptance": "exactly_one_classified_transition",
        "sequence_rule": "destination_equals_source_plus_one",
        "copy_target_rule": "destination_is_other_logical_copy_id",
    },
    "transition_classifier": {
        "journal_transition_kinds": [
            "stage_verified_candidate",
            "restage_same_candidate",
            "attempt",
            "confirm",
            "fail_or_exhaust",
            "pending_revalidation_failed",
        ],
        "exactly_one_match_required": True,
        "zero_matches": "invalid",
        "multiple_matches": "ambiguous",
        "reboot": (
            "reselect_same_existing_committed_record_without_new_record_"
            "or_sequence_advance"
        ),
        "higher_sequence_noop": (
            "restage_same_candidate_only_with_exact_commit_evidence"
        ),
        "common_unchanged_fields": ["format_version", "length"],
        "rollback_reason_rules": {
            "stage_verified_candidate": "none",
            "restage_same_candidate": "unchanged",
            "attempt": "unchanged",
            "confirm": "none",
            "fail_or_exhaust": "trigger_specific",
            "pending_revalidation_failed": "pending_revalidation_failed",
        },
    },
    "verified_new_candidate_staging": {
        "requires": [
            "candidate_identity_differs_from_source_pending_identity",
            "candidate_identity_differs_from_source_bad_identity",
            "existing_manifest_and_artifact_validation_security_gates_passed",
        ],
        "atomic_fields": [
            "pending_slot",
            "version",
            "hash",
            "trial_count",
            "bad_slot",
            "rollback_reason",
        ],
        "trial_count": 0,
        "bad_clear_rule": "only_same_slot_different_verified_identity",
        "first_boot_transition": (
            "durably_commit_0_to_1_then_boot_pending"
        ),
    },
    "same_candidate_restage_or_retry": {
        "identity_rule": "exact_candidate_identity_unchanged",
        "trial_count_rule": "preserve_exactly_never_reset_or_decrease",
        "reboot_rule": "preserve_last_committed_trial_count",
        "unchanged_fields": [
            "format_version",
            "length",
            "active_slot",
            "confirmed_slot",
            "pending_slot",
            "bad_slot",
            "trial_count",
            "version",
            "hash",
            "rollback_reason",
        ],
    },
    "attempt": {
        "identity_rule": "exact_pending_candidate_identity_unchanged",
        "allowed_trial_count_transitions": ["0_to_1", "1_to_2"],
        "durable_commit_before_jump": True,
        "action_after_commit": "boot_pending",
        "reboot_preserves_committed_count": True,
        "unchanged_fields": [
            "format_version",
            "length",
            "active_slot",
            "confirmed_slot",
            "pending_slot",
            "bad_slot",
            "version",
            "hash",
            "rollback_reason",
        ],
    },
    "bad_identity_reuse": {
        "same_exact_identity": "reject_restage_and_trial_reset",
        "same_slot_different_verified_version_or_hash": (
            "clear_bad_and_stage_with_trial_count_zero"
        ),
    },
    "confirmation_terminal": {
        "atomic_destination": {
            "format_version": "source.format_version",
            "length": "source.length",
            "active_slot": "source.pending_slot",
            "confirmed_slot": "source.pending_slot",
            "pending_slot": "none",
            "bad_slot": "none",
            "trial_count": 0,
            "version": "source.version",
            "hash": "source.hash",
            "rollback_reason": "none",
        }
    },
    "failure_or_exhaustion_terminal": {
        "triggers": [
            "explicit_trial_failure",
            "trial_count_reached_max_without_confirmation",
            "pending_revalidation_failed",
        ],
        "source_trial_count_rules": {
            "explicit_trial_failure": "one_to_trial_boot_max_inclusive",
            "trial_count_reached_max_without_confirmation": (
                "equal_to_trial_boot_max"
            ),
            "pending_revalidation_failed": (
                "zero_to_trial_boot_max_inclusive"
            ),
        },
        "rollback_reason_by_trigger": {
            "explicit_trial_failure": "explicit_trial_failure",
            "trial_count_reached_max_without_confirmation": "trial_exhausted",
            "pending_revalidation_failed": "pending_revalidation_failed",
        },
        "atomic_destination": {
            "format_version": "source.format_version",
            "length": "source.length",
            "active_slot": "source.confirmed_slot",
            "confirmed_slot": "source.confirmed_slot",
            "pending_slot": "none",
            "bad_slot": "source.pending_slot",
            "trial_count": 0,
            "version": "source.version",
            "hash": "source.hash",
            "rollback_reason": "trigger_specific_exact_value",
        },
        "bad_identity_retained_for_same_candidate_rejection": True,
        "pending_revalidation_failed_authorization": {
            "pending_identity_evidence": "exact_identity_absent",
            "confirmed_slot_evidence": "exact_slot_present",
            "metadata_boolean_trusted": False,
            "historical_committed_replay": (
                "structural_validation_without_runtime_evidence"
            ),
            "input_schema_ref": (
                "flash_fota.boot_metadata.boot_decision."
                "external_revalidation.input_schema"
            ),
        },
    },
    "journal_power_cut": {
        "before_new_record_fully_committed_and_verified": (
            "select_older_valid_copy"
        ),
        "zero_valid_copies": "service_recovery_without_image_write",
        "pending_revalidation_terminal_power_cut": (
            "reselect_prior_pending_metadata_and_repeat_revalidation"
        ),
        "attempt_record_power_cut": (
            "reselect_prior_pending_count_and_repeat_increment_commit"
        ),
        "trial_exhaustion_terminal_power_cut": (
            "reselect_prior_exhausted_pending_and_repeat_rollback_commit"
        ),
        "uncounted_pending_boot_allowed": False,
    },
}

EXPECTED_BOOT_DECISION = {
    "metadata_selection": {
        "before_current_artifact_revalidation": True,
        "pending_revalidation_part_of_committed_metadata_validity": False,
        "decision_uses_exact_selected_record_reference": True,
    },
    "external_revalidation": {
        "pending_identity_source": (
            "bootloader_recomputed_manifest_and_artifact_checks"
        ),
        "pending_identity_fields": ["pending_slot", "version", "hash"],
        "confirmed_slot_source": (
            "bootloader_revalidated_bootable_firmware_slot"
        ),
        "confirmed_slot_identity": "exact_logical_slot_identifier",
        "metadata_boolean_trusted": False,
        "input_schema": EXPECTED_EXTERNAL_REVALIDATION_INPUT_SCHEMA,
    },
    "result_shape": {
        "fields": [
            "disposition",
            "source_record_reference",
            "next_journal_record",
            "post_commit_action",
        ],
        "service_recovery_source_record_reference": "none",
        "no_next_journal_record": "none",
        "no_post_commit_action": "none",
    },
    "dispositions": {
        "no_valid_metadata": "service_recovery_without_image_write",
        "no_pending_confirmed_valid": "boot_confirmed",
        "pending_valid_trial_remaining": (
            "attempt_required_commit_then_boot_pending"
        ),
        "pending_valid_trial_exhausted": (
            "rollback_required_trial_exhausted_commit_then_boot_confirmed"
        ),
        "pending_invalid_confirmed_valid": (
            "rollback_required_pending_revalidation_failed_commit_then_"
            "boot_confirmed"
        ),
        "confirmed_invalid": "service_recovery_without_image_write",
    },
    "attempt_record": {
        "transition_kind": "attempt",
        "allowed_source_trial_counts": "zero_or_one",
        "atomic_destination": {
            "copy_id": "other_logical_copy_id",
            "format_version": "source.format_version",
            "length": "source.length",
            "sequence": "source.sequence_plus_one",
            "active_slot": "source.active_slot",
            "confirmed_slot": "source.confirmed_slot",
            "pending_slot": "source.pending_slot",
            "bad_slot": "none",
            "trial_count": "source.trial_count_plus_one",
            "version": "source.version",
            "hash": "source.hash",
            "rollback_reason": "none",
        },
        "commit_before_post_commit_action": True,
        "post_commit_action": "boot_pending",
        "power_cut_before_commit": (
            "reselect_prior_pending_count_and_repeat_increment_commit"
        ),
        "uncounted_pending_boot_allowed": False,
    },
    "trial_exhaustion_terminal": {
        "transition_kind": "fail_or_exhaust",
        "source_trial_count": "trial_boot_max",
        "atomic_destination": {
            "copy_id": "other_logical_copy_id",
            "format_version": "source.format_version",
            "length": "source.length",
            "sequence": "source.sequence_plus_one",
            "active_slot": "source.confirmed_slot",
            "confirmed_slot": "source.confirmed_slot",
            "pending_slot": "none",
            "bad_slot": "source.pending_slot",
            "trial_count": 0,
            "version": "source.version",
            "hash": "source.hash",
            "rollback_reason": "trial_exhausted",
        },
        "commit_before_post_commit_action": True,
        "post_commit_action": "boot_confirmed",
        "power_cut_before_commit": (
            "reselect_prior_exhausted_pending_and_repeat_rollback_commit"
        ),
        "pending_boot_allowed": False,
    },
    "pending_revalidation_failure_terminal": {
        "transition_kind": "pending_revalidation_failed",
        "allowed_at_trial_count_zero": True,
        "atomic_destination": {
            "copy_id": "other_logical_copy_id",
            "format_version": "source.format_version",
            "length": "source.length",
            "sequence": "source.sequence_plus_one",
            "active_slot": "source.confirmed_slot",
            "confirmed_slot": "source.confirmed_slot",
            "pending_slot": "none",
            "bad_slot": "source.pending_slot",
            "trial_count": 0,
            "version": "source.version",
            "hash": "source.hash",
            "rollback_reason": "pending_revalidation_failed",
        },
        "commit_before_post_commit_action": True,
        "post_commit_action": "boot_confirmed",
        "power_cut_before_commit": (
            "reselect_prior_pending_metadata_and_repeat_revalidation"
        ),
        "unverified_pending_boot_allowed": False,
    },
}
EXPECTED_SERVICE_RECOVERY_DECISION = {
    "disposition": "service_recovery_without_image_write",
    "source_record_reference": None,
    "next_journal_record": None,
    "post_commit_action": None,
}
FLASH_SCALAR_SCHEMA = object()
FLASH_STRING_LIST_SCHEMA = object()


def schema_from_static_template(value):
    if isinstance(value, dict):
        return {
            key: schema_from_static_template(child)
            for key, child in value.items()
        }
    if isinstance(value, list):
        if not all(type(child) is str for child in value):
            raise ValueError("static Flash schema templates require string lists")
        return FLASH_STRING_LIST_SCHEMA
    return FLASH_SCALAR_SCHEMA


EXPECTED_BOOT_METADATA_SCHEMA = {
    "copy_count": FLASH_SCALAR_SCHEMA,
    "copy_validation": FLASH_SCALAR_SCHEMA,
    "copy_topology": schema_from_static_template(EXPECTED_COPY_TOPOLOGY),
    "logical_validation_envelope": schema_from_static_template(
        EXPECTED_LOGICAL_VALIDATION_ENVELOPE
    ),
    "crc": {
        "algorithm": FLASH_SCALAR_SCHEMA,
        "semantic_coverage_fields": FLASH_STRING_LIST_SCHEMA,
        "excluded_fields": FLASH_STRING_LIST_SCHEMA,
        "byte_layout_status": FLASH_SCALAR_SCHEMA,
        "semantic_equality": {
            "projection_source": FLASH_SCALAR_SCHEMA,
            "physical_byte_layout_claimed": FLASH_SCALAR_SCHEMA,
            "missing_field_result": FLASH_SCALAR_SCHEMA,
            "unsupported_value_shape_result": FLASH_SCALAR_SCHEMA,
        },
    },
    "committed_record_validity": {
        "required_checks": FLASH_STRING_LIST_SCHEMA,
    },
    "commit_marker_semantics": schema_from_static_template(
        EXPECTED_COMMIT_MARKER_SEMANTICS
    ),
    "slot_state_invariants": schema_from_static_template(
        EXPECTED_SLOT_STATE_INVARIANTS
    ),
    "candidate_trial_transitions": schema_from_static_template(
        EXPECTED_CANDIDATE_TRIAL_TRANSITIONS
    ),
    "boot_decision": schema_from_static_template(EXPECTED_BOOT_DECISION),
    "selection": {
        "candidate_state": FLASH_SCALAR_SCHEMA,
        "winner": FLASH_SCALAR_SCHEMA,
        "ignored_states": FLASH_STRING_LIST_SCHEMA,
        "equal_sequence_identical_content": FLASH_SCALAR_SCHEMA,
        "equal_sequence_different_content": FLASH_SCALAR_SCHEMA,
        "identical_projection_copy_id_tiebreak": FLASH_SCALAR_SCHEMA,
        "topology_validation": FLASH_SCALAR_SCHEMA,
        "selected_record_reference": {
            "type": FLASH_SCALAR_SCHEMA,
            "fields": FLASH_STRING_LIST_SCHEMA,
            "projection_source": FLASH_SCALAR_SCHEMA,
        },
        "service_recovery_selected_record_reference": FLASH_SCALAR_SCHEMA,
        "zero_valid_copies": FLASH_SCALAR_SCHEMA,
    },
    "write_commit_sequence": FLASH_STRING_LIST_SCHEMA,
    "old_valid_copy_preserved_until": FLASH_SCALAR_SCHEMA,
    "sequence": {
        "domain": FLASH_SCALAR_SCHEMA,
        "monotonic": FLASH_SCALAR_SCHEMA,
        "wrap_policy": FLASH_SCALAR_SCHEMA,
    },
    "trial_boot_sequence": FLASH_STRING_LIST_SCHEMA,
    "reset_before_confirmation_consumes_trial": FLASH_SCALAR_SCHEMA,
    "provisional_confirmation": {
        "deadline_ms_ref": FLASH_SCALAR_SCHEMA,
        "deadline_begins_at": FLASH_SCALAR_SCHEMA,
        "attempt_state": FLASH_SCALAR_SCHEMA,
        "time_source": FLASH_SCALAR_SCHEMA,
        "absolute_wall_clock_deadline_used": FLASH_SCALAR_SCHEMA,
        "reset_consumes_persisted_trial": FLASH_SCALAR_SCHEMA,
        "trial_boot_max_ref": FLASH_SCALAR_SCHEMA,
        "cross_reset_deadline_extension_allowed": FLASH_SCALAR_SCHEMA,
    },
}
EXPECTED_FLASH_FOTA_SCHEMA = {
    key: FLASH_SCALAR_SCHEMA for key in EXPECTED_FLASH_FOTA_KEYS
}
for string_list_key in {
    "bootloader_responsibilities",
    "service_recovery_methods",
    "dual_journal_required_fields",
    "manifest_required_fields",
    "device_key_material",
    "key_management_drills_required",
    "confirm_required_local_gates",
    "confirm_optional_degraded_allowed",
    "model_activation_checks",
    "slot_validation_tests_required",
    "security_claim_scope",
    "first_release_scope",
}:
    EXPECTED_FLASH_FOTA_SCHEMA[string_list_key] = FLASH_STRING_LIST_SCHEMA
EXPECTED_FLASH_FOTA_SCHEMA["boot_metadata"] = EXPECTED_BOOT_METADATA_SCHEMA
EXPECTED_FLASH_FOTA_SCHEMA["manifest_target_slot_values"] = {
    "firmware": FLASH_SCALAR_SCHEMA,
    "model": FLASH_SCALAR_SCHEMA,
}


def resolve_contract_path(data, dotted_path):
    value = data
    for part in dotted_path.split("."):
        value = value[part]
    return value


def assert_logical_validation_abstraction_and_provenance_contract(
    test_case,
    data,
):
    metadata = data["flash_fota"]["boot_metadata"]
    envelope = metadata["logical_validation_envelope"]
    test_case.assertEqual(envelope, EXPECTED_LOGICAL_VALIDATION_ENVELOPE)

    abstraction = envelope["abstraction"]
    test_case.assertEqual(
        abstraction["normative_semantics"]["language_scope"],
        "language_neutral",
    )
    test_case.assertFalse(
        abstraction["normative_semantics"][
            "physical_record_layout_claimed"
        ]
    )
    test_case.assertFalse(
        abstraction["normative_semantics"][
            "production_container_abi_claimed"
        ]
    )
    test_case.assertEqual(
        abstraction["host_executable_model"]["language"],
        "python",
    )
    test_case.assertEqual(
        abstraction["host_executable_model"]["representation_scope"],
        "executable_model_only",
    )
    test_case.assertFalse(
        abstraction["host_executable_model"][
            "representation_labels_normative_for_production"
        ]
    )
    test_case.assertFalse(
        abstraction["host_executable_model"][
            "production_firmware_evidence"
        ]
    )
    test_case.assertEqual(
        abstraction["production_binding"]["language_binding"],
        "implementation_specific",
    )
    test_case.assertEqual(
        abstraction["production_binding"]["container_binding"],
        "implementation_specific",
    )
    test_case.assertTrue(
        abstraction["production_binding"][
            "equivalent_semantics_required"
        ]
    )
    test_case.assertFalse(
        abstraction["production_binding"][
            "python_representation_required"
        ]
    )

    forbidden_representation_keys = {
        "container_kind",
        "container_kinds",
        "non_none_allowed_kinds",
    }
    bare_python_labels = {"dict", "list", "tuple", "set", "frozenset"}
    scoped_representation_keys = {
        "executable_model_representation",
        "executable_model_representations",
    }

    def assert_scoped_representations(node, path, parent_key=None):
        if isinstance(node, dict):
            test_case.assertFalse(
                forbidden_representation_keys & set(node),
                f"unscoped representation key at {path}",
            )
            for key, child in node.items():
                assert_scoped_representations(
                    child,
                    f"{path}.{key}",
                    key,
                )
        elif isinstance(node, list):
            for index, child in enumerate(node):
                assert_scoped_representations(
                    child,
                    f"{path}[{index}]",
                    parent_key,
                )
        elif isinstance(node, str):
            test_case.assertNotIn(
                node,
                bare_python_labels,
                f"bare Python representation at {path}",
            )
            if node.startswith("python_"):
                test_case.assertIn(
                    parent_key,
                    scoped_representation_keys,
                    f"unscoped Python label at {path}",
                )

    assert_scoped_representations(
        envelope,
        "flash_fota.boot_metadata.logical_validation_envelope",
    )
    external_schema = metadata["boot_decision"]["external_revalidation"][
        "input_schema"
    ]
    test_case.assertEqual(
        external_schema,
        EXPECTED_EXTERNAL_REVALIDATION_INPUT_SCHEMA,
    )
    assert_scoped_representations(
        external_schema,
        "flash_fota.boot_metadata.boot_decision.external_revalidation."
        "input_schema",
    )

    field_roles = envelope["field_roles"]
    required_fields = resolve_contract_path(
        data,
        field_roles["coverage"]["required_fields_ref"],
    )
    test_case.assertEqual(
        required_fields,
        envelope["record"]["required_fields"],
    )
    flattened_role_fields = []
    for role_name, role in field_roles["roles"].items():
        has_fields = "fields" in role
        has_fields_ref = "fields_ref" in role
        test_case.assertNotEqual(
            has_fields,
            has_fields_ref,
            f"role must use exactly one field declaration: {role_name}",
        )
        fields = (
            role["fields"]
            if has_fields
            else resolve_contract_path(data, role["fields_ref"])
        )
        test_case.assertIsInstance(fields, list)
        test_case.assertTrue(all(type(field) is str for field in fields))
        flattened_role_fields.extend(fields)

    test_case.assertEqual(set(flattened_role_fields), set(required_fields))
    test_case.assertEqual(len(flattened_role_fields), len(required_fields))
    for field in required_fields:
        test_case.assertEqual(
            flattened_role_fields.count(field),
            1,
            f"required field role cardinality: {field}",
        )

    roles = field_roles["roles"]
    journal = roles["journal_semantic_fields"]
    test_case.assertEqual(
        resolve_contract_path(data, journal["fields_ref"]),
        metadata["crc"]["semantic_coverage_fields"],
    )
    test_case.assertTrue(journal["logical_persistence"])
    test_case.assertTrue(journal["crc_covered"])
    test_case.assertFalse(journal["authority_alone"])
    test_case.assertFalse(journal["physical_byte_layout_claimed"])

    for role_name in [
        "storage_copy_reader_binding",
        "decoder_validation_annotations",
        "commit_marker_logical_annotation",
        "host_executable_model_validation_context",
    ]:
        role = roles[role_name]
        test_case.assertFalse(role["logical_persistence"])
        test_case.assertFalse(role["persisted_in_journal_payload"])
        test_case.assertFalse(role["crc_covered"])
        test_case.assertFalse(role["authority_alone"])

    copy_binding = roles["storage_copy_reader_binding"]
    test_case.assertFalse(copy_binding["record_payload_authority"])
    test_case.assertEqual(
        copy_binding["physical_copy_binding_status"],
        "address_and_value_not_frozen",
    )

    marker = roles["commit_marker_logical_annotation"]
    test_case.assertFalse(marker["added_journal_semantic_field"])
    test_case.assertTrue(marker["underlying_marker"]["persisted"])
    test_case.assertFalse(marker["underlying_marker"]["crc_covered"])
    test_case.assertEqual(
        resolve_contract_path(
            data,
            marker["underlying_marker"]["exact_bit_pattern_status_ref"],
        ),
        metadata["commit_marker_semantics"]["exact_bit_pattern_status"],
    )

    validation_context = roles["host_executable_model_validation_context"]
    test_case.assertEqual(
        set(validation_context["field_sources"]),
        set(validation_context["fields"]),
    )
    separation = validation_context["external_revalidation_separation"]
    test_case.assertTrue(
        separation[
            "current_pending_and_confirmed_revalidation_separately_required"
        ]
    )
    test_case.assertEqual(
        resolve_contract_path(data, separation["ref"]),
        metadata["boot_decision"]["external_revalidation"],
    )


def assert_power_incident_lifecycle_contract(test_case, lifecycle):
    test_case.assertEqual(lifecycle["identity"]["storage"], "durable")
    test_case.assertEqual(lifecycle["identity"]["persistence"], "until_closed")
    test_case.assertEqual(
        lifecycle["restore_before_commit"]["close_incident_id_source"],
        "persisted_open_power_incident_id",
    )
    test_case.assertEqual(
        lifecycle["restore_after_commit"]["close_incident_id_source"],
        "persisted_open_power_incident_id",
    )
    test_case.assertEqual(lifecycle, EXPECTED_POWER_INCIDENT_LIFECYCLE)


def assert_flash_schema_shape(test_case, value, schema, path):
    if schema is FLASH_SCALAR_SCHEMA:
        if isinstance(value, (dict, list)):
            test_case.fail(f"Flash schema scalar mismatch at {path}")
        return

    if schema is FLASH_STRING_LIST_SCHEMA:
        if not isinstance(value, list):
            test_case.fail(f"Flash schema list mismatch at {path}")
        for index, child in enumerate(value):
            if type(child) is not str:
                test_case.fail(
                    "Flash schema list element mismatch at "
                    f"{path}[{index}]: expected string"
                )
        return

    if not isinstance(value, dict):
        test_case.fail(f"Flash schema object mismatch at {path}")
    actual_keys = set(value)
    expected_keys = set(schema)
    if actual_keys != expected_keys:
        test_case.fail(
            f"Flash schema key mismatch at {path}: "
            f"unknown={sorted(actual_keys - expected_keys)}, "
            f"missing={sorted(expected_keys - actual_keys)}"
        )
    for key, child_schema in schema.items():
        assert_flash_schema_shape(
            test_case,
            value[key],
            child_schema,
            f"{path}.{key}",
        )


def assert_exact_flash_fota_allowed_keys(test_case, flash):
    assert_flash_schema_shape(
        test_case,
        flash,
        EXPECTED_FLASH_FOTA_SCHEMA,
        "flash_fota",
    )


def normalized_flash_key_parts(key):
    camel_separated = FLASH_CAMEL_BOUNDARY.sub("_", key)
    return [
        part.lower()
        for part in FLASH_NON_ALNUM.split(camel_separated)
        if part
    ]


def is_flash_location_key(key):
    parts = normalized_flash_key_parts(key)
    compact = "".join(parts)
    return any(part in FLASH_LOCATION_TOKENS for part in parts) or any(
        compact.startswith(token) or compact.endswith(token)
        for token in FLASH_LOCATION_TOKENS
    )


def assert_no_frozen_flash_location_keys(test_case, value):
    violations = []

    def location_value_kind(child):
        if isinstance(child, (dict, list)):
            return "container"
        if type(child) is int:
            return "decimal"
        if isinstance(child, str) and FLASH_HEX_LITERAL.fullmatch(child):
            return "hex_literal"
        if isinstance(child, str) and FLASH_DECIMAL_STRING.fullmatch(child):
            return "decimal_string"
        return "location_value"

    def walk(node, path):
        if isinstance(node, dict):
            for key, child in node.items():
                child_path = f"{path}.{key}"
                if (
                    is_flash_location_key(key)
                    and child_path not in KNOWN_NON_FLASH_LOCATION_PATHS
                ):
                    parts = normalized_flash_key_parts(key)
                    status_like = bool(parts) and parts[-1] == "status"
                    known_not_frozen_status = (
                        child_path
                        in KNOWN_NOT_FROZEN_FLASH_LOCATION_STATUS_PATHS
                    )
                    if status_like and known_not_frozen_status:
                        if not (
                            isinstance(child, str)
                            and child.startswith("not_frozen")
                        ):
                            violations.append(f"{child_path}:invalid_status")
                    elif status_like:
                        violations.append(f"{child_path}:unknown_status")
                    else:
                        violations.append(
                            f"{child_path}:{location_value_kind(child)}"
                        )
                walk(child, child_path)
        elif isinstance(node, list):
            for index, child in enumerate(node):
                walk(child, f"{path}[{index}]")

    walk(value, "flash_fota")
    if violations:
        test_case.fail(f"forbidden Flash location keys: {violations}")


def assert_boot_metadata_state_semantics_contract(test_case, metadata):
    test_case.assertEqual(
        metadata["commit_marker_semantics"],
        EXPECTED_COMMIT_MARKER_SEMANTICS,
    )
    test_case.assertEqual(
        metadata["slot_state_invariants"],
        EXPECTED_SLOT_STATE_INVARIANTS,
    )
    test_case.assertEqual(
        metadata.get("candidate_trial_transitions"),
        EXPECTED_CANDIDATE_TRIAL_TRANSITIONS,
    )


def make_boot_metadata_copy(
    copy_id,
    sequence,
    *,
    format_version=1,
    length=1,
    state="committed",
    supported_format=True,
    approved_length=True,
    crc_matches=True,
    commit_marker_state="programmed",
    active_slot="logical_a",
    confirmed_slot="logical_a",
    pending_slot="none",
    bad_slot="none",
    trial_count=0,
    version="1.0.0",
    hash=None,
    rollback_reason="none",
    valid_firmware_slots=None,
    failed_candidate_slots=None,
):
    if hash is None:
        hash = "a" * 64
    if valid_firmware_slots is None:
        valid_firmware_slots = ["logical_a", "logical_b"]
    if failed_candidate_slots is None:
        failed_candidate_slots = []
    return {
        "copy_id": copy_id,
        "format_version": format_version,
        "length": length,
        "sequence": sequence,
        "state": state,
        "supported_format": supported_format,
        "approved_length": approved_length,
        "crc_matches": crc_matches,
        "commit_marker_state": commit_marker_state,
        "active_slot": active_slot,
        "confirmed_slot": confirmed_slot,
        "pending_slot": pending_slot,
        "bad_slot": bad_slot,
        "trial_count": trial_count,
        "version": version,
        "hash": hash,
        "rollback_reason": rollback_reason,
        "valid_firmware_slots": valid_firmware_slots,
        "failed_candidate_slots": failed_candidate_slots,
    }


def boot_metadata_record_envelope_is_valid(
    boot_metadata,
    record,
    trial_boot_max=None,
):
    envelope = boot_metadata.get("logical_validation_envelope")
    if envelope is None or type(record) is not dict:
        return False
    record_contract = envelope.get("record")
    if type(record_contract) is not dict:
        return False
    required_fields = record_contract.get("required_fields")
    if type(required_fields) is not list or set(record) != set(required_fields):
        return False

    integer_fields = ["format_version", "length", "sequence", "trial_count"]
    for field in integer_fields:
        if type(record[field]) is not int:
            return False

    for field in ["supported_format", "approved_length", "crc_matches"]:
        if type(record[field]) is not bool:
            return False

    string_fields = [
        "copy_id",
        "state",
        "commit_marker_state",
        "active_slot",
        "confirmed_slot",
        "pending_slot",
        "bad_slot",
        "version",
        "hash",
        "rollback_reason",
    ]
    if any(type(record[field]) is not str for field in string_fields):
        return False

    state_schema = record_contract["field_schemas"]["state"]
    if record["state"] not in state_schema["allowed_values"]:
        return False
    if (
        record["commit_marker_state"]
        not in boot_metadata["commit_marker_semantics"]["state_meanings"]
    ):
        return False

    invariants = boot_metadata["slot_state_invariants"]
    logical_slots = tuple(invariants["logical_firmware_slots"])
    if record["active_slot"] not in logical_slots:
        return False
    if record["confirmed_slot"] not in logical_slots:
        return False
    if record["pending_slot"] not in invariants["pending_slot"]["allowed_values"]:
        return False
    if record["bad_slot"] not in invariants["bad_slot"]["allowed_values"]:
        return False
    reason_domain = boot_metadata["candidate_trial_transitions"][
        "individual_committed_record_state"
    ]["rollback_reason_domain"]
    if record["rollback_reason"] not in reason_domain:
        return False

    allowed_collection_types = {list, tuple, set, frozenset}
    for field in ["valid_firmware_slots", "failed_candidate_slots"]:
        collection = record[field]
        if type(collection) not in allowed_collection_types:
            return False
        if any(type(slot) is not str for slot in collection):
            return False
        if any(slot not in logical_slots for slot in collection):
            return False
    return True


def boot_metadata_snapshot_is_valid(
    boot_metadata,
    copies,
    trial_boot_max,
):
    if type(copies) not in {list, tuple}:
        return False
    copy_count = boot_metadata.get("copy_count")
    if type(copy_count) is not int or copy_count < 0 or len(copies) > copy_count:
        return False
    return all(
        type(record) is dict
        and "copy_id" in record
        and type(record["copy_id"]) is str
        for record in copies
    )


def normalize_recomputed_evidence(
    boot_metadata,
    verified_candidate_identities,
    revalidated_bootable_slots,
):
    input_schema = boot_metadata.get("boot_decision", {}).get(
        "external_revalidation", {}
    ).get("input_schema")
    if type(input_schema) is not dict:
        return False, frozenset(), frozenset()
    allowed_container_types = {list, tuple, set, frozenset}
    logical_slots = tuple(
        boot_metadata["slot_state_invariants"]["logical_firmware_slots"]
    )

    def normalize_pending(evidence):
        if evidence is None:
            return True, frozenset()
        if type(evidence) not in allowed_container_types:
            return False, frozenset()
        normalized = []
        for element in evidence:
            if type(element) is not tuple or len(element) != 3:
                return False, frozenset()
            pending_slot, version, artifact_hash = element
            if type(pending_slot) is not str or pending_slot not in logical_slots:
                return False, frozenset()
            if type(version) is not str or not version or not version.strip():
                return False, frozenset()
            if (
                type(artifact_hash) is not str
                or re.fullmatch(r"[0-9a-f]{64}", artifact_hash) is None
            ):
                return False, frozenset()
            normalized.append(element)
        return True, frozenset(normalized)

    def normalize_confirmed(evidence):
        if evidence is None:
            return True, frozenset()
        if type(evidence) not in allowed_container_types:
            return False, frozenset()
        normalized = []
        for element in evidence:
            if type(element) is not str or element not in logical_slots:
                return False, frozenset()
            normalized.append(element)
        return True, frozenset(normalized)

    pending_valid, pending = normalize_pending(verified_candidate_identities)
    confirmed_valid, confirmed = normalize_confirmed(
        revalidated_bootable_slots
    )
    if not pending_valid or not confirmed_valid:
        return False, frozenset(), frozenset()
    return True, pending, confirmed


def boot_metadata_semantic_projection(
    boot_metadata,
    record,
    trial_boot_max=None,
):
    if not boot_metadata_record_envelope_is_valid(
        boot_metadata,
        record,
        trial_boot_max,
    ):
        return None
    integer_fields = {
        "format_version": 1,
        "length": 1,
        "sequence": 0,
        "trial_count": 0,
    }
    string_fields = {
        "active_slot",
        "pending_slot",
        "confirmed_slot",
        "bad_slot",
        "version",
        "hash",
        "rollback_reason",
    }
    projection = []
    for field in boot_metadata["crc"]["semantic_coverage_fields"]:
        if field not in record:
            return None
        value = record[field]
        if field in integer_fields:
            if type(value) is not int or value < integer_fields[field]:
                return None
        elif field in string_fields:
            if type(value) is not str:
                return None
        else:
            return None
        projection.append((field, value))
    return tuple(projection)


class BootMetadataRecordReference(NamedTuple):
    copy_id: str
    sequence: int
    canonical_semantic_projection: tuple


def commit_marker_is_valid(boot_metadata, record):
    if not boot_metadata_record_envelope_is_valid(boot_metadata, record):
        return False
    semantics = boot_metadata.get("commit_marker_semantics")
    if semantics is None:
        return False
    marker_state = record["commit_marker_state"]
    return (
        marker_state == semantics["valid_committed_state"]
        and semantics["state_meanings"].get(marker_state) == "committed"
        and marker_state not in semantics["invalid_states"]
    )


def slot_state_invariants_are_valid(
    boot_metadata,
    record,
    trial_boot_max,
):
    if not boot_metadata_record_envelope_is_valid(
        boot_metadata,
        record,
        trial_boot_max,
    ):
        return False
    invariants = boot_metadata.get("slot_state_invariants")
    if invariants is None:
        return False

    logical_slots = set(invariants["logical_firmware_slots"])
    none_value = invariants["none_value"]
    valid_firmware_slots = set(record["valid_firmware_slots"])
    failed_candidate_slots = set(record["failed_candidate_slots"])
    active_slot = record["active_slot"]
    confirmed_slot = record["confirmed_slot"]
    pending_slot = record["pending_slot"]
    bad_slot = record["bad_slot"]
    trial_count = record.get("trial_count")

    if type(trial_boot_max) is not int or trial_boot_max < 0:
        return False
    if (
        type(trial_count) is not int
        or trial_count < 0
        or trial_count > trial_boot_max
    ):
        return False

    if not valid_firmware_slots.issubset(logical_slots):
        return False
    if not failed_candidate_slots.issubset(logical_slots):
        return False
    if active_slot not in logical_slots or active_slot not in valid_firmware_slots:
        return False
    if (
        confirmed_slot not in logical_slots
        or confirmed_slot not in valid_firmware_slots
        or confirmed_slot == bad_slot
    ):
        return False
    if pending_slot != none_value and (
        pending_slot not in logical_slots
        or pending_slot not in valid_firmware_slots
        or pending_slot == active_slot
        or pending_slot == confirmed_slot
    ):
        return False
    if bad_slot != none_value and (
        bad_slot not in logical_slots
        or bad_slot not in failed_candidate_slots
        or bad_slot == active_slot
        or bad_slot == confirmed_slot
        or (pending_slot != none_value and bad_slot == pending_slot)
    ):
        return False
    return True


def candidate_identity_shape_is_valid(boot_metadata, record):
    if not boot_metadata_record_envelope_is_valid(boot_metadata, record):
        return False
    transitions = boot_metadata.get("candidate_trial_transitions")
    if transitions is None:
        return False
    identity_contract = transitions.get("individual_committed_record_identity")
    if identity_contract is None:
        return False

    version = record.get("version")
    artifact_hash = record.get("hash")
    if not isinstance(version, str) or not version or not version.strip():
        return False
    if (
        not isinstance(artifact_hash, str)
        or re.fullmatch(r"[0-9a-f]{64}", artifact_hash) is None
    ):
        return False

    none_value = boot_metadata["slot_state_invariants"]["none_value"]
    pending_slot = record.get("pending_slot")
    bad_slot = record.get("bad_slot")
    if pending_slot != none_value and bad_slot != none_value:
        return False
    if pending_slot == none_value and bad_slot == none_value:
        return record.get("active_slot") == record.get("confirmed_slot")
    return True


def rollback_reason_state_is_valid(boot_metadata, record, trial_boot_max):
    if not boot_metadata_record_envelope_is_valid(
        boot_metadata,
        record,
        trial_boot_max,
    ):
        return False
    transitions = boot_metadata.get("candidate_trial_transitions")
    if transitions is None:
        return False
    state_contract = transitions.get("individual_committed_record_state")
    if state_contract is None:
        return False
    reason = record.get("rollback_reason")
    pending_slot = record.get("pending_slot")
    bad_slot = record.get("bad_slot")
    trial_count = record.get("trial_count")
    none_value = boot_metadata["slot_state_invariants"]["none_value"]
    if reason not in state_contract["rollback_reason_domain"]:
        return False
    if pending_slot != none_value:
        return (
            bad_slot == none_value
            and reason == "none"
            and type(trial_count) is int
            and 0 <= trial_count <= trial_boot_max
        )
    if bad_slot == none_value:
        return (
            record.get("active_slot") == record.get("confirmed_slot")
            and trial_count == 0
            and reason == "none"
        )
    return (
        trial_count == 0
        and reason
        in {
            "explicit_trial_failure",
            "trial_exhausted",
            "pending_revalidation_failed",
        }
    )


def _transition_record_is_valid(boot_metadata, record, trial_boot_max):
    return (
        boot_metadata_record_envelope_is_valid(
            boot_metadata,
            record,
            trial_boot_max,
        )
        and record.get("state") == "committed"
        and record.get("supported_format") is True
        and record.get("approved_length") is True
        and record.get("crc_matches") is True
        and boot_metadata_semantic_projection(boot_metadata, record) is not None
        and commit_marker_is_valid(boot_metadata, record)
        and slot_state_invariants_are_valid(
            boot_metadata,
            record,
            trial_boot_max,
        )
        and candidate_identity_shape_is_valid(boot_metadata, record)
        and rollback_reason_state_is_valid(
            boot_metadata,
            record,
            trial_boot_max,
        )
    )


def _transition_kind_matches(
    boot_metadata,
    source,
    destination,
    transition_kind,
    trial_boot_max,
    *,
    verified_candidate_identities,
    revalidated_bootable_slots,
    journal_commit_authorization,
):
    transitions = boot_metadata["candidate_trial_transitions"]
    none_value = boot_metadata["slot_state_invariants"]["none_value"]
    candidate_fields = transitions["candidate_identity_fields"]
    bad_fields = transitions["bad_identity_fields"]

    def fields_unchanged(*fields):
        return all(source[field] == destination[field] for field in fields)

    def identity(record, fields):
        return tuple(record[field] for field in fields)

    source_candidate = identity(source, candidate_fields)
    destination_candidate = identity(destination, candidate_fields)
    source_bad = identity(source, bad_fields)

    if transition_kind == "stage_verified_candidate":
        if (
            journal_commit_authorization
            and destination_candidate not in verified_candidate_identities
        ):
            return False
        if (
            destination["pending_slot"] == none_value
            or destination["bad_slot"] != none_value
            or destination["trial_count"] != 0
            or destination["rollback_reason"] != "none"
            or not fields_unchanged("active_slot", "confirmed_slot")
            or destination_candidate == source_candidate
        ):
            return False
        if source["bad_slot"] != none_value:
            return (
                destination["pending_slot"] == source["bad_slot"]
                and destination_candidate != source_bad
            )
        return True

    if transition_kind == "restage_same_candidate":
        if (
            journal_commit_authorization
            and destination_candidate not in verified_candidate_identities
        ):
            return False
        return (
            source["pending_slot"] != none_value
            and destination_candidate == source_candidate
            and fields_unchanged(
                "format_version",
                "length",
                "active_slot",
                "confirmed_slot",
                "pending_slot",
                "bad_slot",
                "trial_count",
                "version",
                "hash",
                "rollback_reason",
            )
        )

    if transition_kind == "attempt":
        return (
            source["pending_slot"] != none_value
            and destination_candidate == source_candidate
            and source["trial_count"] in {0, 1}
            and destination["trial_count"] == source["trial_count"] + 1
            and destination["trial_count"] <= trial_boot_max
            and fields_unchanged(
                "format_version",
                "length",
                "active_slot",
                "confirmed_slot",
                "pending_slot",
                "bad_slot",
                "version",
                "hash",
                "rollback_reason",
            )
        )

    if transition_kind == "confirm":
        return (
            source["pending_slot"] != none_value
            and 0 < source["trial_count"] <= trial_boot_max
            and destination["active_slot"] == source["pending_slot"]
            and destination["confirmed_slot"] == source["pending_slot"]
            and destination["pending_slot"] == none_value
            and destination["bad_slot"] == none_value
            and destination["trial_count"] == 0
            and destination["version"] == source["version"]
            and destination["hash"] == source["hash"]
            and destination["rollback_reason"] == "none"
        )

    if transition_kind == "fail_or_exhaust":
        if source["pending_slot"] == none_value:
            return False
        common_terminal = (
            0 < source["trial_count"] <= trial_boot_max
            and destination["active_slot"] == source["confirmed_slot"]
            and destination["confirmed_slot"] == source["confirmed_slot"]
            and destination["pending_slot"] == none_value
            and destination["bad_slot"] == source["pending_slot"]
            and destination["trial_count"] == 0
            and destination["version"] == source["version"]
            and destination["hash"] == source["hash"]
        )
        if not common_terminal:
            return False
        if destination["rollback_reason"] == "explicit_trial_failure":
            return True
        return (
            destination["rollback_reason"] == "trial_exhausted"
            and source["trial_count"] == trial_boot_max
        )

    if transition_kind == "pending_revalidation_failed":
        if (
            journal_commit_authorization
            and (
                source["confirmed_slot"] not in revalidated_bootable_slots
                or source_candidate in verified_candidate_identities
            )
        ):
            return False
        return (
            source["pending_slot"] != none_value
            and 0 <= source["trial_count"] <= trial_boot_max
            and destination["active_slot"] == source["confirmed_slot"]
            and destination["confirmed_slot"] == source["confirmed_slot"]
            and destination["pending_slot"] == none_value
            and destination["bad_slot"] == source["pending_slot"]
            and destination["trial_count"] == 0
            and destination["version"] == source["version"]
            and destination["hash"] == source["hash"]
            and destination["rollback_reason"]
            == "pending_revalidation_failed"
        )

    return False


def classify_boot_metadata_transition(
    boot_metadata,
    source,
    destination,
    trial_boot_max,
    *,
    verified_candidate_identities=None,
    revalidated_bootable_slots=None,
    journal_commit_authorization=True,
):
    transitions = boot_metadata.get("candidate_trial_transitions")
    if transitions is None:
        return "invalid"
    classifier = transitions.get("transition_classifier")
    if classifier is None:
        return "invalid"
    if journal_commit_authorization:
        evidence_valid, verified_candidate_identities, (
            revalidated_bootable_slots
        ) = normalize_recomputed_evidence(
            boot_metadata,
            verified_candidate_identities,
            revalidated_bootable_slots,
        )
        if not evidence_valid:
            return "invalid"
    else:
        verified_candidate_identities = frozenset()
        revalidated_bootable_slots = frozenset()
    copy_topology = boot_metadata.get("copy_topology")
    if copy_topology is None:
        return "invalid"
    logical_copy_ids = tuple(copy_topology.get("logical_copy_ids", []))
    if (
        not _transition_record_is_valid(
            boot_metadata, source, trial_boot_max
        )
        or not _transition_record_is_valid(
            boot_metadata, destination, trial_boot_max
        )
        or type(source.get("sequence")) is not int
        or type(destination.get("sequence")) is not int
        or destination["sequence"] != source["sequence"] + 1
        or source.get("copy_id") not in logical_copy_ids
        or destination.get("copy_id") not in logical_copy_ids
        or source.get("copy_id") == destination.get("copy_id")
        or source.get("format_version") != destination.get("format_version")
        or source.get("length") != destination.get("length")
    ):
        return "invalid"

    matches = [
        kind
        for kind in classifier.get("journal_transition_kinds", [])
        if _transition_kind_matches(
            boot_metadata,
            source,
            destination,
            kind,
            trial_boot_max,
            verified_candidate_identities=verified_candidate_identities,
            revalidated_bootable_slots=revalidated_bootable_slots,
            journal_commit_authorization=journal_commit_authorization,
        )
    ]
    if not matches:
        return classifier.get("zero_matches", "invalid")
    if len(matches) != 1:
        return classifier.get("multiple_matches", "ambiguous")
    return matches[0]


def boot_metadata_transition_is_valid(
    boot_metadata,
    source,
    destination,
    transition_kind,
    trial_boot_max,
    *,
    verified_candidate_identities=None,
    revalidated_bootable_slots=None,
):
    if transition_kind == "reboot":
        return False
    return classify_boot_metadata_transition(
        boot_metadata,
        source,
        destination,
        trial_boot_max,
        verified_candidate_identities=verified_candidate_identities,
        revalidated_bootable_slots=revalidated_bootable_slots,
    ) == transition_kind


def _select_boot_metadata_record(
    boot_metadata,
    copies,
    trial_boot_max,
):
    service_recovery = boot_metadata["selection"]["zero_valid_copies"]
    if not boot_metadata_snapshot_is_valid(
        boot_metadata,
        copies,
        trial_boot_max,
    ):
        return service_recovery, None, None
    copy_topology = boot_metadata.get("copy_topology")
    if copy_topology is None:
        return service_recovery, None, None
    logical_copy_ids = tuple(copy_topology.get("logical_copy_ids", []))
    copy_ids = [record.get("copy_id") for record in copies]
    if (
        logical_copy_ids != ("copy_a", "copy_b")
        or any(copy_id not in logical_copy_ids for copy_id in copy_ids)
        or len(copy_ids) != len(set(copy_ids))
    ):
        return service_recovery, None, None

    checkers = {
        "supported_format": lambda record: (
            record.get("supported_format") is True
        ),
        "approved_length_for_format": lambda record: (
            record.get("approved_length") is True
        ),
        "matching_crc": lambda record: record.get("crc_matches") is True,
        "committed_marker": lambda record: commit_marker_is_valid(
            boot_metadata, record
        ),
        "slot_state_invariants": lambda record: slot_state_invariants_are_valid(
            boot_metadata,
            record,
            trial_boot_max,
        ),
        "candidate_identity_shape": lambda record: (
            candidate_identity_shape_is_valid(boot_metadata, record)
        ),
        "rollback_reason_state_relationships": lambda record: (
            rollback_reason_state_is_valid(
                boot_metadata,
                record,
                trial_boot_max,
            )
        ),
        "canonical_semantic_projection": lambda record: (
            boot_metadata_semantic_projection(boot_metadata, record)
            is not None
        ),
    }
    required_checks = boot_metadata["committed_record_validity"][
        "required_checks"
    ]
    transition_check = "candidate_trial_transition"
    individual_checks = [
        check for check in required_checks if check != transition_check
    ]
    if any(check not in checkers for check in individual_checks):
        return service_recovery, None, None
    ignored_states = set(boot_metadata["selection"]["ignored_states"])
    candidates = [
        record
        for record in copies
        if boot_metadata_record_envelope_is_valid(
            boot_metadata,
            record,
            trial_boot_max,
        )
        and record.get("state") not in ignored_states
        and record.get("state") == "committed"
        and type(record.get("sequence")) is int
        and record["sequence"] >= 0
        and boot_metadata_semantic_projection(boot_metadata, record) is not None
        and all(checkers[check](record) for check in individual_checks)
    ]

    if transition_check in required_checks:
        accepted = []
        for record in sorted(
            candidates,
            key=lambda candidate: (
                candidate["sequence"],
                candidate["copy_id"],
            ),
        ):
            predecessors = [
                candidate
                for candidate in accepted
                if candidate["sequence"] < record["sequence"]
            ]
            if not predecessors:
                accepted.append(record)
                continue
            predecessor_sequence = max(
                candidate["sequence"] for candidate in predecessors
            )
            predecessor_group = [
                candidate
                for candidate in predecessors
                if candidate["sequence"] == predecessor_sequence
            ]
            predecessor_projections = {
                boot_metadata_semantic_projection(boot_metadata, candidate)
                for candidate in predecessor_group
            }
            if len(predecessor_projections) != 1:
                continue
            predecessor = min(
                predecessor_group,
                key=lambda candidate: candidate["copy_id"],
            )
            if classify_boot_metadata_transition(
                boot_metadata,
                predecessor,
                record,
                trial_boot_max,
                journal_commit_authorization=False,
            ) not in {"invalid", "ambiguous"}:
                accepted.append(record)
        candidates = accepted

    if not candidates:
        return service_recovery, None, None

    highest_sequence = max(record["sequence"] for record in candidates)
    newest = [
        record for record in candidates if record["sequence"] == highest_sequence
    ]
    projections = {
        boot_metadata_semantic_projection(boot_metadata, record)
        for record in newest
    }
    if len(projections) != 1:
        return (
            boot_metadata["selection"]["equal_sequence_different_content"],
            None,
            None,
        )
    winner = min(newest, key=lambda record: record["copy_id"])
    reference = BootMetadataRecordReference(
        copy_id=winner["copy_id"],
        sequence=winner["sequence"],
        canonical_semantic_projection=(
            boot_metadata_semantic_projection(boot_metadata, winner)
        ),
    )
    return "selected", reference, winner


def select_boot_metadata_copy(
    boot_metadata,
    copies,
    trial_boot_max,
):
    status, reference, _ = _select_boot_metadata_record(
        boot_metadata,
        copies,
        trial_boot_max,
    )
    return status, reference


def decide_boot_metadata(
    boot_metadata,
    copies,
    trial_boot_max,
    *,
    verified_candidate_identities=None,
    revalidated_bootable_slots=None,
):
    evidence_valid, verified_candidate_identities, revalidated_bootable_slots = (
        normalize_recomputed_evidence(
            boot_metadata,
            verified_candidate_identities,
            revalidated_bootable_slots,
        )
    )
    if not evidence_valid:
        return {
            "disposition": "service_recovery_without_image_write",
            "source_record_reference": None,
            "next_journal_record": None,
            "post_commit_action": None,
        }

    selection_result, source_reference, selected = (
        _select_boot_metadata_record(
            boot_metadata,
            copies,
            trial_boot_max,
        )
    )
    decision = {
        "disposition": selection_result,
        "source_record_reference": source_reference,
        "next_journal_record": None,
        "post_commit_action": None,
    }
    if selection_result != "selected":
        decision["source_record_reference"] = None
        return decision

    confirmed_slot = selected["confirmed_slot"]
    if confirmed_slot not in revalidated_bootable_slots:
        decision["disposition"] = "service_recovery_without_image_write"
        decision["source_record_reference"] = None
        return decision

    none_value = boot_metadata["slot_state_invariants"]["none_value"]
    if selected["pending_slot"] == none_value:
        decision["disposition"] = "boot_confirmed"
        return decision

    pending_identity_fields = boot_metadata["boot_decision"][
        "external_revalidation"
    ]["pending_identity_fields"]
    pending_identity = tuple(
        selected[field] for field in pending_identity_fields
    )
    logical_copy_ids = tuple(
        boot_metadata["copy_topology"]["logical_copy_ids"]
    )
    target_copy_id = next(
        copy_id
        for copy_id in logical_copy_ids
        if copy_id != selected["copy_id"]
    )

    next_record = copy.deepcopy(selected)
    next_record.update(
        {
            "copy_id": target_copy_id,
            "sequence": selected["sequence"] + 1,
            "state": "committed",
            "commit_marker_state": "programmed",
        }
    )
    if pending_identity in verified_candidate_identities:
        if selected["trial_count"] < trial_boot_max:
            next_record["trial_count"] = selected["trial_count"] + 1
            decision["disposition"] = (
                "attempt_required_commit_then_boot_pending"
            )
            decision["post_commit_action"] = "boot_pending"
        else:
            next_record.update(
                {
                    "active_slot": confirmed_slot,
                    "confirmed_slot": confirmed_slot,
                    "pending_slot": none_value,
                    "bad_slot": selected["pending_slot"],
                    "trial_count": 0,
                    "rollback_reason": "trial_exhausted",
                    "failed_candidate_slots": sorted(
                        set(selected["failed_candidate_slots"])
                        | {selected["pending_slot"]}
                    ),
                }
            )
            decision["disposition"] = (
                "rollback_required_trial_exhausted_commit_then_"
                "boot_confirmed"
            )
            decision["post_commit_action"] = "boot_confirmed"
    else:
        next_record.update(
            {
                "active_slot": confirmed_slot,
                "confirmed_slot": confirmed_slot,
                "pending_slot": none_value,
                "bad_slot": selected["pending_slot"],
                "trial_count": 0,
                "rollback_reason": "pending_revalidation_failed",
                "failed_candidate_slots": sorted(
                    set(selected["failed_candidate_slots"])
                    | {selected["pending_slot"]}
                ),
            }
        )
        decision["disposition"] = (
            "rollback_required_pending_revalidation_failed_commit_then_"
            "boot_confirmed"
        )
        decision["post_commit_action"] = "boot_confirmed"

    decision["next_journal_record"] = next_record
    return decision



class FirmwareRuntimeContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = json.loads(CONTRACT.read_text(encoding="utf-8"))

    def assert_selected_copy(self, result, expected_copy_id):
        status, reference = result
        self.assertEqual(status, "selected")
        self.assertIsInstance(reference, BootMetadataRecordReference)
        self.assertEqual(reference.copy_id, expected_copy_id)
        self.assertIsInstance(reference.canonical_semantic_projection, tuple)

    def test_exact_top_level_shape_and_identity(self):
        self.assertEqual(
            list(self.data),
            [
                "contract_id",
                "schema_version",
                "hardware_revision",
                "hardware_qualification",
                "ownership",
                "lifecycle",
                "timing_ms",
                "sensor",
                "snapshots",
                "power",
                "event_types",
                "flash_fota",
            ],
        )
        self.assertEqual(self.data["contract_id"], "nb_iot.g1.firmware_runtime")
        self.assertEqual(self.data["schema_version"], 1)
        self.assertEqual(
            self.data["hardware_revision"], "NB-IOT-HW-R1-20260702-ASBUILT"
        )

    def test_hardware_identifier_is_not_production_qualification(self):
        qualification = self.data["hardware_qualification"]
        self.assertEqual(
            qualification["identity_scope"], "official_identifier_only"
        )
        self.assertFalse(qualification["production_qualified"])
        self.assertEqual(
            qualification["qualification_status"],
            "pending_as_built_hardware_evidence",
        )
        self.assertEqual(
            qualification["release_gate_refs"],
            ["sensor.hardware_release_gates", "power.hardware_release_gates"],
        )
        for gate_ref in qualification["release_gate_refs"]:
            self.assertTrue(resolve_contract_path(self.data, gate_ref))

    def test_single_ownership_contract(self):
        ownership = self.data["ownership"]
        self.assertEqual(ownership["uart0_modem"], "v2_modem_command_service")
        self.assertEqual(ownership["pdp_mqtt"], "v2_communication_lifecycle")
        self.assertEqual(ownership["one_wire_i2s"], "sensor_coordinator")
        self.assertEqual(ownership["flash"], "central_flash_service")
        self.assertEqual(ownership["snapshot"], "snapshot_service")
        self.assertEqual(ownership["shutdown"], "power_state_service")
        self.assertEqual(ownership["config_command"], "request_coordinator")
        self.assertTrue(ownership["single_writer_required"])
        self.assertTrue(ownership["resource_map_release_evidence_required"])
        self.assertTrue(ownership["smp_affinity_priority_release_evidence_required"])
        self.assertTrue(ownership["high_water_release_evidence_required"])
        self.assertTrue(ownership["lock_order_release_evidence_required"])

    def test_continuous_transport_and_post_config_liveness(self):
        lifecycle = self.data["lifecycle"]
        self.assertFalse(lifecycle["boot_runtime_transport_teardown"])
        self.assertTrue(lifecycle["periodic_ready_ends_boot_orchestration_only"])
        self.assertTrue(lifecycle["runtime_reuses_boot_transport"])
        self.assertFalse(lifecycle["legacy_hybrid_allowed"])
        self.assertEqual(
            lifecycle["transport_teardown_allowed_reasons"],
            ["https_exclusive_mode", "actual_shutdown"],
        )
        self.assertEqual(
            lifecycle["post_config_liveness_gate"],
            [
                "at_ok",
                "same_session_puback",
                "subscription_alive",
                "followup_config_received",
            ],
        )
        self.assertFalse(lifecycle["failed_liveness_may_report_periodic_ready"])

    def test_boot_snapshot_freeze_point(self):
        self.assertEqual(
            self.data["lifecycle"]["boot_snapshot_freeze_point"],
            "after_post_config_liveness_before_periodic_ready",
        )

    def test_modem_recovery_and_io_invariants(self):
        lifecycle = self.data["lifecycle"]
        self.assertEqual(lifecycle["at_command_terminator"], "\r")
        self.assertEqual(lifecycle["uart_response_guard_bytes"], 256)
        self.assertEqual(lifecycle["debug_stdio"], "usb_only")
        self.assertFalse(lifecycle["uart0_stdio_enabled"])
        self.assertEqual(
            lifecycle["recovery_order"],
            [
                "command_retry",
                "pdp_recovery",
                "cfun_recovery",
                "gp3_modem_reset",
                "pico_reboot",
            ],
        )
        self.assertEqual(lifecycle["automatic_pico_reboot_allowed"], [0, 1, 2])
        self.assertEqual(lifecycle["automatic_pico_reboot_hard_cap"], 2)
        self.assertTrue(lifecycle["cfun_requires_bounded_sim_attach_recovery"])
        self.assertTrue(lifecycle["modem_communication_busy_guard_required"])
        self.assertEqual(
            lifecycle["mqtt_keepalive_status"],
            "not_frozen_until_24h_disconnect_reconnect_test",
        )

    def test_runtime_release_evidence_policies(self):
        lifecycle = self.data["lifecycle"]
        self.assertEqual(
            lifecycle["firmware_artifact_roles"],
            ["independent_boot_v2_baseline", "boot_runtime_v2_production"],
        )
        self.assertEqual(
            lifecycle["logging_policy"],
            {
                "bounded": True,
                "nonblocking": True,
                "critical_safety_failure_protected": True,
                "debug_repetitive_drop_counted": True,
                "depth_rate_status": "implementation_defined",
            },
        )
        self.assertEqual(
            lifecycle["queue_policy"],
            {
                "critical_noncritical_loss_policies_required": True,
                "receiver_ownership_required": True,
                "critical_event_loss_allowed": False,
                "depth_status": "implementation_defined",
            },
        )
        self.assertEqual(
            lifecycle["release_fault_injection_targets"],
            ["command", "queue", "sensor", "network", "flash", "power"],
        )
        self.assertTrue(lifecycle["acceptance_timing_ledger_required"])
        self.assertEqual(lifecycle["unknown_timing_status"], "measure_before_freeze")
        self.assertEqual(
            lifecycle["lcd_startup_policy"],
            {
                "target_ms": 3000,
                "rollback_ms": 5000,
                "qualification_cycles_per_condition": 10,
                "conditions": ["adapter", "battery", "reboot"],
            },
        )

    def test_https_exclusive_transport_contract(self):
        lifecycle = self.data["lifecycle"]
        self.assertEqual(lifecycle["https_entry_requirement"], "mqtt_normal_shutdown")
        self.assertEqual(
            lifecycle["https_exit_requirements"],
            [
                "mqtt_reconnect",
                "resubscribe",
                "config_pull",
                "command_pull",
                "queued_telemetry_replay",
            ],
        )
        self.assertEqual(
            lifecycle["https_allowed_transfers"],
            ["audio_upload", "firmware_artifact_download"],
        )
        self.assertTrue(lifecycle["https_preserves_queued_telemetry"])
        self.assertTrue(lifecycle["https_requires_hardware_qualification"])

    def test_timing_contract(self):
        self.assertEqual(
            self.data["timing_ms"],
            {
                "boot_sensor_search_max": 15000,
                "degraded_sensor_retry": 10000,
                "temperature_period": 30000,
                "telemetry_period": 1200000,
                "rssi_period": 300000,
                "failure_retry": 60000,
                "adapter_debounce": 1000,
                "shutdown_commit": 210000,
                "shutdown_cleanup_window_after_commit": 90000,
                "shutdown_absolute_off": 300000,
                "fota_confirm_deadline": 600000,
            },
        )
        self.assertEqual(
            self.data["timing_ms"]["fota_confirm_deadline"],
            self.data["flash_fota"]["confirm_deadline_ms"],
        )

    def test_product_temperature_sample_interval_matches_contract(self):
        config_source = (ROOT / "src" / "config.h").read_text()
        sample_interval = re.search(
            r"^#define\s+DS18B20_SAMPLE_INTERVAL_MS\s+(\d+)\s*$",
            config_source,
            re.MULTILINE,
        )
        self.assertIsNotNone(sample_interval)
        self.assertEqual(
            int(sample_interval.group(1)),
            self.data["timing_ms"]["temperature_period"],
        )

    def test_shutdown_timing_origin_and_relation(self):
        timing = self.data["timing_ms"]
        self.assertEqual(
            self.data["power"]["shutdown_timing_semantics"],
            {
                "elapsed_origin": "battery_grace_entry_after_adapter_debounce",
                "cleanup_starts_at": "shutdown_commit",
                "cleanup_window": "shutdown_cleanup_window_after_commit",
                "absolute_off": "shutdown_absolute_off",
                "required_relation": (
                    "cleanup_starts_at_plus_cleanup_window_equals_absolute_off"
                ),
            },
        )
        self.assertEqual(
            timing["shutdown_commit"]
            + timing["shutdown_cleanup_window_after_commit"],
            timing["shutdown_absolute_off"],
        )

    def test_sensor_pair_health_and_stale_rules(self):
        sensor = self.data["sensor"]
        self.assertEqual(sensor["pass_rule"], "at_least_one_complete_temp_mic_pair")
        self.assertEqual(sensor["otherwise_rule"], "degraded")
        self.assertTrue(sensor["unused_empty_other_port_may_still_pass"])
        self.assertEqual(
            sensor["health_codes"],
            {"unknown": 0, "pass": 1, "degraded": 2, "failed": 3},
        )
        self.assertEqual(
            sensor["value_source_codes"],
            {"none": 0, "fresh": 1, "crc_fallback": 2},
        )
        self.assertEqual(
            sensor["quality_fields"],
            [
                "health",
                "has_value",
                "value",
                "value_source",
                "stale",
                "consecutive_failures",
                "last_valid_at",
            ],
        )
        self.assertEqual(sensor["crc_fallback_max_consecutive"], 3)
        self.assertEqual(sensor["crc_fallback_max_age_ms"], 30000)
        self.assertEqual(sensor["crc_fallback_use"], "lcd_display_only")
        self.assertEqual(sensor["invalid_from_consecutive_failure"], 4)
        self.assertEqual(sensor["invalid_when_age_exceeds_ms"], 30000)
        self.assertFalse(sensor["stale_may_raise_or_clear_alarm"])
        self.assertFalse(sensor["stale_may_publish_telemetry"])
        self.assertFalse(sensor["invalid_numeric_sentinel_allowed"])
        self.assertEqual(sensor["audio_discard_total_ms"], 5000)
        self.assertTrue(sensor["audio_discard_requires_pre_and_post_segments"])
        self.assertEqual(sensor["audio_discard_segment_allocation_status"], "not_frozen")
        self.assertNotIn("audio_discard_before_ms", sensor)
        self.assertNotIn("audio_discard_after_ms", sensor)
        self.assertTrue(sensor["one_wire_i2s_time_slicing_required"])
        self.assertEqual(
            sensor["production_calibration_status"],
            "not_frozen_until_multipoint_calibration",
        )
        self.assertEqual(
            sensor["audio_sample_rate_status"],
            "not_frozen_until_3m_cable_qualification",
        )
        self.assertEqual(sensor["audio_sample_rate_candidates_hz"], [8000, 16000, 24000])
        self.assertEqual(
            sensor["hardware_release_gates"],
            [
                "port1_port2_i2s_on_off_temperature_crc",
                "three_meter_cable_sample_rate_comparison",
                "temp1_temp2_multipoint_calibration",
            ],
        )

    def test_snapshot_contracts_and_forbidden_values(self):
        snapshots = self.data["snapshots"]
        self.assertEqual(snapshots["boot"]["schema_version"], 1)
        self.assertTrue(snapshots["boot"]["immutable_one_shot"])
        self.assertEqual(
            snapshots["boot"]["required_fields"],
            [
                "health",
                "last_completed_stage",
                "hardware_revision",
                "firmware_build_id",
                "config_version",
                "config_valid",
                "sensors",
                "modem_ready",
                "pdp_session_id",
                "mqtt_session_id",
                "subscription_alive",
                "post_config_liveness",
                "reboot_guard",
                "recovery_summary",
            ],
        )
        self.assertEqual(snapshots["runtime"]["schema_version"], 1)
        self.assertTrue(snapshots["runtime"]["atomic_latest_replacement"])
        self.assertTrue(snapshots["runtime"]["revision_monotonic"])
        self.assertEqual(
            snapshots["runtime"]["required_fields"],
            [
                "revision",
                "sensors",
                "network_state",
                "adapter_state",
                "power_state",
                "alarm_state",
                "ui_state",
                "config_version",
                "last_command_id",
                "last_command_result",
                "queue_summary",
                "drop_summary",
                "recovery_summary",
            ],
        )
        self.assertEqual(
            snapshots["forbidden_member_kinds"],
            [
                "pointer",
                "task_handle",
                "queue_handle",
                "mutex_handle",
                "secret",
                "mutable_modem_object",
            ],
        )
        self.assertEqual(snapshots["device_boot_snapshot_count"], 1)
        self.assertEqual(snapshots["device_runtime_snapshot_count"], 1)
        self.assertEqual(snapshots["history_owner"], "server_database")
        self.assertFalse(snapshots["consumer_write_allowed"])
        self.assertEqual(snapshots["sensor_fields_ref"], "sensor.quality_fields")
        self.assertEqual(
            resolve_contract_path(self.data, snapshots["sensor_fields_ref"]),
            self.data["sensor"]["quality_fields"],
        )

    def test_power_state_incident_and_deadline_contract(self):
        power = self.data["power"]
        self.assertEqual(
            power["states"],
            {"external_power": 1, "grace": 2, "committed": 3, "cleanup": 4, "off": 5},
        )
        self.assertTrue(power["restore_before_commit_cancels"])
        self.assertTrue(power["restore_after_commit_is_ignored"])
        self.assertFalse(power["new_modem_or_flash_work_after_cleanup_deadline"])
        self.assertFalse(power["message_failure_blocks_poweroff"])
        self.assertTrue(power["existing_transport_preserved_during_grace"])
        self.assertEqual(
            power["incident_required_fields"],
            ["incident_id", "sequence", "unix_seconds", "clock_valid"],
        )
        self.assertEqual(
            power["timing_status"], "provisional_until_hardware_qualification"
        )
        self.assertFalse(power["battery_low_supported"])
        self.assertEqual(
            power["adapter_signal"],
            {
                "gpio": "gp7",
                "source": "u3_mp1584_out_plus_divider",
                "present_level": "high",
                "measurement_status": "unverified_until_hardware_qualification",
                "safe_upper_bound_mv": 3300,
            },
        )
        self.assertTrue(power["hardware_forced_off_timer_separate_from_firmware_grace"])
        self.assertEqual(
            power["shutdown_final_action"],
            {"usb_disconnected": "gp15_kill", "usb_connected": "watchdog_reboot"},
        )
        self.assertEqual(
            power["hardware_release_gates"],
            [
                "gp7_u3_voltage_residual_backfeed_10_cycle",
                "battery_load_voltage_current_temperature",
                "shutdown_termination_trace",
                "minimum_as_built_bom",
                "schematic_pcb_drc_continuity",
                "hardware_qualification_matrix",
            ],
        )
        self.assertEqual(
            power["grace_entry_actions"],
            [
                "buzzer_pwm_low",
                "stop_periodic_telemetry",
                "stop_long_reconnect",
                "power_failure_warning_once",
            ],
        )
        self.assertEqual(
            power["cleanup_order"],
            [
                "block_new_work",
                "dying_gasp_best_effort",
                "bounded_session_cleanup",
                "pdp_cfun_cpwroff_best_effort",
                "shutdown_record",
                "kill_or_watchdog_reboot",
            ],
        )

    def test_power_incident_lifecycle_is_exact_and_machine_readable(self):
        power = self.data["power"]
        self.assertIn("incident_lifecycle", power)
        lifecycle = power["incident_lifecycle"]
        assert_power_incident_lifecycle_contract(self, lifecycle)
        self.assertEqual(
            resolve_contract_path(self.data, lifecycle["required_fields_ref"]),
            power["incident_required_fields"],
        )
        self.assertEqual(
            set(lifecycle["event_transitions"]),
            set(lifecycle["correlated_events"]),
        )

    def test_power_incident_lifecycle_mutations_fail_closed(self):
        power = self.data["power"]
        self.assertIn("incident_lifecycle", power)

        changed_close_id = copy.deepcopy(power["incident_lifecycle"])
        changed_close_id["restore_after_commit"]["close_incident_id_source"] = (
            "new_incident_id"
        )
        with self.assertRaises(AssertionError):
            assert_power_incident_lifecycle_contract(self, changed_close_id)

        non_durable_identity = copy.deepcopy(power["incident_lifecycle"])
        non_durable_identity["identity"]["storage"] = "volatile"
        with self.assertRaises(AssertionError):
            assert_power_incident_lifecycle_contract(self, non_durable_identity)

    def test_event_codes_are_exact_unique_positive_integers(self):
        self.assertEqual(
            self.data["event_types"],
            {
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
            },
        )
        values = list(self.data["event_types"].values())
        self.assertTrue(all(type(value) is int and value > 0 for value in values))
        self.assertEqual(len(values), len(set(values)))

    def test_flash_slot_bootloader_and_metadata_safety(self):
        flash = self.data["flash_fota"]
        self.assertTrue(flash["central_service_required"])
        self.assertTrue(flash["lock_order_required"])
        self.assertTrue(flash["flash_safe_execute_required"])
        self.assertEqual(flash["log_entry_alignment_bytes"], 32)
        self.assertEqual(
            flash["partition_address_status"],
            "not_frozen_until_size_table_approval",
        )
        self.assertEqual(
            flash["partition_size_status"],
            "not_frozen_until_size_table_approval",
        )
        self.assertFalse(flash["confirmed_slot_writable"])
        self.assertEqual(flash["download_target"], "inactive_slot_only")
        self.assertTrue(flash["bootloader_immutable"])
        self.assertFalse(flash["bootloader_application_fota_writable"])
        self.assertEqual(
            flash["bootloader_responsibilities"],
            ["slot_validation", "slot_selection", "trial", "rollback"],
        )
        self.assertEqual(
            flash["service_recovery_methods"], ["usb_bootsel", "service_image"]
        )
        self.assertTrue(flash["firmware_ab_required"])
        self.assertEqual(flash["trial_boot_max"], 2)
        self.assertEqual(flash["confirm_deadline_ms"], 600000)
        self.assertTrue(flash["trial_policy_provisional"])
        self.assertTrue(flash["dual_journal_required"])
        self.assertTrue(flash["boot_failure_recovery_history_required"])
        self.assertEqual(flash["boot_diagnostic_history"], "bounded_local")
        self.assertEqual(
            flash["record_layout_status"],
            "not_frozen_until_size_table_approval",
        )
        self.assertEqual(
            flash["dual_journal_required_fields"],
            [
                "format_version",
                "length",
                "sequence",
                "crc32",
                "commit_marker",
                "active_slot",
                "pending_slot",
                "confirmed_slot",
                "bad_slot",
                "trial_count",
                "version",
                "hash",
                "rollback_reason",
            ],
        )
        self.assertNotIn("absolute_addresses", flash)
        self.assertNotIn("partition_addresses", flash)
        self.assertNotIn("slot_addresses", flash)

    def test_boot_metadata_crc_validity_and_selection_contract(self):
        flash = self.data["flash_fota"]
        self.assertIn("boot_metadata", flash)
        metadata = flash["boot_metadata"]
        self.assertEqual(metadata["copy_count"], 2)
        self.assertEqual(metadata["copy_validation"], "independent")
        self.assertEqual(metadata.get("copy_topology"), EXPECTED_COPY_TOPOLOGY)

        crc = metadata["crc"]
        self.assertEqual(crc["algorithm"], "CRC-32/ISO-HDLC")
        self.assertEqual(crc["excluded_fields"], ["crc32", "commit_marker"])
        self.assertIn("semantic_equality", crc)
        self.assertEqual(
            crc["semantic_equality"],
            {
                "projection_source": "ordered_crc_semantic_coverage_fields",
                "physical_byte_layout_claimed": False,
                "missing_field_result": "record_invalid",
                "unsupported_value_shape_result": "record_invalid",
            },
        )
        self.assertEqual(
            crc["semantic_coverage_fields"],
            [
                field
                for field in flash["dual_journal_required_fields"]
                if field not in crc["excluded_fields"]
            ],
        )
        self.assertEqual(
            crc["byte_layout_status"],
            "not_frozen_until_size_table_approval",
        )
        self.assertEqual(crc["byte_layout_status"], flash["record_layout_status"])
        self.assertEqual(
            metadata["committed_record_validity"],
            {
                "required_checks": [
                    "supported_format",
                    "approved_length_for_format",
                    "matching_crc",
                    "committed_marker",
                    "slot_state_invariants",
                    "candidate_identity_shape",
                    "rollback_reason_state_relationships",
                    "canonical_semantic_projection",
                    "candidate_trial_transition",
                ]
            },
        )
        self.assertEqual(
            metadata["selection"],
            {
                "candidate_state": "valid_committed",
                "winner": "highest_unsigned_sequence",
                "ignored_states": ["torn", "uncommitted", "crc_invalid"],
                "equal_sequence_identical_content": "acceptable",
                "equal_sequence_different_content": (
                    "service_recovery_without_image_write"
                ),
                "identical_projection_copy_id_tiebreak": (
                    "lexicographically_smallest_copy_id"
                ),
                "topology_validation": "before_record_filtering_and_selection",
                "selected_record_reference": {
                    "type": "immutable",
                    "fields": [
                        "copy_id",
                        "sequence",
                        "canonical_semantic_projection",
                    ],
                    "projection_source": "ordered_crc_semantic_coverage_fields",
                },
                "service_recovery_selected_record_reference": "none",
                "zero_valid_copies": "service_recovery_without_image_write",
            },
        )

    def test_logical_validation_envelope_contract_is_exact(self):
        metadata = self.data["flash_fota"]["boot_metadata"]
        self.assertEqual(
            metadata.get("logical_validation_envelope"),
            EXPECTED_LOGICAL_VALIDATION_ENVELOPE,
        )

    def test_logical_validation_abstraction_and_field_roles_are_exact(self):
        assert_logical_validation_abstraction_and_provenance_contract(
            self,
            self.data,
        )

    def test_logical_validation_abstraction_and_provenance_mutations_fail(self):
        assert_logical_validation_abstraction_and_provenance_contract(
            self,
            self.data,
        )
        prefix = (
            "flash_fota",
            "boot_metadata",
            "logical_validation_envelope",
        )
        delete = object()

        def mutation_at(path, value):
            fixture = copy.deepcopy(self.data)
            target = fixture
            for key in path[:-1]:
                target = target[key]
            if value is delete:
                target.pop(path[-1])
            else:
                target[path[-1]] = value
            return fixture

        mutation_specs = [
            (
                "language_neutral_semantics",
                prefix
                + ("abstraction", "normative_semantics", "language_scope"),
                "python_specific",
            ),
            (
                "representation_scope_deleted",
                prefix
                + (
                    "abstraction",
                    "host_executable_model",
                    "representation_scope",
                ),
                delete,
            ),
            (
                "host_labels_become_normative",
                prefix
                + (
                    "abstraction",
                    "host_executable_model",
                    "representation_labels_normative_for_production",
                ),
                True,
            ),
            (
                "python_claimed_as_firmware_evidence",
                prefix
                + (
                    "abstraction",
                    "host_executable_model",
                    "production_firmware_evidence",
                ),
                True,
            ),
            (
                "production_language_fixed_to_python",
                prefix
                + (
                    "abstraction",
                    "production_binding",
                    "language_binding",
                ),
                "python",
            ),
            (
                "production_binding_fixed_to_python",
                prefix
                + (
                    "abstraction",
                    "production_binding",
                    "container_binding",
                ),
                "python_container_abi",
            ),
            (
                "equivalent_semantics_not_required",
                prefix
                + (
                    "abstraction",
                    "production_binding",
                    "equivalent_semantics_required",
                ),
                False,
            ),
            (
                "physical_record_layout_claimed",
                prefix
                + (
                    "abstraction",
                    "normative_semantics",
                    "physical_record_layout_claimed",
                ),
                True,
            ),
            (
                "production_container_abi_claimed",
                prefix
                + (
                    "abstraction",
                    "normative_semantics",
                    "production_container_abi_claimed",
                ),
                True,
            ),
            (
                "journal_source_changed",
                prefix
                + (
                    "field_roles",
                    "roles",
                    "journal_semantic_fields",
                    "source",
                ),
                "host_fixture",
            ),
            (
                "journal_persistence_removed",
                prefix
                + (
                    "field_roles",
                    "roles",
                    "journal_semantic_fields",
                    "logical_persistence",
                ),
                False,
            ),
            (
                "journal_crc_removed",
                prefix
                + (
                    "field_roles",
                    "roles",
                    "journal_semantic_fields",
                    "crc_covered",
                ),
                False,
            ),
            (
                "copy_id_authority_granted",
                prefix
                + (
                    "field_roles",
                    "roles",
                    "storage_copy_reader_binding",
                    "authority_alone",
                ),
                True,
            ),
            (
                "copy_id_payload_authority_granted",
                prefix
                + (
                    "field_roles",
                    "roles",
                    "storage_copy_reader_binding",
                    "record_payload_authority",
                ),
                True,
            ),
            (
                "decoder_annotation_persisted",
                prefix
                + (
                    "field_roles",
                    "roles",
                    "decoder_validation_annotations",
                    "persisted_in_journal_payload",
                ),
                True,
            ),
            (
                "decoder_physical_layout_claimed",
                prefix
                + (
                    "field_roles",
                    "roles",
                    "decoder_validation_annotations",
                    "physical_byte_layout_claimed",
                ),
                True,
            ),
            (
                "marker_annotation_added_to_crc",
                prefix
                + (
                    "field_roles",
                    "roles",
                    "commit_marker_logical_annotation",
                    "crc_covered",
                ),
                True,
            ),
            (
                "marker_underlying_persistence_removed",
                prefix
                + (
                    "field_roles",
                    "roles",
                    "commit_marker_logical_annotation",
                    "underlying_marker",
                    "persisted",
                ),
                False,
            ),
            (
                "valid_slot_context_source_changed",
                prefix
                + (
                    "field_roles",
                    "roles",
                    "host_executable_model_validation_context",
                    "field_sources",
                    "valid_firmware_slots",
                ),
                "record_payload",
            ),
            (
                "slot_context_persisted",
                prefix
                + (
                    "field_roles",
                    "roles",
                    "host_executable_model_validation_context",
                    "persisted_in_journal_payload",
                ),
                True,
            ),
            (
                "slot_context_crc_covered",
                prefix
                + (
                    "field_roles",
                    "roles",
                    "host_executable_model_validation_context",
                    "crc_covered",
                ),
                True,
            ),
            (
                "slot_context_authority_granted",
                prefix
                + (
                    "field_roles",
                    "roles",
                    "host_executable_model_validation_context",
                    "authority_alone",
                ),
                True,
            ),
            (
                "external_revalidation_separation_removed",
                prefix
                + (
                    "field_roles",
                    "roles",
                    "host_executable_model_validation_context",
                    "external_revalidation_separation",
                    "current_pending_and_confirmed_revalidation_"
                    "separately_required",
                ),
                False,
            ),
            (
                "external_revalidation_ref_changed",
                prefix
                + (
                    "field_roles",
                    "roles",
                    "host_executable_model_validation_context",
                    "external_revalidation_separation",
                    "ref",
                ),
                "flash_fota.boot_metadata.selection",
            ),
        ]
        for name, path, value in mutation_specs:
            with self.subTest(name=name):
                with self.assertRaises(AssertionError):
                    assert_logical_validation_abstraction_and_provenance_contract(
                        self,
                        mutation_at(path, value),
                    )

        duplicate_role = copy.deepcopy(self.data)
        duplicate_role["flash_fota"]["boot_metadata"][
            "logical_validation_envelope"
        ]["field_roles"]["roles"]["decoder_validation_annotations"][
            "fields"
        ].append("copy_id")
        missing_role = copy.deepcopy(self.data)
        missing_role["flash_fota"]["boot_metadata"][
            "logical_validation_envelope"
        ]["field_roles"]["roles"][
            "host_executable_model_validation_context"
        ]["fields"].remove("failed_candidate_slots")
        for name, fixture in [
            ("duplicate_required_field_role", duplicate_role),
            ("missing_required_field_role", missing_role),
        ]:
            with self.subTest(name=name):
                with self.assertRaises(AssertionError):
                    assert_logical_validation_abstraction_and_provenance_contract(
                        self,
                        fixture,
                    )

    def test_malformed_snapshot_and_record_envelopes_recover_exactly(self):
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        base = make_boot_metadata_copy("copy_a", 10)
        required_fields = EXPECTED_LOGICAL_VALIDATION_ENVELOPE["record"][
            "required_fields"
        ]
        record_cases = []
        for field in required_fields:
            record = copy.deepcopy(base)
            record.pop(field)
            record_cases.append((f"missing_{field}", [record]))

        for name, element in [
            ("element_none", None),
            ("element_list", []),
            ("element_string", "record"),
            ("element_integer", 7),
        ]:
            record_cases.append((name, [element]))

        for field, value in [
            ("state", ["committed"]),
            ("state", {"committed": True}),
            ("commit_marker_state", ["programmed"]),
            ("commit_marker_state", {"programmed": True}),
        ]:
            record = copy.deepcopy(base)
            record[field] = value
            record_cases.append((f"{field}_{type(value).__name__}", [record]))

        invalid_slot_collections = [
            None,
            False,
            7,
            {"logical_a": False},
            "logical_a",
            b"logical_a",
            [["logical_a"]],
            [7],
        ]
        for field in ["valid_firmware_slots", "failed_candidate_slots"]:
            for value in invalid_slot_collections:
                record = copy.deepcopy(base)
                record[field] = value
                record_cases.append(
                    (f"{field}_{type(value).__name__}_{value!r}", [record])
                )

        for field in ["format_version", "length", "sequence", "trial_count"]:
            record = copy.deepcopy(base)
            record[field] = True
            record_cases.append((f"boolean_{field}", [record]))

        for field in ["supported_format", "approved_length", "crc_matches"]:
            record = copy.deepcopy(base)
            record[field] = 1
            record_cases.append((f"nonboolean_{field}", [record]))

        for field in [
            "copy_id",
            "active_slot",
            "confirmed_slot",
            "pending_slot",
            "bad_slot",
            "version",
            "hash",
            "rollback_reason",
        ]:
            record = copy.deepcopy(base)
            record[field] = []
            record_cases.append((f"nonstr_{field}", [record]))

        unknown_claim = copy.deepcopy(base)
        unknown_claim["candidate_verified"] = True
        record_cases.append(("unknown_candidate_verified", [unknown_claim]))

        top_level_cases = [
            ("top_none", lambda: None),
            ("top_mapping", lambda: copy.deepcopy(base)),
            ("top_string", lambda: "copy_a"),
            ("top_bytes", lambda: b"copy_a"),
            ("top_scalar", lambda: 7),
            ("top_generator", lambda: (record for record in [base])),
            (
                "over_cardinality",
                lambda: [
                    make_boot_metadata_copy("copy_a", 1),
                    make_boot_metadata_copy("copy_b", 1),
                    make_boot_metadata_copy("copy_a", 2),
                ],
            ),
        ]

        for name, snapshot in record_cases:
            for api in ["selection", "decision"]:
                with self.subTest(name=name, api=api):
                    copies = copy.deepcopy(snapshot)
                    if api == "selection":
                        self.assertEqual(
                            select_boot_metadata_copy(
                                metadata,
                                copies,
                                flash["trial_boot_max"],
                            ),
                            ("service_recovery_without_image_write", None),
                        )
                    else:
                        self.assertEqual(
                            decide_boot_metadata(
                                metadata,
                                copies,
                                flash["trial_boot_max"],
                                verified_candidate_identities=[],
                                revalidated_bootable_slots=["logical_a"],
                            ),
                            EXPECTED_SERVICE_RECOVERY_DECISION,
                        )

        for name, snapshot_factory in top_level_cases:
            for api in ["selection", "decision"]:
                with self.subTest(name=name, api=api):
                    copies = snapshot_factory()
                    if api == "selection":
                        self.assertEqual(
                            select_boot_metadata_copy(
                                metadata,
                                copies,
                                flash["trial_boot_max"],
                            ),
                            ("service_recovery_without_image_write", None),
                        )
                    else:
                        self.assertEqual(
                            decide_boot_metadata(
                                metadata,
                                copies,
                                flash["trial_boot_max"],
                                verified_candidate_identities=[],
                                revalidated_bootable_slots=["logical_a"],
                            ),
                            EXPECTED_SERVICE_RECOVERY_DECISION,
                        )

        missing_copy_id = copy.deepcopy(base)
        missing_copy_id.pop("copy_id")
        nonstring_copy_id = copy.deepcopy(base)
        nonstring_copy_id["copy_id"] = []
        for name, malformed_second in [
            ("second_none", None),
            ("second_list", []),
            ("second_mapping_without_id", {}),
            ("second_missing_copy_id", missing_copy_id),
            ("second_nonstring_copy_id", nonstring_copy_id),
        ]:
            with self.subTest(name=name, api="selection"):
                self.assertEqual(
                    select_boot_metadata_copy(
                        metadata,
                        [copy.deepcopy(base), malformed_second],
                        flash["trial_boot_max"],
                    ),
                    ("service_recovery_without_image_write", None),
                )
            with self.subTest(name=name, api="decision"):
                self.assertEqual(
                    decide_boot_metadata(
                        metadata,
                        [copy.deepcopy(base), malformed_second],
                        flash["trial_boot_max"],
                        verified_candidate_identities=[],
                        revalidated_bootable_slots=["logical_a"],
                    ),
                    EXPECTED_SERVICE_RECOVERY_DECISION,
                )

    def test_well_formed_snapshot_envelopes_preserve_selection(self):
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        copy_a = make_boot_metadata_copy("copy_a", 10)
        copy_b = make_boot_metadata_copy("copy_b", 10)
        torn = make_boot_metadata_copy(
            "copy_b",
            11,
            state="torn",
            commit_marker_state="torn",
        )
        crc_invalid = make_boot_metadata_copy(
            "copy_b",
            11,
            state="crc_invalid",
            crc_matches=False,
        )
        self.assertEqual(
            select_boot_metadata_copy(metadata, [], flash["trial_boot_max"]),
            ("service_recovery_without_image_write", None),
        )
        self.assertEqual(
            decide_boot_metadata(metadata, tuple(), flash["trial_boot_max"]),
            EXPECTED_SERVICE_RECOVERY_DECISION,
        )
        for record in [copy_a, copy_b]:
            with self.subTest(kind="sole", copy_id=record["copy_id"]):
                self.assert_selected_copy(
                    select_boot_metadata_copy(
                        metadata,
                        (record,),
                        flash["trial_boot_max"],
                    ),
                    record["copy_id"],
                )
        self.assert_selected_copy(
            select_boot_metadata_copy(
                metadata,
                [copy_a, copy_b],
                flash["trial_boot_max"],
            ),
            "copy_a",
        )
        for ignored in [torn, crc_invalid]:
            with self.subTest(kind="well_formed_ignored", state=ignored["state"]):
                self.assert_selected_copy(
                    select_boot_metadata_copy(
                        metadata,
                        [copy_a, ignored],
                        flash["trial_boot_max"],
                    ),
                    "copy_a",
                )
        for collection_type in [list, tuple, set, frozenset]:
            with self.subTest(slot_collection=collection_type.__name__):
                record = make_boot_metadata_copy(
                    "copy_a",
                    10,
                    valid_firmware_slots=collection_type(
                        ["logical_a", "logical_b"]
                    ),
                    failed_candidate_slots=collection_type(),
                )
                self.assert_selected_copy(
                    select_boot_metadata_copy(
                        metadata,
                        [record],
                        flash["trial_boot_max"],
                    ),
                    "copy_a",
                )

    def test_identified_invalid_record_preserves_other_copy_fallback(self):
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        invalid_cases = []
        for field in [
            "commit_marker_state",
            "valid_firmware_slots",
            "rollback_reason",
        ]:
            record = make_boot_metadata_copy("copy_a", 10)
            record.pop(field)
            invalid_cases.append((f"missing_{field}", record))
        for name, field, value in [
            ("list_state", "state", ["committed"]),
            ("numeric_slot", "active_slot", 1),
            ("boolean_sequence", "sequence", True),
            ("negative_trial", "trial_count", -1),
            ("overflow_trial", "trial_count", 3),
            ("slot_mapping", "valid_firmware_slots", {"logical_a": False}),
        ]:
            record = make_boot_metadata_copy("copy_a", 10)
            record[field] = value
            invalid_cases.append((name, record))
        unknown_claim = make_boot_metadata_copy("copy_a", 10)
        unknown_claim["candidate_verified"] = True
        invalid_cases.append(("unknown_claim", unknown_claim))

        valid = make_boot_metadata_copy("copy_b", 10)
        for name, invalid in invalid_cases:
            with self.subTest(name=name, api="selection"):
                self.assert_selected_copy(
                    select_boot_metadata_copy(
                        metadata,
                        [invalid, valid],
                        flash["trial_boot_max"],
                    ),
                    "copy_b",
                )
            with self.subTest(name=name, api="decision"):
                decision = decide_boot_metadata(
                    metadata,
                    [invalid, valid],
                    flash["trial_boot_max"],
                    verified_candidate_identities=[],
                    revalidated_bootable_slots=["logical_a"],
                )
                self.assertEqual(decision["disposition"], "boot_confirmed")
                self.assertEqual(
                    decision["source_record_reference"].copy_id,
                    "copy_b",
                )
                self.assertIsNone(decision["next_journal_record"])
                self.assertIsNone(decision["post_commit_action"])

    def test_malformed_records_make_individual_validators_nonthrowing(self):
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        base = make_boot_metadata_copy("copy_a", 10)
        missing_marker = copy.deepcopy(base)
        missing_marker.pop("commit_marker_state")
        list_state = copy.deepcopy(base)
        list_state["state"] = ["committed"]
        missing_slots = copy.deepcopy(base)
        missing_slots["valid_firmware_slots"] = None
        boolean_sequence = copy.deepcopy(base)
        boolean_sequence["sequence"] = True
        for name, record in [
            ("none", None),
            ("list", []),
            ("missing_marker", missing_marker),
            ("list_state", list_state),
            ("missing_slots", missing_slots),
            ("boolean_sequence", boolean_sequence),
        ]:
            with self.subTest(name=name, validator="projection"):
                self.assertIsNone(
                    boot_metadata_semantic_projection(metadata, record)
                )
            for validator in [
                commit_marker_is_valid,
                candidate_identity_shape_is_valid,
            ]:
                with self.subTest(name=name, validator=validator.__name__):
                    self.assertFalse(validator(metadata, record))
            for validator in [
                slot_state_invariants_are_valid,
                rollback_reason_state_is_valid,
            ]:
                with self.subTest(name=name, validator=validator.__name__):
                    self.assertFalse(
                        validator(metadata, record, flash["trial_boot_max"])
                    )

    def test_malformed_transition_records_return_invalid_and_false(self):
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        identity = [("logical_b", "2.0.0", "b" * 64)]
        source = make_boot_metadata_copy("copy_a", 10)
        destination = make_boot_metadata_copy(
            "copy_b",
            11,
            pending_slot="logical_b",
            version="2.0.0",
            hash="b" * 64,
        )
        missing_marker = copy.deepcopy(source)
        missing_marker.pop("commit_marker_state")
        list_state = copy.deepcopy(source)
        list_state["state"] = ["committed"]
        none_slots = copy.deepcopy(source)
        none_slots["valid_firmware_slots"] = None
        boolean_sequence = copy.deepcopy(source)
        boolean_sequence["sequence"] = True
        malformed_records = [
            ("none", None),
            ("list", []),
            ("missing_marker", missing_marker),
            ("list_state", list_state),
            ("none_slots", none_slots),
            ("boolean_sequence", boolean_sequence),
        ]
        for name, malformed in malformed_records:
            for side in ["source", "destination"]:
                with self.subTest(name=name, side=side, api="classifier"):
                    actual_source = malformed if side == "source" else source
                    actual_destination = (
                        malformed if side == "destination" else destination
                    )
                    self.assertEqual(
                        classify_boot_metadata_transition(
                            metadata,
                            actual_source,
                            actual_destination,
                            flash["trial_boot_max"],
                            verified_candidate_identities=identity,
                            revalidated_bootable_slots=[],
                        ),
                        "invalid",
                    )
                with self.subTest(name=name, side=side, api="validator"):
                    actual_source = malformed if side == "source" else source
                    actual_destination = (
                        malformed if side == "destination" else destination
                    )
                    self.assertFalse(
                        boot_metadata_transition_is_valid(
                            metadata,
                            actual_source,
                            actual_destination,
                            "stage_verified_candidate",
                            flash["trial_boot_max"],
                            verified_candidate_identities=identity,
                            revalidated_bootable_slots=[],
                        )
                    )

    def test_external_revalidation_input_schema_is_exact(self):
        metadata = self.data["flash_fota"]["boot_metadata"]
        self.assertEqual(
            metadata["boot_decision"]["external_revalidation"].get(
                "input_schema"
            ),
            EXPECTED_EXTERNAL_REVALIDATION_INPUT_SCHEMA,
        )
        expected_ref = (
            "flash_fota.boot_metadata.boot_decision.external_revalidation."
            "input_schema"
        )
        transitions = metadata["candidate_trial_transitions"]
        self.assertEqual(
            transitions["verification_evidence"].get("input_schema_ref"),
            expected_ref,
        )
        self.assertEqual(
            transitions["failure_or_exhaustion_terminal"][
                "pending_revalidation_failed_authorization"
            ].get("input_schema_ref"),
            expected_ref,
        )

    def test_malformed_external_evidence_recovers_without_authority(self):
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        candidate_hash = "b" * 64
        identity = ("logical_b", "2.0.0", candidate_hash)
        pending = make_boot_metadata_copy(
            "copy_a",
            10,
            pending_slot="logical_b",
            version="2.0.0",
            hash=candidate_hash,
        )
        confirmed_factories = [
            ("substring_string", lambda: "xlogical_a"),
            ("exact_string", lambda: "logical_a"),
            ("bytes", lambda: b"logical_a"),
            ("false_mapping", lambda: {"logical_a": False}),
            ("scalar", lambda: 7),
            ("generator", lambda: (slot for slot in ["logical_a"])),
            ("nested", lambda: [["logical_a"]]),
            ("unknown_slot", lambda: ["logical_x"]),
        ]
        pending_factories = [
            ("false_mapping", lambda: {identity: False}),
            ("string", lambda: "logical_b"),
            ("bytes", lambda: b"logical_b"),
            ("scalar", lambda: 7),
            ("generator", lambda: (item for item in [identity])),
            ("wrong_arity", lambda: [("logical_b", "2.0.0")]),
            (
                "identity_list",
                lambda: [["logical_b", "2.0.0", candidate_hash]],
            ),
            ("non_string", lambda: [("logical_b", 2, candidate_hash)]),
            ("none_slot", lambda: [("none", "2.0.0", candidate_hash)]),
            ("blank_version", lambda: [("logical_b", " ", candidate_hash)]),
            ("uppercase_hash", lambda: [("logical_b", "2.0.0", "B" * 64)]),
            ("malformed_hash", lambda: [("logical_b", "2.0.0", "bad")]),
            ("mixed", lambda: [identity, ("logical_b", "2.0.0")]),
        ]
        for name, evidence_factory in confirmed_factories:
            for pending_evidence_kind, pending_evidence in [
                ("exact", [identity]),
                ("missing", []),
            ]:
                with self.subTest(
                    kind="confirmed",
                    name=name,
                    pending_evidence=pending_evidence_kind,
                ):
                    self.assertEqual(
                        decide_boot_metadata(
                            metadata,
                            [pending],
                            flash["trial_boot_max"],
                            verified_candidate_identities=pending_evidence,
                            revalidated_bootable_slots=evidence_factory(),
                        ),
                        EXPECTED_SERVICE_RECOVERY_DECISION,
                    )
        for name, evidence_factory in pending_factories:
            with self.subTest(kind="pending", name=name):
                self.assertEqual(
                    decide_boot_metadata(
                        metadata,
                        [pending],
                        flash["trial_boot_max"],
                        verified_candidate_identities=evidence_factory(),
                        revalidated_bootable_slots=["logical_a"],
                    ),
                    EXPECTED_SERVICE_RECOVERY_DECISION,
                )

    def test_valid_external_evidence_container_matrix_is_exact(self):
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        candidate_hash = "b" * 64
        identity = ("logical_b", "2.0.0", candidate_hash)
        container_factories = [list, tuple, set, frozenset]
        dispositions = {
            0: "attempt_required_commit_then_boot_pending",
            1: "attempt_required_commit_then_boot_pending",
            2: (
                "rollback_required_trial_exhausted_commit_then_"
                "boot_confirmed"
            ),
        }
        for container_type in container_factories:
            for count, disposition in dispositions.items():
                with self.subTest(
                    container=container_type.__name__,
                    count=count,
                ):
                    pending = make_boot_metadata_copy(
                        "copy_a",
                        10,
                        pending_slot="logical_b",
                        trial_count=count,
                        version="2.0.0",
                        hash=candidate_hash,
                    )
                    decision = decide_boot_metadata(
                        metadata,
                        [pending],
                        flash["trial_boot_max"],
                        verified_candidate_identities=container_type([identity]),
                        revalidated_bootable_slots=container_type(["logical_a"]),
                    )
                    self.assertEqual(decision["disposition"], disposition)

            with self.subTest(container=container_type.__name__, kind="empty"):
                pending = make_boot_metadata_copy(
                    "copy_a",
                    10,
                    pending_slot="logical_b",
                    version="2.0.0",
                    hash=candidate_hash,
                )
                missing = decide_boot_metadata(
                    metadata,
                    [pending],
                    flash["trial_boot_max"],
                    verified_candidate_identities=container_type(),
                    revalidated_bootable_slots=container_type(["logical_a"]),
                )
                self.assertEqual(
                    missing["disposition"],
                    "rollback_required_pending_revalidation_failed_commit_"
                    "then_boot_confirmed",
                )
                confirmed_missing = decide_boot_metadata(
                    metadata,
                    [pending],
                    flash["trial_boot_max"],
                    verified_candidate_identities=container_type([identity]),
                    revalidated_bootable_slots=container_type(),
                )
                self.assertEqual(
                    confirmed_missing,
                    EXPECTED_SERVICE_RECOVERY_DECISION,
                )

        duplicate_evidence = decide_boot_metadata(
            metadata,
            [
                make_boot_metadata_copy(
                    "copy_a",
                    10,
                    pending_slot="logical_b",
                    version="2.0.0",
                    hash=candidate_hash,
                )
            ],
            flash["trial_boot_max"],
            verified_candidate_identities=[identity, identity],
            revalidated_bootable_slots=["logical_a", "logical_a"],
        )
        self.assertEqual(
            duplicate_evidence["disposition"],
            "attempt_required_commit_then_boot_pending",
        )
        wrong_identity = ("logical_b", "9.9.9", "c" * 64)
        wrong = decide_boot_metadata(
            metadata,
            [
                make_boot_metadata_copy(
                    "copy_a",
                    10,
                    pending_slot="logical_b",
                    version="2.0.0",
                    hash=candidate_hash,
                )
            ],
            flash["trial_boot_max"],
            verified_candidate_identities=[wrong_identity],
            revalidated_bootable_slots=["logical_a"],
        )
        self.assertEqual(
            wrong["disposition"],
            "rollback_required_pending_revalidation_failed_commit_then_"
            "boot_confirmed",
        )

    def test_malformed_evidence_invalidates_current_classifier_only(self):
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        candidate_hash = "b" * 64
        identity = ("logical_b", "2.0.0", candidate_hash)
        source = make_boot_metadata_copy(
            "copy_a",
            10,
            pending_slot="logical_b",
            version="2.0.0",
            hash=candidate_hash,
        )
        terminal = make_boot_metadata_copy(
            "copy_b",
            11,
            bad_slot="logical_b",
            version="2.0.0",
            hash=candidate_hash,
            rollback_reason="pending_revalidation_failed",
            failed_candidate_slots=["logical_b"],
        )
        malformed_factories = [
            ("pending_mapping", lambda: ({identity: False}, ["logical_a"])),
            ("pending_string", lambda: ("logical_b", ["logical_a"])),
            ("pending_bytes", lambda: (b"logical_b", ["logical_a"])),
            ("pending_scalar", lambda: (7, ["logical_a"])),
            (
                "pending_generator",
                lambda: ((item for item in [identity]), ["logical_a"]),
            ),
            (
                "pending_wrong_arity",
                lambda: ([("logical_b", "2.0.0")], ["logical_a"]),
            ),
            (
                "pending_identity_list",
                lambda: (
                    [["logical_b", "2.0.0", candidate_hash]],
                    ["logical_a"],
                ),
            ),
            (
                "pending_non_string",
                lambda: ([("logical_b", 2, candidate_hash)], ["logical_a"]),
            ),
            (
                "pending_none_slot",
                lambda: ([("none", "2.0.0", candidate_hash)], ["logical_a"]),
            ),
            (
                "pending_blank_version",
                lambda: ([("logical_b", " ", candidate_hash)], ["logical_a"]),
            ),
            (
                "pending_uppercase_hash",
                lambda: ([("logical_b", "2.0.0", "B" * 64)], ["logical_a"]),
            ),
            (
                "pending_malformed_hash",
                lambda: ([("logical_b", "2.0.0", "bad")], ["logical_a"]),
            ),
            (
                "pending_mixed",
                lambda: (
                    [identity, ("logical_b", "2.0.0")],
                    ["logical_a"],
                ),
            ),
            ("confirmed_substring", lambda: ([identity], "xlogical_a")),
            ("confirmed_exact_string", lambda: ([identity], "logical_a")),
            ("confirmed_bytes", lambda: ([identity], b"logical_a")),
            (
                "confirmed_mapping",
                lambda: ([identity], {"logical_a": False}),
            ),
            ("confirmed_scalar", lambda: ([identity], 7)),
            (
                "confirmed_generator",
                lambda: ([identity], (slot for slot in ["logical_a"])),
            ),
            ("confirmed_nested", lambda: ([identity], [["logical_a"]])),
            ("confirmed_unknown", lambda: ([identity], ["logical_x"])),
        ]
        for name, evidence_factory in malformed_factories:
            with self.subTest(name=name, api="classifier"):
                pending_evidence, confirmed_evidence = evidence_factory()
                self.assertEqual(
                    classify_boot_metadata_transition(
                        metadata,
                        source,
                        terminal,
                        flash["trial_boot_max"],
                        verified_candidate_identities=pending_evidence,
                        revalidated_bootable_slots=confirmed_evidence,
                    ),
                    "invalid",
                )
            with self.subTest(name=name, api="validator"):
                pending_evidence, confirmed_evidence = evidence_factory()
                self.assertFalse(
                    boot_metadata_transition_is_valid(
                        metadata,
                        source,
                        terminal,
                        "pending_revalidation_failed",
                        flash["trial_boot_max"],
                        verified_candidate_identities=pending_evidence,
                        revalidated_bootable_slots=confirmed_evidence,
                    )
                )

        self.assertEqual(
            classify_boot_metadata_transition(
                metadata,
                source,
                terminal,
                flash["trial_boot_max"],
                verified_candidate_identities="malformed",
                revalidated_bootable_slots={"logical_a": False},
                journal_commit_authorization=False,
            ),
            "pending_revalidation_failed",
        )
        self.assertEqual(
            classify_boot_metadata_transition(
                metadata,
                source,
                terminal,
                flash["trial_boot_max"],
                verified_candidate_identities=[],
                revalidated_bootable_slots=["logical_a"],
            ),
            "pending_revalidation_failed",
        )
        self.assertEqual(
            classify_boot_metadata_transition(
                metadata,
                source,
                terminal,
                flash["trial_boot_max"],
                verified_candidate_identities=[identity],
                revalidated_bootable_slots=["logical_a"],
            ),
            "invalid",
        )

    def test_boot_metadata_record_model_uses_every_crc_semantic_field(self):
        metadata = self.data["flash_fota"]["boot_metadata"]
        parameters = inspect.signature(make_boot_metadata_copy).parameters
        self.assertNotIn("semantic_content", parameters)
        for field in metadata["crc"]["semantic_coverage_fields"]:
            with self.subTest(field=field):
                self.assertIn(field, parameters)

    def test_crc_semantic_projection_is_contract_driven_and_fail_closed(self):
        self.assertIn("boot_metadata_semantic_projection", globals())
        metadata = self.data["flash_fota"]["boot_metadata"]
        record = make_boot_metadata_copy("copy_a", 8)
        projection = boot_metadata_semantic_projection(metadata, record)
        self.assertEqual(
            [field for field, _ in projection],
            metadata["crc"]["semantic_coverage_fields"],
        )

        mutations = {
            "format_version": 2,
            "length": 2,
            "sequence": 9,
            "active_slot": "logical_b",
            "pending_slot": "logical_b",
            "confirmed_slot": "logical_b",
            "bad_slot": "logical_b",
            "trial_count": 1,
            "version": "2.0.0",
            "hash": "b" * 64,
            "rollback_reason": "explicit_trial_failure",
        }
        self.assertEqual(
            set(mutations),
            set(metadata["crc"]["semantic_coverage_fields"]),
        )
        for field, value in mutations.items():
            with self.subTest(field=field):
                mutated = copy.deepcopy(record)
                mutated[field] = value
                self.assertNotEqual(
                    boot_metadata_semantic_projection(metadata, mutated),
                    projection,
                )

        missing = copy.deepcopy(record)
        missing.pop("rollback_reason")
        self.assertIsNone(boot_metadata_semantic_projection(metadata, missing))
        unsupported = copy.deepcopy(record)
        unsupported["version"] = {"not": "scalar"}
        self.assertIsNone(
            boot_metadata_semantic_projection(metadata, unsupported)
        )
        boolean_integer = copy.deepcopy(record)
        boolean_integer["length"] = True
        self.assertIsNone(
            boot_metadata_semantic_projection(metadata, boolean_integer)
        )

    def test_equal_sequence_projection_is_order_independent(self):
        self.assertNotIn(
            "semantic_content",
            inspect.signature(make_boot_metadata_copy).parameters,
        )
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        left = make_boot_metadata_copy("copy_b", 8)
        right = make_boot_metadata_copy("copy_a", 8)
        for copies in ([left, right], [right, left]):
            with self.subTest(kind="identical", order=[c["copy_id"] for c in copies]):
                self.assert_selected_copy(
                    select_boot_metadata_copy(
                        metadata,
                        copies,
                        flash["trial_boot_max"],
                    ),
                    "copy_a",
                )

        conflicting = copy.deepcopy(right)
        conflicting.update(
            {
                "active_slot": "logical_b",
                "confirmed_slot": "logical_b",
                "version": "2.0.0",
                "hash": "b" * 64,
            }
        )
        for copies in ([left, conflicting], [conflicting, left]):
            with self.subTest(kind="conflict", order=[c["copy_id"] for c in copies]):
                self.assertEqual(
                    select_boot_metadata_copy(
                        metadata,
                        copies,
                        flash["trial_boot_max"],
                    ),
                    ("service_recovery_without_image_write", None),
                )

    def test_selection_fails_closed_for_invalid_semantic_projection(self):
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        semantic_fields = metadata["crc"]["semantic_coverage_fields"]
        for field in semantic_fields:
            with self.subTest(field=field, mutation="missing"):
                missing = make_boot_metadata_copy("copy_a", 8)
                missing.pop(field)
                self.assertEqual(
                    select_boot_metadata_copy(
                        metadata,
                        [missing],
                        flash["trial_boot_max"],
                    ),
                    ("service_recovery_without_image_write", None),
                )

            with self.subTest(field=field, mutation="container"):
                unsupported = make_boot_metadata_copy("copy_a", 8)
                unsupported[field] = {"unsupported": "container"}
                self.assertEqual(
                    select_boot_metadata_copy(
                        metadata,
                        [unsupported],
                        flash["trial_boot_max"],
                    ),
                    ("service_recovery_without_image_write", None),
                )

    def test_boot_decision_contract_is_exact(self):
        metadata = self.data["flash_fota"]["boot_metadata"]
        self.assertEqual(metadata.get("boot_decision"), EXPECTED_BOOT_DECISION)

    def test_boot_metadata_marker_and_slot_semantics_are_exact(self):
        metadata = self.data["flash_fota"]["boot_metadata"]
        self.assertIn("commit_marker_semantics", metadata)
        self.assertIn("slot_state_invariants", metadata)
        assert_boot_metadata_state_semantics_contract(self, metadata)

        transition = metadata["commit_marker_semantics"]["program_transition"]
        write_steps = metadata["write_commit_sequence"]
        self.assertLess(
            write_steps.index(transition["allowed_after_step"]),
            write_steps.index("program_commit_marker_last"),
        )
        self.assertEqual(
            metadata["commit_marker_semantics"][
                "old_valid_copy_preserved_until"
            ],
            metadata["old_valid_copy_preserved_until"],
        )
        slot_invariants = metadata["slot_state_invariants"]
        self.assertFalse(slot_invariants["numeric_slot_codes_allowed"])
        self.assertFalse(slot_invariants["physical_layout_values_allowed"])
        self.assertTrue(
            all(
                isinstance(slot, str)
                for slot in slot_invariants["logical_firmware_slots"]
            )
        )

    def test_boot_metadata_state_semantic_mutations_fail_closed(self):
        metadata = self.data["flash_fota"]["boot_metadata"]
        self.assertIn("commit_marker_semantics", metadata)
        self.assertIn("slot_state_invariants", metadata)

        marker_mutation = copy.deepcopy(metadata)
        marker_mutation["commit_marker_semantics"]["state_meanings"][
            "partial"
        ] = "committed"
        with self.assertRaises(AssertionError):
            assert_boot_metadata_state_semantics_contract(self, marker_mutation)

        slot_mutation = copy.deepcopy(metadata)
        slot_mutation["slot_state_invariants"]["pending_slot"][
            "trial_must_not_equal_field"
        ] = "active_slot"
        with self.assertRaises(AssertionError):
            assert_boot_metadata_state_semantics_contract(self, slot_mutation)

        trial_mutation = copy.deepcopy(metadata)
        self.assertIn(
            "trial_count",
            trial_mutation["slot_state_invariants"],
        )
        trial_mutation["slot_state_invariants"]["trial_count"][
            "type"
        ] = "integer_including_boolean"
        with self.assertRaises(AssertionError):
            assert_boot_metadata_state_semantics_contract(self, trial_mutation)

        action_mutation = copy.deepcopy(metadata)
        self.assertIn(
            "next_boot_actions",
            action_mutation["slot_state_invariants"],
        )
        action_mutation["slot_state_invariants"]["next_boot_actions"][
            "valid_pending_trial"
        ]["action"] = "boot_pending_without_durable_increment"
        with self.assertRaises(AssertionError):
            assert_boot_metadata_state_semantics_contract(self, action_mutation)

    def test_candidate_trial_transition_contract_is_exact(self):
        metadata = self.data["flash_fota"]["boot_metadata"]
        self.assertIn("candidate_trial_transitions", metadata)
        self.assertEqual(
            metadata["candidate_trial_transitions"],
            EXPECTED_CANDIDATE_TRIAL_TRANSITIONS,
        )

    def test_candidate_trial_transition_contract_mutations_fail_closed(self):
        metadata = self.data["flash_fota"]["boot_metadata"]
        self.assertIn("candidate_trial_transitions", metadata)
        self.assertIn(
            "individual_committed_record_identity",
            metadata["candidate_trial_transitions"],
        )
        self.assertIn(
            "verification_evidence",
            metadata["candidate_trial_transitions"],
        )
        self.assertIn(
            "transition_classifier",
            metadata["candidate_trial_transitions"],
        )

        identity = copy.deepcopy(metadata)
        identity["candidate_trial_transitions"][
            "candidate_identity_fields"
        ].remove("hash")

        staging = copy.deepcopy(metadata)
        staging["candidate_trial_transitions"][
            "verified_new_candidate_staging"
        ]["atomic_fields"].remove("version")

        retry = copy.deepcopy(metadata)
        retry["candidate_trial_transitions"][
            "same_candidate_restage_or_retry"
        ]["trial_count_rule"] = "reset_allowed"

        bad_clear = copy.deepcopy(metadata)
        bad_clear["candidate_trial_transitions"]["bad_identity_reuse"][
            "same_exact_identity"
        ] = "clear_bad_and_retry"

        confirmation = copy.deepcopy(metadata)
        confirmation["candidate_trial_transitions"][
            "confirmation_terminal"
        ]["atomic_destination"]["pending_slot"] = "source.pending_slot"

        failure = copy.deepcopy(metadata)
        failure["candidate_trial_transitions"][
            "failure_or_exhaustion_terminal"
        ]["atomic_destination"]["bad_slot"] = "none"

        identity_shape = copy.deepcopy(metadata)
        identity_shape["candidate_trial_transitions"][
            "individual_committed_record_identity"
        ]["hash"]["format"] = "any_string"

        verification_evidence = copy.deepcopy(metadata)
        verification_evidence["candidate_trial_transitions"][
            "verification_evidence"
        ]["metadata_boolean_trusted"] = True

        classifier = copy.deepcopy(metadata)
        classifier["candidate_trial_transitions"][
            "transition_classifier"
        ]["journal_transition_kinds"].append("attempt")

        for name, mutation in [
            ("identity", identity),
            ("staging_atomic_fields", staging),
            ("retry_preservation", retry),
            ("bad_clear", bad_clear),
            ("confirmation_terminal", confirmation),
            ("failure_terminal", failure),
            ("individual_identity_shape", identity_shape),
            ("verification_evidence", verification_evidence),
            ("transition_classifier", classifier),
        ]:
            with self.subTest(name=name):
                with self.assertRaises(AssertionError):
                    assert_boot_metadata_state_semantics_contract(
                        self,
                        mutation,
                    )

    def test_candidate_identity_shape_applies_to_sole_and_base_copies(self):
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        valid = make_boot_metadata_copy(
            "copy_b",
            8,
            version="2.0.0",
            hash="b" * 64,
        )
        invalid_records = {
            "empty_version": make_boot_metadata_copy(
                "copy_a",
                7,
                version="",
                hash="a" * 64,
            ),
            "malformed_hash": make_boot_metadata_copy(
                "copy_a",
                7,
                version="1.0.0",
                hash="not-a-sha256",
            ),
            "uppercase_hash": make_boot_metadata_copy(
                "copy_a",
                7,
                version="1.0.0",
                hash="A" * 64,
            ),
            "ambiguous_active_confirmed_owner": make_boot_metadata_copy(
                "copy_a",
                7,
                active_slot="logical_b",
                confirmed_slot="logical_a",
                version="1.0.0",
                hash="a" * 64,
            ),
        }

        for name, invalid in invalid_records.items():
            with self.subTest(name=name, topology="sole"):
                self.assertEqual(
                    select_boot_metadata_copy(
                        metadata,
                        [invalid],
                        flash["trial_boot_max"],
                    ),
                    ("service_recovery_without_image_write", None),
                )
            with self.subTest(name=name, topology="invalid_base_valid_newer"):
                self.assert_selected_copy(
                    select_boot_metadata_copy(
                        metadata,
                        [invalid, valid],
                        flash["trial_boot_max"],
                    ),
                    "copy_b",
                )

    def test_transition_classifier_is_exact_and_reboot_is_reselection_only(self):
        self.assertIn("classify_boot_metadata_transition", globals())
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        candidate_hash = "b" * 64
        identity = {("logical_b", "2.0.0", candidate_hash)}
        base = make_boot_metadata_copy("copy_a", 10)
        staged = make_boot_metadata_copy(
            "copy_b",
            11,
            pending_slot="logical_b",
            version="2.0.0",
            hash=candidate_hash,
        )
        first_attempt = copy.deepcopy(staged)
        first_attempt.update(
            {"copy_id": "copy_a", "sequence": 12, "trial_count": 1}
        )
        second_attempt = copy.deepcopy(first_attempt)
        second_attempt.update(
            {"copy_id": "copy_b", "sequence": 13, "trial_count": 2}
        )
        terminal = make_boot_metadata_copy(
            "copy_a",
            14,
            active_slot="logical_a",
            confirmed_slot="logical_a",
            bad_slot="logical_b",
            trial_count=0,
            version="2.0.0",
            hash=candidate_hash,
            rollback_reason="trial_exhausted",
            failed_candidate_slots=["logical_b"],
        )

        chain = [base, staged, first_attempt, second_attempt, terminal]
        self.assertEqual(
            [record["copy_id"] for record in chain],
            ["copy_a", "copy_b", "copy_a", "copy_b", "copy_a"],
        )
        expected = [
            "stage_verified_candidate",
            "attempt",
            "attempt",
            "fail_or_exhaust",
        ]
        for source, destination, expected_kind in zip(
            chain,
            chain[1:],
            expected,
        ):
            with self.subTest(expected_kind=expected_kind):
                self.assertEqual(
                    classify_boot_metadata_transition(
                        metadata,
                        source,
                        destination,
                        flash["trial_boot_max"],
                        verified_candidate_identities=identity,
                    ),
                    expected_kind,
                )

        higher_sequence_noop = copy.deepcopy(base)
        higher_sequence_noop.update({"copy_id": "copy_b", "sequence": 11})
        self.assertEqual(
            classify_boot_metadata_transition(
                metadata,
                base,
                higher_sequence_noop,
                flash["trial_boot_max"],
            ),
            "invalid",
        )
        restaged = copy.deepcopy(staged)
        restaged.update({"copy_id": "copy_a", "sequence": 12})
        self.assertEqual(
            classify_boot_metadata_transition(
                metadata,
                staged,
                restaged,
                flash["trial_boot_max"],
            ),
            "invalid",
        )
        self.assertEqual(
            classify_boot_metadata_transition(
                metadata,
                staged,
                restaged,
                flash["trial_boot_max"],
                verified_candidate_identities=identity,
            ),
            "restage_same_candidate",
        )
        self.assertFalse(
            boot_metadata_transition_is_valid(
                metadata,
                staged,
                restaged,
                "reboot",
                flash["trial_boot_max"],
                verified_candidate_identities=identity,
            )
        )

        for field, value in [
            ("format_version", 2),
            ("length", 2),
            ("rollback_reason", "unexpected"),
        ]:
            with self.subTest(changed_field=field):
                invalid_attempt = copy.deepcopy(first_attempt)
                invalid_attempt.update(
                    {"copy_id": "copy_b", "sequence": 13, field: value}
                )
                self.assertEqual(
                    classify_boot_metadata_transition(
                        metadata,
                        first_attempt,
                        invalid_attempt,
                        flash["trial_boot_max"],
                    ),
                    "invalid",
                )

        ambiguous_metadata = copy.deepcopy(metadata)
        ambiguous_metadata["candidate_trial_transitions"][
            "transition_classifier"
        ]["journal_transition_kinds"].append("attempt")
        self.assertEqual(
            classify_boot_metadata_transition(
                ambiguous_metadata,
                staged,
                first_attempt,
                flash["trial_boot_max"],
            ),
            "ambiguous",
        )

    def test_boot_decision_separates_pending_and_confirmed_revalidation(self):
        self.assertIn("decide_boot_metadata", globals())
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        candidate_hash = "b" * 64
        identity = ("logical_b", "2.0.0", candidate_hash)
        wrong_identity = ("logical_b", "9.9.9", "c" * 64)
        pending = make_boot_metadata_copy(
            "copy_b",
            11,
            pending_slot="logical_b",
            version="2.0.0",
            hash=candidate_hash,
        )
        torn_source = make_boot_metadata_copy(
            "copy_a",
            10,
            state="torn",
            commit_marker_state="torn",
        )
        invalid_source = make_boot_metadata_copy(
            "copy_a",
            10,
            crc_matches=False,
        )

        for name, copies, pending_evidence in [
            ("sole_missing", [pending], set()),
            ("sole_wrong", [pending], {wrong_identity}),
            ("torn_source_missing", [torn_source, pending], set()),
            ("invalid_source_missing", [invalid_source, pending], set()),
        ]:
            with self.subTest(name=name):
                decision = decide_boot_metadata(
                    metadata,
                    copies,
                    flash["trial_boot_max"],
                    verified_candidate_identities=pending_evidence,
                    revalidated_bootable_slots={"logical_a"},
                )
                self.assertEqual(
                    decision["disposition"],
                    "rollback_required_pending_revalidation_failed_commit_"
                    "then_boot_confirmed",
                )
                self.assertEqual(
                    decision["source_record_reference"].copy_id,
                    "copy_b",
                )
                self.assertEqual(
                    decision["post_commit_action"],
                    "boot_confirmed",
                )

        exact = decide_boot_metadata(
            metadata,
            [pending],
            flash["trial_boot_max"],
            verified_candidate_identities={identity},
            revalidated_bootable_slots={"logical_a"},
        )
        self.assertEqual(
            exact["disposition"],
            "attempt_required_commit_then_boot_pending",
        )
        self.assertEqual(exact["source_record_reference"].copy_id, "copy_b")
        self.assertEqual(exact["next_journal_record"]["trial_count"], 1)
        self.assertEqual(exact["post_commit_action"], "boot_pending")

        confirmed_invalid = decide_boot_metadata(
            metadata,
            [pending],
            flash["trial_boot_max"],
            verified_candidate_identities=set(),
            revalidated_bootable_slots=set(),
        )
        self.assertEqual(
            confirmed_invalid["disposition"],
            "service_recovery_without_image_write",
        )

        no_valid_metadata = decide_boot_metadata(
            metadata,
            [torn_source, invalid_source],
            flash["trial_boot_max"],
            revalidated_bootable_slots={"logical_a"},
        )
        self.assertEqual(
            no_valid_metadata["disposition"],
            "service_recovery_without_image_write",
        )

        metadata_claim = copy.deepcopy(pending)
        metadata_claim["candidate_verified"] = True
        metadata_claim["confirmed_valid"] = True
        claimed = decide_boot_metadata(
            metadata,
            [metadata_claim],
            flash["trial_boot_max"],
        )
        self.assertEqual(
            claimed["disposition"],
            "service_recovery_without_image_write",
        )

        rollback = decide_boot_metadata(
            metadata,
            [pending],
            flash["trial_boot_max"],
            verified_candidate_identities=set(),
            revalidated_bootable_slots={"logical_a"},
        )
        terminal = rollback["next_journal_record"]
        self.assertEqual(
            {
                field: terminal[field]
                for field in metadata["crc"]["semantic_coverage_fields"]
            },
            {
                "format_version": pending["format_version"],
                "length": pending["length"],
                "sequence": pending["sequence"] + 1,
                "active_slot": "logical_a",
                "pending_slot": "none",
                "confirmed_slot": "logical_a",
                "bad_slot": "logical_b",
                "trial_count": 0,
                "version": "2.0.0",
                "hash": candidate_hash,
                "rollback_reason": "pending_revalidation_failed",
            },
        )
        self.assertEqual(
            classify_boot_metadata_transition(
                metadata,
                pending,
                terminal,
                flash["trial_boot_max"],
                revalidated_bootable_slots={"logical_a"},
            ),
            "pending_revalidation_failed",
        )

        torn_terminal = copy.deepcopy(terminal)
        torn_terminal.update(
            {"state": "torn", "commit_marker_state": "torn"}
        )
        retry = decide_boot_metadata(
            metadata,
            [pending, torn_terminal],
            flash["trial_boot_max"],
            revalidated_bootable_slots={"logical_a"},
        )
        self.assertEqual(
            retry["disposition"],
            "rollback_required_pending_revalidation_failed_commit_then_"
            "boot_confirmed",
        )
        self.assertEqual(retry["source_record_reference"].copy_id, "copy_b")

        after_commit = decide_boot_metadata(
            metadata,
            [pending, terminal],
            flash["trial_boot_max"],
            revalidated_bootable_slots={"logical_a"},
        )
        self.assertEqual(after_commit["disposition"], "boot_confirmed")
        self.assertEqual(
            after_commit["source_record_reference"].copy_id,
            terminal["copy_id"],
        )
        self.assertIsNone(after_commit["next_journal_record"])
        self.assertIsNone(after_commit["post_commit_action"])

        same_failed_identity = make_boot_metadata_copy(
            "copy_b",
            terminal["sequence"] + 1,
            pending_slot="logical_b",
            version="2.0.0",
            hash=candidate_hash,
        )
        self.assertEqual(
            classify_boot_metadata_transition(
                metadata,
                terminal,
                same_failed_identity,
                flash["trial_boot_max"],
                verified_candidate_identities={identity},
            ),
            "invalid",
        )

    def test_boot_entry_requires_attempt_commit_or_exhaustion_terminal(self):
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        candidate_hash = "b" * 64
        identity = ("logical_b", "2.0.0", candidate_hash)
        expected_by_count = {
            0: ("attempt_required_commit_then_boot_pending", 1, "none"),
            1: ("attempt_required_commit_then_boot_pending", 2, "none"),
            2: (
                "rollback_required_trial_exhausted_commit_then_boot_confirmed",
                0,
                "trial_exhausted",
            ),
        }
        for count, expected in expected_by_count.items():
            with self.subTest(trial_count=count):
                source = make_boot_metadata_copy(
                    "copy_a",
                    10,
                    pending_slot="logical_b",
                    trial_count=count,
                    version="2.0.0",
                    hash=candidate_hash,
                )
                decision = decide_boot_metadata(
                    metadata,
                    [source],
                    flash["trial_boot_max"],
                    verified_candidate_identities={identity},
                    revalidated_bootable_slots={"logical_a"},
                )
                disposition, next_count, reason = expected
                self.assertEqual(decision["disposition"], disposition)
                self.assertEqual(
                    set(decision),
                    {
                        "disposition",
                        "source_record_reference",
                        "next_journal_record",
                        "post_commit_action",
                    },
                )
                next_record = decision["next_journal_record"]
                self.assertEqual(next_record["copy_id"], "copy_b")
                self.assertEqual(next_record["sequence"], 11)
                self.assertEqual(next_record["trial_count"], next_count)
                self.assertEqual(next_record["rollback_reason"], reason)
                self.assertEqual(
                    decision["post_commit_action"],
                    "boot_pending" if count < 2 else "boot_confirmed",
                )

    def test_copy_topology_and_selected_record_reference_fail_closed(self):
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        self.assertNotIn(
            "verified_candidate_identities",
            inspect.signature(select_boot_metadata_copy).parameters,
        )
        older = make_boot_metadata_copy("copy_a", 10)
        duplicate_torn = make_boot_metadata_copy(
            "copy_a",
            11,
            state="torn",
            commit_marker_state="torn",
            pending_slot="logical_b",
            version="2.0.0",
            hash="b" * 64,
        )
        duplicate_valid = make_boot_metadata_copy("copy_a", 11)
        unknown = make_boot_metadata_copy("copy_x", 10)
        for name, copies in [
            ("duplicate_with_torn", [older, duplicate_torn]),
            ("duplicate_both_valid", [older, duplicate_valid]),
            ("unknown_copy_id", [unknown]),
        ]:
            with self.subTest(name=name):
                self.assertEqual(
                    select_boot_metadata_copy(
                        metadata,
                        copies,
                        flash["trial_boot_max"],
                    ),
                    ("service_recovery_without_image_write", None),
                )

        for copy_id in ["copy_a", "copy_b"]:
            with self.subTest(copy_id=copy_id):
                source = make_boot_metadata_copy(copy_id, 10)
                status, reference = select_boot_metadata_copy(
                    metadata,
                    [source],
                    flash["trial_boot_max"],
                )
                self.assertEqual(status, "selected")
                self.assertTrue(hasattr(reference, "copy_id"))
                self.assertEqual(reference.copy_id, copy_id)
                self.assertEqual(reference.sequence, 10)
                self.assertEqual(
                    reference.canonical_semantic_projection,
                    boot_metadata_semantic_projection(metadata, source),
                )
                with self.assertRaises(AttributeError):
                    reference.copy_id = "copy_x"

    def test_pending_revalidation_failure_requires_exact_identity_absence(self):
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        candidate_hash = "b" * 64
        identity = ("logical_b", "2.0.0", candidate_hash)
        wrong_identity = ("logical_b", "9.9.9", "c" * 64)
        for count in [0, 1, 2]:
            source = make_boot_metadata_copy(
                "copy_a",
                10,
                pending_slot="logical_b",
                trial_count=count,
                version="2.0.0",
                hash=candidate_hash,
            )
            terminal = make_boot_metadata_copy(
                "copy_b",
                11,
                bad_slot="logical_b",
                version="2.0.0",
                hash=candidate_hash,
                rollback_reason="pending_revalidation_failed",
                failed_candidate_slots=["logical_b"],
            )
            for name, evidence, expected in [
                ("missing", set(), "pending_revalidation_failed"),
                ("wrong", {wrong_identity}, "pending_revalidation_failed"),
                ("exact", {identity}, "invalid"),
            ]:
                with self.subTest(trial_count=count, evidence=name):
                    self.assertEqual(
                        classify_boot_metadata_transition(
                            metadata,
                            source,
                            terminal,
                            flash["trial_boot_max"],
                            verified_candidate_identities=evidence,
                            revalidated_bootable_slots={"logical_a"},
                        ),
                        expected,
                    )

    def test_individual_rollback_reason_state_applies_to_sole_record(self):
        self.assertIn("rollback_reason_state_is_valid", globals())
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        invalid_records = {
            "arbitrary": make_boot_metadata_copy(
                "copy_a", 1, rollback_reason="arbitrary"
            ),
            "blank": make_boot_metadata_copy(
                "copy_a", 1, rollback_reason=""
            ),
            "reason_without_bad": make_boot_metadata_copy(
                "copy_a", 1, rollback_reason="trial_exhausted"
            ),
            "bad_without_reason": make_boot_metadata_copy(
                "copy_a",
                1,
                bad_slot="logical_b",
                failed_candidate_slots=["logical_b"],
            ),
            "pending_with_failure_reason": make_boot_metadata_copy(
                "copy_a",
                1,
                pending_slot="logical_b",
                rollback_reason="explicit_trial_failure",
            ),
            "confirmed_with_nonzero_count": make_boot_metadata_copy(
                "copy_a", 1, trial_count=2
            ),
        }
        for name, record in invalid_records.items():
            with self.subTest(name=name):
                self.assertFalse(
                    rollback_reason_state_is_valid(
                        metadata,
                        record,
                        flash["trial_boot_max"],
                    )
                )
                self.assertEqual(
                    select_boot_metadata_copy(
                        metadata,
                        [record],
                        flash["trial_boot_max"],
                    ),
                    ("service_recovery_without_image_write", None),
                )

    def test_transition_sequence_and_copy_topology_are_exact(self):
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        candidate_hash = "b" * 64
        identity = {("logical_b", "2.0.0", candidate_hash)}
        source = make_boot_metadata_copy("copy_a", 10)
        valid = make_boot_metadata_copy(
            "copy_b",
            11,
            pending_slot="logical_b",
            version="2.0.0",
            hash=candidate_hash,
        )
        self.assertEqual(
            classify_boot_metadata_transition(
                metadata,
                source,
                valid,
                flash["trial_boot_max"],
                verified_candidate_identities=identity,
            ),
            "stage_verified_candidate",
        )
        invalid_destinations = {
            "sequence_gap": {"copy_id": "copy_b", "sequence": 12},
            "sequence_rollback": {"copy_id": "copy_b", "sequence": 9},
            "same_sequence": {"copy_id": "copy_b", "sequence": 10},
            "same_copy": {"copy_id": "copy_a", "sequence": 11},
            "unknown_copy": {"copy_id": "copy_x", "sequence": 11},
        }
        for name, updates in invalid_destinations.items():
            with self.subTest(name=name):
                destination = copy.deepcopy(valid)
                destination.update(updates)
                self.assertEqual(
                    classify_boot_metadata_transition(
                        metadata,
                        source,
                        destination,
                        flash["trial_boot_max"],
                        verified_candidate_identities=identity,
                    ),
                    "invalid",
                )

    def test_attempt_and_terminal_power_cuts_repeat_required_commit(self):
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        candidate_hash = "b" * 64
        identity = {("logical_b", "2.0.0", candidate_hash)}

        pending_zero = make_boot_metadata_copy(
            "copy_a",
            10,
            pending_slot="logical_b",
            trial_count=0,
            version="2.0.0",
            hash=candidate_hash,
        )
        first = decide_boot_metadata(
            metadata,
            [pending_zero],
            flash["trial_boot_max"],
            verified_candidate_identities=identity,
            revalidated_bootable_slots={"logical_a"},
        )
        self.assertEqual(
            first["disposition"],
            "attempt_required_commit_then_boot_pending",
        )
        self.assertEqual(first["source_record_reference"].copy_id, "copy_a")
        first_record = first["next_journal_record"]
        self.assertEqual(first_record["copy_id"], "copy_b")
        self.assertEqual(first_record["sequence"], 11)
        torn_first = copy.deepcopy(first_record)
        torn_first.update({"state": "torn", "commit_marker_state": "torn"})
        retry_first = decide_boot_metadata(
            metadata,
            [pending_zero, torn_first],
            flash["trial_boot_max"],
            verified_candidate_identities=identity,
            revalidated_bootable_slots={"logical_a"},
        )
        self.assertEqual(
            retry_first["next_journal_record"]["trial_count"], 1
        )

        second = decide_boot_metadata(
            metadata,
            [pending_zero, first_record],
            flash["trial_boot_max"],
            verified_candidate_identities=identity,
            revalidated_bootable_slots={"logical_a"},
        )
        second_record = second["next_journal_record"]
        self.assertEqual(
            second["disposition"],
            "attempt_required_commit_then_boot_pending",
        )
        self.assertEqual(second["source_record_reference"].copy_id, "copy_b")
        self.assertEqual(second_record["copy_id"], "copy_a")
        self.assertEqual(second_record["sequence"], 12)
        self.assertEqual(second_record["trial_count"], 2)

        exhausted = decide_boot_metadata(
            metadata,
            [first_record, second_record],
            flash["trial_boot_max"],
            verified_candidate_identities=identity,
            revalidated_bootable_slots={"logical_a"},
        )
        terminal = exhausted["next_journal_record"]
        self.assertEqual(
            exhausted["disposition"],
            "rollback_required_trial_exhausted_commit_then_boot_confirmed",
        )
        self.assertEqual(
            exhausted["source_record_reference"].copy_id,
            "copy_a",
        )
        self.assertEqual(terminal["copy_id"], "copy_b")
        self.assertEqual(terminal["sequence"], 13)
        self.assertEqual(terminal["rollback_reason"], "trial_exhausted")
        torn_terminal = copy.deepcopy(terminal)
        torn_terminal.update(
            {"state": "uncommitted", "commit_marker_state": "erased"}
        )
        retry_terminal = decide_boot_metadata(
            metadata,
            [second_record, torn_terminal],
            flash["trial_boot_max"],
            verified_candidate_identities=identity,
            revalidated_bootable_slots={"logical_a"},
        )
        self.assertEqual(
            retry_terminal["disposition"],
            "rollback_required_trial_exhausted_commit_then_boot_confirmed",
        )
        self.assertEqual(
            retry_terminal["next_journal_record"]["rollback_reason"],
            "trial_exhausted",
        )

    def test_selected_reference_is_the_record_used_by_decision(self):
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        valid = make_boot_metadata_copy("copy_a", 10)
        torn = make_boot_metadata_copy(
            "copy_b",
            11,
            state="torn",
            commit_marker_state="torn",
            pending_slot="logical_b",
            version="2.0.0",
            hash="b" * 64,
        )
        status, reference = select_boot_metadata_copy(
            metadata,
            [valid, torn],
            flash["trial_boot_max"],
        )
        self.assertEqual(status, "selected")
        self.assertTrue(hasattr(reference, "copy_id"))
        self.assertEqual(reference.copy_id, "copy_a")
        self.assertEqual(reference.sequence, 10)
        decision = decide_boot_metadata(
            metadata,
            [valid, torn],
            flash["trial_boot_max"],
            revalidated_bootable_slots={"logical_a"},
        )
        self.assertEqual(decision["disposition"], "boot_confirmed")
        self.assertEqual(decision["source_record_reference"], reference)
        self.assertIsNone(decision["next_journal_record"])

        duplicate = copy.deepcopy(torn)
        duplicate["copy_id"] = "copy_a"
        duplicate_decision = decide_boot_metadata(
            metadata,
            [valid, duplicate],
            flash["trial_boot_max"],
            verified_candidate_identities={
                ("logical_b", "2.0.0", "b" * 64)
            },
            revalidated_bootable_slots={"logical_a"},
        )
        self.assertEqual(
            duplicate_decision,
            {
                "disposition": "service_recovery_without_image_write",
                "source_record_reference": None,
                "next_journal_record": None,
                "post_commit_action": None,
            },
        )

    def test_version_identity_rejects_blank_without_normalization(self):
        self.assertNotIn(
            "semantic_content",
            inspect.signature(make_boot_metadata_copy).parameters,
        )
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        for version in ["", " ", "\t\n", "\u00a0"]:
            with self.subTest(version=repr(version)):
                record = make_boot_metadata_copy("blank", 1, version=version)
                self.assertFalse(
                    candidate_identity_shape_is_valid(metadata, record)
                )

        for version in [" 2.0.0 ", "２.０.０"]:
            with self.subTest(version=version):
                record = make_boot_metadata_copy("exact", 1, version=version)
                original = record["version"]
                self.assertTrue(
                    candidate_identity_shape_is_valid(metadata, record)
                )
                self.assertEqual(record["version"], original)

        padded = make_boot_metadata_copy(
            "copy_b",
            2,
            pending_slot="logical_b",
            version=" 2.0.0 ",
            hash="b" * 64,
        )
        exact = decide_boot_metadata(
            metadata,
            [padded],
            flash["trial_boot_max"],
            verified_candidate_identities={
                ("logical_b", " 2.0.0 ", "b" * 64)
            },
            revalidated_bootable_slots={"logical_a"},
        )
        normalized_mismatch = decide_boot_metadata(
            metadata,
            [padded],
            flash["trial_boot_max"],
            verified_candidate_identities={
                ("logical_b", "2.0.0", "b" * 64)
            },
            revalidated_bootable_slots={"logical_a"},
        )
        self.assertEqual(
            exact["disposition"],
            "attempt_required_commit_then_boot_pending",
        )
        self.assertEqual(
            normalized_mismatch["disposition"],
            "rollback_required_pending_revalidation_failed_commit_then_"
            "boot_confirmed",
        )

    def test_metadata_selection_precedes_current_pending_revalidation(self):
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        candidate_hash = "b" * 64
        legacy = make_boot_metadata_copy(
            "copy_a",
            10,
            pending_slot="logical_b",
            trial_count=2,
        )
        staged = make_boot_metadata_copy(
            "copy_b",
            11,
            pending_slot="logical_b",
            version="2.0.0",
            hash=candidate_hash,
        )
        metadata_claim = copy.deepcopy(staged)
        metadata_claim["candidate_verified"] = True

        for name, destination in [
            ("no_evidence", staged),
        ]:
            with self.subTest(name=name):
                self.assert_selected_copy(
                    select_boot_metadata_copy(
                        metadata,
                        [legacy, destination],
                        flash["trial_boot_max"],
                    ),
                    "copy_b",
                )
        self.assert_selected_copy(
            select_boot_metadata_copy(
                metadata,
                [legacy, metadata_claim],
                flash["trial_boot_max"],
            ),
            "copy_a",
        )

    def test_pending_selection_is_independent_of_current_revalidation(self):
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        candidate_hash = "b" * 64
        candidate_identity = ("logical_b", "2.0.0", candidate_hash)
        wrong_identity = ("logical_b", "9.9.9", "c" * 64)
        pending = make_boot_metadata_copy(
            "copy_b",
            11,
            pending_slot="logical_b",
            version="2.0.0",
            hash=candidate_hash,
        )
        metadata_claim = copy.deepcopy(pending)
        metadata_claim["candidate_verified"] = True
        torn_source = make_boot_metadata_copy(
            "copy_a",
            10,
            state="torn",
            commit_marker_state="torn",
        )
        invalid_source = make_boot_metadata_copy(
            "copy_a",
            10,
            crc_matches=False,
        )

        selection_cases = [
            ("sole_no_evidence", [pending]),
            ("torn_source_no_evidence", [torn_source, pending]),
            ("invalid_source_wrong_identity", [invalid_source, pending]),
        ]
        for name, copies in selection_cases:
            with self.subTest(name=name):
                self.assert_selected_copy(
                    select_boot_metadata_copy(
                        metadata,
                        copies,
                        flash["trial_boot_max"],
                    ),
                    "copy_b",
                )
        self.assertEqual(
            select_boot_metadata_copy(
                metadata,
                [metadata_claim],
                flash["trial_boot_max"],
            ),
            ("service_recovery_without_image_write", None),
        )

        for name, copies in [
            ("sole_exact_evidence", [pending]),
            ("torn_source_exact_evidence", [torn_source, pending]),
        ]:
            with self.subTest(name=name):
                self.assert_selected_copy(
                    select_boot_metadata_copy(
                        metadata,
                        copies,
                        flash["trial_boot_max"],
                    ),
                    "copy_b",
                )

        bad_recovery = make_boot_metadata_copy(
            "copy_a",
            12,
            bad_slot="logical_b",
            rollback_reason="explicit_trial_failure",
            failed_candidate_slots=["logical_b"],
        )
        self.assert_selected_copy(
            select_boot_metadata_copy(
                metadata,
                [bad_recovery],
                flash["trial_boot_max"],
            ),
            "copy_a",
        )

    def test_recomputed_verification_evidence_matches_exact_candidate_identity(self):
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        self.assertNotIn(
            "verified_candidate_identities",
            inspect.signature(select_boot_metadata_copy).parameters,
        )
        self.assertIn(
            "verified_candidate_identities",
            inspect.signature(boot_metadata_transition_is_valid).parameters,
        )
        candidate_hash = "b" * 64
        candidate_identity = ("logical_b", "2.0.0", candidate_hash)
        other_identity = ("logical_b", "9.9.9", "c" * 64)
        legacy = make_boot_metadata_copy(
            "copy_a",
            10,
            pending_slot="logical_b",
            trial_count=2,
        )
        staged = make_boot_metadata_copy(
            "copy_b",
            11,
            pending_slot="logical_b",
            version="2.0.0",
            hash=candidate_hash,
        )

        self.assert_selected_copy(
            select_boot_metadata_copy(
                metadata,
                [legacy, staged],
                flash["trial_boot_max"],
            ),
            "copy_b",
        )
        self.assert_selected_copy(
            select_boot_metadata_copy(
                metadata,
                [legacy, staged],
                flash["trial_boot_max"],
            ),
            "copy_b",
        )
        self.assertTrue(
            boot_metadata_transition_is_valid(
                metadata,
                legacy,
                staged,
                "stage_verified_candidate",
                flash["trial_boot_max"],
                verified_candidate_identities={candidate_identity},
            )
        )
        self.assertFalse(
            boot_metadata_transition_is_valid(
                metadata,
                legacy,
                staged,
                "stage_verified_candidate",
                flash["trial_boot_max"],
                verified_candidate_identities={other_identity},
            )
        )

    def test_new_candidate_staging_and_first_attempt_transition_model(self):
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        self.assertIn("candidate_trial_transitions", metadata)
        self.assertNotIn(
            "verified_candidate_identities",
            inspect.signature(select_boot_metadata_copy).parameters,
        )
        self.assertIn(
            "verified_candidate_identities",
            inspect.signature(boot_metadata_transition_is_valid).parameters,
        )
        old_hash = "a" * 64
        new_hash = "b" * 64
        new_identity = {("logical_b", "2.0.0", new_hash)}

        legacy = make_boot_metadata_copy(
            "copy_a",
            10,
            pending_slot="logical_b",
            trial_count=2,
            version="1.0.0",
            hash=old_hash,
        )
        staged = make_boot_metadata_copy(
            "copy_b",
            11,
            pending_slot="logical_b",
            trial_count=0,
            version="2.0.0",
            hash=new_hash,
        )
        stale_count_stage = copy.deepcopy(staged)
        stale_count_stage["trial_count"] = 2

        self.assertTrue(
            slot_state_invariants_are_valid(
                metadata,
                legacy,
                flash["trial_boot_max"],
            )
        )
        self.assertTrue(
            slot_state_invariants_are_valid(
                metadata,
                stale_count_stage,
                flash["trial_boot_max"],
            ),
            "destination shape alone intentionally remains valid",
        )
        self.assertTrue(
            boot_metadata_transition_is_valid(
                metadata,
                legacy,
                staged,
                "stage_verified_candidate",
                flash["trial_boot_max"],
                verified_candidate_identities=new_identity,
            )
        )
        self.assertFalse(
            boot_metadata_transition_is_valid(
                metadata,
                legacy,
                stale_count_stage,
                "stage_verified_candidate",
                flash["trial_boot_max"],
                verified_candidate_identities=new_identity,
            )
        )
        self.assertFalse(
            boot_metadata_transition_is_valid(
                metadata,
                legacy,
                staged,
                "simple_field_mutation",
                flash["trial_boot_max"],
                verified_candidate_identities=new_identity,
            )
        )
        self.assert_selected_copy(
            select_boot_metadata_copy(
                metadata,
                [legacy, staged],
                flash["trial_boot_max"],
            ),
            "copy_b",
        )
        self.assert_selected_copy(
            select_boot_metadata_copy(
                metadata,
                [legacy, stale_count_stage],
                flash["trial_boot_max"],
            ),
            "copy_a",
        )

        first_attempt = copy.deepcopy(staged)
        first_attempt.update(
            {
                "copy_id": "copy_a",
                "sequence": 12,
                "trial_count": 1,
            }
        )
        self.assertTrue(
            boot_metadata_transition_is_valid(
                metadata,
                staged,
                first_attempt,
                "attempt",
                flash["trial_boot_max"],
            )
        )
        second_attempt = copy.deepcopy(first_attempt)
        second_attempt.update(
            {
                "copy_id": "copy_b",
                "sequence": 13,
                "trial_count": 2,
            }
        )
        self.assertTrue(
            boot_metadata_transition_is_valid(
                metadata,
                first_attempt,
                second_attempt,
                "attempt",
                flash["trial_boot_max"],
            )
        )
        self.assertEqual(
            metadata["candidate_trial_transitions"][
                "verified_new_candidate_staging"
            ]["first_boot_transition"],
            "durably_commit_0_to_1_then_boot_pending",
        )

    def test_same_candidate_and_bad_identity_transition_model(self):
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        self.assertIn("candidate_trial_transitions", metadata)
        self.assertNotIn(
            "verified_candidate_identities",
            inspect.signature(select_boot_metadata_copy).parameters,
        )
        self.assertIn(
            "verified_candidate_identities",
            inspect.signature(boot_metadata_transition_is_valid).parameters,
        )
        candidate_hash = "b" * 64
        pending_identity = {("logical_b", "2.0.0", candidate_hash)}

        pending = make_boot_metadata_copy(
            "copy_a",
            20,
            pending_slot="logical_b",
            trial_count=1,
            version="2.0.0",
            hash=candidate_hash,
        )
        reset = copy.deepcopy(pending)
        reset.update({"copy_id": "copy_b", "sequence": 21, "trial_count": 0})
        preserved = copy.deepcopy(reset)
        preserved["trial_count"] = 1

        self.assertFalse(
            boot_metadata_transition_is_valid(
                metadata,
                pending,
                reset,
                "restage_same_candidate",
                flash["trial_boot_max"],
                verified_candidate_identities=pending_identity,
            )
        )
        self.assert_selected_copy(
            select_boot_metadata_copy(
                metadata,
                [pending, reset],
                flash["trial_boot_max"],
            ),
            "copy_a",
        )
        self.assertTrue(
            boot_metadata_transition_is_valid(
                metadata,
                pending,
                preserved,
                "restage_same_candidate",
                flash["trial_boot_max"],
                verified_candidate_identities=pending_identity,
            )
        )
        self.assert_selected_copy(
            select_boot_metadata_copy(
                metadata,
                [pending, preserved],
                flash["trial_boot_max"],
            ),
            "copy_b",
        )
        self.assertFalse(
            boot_metadata_transition_is_valid(
                metadata,
                pending,
                copy.deepcopy(pending),
                "reboot",
                flash["trial_boot_max"],
            )
        )

        bad = make_boot_metadata_copy(
            "copy_a",
            30,
            bad_slot="logical_b",
            trial_count=0,
            version="2.0.0",
            hash=candidate_hash,
            rollback_reason="explicit_trial_failure",
            failed_candidate_slots=["logical_b"],
        )
        same_bad_restage = make_boot_metadata_copy(
            "copy_b",
            31,
            pending_slot="logical_b",
            trial_count=0,
            version="2.0.0",
            hash=candidate_hash,
        )
        different_verified_restage = copy.deepcopy(same_bad_restage)
        different_verified_restage.update(
            {
                "version": "3.0.0",
                "hash": "c" * 64,
            }
        )
        self.assertFalse(
            boot_metadata_transition_is_valid(
                metadata,
                bad,
                same_bad_restage,
                "stage_verified_candidate",
                flash["trial_boot_max"],
                verified_candidate_identities=pending_identity,
            )
        )
        self.assertTrue(
            boot_metadata_transition_is_valid(
                metadata,
                bad,
                different_verified_restage,
                "stage_verified_candidate",
                flash["trial_boot_max"],
                verified_candidate_identities={
                    ("logical_b", "3.0.0", "c" * 64)
                },
            )
        )
        self.assert_selected_copy(
            select_boot_metadata_copy(
                metadata,
                [bad, same_bad_restage],
                flash["trial_boot_max"],
            ),
            "copy_a",
        )
        self.assert_selected_copy(
            select_boot_metadata_copy(
                metadata,
                [bad, different_verified_restage],
                flash["trial_boot_max"],
            ),
            "copy_b",
        )

    def test_candidate_terminal_and_power_cut_transition_model(self):
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        self.assertIn("candidate_trial_transitions", metadata)
        candidate_hash = "b" * 64

        pending_once = make_boot_metadata_copy(
            "copy_a",
            40,
            pending_slot="logical_b",
            trial_count=1,
            version="2.0.0",
            hash=candidate_hash,
        )
        confirmed = make_boot_metadata_copy(
            "copy_b",
            41,
            active_slot="logical_b",
            confirmed_slot="logical_b",
            trial_count=0,
            version="2.0.0",
            hash=candidate_hash,
        )
        invalid_confirm = copy.deepcopy(confirmed)
        invalid_confirm["trial_count"] = 1
        self.assertTrue(
            boot_metadata_transition_is_valid(
                metadata,
                pending_once,
                confirmed,
                "confirm",
                flash["trial_boot_max"],
            )
        )
        self.assert_selected_copy(
            select_boot_metadata_copy(
                metadata,
                [pending_once, confirmed],
                flash["trial_boot_max"],
            ),
            "copy_b",
        )
        self.assertFalse(
            boot_metadata_transition_is_valid(
                metadata,
                pending_once,
                invalid_confirm,
                "confirm",
                flash["trial_boot_max"],
            )
        )
        pending_exhausted = copy.deepcopy(pending_once)
        pending_exhausted.update(
            {
                "copy_id": "copy_a",
                "sequence": 50,
                "trial_count": 2,
            }
        )
        failed = make_boot_metadata_copy(
            "copy_b",
            51,
            bad_slot="logical_b",
            trial_count=0,
            version="2.0.0",
            hash=candidate_hash,
            rollback_reason="trial_exhausted",
            failed_candidate_slots=["logical_b"],
        )
        invalid_failure = copy.deepcopy(failed)
        invalid_failure.update(
            {"bad_slot": "none", "failed_candidate_slots": []}
        )
        self.assertTrue(
            boot_metadata_transition_is_valid(
                metadata,
                pending_exhausted,
                failed,
                "fail_or_exhaust",
                flash["trial_boot_max"],
            )
        )
        self.assertFalse(
            boot_metadata_transition_is_valid(
                metadata,
                pending_exhausted,
                invalid_failure,
                "fail_or_exhaust",
                flash["trial_boot_max"],
            )
        )
        self.assert_selected_copy(
            select_boot_metadata_copy(
                metadata,
                [pending_exhausted, failed],
                flash["trial_boot_max"],
            ),
            "copy_b",
        )

        legacy = make_boot_metadata_copy(
            "copy_a",
            60,
            pending_slot="logical_b",
            trial_count=2,
        )
        staged_torn = make_boot_metadata_copy(
            "copy_b",
            61,
            state="uncommitted",
            pending_slot="logical_b",
            version="2.0.0",
            hash=candidate_hash,
        )
        attempt_torn = copy.deepcopy(pending_once)
        attempt_torn.update(
            {
                "copy_id": "copy_b",
                "sequence": 41,
                "state": "torn",
                "trial_count": 2,
            }
        )
        confirm_torn = copy.deepcopy(confirmed)
        confirm_torn["state"] = "uncommitted"
        rollback_torn = copy.deepcopy(failed)
        rollback_torn["state"] = "torn"

        power_cut_cases = [
            ("staging", legacy, staged_torn),
            ("attempt", pending_once, attempt_torn),
            ("confirm", pending_once, confirm_torn),
            ("rollback", pending_exhausted, rollback_torn),
        ]
        for name, previous, torn in power_cut_cases:
            with self.subTest(name=name):
                self.assert_selected_copy(
                    select_boot_metadata_copy(
                        metadata,
                        [previous, torn],
                        flash["trial_boot_max"],
                    ),
                    previous["copy_id"],
                )

    def test_boot_metadata_copy_selection_model(self):
        flash = self.data["flash_fota"]
        self.assertIn("boot_metadata", flash)
        metadata = flash["boot_metadata"]

        cases = [
            (
                "higher sequence no-op loses to older valid",
                [
                    make_boot_metadata_copy("copy_a", 7,),
                    make_boot_metadata_copy("copy_b", 8,),
                ],
                ("selected", "copy_a"),
            ),
            (
                "newer torn loses to older valid",
                [
                    make_boot_metadata_copy("copy_a", 7,),
                    make_boot_metadata_copy(
                        "copy_b", 8, state="torn"
                    ),
                ],
                ("selected", "copy_a"),
            ),
            (
                "crc invalid loses",
                [
                    make_boot_metadata_copy("copy_a", 7,),
                    make_boot_metadata_copy(
                        "copy_b",
                        8,
                        state="crc_invalid",
                        crc_matches=False,
                    ),
                ],
                ("selected", "copy_a"),
            ),
            (
                "partial commit marker loses to older valid",
                [
                    make_boot_metadata_copy("copy_a", 7,),
                    make_boot_metadata_copy(
                        "copy_b",
                        8,
                        commit_marker_state="partial",
                    ),
                ],
                ("selected", "copy_a"),
            ),
            (
                "torn commit marker loses to older valid",
                [
                    make_boot_metadata_copy("copy_a", 7,),
                    make_boot_metadata_copy(
                        "copy_b",
                        8,
                        commit_marker_state="torn",
                    ),
                ],
                ("selected", "copy_a"),
            ),
            (
                "confirmed bad slot combination loses to older valid",
                [
                    make_boot_metadata_copy("copy_a", 7,),
                    make_boot_metadata_copy(
                        "copy_b",
                        8,
                        active_slot="logical_b",
                        confirmed_slot="logical_b",
                        bad_slot="logical_b",
                        failed_candidate_slots=["logical_b"],
                    ),
                ],
                ("selected", "copy_a"),
            ),
            (
                "pending trial equal to confirmed rollback loses",
                [
                    make_boot_metadata_copy("copy_a", 7,),
                    make_boot_metadata_copy(
                        "copy_b",
                        8,
                        active_slot="logical_b",
                        confirmed_slot="logical_a",
                        pending_slot="logical_a",
                    ),
                ],
                ("selected", "copy_a"),
            ),
            (
                "nonfailed bad candidate loses to older valid",
                [
                    make_boot_metadata_copy("copy_a", 7,),
                    make_boot_metadata_copy(
                        "copy_b",
                        8,
                        bad_slot="logical_b",
                    ),
                ],
                ("selected", "copy_a"),
            ),
            (
                "numeric slot code loses to older valid",
                [
                    make_boot_metadata_copy("copy_a", 7,),
                    make_boot_metadata_copy(
                        "copy_b",
                        8,
                        active_slot=1,
                        valid_firmware_slots=[1, "logical_a"],
                    ),
                ],
                ("selected", "copy_a"),
            ),
            (
                "active bad slot combination loses to older valid",
                [
                    make_boot_metadata_copy("copy_a", 7,),
                    make_boot_metadata_copy(
                        "copy_b",
                        8,
                        active_slot="logical_b",
                        confirmed_slot="logical_a",
                        bad_slot="logical_b",
                        failed_candidate_slots=["logical_b"],
                    ),
                ],
                ("selected", "copy_a"),
            ),
            (
                "pending bad slot combination loses to older valid",
                [
                    make_boot_metadata_copy("copy_a", 7,),
                    make_boot_metadata_copy(
                        "copy_b",
                        8,
                        pending_slot="logical_b",
                        bad_slot="logical_b",
                        failed_candidate_slots=["logical_b"],
                    ),
                ],
                ("selected", "copy_a"),
            ),
            (
                "negative trial count loses to older valid",
                [
                    make_boot_metadata_copy("copy_a", 7,),
                    make_boot_metadata_copy(
                        "copy_b", 8, trial_count=-1
                    ),
                ],
                ("selected", "copy_a"),
            ),
            (
                "boolean trial count loses to older valid",
                [
                    make_boot_metadata_copy("copy_a", 7,),
                    make_boot_metadata_copy(
                        "copy_b", 8, trial_count=True
                    ),
                ],
                ("selected", "copy_a"),
            ),
            (
                "trial count overflow loses to older valid",
                [
                    make_boot_metadata_copy("copy_a", 7,),
                    make_boot_metadata_copy(
                        "copy_b", 8, trial_count=3
                    ),
                ],
                ("selected", "copy_a"),
            ),
            (
                "maximum trial count historical restage is structurally valid",
                [
                    make_boot_metadata_copy(
                        "copy_a",
                        7,
                        pending_slot="logical_b",
                        trial_count=2,
                    ),
                    make_boot_metadata_copy(
                        "copy_b",
                        8,
                        pending_slot="logical_b",
                        trial_count=2,
                    ),
                ],
                ("selected", "copy_b"),
            ),
            (
                "equal sequence identical content is acceptable",
                [
                    make_boot_metadata_copy("copy_a", 8,),
                    make_boot_metadata_copy("copy_b", 8,),
                ],
                ("selected", "copy_a"),
            ),
            (
                "equal sequence conflicting content fails safe",
                [
                    make_boot_metadata_copy("copy_a", 8,),
                    make_boot_metadata_copy(
                        "copy_b",
                        8,
                        active_slot="logical_b",
                        confirmed_slot="logical_b",
                        version="2.0.0",
                        hash="b" * 64,
                    ),
                ],
                ("service_recovery_without_image_write", None),
            ),
            (
                "no valid copy enters service recovery",
                [
                    make_boot_metadata_copy(
                        "copy_a", 7, state="uncommitted"
                    ),
                    make_boot_metadata_copy(
                        "copy_b", 8, state="torn"
                    ),
                ],
                ("service_recovery_without_image_write", None),
            ),
            (
                "no trial-valid copy enters service recovery",
                [
                    make_boot_metadata_copy(
                        "copy_a", 7, trial_count=-1
                    ),
                    make_boot_metadata_copy(
                        "copy_b", 8, trial_count=3
                    ),
                ],
                ("service_recovery_without_image_write", None),
            ),
        ]

        for name, copies, expected in cases:
            with self.subTest(name=name):
                result = select_boot_metadata_copy(
                    metadata,
                    copies,
                    flash["trial_boot_max"],
                )
                if expected[0] == "selected":
                    self.assert_selected_copy(result, expected[1])
                else:
                    self.assertEqual(result, expected)

    def test_boot_metadata_next_boot_action_model(self):
        flash = self.data["flash_fota"]
        metadata = flash["boot_metadata"]
        invariants = metadata["slot_state_invariants"]
        self.assertIn("trial_count", invariants)
        self.assertIn("next_boot_actions", invariants)
        actions = invariants["next_boot_actions"]

        def next_boot_action(record):
            if not slot_state_invariants_are_valid(
                metadata,
                record,
                flash["trial_boot_max"],
            ):
                return actions["impossible_or_trial_overflow"]["record_result"]
            if record["bad_slot"] != invariants["none_value"]:
                return actions["failed_bad_candidate"]["action"]
            if record["pending_slot"] == invariants["none_value"]:
                return actions["confirmed_no_pending_no_bad"]["action"]
            if record["trial_count"] < flash["trial_boot_max"]:
                return actions["valid_pending_trial"]["action"]
            return actions["trial_exhausted_pending"]["action"]

        cases = [
            (
                "confirmed active",
                make_boot_metadata_copy("copy_a", 1,),
                "boot_active",
            ),
            (
                "first pending trial",
                make_boot_metadata_copy(
                    "copy_a",
                    1,
                    pending_slot="logical_b",
                    trial_count=0,
                ),
                "durably_increment_trial_count_then_boot_pending",
            ),
            (
                "second pending trial",
                make_boot_metadata_copy(
                    "copy_a",
                    1,
                    pending_slot="logical_b",
                    trial_count=1,
                ),
                "durably_increment_trial_count_then_boot_pending",
            ),
            (
                "exhausted pending trial",
                make_boot_metadata_copy(
                    "copy_a",
                    1,
                    pending_slot="logical_b",
                    trial_count=2,
                ),
                "mark_pending_bad_then_rollback_to_confirmed",
            ),
            (
                "failed candidate rollback",
                make_boot_metadata_copy(
                    "copy_a",
                    1,
                    bad_slot="logical_b",
                    failed_candidate_slots=["logical_b"],
                ),
                "rollback_to_confirmed",
            ),
            (
                "trial overflow is invalid",
                make_boot_metadata_copy(
                    "copy_a", 1, trial_count=3
                ),
                "invalid",
            ),
        ]

        for name, record, expected in cases:
            with self.subTest(name=name):
                self.assertEqual(next_boot_action(record), expected)

    def test_boot_metadata_commit_trial_and_deadline_model(self):
        flash = self.data["flash_fota"]
        self.assertIn("boot_metadata", flash)
        metadata = flash["boot_metadata"]
        expected_write_commit_sequence = [
            "choose_non_selected_copy",
            "erase_target_copy",
            "program_uncommitted_record",
            "readback_verify_fields_and_crc",
            "program_commit_marker_last",
            "verify_committed_record",
            "select_in_ram",
        ]
        self.assertEqual(
            metadata["write_commit_sequence"],
            expected_write_commit_sequence,
        )
        program_steps = [
            step
            for step in metadata["write_commit_sequence"]
            if step.startswith("program_")
        ]
        self.assertEqual(program_steps[-1], "program_commit_marker_last")
        self.assertEqual(
            metadata["old_valid_copy_preserved_until"],
            "new_copy_fully_committed_and_verified",
        )
        self.assertEqual(
            metadata["sequence"],
            {
                "domain": "unsigned",
                "monotonic": True,
                "wrap_policy": "forbidden_without_explicit_format_migration",
            },
        )
        self.assertEqual(
            metadata["trial_boot_sequence"],
            [
                "increment_trial_count",
                "durably_commit_trial_count",
                "jump_to_pending_slot",
            ],
        )
        self.assertLess(
            metadata["trial_boot_sequence"].index("increment_trial_count"),
            metadata["trial_boot_sequence"].index("jump_to_pending_slot"),
        )
        self.assertLess(
            metadata["trial_boot_sequence"].index("durably_commit_trial_count"),
            metadata["trial_boot_sequence"].index("jump_to_pending_slot"),
        )
        self.assertTrue(metadata["reset_before_confirmation_consumes_trial"])
        self.assertEqual(
            metadata["provisional_confirmation"],
            {
                "deadline_ms_ref": "timing_ms.fota_confirm_deadline",
                "deadline_begins_at": "trial_runtime_entry",
                "attempt_state": "already_counted",
                "time_source": "same_boot_monotonic",
                "absolute_wall_clock_deadline_used": False,
                "reset_consumes_persisted_trial": True,
                "trial_boot_max_ref": "flash_fota.trial_boot_max",
                "cross_reset_deadline_extension_allowed": False,
            },
        )
        deadline = metadata["provisional_confirmation"]
        self.assertEqual(
            resolve_contract_path(self.data, deadline["deadline_ms_ref"]),
            flash["confirm_deadline_ms"],
        )
        self.assertEqual(
            resolve_contract_path(self.data, deadline["trial_boot_max_ref"]),
            flash["trial_boot_max"],
        )

    def test_manifest_security_and_release_gates(self):
        flash = self.data["flash_fota"]
        self.assertEqual(
            flash["manifest_required_fields"],
            [
                "format_version",
                "artifact_type",
                "target_slot",
                "hardware_revision",
                "version",
                "size",
                "sha256",
                "anti_rollback_counter",
                "key_id",
                "signature",
            ],
        )
        self.assertEqual(flash["hash_algorithm"], "sha256")
        self.assertTrue(flash["asymmetric_signature_required"])
        self.assertTrue(flash["key_id_required"])
        self.assertTrue(flash["anti_rollback_required"])
        self.assertEqual(flash["signature_encoding_status"], "not_frozen")
        self.assertEqual(flash["signature_algorithm"], "ecdsa_p256_sha256")
        self.assertEqual(
            flash["manifest_serialization"], "deterministic_cbor_rfc8949"
        )
        self.assertEqual(
            flash["manifest_signed_bytes"],
            "deterministic_map_without_signature_field",
        )
        self.assertNotIn("signature_algorithm_status", flash)
        self.assertNotIn("manifest_serialization_status", flash)
        self.assertNotIn("signature_encoding", flash)
        self.assertEqual(
            flash["manifest_target_slot_values"],
            {
                "firmware": "inactive_slot_only",
                "model": "inactive_model_slot_only",
            },
        )
        self.assertTrue(flash["manifest_target_slot_is_logical"])
        self.assertEqual(
            flash["manifest_target_slot_values"]["firmware"],
            flash["download_target"],
        )
        self.assertEqual(
            flash["manifest_target_slot_values"]["model"],
            flash["model_download_target"],
        )
        self.assertEqual(
            flash["private_key_location"], "separate_signing_environment"
        )
        self.assertEqual(flash["device_key_material"], ["public_root", "key_id"])
        self.assertEqual(
            flash["key_management_drills_required"],
            ["backup", "rotation", "revoke"],
        )
        self.assertEqual(
            flash["confirm_required_local_gates"],
            [
                "power_control_ok",
                "runtime_started",
                "flash_integrity_ok",
                "config_integrity_ok",
                "at_least_one_valid_temperature",
            ],
        )
        self.assertEqual(
            flash["confirm_optional_degraded_allowed"],
            ["lcd", "audio", "network"],
        )
        self.assertTrue(flash["failed_image_marked_bad"])
        self.assertEqual(flash["rollback_target"], "previous_confirmed_slot")
        self.assertTrue(flash["erase_program_commit_power_cut_tests_required"])
        self.assertTrue(flash["runtime_v2_stability_gate_required"])
        self.assertEqual(
            flash["artifact_download_transport"],
            "https_exclusive_after_mqtt_normal_shutdown",
        )
        self.assertTrue(flash["artifacts_immutable"])
        self.assertTrue(flash["firmware_model_activation_independent"])
        self.assertTrue(flash["model_atomic_activation_required"])
        self.assertTrue(flash["shared_artifact_pipeline"])
        self.assertEqual(flash["model_download_target"], "inactive_model_slot_only")
        self.assertEqual(
            flash["model_activation_checks"],
            ["hash", "signature", "compatibility", "inference_self_test"],
        )
        self.assertEqual(
            flash["model_failure_rollback_target"], "previous_confirmed_model"
        )
        self.assertEqual(
            flash["slot_validation_tests_required"],
            [
                "slot_specific_linker",
                "slot_specific_artifact",
                "slot_header",
                "vector_jump",
                "slot_a_build_jump_rollback",
                "slot_b_build_jump_rollback",
            ],
        )
        self.assertEqual(
            flash["security_claim_scope"],
            [
                "accidental_corruption",
                "unauthorized_network_image",
                "downgrade",
            ],
        )
        self.assertFalse(flash["physical_supply_chain_security_claimed"])
        self.assertEqual(
            flash["first_release_scope"],
            [
                "immutable_bootloader",
                "firmware_ab",
                "rollback",
                "minimum_remote_downloader",
                "inactive_slot_installer",
                "first_remote_update_validation",
            ],
        )
        self.assertEqual(flash["automatic_rollout_status"], "deferred_to_next_release")
        self.assertEqual(
            flash["automatic_install_schedule_status"],
            "not_frozen_until_bootstrap_canary",
        )
        self.assertTrue(flash["release_pipeline_audit_required"])

    def test_no_flash_absolute_address_literal_is_frozen(self):
        serialized = json.dumps(self.data["flash_fota"], sort_keys=True)
        self.assertIsNone(FLASH_HEX_LITERAL.search(serialized))
        assert_no_frozen_flash_location_keys(self, self.data["flash_fota"])

    def test_flash_fota_allowed_key_set_is_exact(self):
        assert_exact_flash_fota_allowed_keys(self, self.data["flash_fota"])

    def test_unknown_flash_fota_schema_key_is_rejected(self):
        negative_fixture = copy.deepcopy(self.data["flash_fota"])
        negative_fixture["unexpected_layout"] = "not_frozen"
        with self.assertRaisesRegex(AssertionError, "unexpected_layout"):
            assert_exact_flash_fota_allowed_keys(self, negative_fixture)

    def test_flash_fota_nested_schema_is_fail_closed(self):
        partition_map = copy.deepcopy(self.data["flash_fota"])
        partition_map["boot_metadata"]["partition_map"] = {
            "slot_a": 1048576
        }

        unknown_object = copy.deepcopy(self.data["flash_fota"])
        unknown_object["boot_metadata"]["selection"]["future_policy"] = {
            "mode": "safe"
        }

        unknown_list = copy.deepcopy(self.data["flash_fota"])
        unknown_list["boot_metadata"]["selection"]["future_modes"] = [
            "safe"
        ]

        unknown_status = copy.deepcopy(self.data["flash_fota"])
        unknown_status["boot_metadata"]["slot_state_invariants"][
            "slot_base_status"
        ] = "not_frozen_until_size_table_approval"

        object_list_element = copy.deepcopy(self.data["flash_fota"])
        object_list_element["bootloader_responsibilities"].append(
            {"name": "rollback"}
        )

        numeric_list_element = copy.deepcopy(self.data["flash_fota"])
        numeric_list_element["service_recovery_methods"].append(7)

        cases = [
            ("partition_map", partition_map),
            ("future_policy", unknown_object),
            ("future_modes", unknown_list),
            ("slot_base_status", unknown_status),
            ("bootloader_responsibilities", object_list_element),
            ("service_recovery_methods", numeric_list_element),
        ]
        for expected_fragment, fixture in cases:
            with self.subTest(expected_fragment=expected_fragment):
                with self.assertRaisesRegex(
                    AssertionError,
                    expected_fragment,
                ):
                    assert_exact_flash_fota_allowed_keys(self, fixture)

    def test_nested_decimal_flash_offset_is_rejected(self):
        negative_fixture = {"slots": {"slot_a_offset": 1048576}}
        with self.assertRaisesRegex(AssertionError, "slot_a_offset"):
            assert_no_frozen_flash_location_keys(self, negative_fixture)

    def test_flash_location_guard_rejects_bypasses_and_allows_status(self):
        negative_fixtures = {
            "slot_a_start": {"nested": {"slot_a_start": 1048576}},
            "slot_locations": {"nested": [{"slot_locations": [1048576]}]},
            "slot_a_addr": {"slot_a_addr": "1048576"},
            "partition_end": {"partition_end": "0x10100000"},
        }
        for key, fixture in negative_fixtures.items():
            with self.subTest(key=key):
                with self.assertRaisesRegex(AssertionError, key):
                    assert_no_frozen_flash_location_keys(self, fixture)

        assert_no_frozen_flash_location_keys(
            self,
            {
                "partition_address_status": (
                    "not_frozen_until_size_table_approval"
                )
            },
        )
        with self.assertRaisesRegex(AssertionError, "slot_base_status"):
            assert_no_frozen_flash_location_keys(
                self,
                {
                    "nested": [
                        {
                            "slot_base_status": (
                                "not_frozen_until_size_table_approval"
                            )
                        }
                    ]
                },
            )
        with self.assertRaisesRegex(AssertionError, "slot_base_status"):
            assert_no_frozen_flash_location_keys(
                self,
                {"slot_base_status": "approved"},
            )

    def test_flash_location_guard_normalizes_key_spelling(self):
        location_tokens = [
            "address",
            "addr",
            "base",
            "offset",
            "origin",
            "start",
            "end",
            "location",
            "locations",
        ]
        for token in location_tokens:
            spellings = {
                "snake": f"slot_{token}",
                "camel": f"slot{token.title()}",
                "pascal": f"Slot{token.title()}",
                "kebab": f"slot-{token}",
                "compact_suffix": f"slot{token}",
                "compact_prefix": f"{token}slot",
            }
            for spelling, key in spellings.items():
                with self.subTest(token=token, spelling=spelling, key=key):
                    with self.assertRaisesRegex(AssertionError, key):
                        assert_no_frozen_flash_location_keys(
                            self,
                            {"boot_metadata": {key: 1048576}},
                        )

        with self.assertRaisesRegex(
            AssertionError,
            r"boot_metadata\.slotStart",
        ):
            assert_no_frozen_flash_location_keys(
                self,
                {"boot_metadata": {"slotStart": 1048576}},
            )


if __name__ == "__main__":
    unittest.main()
