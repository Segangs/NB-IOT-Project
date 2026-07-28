from __future__ import annotations

import sys
from pathlib import Path
from types import ModuleType, SimpleNamespace
import unittest


PROJECT_DIR = Path(__file__).resolve().parents[1]
if str(PROJECT_DIR) not in sys.path:
    sys.path.insert(0, str(PROJECT_DIR))

try:
    import supabase as supabase_module
except ModuleNotFoundError:
    supabase_module = ModuleType("supabase")

if not hasattr(supabase_module, "Client"):
    class Client:
        pass

    def create_client(url: object, key: object) -> Client:
        del url, key
        return Client()

    supabase_module.Client = Client
    supabase_module.create_client = create_client
    sys.modules["supabase"] = supabase_module

import app as app_module


class FakeTable:
    def __init__(self, database: "FakeSupabase", name: str) -> None:
        self.database = database
        self.name = name
        self.filters: list[tuple[str, object]] = []
        self.insert_data: dict[str, object] | None = None

    def select(self, columns: str) -> "FakeTable":
        del columns
        return self

    def eq(self, column: str, value: object) -> "FakeTable":
        self.filters.append((column, value))
        return self

    def limit(self, count: int) -> "FakeTable":
        del count
        return self

    def insert(self, data: dict[str, object]) -> "FakeTable":
        self.insert_data = dict(data)
        return self

    def execute(self) -> SimpleNamespace:
        if self.name == "device":
            if self.database.owner_error is not None:
                raise self.database.owner_error
            rows = [
                dict(row)
                for row in self.database.devices
                if all(row.get(column) == value for column, value in self.filters)
            ]
            return SimpleNamespace(data=rows)
        if self.name == "deviceCmds" and self.insert_data is not None:
            if self.database.insert_error is not None:
                raise self.database.insert_error
            self.database.insert_calls.append(dict(self.insert_data))
            return SimpleNamespace(data=[dict(self.insert_data)])
        raise AssertionError(f"unexpected fake query for table {self.name}")


class FakeSupabase:
    def __init__(self, devices: list[dict[str, object]] | None = None) -> None:
        self.devices = devices or []
        self.insert_calls: list[dict[str, object]] = []
        self.table_calls: list[str] = []
        self.owner_error: Exception | None = None
        self.insert_error: Exception | None = None

    def table(self, name: str) -> FakeTable:
        self.table_calls.append(name)
        return FakeTable(self, name)


