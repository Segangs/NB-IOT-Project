#!/usr/bin/env python3
"""Fail-closed EMQX 6.2 power-event Action/Rule configuration utility."""

import argparse
import os
import sys
from typing import Any, Dict, Mapping, Optional, Sequence

try:
    from . import emqx_command_setup as common
except ImportError:
    import emqx_command_setup as common


RESOURCE_IDS = {
    "event_action": "supabase_power_event",
    "event_rule": "power_event_rule",
}
REQUIRED_ENV_KEYS = (
    "SUPABASE_URL",
    "SUPABASE_KEY",
    "EMQX_EVENT_GATEWAY_SECRET",
)
MODES = ("plan", "apply-disabled", "verify", "enable", "disable", "delete")
UINT32_MAX = 4294967295


def build_event_action(
    request_secret: str,
    enabled: bool = False,
) -> Dict[str, Any]:
    body = common._json_template(
        (
            ("p_imei", "${p_imei}", False),
            ("p_schema", "${p_schema}", True),
            ("p_event_type", "${p_event_type}", True),
            ("p_incident_id", "${p_incident_id}", True),
            ("p_sequence", "${p_sequence}", True),
            ("p_state", "${p_state}", True),
            ("p_value0", "${p_value0}", True),
            ("p_value1", "${p_value1}", True),
            ("p_unix_seconds", "${p_unix_seconds}", True),
            ("p_clock_valid", "${p_clock_valid}", True),
            ("p_request_secret", request_secret, False),
        )
    )
    return {
        "type": "http",
        "name": RESOURCE_IDS["event_action"],
        "enable": bool(enabled),
        "connector": common.ACTION_CONNECTOR_NAME,
        "parameters": {
            "body": body,
            "headers": {"content-type": "application/json"},
            "max_retries": 0,
            "method": "post",
            "path": "/rest/v1/rpc/ingest_device_power_event",
        },
    }


def build_event_rule(enabled: bool = False) -> Dict[str, Any]:
    payload = "json_decode(payload)"
    values = tuple(f"nth({index}, {payload})" for index in range(1, 10))
    (
        schema,
        event_type,
        incident_id,
        sequence,
        state,
        value0,
        value1,
        unix_seconds,
        clock_valid,
    ) = values
    return {
        "id": RESOURCE_IDS["event_rule"],
        "sql": (
            "SELECT clientid as p_imei, "
            f"int({schema}) as p_schema, "
            f"int({event_type}) as p_event_type, "
            f"int({incident_id}) as p_incident_id, "
            f"int({sequence}) as p_sequence, "
            f"int({state}) as p_state, "
            f"int({value0}) as p_value0, "
            f"int({value1}) as p_value1, "
            f"int({unix_seconds}) as p_unix_seconds, "
            f"bool({clock_valid}) as p_clock_valid "
            'FROM "devices/+/event" '
            "WHERE flags.retain = false "
            f"and is_array({payload}) "
            f"and length({payload}) = 9 "
            + "".join(f"and is_int({value}) " for value in values)
            + "and nth(2, split(topic, '/')) = clientid "
            f"and {schema} = 1 "
            f"and {incident_id} >= 1 "
            f"and {incident_id} <= {UINT32_MAX} "
            f"and {unix_seconds} >= 0 "
            f"and {unix_seconds} <= {UINT32_MAX} "
            f"and {clock_valid} >= 0 "
            f"and {clock_valid} <= 1 "
            f"and ({clock_valid} = 1 or {unix_seconds} = 0) "
            "and ("
            f"({event_type} = 4 and {sequence} = 1 "
            f"and {state} = 1 and {value0} = 0 and {value1} = 0) "
            "or "
            f"({event_type} = 5 and {sequence} = 2 "
            f"and {state} = 0 and {value0} = 1 and {value1} = 0) "
            "or "
            f"({event_type} = 6 and {sequence} = 2 "
            f"and {state} = 2 and {value0} = 210 and {value1} = 90)"
            ")"
        ),
        "actions": [f"http:{RESOURCE_IDS['event_action']}"],
        "enable": bool(enabled),
    }


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Manage disabled-first EMQX power-event resources."
    )
    parser.add_argument(
        "mode",
        nargs="?",
        choices=MODES,
        default="plan",
    )
    parser.add_argument(
        "--enable",
        action="store_true",
        help="Required acknowledgement for the enable mode.",
    )
    args = parser.parse_args(argv)
    if args.mode == "enable" and not args.enable:
        parser.error("enable mode requires --enable")
    if args.mode != "enable" and args.enable:
        parser.error("--enable is valid only with enable mode")
    return args


