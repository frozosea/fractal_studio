"""Active AI provider resolution shared by the streaming and listing clients.

Both clients accept either the typed ``Settings`` or test ``SimpleNamespace``
objects that carry the same field names, so resolution is duck-typed and never
reveals a secret into logs or error text.
"""

from __future__ import annotations

from app.core.config import reveal_secret


def uses_deepseek(settings: object) -> bool:
    return getattr(settings, "ai_provider", "siliconflow") == "deepseek"


def provider_endpoint(settings: object) -> tuple[str, str, str]:
    """Return the active provider's (base_url, api_key, model)."""

    if uses_deepseek(settings):
        return (
            settings.deepseek_base_url,
            reveal_secret(settings.deepseek_api_key),
            settings.deepseek_model,
        )
    return (
        settings.siliconflow_base_url,
        reveal_secret(settings.siliconflow_api_key),
        settings.siliconflow_model,
    )
