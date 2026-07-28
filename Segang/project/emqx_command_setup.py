#!/usr/bin/env python3
"""Fail-closed EMQX 6.2 command Action/Rule configuration utility."""

import argparse
import base64
import binascii
import json
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, Mapping, Optional, Sequence, Tuple
from urllib import error, request


CONNECTOR_ITEM_ID = "http:supabase_api"
ACTION_CONNECTOR_NAME = "supabase_api"
RESOURCE_IDS = {
    "request_action": "supabase_command_request",
    "ack_action": "supabase_command_ack",
    "request_rule": "command_request_rule",
    "ack_rule": "command_ack_rule",
}
REQUIRED_ENV_KEYS = (
    "SUPABASE_URL",
    "SUPABASE_KEY",
    "EMQX_COMMAND_GATEWAY_SECRET",
)
MODES = ("plan", "apply-disabled", "verify", "enable", "disable", "delete")
DEFAULT_EMQX_API = "http://localhost:18083/api/v5"
UINT32_MAX = 4294967295


@dataclass(frozen=True)
class HttpResponse:
    status_code: int
    payload: Any


@dataclass(frozen=True)
class ManagedResource:
    resource_id: str
    kind: str
    collection_path: str
    item_path: str
    desired: Dict[str, Any]


class CommandSetupError(RuntimeError):
    pass


class UrllibTransport:
    def request(
        self,
        method: str,
        url: str,
        *,
        headers: Optional[Mapping[str, str]] = None,
        json_body: Optional[Mapping[str, Any]] = None,
    ) -> HttpResponse:
        data = None
        if json_body is not None:
            data = json.dumps(
                json_body,
                ensure_ascii=True,
                separators=(",", ":"),
            ).encode("utf-8")
        api_request = request.Request(
            url,
            data=data,
            headers=dict(headers or {}),
            method=method,
        )
        try:
            with request.urlopen(api_request, timeout=10) as response:
                return HttpResponse(
                    response.status,
                    _decode_json_response(response.read()),
                )
        except error.HTTPError as exc:
            return HttpResponse(
                exc.code,
                _decode_json_response(exc.read()),
            )


def _decode_json_response(raw: bytes) -> Any:
    if not raw:
        return None
    try:
        return json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None


def build_request_rule(enabled: bool = False) -> Dict[str, Any]:
    payload = "json_decode(payload)"
    first = f"nth(1, {payload})"
    second = f"nth(2, {payload})"
    return {
        "id": RESOURCE_IDS["request_rule"],
        "sql": (
            "SELECT clientid as p_imei, "
            f"int({first}) as p_request_id, "
            f"int({second}) as p_last_cmd_id "
            'FROM "devices/+/cmd/request" '
            "WHERE flags.retain = false "
            f"and is_array({payload}) "
            f"and length({payload}) = 2 "
            f"and is_int({first}) "
            f"and is_int({second}) "
            "and nth(2, split(topic, '/')) = clientid "
            f"and {first} >= 1 "
            f"and {first} <= {UINT32_MAX} "
            f"and {second} >= 0 "
            f"and {second} <= {UINT32_MAX}"
        ),
        "actions": [f"http:{RESOURCE_IDS['request_action']}"],
        "enable": bool(enabled),
    }


def build_ack_rule(enabled: bool = False) -> Dict[str, Any]:
    payload = "json_decode(payload)"
    values = tuple(f"nth({index}, {payload})" for index in range(1, 7))
    cmd_id, phase, result, ack_error, unix_seconds, clock_valid = values
    return {
        "id": RESOURCE_IDS["ack_rule"],
        "sql": (
            "SELECT clientid as p_imei, "
            f"int({cmd_id}) as p_cmd_id, "
            f"int({phase}) as p_phase, "
            f"int({result}) as p_result, "
            f"int({ack_error}) as p_error, "
            f"int({unix_seconds}) as p_unix_seconds, "
            f"bool({clock_valid}) as p_clock_valid "
            'FROM "devices/+/cmd/ack" '
            "WHERE flags.retain = false "
            f"and is_array({payload}) "
            f"and length({payload}) = 6 "
            + "".join(f"and is_int({value}) " for value in values)
            + "and nth(2, split(topic, '/')) = clientid "
            f"and {cmd_id} >= 1 "
            f"and {cmd_id} <= {UINT32_MAX} "
            f"and {phase} >= 1 "
            f"and {phase} <= 2 "
            f"and {result} >= 1 "
            f"and {result} <= 4 "
            f"and {ack_error} >= 0 "
            f"and {ack_error} <= 5 "
            f"and {unix_seconds} >= 0 "
            f"and {unix_seconds} <= {UINT32_MAX} "
            f"and {clock_valid} >= 0 "
            f"and {clock_valid} <= 1 "
            f"and ({clock_valid} = 1 or {unix_seconds} = 0) "
            "and ("
            f"({phase} = 1 and {result} = 1 and {ack_error} = 0) "
            "or "
            f"({phase} = 2 and ("
            f"({result} = 2 and {ack_error} = 0) "
            "or "
            f"({result} = 3 and {ack_error} >= 1 "
            f"and {ack_error} <= 4) "
            "or "
            f"({result} = 4 and {ack_error} = 5)"
            "))"
            ")"
        ),
        "actions": [f"http:{RESOURCE_IDS['ack_action']}"],
        "enable": bool(enabled),
    }


