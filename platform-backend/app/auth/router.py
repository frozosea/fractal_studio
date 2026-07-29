"""M1 HTTP routes: opaque cookie sessions, profile/RBAC and CSRF token."""

from __future__ import annotations

from fastapi import APIRouter, Depends, Header, HTTPException, Request, Response, status
from fastapi.responses import JSONResponse

from app.auth import service
from app.auth.rate_limit import AuthRateLimitUnavailable, AuthRateLimiter
from app.auth.models import AccessPrincipal, CreatorProfileInput, CredentialsInput
from app.core.access_middleware import (
    csrf_token,
    enforce_origin_and_csrf,
    enforce_same_origin_or_no_origin,
    require_principal,
)
from app.core.config import get_settings


router = APIRouter(prefix="/v1", tags=["auth"])


def _set_session_cookie(response: Response, session_token: str) -> None:
    settings = get_settings()
    response.set_cookie(
        key="fs_session",
        value=session_token,
        max_age=settings.session_ttl_days * 24 * 60 * 60,
        httponly=True,
        secure=settings.session_cookie_secure,
        samesite="lax",
        path="/",
    )


def _session_body(user: object, session_token: str) -> dict[str, object]:
    """
    Sign-in payload, including the raw session token.

    The cookie is shared by every tab in a browser window, so a tab that wants
    its own account keeps this token in sessionStorage and sends it as a bearer
    token. That deliberately puts the token within reach of JavaScript, which
    the HttpOnly cookie alone is not — the trade accepted to support several
    accounts signed in side by side in one window.
    """
    body = user.model_dump(mode="json", by_alias=True)  # type: ignore[attr-defined]
    body["sessionToken"] = session_token
    return body


def _clear_session_cookie(response: Response) -> None:
    response.delete_cookie(
        key="fs_session",
        path="/",
        httponly=True,
        secure=get_settings().session_cookie_secure,
        samesite="lax",
    )


@router.post("/auth/register", status_code=status.HTTP_201_CREATED)
async def register(payload: CredentialsInput, request: Request, response: Response) -> dict[str, object]:
    enforce_same_origin_or_no_origin(request)
    try:
        await AuthRateLimiter().enforce(action="register", email=payload.email, request=request)
    except AuthRateLimitUnavailable as error:
        raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail="auth_rate_limit_unavailable") from error
    user, session_token = await service.register(payload.email, payload.password, request)
    _set_session_cookie(response, session_token)
    return {"data": _session_body(user, session_token)}


@router.post("/auth/login")
async def login(payload: CredentialsInput, request: Request, response: Response) -> dict[str, object]:
    enforce_same_origin_or_no_origin(request)
    try:
        await AuthRateLimiter().enforce(action="login", email=payload.email, request=request)
    except AuthRateLimitUnavailable as error:
        raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail="auth_rate_limit_unavailable") from error
    user, session_token = await service.login(payload.email, payload.password, request)
    _set_session_cookie(response, session_token)
    return {"data": _session_body(user, session_token)}


@router.post("/auth/logout", status_code=status.HTTP_204_NO_CONTENT)
async def logout(
    request: Request, response: Response, principal: AccessPrincipal = Depends(require_principal)
) -> Response:
    enforce_origin_and_csrf(request, principal)
    await service.logout(principal, request)
    # A tab signed in with a bearer token must not clear the shared cookie:
    # that cookie belongs to whichever account a sibling tab is using.
    if getattr(request.state, "session_source", "cookie") == "cookie":
        _clear_session_cookie(response)
    response.status_code = status.HTTP_204_NO_CONTENT
    return response


@router.get("/me")
async def me(principal: AccessPrincipal = Depends(require_principal)) -> dict[str, object]:
    user = await service.current_user(principal)
    return {"data": user.model_dump(mode="json", by_alias=True)}


@router.get("/auth/session-token")
async def adopt_session(principal: AccessPrincipal = Depends(require_principal)) -> dict[str, object]:
    """
    Hand this tab the raw token of the session it just authenticated with.

    A tab that authenticated by cookie stays coupled to every other tab in the
    window — the cookie is shared, and the next sign-in anywhere re-points it.
    Calling this once turns that into a bearer token the tab keeps in its own
    sessionStorage, after which the tab never reads the cookie again. Same
    exposure as the sign-in payload: deliberately reachable from JavaScript.
    """
    return {"data": {"sessionToken": principal.session_token}}


@router.get("/auth/csrf-token")
async def get_csrf_token(principal: AccessPrincipal = Depends(require_principal)) -> dict[str, object]:
    return {"data": {"token": csrf_token(principal.session_token)}}


@router.patch("/me/creator-profile")
async def creator_profile(
    payload: CreatorProfileInput,
    request: Request,
    idempotency_key: str = Header(..., alias="Idempotency-Key"),
    principal: AccessPrincipal = Depends(require_principal),
) -> Response:
    enforce_origin_and_csrf(request, principal)
    body, session_token, replayed, stored_headers = await service.upsert_creator_profile(
        principal, payload, idempotency_key, request
    )
    response = JSONResponse(content=body, status_code=status.HTTP_200_OK)
    for name, value in stored_headers.items():
        response.headers[name] = value
    if session_token is not None and not replayed:
        _set_session_cookie(response, session_token)
        # The rotated token has to reach a bearer tab too, or its stored token
        # is the revoked one and the next request 401s.
        if getattr(request.state, "session_source", "cookie") == "bearer":
            response.headers["X-Session-Token"] = session_token
    return response
