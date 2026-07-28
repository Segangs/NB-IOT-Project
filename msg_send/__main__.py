from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys
from typing import Callable, Mapping, Optional, Sequence, TextIO

from .callback import parse_push_result
from .config import RuntimeConfig, load_runtime_config
from .runtime import ProductionRuntime, build_runtime
from .synthetic_callback import load_synthetic_payload
from .webhook_spool import CallbackSpoolDrainer


def main(
    argv: Optional[Sequence[str]] = None,
    *,
    environ: Optional[Mapping[str, str]] = None,
    runtime_factory: Callable[
        [RuntimeConfig],
        ProductionRuntime,
    ] = build_runtime,
    output: TextIO = sys.stdout,
) -> int:
    parser = _parser()
    args = parser.parse_args(argv)
    if args.command == "one-shot" and not args.confirm_one_shot:
        parser.error("one-shot requires --confirm-one-shot")

    config = load_runtime_config(
        os.environ if environ is None else environ
    )
    if (
        args.command == "drain-callback-spool"
        and not config.callback_apply_enabled
    ):
        parser.error(
            "drain-callback-spool requires "
            "MSG_SEND_CALLBACK_APPLY_ENABLED=true"
        )
    runtime = runtime_factory(config)
    if args.command == "health":
        health = runtime.health()
        _write_json(
            output,
            {
                "healthy": health.healthy,
                "claim_enabled": health.claim_enabled,
                "send_enabled": health.send_enabled,
                "provider_result_lag_seconds":
                    health.worker.provider_result_lag_seconds,
                "consecutive_cycle_failures":
                    health.worker.consecutive_cycle_failures,
            },
        )
        return 0 if health.healthy else 1
    if args.command == "run-once":
        result = runtime.run_once()
        _write_json(
            output,
            {
                "executed": result.executed,
                "success": result.success,
                "reason": result.reason,
            },
        )
        return 0 if result.success else 1
    if args.command == "one-shot":
        result = runtime.one_shot(str(args.expected_message_id))
        _write_json(
            output,
            {
                "executed": result.executed,
                "success": result.success,
                "reason": result.reason,
            },
        )
        return 0 if result.success else 1
    if args.command == "reconcile-callback":
        result = runtime.reconcile_callback(args.provider_request_id)
        _write_json(
            output,
            {
                "executed": result.executed,
                "success": result.success,
                "reason": result.reason,
            },
        )
        return 0 if result.success else 1
    if args.command == "synthetic-callback":
        payload = load_synthetic_payload(args.input)
        if args.apply:
            if not config.callback_apply_enabled:
                parser.error(
                    "synthetic callback apply gate is disabled"
                )
            action = runtime.handle_callback_payload_non_sending(payload)
            _write_json(output, {"action": action.value})
        else:
            result = parse_push_result(payload, config.tariffs)
            _write_json(
                output,
                {
                    "valid": True,
                    "channel": result.channel.value,
                    "delivered": result.delivered,
                    "result_at": result.result_at.isoformat(),
                },
            )
        return 0
    if args.command == "drain-callback-spool":
        result = CallbackSpoolDrainer(
            args.spool_root,
            runtime,
            max_files=args.max_files,
        ).drain()
        _write_json(
            output,
            {
                "success": result.success,
                "processed": result.processed,
                "retried": result.retried,
                "quarantined": result.quarantined,
            },
        )
        return 0 if result.success else 1
    parser.error("unsupported command")
    return 2


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="python -m msg_send")
    commands = parser.add_subparsers(
        dest="command",
        required=True,
    )
    commands.add_parser("health")
    commands.add_parser("run-once")
    one_shot = commands.add_parser("one-shot")
    one_shot.add_argument(
        "--expected-message-id",
        type=int,
        required=True,
    )
    one_shot.add_argument(
        "--confirm-one-shot",
        action="store_true",
    )
    reconcile = commands.add_parser("reconcile-callback")
    reconcile.add_argument(
        "--provider-request-id",
        required=True,
    )
    synthetic = commands.add_parser("synthetic-callback")
    synthetic.add_argument(
        "--input",
        type=Path,
        required=True,
    )
    synthetic.add_argument("--apply", action="store_true")
    drain = commands.add_parser("drain-callback-spool")
    drain.add_argument(
        "--spool-root",
        type=Path,
        required=True,
    )
    drain.add_argument(
        "--max-files",
        type=int,
        default=64,
    )
    return parser


def _write_json(output: TextIO, payload: Mapping[str, object]) -> None:
    output.write(
        json.dumps(
            payload,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        )
        + "\n"
    )


if __name__ == "__main__":
    raise SystemExit(main())
