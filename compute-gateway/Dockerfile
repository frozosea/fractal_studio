FROM ghcr.io/astral-sh/uv:0.11.31-python3.12-trixie-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    UV_LINK_MODE=copy \
    UV_PROJECT_ENVIRONMENT=/opt/venv

WORKDIR /app

COPY pyproject.toml uv.lock ./
# Precompile dependencies at image-build time. On Docker Desktop this avoids a
# multi-minute first import of FastAPI/Pydantic before the health endpoint binds.
RUN uv sync --locked --no-dev --no-install-project --compile-bytecode

COPY . ./

CMD ["/opt/venv/bin/uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8080"]
