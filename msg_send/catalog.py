from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
from typing import Any, Mapping, Sequence
from urllib.parse import urlsplit


_MARKER_RE = re.compile(r"#\{([A-Za-z][A-Za-z0-9]*)\}")
_OPAQUE_TOKEN_RE = re.compile(r"[A-Za-z0-9_-]{8,256}")
_CHECKSUM_RE = re.compile(r"[0-9a-f]{64}")
_LINK_VARIABLES = frozenset({"tempHistoryToken", "SettingsToken"})
_SOURCE_COLUMNS = (
    "classification",
    "template_code",
    "name",
    "emphasis_auxiliary",
    "emphasis_title",
    "body",
    "supplementary_text",
    "buttons",
    "condition",
)


class CatalogContractError(ValueError):
    """Raised when the tracked provider template contract is invalid."""


@dataclass(frozen=True)
class TemplateButton:
    name: str
    token_variable: str


@dataclass(frozen=True)
class RenderedButton:
    name: str
    url_mobile: str

    def as_payload(self) -> dict[str, str]:
        return {
            "name": self.name,
            "type": "WL",
            "url_mobile": self.url_mobile,
            "url_pc": self.url_mobile,
        }


@dataclass(frozen=True)
class ApprovedTemplate:
    event_key: str
    classification: str
    template_code: str
    name: str
    emphasis_auxiliary: str
    emphasis_title: str
    body: str
    supplementary_text: str
    buttons_source: str
    condition: str
    variables: tuple[str, ...]
    buttons: tuple[TemplateButton, ...]

    @property
    def content_variables(self) -> frozenset[str]:
        return frozenset(self.variables) - _LINK_VARIABLES


@dataclass(frozen=True)
class RenderedTemplate:
    template_code: str
    title: str
    body: str
    buttons: tuple[RenderedButton, ...]

    def as_provider_fields(self) -> dict[str, object]:
        return {
            "title": self.title,
            "message": self.body,
            "button": [
                button.as_payload() for button in self.buttons
            ],
        }


