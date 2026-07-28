import contextlib
import io
import unittest

from Segang.project.tests.test_emqx_command_setup import InMemoryEmqx
from Segang.project.emqx_event_setup import (
    RESOURCE_IDS,
    build_event_action,
    build_event_rule,
    main,
    parse_args,
)


ENV = {
    "SUPABASE_URL": "https://project.supabase.co",
    "SUPABASE_KEY": "ANON_KEY_VALUE",
    "EMQX_API": "http://emqx.local:18083/api/v5",
    "EMQX_API_KEY": "api-key",
    "EMQX_API_SECRET": "api-secret",
    "EMQX_EVENT_GATEWAY_SECRET": "EVENT_GATEWAY_SECRET",
}


def run_main(argv, emqx, environ=ENV):
    output = io.StringIO()
    with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
        result = main(argv, environ=environ, transport=emqx)
    return result, output.getvalue()


class EmqxEventDefinitionTest(unittest.TestCase):
    def test_exact_resource_ids_and_disabled_action(self):
        self.assertEqual(
            RESOURCE_IDS,
            {
                "event_action": "supabase_power_event",
                "event_rule": "power_event_rule",
            },
        )
        action = build_event_action("EVENT_GATEWAY_SECRET", False)
        self.assertEqual(action["connector"], "supabase_api")
        self.assertFalse(action["enable"])
        self.assertEqual(action["parameters"]["max_retries"], 0)
        self.assertEqual(
            action["parameters"]["path"],
            "/rest/v1/rpc/ingest_device_power_event",
        )
        self.assertIn(
            '"p_request_secret":"EVENT_GATEWAY_SECRET"',
            action["parameters"]["body"],
        )

    def test_rule_decodes_exact_nine_integer_fields(self):
        rule = build_event_rule(False)
        sql = rule["sql"]
        self.assertFalse(rule["enable"])
        self.assertEqual(rule["actions"], ["http:supabase_power_event"])
        self.assertIn('FROM "devices/+/event"', sql)
        self.assertIn("length(json_decode(payload)) = 9", sql)
        self.assertIn("flags.retain = false", sql)
        self.assertIn("nth(2, split(topic, '/')) = clientid", sql)
        for position in range(1, 10):
            with self.subTest(position=position):
                self.assertIn(
                    f"is_int(nth({position}, json_decode(payload)))",
                    sql,
                )
        for alias, position in (
            ("p_schema", 1),
            ("p_event_type", 2),
            ("p_incident_id", 3),
            ("p_sequence", 4),
            ("p_state", 5),
            ("p_value0", 6),
            ("p_value1", 7),
            ("p_unix_seconds", 8),
        ):
            self.assertIn(
                f"int(nth({position}, json_decode(payload))) as {alias}",
                sql,
            )
        self.assertIn(
            "bool(nth(9, json_decode(payload))) as p_clock_valid",
            sql,
        )

    def test_rule_rejects_noncanonical_power_event_mutants(self):
        sql = build_event_rule(False)["sql"]
        for contract in (
            "nth(1, json_decode(payload)) = 1",
            "nth(3, json_decode(payload)) >= 1",
            "nth(3, json_decode(payload)) <= 4294967295",
            "nth(9, json_decode(payload)) >= 0",
            "nth(9, json_decode(payload)) <= 1",
            "nth(9, json_decode(payload)) = 1 "
            "or nth(8, json_decode(payload)) = 0",
            "nth(2, json_decode(payload)) = 4 "
            "and nth(4, json_decode(payload)) = 1 "
            "and nth(5, json_decode(payload)) = 1 "
            "and nth(6, json_decode(payload)) = 0 "
            "and nth(7, json_decode(payload)) = 0",
            "nth(2, json_decode(payload)) = 5 "
            "and nth(4, json_decode(payload)) = 2 "
            "and nth(5, json_decode(payload)) = 0 "
            "and nth(6, json_decode(payload)) = 1 "
            "and nth(7, json_decode(payload)) = 0",
            "nth(2, json_decode(payload)) = 6 "
            "and nth(4, json_decode(payload)) = 2 "
            "and nth(5, json_decode(payload)) = 2 "
            "and nth(6, json_decode(payload)) = 210 "
            "and nth(7, json_decode(payload)) = 90",
        ):
            with self.subTest(contract=contract):
                self.assertIn(contract, sql)


class EmqxEventLifecycleTest(unittest.TestCase):
    def test_plan_is_offline_and_never_prints_secret(self):
        class NoNetwork:
            def request(self, *args, **kwargs):
                raise AssertionError("plan must not call network")

        self.assertEqual(parse_args([]).mode, "plan")
        code, output = run_main([], NoNetwork())
        self.assertEqual(code, 0)
        self.assertNotIn(ENV["EMQX_EVENT_GATEWAY_SECRET"], output)

    def test_disabled_first_then_explicit_enable(self):
        emqx = InMemoryEmqx()
        emqx.connector["url"] = ENV["SUPABASE_URL"]

        code, _ = run_main(["apply-disabled"], emqx)
        self.assertEqual(code, 0)
        self.assertEqual(emqx.active_managed(), set())

        code, _ = run_main(["enable", "--enable"], emqx)
        self.assertEqual(code, 0)
        self.assertEqual(
            emqx.active_managed(),
            {
                "actions/http:supabase_power_event",
                "rules/power_event_rule",
            },
        )
        self.assertEqual(emqx.connector_mutations, 0)


if __name__ == "__main__":
    unittest.main()