def build_request_action(
    request_secret: str,
    enabled: bool = False,
) -> Dict[str, Any]:
    return _build_action(
        name=RESOURCE_IDS["request_action"],
        path="/rest/v1/rpc/publish_device_command_response",
        body=_json_template(
            (
                ("p_imei", "${p_imei}", False),
                ("p_request_id", "${p_request_id}", True),
                ("p_last_cmd_id", "${p_last_cmd_id}", True),
                ("p_request_secret", request_secret, False),
            )
        ),
        enabled=enabled,
    )


def build_ack_action(
    request_secret: str,
    enabled: bool = False,
) -> Dict[str, Any]:
    return _build_action(
        name=RESOURCE_IDS["ack_action"],
        path="/rest/v1/rpc/publish_device_command_ack_receipt",
        body=_json_template(
            (
                ("p_imei", "${p_imei}", False),
                ("p_cmd_id", "${p_cmd_id}", True),
                ("p_phase", "${p_phase}", True),
                ("p_result", "${p_result}", True),
                ("p_error", "${p_error}", True),
                ("p_unix_seconds", "${p_unix_seconds}", True),
                ("p_clock_valid", "${p_clock_valid}", True),
                ("p_request_secret", request_secret, False),
            )
        ),
        enabled=enabled,
    )


def _build_action(
    *,
    name: str,
    path: str,
    body: str,
    enabled: bool,
) -> Dict[str, Any]:
    return {
        "type": "http",
        "name": name,
        "enable": bool(enabled),
        "connector": ACTION_CONNECTOR_NAME,
        "parameters": {
            "body": body,
            "headers": {"content-type": "application/json"},
            "max_retries": 0,
            "method": "post",
            "path": path,
        },
    }


def _json_template(fields: Iterable[Tuple[str, str, bool]]) -> str:
    rendered = []
    for key, value, raw in fields:
        encoded_value = value if raw else json.dumps(value, ensure_ascii=True)
        rendered.append(
            f"{json.dumps(key, ensure_ascii=True)}:{encoded_value}"
        )
    return "{" + ",".join(rendered) + "}"


def _contains_line_break(value: str) -> bool:
    return "\r" in value or "\n" in value


def _normalize_auth_header(value: str) -> str:
    if not isinstance(value, str) or not value or _contains_line_break(value):
        raise ValueError("invalid EMQX API authorization")
    candidate = value.strip()
    name, separator, header_value = candidate.partition(":")
    if separator and name.strip().lower() != "authorization":
        raise ValueError("invalid EMQX API authorization")
    candidate = header_value.strip() if separator else candidate
    match = re.fullmatch(r"Basic ([A-Za-z0-9+/]+={0,2})", candidate)
    if match is None:
        raise ValueError("invalid EMQX API authorization")
    try:
        decoded = base64.b64decode(match.group(1), validate=True)
    except (binascii.Error, ValueError):
        raise ValueError("invalid EMQX API authorization")
    if b":" not in decoded or not decoded.split(b":", 1)[0]:
        raise ValueError("invalid EMQX API authorization")
    return f"Basic {match.group(1)}"


def build_api_authorization(env: Mapping[str, str]) -> str:
    api_key = env.get("EMQX_API_KEY")
    api_secret = env.get("EMQX_API_SECRET")
    supplied = env.get("EMQX_API_AUTH_HEADER")
    if api_key or api_secret:
        if not api_key or not api_secret:
            raise ValueError("incomplete EMQX API credentials")
        if (
            _contains_line_break(api_key)
            or _contains_line_break(api_secret)
            or ":" in api_key
        ):
            raise ValueError("invalid EMQX API credentials")
        token = base64.b64encode(
            f"{api_key}:{api_secret}".encode("utf-8")
        ).decode("ascii")
        generated = f"Basic {token}"
        if supplied is not None and _normalize_auth_header(supplied) != generated:
            raise ValueError("ambiguous EMQX API credentials")
        return generated
    if supplied is None:
        raise ValueError("missing EMQX API credentials")
    return _normalize_auth_header(supplied)


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Manage disabled-first EMQX command resources."
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


