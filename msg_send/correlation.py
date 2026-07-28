from __future__ import annotations

import re
from uuid import UUID


_REFKEY_RE = re.compile(r"[0-9a-f]{32}")


def lease_uuid_to_refkey(lease_token: str) -> str:
    if not isinstance(lease_token, str) or len(lease_token) != 36:
        raise ValueError("lease token must be a canonical UUID")
    try:
        parsed = UUID(lease_token)
    except (ValueError, AttributeError) as exc:
        raise ValueError("lease token must be a canonical UUID") from exc
    if str(parsed) != lease_token.lower():
        raise ValueError("lease token must be a canonical UUID")
    return parsed.hex


def refkey_to_lease_uuid(refkey: str) -> str:
    if not isinstance(refkey, str) or not _REFKEY_RE.fullmatch(refkey):
        raise ValueError("refkey must be exactly 32 lowercase hex characters")
    return str(UUID(hex=refkey))
