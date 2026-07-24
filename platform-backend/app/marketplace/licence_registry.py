"""Server-owned immutable licence templates. Browser never supplies sale terms."""

from __future__ import annotations

from copy import deepcopy

from fastapi import HTTPException, status


_TERMS: dict[tuple[str, str], dict[str, object]] = {
    ("personal", "v1"): {
        "code": "personal",
        "termsVersion": "v1",
        "commercialUse": False,
        "redistribution": False,
        "attributionRequired": False,
    },
    ("commercial", "v1"): {
        "code": "commercial",
        "termsVersion": "v1",
        "commercialUse": True,
        "redistribution": False,
        "attributionRequired": False,
    },
}


class LicenceRegistry:
    def resolve_immutable_terms(self, *, code: str, terms_version: str) -> dict[str, object]:
        terms = _TERMS.get((code, terms_version))
        if terms is None:
            raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="unknown_licence")
        return deepcopy(terms)
