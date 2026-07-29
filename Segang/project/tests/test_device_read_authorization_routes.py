from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from types import ModuleType, SimpleNamespace
import sys
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
from limited_links import LimitedSessionGrant, LinkPurpose
from werkzeug.test import ClientRedirectError


NOW = datetime(2026, 7, 29, 12, 0, tzinfo=timezone.utc)
SESSION_ID = "limited_session_reference_1234567890abcdef"
CSRF_TOKEN = "limited_csrf_reference_1234567890abcdefgh"
BASE_URL = "https://localhost"


@dataclass(frozen=True)
class QueryRecord:
    table: str
    operations: tuple[tuple[str, object], ...]


class FakeQuery:
    def __init__(self, database: "FakeSupabase", table: str) -> None:
        self.database = database
        self.table = table
        self.operations: list[tuple[str, object]] = []

    def select(self, columns: str) -> "FakeQuery":
        self.operations.append(("select", columns))
        return self

    def eq(self, column: str, value: object) -> "FakeQuery":
        self.operations.append(("eq", (column, value)))
        return self

    def in_(self, column: str, values: object) -> "FakeQuery":
        self.operations.append(("in", (column, tuple(values))))
        return self

    def gte(self, column: str, value: object) -> "FakeQuery":
        self.operations.append(("gte", (column, value)))
        return self

    def order(self, column: str, *, desc: bool = False) -> "FakeQuery":
        self.operations.append(("order", (column, desc)))
        return self

    def limit(self, count: int) -> "FakeQuery":
        self.operations.append(("limit", count))
        return self

    def execute(self) -> SimpleNamespace:
        self.database.executed.append(
            QueryRecord(self.table, tuple(self.operations))
        )
        rows = [dict(row) for row in self.database.rows.get(self.table, ())]
        explicit_limit = False
        for operation, argument in self.operations:
            if operation == "eq":
                column, value = argument
                rows = [row for row in rows if row.get(column) == value]
            elif operation == "in":
                column, values = argument
                rows = [row for row in rows if row.get(column) in values]
            elif operation == "gte":
                column, value = argument
                rows = [
                    row for row in rows
                    if row.get(column) is not None and row[column] >= value
                ]
            elif operation == "order":
                column, descending = argument
                rows.sort(
                    key=lambda row: row.get(column),
                    reverse=descending,
                )
            elif operation == "limit":
                rows = rows[:argument]
                explicit_limit = True
        if not explicit_limit:
            rows = rows[:self.database.max_rows_per_response]
        return SimpleNamespace(data=rows)


