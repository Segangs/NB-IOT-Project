import base64
import copy
import contextlib
import io
import unittest
from urllib.parse import urlparse

from Segang.project.emqx_command_setup import (
    ACTION_CONNECTOR_NAME,
    CONNECTOR_ITEM_ID,
    HttpResponse,
    RESOURCE_IDS,
    build_ack_action,
    build_ack_rule,
    build_api_authorization,
    build_request_action,
    build_request_rule,
    _api_headers,
    main,
    parse_args,
)


ENV = {
    "SUPABASE_URL": "https://project.supabase.co",
    "SUPABASE_KEY": "ANON_KEY_VALUE",
    "EMQX_API": "http://emqx.local:18083/api/v5",
    "EMQX_API_KEY": "api-key",
    "EMQX_API_SECRET": "api-secret",
    "EMQX_AUTH_SECRET": "DEVICE_AUTH_HOOK_SECRET",
    "EMQX_COMMAND_GATEWAY_SECRET": "COMMAND_GATEWAY_SECRET",
}


def run_main(argv, emqx, environ=ENV):
    output = io.StringIO()
    with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
        result = main(argv, environ=environ, transport=emqx)
    return result, output.getvalue()


class InMemoryEmqx:
    """EMQX 6.2 Actions/Rules boundary double with persistent state."""

    def __init__(self):
        self.resources = {}
        self.connector = {
            "type": "http",
            "name": "supabase_api",
            "enable": True,
            "url": ENV["SUPABASE_URL"],
            "headers": {
                "Authorization": "******",
                "Content-type": "application/json",
                "apikey": "******",
            },
        }
        self.put_status = 200
        self.mutation_count = 0
        self.failure_index = None
        self.failure_kind = None
        self.failure_after_apply = False
        self.failure_used = False
        self.connector_mutations = 0
        self.get_counts = {}
        self.drift_after_get = None

    def inject_failure(self, index, kind, *, after_apply=False):
        self.mutation_count = 0
        self.failure_index = index
        self.failure_kind = kind
        self.failure_after_apply = after_apply
        self.failure_used = False

    def clear_failure(self):
        self.failure_index = None
        self.failure_kind = None
        self.failure_after_apply = False
        self.failure_used = False
        self.mutation_count = 0

    def active_managed(self):
        return {
            path
            for path, resource in self.resources.items()
            if resource.get("enable") is True
        }

    def request(self, method, url, *, headers=None, json_body=None):
        path = urlparse(url).path.split("/api/v5/", 1)[1]

        if path == "connectors/http:supabase_api":
            if method != "GET":
                self.connector_mutations += 1
                return HttpResponse(405, {"code": "METHOD_NOT_ALLOWED"})
            payload = dict(self.connector)
            payload["status"] = "connected"
            return HttpResponse(200, payload)

        if method == "GET":
            if path not in self.resources:
                return HttpResponse(404, {"code": "NOT_FOUND"})
            self.get_counts[path] = self.get_counts.get(path, 0) + 1
            payload = copy.deepcopy(self.resources[path])
            payload["status"] = "connected"
            payload["node_status"] = [{"node": "emqx@node", "status": "connected"}]
            if self.drift_after_get == (path, self.get_counts[path]):
                if path.startswith("actions/"):
                    self.resources[path]["parameters"]["path"] = "/toctou-drift"
                else:
                    self.resources[path]["sql"] = "SELECT * FROM \"drift\""
            return HttpResponse(200, payload)

        if path.startswith("connectors"):
            self.connector_mutations += 1

        mutation = self._mutation(method, path, json_body)
        self.mutation_count += 1
        should_fail = (
            not self.failure_used
            and self.failure_index == self.mutation_count
        )
        if should_fail:
            self.failure_used = True
            if self.failure_kind == "conflict":
                mutation()
                return HttpResponse(409, {"code": "ALREADY_EXISTS"})
            if self.failure_kind == "timeout":
                if self.failure_after_apply:
                    mutation()
                raise TimeoutError(ENV["EMQX_COMMAND_GATEWAY_SECRET"])
            return HttpResponse(
                500,
                {
                    "authorization": ENV["EMQX_API_SECRET"],
                    "body": ENV["EMQX_COMMAND_GATEWAY_SECRET"],
                },
            )

        status = mutation()
        return HttpResponse(status, None)

    def _mutation(self, method, path, json_body):
        def apply():
            if method == "POST" and path == "actions":
                item = f"actions/{json_body['type']}:{json_body['name']}"
                if item in self.resources:
                    return 409
                self.resources[item] = copy.deepcopy(json_body)
                return 201
            if method == "POST" and path == "rules":
                item = f"rules/{json_body['id']}"
                if item in self.resources:
                    return 409
                self.resources[item] = copy.deepcopy(json_body)
                return 201
            if (
                method == "PUT"
                and path.startswith("actions/http:")
                and "/enable/" in path
            ):
                item, enabled = path.rsplit("/enable/", 1)
                if item not in self.resources or json_body is not None:
                    return 400
                self.resources[item]["enable"] = enabled == "true"
                return self.put_status
            if method == "PUT" and path.startswith("rules/"):
                if path not in self.resources or "id" in json_body:
                    return 400
                rule_id = self.resources[path]["id"]
                self.resources[path] = copy.deepcopy(json_body)
                self.resources[path]["id"] = rule_id
                return self.put_status
            if method == "DELETE":
                if path not in self.resources:
                    return 404
                del self.resources[path]
                return 204
            return 404

        return apply