class TemplateCatalog:
    def __init__(
        self,
        *,
        revision: str,
        source_kind: str,
        source_filename: str,
        source_captured_at: str,
        source_columns: Sequence[str],
        source_file_sha256: str,
        source_rows_sha256: str,
        catalog_checksum_sha256: str,
        templates: Sequence[ApprovedTemplate],
    ) -> None:
        if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", revision):
            raise CatalogContractError("catalog revision must be YYYY-MM-DD")
        if not source_filename:
            raise CatalogContractError("source filename is required")
        if source_kind != "google_sheets_export":
            raise CatalogContractError("invalid catalog source kind")
        if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", source_captured_at):
            raise CatalogContractError("invalid source capture date")
        if tuple(source_columns) != _SOURCE_COLUMNS:
            raise CatalogContractError("invalid source column metadata")
        if not _CHECKSUM_RE.fullmatch(source_file_sha256):
            raise CatalogContractError("invalid source checksum")
        if not _CHECKSUM_RE.fullmatch(source_rows_sha256):
            raise CatalogContractError("invalid source rows checksum")
        if not _CHECKSUM_RE.fullmatch(catalog_checksum_sha256):
            raise CatalogContractError("invalid catalog checksum")
        if len(templates) != 8:
            raise CatalogContractError("approved catalog must contain 8 templates")

        by_event = {template.event_key: template for template in templates}
        by_code = {
            template.template_code: template for template in templates
        }
        if len(by_event) != len(templates):
            raise CatalogContractError("duplicate catalog event key")
        if len(by_code) != len(templates):
            raise CatalogContractError("duplicate catalog template code")

        self.revision = revision
        self.source_kind = source_kind
        self.source_filename = source_filename
        self.source_captured_at = source_captured_at
        self.source_columns = tuple(source_columns)
        self.source_file_sha256 = source_file_sha256
        self.source_rows_sha256 = source_rows_sha256
        self.catalog_checksum_sha256 = catalog_checksum_sha256
        self.templates = tuple(templates)
        self._by_event = by_event
        self._by_code = by_code

    @property
    def event_keys(self) -> tuple[str, ...]:
        return tuple(template.event_key for template in self.templates)

    @property
    def template_codes(self) -> tuple[str, ...]:
        return tuple(
            template.template_code for template in self.templates
        )

    @classmethod
    def load_approved(cls) -> "TemplateCatalog":
        path = (
            Path(__file__).resolve().parent
            / "data"
            / "approved_templates.v1.json"
        )
        with path.open("r", encoding="utf-8") as stream:
            payload = json.load(stream)
        return cls.from_mapping(payload)

    @classmethod
    def from_mapping(cls, payload: Mapping[str, Any]) -> "TemplateCatalog":
        try:
            source = payload["source"]
            raw_templates = payload["templates"]
            revision = payload["revision"]
            checksum = payload["catalog_checksum_sha256"]
        except (KeyError, TypeError) as exc:
            raise CatalogContractError("catalog metadata is incomplete") from exc
        if payload.get("schema_version") != 1:
            raise CatalogContractError("unsupported catalog schema version")
        if not isinstance(source, Mapping) or not isinstance(
            raw_templates,
            list,
        ):
            raise CatalogContractError("invalid catalog structure")

        computed_checksum = hashlib.sha256(
            json.dumps(
                raw_templates,
                ensure_ascii=False,
                sort_keys=True,
                separators=(",", ":"),
            ).encode("utf-8")
        ).hexdigest()
        if checksum != computed_checksum:
            raise CatalogContractError("catalog checksum mismatch")
        if source.get("row_count") != len(raw_templates):
            raise CatalogContractError("catalog source row count mismatch")
        source_columns = (
            "classification",
            "template_code",
            "name",
            "emphasis_auxiliary",
            "emphasis_title",
            "body",
            "supplementary_text",
            "buttons_source",
            "condition",
        )
        source_rows = [
            [raw.get(column) for column in source_columns]
            if isinstance(raw, Mapping)
            else []
            for raw in raw_templates
        ]
        computed_source_rows_checksum = hashlib.sha256(
            json.dumps(
                source_rows,
                ensure_ascii=False,
                separators=(",", ":"),
            ).encode("utf-8")
        ).hexdigest()
        source_rows_checksum = source.get("rows_canonical_sha256")
        if source_rows_checksum != computed_source_rows_checksum:
            raise CatalogContractError("source rows checksum mismatch")

        templates = tuple(
            _parse_template(raw) for raw in raw_templates
        )
        return cls(
            revision=str(revision),
            source_kind=str(source.get("kind", "")),
            source_filename=str(source.get("filename", "")),
            source_captured_at=str(source.get("captured_at", "")),
            source_columns=tuple(source.get("columns", ())),
            source_file_sha256=str(source.get("file_sha256", "")),
            source_rows_sha256=str(source_rows_checksum),
            catalog_checksum_sha256=str(checksum),
            templates=templates,
        )

    def by_event(self, event_key: str) -> ApprovedTemplate:
        try:
            return self._by_event[event_key]
        except KeyError as exc:
            raise CatalogContractError("unknown catalog event") from exc

    def by_code(self, template_code: str) -> ApprovedTemplate:
        try:
            return self._by_code[template_code]
        except KeyError as exc:
            raise CatalogContractError("unknown template code") from exc

    def render(
        self,
        template_code: str,
        parameters: Mapping[str, object],
        *,
        history_url: str,
        settings_url: str,
    ) -> RenderedTemplate:
        template = self.by_code(template_code)
        actual_variables = frozenset(parameters)
        expected_variables = template.content_variables
        if actual_variables != expected_variables:
            missing = sorted(expected_variables - actual_variables)
            extra = sorted(actual_variables - expected_variables)
            details = []
            if missing:
                details.append("missing=" + ",".join(missing))
            if extra:
                details.append("extra=" + ",".join(extra))
            raise CatalogContractError(
                "template variables do not match: " + " ".join(details)
            )

        values = {
            key: _safe_parameter_value(value)
            for key, value in parameters.items()
        }
        title = _render_text(template.emphasis_title, values)
        body = _render_text(template.body, values)
        if len(title) > 50:
            raise CatalogContractError("rendered title exceeds 50 characters")
        if len(body) > 1300:
            raise CatalogContractError("rendered body exceeds 1300 characters")

        urls = {
            "tempHistoryToken": _validated_opaque_link(history_url),
            "SettingsToken": _validated_opaque_link(settings_url),
        }
        rendered_buttons = tuple(
            RenderedButton(
                name=button.name,
                url_mobile=urls[button.token_variable],
            )
            for button in template.buttons
        )
        if len(rendered_buttons) > 5:
            raise CatalogContractError("template exceeds five buttons")
        if any(len(button.name) > 28 for button in rendered_buttons):
            raise CatalogContractError(
                "template button name exceeds 28 characters"
            )
        return RenderedTemplate(
            template_code=template.template_code,
            title=title,
            body=body,
            buttons=rendered_buttons,
        )


