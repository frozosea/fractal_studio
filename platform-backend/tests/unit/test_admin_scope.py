from uuid import UUID

from fastapi import HTTPException, Request
import pytest

from app.auth.models import AccessPrincipal
from app.core.access_middleware import enforce_account_scope


def _request(path: str) -> Request:
    return Request({"type": "http", "method": "GET", "path": path, "headers": []})


def _principal(*roles: str) -> AccessPrincipal:
    return AccessPrincipal(
        user_id=UUID("00000000-0000-4000-8000-000000000006"),
        session_id=UUID("00000000-0000-4000-8000-000000000007"),
        roles=frozenset(roles),
        session_token="test-session-token",
    )


@pytest.mark.parametrize(
    "path",
    [
        "/v1/me",
        "/v1/auth/logout",
        "/v1/auth/session-token",
        "/v1/auth/csrf-token",
        "/internal/v1/admin/statistics",
        "/internal/v1/admin/compute-nodes",
        "/internal/v1/admin/users/00000000-0000-4000-8000-000000000001",
    ],
)
def test_administrator_can_only_use_identity_and_administration_apis(path: str) -> None:
    enforce_account_scope(_request(path), _principal("admin"))


@pytest.mark.parametrize(
    "path",
    [
        "/v1/studio/capabilities",
        "/v1/studio/preview",
        "/v1/me/assets",
        "/v1/me/purchases",
        "/v1/membership/checkout",
        "/internal/v1/payout-requests",
    ],
)
def test_administrator_is_rejected_from_customer_and_creator_apis(path: str) -> None:
    with pytest.raises(HTTPException) as error:
        enforce_account_scope(_request(path), _principal("admin"))
    assert error.value.status_code == 403
    assert error.value.detail == "admin_scope_only"


def test_regular_account_scope_is_unchanged() -> None:
    enforce_account_scope(_request("/v1/studio/preview"), _principal("creator"))
