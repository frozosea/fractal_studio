# Pinned, and pinned to 24 specifically.
#
# `next dev` compiles the next-intl middleware with an eval-based devtool and
# the edge runtime then evaluates it in a `vm` context created with
# `codeGeneration: { strings: false }`. Node 22.23.2 enforces that and throws
# `EvalError: Code generation from strings disallowed` on every request the
# middleware matches — which is every page, so dev 500s completely. Production
# `next start` never hits that path, which is why the breakage stayed hidden
# while this service was being run in production mode.
#
# Node 24 does not reject it, so dev works. Node 20 is not an option: pnpm 11
# requires 22+ and dies with ERR_UNKNOWN_BUILTIN_MODULE.
#
# The tag is pinned to a patch because this arrived through a floating
# `node:22-bookworm-slim` changing underneath the image.
FROM node:24.18.0-bookworm-slim

WORKDIR /app
RUN npm install --global pnpm@11.17.0 --registry=https://registry.npmmirror.com

ENV NEXT_TELEMETRY_DISABLED=1 \
    NEXT_PUBLIC_PLATFORM_API_URL=/platform \
    PLATFORM_INTERNAL_URL=http://api:8000

COPY frontend/package.json frontend/pnpm-lock.yaml ./
RUN pnpm install --frozen-lockfile --registry=https://registry.npmmirror.com
RUN sha256sum pnpm-lock.yaml | awk '{print $1}' > node_modules/.fractal-lock.sha256

COPY frontend/ ./

EXPOSE 3000
CMD ["pnpm", "dev", "--hostname", "0.0.0.0", "--port", "3000"]