class EmqxCommandDefinitionTest(unittest.TestCase):
    def test_api_headers_use_explicit_non_default_user_agent(self):
        headers = _api_headers(ENV)

        self.assertEqual(
            headers["User-Agent"],
            "NB-IOT-EMQX-Setup/1.0",
        )
        self.assertNotIn("Python-urllib", headers["User-Agent"])

    def test_resource_ids_are_two_actions_and_two_rules(self):
        self.assertEqual(CONNECTOR_ITEM_ID, "http:supabase_api")
        self.assertEqual(ACTION_CONNECTOR_NAME, "supabase_api")
        self.assertEqual(
            RESOURCE_IDS,
            {
                "request_action": "supabase_command_request",
                "ack_action": "supabase_command_ack",
                "request_rule": "command_request_rule",
                "ack_rule": "command_ack_rule",
            },
        )

    def test_actions_use_emqx_62_http_action_schema_and_no_retry(self):
        request_action = build_request_action(
            ENV["EMQX_COMMAND_GATEWAY_SECRET"],
            False,
        )
        ack_action = build_ack_action(
            ENV["EMQX_COMMAND_GATEWAY_SECRET"],
            False,
        )

        for action in (request_action, ack_action):
            self.assertEqual(
                set(action),
                {"type", "name", "enable", "connector", "parameters"},
            )
            self.assertEqual(action["type"], "http")
            self.assertEqual(action["connector"], "supabase_api")
            self.assertFalse(action["enable"])
            self.assertEqual(
                set(action["parameters"]),
                {"body", "headers", "max_retries", "method", "path"},
            )
            self.assertEqual(action["parameters"]["method"], "post")
            self.assertEqual(
                action["parameters"]["headers"],
                {"content-type": "application/json"},
            )
            self.assertEqual(action["parameters"]["max_retries"], 0)

        self.assertEqual(
            request_action["parameters"]["path"],
            "/rest/v1/rpc/publish_device_command_response",
        )
        self.assertEqual(
            ack_action["parameters"]["path"],
            "/rest/v1/rpc/publish_device_command_ack_receipt",
        )
        self.assertIn(
            '"p_request_secret":"COMMAND_GATEWAY_SECRET"',
            request_action["parameters"]["body"],
        )
        self.assertIn(
            '"p_request_secret":"COMMAND_GATEWAY_SECRET"',
            ack_action["parameters"]["body"],
        )

    def test_rules_reference_http_actions_and_exact_topics(self):
        request_rule = build_request_rule(False)
        ack_rule = build_ack_rule(False)

        self.assertEqual(
            request_rule["actions"], ["http:supabase_command_request"]
        )
        self.assertEqual(ack_rule["actions"], ["http:supabase_command_ack"])
        self.assertIn('FROM "devices/+/cmd/request"', request_rule["sql"])
        self.assertIn('FROM "devices/+/cmd/ack"', ack_rule["sql"])
        self.assertFalse(request_rule["enable"])
        self.assertFalse(ack_rule["enable"])

    def test_rules_use_emqx_supported_int_and_bool_conversions(self):
        request_sql = build_request_rule(False)["sql"]
        ack_sql = build_ack_rule(False)["sql"]

        self.assertNotIn("cast(", request_sql)
        self.assertNotIn("cast(", ack_sql)
        self.assertIn(
            "int(nth(1, json_decode(payload))) as p_request_id",
            request_sql,
        )
        self.assertIn(
            "int(nth(2, json_decode(payload))) as p_last_cmd_id",
            request_sql,
        )
        for alias, position in (
            ("p_cmd_id", 1),
            ("p_phase", 2),
            ("p_result", 3),
            ("p_error", 4),
            ("p_unix_seconds", 5),
        ):
            with self.subTest(alias=alias):
                self.assertIn(
                    f"int(nth({position}, json_decode(payload))) as {alias}",
                    ack_sql,
                )
        self.assertIn(
            "bool(nth(6, json_decode(payload))) as p_clock_valid",
            ack_sql,
        )

    def test_request_sql_rejects_shape_identity_retain_type_and_range_mutants(self):
        sql = build_request_rule(False)["sql"]
        required = (
            "is_array(json_decode(payload))",
            "length(json_decode(payload)) = 2",
            "is_int(nth(1, json_decode(payload)))",
            "is_int(nth(2, json_decode(payload)))",
            "nth(2, split(topic, '/')) = clientid",
            "flags.retain = false",
            "nth(1, json_decode(payload)) >= 1",
            "nth(1, json_decode(payload)) <= 4294967295",
            "nth(2, json_decode(payload)) >= 0",
            "nth(2, json_decode(payload)) <= 4294967295",
        )
        for guard in required:
            with self.subTest(guard=guard):
                self.assertIn(guard, sql)

    def test_ack_sql_rejects_shape_identity_retain_range_and_semantic_mutants(self):
        sql = build_ack_rule(False)["sql"]
        required = (
            "is_array(json_decode(payload))",
            "length(json_decode(payload)) = 6",
            "nth(2, split(topic, '/')) = clientid",
            "flags.retain = false",
            "nth(1, json_decode(payload)) >= 1",
            "nth(1, json_decode(payload)) <= 4294967295",
            "nth(2, json_decode(payload)) >= 1",
            "nth(2, json_decode(payload)) <= 2",
            "nth(3, json_decode(payload)) >= 1",
            "nth(3, json_decode(payload)) <= 4",
            "nth(4, json_decode(payload)) >= 0",
            "nth(4, json_decode(payload)) <= 5",
            "nth(5, json_decode(payload)) >= 0",
            "nth(5, json_decode(payload)) <= 4294967295",
            "nth(6, json_decode(payload)) >= 0",
            "nth(6, json_decode(payload)) <= 1",
            "nth(6, json_decode(payload)) = 1 "
            "or nth(5, json_decode(payload)) = 0",
            "nth(2, json_decode(payload)) = 1 "
            "and nth(3, json_decode(payload)) = 1 "
            "and nth(4, json_decode(payload)) = 0",
            "nth(3, json_decode(payload)) = 2 "
            "and nth(4, json_decode(payload)) = 0",
            "nth(3, json_decode(payload)) = 3 "
            "and nth(4, json_decode(payload)) >= 1 "
            "and nth(4, json_decode(payload)) <= 4",
            "nth(3, json_decode(payload)) = 4 "
            "and nth(4, json_decode(payload)) = 5",
        )
        for position in range(1, 7):
            required += (
                f"is_int(nth({position}, json_decode(payload)))",
            )
        for guard in required:
            with self.subTest(guard=guard):
                self.assertIn(guard, sql)