def _load_dotenv(target: Dict[str, str]) -> None:
    dotenv_path = Path(__file__).resolve().with_name(".env")
    if not dotenv_path.is_file():
        return
    for raw_line in dotenv_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()
        if (
            len(value) >= 2
            and value[0] == value[-1]
            and value[0] in ("'", '"')
        ):
            value = value[1:-1]
        if key and key not in target:
            target[key] = value


def _resolve_environment(
    environ: Optional[Mapping[str, str]],
) -> Dict[str, str]:
    resolved = dict(os.environ if environ is None else environ)
    if environ is None:
        _load_dotenv(resolved)
    missing = [key for key in REQUIRED_ENV_KEYS if not resolved.get(key)]
    if missing:
        raise CommandSetupError("missing required environment")
    build_api_authorization(resolved)
    for key in ("SUPABASE_URL", "EMQX_COMMAND_GATEWAY_SECRET"):
        if _contains_line_break(resolved[key]):
            raise CommandSetupError("invalid environment")
    return resolved


def _desired_resources(env: Mapping[str, str]) -> Sequence[ManagedResource]:
    return (
        ManagedResource(
            RESOURCE_IDS["request_action"],
            "action",
            "actions",
            f"actions/http:{RESOURCE_IDS['request_action']}",
            build_request_action(
                env["EMQX_COMMAND_GATEWAY_SECRET"],
                False,
            ),
        ),
        ManagedResource(
            RESOURCE_IDS["ack_action"],
            "action",
            "actions",
            f"actions/http:{RESOURCE_IDS['ack_action']}",
            build_ack_action(
                env["EMQX_COMMAND_GATEWAY_SECRET"],
                False,
            ),
        ),
        ManagedResource(
            RESOURCE_IDS["request_rule"],
            "rule",
            "rules",
            f"rules/{RESOURCE_IDS['request_rule']}",
            build_request_rule(False),
        ),
        ManagedResource(
            RESOURCE_IDS["ack_rule"],
            "rule",
            "rules",
            f"rules/{RESOURCE_IDS['ack_rule']}",
            build_ack_rule(False),
        ),
    )


def _api_headers(env: Mapping[str, str]) -> Dict[str, str]:
    return {
        "Authorization": build_api_authorization(env),
        "Content-Type": "application/json",
        "User-Agent": "NB-IOT-EMQX-Setup/1.0",
    }


def _api_url(env: Mapping[str, str], path: str) -> str:
    base = (
        env.get("EMQX_API")
        or env.get("EMQX_API_URL")
        or DEFAULT_EMQX_API
    ).rstrip("/")
    if _contains_line_break(base):
        raise CommandSetupError("invalid EMQX API URL")
    return f"{base}/{path}"


def _normalized_headers(value: Any) -> Optional[Dict[str, Any]]:
    if isinstance(value, Mapping):
        return {str(key).lower(): item for key, item in value.items()}
    if isinstance(value, list):
        normalized = {}
        for item in value:
            if not isinstance(item, Mapping):
                return None
            key = item.get("key", item.get("name"))
            if key is None or "value" not in item:
                return None
            normalized[str(key).lower()] = item["value"]
        return normalized
    return None


def _projection(payload: Mapping[str, Any], kind: str) -> Dict[str, Any]:
    if kind == "rule":
        return {
            "id": payload.get("id"),
            "sql": payload.get("sql"),
            "actions": payload.get("actions"),
        }
    parameters = payload.get("parameters")
    if not isinstance(parameters, Mapping):
        parameters = {}
    return {
        "type": payload.get("type"),
        "name": payload.get("name"),
        "connector": payload.get("connector"),
        "parameters": {
            "body": parameters.get("body"),
            "headers": _normalized_headers(parameters.get("headers")),
            "max_retries": parameters.get("max_retries"),
            "method": str(parameters.get("method", "")).lower(),
            "path": parameters.get("path"),
        },
    }


