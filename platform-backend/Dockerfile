FROM ghcr.io/astral-sh/uv:0.11.31-python3.12-trixie-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    UV_LINK_MODE=copy \
    UV_NO_DEV=1 \
    UV_PROJECT_ENVIRONMENT=/opt/venv \
    UV_DEFAULT_INDEX=https://pypi.tuna.tsinghua.edu.cn/simple

WORKDIR /app

# deb.debian.org resolves through the host's proxy at ~57 KB/s here; the Tsinghua mirror
# is reached directly. apt's http method also has no default timeout, so a silently
# dropped connection wedges the build forever instead of failing.
RUN printf 'Acquire::http::Timeout "30";\nAcquire::https::Timeout "30";\nAcquire::Retries "3";\n' \
        > /etc/apt/apt.conf.d/99-timeout \
    && sed -i -e 's|deb.debian.org|mirrors.tuna.tsinghua.edu.cn|g' \
              -e 's|security.debian.org|mirrors.tuna.tsinghua.edu.cn|g' \
        /etc/apt/sources.list.d/debian.sources \
    && apt-get update \
    && apt-get install -y --no-install-recommends build-essential cmake ffmpeg fonts-dejavu-core \
    && rm -rf /var/lib/apt/lists/*

COPY pyproject.toml uv.lock ./
RUN uv sync --locked --no-install-project

COPY . ./
