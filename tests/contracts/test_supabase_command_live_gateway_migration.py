import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERSION = "20260726235000"
UP = ROOT / "supabase" / "migrations" / f"{VERSION}_command_live_gateway.sql"
DOWN = ROOT / "supabase" / "rollbacks" / f"{VERSION}_command_live_gateway_down.sql"
PRECHECK = (
    ROOT
    / "supabase"
    / "prechecks"
    / f"{VERSION}_command_live_gateway_precheck.sql"
)
VERIFY = (
    ROOT
    / "supabase"
    / "verifications"
    / f"{VERSION}_command_live_gateway_verify.sql"
)
EXPECTED = ROOT / "supabase" / "command_live_gateway_expected.md"
COMMAND_STATE_UP = (
    ROOT
    / "supabase"
    / "migrations"
    / "20260725054445_device_command_state.sql"
)
COMPACT_WIRE_UP = (
    ROOT
    / "supabase"
    / "migrations"
    / "20260727050000_command_wire_compact_payload.sql"
)
COMPACT_WIRE_DOWN = (
    ROOT
    / "supabase"
    / "rollbacks"
    / "20260727050000_command_wire_compact_payload_down.sql"
)

RESPONSE_SIGNATURE = (
    "public.publish_device_command_response(text,bigint,bigint,text)"
)
ACK_RECEIPT_SIGNATURE = (
    "public.publish_device_command_ack_receipt("
    "text,bigint,smallint,smallint,smallint,bigint,boolean,text)"
)
CLAIM_SIGNATURE = "public.claim_device_command(text,bigint,bigint,integer)"
ACK_SIGNATURE = (
    "public.ack_device_command("
    "text,bigint,smallint,smallint,smallint,bigint,boolean)"
)


def sql(path: Path) -> str:
    if not path.is_file():
        return ""
    return path.read_text(encoding="utf-8")


def compact(source: str) -> str:
    return re.sub(r"\s+", " ", source.lower()).strip()


