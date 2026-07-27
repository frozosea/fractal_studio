FROM node:22-bookworm-slim

WORKDIR /app
RUN corepack enable

COPY frontend/package.json frontend/pnpm-lock.yaml ./
RUN pnpm install --frozen-lockfile
RUN sha256sum pnpm-lock.yaml | awk '{print $1}' > node_modules/.fractal-lock.sha256

COPY frontend/ ./

EXPOSE 3000
CMD ["pnpm", "dev", "--hostname", "0.0.0.0", "--port", "3000"]
