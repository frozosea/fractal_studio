FROM ghcr.io/astral-sh/uv:0.11.31-python3.12-trixie-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    UV_LINK_MODE=copy \
    UV_NO_DEV=1 \
    UV_PROJECT_ENVIRONMENT=/opt/venv \
    UV_DEFAULT_INDEX=https://pypi.org/simple

WORKDIR /app

# Keep the official Debian source: the previously configured mirror may reject
# Docker Desktop requests with 403, preventing workers from being deployed.
RUN printf 'Acquire::http::Timeout "30";\nAcquire::https::Timeout "30";\nAcquire::Retries "3";\n' \
        > /etc/apt/apt.conf.d/99-timeout \
    && apt-get update \
    && apt-get install -y --no-install-recommends build-essential cmake ffmpeg fonts-dejavu-core \
    && rm -rf /var/lib/apt/lists/*

COPY pyproject.toml uv.lock ./
# Lockfile came from a mirror that rejects some Docker Desktop traffic with 403.
# Keep source lock untouched in git, rewrite only image-local artifact URLs.
RUN sed -i 's#https://pypi.tuna.tsinghua.edu.cn#https://files.pythonhosted.org#g' uv.lock \
    && uv sync --locked --no-install-project

COPY . ./