def _parse_template(raw: object) -> ApprovedTemplate:
    if not isinstance(raw, Mapping):
        raise CatalogContractError("template entry must be an object")
    required_text_fields = (
        "event_key",
        "classification",
        "template_code",
        "name",
        "emphasis_auxiliary",
        "emphasis_title",
        "body",
        "supplementary_text",
        "buttons_source",
    )
    values: dict[str, str] = {}
    for field_name in required_text_fields:
        value = raw.get(field_name)
        if not isinstance(value, str) or not value:
            raise CatalogContractError(
                f"template {field_name} must be non-empty text"
            )
        values[field_name] = value
    condition = raw.get("condition")
    if not isinstance(condition, str):
        raise CatalogContractError("template condition must be text")

    raw_variables = raw.get("variables")
    if (
        not isinstance(raw_variables, list)
        or not raw_variables
        or not all(
            isinstance(variable, str) and variable
            for variable in raw_variables
        )
    ):
        raise CatalogContractError("template variables are invalid")
    variables = tuple(raw_variables)
    if len(set(variables)) != len(variables):
        raise CatalogContractError("duplicate template variable")

    normalized_buttons_source = values["buttons_source"].replace(
        "#{tempHistoryToken)}",
        "#{tempHistoryToken}",
    )
    extracted = set(
        _MARKER_RE.findall(
            values["emphasis_title"]
            + "\n"
            + values["body"]
            + "\n"
            + normalized_buttons_source
        )
    )
    if extracted != set(variables):
        raise CatalogContractError("declared template variables do not match")
    if "#{tempHistoryToken)}" not in values["buttons_source"]:
        raise CatalogContractError(
            "source history marker compatibility metadata missing"
        )

    button_lines = values["buttons_source"].splitlines()
    expected_lines = (
        "온도 이력 확인 https://zxcx.io/s/#{tempHistoryToken)}",
        "설정 변경 https://zxcx.io/s/#{SettingsToken}",
    )
    if tuple(button_lines) != expected_lines:
        raise CatalogContractError("approved button source mismatch")
    buttons = (
        TemplateButton("온도 이력 확인", "tempHistoryToken"),
        TemplateButton("설정 변경", "SettingsToken"),
    )
    return ApprovedTemplate(
        event_key=values["event_key"],
        classification=values["classification"],
        template_code=values["template_code"],
        name=values["name"],
        emphasis_auxiliary=values["emphasis_auxiliary"],
        emphasis_title=values["emphasis_title"],
        body=values["body"],
        supplementary_text=values["supplementary_text"],
        buttons_source=values["buttons_source"],
        condition=condition,
        variables=variables,
        buttons=buttons,
    )


def _safe_parameter_value(value: object) -> str:
    if value is None:
        raise CatalogContractError("template parameter must not be null")
    rendered = str(value)
    if not rendered or "#{" in rendered:
        raise CatalogContractError("template parameter is empty or unresolved")
    return rendered


def _render_text(template: str, values: Mapping[str, str]) -> str:
    rendered = template
    for name, value in values.items():
        rendered = rendered.replace(f"#{{{name}}}", value)
    if "#{" in rendered or _MARKER_RE.search(rendered):
        raise CatalogContractError("rendered template contains a marker")
    return rendered


def _validated_opaque_link(value: str) -> str:
    try:
        parsed = urlsplit(value)
    except ValueError as exc:
        raise CatalogContractError("invalid opaque link") from exc
    path_parts = parsed.path.split("/")
    if (
        parsed.scheme != "https"
        or parsed.netloc != "zxcx.io"
        or parsed.query
        or parsed.fragment
        or len(path_parts) != 3
        or path_parts[:2] != ["", "s"]
        or not _OPAQUE_TOKEN_RE.fullmatch(path_parts[2])
        or ")" in value
        or "#{" in value
    ):
        raise CatalogContractError("invalid opaque link")
    return value
