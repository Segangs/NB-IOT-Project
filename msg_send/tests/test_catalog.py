from __future__ import annotations

import unittest

from msg_send.catalog import (
    CatalogContractError,
    TemplateCatalog,
)


EXPECTED_TEMPLATES = {
    "temperature_high": (
        "bizp_2026070914231016762370714",
        "온도",
        "[알림] 온도이상",
        "이상온도 감지 알림",
        "현재 온도 #{sensorValue}℃",
        {
            "WorkplaceName",
            "SettingsToken",
            "sensorValue",
            "sensorvaluetime",
            "setTmpUpLimit",
            "tempHistoryToken",
            "userMachineName",
        },
    ),
    "temperature_recovered": (
        "bizp_2026071314514616762878062",
        "온도",
        "[알림] 온도복귀",
        "이상온도 복귀 알림",
        "온도 복귀 #{sensorValue}℃",
        {
            "SettingsToken",
            "sensorValue",
            "setTmpUpLimit",
            "tempHistoryToken",
            "userMachineName",
            "workplaceName",
        },
    ),
    "device_boot": (
        "bizp_2026071315003676625784727",
        "전원",
        "[전원] 기기부팅",
        "기기 알림",
        "기기 켜짐 알림",
        {
            "SettingsToken",
            "eventTime",
            "tempHistoryToken",
            "userMachineName",
            "workplaceName",
        },
    ),
    "device_disconnected": (
        "bizp_2026071315185116762787563",
        "전원",
        "[전원] 연결끊김",
        "기기 알림",
        "기기 통신 오류",
        {
            "SettingsToken",
            "lastSeenAt",
            "tempHistoryToken",
            "userMachineName",
            "workplaceName",
        },
    ),
    "adapter_removed": (
        "bizp_2026071315030116762724896",
        "전원",
        "[전원] 전원분리 및 꺼짐예고",
        "기기 알림",
        "기기 꺼짐(예정) 알림",
        {
            "SettingsToken",
            "eventTime",
            "tempHistoryToken",
            "userMachineName",
            "workplaceName",
        },
    ),
    "power_shutdown": (
        "bizp_2026071315073916762730996",
        "전원",
        "[전원] 꺼짐알림",
        "기기 알림",
        "기기 꺼짐 알림",
        {
            "SettingsToken",
            "eventTime",
            "tempHistoryToken",
            "userMachineName",
            "workplaceName",
        },
    ),
    "adapter_restored": (
        "bizp_2026071315101276625891453",
        "전원",
        "[전원] 전원복구",
        "기기 알림",
        "전원 복구 알림",
        {
            "SettingsToken",
            "eventTime",
            "tempHistoryToken",
            "userMachineName",
            "workplaceName",
        },
    ),
    "sensor_fault": (
        "bizp_2026071315135276625976752",
        "센서",
        "[센서] 센서이상",
        "센서 알림",
        "센서 이상 알림",
        {
            "SettingsToken",
            "eventTime",
            "sensorName",
            "sensorStatus",
            "tempHistoryToken",
            "userMachineName",
            "workplaceName",
        },
    ),
}


