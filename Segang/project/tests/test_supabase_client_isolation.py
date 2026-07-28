import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


class SupabaseClientIsolationTest(unittest.TestCase):
    def test_oauth_factory_returns_distinct_clients(self):
        from supabase_clients import create_oauth_client

        created = []

        def factory(url, key):
            client = {"url": url, "key": key, "index": len(created)}
            created.append(client)
            return client

        first = create_oauth_client(
            "https://example.supabase.co",
            "key",
            factory=factory,
        )
        second = create_oauth_client(
            "https://example.supabase.co",
            "key",
            factory=factory,
        )

        self.assertIsNot(first, second)
        self.assertEqual([0, 1], [first["index"], second["index"]])

    def test_machine_and_oauth_factories_return_independent_clients(self):
        from supabase_clients import (
            create_machine_client,
            create_oauth_client,
        )

        def factory(url, key):
            return {"url": url, "key": key, "session": None}

        machine = create_machine_client(
            "https://example.supabase.co",
            "key",
            factory=factory,
        )
        oauth = create_oauth_client(
            "https://example.supabase.co",
            "key",
            factory=factory,
        )
        oauth["session"] = "user-session"

        self.assertIsNot(machine, oauth)
        self.assertIsNone(machine["session"])

    def test_parallel_oauth_requests_never_share_client_state(self):
        from supabase_clients import create_oauth_client

        def exchange_for_user(user_id):
            client = create_oauth_client(
                "https://example.supabase.co",
                "key",
                factory=lambda url, key: {
                    "url": url,
                    "key": key,
                    "session": None,
                },
            )
            client["session"] = f"session-{user_id}"
            return id(client), client["session"]

        with ThreadPoolExecutor(max_workers=8) as executor:
            results = list(executor.map(exchange_for_user, range(32)))

        sessions = [session for _, session in results]
        self.assertEqual(32, len(set(sessions)))
        self.assertEqual(
            [f"session-{index}" for index in range(32)],
            sessions,
        )

    def test_oauth_client_uses_request_scoped_pkce_storage(self):
        from supabase_clients import OAuthPkceStorage, create_oauth_client

        storage = OAuthPkceStorage()
        captured = {}

        def factory(url, key, options=None):
            captured["options"] = options
            return {"url": url, "key": key}

        create_oauth_client(
            "https://example.supabase.co",
            "key",
            factory=factory,
            storage=storage,
            options_factory=lambda **kwargs: kwargs,
        )

        self.assertIs(storage, captured["options"]["storage"])

    def test_pkce_verifier_can_cross_flask_request_boundary(self):
        from supabase_clients import OAuthPkceStorage

        storage = OAuthPkceStorage()
        storage.set_item(
            "sb-example-auth-token-code-verifier",
            "verifier-secret",
        )

        self.assertEqual("verifier-secret", storage.code_verifier())
        self.assertIsNone(storage.get_item("unrelated"))

        storage.remove_item("sb-example-auth-token-code-verifier")
        self.assertIsNone(storage.code_verifier())

    def test_app_uses_local_oauth_client_for_auth_mutations(self):
        source = (
            Path(__file__)
            .resolve()
            .parents[1]
            .joinpath("app.py")
            .read_text()
        )

        self.assertIn("supabase_machine", source)
        self.assertNotIn(
            "supabase_machine.auth.exchange_code_for_session",
            source,
        )
        self.assertIn(
            "oauth_client.auth.exchange_code_for_session",
            source,
        )
        self.assertNotIn(
            "supabase.auth.exchange_code_for_session",
            source,
        )
        self.assertIn('session["oauth_code_verifier"]', source)
        self.assertIn('"code_verifier": code_verifier', source)

    def test_public_message_routes_bypass_admin_session_middleware(self):
        source = (
            Path(__file__)
            .resolve()
            .parents[1]
            .joinpath("app.py")
            .read_text()
        )
        middleware = source[
            source.index("def check_admin_inactivity():") :
            source.index("\ndef get_dynamic_anomalies()")
        ]

        self.assertIn('"message_delivery"', middleware)
        self.assertIn('"device_temperature_settings"', middleware)


if __name__ == "__main__":
    unittest.main()