class CommandRouteTests(unittest.TestCase):
    def setUp(self) -> None:
        app_module.app.config.update(TESTING=True, SECRET_KEY="command-route-test")
        self.fake_supabase = FakeSupabase(
            devices=[
                {"deviceId": 42, "userId": 7},
                {"deviceId": 43, "userId": 8},
                {"deviceId": 2147483647, "userId": 7},
            ]
        )
        self.original_supabase = app_module.supabase
        app_module.supabase = self.fake_supabase
        self.client = app_module.app.test_client()

    def tearDown(self) -> None:
        app_module.supabase = self.original_supabase

    def login(self, user_id: int = 7) -> None:
        with self.client.session_transaction() as flask_session:
            flask_session["user_id"] = user_id

    def test_login_is_required_before_command_insert(self) -> None:
        response = self.client.post(
            "/send-command",
            json={"deviceId": 42, "cmd": 3},
        )

        self.assertEqual(401, response.status_code)
        self.assertEqual([], self.fake_supabase.insert_calls)

    def test_non_integer_or_out_of_range_opcodes_are_rejected(self) -> None:
        self.login()

        for bad_opcode in (
            None,
            "",
            "1",
            "1.5",
            False,
            True,
            0,
            1.0,
            2.5,
            5,
            10,
            -1,
        ):
            with self.subTest(opcode=bad_opcode):
                response = self.client.post(
                    "/send-command",
                    json={"deviceId": 42, "cmd": bad_opcode},
                )

                self.assertEqual(400, response.status_code)
                self.assertEqual([], self.fake_supabase.insert_calls)

    def test_invalid_device_ids_are_rejected_before_database_insert(self) -> None:
        self.login()

        for bad_device_id in (
            None,
            "",
            "42",
            False,
            True,
            0,
            42.0,
            -1,
            2147483648,
            10**100,
        ):
            with self.subTest(device_id=bad_device_id):
                self.fake_supabase.table_calls.clear()
                response = self.client.post(
                    "/send-command",
                    json={"deviceId": bad_device_id, "cmd": 3},
                )

                self.assertEqual(400, response.status_code)
                self.assertEqual([], self.fake_supabase.insert_calls)
                self.assertEqual([], self.fake_supabase.table_calls)

    def test_largest_postgres_integer_device_id_is_accepted(self) -> None:
        self.login()

        response = self.client.post(
            "/send-command",
            json={"deviceId": 2147483647, "cmd": 3},
        )

        self.assertEqual(200, response.status_code)
        self.assertEqual(
            [{"deviceId": 2147483647, "cmd": 3, "status": 0}],
            self.fake_supabase.insert_calls,
        )

    def test_missing_device_is_not_inserted(self) -> None:
        self.login()

        response = self.client.post(
            "/send-command",
            json={"deviceId": 999, "cmd": 3},
        )

        self.assertEqual(404, response.status_code)
        self.assertEqual([], self.fake_supabase.insert_calls)

    def test_device_owned_by_another_user_is_not_inserted(self) -> None:
        self.login()

        response = self.client.post(
            "/send-command",
            json={"deviceId": 43, "cmd": 3},
        )

        self.assertEqual(404, response.status_code)
        self.assertEqual([], self.fake_supabase.insert_calls)

    def test_valid_opcodes_are_inserted_as_exact_integers(self) -> None:
        self.login()

        for opcode in (1, 2, 3, 4):
            with self.subTest(opcode=opcode):
                self.fake_supabase.insert_calls.clear()

                response = self.client.post(
                    "/send-command",
                    json={"deviceId": 42, "cmd": opcode},
                )

                self.assertEqual(200, response.status_code)
                self.assertEqual(
                    [{"deviceId": 42, "cmd": opcode, "status": 0}],
                    self.fake_supabase.insert_calls,
                )
                self.assertIs(
                    int,
                    type(self.fake_supabase.insert_calls[0]["cmd"]),
                )

    def test_ownership_query_exception_returns_only_generic_error(self) -> None:
        self.login()
        sentinel = "OWNER_SENTINEL DB detail secret=owner-secret"
        self.fake_supabase.owner_error = RuntimeError(sentinel)

        with self.assertLogs(app_module.app.logger, level="ERROR") as logs:
            response = self.client.post(
                "/send-command",
                json={"deviceId": 42, "cmd": 3},
            )
            self.assertEqual(500, response.status_code)
            self.assertEqual(
                {
                    "success": False,
                    "error": "명령 처리 중 오류가 발생했습니다.",
                },
                response.get_json(),
            )
            self.assertNotIn(sentinel, response.get_data(as_text=True))
            self.assertNotIn("DB detail", response.get_data(as_text=True))
            self.assertNotIn("owner-secret", response.get_data(as_text=True))
            self.assertEqual(["device"], self.fake_supabase.table_calls)
            self.assertEqual([], self.fake_supabase.insert_calls)
        self.assertIn("Device command request failed", logs.output[0])

    def test_insert_exception_returns_only_generic_error(self) -> None:
        self.login()
        sentinel = "INSERT_SENTINEL DB detail secret=insert-secret"
        self.fake_supabase.insert_error = RuntimeError(sentinel)

        with self.assertLogs(app_module.app.logger, level="ERROR") as logs:
            response = self.client.post(
                "/send-command",
                json={"deviceId": 42, "cmd": 3},
            )
            self.assertEqual(500, response.status_code)
            self.assertEqual(
                {
                    "success": False,
                    "error": "명령 처리 중 오류가 발생했습니다.",
                },
                response.get_json(),
            )
            self.assertNotIn(sentinel, response.get_data(as_text=True))
            self.assertNotIn("DB detail", response.get_data(as_text=True))
            self.assertNotIn("insert-secret", response.get_data(as_text=True))
            self.assertEqual(
                ["device", "deviceCmds"],
                self.fake_supabase.table_calls,
            )
            self.assertEqual([], self.fake_supabase.insert_calls)
        self.assertIn("Device command request failed", logs.output[0])

    def test_command_modal_renders_the_live_opcode_labels(self) -> None:
        with app_module.app.test_request_context():
            rendered = app_module.render_template("device_status.html", logs=[])

        expected_options = (
            '<option value="1">시스템 재부팅</option>',
            '<option value="2">전원 종료</option>',
            '<option value="3">현재 상태 확인</option>',
            '<option value="4">FOTA 준비(현재 보류)</option>',
        )
        for expected_option in expected_options:
            with self.subTest(option=expected_option):
                self.assertIn(expected_option, rendered)
        self.assertNotIn('<option value="10">', rendered)
        self.assertIn(
            "const deviceId = "
            "Number(document.getElementById('modalDeviceId').value);",
            rendered,
        )
        self.assertIn(
            "const cmd = "
            "Number(document.getElementById('modalCmdSelect').value);",
            rendered,
        )
        self.assertRegex(
            rendered,
            r"body: JSON\.stringify\(\{\s*"
            r"deviceId: deviceId,\s*"
            r"cmd: cmd\s*"
            r"\}\)",
        )


if __name__ == "__main__":
    unittest.main()