class SupabaseCommandLiveGatewayMigrationContractTest(unittest.TestCase):
    def _assert_vault_identity_contract(self, source: str):
        normalized = compact(source)
        self.assertIn(
            "vault.create_secret(text,text,text,uuid)",
            normalized,
        )
        self.assertIn("p.pronargs = 4", normalized)
        self.assertIn("p.pronargdefaults = 3", normalized)
        self.assertNotIn(
            "vault.create_secret(text,text,text)')",
            normalized,
        )

    def _assert_exact_trigger_catalog_contract(self, source: str):
        normalized = compact(source)

        for token in (
            "v_legacy_trigger_count",
            "v_sync_trigger_count",
            "t.tgenabled = 'o'",
            "t.tgtype = 7",
            "t.tgfoid = 'public.assign_device_command()'::regprocedure",
            "t.tgtype = 21",
            "t.tgfoid = "
            "'public.sync_device_command_state()'::regprocedure",
            "t.tgqual is null",
            "t.tgnargs = 0",
            "t.tgattr::text = (",
            "('status', 1)",
            "('sent_at', 2)",
            "('cmd', 3)",
            "('deviceid', 4)",
            "pg_catalog.string_agg(",
            "a.attnum::text",
            "order by expected.ordinality",
        ):
            self.assertIn(token, normalized)

        self.assertGreaterEqual(normalized.count("t.tgqual is null"), 2)
        self.assertGreaterEqual(normalized.count("t.tgnargs = 0"), 2)
        self.assertIn(
            "if v_sync_trigger_count <> 1 then",
            normalized,
        )

    def _assert_wrapper_runtime_contract(
        self,
        up_source: str,
        verify_source: str,
    ):
        up = compact(up_source)
        verify = compact(verify_source)
        response = up[
            up.index(
                "create function public.publish_device_command_response"
            ) :
            up.index(
                "create function public.publish_device_command_ack_receipt"
            )
        ]
        ack = up[
            up.index(
                "create function public.publish_device_command_ack_receipt"
            ) :
            up.index(
                "revoke all on function "
                "public.publish_device_command_response"
            )
        ]

        for index, field in enumerate(
            (
                "v_receipt_request_id",
                "v_receipt_cmd_id",
                "v_receipt_opcode",
                "v_receipt_job_id",
                "v_receipt_ttl_seconds",
            )
        ):
            self.assertIn(
                f"{field} := (v_claim_receipt ->> {index})",
                response,
            )
            self.assertIn(
                f"pg_catalog.jsonb_typeof(v_claim_receipt -> {index}) "
                "is distinct from 'number'",
                response,
            )

        for index, field in enumerate(
            (
                "v_receipt_cmd_id",
                "v_receipt_phase",
                "v_receipt_result",
                "v_receipt_status",
                "v_receipt_error",
            )
        ):
            self.assertIn(
                f"{field} := (v_ack_receipt ->> {index})",
                ack,
            )
            self.assertIn(
                f"pg_catalog.jsonb_typeof(v_ack_receipt -> {index}) "
                "is distinct from 'number'",
                ack,
            )

        for block in (response, ack):
            self.assertIn(
                "v_emqx_publish_key || ':' || v_emqx_publish_secret",
                block,
            )
            self.assertIn(
                "if pg_catalog.octet_length(v_payload) > 80 then",
                block,
            )
            self.assertIn("if v_http_request_id is null then", block)

        self.assertIn(
            "v_payload := pg_catalog.replace( "
            "v_claim_receipt::text, ' ', '' )",
            response,
        )
        self.assertIn(
            "v_payload := pg_catalog.replace( "
            "v_ack_receipt::text, ' ', '' )",
            ack,
        )
        self.assertNotIn(
            "v_payload := pg_catalog.jsonb_build_array(",
            response,
        )
        self.assertNotIn(
            "v_payload := pg_catalog.jsonb_build_array(",
            ack,
        )

        self.assertNotRegex(
            up,
            r"grant\s+execute\s+on\s+function\s+"
            r"public\.publish_device_command_(?:response|ack_receipt)"
            r"\([^;]+?\)\s+to\s+[^;]*\bauthenticator\b",
        )
        self.assertIn("t.tgtype = 21", up)
        self.assertNotIn(
            "drop trigger trg_sync_device_command_state",
            up,
        )

        for token in (
            "select public.publish_device_command_response(",
            "select public.publish_device_command_ack_receipt(",
            "from net.http_request_queue as q",
            "q.id = v_response_request_id",
            "q.id = v_ack_request_id",
            "q.method = 'post'",
            "q.url = v_emqx_publish_url",
            "q.headers ->> 'content-type' = 'application/json'",
            "q.headers ->> 'authorization' = v_expected_authorization",
            "pg_catalog.convert_from(q.body, 'utf8')::jsonb",
            "'devices/' || v_probe_imei || '/cmd/response'",
            "'devices/' || v_probe_imei || '/cmd/ack/receipt'",
            "'qos', 1",
            "'retain', false",
            "pg_catalog.octet_length(v_expected_response_payload) > 80",
            "pg_catalog.octet_length(v_expected_ack_payload) > 80",
            "v_expected_response_payload := pg_catalog.replace(",
            "v_expected_ack_payload := pg_catalog.replace(",
            "q.timeout_milliseconds = 5000",
            "v_response_request_id is null",
            "v_ack_request_id is null",
            "v_response_request_id = v_ack_request_id",
            "acl.grantee not in (",
            "'anon'::regrole",
            "'service_role'::regrole",
        ):
            self.assertIn(token, verify)

    def _assert_rollback_provenance_contract(
        self,
        up_source: str,
        verify_source: str,
        down_source: str,
    ):
        up = compact(up_source)
        verify = compact(verify_source)
        down = compact(down_source)

        for source in (verify, down):
            for token in (
                "s.id = d.id",
                "secret_id=' || s.id::text",
                "s.decrypted_secret is not distinct from "
                "v_config_request_secret",
                "pg_catalog.pg_get_functiondef(p.oid)",
                "pg_catalog.obj_description(p.oid, 'pg_proc')",
                "definition_md5=",
                "owner_oid=",
                "';owner_oid=' || p.proowner::text",
                "pg_catalog.aclexplode",
                "acl.grantee not in (",
            ):
                self.assertIn(token, source)

            self.assertIn(
                "d.description = "
                "'nb-iot command gateway migration "
                "20260726235000;secret_id=' || s.id::text",
                source,
            )

        self.assertIn(
            "vault.create_secret(text,text,text,uuid)",
            up,
        )
        self.assertIn(
            "vault.update_secret(uuid,text,text,text,uuid)",
            up,
        )
        self.assertIn("secret_id=' || v_gateway_secret_id::text", up)
        self.assertIn("pg_catalog.pg_get_functiondef", up)
        self.assertIn("definition_md5=", up)
        self.assertIn("owner_oid=", up)

    def _assert_cutover_safety_contract(
        self,
        up_source: str,
        down_source: str,
    ):
        up = up_source.lower()
        down = down_source.lower()
        up_compact = compact(up_source)
        down_compact = compact(down_source)

        self.assertIn(
            "drop trigger trg_assign_device_command on public.sensorvalue;",
            up_compact,
        )
        self.assertNotIn("drop function public.assign_device_command", up)
        self.assertIn(
            "create trigger trg_assign_device_command "
            "before insert on public.sensorvalue "
            "for each row "
            "execute function public.assign_device_command();",
            down_compact,
        )
        self.assertLess(
            down.index("create trigger trg_assign_device_command"),
            down.index("drop function public.publish_device_command_response"),
        )
        self.assertLess(
            down.index("create trigger trg_assign_device_command"),
            down.index(
                "drop function public.publish_device_command_ack_receipt"
            ),
        )
        self._assert_vault_rollback_without_update_contract(down_source)
        self.assertNotIn("nb_iot_config_request_secret';", down_compact)
        self.assertNotIn("drop function public.claim_device_command", down)
        self.assertNotIn("drop function public.ack_device_command", down)

    def _assert_vault_rollback_without_update_contract(
        self,
        down_source: str,
    ):
        down = compact(down_source)
        remove = down[
            down.index("do $remove_command_gateway_secret$") :
            down.index("$remove_command_gateway_secret$;")
        ]

        self.assertNotIn("for update", down)
        for token in (
            "select s.id into strict v_gateway_secret_id "
            "from vault.secrets as s "
            "where s.name = 'nb_iot_command_gateway_secret';",
            "select s.decrypted_secret "
            "into strict v_config_request_secret "
            "from vault.decrypted_secrets as s "
            "where s.name = 'nb_iot_config_request_secret' "
            "and nullif(s.decrypted_secret, '') is not null;",
            "delete from vault.secrets as target "
            "where target.id = v_gateway_secret_id",
            "target.name = 'nb_iot_command_gateway_secret'",
            "target.description = "
            "'nb-iot command gateway migration "
            "20260726235000;secret_id=' "
            "|| v_gateway_secret_id::text",
            "from vault.decrypted_secrets as alias_secret",
            "alias_secret.id = v_gateway_secret_id",
            "alias_secret.name = 'nb_iot_command_gateway_secret'",
            "alias_secret.decrypted_secret is not distinct from "
            "v_config_request_secret",
            "get diagnostics v_deleted_count = row_count;",
            "if v_deleted_count <> 1 then",
        ):
            self.assertIn(token, remove)

        self.assertNotIn(
            "delete from vault.secrets "
            "where name = 'nb_iot_command_gateway_secret'",
            remove,
        )

    def test_all_review_artifacts_exist(self):
        for path in (
            UP,
            DOWN,
            PRECHECK,
            VERIFY,
            EXPECTED,
            COMPACT_WIRE_UP,
            COMPACT_WIRE_DOWN,
        ):
            with self.subTest(path=path):
                self.assertTrue(path.is_file(), f"missing migration artifact: {path}")

    def test_compact_wire_correction_is_guarded_reversible_and_provenance_safe(self):
        up = compact(sql(COMPACT_WIRE_UP))
        down = compact(sql(COMPACT_WIRE_DOWN))

        for source in (up, down):
            with self.subTest(direction="up" if source == up else "down"):
                self.assertIn("begin;", source)
                self.assertIn("set local lock_timeout = '5s'", source)
                self.assertIn("set local statement_timeout = '30s'", source)
                self.assertIn("pg_catalog.pg_get_functiondef", source)
                self.assertIn("execute v_rewritten", source)
                self.assertIn("definition_md5=", source)
                self.assertIn(";owner_oid=", source)
                self.assertIn("commit;", source)

        self.assertIn("v_claim_receipt::text", up)
        self.assertIn("v_ack_receipt::text", up)
        self.assertIn("pg_catalog.replace(", up)
        self.assertIn("compact payload source drift", up)
        self.assertIn("compact payload verification failed", up)
        self.assertIn("pg_catalog.jsonb_build_array(", down)
        self.assertIn("rollback source drift", down)

    def test_up_contains_the_required_gateway_boundary(self):
        up = sql(UP).lower()
        self.assertIn("security definer", up)
        self.assertIn("set search_path = ''", up)
        self.assertIn("public.claim_device_command(", up)
        self.assertIn("public.ack_device_command(", up)
        self.assertIn("net.http_post(", up)
        self.assertIn("/cmd/response", up)
        self.assertIn("/cmd/ack/receipt", up)
        self.assertIn("'qos', 1", up)
        self.assertIn("'retain', false", up)
        self.assertIn("octet_length", up)
        self.assertIn("drop trigger trg_assign_device_command", up)
        self.assertNotIn("drop function public.assign_device_command", up)

    def test_wrapper_signatures_and_return_types_are_exact(self):
        up = compact(sql(UP))
        self.assertIn(
            "create function public.publish_device_command_response( "
            "p_imei text, p_request_id bigint, p_last_cmd_id bigint, "
            "p_request_secret text ) returns bigint",
            up,
        )
        self.assertIn(
            "create function public.publish_device_command_ack_receipt( "
            "p_imei text, p_cmd_id bigint, p_phase smallint, "
            "p_result smallint, p_error smallint, p_unix_seconds bigint, "
            "p_clock_valid boolean, p_request_secret text ) returns bigint",
            up,
        )

    def test_request_wrapper_validates_delegates_and_reuses_topic_identity(self):
        source = sql(UP).lower()
        self.assertIn(
            "create function public.publish_device_command_response",
            source,
        )
        self.assertIn(
            "create function public.publish_device_command_ack_receipt",
            source,
        )
        request = source[
            source.index(
                "create function public.publish_device_command_response"
            ) :
            source.index(
                "create function public.publish_device_command_ack_receipt"
            )
        ]
        request_compact = compact(request)

        for token in (
            "p_imei is null",
            "p_imei !~ '^[0-9]{10,20}$'",
            "p_request_id is null",
            "p_request_id not between 1 and 4294967295",
            "p_last_cmd_id is null",
            "p_last_cmd_id not between 0 and 4294967295",
            "p_request_secret is null",
            "nb_iot_command_gateway_secret",
            "p_request_secret is distinct from v_expected_request_secret",
            "pg_catalog.replace",
            "octet_length(v_payload) > 80",
        ):
            with self.subTest(token=token):
                self.assertIn(token, request)

        self.assertIn(
            "public.claim_device_command( p_imei, p_request_id, "
            "p_last_cmd_id, 60 )",
            request_compact,
        )
        self.assertIn(
            "'topic', 'devices/' || p_imei || '/cmd/response'",
            request_compact,
        )
        self.assertNotRegex(
            request,
            r"'topic'\s*,\s*'devices/'\s*\|\|\s*"
            r"(?!p_imei\b)[a-z_][a-z0-9_]*",
        )

    def test_ack_wrapper_validates_delegates_and_reuses_topic_identity(self):
        source = sql(UP).lower()
        self.assertIn(
            "create function public.publish_device_command_ack_receipt",
            source,
        )
        self.assertIn(
            "revoke all on function public.publish_device_command_response",
            source,
        )
        ack = source[
            source.index(
                "create function public.publish_device_command_ack_receipt"
            ) :
            source.index(
                "revoke all on function "
                "public.publish_device_command_response"
            )
        ]
        ack_compact = compact(ack)

        for token in (
            "p_imei is null",
            "p_imei !~ '^[0-9]{10,20}$'",
            "p_cmd_id is null",
            "p_cmd_id not between 1 and 4294967295",
            "p_phase is null",
            "p_phase not between 1 and 2",
            "p_result is null",
            "p_result not between 1 and 4",
            "p_error is null",
            "p_error not between 0 and 5",
            "p_unix_seconds is null",
            "p_unix_seconds not between 0 and 4294967295",
            "p_clock_valid is null",
            "p_request_secret is null",
            "nb_iot_command_gateway_secret",
            "p_request_secret is distinct from v_expected_request_secret",
            "pg_catalog.replace",
            "octet_length(v_payload) > 80",
        ):
            with self.subTest(token=token):
                self.assertIn(token, ack)

        self.assertIn(
            "public.ack_device_command( p_imei, p_cmd_id, p_phase, "
            "p_result, p_error, p_unix_seconds, p_clock_valid )",
            ack_compact,
        )
        self.assertIn(
            "'topic', 'devices/' || p_imei || '/cmd/ack/receipt'",
            ack_compact,
        )
        self.assertNotRegex(
            ack,
            r"'topic'\s*,\s*'devices/'\s*\|\|\s*"
            r"(?!p_imei\b)[a-z_][a-z0-9_]*",
        )

    def test_deserialized_internal_receipt_fields_cannot_be_null(self):
        source = sql(UP).lower()
        response = source[
            source.index(
                "create function public.publish_device_command_response"
            ) :
            source.index(
                "create function public.publish_device_command_ack_receipt"
            )
        ]
        ack = source[
            source.index(
                "create function public.publish_device_command_ack_receipt"
            ) :
            source.index(
                "revoke all on function "
                "public.publish_device_command_response"
            )
        ]

        for token in (
            "v_receipt_request_id is null",
            "v_receipt_cmd_id is null",
            "v_receipt_opcode is null",
            "v_receipt_job_id is null",
            "v_receipt_ttl_seconds is null",
        ):
            with self.subTest(wrapper="response", token=token):
                self.assertIn(token, response)

        for token in (
            "v_receipt_cmd_id is null",
            "v_receipt_phase is null",
            "v_receipt_result is null",
            "v_receipt_status is null",
            "v_receipt_error is null",
        ):
            with self.subTest(wrapper="ack", token=token):
                self.assertIn(token, ack)

    def test_both_wrappers_use_vault_basic_auth_and_bounded_pg_net(self):
        source = sql(UP).lower()
        self.assertIn(
            "create function public.publish_device_command_response",
            source,
        )
        self.assertIn(
            "create function public.publish_device_command_ack_receipt",
            source,
        )
        self.assertIn(
            "revoke all on function public.publish_device_command_response",
            source,
        )
        response = source[
            source.index(
                "create function public.publish_device_command_response"
            ) :
            source.index(
                "create function public.publish_device_command_ack_receipt"
            )
        ]
        ack = source[
            source.index(
                "create function public.publish_device_command_ack_receipt"
            ) :
            source.index(
                "revoke all on function "
                "public.publish_device_command_response"
            )
        ]

        for block in (response, ack):
            with self.subTest(wrapper=block[:80]):
                self.assertIn("security definer", block)
                self.assertIn("set search_path = ''", block)
                self.assertIn("from vault.decrypted_secrets", block)
                self.assertIn("nb_iot_emqx_publish_url", block)
                self.assertIn("nb_iot_emqx_publish_key", block)
                self.assertIn("nb_iot_emqx_publish_secret", block)
                self.assertIn("'authorization', 'basic '", block)
                self.assertIn("pg_catalog.convert_to(", block)
                self.assertIn("pg_catalog.encode(", block)
                self.assertIn("pg_catalog.replace(", block)
                self.assertIn("net.http_post(", block)
                self.assertIn("timeout_milliseconds := 5000", block)
                self.assertIn("'qos', 1", block)
                self.assertIn("'retain', false", block)

    def test_alias_is_copied_inside_the_database_before_wrapper_creation(self):
        source = sql(UP).lower()
        self.assertIn("from vault.decrypted_secrets", source)
        self.assertIn("where name = 'nb_iot_config_request_secret'", source)
        self.assertIn("vault.create_secret(", source)
        self.assertIn("'nb_iot_command_gateway_secret'", source)
        self.assertLess(
            source.index("vault.create_secret("),
            source.index(
                "create function public.publish_device_command_response"
            ),
        )
        self.assertNotRegex(
            source,
            r"(update|delete\s+from)\s+vault\.(secrets|decrypted_secrets)"
            r".*nb_iot_config_request_secret",
        )

    def test_wrapper_acl_is_anon_and_service_role_only(self):
        source = compact(sql(UP))
        for signature in (RESPONSE_SIGNATURE, ACK_RECEIPT_SIGNATURE):
            with self.subTest(signature=signature):
                self.assertIn(
                    f"revoke all on function {signature} "
                    "from public, anon, authenticated, service_role;",
                    source,
                )
                self.assertIn(
                    f"grant execute on function {signature} "
                    "to anon, service_role;",
                    source,
                )
                self.assertNotIn(
                    f"grant execute on function {signature} to authenticated",
                    source,
                )

        for signature in (CLAIM_SIGNATURE, ACK_SIGNATURE):
            with self.subTest(signature=signature):
                self.assertIn(
                    f"revoke all on function {signature} "
                    "from public, anon, authenticated;",
                    source,
                )
                self.assertIn(
                    f"grant execute on function {signature} to service_role;",
                    source,
                )
                self.assertNotIn(
                    f"grant execute on function {signature} to anon",
                    source,
                )

    def test_trigger_cutover_is_locked_guarded_and_last(self):
        source = sql(UP).lower()
        source_compact = compact(source)
        sensor_lock = (
            "lock table public.sensorvalue in share row exclusive mode;"
        )
        command_lock = (
            'lock table public."devicecmds" in share row exclusive mode;'
        )
        drop = "drop trigger trg_assign_device_command on public.sensorvalue;"

        self.assertTrue(source.lstrip().startswith("begin;"))
        self.assertLess(source.index(sensor_lock), source.index(command_lock))
        self.assertLess(
            source.index(command_lock),
            source.index("vault.create_secret("),
        )
        self.assertIn('from public."devicecmds" where status = 0', source_compact)
        self.assertIn("trg_sync_device_command_state", source)
        self.assertIn(
            "t.tgfoid = "
            "'public.sync_device_command_state()'::regprocedure",
            source_compact,
        )
        self.assertEqual(source.count(drop), 1)
        self.assertRegex(
            source.rstrip(),
            r"drop trigger trg_assign_device_command "
            r"on public\.sensorvalue;\s*commit;$",
        )

    def test_precheck_is_value_silent_and_guards_the_exact_baseline(self):
        source = sql(PRECHECK).lower()
        source_compact = compact(source)
        self.assertTrue(source.lstrip().startswith("begin read only;"))
        for token in (
            'public."devicecmds"',
            "public.device_command_state",
            CLAIM_SIGNATURE,
            ACK_SIGNATURE,
            "trg_sync_device_command_state",
            "trg_assign_device_command",
            "t.tgenabled = 'o'",
            "t.tgtype = 7",
            "t.tgfoid = 'public.assign_device_command()'::regprocedure",
            "de97b37ad246c5e509ae06f821a10338",
            "nb_iot_config_request_secret",
            "nb_iot_emqx_publish_url",
            "nb_iot_emqx_publish_key",
            "nb_iot_emqx_publish_secret",
            "nb_iot_command_gateway_secret",
            "publish_device_command_response",
            "publish_device_command_ack_receipt",
        ):
            with self.subTest(token=token):
                self.assertIn(token, source_compact)
        self.assertIn('from public."devicecmds" where status = 0', source_compact)
        self.assertIn("vault.decrypted_secrets", source)
        self.assertNotRegex(
            source,
            r"select\s+.*decrypted_secret\s*(?:;|\\gset|\\gexec)",
        )

    def test_verify_checks_alias_functions_acl_and_no_command_claim(self):
        source = compact(sql(VERIFY))
        for token in (
            "nb_iot_config_request_secret",
            "nb_iot_command_gateway_secret",
            RESPONSE_SIGNATURE,
            ACK_RECEIPT_SIGNATURE,
            CLAIM_SIGNATURE,
            ACK_SIGNATURE,
            "prosecdef",
            "proconfig",
            "has_function_privilege('anon'",
            "has_function_privilege(",
            "'authenticated'",
            "'service_role'",
            "pg_catalog.aclexplode",
            "acl.grantee not in (",
            "trg_assign_device_command",
            "assign_device_command()",
            "de97b37ad246c5e509ae06f821a10338",
            "trg_sync_device_command_state",
            "public.claim_device_command(",
            "when sqlstate '55000' then",
            "jsonb_build_array(",
            "v_probe_request_id, 0, 0, 0, 0",
        ):
            with self.subTest(token=token):
                self.assertIn(token, source)
        self.assertIn("decrypted_secret is not distinct from", source)

    def test_verify_ack_wrapper_call_casts_literals_to_the_exact_signature(self):
        source = compact(sql(VERIFY))
        self.assertIn(
            "select public.publish_device_command_ack_receipt( "
            "v_probe_imei, 4294967295::bigint, 1::smallint, "
            "1::smallint, 0::smallint, 0::bigint, false, "
            "v_gateway_request_secret ) into v_ack_request_id;",
            source,
        )

    def test_down_restores_exact_trigger_before_removing_gateway(self):
        up_source = sql(UP)
        down_source = sql(DOWN)
        self._assert_cutover_safety_contract(up_source, down_source)

        down = down_source.lower()
        self.assertTrue(down.lstrip().startswith("begin;"))
        self.assertIn(
            "drop function "
            "public.publish_device_command_response(text,bigint,bigint,text);",
            compact(down_source),
        )
        self.assertIn(
            "drop function public.publish_device_command_ack_receipt("
            "text,bigint,smallint,smallint,smallint,bigint,boolean,text);",
            compact(down_source),
        )
        self.assertNotIn("update vault.secrets", down)
        self.assertNotIn("delete from vault.decrypted_secrets", down)

    def test_down_needs_only_select_and_delete_on_vault_secrets(self):
        self._assert_vault_rollback_without_update_contract(sql(DOWN))

    def test_vault_rollback_contract_rejects_lock_and_broad_delete_mutants(self):
        source = sql(DOWN)

        locking_mutant = source.replace(
            "where s.name = 'nb_iot_command_gateway_secret';",
            "where s.name = 'nb_iot_command_gateway_secret'\n"
            "    for update;",
            1,
        )
        broad_delete_mutant = source.replace(
            "where target.id = v_gateway_secret_id",
            "where target.name = 'nb_iot_command_gateway_secret'",
            1,
        )
        provenance_mutant = source.replace(
            "and target.description =\n"
            "          'NB-IOT command gateway migration "
            "20260726235000;secret_id='\n"
            "              || v_gateway_secret_id::text",
            "and target.description is not null",
            1,
        )

        for name, mutant in (
            ("Vault row lock added", locking_mutant),
            ("alias ID predicate removed", broad_delete_mutant),
            ("alias provenance predicate bypassed", provenance_mutant),
        ):
            with self.subTest(name=name):
                with self.assertRaises(AssertionError):
                    self._assert_vault_rollback_without_update_contract(
                        mutant,
                    )

    def test_cutover_contract_rejects_realistic_rollback_mutants(self):
        up_source = sql(UP)
        down_source = sql(DOWN)
        self._assert_cutover_safety_contract(up_source, down_source)

        trigger_timing_mutant = down_source.replace(
            "before insert on public.sensorvalue",
            "after insert on public.sensorvalue",
            1,
        )
        missing_alias_cleanup_mutant = down_source.replace(
            "delete from vault.secrets as target",
            "-- gateway alias cleanup removed",
            1,
        )
        premature_legacy_drop_mutant = up_source.replace(
            "commit;",
            "drop function public.assign_device_command();\n\ncommit;",
            1,
        )

        for name, mutated_up, mutated_down in (
            ("rollback trigger timing changed", up_source, trigger_timing_mutant),
            ("gateway alias cleanup removed", up_source, missing_alias_cleanup_mutant),
            ("legacy rollback function removed", premature_legacy_drop_mutant, down_source),
        ):
            with self.subTest(name=name):
                with self.assertRaises(AssertionError):
                    self._assert_cutover_safety_contract(
                        mutated_up,
                        mutated_down,
                    )

    def test_expected_result_declares_live_apply_and_rollback_boundaries(self):
        source = sql(EXPECTED)
        for token in (
            "live apply: 0",
            "`nb_iot_command_gateway_secret`",
            "`nb_iot_config_request_secret`",
            "`trg_assign_device_command`",
            "`assign_device_command()`",
            "80",
            "`anon`",
            "`service_role`",
            "`authenticated`",
            "PostgreSQL 17",
        ):
            with self.subTest(token=token):
                self.assertIn(token, source)

    def test_vault_guard_uses_current_full_identity_and_default_metadata(self):
        for path in (PRECHECK, UP):
            with self.subTest(path=path):
                self._assert_vault_identity_contract(sql(path))

    def test_vault_identity_contract_rejects_identity_and_default_mutants(self):
        source = compact(sql(UP))
        self._assert_vault_identity_contract(source)

        mutants = (
            source.replace(
                "vault.create_secret(text,text,text,uuid)",
                "vault.create_secret(text,text,text)",
            ),
            source.replace("p.pronargdefaults = 3", "p.pronargdefaults = 2"),
        )
        for mutant in mutants:
            with self.assertRaises(AssertionError):
                self._assert_vault_identity_contract(mutant)

    def test_trigger_guards_fix_exact_shape_in_every_lifecycle_artifact(self):
        for path in (PRECHECK, UP, VERIFY, DOWN):
            with self.subTest(path=path):
                self._assert_exact_trigger_catalog_contract(sql(path))

        self.assertIn(
            "if v_legacy_trigger_count <> 1 then",
            compact(sql(PRECHECK)),
        )
        self.assertIn(
            "if v_legacy_trigger_count <> 1 then",
            compact(sql(UP)),
        )
        self.assertIn(
            "if v_legacy_trigger_count <> 0 then",
            compact(sql(VERIFY)),
        )
        down = compact(sql(DOWN))
        self.assertIn("if v_legacy_trigger_count <> 0 then", down)
        self.assertIn("if v_legacy_trigger_count <> 1 then", down)

    def test_exact_trigger_contract_rejects_shape_mutants(self):
        source = sql(UP)
        self._assert_exact_trigger_catalog_contract(source)

        mutants = {
            "legacy WHEN clause": source.replace(
                "and t.tgqual is null",
                "and t.tgqual is not null",
                1,
            ),
            "legacy trigger argument": source.replace(
                "and t.tgnargs = 0",
                "and t.tgnargs = 1",
                1,
            ),
            "sync trigger removed from guard": source.replace(
                "and t.tgtype = 21",
                "",
                1,
            ),
            "sync event shrunk to INSERT": source.replace(
                "and t.tgtype = 21",
                "and t.tgtype = 5",
                1,
            ),
            "sync WHEN clause": source.replace(
                "and t.tgqual is null",
                "and t.tgqual is not null",
            ),
            "sync trigger argument": source.replace(
                "and t.tgnargs = 0",
                "and t.tgnargs = 1",
            ),
            "sync UPDATE OF target drift": source.replace(
                "('sent_at', 2)",
                "('updated_at', 2)",
                1,
            ),
        }

        for name, mutant in mutants.items():
            with self.subTest(name=name):
                with self.assertRaises(AssertionError):
                    self._assert_exact_trigger_catalog_contract(mutant)

    def test_verify_executes_both_wrappers_and_checks_exact_queue_rows(self):
        self._assert_wrapper_runtime_contract(sql(UP), sql(VERIFY))

    def test_runtime_contract_rejects_seven_reviewed_mutants(self):
        up_source = sql(UP)
        verify_source = sql(VERIFY)
        self._assert_wrapper_runtime_contract(up_source, verify_source)

        swapped_positions = up_source.replace(
            "v_receipt_request_id := (v_claim_receipt ->> 0)",
            "v_receipt_request_id := (v_claim_receipt ->> 1)",
            1,
        ).replace(
            "v_receipt_cmd_id := (v_claim_receipt ->> 1)",
            "v_receipt_cmd_id := (v_claim_receipt ->> 0)",
            1,
        )
        reversed_basic = up_source.replace(
            "v_emqx_publish_key || ':' || v_emqx_publish_secret",
            "v_emqx_publish_secret || ':' || v_emqx_publish_key",
        )
        payload_limit_800 = up_source.replace(
            "octet_length(v_payload) > 80",
            "octet_length(v_payload) > 800",
        )
        sync_trigger_removed = up_source.replace(
            "drop trigger trg_assign_device_command",
            "drop trigger trg_sync_device_command_state "
            'on public."deviceCmds";\n\n'
            "drop trigger trg_assign_device_command",
            1,
        )
        unauthorized_acl = up_source.replace(
            "drop trigger trg_assign_device_command",
            "grant execute on function "
            "public.publish_device_command_response"
            "(text,bigint,bigint,text) to authenticator;\n\n"
            "drop trigger trg_assign_device_command",
            1,
        )
        null_guards_removed = up_source.replace(
            "if v_http_request_id is null then",
            "if false then",
        )
        array_type_guards_removed = up_source.replace(
            "pg_catalog.jsonb_typeof(v_claim_receipt ->",
            "pg_catalog.jsonb_typeof('0'::jsonb ->",
        ).replace(
            "pg_catalog.jsonb_typeof(v_ack_receipt ->",
            "pg_catalog.jsonb_typeof('0'::jsonb ->",
        )

        for name, mutant in (
            ("response positions swapped", swapped_positions),
            ("Basic operands reversed", reversed_basic),
            ("payload limit expanded", payload_limit_800),
            ("sync trigger removed before cutover", sync_trigger_removed),
            ("authenticator ACL added", unauthorized_acl),
            ("pg_net null guards removed", null_guards_removed),
            ("JSON element type guards removed", array_type_guards_removed),
        ):
            with self.subTest(name=name):
                with self.assertRaises(AssertionError):
                    self._assert_wrapper_runtime_contract(
                        mutant,
                        verify_source,
                    )

    def test_ack_receipt_types_and_status_error_semantics_are_exact(self):
        source = compact(sql(UP))
        for token in (
            "v_receipt_status = 1 and v_receipt_error <> 0",
            "v_receipt_status = 2 "
            "and v_receipt_error not in (1, 2)",
            "v_receipt_status = 3 "
            "and v_receipt_error not in (3, 4)",
        ):
            with self.subTest(token=token):
                self.assertIn(token, source)

    def test_overloads_owner_and_direct_acl_are_fail_closed(self):
        for path in (PRECHECK, UP):
            source = compact(sql(path))
            with self.subTest(path=path):
                self.assertIn("from pg_catalog.pg_proc as p", source)
                self.assertIn(
                    "p.proname in ( "
                    "'publish_device_command_response', "
                    "'publish_device_command_ack_receipt' )",
                    source,
                )

        for path in (VERIFY, DOWN):
            source = compact(sql(path))
            with self.subTest(path=path):
                self.assertIn("p.proowner", source)
                self.assertIn("pg_catalog.aclexplode", source)
                self.assertIn("acl.grantee not in (", source)
                self.assertIn("'anon'::regrole", source)
                self.assertIn("'service_role'::regrole", source)

    def test_alias_and_wrapper_provenance_is_verified_before_rollback(self):
        self._assert_rollback_provenance_contract(
            sql(UP),
            sql(VERIFY),
            sql(DOWN),
        )

    def test_rollback_provenance_rejects_replacement_and_acl_mutants(self):
        up_source = sql(UP)
        verify_source = sql(VERIFY)
        down_source = sql(DOWN)
        self._assert_rollback_provenance_contract(
            up_source,
            verify_source,
            down_source,
        )

        compact_down = compact(down_source)
        mutants = {
            "alias UUID marker removed": compact_down.replace(
                "secret_id=' || s.id::text",
                "secret_id='",
                1,
            ),
            "wrapper definition marker removed": compact_down.replace(
                "pg_catalog.pg_get_functiondef(p.oid)",
                "''",
                1,
            ),
            "wrapper owner marker removed": compact_down.replace(
                "';owner_oid=' || p.proowner::text",
                "''",
                1,
            ),
            "allowlist ACL guard removed": compact_down.replace(
                "acl.grantee not in (",
                "acl.grantee in (",
                1,
            ),
        }

        for name, mutant in mutants.items():
            with self.subTest(name=name):
                with self.assertRaises(AssertionError):
                    self._assert_rollback_provenance_contract(
                        up_source,
                        verify_source,
                        mutant,
                    )

    def test_queue_id_and_bounded_existing_recovery_contract_are_documented(self):
        expected = sql(EXPECTED).lower()
        command_state = compact(sql(COMMAND_STATE_UP))

        for token in (
            "queued request id",
            "not http or mqtt delivery success",
            "_http_response",
            "exact 2xx",
            "60-second lease",
            "delivery_attempts < 5",
            "at most five",
            "idempotent",
            "no autonomous retry",
            "no persistent worker",
        ):
            with self.subTest(token=token):
                self.assertIn(token, expected)

        for token in (
            "s.lease_expires_at <= clock_timestamp()",
            "s.delivery_attempts < 5",
            "delivery_attempts = delivery_attempts + 1",
            "idempotently stores accepted/final ack",
        ):
            with self.subTest(token=token):
                self.assertIn(token, command_state)

        up = sql(UP).lower()
        self.assertGreaterEqual(up.count("queued request id only"), 2)
        self.assertGreaterEqual(
            up.count("not http or mqtt delivery success"),
            2,
        )

    def test_gateway_adds_no_worker_helper_or_tracking_schema(self):
        up = compact(sql(UP))
        self.assertEqual(
            up.count("create function public.publish_device_command_"),
            2,
        )
        for token in (
            "create table",
            "create materialized view",
            "create extension",
            "net._http_response",
            "request_tracking",
            "retry_worker",
            "pg_cron",
        ):
            with self.subTest(token=token):
                self.assertNotIn(token, up)


if __name__ == "__main__":
    unittest.main()
