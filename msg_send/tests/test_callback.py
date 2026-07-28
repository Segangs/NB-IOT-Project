from __future__ import annotations

from datetime import datetime, timezone
from decimal import Decimal
import unittest

from msg_send.callback import (
    CallbackContractError,
    parse_push_result,
)
from msg_send.config import TariffConfig
from msg_send.correlation import (
    lease_uuid_to_refkey,
    refkey_to_lease_uuid,
)
from msg_send.domain import DeliveryChannel


LEASE_UUID = "12345678-1234-4abc-8def-1234567890ab"
REFKEY = "1234567812344abc8def1234567890ab"


class CorrelationTests(unittest.TestCase):
    def test_uuid_and_provider_refkey_mapping_is_exact_and_reversible(
        self,
    ) -> None:
        self.assertEqual(lease_uuid_to_refkey(LEASE_UUID), REFKEY)
        self.assertEqual(refkey_to_lease_uuid(REFKEY), LEASE_UUID)
        self.assertEqual(len(REFKEY), 32)
        self.assertEqual(REFKEY, REFKEY.lower())

    def test_refkey_rejects_noncanonical_values(self) -> None:
        for value in (
            REFKEY.upper(),
            f"prefix{REFKEY}",
            "1234",
            "z" * 32,
        ):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    refkey_to_lease_uuid(value)


class CallbackParserTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tariffs = TariffConfig(
            alimtalk=Decimal("8.5000"),
            sms=Decimal("12.0000"),
        )

    def test_uppercase_alimtalk_push_maps_7000_and_explicit_tariff(
        self,
    ) -> None:
        result = parse_push_result(
            {
                "CMSGID": "provider-request-1",
                "REFKEY": REFKEY,
                "MSGID": "provider-result-1",
                "UNIXTIME": "1785038400",
                "RESULT": "7000",
                "MEDIA": "AT",
            },
            self.tariffs,
        )

        self.assertEqual(result.request_id, "provider-request-1")
        self.assertEqual(result.submission_token, LEASE_UUID)
        self.assertEqual(result.result_id, "provider-result-1")
        self.assertIs(result.channel, DeliveryChannel.ALIMTALK)
        self.assertTrue(result.delivered)
        self.assertEqual(result.failure_code, None)
        self.assertEqual(result.cost_amount, Decimal("8.5000"))
        self.assertEqual(
            result.result_at,
            datetime.fromtimestamp(1785038400, tz=timezone.utc),
        )

    def test_sms_push_uses_4100_and_sms_tariff(self) -> None:
        result = parse_push_result(
            {
                "CMSGID": "provider-request-2",
                "REFKEY": REFKEY,
                "MSGID": "provider-result-2",
                "UNIXTIME": 1785038401,
                "RESULT": 4100,
                "MEDIA": "SMS",
            },
            self.tariffs,
        )
        self.assertIs(result.channel, DeliveryChannel.SMS)
        self.assertTrue(result.delivered)
        self.assertEqual(result.cost_amount, Decimal("12.0000"))

    def test_wrong_success_code_or_unknown_channel_is_not_accepted(
        self,
    ) -> None:
        wrong_code = parse_push_result(
            {
                "CMSGID": "provider-request-3",
                "REFKEY": REFKEY,
                "MSGID": "provider-result-3",
                "UNIXTIME": "1785038402",
                "RESULT": "4100",
                "MEDIA": "AT",
            },
            self.tariffs,
        )
        self.assertFalse(wrong_code.delivered)
        self.assertEqual(wrong_code.failure_code, "4100")

        with self.assertRaisesRegex(CallbackContractError, "MEDIA"):
            parse_push_result(
                {
                    "CMSGID": "provider-request-4",
                    "REFKEY": REFKEY,
                    "MSGID": "provider-result-4",
                    "UNIXTIME": "1785038403",
                    "RESULT": "7000",
                    "MEDIA": "FT",
                },
                self.tariffs,
            )

    def test_lowercase_keys_and_invalid_refkey_fail_closed(self) -> None:
        with self.assertRaises(CallbackContractError):
            parse_push_result(
                {
                    "cmsgid": "provider-request-5",
                    "refkey": REFKEY,
                    "msgid": "provider-result-5",
                    "unixtime": "1785038404",
                    "result": "7000",
                    "media": "AT",
                },
                self.tariffs,
            )
        with self.assertRaises(CallbackContractError):
            parse_push_result(
                {
                    "CMSGID": "provider-request-6",
                    "REFKEY": REFKEY.upper(),
                    "MSGID": "provider-result-6",
                    "UNIXTIME": "1785038405",
                    "RESULT": "7000",
                    "MEDIA": "AT",
                },
                self.tariffs,
            )


if __name__ == "__main__":
    unittest.main()