def _resolve_environment(
    environ: Optional[Mapping[str, str]],
) -> Dict[str, str]:
    resolved = dict(os.environ if environ is None else environ)
    if environ is None:
        common._load_dotenv(resolved)
    missing = [key for key in REQUIRED_ENV_KEYS if not resolved.get(key)]
    if missing:
        raise common.CommandSetupError("missing required environment")
    common.build_api_authorization(resolved)
    for key in ("SUPABASE_URL", "EMQX_EVENT_GATEWAY_SECRET"):
        if common._contains_line_break(resolved[key]):
            raise common.CommandSetupError("invalid environment")
    return resolved


def _desired_resources(
    env: Mapping[str, str],
) -> Sequence[common.ManagedResource]:
    return (
        common.ManagedResource(
            RESOURCE_IDS["event_action"],
            "action",
            "actions",
            f"actions/http:{RESOURCE_IDS['event_action']}",
            build_event_action(env["EMQX_EVENT_GATEWAY_SECRET"], False),
        ),
        common.ManagedResource(
            RESOURCE_IDS["event_rule"],
            "rule",
            "rules",
            f"rules/{RESOURCE_IDS['event_rule']}",
            build_event_rule(False),
        ),
    )


def _run_mode(
    mode: str,
    env: Mapping[str, str],
    transport: Any,
) -> None:
    resources = _desired_resources(env)
    if mode == "plan":
        common._print_states(mode, resources)
        return

    if mode in ("apply-disabled", "verify", "enable"):
        common._validate_connector(env, transport)

    if mode == "apply-disabled":
        existing = common._get_existing(resources, env, transport)
        created_ids = []
        try:
            for resource in resources:
                current = existing[resource.resource_id]
                if current is None:
                    common._write(
                        "POST",
                        resource,
                        resource.desired,
                        env,
                        transport,
                    )
                    created_ids.append(resource.resource_id)
                elif current.get("enable") is not False:
                    common._set_enabled_one(
                        resource,
                        False,
                        env,
                        transport,
                    )
            verified = common._verify(resources, env, transport, False)
        except Exception:
            common._disable_all(resources, env, transport)
            common._delete_created(
                resources,
                created_ids,
                env,
                transport,
            )
            raise common.CommandSetupError("apply-disabled failed")
        common._print_states(mode, resources, verified)
        return

    if mode == "verify":
        verified = common._verify(resources, env, transport, None)
        common._print_states(mode, resources, verified)
        return

    if mode == "enable":
        existing = common._get_existing(resources, env, transport)
        common._enable_all(resources, existing, env, transport)
        verified = common._verify(resources, env, transport, True)
        common._print_states(mode, resources, verified)
        return

    if mode == "disable":
        common._get_existing(resources, env, transport)
        if not common._disable_all(resources, env, transport):
            raise common.CommandSetupError("disable failed")
        verified = common._verify(resources, env, transport, False)
        common._print_states(mode, resources, verified)
        return

    if mode == "delete":
        common._get_existing(resources, env, transport)
        if not common._disable_all(resources, env, transport):
            raise common.CommandSetupError("delete pre-disable failed")
        existing = common._get_existing(resources, env, transport)
        errors = False
        for resource in reversed(resources):
            if existing[resource.resource_id] is None:
                continue
            try:
                current = common._read_one(
                    resource,
                    env,
                    transport,
                    validate_definition=True,
                )
                if current is None:
                    continue
                common._write("DELETE", resource, None, env, transport)
            except Exception:
                errors = True
        remaining = common._get_existing(
            resources,
            env,
            transport,
            validate_definition=False,
        )
        if errors or any(value is not None for value in remaining.values()):
            raise common.CommandSetupError("delete failed")
        common._print_states(mode, resources, remaining)
        return

    raise common.CommandSetupError("unsupported mode")


def main(
    argv: Optional[Sequence[str]] = None,
    *,
    environ: Optional[Mapping[str, str]] = None,
    transport: Optional[Any] = None,
) -> int:
    args = parse_args(argv)
    try:
        resolved = _resolve_environment(environ)
        _run_mode(
            args.mode,
            resolved,
            transport if transport is not None else common.UrllibTransport(),
        )
        return 0
    except Exception:
        print(f"ERROR mode={args.mode} event-resource-operation-failed")
        return 1


if __name__ == "__main__":
    sys.exit(main())
