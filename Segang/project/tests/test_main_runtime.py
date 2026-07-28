from __future__ import annotations

import os
import sys
from pathlib import Path
from types import ModuleType
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

import main as main_module


class _FakeResponse:
    def __enter__(self) -> "_FakeResponse":
        return self

    def __exit__(self, *args: object) -> None:
        del args

    def read(self) -> bytes:
        return b"OK"


class DuckDnsRuntimeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.original_token = os.environ.pop("DUCKDNS_TOKEN", None)
        self.original_domain = os.environ.pop("DUCKDNS_DOMAIN", None)
        self.original_urlopen = main_module.urllib.request.urlopen
        self.original_get_interface_ip = main_module.get_interface_ip
        self.urls: list[str] = []

        main_module.get_interface_ip = lambda _interface: "203.0.113.9"

        def fake_urlopen(request: object, timeout: int) -> _FakeResponse:
            del timeout
            self.urls.append(request.full_url)
            return _FakeResponse()

        main_module.urllib.request.urlopen = fake_urlopen

    def tearDown(self) -> None:
        main_module.urllib.request.urlopen = self.original_urlopen
        main_module.get_interface_ip = self.original_get_interface_ip
        if self.original_token is not None:
            os.environ["DUCKDNS_TOKEN"] = self.original_token
        else:
            os.environ.pop("DUCKDNS_TOKEN", None)
        if self.original_domain is not None:
            os.environ["DUCKDNS_DOMAIN"] = self.original_domain
        else:
            os.environ.pop("DUCKDNS_DOMAIN", None)

    def test_missing_token_skips_external_update(self) -> None:
        main_module.update_duckdns()

        self.assertEqual(self.urls, [])

    def test_configured_token_and_domain_build_request(self) -> None:
        os.environ["DUCKDNS_TOKEN"] = "TOKEN_CANARY"
        os.environ["DUCKDNS_DOMAIN"] = "coldroom"

        main_module.update_duckdns()

        self.assertEqual(len(self.urls), 1)
        self.assertIn("domains=coldroom", self.urls[0])
        self.assertIn("token=TOKEN_CANARY", self.urls[0])
        self.assertIn("ip=203.0.113.9", self.urls[0])


if __name__ == "__main__":
    unittest.main()