class EmqxCommandCredentialTest(unittest.TestCase):
    def test_key_and_secret_build_rfc7617_basic_value(self):
        expected = base64.b64encode(b"api-key:api-secret").decode("ascii")
        self.assertEqual(
            build_api_authorization(ENV),
            f"Basic {expected}",
        )

    def test_value_only_and_full_header_compatibility_are_normalized(self):
        token = base64.b64encode(b"legacy-key:legacy-secret").decode("ascii")
        for supplied in (
            f"Basic {token}",
            f"Authorization: Basic {token}",
            f"authorization:Basic {token}",
        ):
            with self.subTest(supplied=supplied):
                legacy = dict(ENV)
                legacy.pop("EMQX_API_KEY")
                legacy.pop("EMQX_API_SECRET")
                legacy["EMQX_API_AUTH_HEADER"] = supplied
                self.assertEqual(
                    build_api_authorization(legacy),
                    f"Basic {token}",
                )

    def test_malformed_or_header_injection_credentials_are_rejected(self):
        invalid = (
            "Basic not*base64",
            "X-Api-Key: Basic YTpi",
            "Authorization: Bearer token",
            "Basic YTpi\r\nX-Evil: yes",
        )
        for supplied in invalid:
            with self.subTest(supplied=supplied):
                legacy = dict(ENV)
                legacy.pop("EMQX_API_KEY")
                legacy.pop("EMQX_API_SECRET")
                legacy["EMQX_API_AUTH_HEADER"] = supplied
                with self.assertRaises(ValueError):
                    build_api_authorization(legacy)

    def test_primary_credentials_reject_crlf_colon_user_and_ambiguous_header(self):
        mutations = []
        bad_key = dict(ENV)
        bad_key["EMQX_API_KEY"] = "bad:key"
        mutations.append(bad_key)
        bad_secret = dict(ENV)
        bad_secret["EMQX_API_SECRET"] = "bad\r\nsecret"
        mutations.append(bad_secret)
        ambiguous = dict(ENV)
        ambiguous["EMQX_API_AUTH_HEADER"] = "Basic YTpi"
        mutations.append(ambiguous)
        for env in mutations:
            with self.subTest(env_keys=sorted(env)):
                with self.assertRaises(ValueError):
                    build_api_authorization(env)