class ApprovedCatalogTests(unittest.TestCase):
    def setUp(self) -> None:
        self.catalog = TemplateCatalog.load_approved()

    def test_catalog_contains_all_eight_exact_source_entries(self) -> None:
        self.assertEqual(set(self.catalog.event_keys), set(EXPECTED_TEMPLATES))
        self.assertEqual(len(self.catalog.templates), 8)
        self.assertEqual(len(set(self.catalog.template_codes)), 8)

        for event_key, expected in EXPECTED_TEMPLATES.items():
            template = self.catalog.by_event(event_key)
            (
                code,
                classification,
                name,
                auxiliary,
                emphasis_title,
                variables,
            ) = expected
            self.assertEqual(template.template_code, code)
            self.assertEqual(template.classification, classification)
            self.assertEqual(template.name, name)
            self.assertEqual(template.emphasis_auxiliary, auxiliary)
            self.assertEqual(template.emphasis_title, emphasis_title)
            self.assertEqual(set(template.variables), variables)
            self.assertEqual(
                [button.name for button in template.buttons],
                ["온도 이력 확인", "설정 변경"],
            )
            self.assertTrue(template.body)
            self.assertTrue(template.supplementary_text)
            self.assertIsInstance(template.condition, str)

    def test_catalog_source_revision_and_checksums_are_verified(self) -> None:
        self.assertEqual(self.catalog.revision, "2026-07-27")
        self.assertEqual(
            self.catalog.source_file_sha256,
            "551dbda50547c9ee88c9170e574437da3"
            "e642d2cc0f269066a0533b6da69225c",
        )
        self.assertEqual(self.catalog.source_filename, "nb_iot_template_rows.json")
        self.assertEqual(self.catalog.source_kind, "google_sheets_export")
        self.assertEqual(self.catalog.source_captured_at, "2026-07-26")
        self.assertEqual(
            self.catalog.source_columns,
            (
                "classification",
                "template_code",
                "name",
                "emphasis_auxiliary",
                "emphasis_title",
                "body",
                "supplementary_text",
                "buttons",
                "condition",
            ),
        )
        self.assertEqual(
            self.catalog.source_rows_sha256,
            "03dcd14579bc5b33a652035ce2908e65cfbde886aca07c509418dca3bdd7cd10",
        )
        self.assertEqual(len(self.catalog.catalog_checksum_sha256), 64)

    def test_power_event_bodies_match_bizppurio_approved_text_exactly(
        self,
    ) -> None:
        expected_bodies = {
            "adapter_removed": (
                "기기의 전원 어댑터가 분리되어 곧 전원이 꺼집니다.\n\n"
                "- 사업장명 : #{workplaceName}\n"
                "- 기기명 : #{userMachineName}\n\n"
                "- 감지시간 : #{eventTime}\n"
                "- 예상종료 : 약 5분 이내\n\n"
                "사업장 전체 전원, 기기 주변 콘센트, 어댑터 연결 상태를 "
                "확인해 주시기 바랍니다.\n\n"
                "전원이 꺼지기 전에 어댑터를 신속히 다시 연결해 주십시오."
            ),
            "power_shutdown": (
                "어댑터 분리 후 5분이 경과하여 기기가 꺼집니다.\n\n"
                "- 사업장명 : #{workplaceName}\n"
                "- 기기명 : #{userMachineName}\n"
                "- 종료시간 : #{eventTime}\n\n"
                "전원 연결 후 기기 하단의 전원 버튼을 눌러 기기를 다시 "
                "켜 주십시오."
            ),
            "adapter_restored": (
                "분리되었던 전원 어댑터가 다시 연결되어 기기가 정상 구동 "
                "중입니다.\n\n"
                "- 사업장명 : #{workplaceName}\n"
                "- 기기명 : #{userMachineName}\n"
                "- 복구시간 : #{eventTime}\n\n"
                "냉장/냉동고의 온도와 기기 상태가 정상적으로 유지되는지 "
                "확인해 주시기 바랍니다."
            ),
        }

        for event_key, expected_body in expected_bodies.items():
            with self.subTest(event_key=event_key):
                self.assertEqual(
                    self.catalog.by_event(event_key).body,
                    expected_body,
                )

    def test_production_render_uses_emphasis_title_body_and_exact_buttons(
        self,
    ) -> None:
        rendered = self.catalog.render(
            "bizp_2026071315003676625784727",
            {
                "workplaceName": "서울 1호점",
                "userMachineName": "냉동고 A",
                "eventTime": "2026-07-26 12:30",
            },
            history_url="https://zxcx.io/s/history_token_123456",
            settings_url="https://zxcx.io/s/settings_token_123456",
        )

        self.assertEqual(rendered.title, "기기 켜짐 알림")
        self.assertEqual(
            rendered.body,
            "기기가 정상적으로 서버와 연결을 시작했습니다.\n\n"
            "- 사업장명 : 서울 1호점\n"
            "- 기기명 : 냉동고 A\n"
            "- 부팅시간 : 2026-07-26 12:30",
        )
        self.assertEqual(
            [button.as_payload() for button in rendered.buttons],
            [
                {
                    "name": "온도 이력 확인",
                    "type": "WL",
                    "url_mobile": "https://zxcx.io/s/history_token_123456",
                    "url_pc": "https://zxcx.io/s/history_token_123456",
                },
                {
                    "name": "설정 변경",
                    "type": "WL",
                    "url_mobile": "https://zxcx.io/s/settings_token_123456",
                    "url_pc": "https://zxcx.io/s/settings_token_123456",
                },
            ],
        )
        self.assertEqual(
            set(rendered.as_provider_fields()),
            {"title", "message", "button"},
        )

    def test_high_temperature_preserves_uppercase_workplace_variable(self) -> None:
        common = {
            "userMachineName": "냉동고 A",
            "sensorValue": "12.5",
            "setTmpUpLimit": "-15",
            "sensorvaluetime": "2026-07-26 12:31",
        }
        with self.assertRaisesRegex(
            CatalogContractError,
            "WorkplaceName",
        ):
            self.catalog.render(
                "bizp_2026070914231016762370714",
                {**common, "workplaceName": "wrong case"},
                history_url="https://zxcx.io/s/history_token_123456",
                settings_url="https://zxcx.io/s/settings_token_123456",
            )

        rendered = self.catalog.render(
            "bizp_2026070914231016762370714",
            {**common, "WorkplaceName": "서울 1호점"},
            history_url="https://zxcx.io/s/history_token_123456",
            settings_url="https://zxcx.io/s/settings_token_123456",
        )
        self.assertIn("서울 1호점", rendered.body)

    def test_malformed_history_marker_never_leaves_marker_or_parenthesis(
        self,
    ) -> None:
        rendered = self.catalog.render(
            "bizp_2026071315003676625784727",
            {
                "workplaceName": "서울 1호점",
                "userMachineName": "냉동고 A",
                "eventTime": "2026-07-26 12:30",
            },
            history_url="https://zxcx.io/s/history_token_123456",
            settings_url="https://zxcx.io/s/settings_token_123456",
        )
        history_button = rendered.buttons[0]
        self.assertNotIn("#{", history_button.url_mobile)
        self.assertNotIn(")", history_button.url_mobile)

    def test_render_rejects_unresolved_variables_and_non_opaque_links(
        self,
    ) -> None:
        valid_params = {
            "workplaceName": "서울 1호점",
            "userMachineName": "냉동고 A",
            "eventTime": "#{notResolved}",
        }
        with self.assertRaises(CatalogContractError):
            self.catalog.render(
                "bizp_2026071315003676625784727",
                valid_params,
                history_url="https://zxcx.io/s/history_token_123456",
                settings_url="https://zxcx.io/s/settings_token_123456",
            )
        with self.assertRaises(CatalogContractError):
            self.catalog.render(
                "bizp_2026071315003676625784727",
                {**valid_params, "eventTime": "2026-07-26 12:30"},
                history_url="http://zxcx.io/s/history_token_123456",
                settings_url="https://zxcx.io/s/settings_token_123456",
            )
        with self.assertRaises(CatalogContractError):
            self.catalog.render(
                "bizp_2026071315003676625784727",
                {**valid_params, "eventTime": "2026-07-26 12:30"},
                history_url="https://evil.example/s/history_token_123456",
                settings_url="https://zxcx.io/s/settings_token_123456",
            )


if __name__ == "__main__":
    unittest.main()