def _validate_connector(
    env: Mapping[str, str],
    transport: Any,
) -> None:
    response = transport.request(
        "GET",
        _api_url(env, f"connectors/{CONNECTOR_ITEM_ID}"),
        headers=_api_headers(env),
    )
    if response.status_code != 200 or not isinstance(
        response.payload, Mapping
    ):
        raise CommandSetupError("required connector unavailable")
    payload = response.payload
    connector_url = payload.get("url", payload.get("base_url"))
    headers = _normalized_headers(payload.get("headers"))
    if (
        payload.get("type") != "http"
        or payload.get("name") != "supabase_api"
        or payload.get("enable") is not True
        or not isinstance(connector_url, str)
        or connector_url.rstrip("/") != env["SUPABASE_URL"].rstrip("/")
        or headers is None
        or set(headers) != {"authorization", "content-type", "apikey"}
    ):
        raise CommandSetupError("required connector definition drift")


def _read_one(
    resource: ManagedResource,
    env: Mapping[str, str],
    transport: Any,
    *,
    validate_definition: bool,
) -> Optional[Mapping[str, Any]]:
    response = transport.request(
        "GET",
        _api_url(env, resource.item_path),
        headers=_api_headers(env),
    )
    if response.status_code == 404:
        return None
    if response.status_code != 200 or not isinstance(
        response.payload, Mapping
    ):
        raise CommandSetupError("managed resource read failed")
    if (
        validate_definition
        and _projection(response.payload, resource.kind)
        != _projection(resource.desired, resource.kind)
    ):
        raise CommandSetupError("managed resource definition drift")
    return response.payload


def _get_existing(
    resources: Sequence[ManagedResource],
    env: Mapping[str, str],
    transport: Any,
    *,
    validate_definition: bool = True,
) -> Dict[str, Optional[Mapping[str, Any]]]:
    return {
        resource.resource_id: _read_one(
            resource,
            env,
            transport,
            validate_definition=validate_definition,
        )
        for resource in resources
    }


def _write(
    method: str,
    resource: ManagedResource,
    payload: Optional[Mapping[str, Any]],
    env: Mapping[str, str],
    transport: Any,
) -> None:
    path = (
        resource.collection_path
        if method == "POST"
        else resource.item_path
    )
    response = transport.request(
        method,
        _api_url(env, path),
        headers=_api_headers(env),
        json_body=payload,
    )
    expected = {
        "POST": (200, 201),
        "PUT": (200, 204),
        "DELETE": (200, 204),
    }[method]
    if response.status_code not in expected:
        raise CommandSetupError("managed resource mutation failed")


def _set_enabled_one(
    resource: ManagedResource,
    enabled: bool,
    env: Mapping[str, str],
    transport: Any,
) -> None:
    if resource.kind == "action":
        path = (
            f"{resource.item_path}/enable/"
            f"{str(bool(enabled)).lower()}"
        )
        payload = None
    else:
        path = resource.item_path
        payload = dict(resource.desired)
        payload.pop("id", None)
        payload["enable"] = bool(enabled)
    response = transport.request(
        "PUT",
        _api_url(env, path),
        headers=_api_headers(env),
        json_body=payload,
    )
    if response.status_code not in (200, 204):
        raise CommandSetupError("managed resource state mutation failed")


def _verify(
    resources: Sequence[ManagedResource],
    env: Mapping[str, str],
    transport: Any,
    expected_enabled: Optional[bool],
) -> Dict[str, Optional[Mapping[str, Any]]]:
    existing = _get_existing(resources, env, transport)
    for resource in resources:
        current = existing[resource.resource_id]
        if current is None:
            raise CommandSetupError("managed resource missing")
        if (
            expected_enabled is not None
            and current.get("enable") is not expected_enabled
        ):
            raise CommandSetupError("managed resource state mismatch")
    return existing


def _disable_all(
    resources: Sequence[ManagedResource],
    env: Mapping[str, str],
    transport: Any,
) -> bool:
    for _attempt in range(2):
        for resource in reversed(resources):
            try:
                current = _read_one(
                    resource,
                    env,
                    transport,
                    validate_definition=True,
                )
                if current is not None and current.get("enable") is not False:
                    _set_enabled_one(
                        resource,
                        False,
                        env,
                        transport,
                    )
            except Exception:
                pass
        safe = True
        for resource in resources:
            try:
                current = _read_one(
                    resource,
                    env,
                    transport,
                    validate_definition=True,
                )
                if current is not None and current.get("enable") is True:
                    safe = False
            except Exception:
                safe = False
        if safe:
            return True
    return False


