from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
from typing import Mapping, Protocol
from uuid import uuid4

from .callback import CallbackContractError
from .domain import PushAction


_SPOOL_NAME = re.compile(r"[0-9a-f]{64}\.json")
_MAX_SPOOL_BYTES = 16 * 1024
_TERMINAL_ACTIONS = frozenset(
    {
        PushAction.DELIVERED,
        PushAction.DUPLICATE,
        PushAction.FAILED,
        PushAction.RETRY_SCHEDULED,
    }
)


class CallbackRuntime(Protocol):
    def handle_callback_payload_non_sending(
        self,
        payload: Mapping[str, object],
    ) -> PushAction:
        ...


@dataclass(frozen=True)
class CallbackSpoolDrainResult:
    success: bool
    processed: int
    retried: int
    quarantined: int


class CallbackSpoolDrainer:
    def __init__(
        self,
        root: Path,
        runtime: CallbackRuntime,
        *,
        max_files: int = 64,
        max_attempts: int = 5,
    ) -> None:
        if not isinstance(root, Path):
            raise TypeError("spool root must be a Path")
        if not 1 <= max_files <= 1000:
            raise ValueError("max_files must be between 1 and 1000")
        if not 1 <= max_attempts <= 100:
            raise ValueError("max_attempts must be between 1 and 100")
        self._root = root
        self._inbox = root / "inbox"
        self._processing = root / "processing"
        self._quarantine = root / "quarantine"
        for directory in (
            self._root,
            self._inbox,
            self._processing,
            self._quarantine,
        ):
            if not directory.is_dir():
                raise ValueError("callback spool directories are missing")
        self._runtime = runtime
        self._max_files = max_files
        self._max_attempts = max_attempts

    def drain(self) -> CallbackSpoolDrainResult:
        processed = 0
        retried = 0
        quarantined = 0
        selected = (
            self._files(self._processing)
            + self._files(self._inbox)
        )[: self._max_files]

        for source in selected:
            try:
                path = self._claim(source)
            except OSError:
                retried += 1
                continue
            if path is None:
                continue

            try:
                envelope = self._load_envelope(path)
                payload = envelope["payload"]
            except (ValueError, TypeError, json.JSONDecodeError):
                self._move_to_quarantine(path)
                quarantined += 1
                continue

            try:
                action = (
                    self._runtime
                    .handle_callback_payload_non_sending(payload)
                )
            except CallbackContractError:
                self._move_to_quarantine(path)
                quarantined += 1
                continue
            except Exception:
                if self._retry_or_quarantine(path, envelope=envelope):
                    quarantined += 1
                else:
                    retried += 1
                continue

            if action in _TERMINAL_ACTIONS:
                path.unlink(missing_ok=True)
                processed += 1
                continue
            if action is PushAction.UNKNOWN:
                self._move_to_quarantine(path)
                quarantined += 1
                continue

            self._move_to_quarantine(path)
            quarantined += 1

        return CallbackSpoolDrainResult(
            success=retried == 0,
            processed=processed,
            retried=retried,
            quarantined=quarantined,
        )

    @staticmethod
    def _files(directory: Path) -> list[Path]:
        return sorted(
            (
                path
                for path in directory.iterdir()
                if (
                    path.is_file()
                    and not path.name.startswith(".bizp-")
                )
            ),
            key=lambda path: path.name,
        )

    def _claim(self, source: Path) -> Path | None:
        if source.parent == self._processing:
            return source if source.exists() else None
        target = self._processing / source.name
        os.replace(source, target)
        return target

    @staticmethod
    def _load_envelope(path: Path) -> dict[str, object]:
        if _SPOOL_NAME.fullmatch(path.name) is None:
            raise ValueError("unsafe callback spool filename")
        size = path.stat().st_size
        if size < 2 or size > _MAX_SPOOL_BYTES:
            raise ValueError("invalid callback spool size")
        envelope = json.loads(path.read_text(encoding="utf-8"))
        if (
            not isinstance(envelope, dict)
            or set(envelope) != {"version", "attempts", "payload"}
            or envelope.get("version") != 1
        ):
            raise ValueError("invalid callback spool envelope")
        attempts = envelope.get("attempts")
        if (
            isinstance(attempts, bool)
            or not isinstance(attempts, int)
            or attempts < 0
            or attempts > 100
        ):
            raise ValueError("invalid callback spool attempts")
        payload = envelope.get("payload")
        if not isinstance(payload, dict):
            raise ValueError("invalid callback spool payload")
        return envelope

    def _retry_or_quarantine(
        self,
        path: Path,
        *,
        envelope: dict[str, object] | None,
    ) -> bool:
        try:
            current = envelope or self._load_envelope(path)
        except (
            OSError,
            UnicodeError,
            ValueError,
            TypeError,
            json.JSONDecodeError,
        ):
            self._move_to_quarantine(path)
            return True
        attempts = int(current["attempts"]) + 1
        if attempts >= self._max_attempts:
            self._move_to_quarantine(path)
            return True
        updated = {
            "version": 1,
            "attempts": attempts,
            "payload": current["payload"],
        }
        self._write_atomic(path, updated)
        os.replace(path, self._inbox / path.name)
        return False

    def _move_to_quarantine(self, path: Path) -> None:
        destination = self._quarantine / path.name
        suffix = 0
        while destination.exists():
            suffix += 1
            destination = self._quarantine / (
                f"{path.name}.{suffix}"
            )
        os.replace(path, destination)

    @staticmethod
    def _write_atomic(
        target: Path,
        payload: Mapping[str, object],
    ) -> None:
        temporary = target.with_name(
            f".{target.name}.{uuid4().hex}.tmp"
        )
        rendered = json.dumps(
            payload,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        )
        try:
            with temporary.open("x", encoding="utf-8") as handle:
                os.chmod(temporary, 0o600)
                handle.write(rendered)
                handle.flush()
                os.fsync(handle.fileno())
            os.replace(temporary, target)
        finally:
            temporary.unlink(missing_ok=True)
