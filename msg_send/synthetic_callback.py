from __future__ import annotations

import json
from pathlib import Path
from typing import Mapping

from .callback import CallbackContractError


_MAX_SYNTHETIC_PAYLOAD_BYTES = 64 * 1024


def load_synthetic_payload(path: Path) -> Mapping[str, object]:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise CallbackContractError(
            "synthetic callback input could not be read"
        ) from exc
    if not raw or len(raw) > _MAX_SYNTHETIC_PAYLOAD_BYTES:
        raise CallbackContractError(
            "synthetic callback input size is invalid"
        )
    try:
        payload = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise CallbackContractError(
            "synthetic callback input is invalid JSON"
        ) from exc
    if not isinstance(payload, dict):
        raise CallbackContractError(
            "synthetic callback input must be an object"
        )
    return payload