def _enable_all(
    resources: Sequence[ManagedResource],
    existing: Mapping[str, Optional[Mapping[str, Any]]],
    env: Mapping[str, str],
    transport: Any,
) -> None:
    if any(existing[resource.resource_id] is None for resource in resources):
        raise CommandSetupError("managed resource missing")
    try:
        for resource in resources:
            current = existing[resource.resource_id]
            if current.get("enable") is True:
                continue
            _set_enabled_one(
                resource,
                True,
                env,
                transport,
            )
        _verify(resources, env, transport, True)
    except Exception:
        if not _disable_all(resources, env, transport):
            raise CommandSetupError("enable compensation failed")
        raise CommandSetupError("enable failed and was compensated")


def _delete_created(
    resources: Sequence[ManagedResource],
    created_ids: Sequence[str],
    env: Mapping[str, str],
    transport: Any,
) -> None:
    created = set(created_ids)
    for resource in reversed(resources):
        if resource.resource_id not in created:
            continue
        try:
            current = _read_one(
                resource,
                env,
                transport,
                validate_definition=True,
            )
            if current is None:
                continue
            response = transport.request(
                "DELETE",
                _api_url(env, resource.item_path),
                headers=_api_headers(env),
                json_body=None,
            )
            if response.status_code not in (200, 204, 404):
                continue
        except Exception:
            continue


def _print_states(
    mode: str,
    resources: Sequence[ManagedResource],
    existing: Optional[Mapping[str, Optional[Mapping[str, Any]]]] = None,
) -> None:
    for resource in resources:
        if existing is None:
            state = "disabled"
        else:
            current = existing.get(resource.resource_id)
            if current is None:
                state = "absent"
            else:
                state = "enabled" if current.get("enable") else "disabled"
        print(
            f"OK mode={mode} resource={resource.resource_id} state={state}"
        )


def _run_mode(
    mode: str,
    env: Mapping[str, str],
    transport: Any,
) -> None:
    resources = _desired_resources(env)
    if mode == "plan":
        _print_states(mode, resources)
        return

    if mode in ("apply-disabled", "verify", "enable"):
        _validate_connector(env, transport)

    if mode == "apply-disabled":
        existing = _get_existing(resources, env, transport)
        created_ids = []
        try:
            for resource in resources:
                current = existing[resource.resource_id]
                if current is None:
                    _write(
                        "POST",
                        resource,
                        resource.desired,
                        env,
                        transport,
                    )
                    created_ids.append(resource.resource_id)
                elif current.get("enable") is not False:
                    _set_enabled_one(
                        resource,
                        False,
                        env,
                        transport,
                    )
            verified = _verify(resources, env, transport, False)
        except Exception:
            _disable_all(resources, env, transport)
            _delete_created(
                resources,
                created_ids,
                env,
                transport,
            )
            raise CommandSetupError("apply-disabled failed")
        _print_states(mode, resources, verified)
        return

    if mode == "verify":
        verified = _verify(resources, env, transport, None)
        _print_states(mode, resources, verified)
        return

    if mode == "enable":
        existing = _get_existing(resources, env, transport)
        _enable_all(resources, existing, env, transport)
        verified = _verify(resources, env, transport, True)
        _print_states(mode, resources, verified)
        return

    if mode == "disable":
        _get_existing(resources, env, transport)
        if not _disable_all(resources, env, transport):
            raise CommandSetupError("disable failed")
        verified = _verify(resources, env, transport, False)
        _print_states(mode, resources, verified)
        return

    if mode == "delete":
        _get_existing(resources, env, transport)
        if not _disable_all(resources, env, transport):
            raise CommandSetupError("delete pre-disable failed")
        existing = _get_existing(resources, env, transport)
        errors = False
        for resource in reversed(resources):
            if existing[resource.resource_id] is None:
                continue
            try:
                current = _read_one(
                    resource,
                    env,
                    transport,
                    validate_definition=True,
                )
                if current is None:
                    continue
                _write("DELETE", resource, None, env, transport)
            except Exception:
                errors = True
        remaining = _get_existing(
            resources,
            env,
            transport,
            validate_definition=False,
        )
        if errors or any(value is not None for value in remaining.values()):
            raise CommandSetupError("delete failed")
        _print_states(mode, resources, remaining)
        return

    raise CommandSetupError("unsupported mode")


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
            transport if transport is not None else UrllibTransport(),
        )
        return 0
    except Exception:
        print(f"ERROR mode={args.mode} command-resource-operation-failed")
        return 1


if __name__ == "__main__":
    sys.exit(main())
