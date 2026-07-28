from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from msg_send.callback import CallbackContractError
from msg_send.domain import PushAction
from msg_send.webhook_spool import CallbackSpoolDrainer


REFKEY = "1234567812344abc8def1234567890ab"


def callback_payload(
    *,
    result_id: str = "provider-result-1",
    result_code: str = "7000",
) -> dict[str, object]:
    return {
        "CMSGID": "provider-request-1",
        "REFKEY": REFKEY,
        "MSGID": result_id,
        "UNIXTIME": "1785038400",
        "RESULT": result_code,
        "MEDIA": "AT",
    }


class FakeCallbackRuntime:
    def __init__(
        self,
        actions: list[PushAction] | None = None,
        error: Exception | None = None,
    ) -> None:
        self.actions = list(actions or [PushAction.DELIVERED])
        self.error = error
        self.payloads: list[dict[str, object]] = []

    def handle_callback_payload_non_sending(
        self,
        payload: dict[str, object],
    ) -> PushAction:
        self.payloads.append(payload)
        if self.error is not None:
            raise self.error
        return self.actions.pop(0)


class CallbackSpoolDrainerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        for name in ("inbox", "processing", "quarantine"):
            (self.root / name).mkdir()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _write(
        self,
        directory: str,
        name: str,
        *,
        payload: dict[str, object] | None = None,
        attempts: int = 0,
    ) -> Path:
        path = self.root / directory / name
        path.write_text(
            json.dumps(
                {
                    "version": 1,
                    "attempts": attempts,
                    "payload": payload or callback_payload(),
                }
            ),
            encoding="utf-8",
        )
        return path

    def test_delivered_duplicate_and_failed_results_are_consumed(
        self,
    ) -> None:
        runtime = FakeCallbackRuntime(
            [
                PushAction.DELIVERED,
                PushAction.DUPLICATE,
                PushAction.FAILED,
            ]
        )
        for index in range(3):
            self._write(
                "inbox",
                f"{index:064x}.json",
                payload=callback_payload(
                    result_id=f"provider-result-{index}"
                ),
            )

        result = CallbackSpoolDrainer(
            self.root,
            runtime,  # type: ignore[arg-type]
            max_files=8,
        ).drain()

        self.assertTrue(result.success)
        self.assertEqual(result.processed, 3)
        self.assertEqual(result.retried, 0)
        self.assertEqual(result.quarantined, 0)
        self.assertEqual(len(runtime.payloads), 3)
        self.assertEqual(list((self.root / "inbox").iterdir()), [])
        self.assertEqual(list((self.root / "processing").iterdir()), [])

    def test_unknown_result_is_quarantined_as_correlation_failure(
        self,
    ) -> None:
        path = self._write("inbox", f"{1:064x}.json")
        runtime = FakeCallbackRuntime([PushAction.UNKNOWN])

        result = CallbackSpoolDrainer(
            self.root,
            runtime,  # type: ignore[arg-type]
            max_files=8,
            max_attempts=3,
        ).drain()

        self.assertTrue(result.success)
        self.assertEqual(result.processed, 0)
        self.assertEqual(result.retried, 0)
        self.assertEqual(result.quarantined, 1)
        self.assertFalse(path.exists())
        self.assertTrue(
            (self.root / "quarantine" / path.name).is_file()
        )

    def test_transient_failure_returns_file_to_inbox(
        self,
    ) -> None:
        path = self._write("inbox", f"{8:064x}.json")
        runtime = FakeCallbackRuntime(error=RuntimeError("temporary"))
        result = CallbackSpoolDrainer(
            self.root,
            runtime,  # type: ignore[arg-type]
            max_files=8,
            max_attempts=3,
        ).drain()
        self.assertFalse(result.success)
        self.assertEqual(result.retried, 1)
        retried = self.root / "inbox" / path.name
        envelope = json.loads(retried.read_text(encoding="utf-8"))
        self.assertEqual(envelope["attempts"], 1)

    def test_runtime_value_error_is_retried_not_quarantined(
        self,
    ) -> None:
        path = self._write("inbox", f"{9:064x}.json")
        runtime = FakeCallbackRuntime(
            error=ValueError("temporary SDK shape failure")
        )

        result = CallbackSpoolDrainer(
            self.root,
            runtime,  # type: ignore[arg-type]
            max_files=8,
            max_attempts=3,
        ).drain()

        self.assertFalse(result.success)
        self.assertEqual(result.retried, 1)
        self.assertEqual(result.quarantined, 0)
        self.assertTrue((self.root / "inbox" / path.name).is_file())

    def test_repeated_transient_failure_is_quarantined(self) -> None:
        path = self._write(
            "inbox",
            f"{2:064x}.json",
            attempts=2,
        )
        runtime = FakeCallbackRuntime(
            error=RuntimeError("repeated temporary failure")
        )

        result = CallbackSpoolDrainer(
            self.root,
            runtime,  # type: ignore[arg-type]
            max_files=8,
            max_attempts=3,
        ).drain()

        self.assertTrue(result.success)
        self.assertEqual(result.retried, 0)
        self.assertEqual(result.quarantined, 1)
        self.assertFalse(path.exists())
        self.assertTrue(
            (self.root / "quarantine" / path.name).is_file()
        )

    def test_contract_error_and_invalid_envelope_are_quarantined(
        self,
    ) -> None:
        invalid_json = self.root / "inbox" / f"{3:064x}.json"
        invalid_json.write_text("{", encoding="utf-8")
        self._write("inbox", f"{4:064x}.json")
        runtime = FakeCallbackRuntime(
            error=CallbackContractError("invalid")
        )

        result = CallbackSpoolDrainer(
            self.root,
            runtime,  # type: ignore[arg-type]
            max_files=8,
        ).drain()

        self.assertTrue(result.success)
        self.assertEqual(result.processed, 0)
        self.assertEqual(result.quarantined, 2)
        self.assertEqual(len(runtime.payloads), 1)

    def test_stranded_processing_file_is_recovered_and_batch_is_bounded(
        self,
    ) -> None:
        self._write("processing", f"{5:064x}.json")
        self._write("inbox", f"{6:064x}.json")
        runtime = FakeCallbackRuntime(
            [PushAction.DELIVERED, PushAction.DELIVERED]
        )

        result = CallbackSpoolDrainer(
            self.root,
            runtime,  # type: ignore[arg-type]
            max_files=1,
        ).drain()

        self.assertTrue(result.success)
        self.assertEqual(result.processed, 1)
        self.assertEqual(len(runtime.payloads), 1)
        self.assertEqual(
            len(list((self.root / "inbox").glob("*.json"))),
            1,
        )

    def test_unsafe_filename_and_oversize_file_are_quarantined(
        self,
    ) -> None:
        self._write("inbox", "not-a-digest.json")
        oversized = self.root / "inbox" / f"{7:064x}.json"
        oversized.write_bytes(b"{" + b"x" * 17000 + b"}")
        runtime = FakeCallbackRuntime()

        result = CallbackSpoolDrainer(
            self.root,
            runtime,  # type: ignore[arg-type]
            max_files=8,
        ).drain()

        self.assertTrue(result.success)
        self.assertEqual(result.quarantined, 2)
        self.assertEqual(runtime.payloads, [])

    def test_in_progress_php_temporary_file_is_ignored(self) -> None:
        temporary = self.root / "inbox" / ".bizp-in-progress"
        temporary.write_text("partial", encoding="utf-8")
        runtime = FakeCallbackRuntime()

        result = CallbackSpoolDrainer(
            self.root,
            runtime,  # type: ignore[arg-type]
            max_files=8,
        ).drain()

        self.assertTrue(result.success)
        self.assertEqual(result.processed, 0)
        self.assertEqual(result.quarantined, 0)
        self.assertTrue(temporary.is_file())
        self.assertEqual(runtime.payloads, [])


if __name__ == "__main__":
    unittest.main()