class FakeSupabase:
    def __init__(self) -> None:
        self.rows = {
            "device": [
                {
                    "deviceId": 42,
                    "deviceIMEI": "OWN-IMEI",
                    "userId": 7,
                    "usimId": 5,
                    "userWorkplaceId": 8,
                },
                {
                    "deviceId": 43,
                    "deviceIMEI": "FOREIGN-IMEI",
                    "userId": 8,
                    "usimId": 6,
                    "userWorkplaceId": 9,
                },
            ],
            "usim": [
                {"usimId": 5, "usimIMSI": "OWN-IMSI"},
                {"usimId": 6, "usimIMSI": "FOREIGN-IMSI"},
            ],
            "device_boot_logs": [
                {
                    "id": 1,
                    "deviceId": 42,
                    "userId": 999,
                    "boottime": "2026-07-29 11:55:00",
                    "bootReasonCode": 1,
                    "cmdId": 1001,
                    "pico_voltage": 3.2,
                    "temperature": 4.0,
                    "flash_integrity": 0,
                    "ram_test": 0,
                    "at_status": 0,
                    "cpin_status": 0,
                    "csq_rssi": 20,
                    "temp_sensor_status": 0,
                    "tmp1_status": 0,
                    "tmp2_status": 0,
                    "mic1_status": 0,
                    "mic2_status": 0,
                },
                {
                    "id": 2,
                    "deviceId": 43,
                    "userId": 8,
                    "boottime": "2026-07-29 11:56:00",
                    "bootReasonCode": 1,
                    "cmdId": 1002,
                    "pico_voltage": 3.2,
                    "temperature": 5.0,
                    "flash_integrity": 0,
                    "ram_test": 0,
                    "at_status": 0,
                    "cpin_status": 0,
                    "csq_rssi": 20,
                    "temp_sensor_status": 0,
                    "tmp1_status": 0,
                    "tmp2_status": 0,
                    "mic1_status": 0,
                    "mic2_status": 0,
                },
            ],
            "deviceCmds": [
                {"cmdId": 1001, "created_at": "2026-07-29T11:54:00"},
                {"cmdId": 1002, "created_at": "2026-07-29T11:55:00"},
            ],
            "USER_SENSOR": [
                {
                    "Id": 8,
                    "userSensorId": 2,
                    "deviceId": 42,
                    "sensorCtgyId": 1,
                    "setTmpUpLimit": 50.0,
                    "setTmpLowLimit": -20.0,
                },
                {
                    "Id": 7,
                    "userSensorId": 1,
                    "deviceId": 42,
                    "sensorCtgyId": 1,
                    "setTmpUpLimit": 10.0,
                    "setTmpLowLimit": -20.0,
                },
                {
                    "Id": 9,
                    "userSensorId": 1,
                    "deviceId": 43,
                    "sensorCtgyId": 1,
                    "setTmpUpLimit": 10.0,
                    "setTmpLowLimit": -20.0,
                },
            ],
            "SENSOR_CTGY": [
                {
                    "sensorCtgyId": 1,
                    "sensorCtgyType": "TMP",
                    "sensorCtgyModel": "DS18B20",
                }
            ],
            "sensorvalue": [
                {
                    "sensorValueId": 71,
                    "sensorId": 7,
                    "sensorValue": 14.0,
                    "sensorvaluetime": "2026-07-29T11:58:00",
                },
                {
                    "sensorValueId": 81,
                    "sensorId": 8,
                    "sensorValue": 5.0,
                    "sensorvaluetime": "2026-07-29T11:57:00",
                },
                {
                    "sensorValueId": 91,
                    "sensorId": 9,
                    "sensorValue": 16.0,
                    "sensorvaluetime": "2026-07-29T11:59:00",
                },
            ],
            "usermachine": [
                {
                    "userMachineId": 420,
                    "deviceId": 42,
                    "machineId": 100,
                    "userMachineName": "OWN-MACHINE",
                },
                {
                    "userMachineId": 430,
                    "deviceId": 43,
                    "machineId": 101,
                    "userMachineName": "FOREIGN-MACHINE",
                },
            ],
            "machine": [
                {"machineId": 100, "modelName": "OWN-MODEL", "systemType": "냉장"},
                {
                    "machineId": 101,
                    "modelName": "FOREIGN-MODEL",
                    "systemType": "냉동",
                },
            ],
            "users": [
                {"userId": 7, "userName": "OWNER"},
                {"userId": 8, "userName": "FOREIGN"},
                {"userId": 999, "userName": "SPOOFED"},
            ],
            "userworkplace": [
                {
                    "userWorkplaceId": 8,
                    "userId": 7,
                    "WorkplaceAddress": "OWN-ADDRESS",
                    "WorkplaceName": "OWN-WORKPLACE",
                },
                {
                    "userWorkplaceId": 9,
                    "userId": 8,
                    "WorkplaceAddress": "FOREIGN-ADDRESS",
                    "WorkplaceName": "FOREIGN-WORKPLACE",
                },
            ],
        }
        self.max_rows_per_response = 1000
        self.executed: list[QueryRecord] = []

    def table(self, table: str) -> FakeQuery:
        if table not in self.rows:
            raise AssertionError(f"unexpected fake table: {table}")
        return FakeQuery(self, table)


class StubMessageServices:
    def __init__(self, grant: LimitedSessionGrant | None = None) -> None:
        self.grant = grant
        self.resolve_calls: list[object] = []

    def resolve_limited_session(
        self,
        session_id: object,
    ) -> LimitedSessionGrant | None:
        self.resolve_calls.append(session_id)
        if session_id != SESSION_ID:
            return None
        return self.grant


def history_grant(**changes: object) -> LimitedSessionGrant:
    values = {
        "purpose": LinkPurpose.TEMP_HISTORY,
        "target_path": "/device-temp-history/42",
        "user_id": 7,
        "workplace_id": 8,
        "device_id": 42,
        "sensor_ids": (7,),
        "expires_at": NOW + timedelta(minutes=15),
        "csrf_token": CSRF_TOKEN,
    }
    values.update(changes)
    return LimitedSessionGrant(**values)


