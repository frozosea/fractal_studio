#!/usr/bin/env bash
set -euo pipefail

PUBLIC_DOMAIN=${PUBLIC_DOMAIN:-fractal.kevin0412.top}

echo "FRP and Caddy services"
systemctl is-active frps
systemctl is-active caddy

echo "Expected VPS listeners"
ss -lnt | awk 'NR == 1 || /:(7000|12222|13010|18110|19020)[[:space:]]/'

echo "Private services through FRP"
curl -fsSI --max-time 10 http://127.0.0.1:13010/ >/dev/null
curl -fsS --max-time 10 http://127.0.0.1:18110/healthz
echo
curl -fsS --max-time 10 http://127.0.0.1:19020/minio/health/live
echo

echo "Public HTTPS routes"
curl -fsSI --max-time 15 "https://${PUBLIC_DOMAIN}/" >/dev/null
curl -fsS --max-time 15 "https://${PUBLIC_DOMAIN}/platform/healthz"
echo

echo "Tunnel verification passed."
echo "Private SSH from this VPS: ssh -p 12222 fractal-studio@127.0.0.1"
