from __future__ import annotations

from dataclasses import dataclass, field
from typing import Mapping, Optional, Protocol
from urllib.error import HTTPError
from urllib.request import Request, urlopen


@dataclass(frozen=True)
class HttpRequest:
    method: str
    url: str
    headers: Mapping[str, str] = field(repr=False)
    body: bytes = field(default=b"", repr=False)
    timeout_seconds: float = 10.0


@dataclass(frozen=True)
class HttpResponse:
    status: int
    headers: Mapping[str, str]
    body: bytes


class HttpTransport(Protocol):
    def request(self, request: HttpRequest) -> HttpResponse:
        ...


class StdlibHttpTransport:
    """One-attempt urllib transport with no application retry loop."""

    def request(self, request: HttpRequest) -> HttpResponse:
        raw_request = Request(
            request.url,
            data=request.body if request.body else None,
            headers=dict(request.headers),
            method=request.method,
        )
        try:
            with urlopen(
                raw_request,
                timeout=request.timeout_seconds,
            ) as response:
                return HttpResponse(
                    status=int(response.status),
                    headers=dict(response.headers.items()),
                    body=response.read(),
                )
        except HTTPError as exc:
            return HttpResponse(
                status=int(exc.code),
                headers=dict(exc.headers.items()) if exc.headers else {},
                body=exc.read(),
            )