class EmqxCommandLifecycleTest(unittest.TestCase):
    def test_apply_defaults_to_plan_only_and_enable_requires_flag(self):
        self.assertEqual(parse_args([]).mode, "plan")
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            parse_args(["enable"])
        self.assertEqual(parse_args(["enable", "--enable"]).mode, "enable")

    def test_plan_has_no_network_and_no_secret_output(self):
        class NoNetwork:
            def request(self, *args, **kwargs):
                raise AssertionError("plan must not call network")

        code, output = run_main([], NoNetwork())
        self.assertEqual(code, 0)
        self.assertNotIn(ENV["EMQX_API_SECRET"], output)
        self.assertNotIn(ENV["EMQX_AUTH_SECRET"], output)
        self.assertNotIn(ENV["EMQX_COMMAND_GATEWAY_SECRET"], output)
        self.assertNotIn(ENV["SUPABASE_KEY"], output)

    def test_missing_dedicated_command_gateway_secret_fails_before_network(self):
        legacy_only = dict(ENV)
        legacy_only.pop("EMQX_COMMAND_GATEWAY_SECRET")
        emqx = InMemoryEmqx()

        code, output = run_main(["apply-disabled"], emqx, legacy_only)

        self.assertEqual(code, 1)
        self.assertEqual(emqx.resources, {})
        self.assertNotIn(ENV["EMQX_AUTH_SECRET"], output)

    def test_actions_use_dedicated_command_secret_not_device_auth_hook_secret(self):
        emqx = InMemoryEmqx()

        code, output = run_main(["apply-disabled"], emqx)

        self.assertEqual(code, 0)
        for path in (
            "actions/http:supabase_command_request",
            "actions/http:supabase_command_ack",
        ):
            body = emqx.resources[path]["parameters"]["body"]
            self.assertIn(ENV["EMQX_COMMAND_GATEWAY_SECRET"], body)
            self.assertNotIn(ENV["EMQX_AUTH_SECRET"], body)
        self.assertNotIn(ENV["EMQX_COMMAND_GATEWAY_SECRET"], output)
        self.assertNotIn(ENV["EMQX_AUTH_SECRET"], output)

    def test_apply_disabled_creates_exact_four_resources_and_never_mutates_connector(self):
        emqx = InMemoryEmqx()
        code, output = run_main(["apply-disabled"], emqx)

        self.assertEqual(code, 0)
        self.assertEqual(
            set(emqx.resources),
            {
                "actions/http:supabase_command_request",
                "actions/http:supabase_command_ack",
                "rules/command_request_rule",
                "rules/command_ack_rule",
            },
        )
        self.assertFalse(any(r["enable"] for r in emqx.resources.values()))
        self.assertEqual(emqx.connector_mutations, 0)
        self.assertNotIn(ENV["EMQX_API_SECRET"], output)
        self.assertNotIn(ENV["EMQX_AUTH_SECRET"], output)
        self.assertNotIn(ENV["EMQX_COMMAND_GATEWAY_SECRET"], output)

    def test_connector_is_read_only_validated_before_creation(self):
        for mutation in (
            {"url": "https://wrong.supabase.co"},
            {"enable": False},
            {"headers": {"content-type": "application/json"}},
        ):
            with self.subTest(mutation=mutation):
                emqx = InMemoryEmqx()
                emqx.connector.update(mutation)
                code, _ = run_main(["apply-disabled"], emqx)
                self.assertEqual(code, 1)
                self.assertEqual(emqx.resources, {})
                self.assertEqual(emqx.connector_mutations, 0)

    def test_existing_definition_drift_fails_before_any_mutation(self):
        emqx = InMemoryEmqx()
        drifted = build_request_action(
            ENV["EMQX_COMMAND_GATEWAY_SECRET"],
            False,
        )
        drifted["parameters"]["path"] = "/wrong"
        emqx.resources["actions/http:supabase_command_request"] = drifted

        code, output = run_main(["apply-disabled"], emqx)

        self.assertEqual(code, 1)
        self.assertEqual(
            set(emqx.resources),
            {"actions/http:supabase_command_request"},
        )
        self.assertNotIn("/wrong", output)

    def test_standalone_disable_and_delete_never_overwrite_or_remove_drift(self):
        for mode in ("disable", "delete"):
            with self.subTest(mode=mode):
                emqx = InMemoryEmqx()
                self.assertEqual(run_main(["apply-disabled"], emqx)[0], 0)
                path = "actions/http:supabase_command_request"
                emqx.resources[path]["parameters"]["path"] = "/drifted"
                emqx.resources[path]["enable"] = True

                code, _ = run_main([mode], emqx)

                self.assertEqual(code, 1)
                self.assertIn(path, emqx.resources)
                self.assertEqual(
                    emqx.resources[path]["parameters"]["path"],
                    "/drifted",
                )
                self.assertTrue(emqx.resources[path]["enable"])

    def test_put_200_and_204_are_both_success(self):
        for status in (200, 204):
            with self.subTest(status=status):
                emqx = InMemoryEmqx()
                self.assertEqual(run_main(["apply-disabled"], emqx)[0], 0)
                emqx.put_status = status
                self.assertEqual(
                    run_main(["enable", "--enable"], emqx)[0],
                    0,
                )
                self.assertEqual(len(emqx.active_managed()), 4)

    def test_each_enable_http_failure_and_timeout_compensates_to_zero_active(self):
        for kind in ("http", "timeout"):
            for position in range(1, 5):
                with self.subTest(kind=kind, position=position):
                    emqx = InMemoryEmqx()
                    self.assertEqual(run_main(["apply-disabled"], emqx)[0], 0)
                    emqx.inject_failure(
                        position,
                        kind,
                        after_apply=(kind == "timeout"),
                    )
                    code, output = run_main(
                        ["enable", "--enable"],
                        emqx,
                    )
                    self.assertEqual(code, 1)
                    self.assertEqual(emqx.active_managed(), set())
                    self.assertNotIn(ENV["EMQX_API_SECRET"], output)
                    self.assertNotIn(ENV["EMQX_AUTH_SECRET"], output)
                    self.assertNotIn(
                        ENV["EMQX_COMMAND_GATEWAY_SECRET"],
                        output,
                    )

    def test_each_disable_failure_and_timeout_still_attempts_all_and_finishes_safe(self):
        for kind in ("http", "timeout"):
            for position in range(1, 5):
                with self.subTest(kind=kind, position=position):
                    emqx = InMemoryEmqx()
                    self.assertEqual(run_main(["apply-disabled"], emqx)[0], 0)
                    self.assertEqual(
                        run_main(["enable", "--enable"], emqx)[0],
                        0,
                    )
                    emqx.inject_failure(
                        position,
                        kind,
                        after_apply=(kind == "timeout"),
                    )
                    code, output = run_main(["disable"], emqx)
                    self.assertEqual(code, 0)
                    self.assertEqual(emqx.active_managed(), set())
                    self.assertNotIn(ENV["EMQX_AUTH_SECRET"], output)
                    self.assertNotIn(
                        ENV["EMQX_COMMAND_GATEWAY_SECRET"],
                        output,
                    )

    def test_delete_disables_before_rule_then_action_removal(self):
        emqx = InMemoryEmqx()
        self.assertEqual(run_main(["apply-disabled"], emqx)[0], 0)
        self.assertEqual(run_main(["enable", "--enable"], emqx)[0], 0)

        code, _ = run_main(["delete"], emqx)

        self.assertEqual(code, 0)
        self.assertEqual(emqx.active_managed(), set())
        self.assertEqual(emqx.resources, {})
        self.assertEqual(emqx.connector_mutations, 0)

    def test_delete_rechecks_exact_definition_immediately_before_each_delete(self):
        emqx = InMemoryEmqx()
        self.assertEqual(run_main(["apply-disabled"], emqx)[0], 0)
        target = "rules/command_ack_rule"
        emqx.drift_after_get = (target, 5)

        code, _ = run_main(["delete"], emqx)

        self.assertEqual(code, 1)
        self.assertIn(target, emqx.resources)
        self.assertEqual(
            emqx.resources[target]["sql"],
            'SELECT * FROM "drift"',
        )

    def test_apply_failure_removes_only_resources_created_by_that_invocation(self):
        path_by_position = {
            1: "actions/http:supabase_command_request",
            2: "actions/http:supabase_command_ack",
            3: "rules/command_request_rule",
            4: "rules/command_ack_rule",
        }
        for kind in ("http", "timeout", "conflict"):
            for position in range(1, 5):
                with self.subTest(kind=kind, position=position):
                    emqx = InMemoryEmqx()
                    emqx.inject_failure(
                        position,
                        kind,
                        after_apply=(kind == "timeout"),
                    )
                    code, output = run_main(["apply-disabled"], emqx)
                    self.assertEqual(code, 1)
                    if kind == "http":
                        self.assertEqual(emqx.resources, {})
                    else:
                        self.assertEqual(
                            set(emqx.resources),
                            {path_by_position[position]},
                        )
                    self.assertNotIn(
                        ENV["EMQX_COMMAND_GATEWAY_SECRET"],
                        output,
                    )

        emqx = InMemoryEmqx()
        preexisting = build_request_action(
            ENV["EMQX_COMMAND_GATEWAY_SECRET"],
            False,
        )
        emqx.resources["actions/http:supabase_command_request"] = preexisting
        emqx.inject_failure(1, "http")
        self.assertEqual(run_main(["apply-disabled"], emqx)[0], 1)
        self.assertEqual(
            set(emqx.resources),
            {"actions/http:supabase_command_request"},
        )

    def test_apply_cleanup_rechecks_exact_definition_before_delete(self):
        emqx = InMemoryEmqx()
        target = "actions/http:supabase_command_request"
        emqx.inject_failure(2, "http")
        emqx.drift_after_get = (target, 2)

        code, _ = run_main(["apply-disabled"], emqx)

        self.assertEqual(code, 1)
        self.assertIn(target, emqx.resources)
        self.assertEqual(
            emqx.resources[target]["parameters"]["path"],
            "/toctou-drift",
        )

    def test_api_failure_never_echoes_credentials_or_body(self):
        emqx = InMemoryEmqx()
        emqx.inject_failure(1, "http")
        code, output = run_main(["apply-disabled"], emqx)
        self.assertEqual(code, 1)
        for secret in (
            ENV["EMQX_API_KEY"],
            ENV["EMQX_API_SECRET"],
            ENV["EMQX_AUTH_SECRET"],
            ENV["EMQX_COMMAND_GATEWAY_SECRET"],
            ENV["SUPABASE_KEY"],
        ):
            self.assertNotIn(secret, output)


if __name__ == "__main__":
    unittest.main()