class DeviceReadAuthorizationRouteTests(unittest.TestCase):
    def setUp(self) -> None:
        app_module.app.config.update(
            TESTING=True,
            SECRET_KEY="device-read-authorization-test",
        )
        self.database = FakeSupabase()
        self.services = StubMessageServices()
        self.rendered: list[tuple[str, dict[str, object]]] = []
        self.original_supabase = app_module.supabase
        self.original_services = app_module.message_services
        self.original_render_template = app_module.render_template
        app_module.supabase = self.database
        app_module.message_services = self.services

        def capture_template(
            template_name: str,
            **context: object,
        ) -> str:
            self.rendered.append((template_name, context))
            return template_name

        app_module.render_template = capture_template
        self.client = app_module.app.test_client()

    def tearDown(self) -> None:
        app_module.supabase = self.original_supabase
        app_module.message_services = self.original_services
        app_module.render_template = self.original_render_template

    def login(self, user_id: int = 7, level: int = 1) -> None:
        with self.client.session_transaction() as flask_session:
            flask_session["user_id"] = user_id
            flask_session["level"] = level
            flask_session["user_name"] = (
                "ADMIN" if level == 0 else "OWNER"
            )

    def token_only(self) -> None:
        with self.client.session_transaction() as flask_session:
            flask_session["supabase_token"] = "token-without-public-user"

    def set_limited_cookie(self) -> None:
        self.client.set_cookie(
            "__Host-limited_session",
            SESSION_ID,
            domain="localhost",
            secure=True,
            httponly=True,
            samesite="Lax",
        )

    def context_for(self, template_name: str) -> dict[str, object]:
        matching = [
            context
            for name, context in self.rendered
            if name == template_name
        ]
        self.assertTrue(matching, f"{template_name} was not rendered")
        return matching[-1]

    def records_for(self, table: str) -> list[QueryRecord]:
        return [
            record
            for record in self.database.executed
            if record.table == table
        ]

    def assert_filter(
        self,
        record: QueryRecord,
        operation: str,
        column: str,
        value: object,
    ) -> None:
        self.assertIn((operation, (column, value)), record.operations)

    def test_regular_device_list_scopes_devices_before_usims(self) -> None:
        self.login()

        response = self.client.get("/devices")

        self.assertEqual(200, response.status_code)
        devices = self.context_for("devices.html")["devices"]
        self.assertEqual([42], [device["deviceId"] for device in devices])
        self.assertEqual("OWN-IMSI", devices[0]["usim_imsi"])
        self.assertEqual(["device", "usim"], [
            record.table for record in self.database.executed
        ])
        self.assert_filter(
            self.database.executed[0],
            "eq",
            "userId",
            7,
        )
        self.assert_filter(
            self.database.executed[1],
            "in",
            "usimId",
            (5,),
        )

    def test_admin_device_list_retains_all_devices(self) -> None:
        self.login(user_id=1, level=0)

        response = self.client.get("/devices")

        self.assertEqual(200, response.status_code)
        devices = self.context_for("devices.html")["devices"]
        self.assertEqual([42, 43], [device["deviceId"] for device in devices])
        self.assertNotIn(
            ("eq", ("userId", 1)),
            self.database.executed[0].operations,
        )

    def test_token_only_dashboard_follow_redirects_to_login_without_loop(
        self,
    ) -> None:
        self.token_only()

        try:
            response = self.client.get(
                "/dashboard",
                follow_redirects=True,
            )
        except ClientRedirectError as error:
            self.fail(f"token-only redirect loop: {error}")

        self.assertEqual(200, response.status_code)
        self.assertEqual("/auth/login", response.request.path)
        self.assertEqual("login.html", self.rendered[-1][0])
        self.assertEqual([], self.database.executed)

    def test_token_only_can_enter_login_and_register_pages(self) -> None:
        self.token_only()

        for path, template_name in (
            ("/auth/login", "login.html"),
            ("/auth/register", "register.html"),
        ):
            with self.subTest(path=path):
                self.rendered.clear()
                response = self.client.get(path)
                self.assertEqual(200, response.status_code)
                self.assertEqual(template_name, self.rendered[-1][0])

    def test_regular_dashboard_omits_realtime_credentials_for_rls_off(
        self,
    ) -> None:
        original_url = app_module.SUPABASE_URL
        original_key = app_module.SUPABASE_KEY
        app_module.SUPABASE_URL = "https://scope-test.supabase.co"
        app_module.SUPABASE_KEY = "scope-test-anon-key"
        try:
            self.login()
            regular = self.client.get("/dashboard")
            regular_context = self.context_for("dashboard.html")

            self.assertEqual(200, regular.status_code)
            self.assertEqual("", regular_context["supabase_url"])
            self.assertEqual("", regular_context["supabase_key"])

            self.rendered.clear()
            self.login(user_id=1, level=0)
            admin = self.client.get("/dashboard")
            admin_context = self.context_for("dashboard.html")

            self.assertEqual(200, admin.status_code)
            self.assertEqual(
                "https://scope-test.supabase.co",
                admin_context["supabase_url"],
            )
            self.assertEqual(
                "scope-test-anon-key",
                admin_context["supabase_key"],
            )
        finally:
            app_module.SUPABASE_URL = original_url
            app_module.SUPABASE_KEY = original_key

    def test_device_status_scopes_parent_before_logs_and_uses_device_owner(
        self,
    ) -> None:
        self.login()

        response = self.client.get("/device-status")

        self.assertEqual(200, response.status_code)
        logs = self.context_for("device_status.html")["logs"]
        self.assertEqual([42], [log["device_id"] for log in logs])
        self.assertEqual("OWNER", logs[0]["user_name"])
        self.assertEqual("device", self.database.executed[0].table)
        boot_query = self.records_for("device_boot_logs")[0]
        self.assert_filter(
            boot_query,
            "in",
            "deviceId",
            (42,),
        )
        command_query = self.records_for("deviceCmds")[0]
        self.assert_filter(
            command_query,
            "in",
            "cmdId",
            (1001,),
        )

    def test_temperature_status_scopes_device_then_sensor_values(self) -> None:
        self.login()

        response = self.client.get("/temp-status")

        self.assertEqual(200, response.status_code)
        temps = self.context_for("temp_status.html")["temps"]
        self.assertEqual({42}, {item["device_id"] for item in temps})
        self.assertEqual("device", self.database.executed[0].table)
        self.assertEqual("USER_SENSOR", self.database.executed[1].table)
        self.assert_filter(
            self.database.executed[1],
            "in",
            "deviceId",
            (42,),
        )
        value_query = self.records_for("sensorvalue")[0]
        self.assert_filter(
            value_query,
            "in",
            "sensorId",
            (7, 8),
        )

    def test_national_temperature_api_returns_only_owned_sensor_rows(
        self,
    ) -> None:
        self.login()

        response = self.client.get("/api/national-temperatures")

        self.assertEqual(200, response.status_code)
        payload = response.get_json()
        self.assertTrue(payload["success"])
        self.assertEqual(2, len(payload["data"]))
        self.assertEqual({"OWNER"}, {
            item["userName"] for item in payload["data"]
        })
        self.assertEqual("device", self.database.executed[0].table)
        self.assert_filter(
            self.records_for("sensorvalue")[0],
            "in",
            "sensorId",
            (7, 8),
        )

    def test_status_api_scopes_latest_aggregates_alerts_and_logs(self) -> None:
        self.login()

        response = self.client.get("/api/status")

        self.assertEqual(200, response.status_code)
        payload = response.get_json()
        self.assertTrue(payload["success"])
        self.assertEqual(1, payload["total_devices"])
        self.assertEqual(1, payload["active_devices"])
        self.assertEqual(1, len(payload["alerts"]))
        self.assertEqual("OWNER", payload["alerts"][0]["user_name"])
        self.assertNotIn("FOREIGN", str(payload))
        self.assertNotIn("FOREIGN-IMEI", str(payload))
        self.assertEqual("device", self.database.executed[0].table)
        for record in self.records_for("device_boot_logs"):
            self.assertTrue(
                ("in", ("deviceId", (42,))) in record.operations
                or ("eq", ("deviceId", 42)) in record.operations,
                record,
            )
        for record in self.records_for("sensorvalue"):
            self.assert_filter(
                record,
                "in",
                "sensorId",
                (7, 8),
            )

    def test_status_average_queries_scoped_latest_fifty_in_database(
        self,
    ) -> None:
        old_rows = [
            {
                "sensorValueId": index,
                "sensorId": 7,
                "sensorValue": 1.0,
                "sensorvaluetime": "2026-07-29T01:00:00",
            }
            for index in range(1, 1001)
        ]
        newest_owner_rows = [
            {
                "sensorValueId": index,
                "sensorId": 8,
                "sensorValue": 20.0,
                "sensorvaluetime": "2026-07-29T11:58:00",
            }
            for index in range(1001, 1051)
        ]
        newest_foreign_rows = [
            {
                "sensorValueId": index,
                "sensorId": 9,
                "sensorValue": 80.0,
                "sensorvaluetime": "2026-07-29T11:59:00",
            }
            for index in range(1051, 1101)
        ]
        self.database.rows["sensorvalue"] = (
            old_rows + newest_owner_rows + newest_foreign_rows
        )

        cases = (
            (7, 1, (7, 8), 20.0),
            (1, 0, (7, 8, 9), 80.0),
        )
        for user_id, level, sensor_ids, expected_average in cases:
            with self.subTest(level=level):
                self.database.executed.clear()
                self.login(user_id=user_id, level=level)

                response = self.client.get("/api/status")

                self.assertEqual(200, response.status_code)
                self.assertEqual(
                    expected_average,
                    response.get_json()["avg_temp"],
                )
                average_queries = [
                    record
                    for record in self.records_for("sensorvalue")
                    if ("select", "sensorValue") in record.operations
                ]
                self.assertEqual(1, len(average_queries))
                self.assertEqual(
                    (
                        ("select", "sensorValue"),
                        ("in", ("sensorId", sensor_ids)),
                        ("order", ("sensorValueId", True)),
                        ("limit", 50),
                    ),
                    average_queries[0].operations,
                )

    def test_status_with_no_scoped_sensors_skips_sensorvalue_queries(
        self,
    ) -> None:
        self.database.rows["USER_SENSOR"] = [
            row
            for row in self.database.rows["USER_SENSOR"]
            if row["deviceId"] == 43
        ]
        self.login()

        response = self.client.get("/api/status")

        self.assertEqual(200, response.status_code)
        self.assertEqual(0, response.get_json()["avg_temp"])
        self.assertEqual([], self.records_for("sensorvalue"))

    def test_empty_regular_scope_returns_empty_without_child_queries(
        self,
    ) -> None:
        cases = (
            ("/device-status", "device_status.html", "logs"),
            ("/temp-status", "temp_status.html", "temps"),
        )
        for path, template, key in cases:
            with self.subTest(path=path):
                self.database.executed.clear()
                self.rendered.clear()
                self.login(user_id=404)

                response = self.client.get(path)

                self.assertEqual(200, response.status_code)
                self.assertEqual([], self.context_for(template)[key])
                self.assertEqual(
                    ["device"],
                    [record.table for record in self.database.executed],
                )

        for path in (
            "/api/national-temperatures",
            "/api/status",
        ):
            with self.subTest(path=path):
                self.database.executed.clear()
                self.login(user_id=404)

                response = self.client.get(path)

                self.assertEqual(200, response.status_code)
                payload = response.get_json()
                self.assertTrue(payload["success"])
                if path == "/api/national-temperatures":
                    self.assertEqual([], payload["data"])
                else:
                    self.assertEqual(0, payload["total_devices"])
                    self.assertEqual(0, payload["active_devices"])
                    self.assertEqual(0, payload["avg_temp"])
                    self.assertEqual([], payload["alerts"])
                    self.assertEqual([], payload["logs"])
                    self.assertEqual([], payload["chart_values"])
                self.assertEqual(
                    ["device"],
                    [record.table for record in self.database.executed],
                )

    def test_foreign_history_matches_missing_without_child_queries(self) -> None:
        self.login()

        foreign = self.client.get("/device-temp-history/43")
        foreign_queries = list(self.database.executed)
        self.database.executed.clear()
        missing = self.client.get("/device-temp-history/404")
        missing_queries = list(self.database.executed)

        self.assertEqual(missing.status_code, foreign.status_code)
        self.assertEqual(
            missing.headers.get("Location"),
            foreign.headers.get("Location"),
        )
        self.assertEqual(["device"], [item.table for item in foreign_queries])
        self.assertEqual(["device"], [item.table for item in missing_queries])
        self.assert_filter(foreign_queries[0], "eq", "deviceId", 43)
        self.assert_filter(foreign_queries[0], "eq", "userId", 7)

    def test_history_owner_and_admin_can_read_authorized_device(self) -> None:
        cases = ((7, 1, 42), (1, 0, 43))
        for user_id, level, device_id in cases:
            with self.subTest(level=level, device_id=device_id):
                self.database.executed.clear()
                self.rendered.clear()
                self.login(user_id=user_id, level=level)

                response = self.client.get(
                    f"/device-temp-history/{device_id}"
                )

                self.assertEqual(200, response.status_code)
                context = self.context_for("device_temp_history.html")
                self.assertEqual(device_id, context["device"]["device_id"])

    def test_valid_limited_history_uses_exact_device_and_sensor_grant(
        self,
    ) -> None:
        self.services.grant = history_grant()
        self.set_limited_cookie()

        response = self.client.get(
            "/device-temp-history/42",
            base_url=BASE_URL,
        )

        self.assertEqual(200, response.status_code)
        self.assertEqual([SESSION_ID], self.services.resolve_calls)
        device_query = self.database.executed[0]
        self.assertEqual("device", device_query.table)
        self.assert_filter(device_query, "eq", "deviceId", 42)
        self.assert_filter(device_query, "eq", "userId", 7)
        self.assert_filter(device_query, "eq", "userWorkplaceId", 8)
        value_query = self.records_for("sensorvalue")[0]
        self.assert_filter(value_query, "in", "sensorId", (7,))
        context = self.context_for("device_temp_history.html")
        self.assertEqual(10.0, context["device"]["upper_limit"])
        history = context["history"]
        self.assertEqual(1, len(history))
        self.assertEqual("TMP1", history[0]["sensor_label"])

    def test_invalid_limited_history_grants_fail_closed(self) -> None:
        cases = (
            history_grant(purpose=LinkPurpose.SETTINGS),
            history_grant(target_path="/device-temp-history/43"),
            history_grant(device_id=43),
            history_grant(sensor_ids=()),
            history_grant(sensor_ids=(999,)),
        )
        for grant in cases:
            with self.subTest(grant=grant):
                self.database.executed.clear()
                self.services.resolve_calls.clear()
                self.services.grant = grant
                self.set_limited_cookie()

                response = self.client.get(
                    "/device-temp-history/42",
                    base_url=BASE_URL,
                )

                self.assertEqual(404, response.status_code)
                self.assertEqual([], self.records_for("sensorvalue"))

    def test_regular_session_ignores_limited_cookie(self) -> None:
        self.services.grant = history_grant(
            user_id=8,
            workplace_id=9,
            device_id=43,
            target_path="/device-temp-history/43",
            sensor_ids=(9,),
        )
        self.login()
        self.set_limited_cookie()

        response = self.client.get(
            "/device-temp-history/42",
            base_url=BASE_URL,
        )

        self.assertEqual(200, response.status_code)
        self.assertEqual([], self.services.resolve_calls)
        self.assert_filter(
            self.database.executed[0],
            "eq",
            "userId",
            7,
        )

    def test_token_only_session_is_not_a_device_read_principal(self) -> None:
        self.services.grant = history_grant()
        self.token_only()
        self.set_limited_cookie()

        html_paths = (
            "/dashboard",
            "/devices",
            "/device-status",
            "/temp-status",
            "/device-temp-history/42",
            "/national-temperatures",
        )
        for path in html_paths:
            with self.subTest(path=path):
                self.database.executed.clear()
                response = self.client.get(path, base_url=BASE_URL)
                self.assertEqual(302, response.status_code)
                self.assertTrue(
                    response.headers["Location"].endswith("/auth/login")
                )
                self.assertEqual([], self.database.executed)

        for path in ("/api/national-temperatures", "/api/status"):
            with self.subTest(path=path):
                self.database.executed.clear()
                response = self.client.get(path, base_url=BASE_URL)
                self.assertEqual(401, response.status_code)
                self.assertEqual([], self.database.executed)

        self.assertEqual([], self.services.resolve_calls)
        with app_module.app.test_request_context():
            app_module.session["supabase_token"] = "still-logged-in-globally"
            self.assertTrue(app_module.is_logged_in())

    def test_unauthenticated_device_pages_and_apis_fail_before_queries(
        self,
    ) -> None:
        for path in (
            "/dashboard",
            "/devices",
            "/device-status",
            "/temp-status",
            "/device-temp-history/42",
            "/national-temperatures",
        ):
            with self.subTest(path=path):
                self.database.executed.clear()
                response = self.client.get(path)
                self.assertEqual(302, response.status_code)
                self.assertEqual([], self.database.executed)

        for path in ("/api/national-temperatures", "/api/status"):
            with self.subTest(path=path):
                self.database.executed.clear()
                response = self.client.get(path)
                self.assertEqual(401, response.status_code)
                self.assertEqual([], self.database.executed)


if __name__ == "__main__":
    unittest.main()
