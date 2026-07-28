#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
  echo "Run this installer as root: sudo $0" >&2
  exit 1
fi

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
FRP_VERSION=${FRP_VERSION:-0.69.0}
PUBLIC_DOMAIN=${PUBLIC_DOMAIN:-fractal.kevin0412.top}

case "$(uname -m)" in
  x86_64)
    FRP_ARCH=amd64
    FRP_SHA256=6b90d1cd28fc661f170c0de90dde03d2c63e4fd7ce0ae2da2ca1c28014b8146e
    ;;
  aarch64|arm64)
    FRP_ARCH=arm64
    FRP_SHA256=24a4fc82b4c041835103419685ea124c4d6a7dbf83d0425481c5831b4ce4b3a4
    ;;
  *)
    echo "Unsupported architecture: $(uname -m)" >&2
    exit 1
    ;;
esac

if command -v apt-get >/dev/null 2>&1; then
  PACKAGE_FAMILY=apt
  apt-get update
  apt-get install -y --no-install-recommends \
    ca-certificates curl openssl tar \
    debian-keyring debian-archive-keyring gpg apt-transport-https
elif command -v dnf >/dev/null 2>&1; then
  PACKAGE_FAMILY=dnf
  dnf install -y ca-certificates curl openssl tar dnf-plugins-core
else
  echo "This installer requires apt-get or dnf" >&2
  exit 1
fi

INSTALL_TMP=$(mktemp -d /tmp/frps-install.XXXXXX)
trap 'rm -rf -- "$INSTALL_TMP"' EXIT
FRP_ARCHIVE="$INSTALL_TMP/frp.tar.gz"

if [[ -n ${FRP_ARCHIVE_FILE:-} ]]; then
  install -m 0644 "$FRP_ARCHIVE_FILE" "$FRP_ARCHIVE"
else
  curl -fL \
    "https://github.com/fatedier/frp/releases/download/v${FRP_VERSION}/frp_${FRP_VERSION}_linux_${FRP_ARCH}.tar.gz" \
    -o "$FRP_ARCHIVE"
fi
printf '%s  %s\n' "$FRP_SHA256" "$FRP_ARCHIVE" | sha256sum -c -
tar -xzf "$FRP_ARCHIVE" -C "$INSTALL_TMP"
install -m 0755 \
  "$INSTALL_TMP/frp_${FRP_VERSION}_linux_${FRP_ARCH}/frps" \
  /usr/local/bin/frps

if ! getent group frp >/dev/null; then
  groupadd --system frp
fi
if ! id -u frp >/dev/null 2>&1; then
  useradd --system --gid frp --home-dir /nonexistent --shell /usr/sbin/nologin frp
fi
install -d -m 0750 -o root -g frp /etc/frp

if [[ ! -s /etc/frp/frps.token ]]; then
  openssl rand -hex 32 > /etc/frp/frps.token
fi
chown root:root /etc/frp/frps.token
chmod 0600 /etc/frp/frps.token
FRP_TOKEN=$(< /etc/frp/frps.token)

cat > /etc/frp/frps.toml <<EOF
bindAddr = "0.0.0.0"
bindPort = 7000
proxyBindAddr = "127.0.0.1"

auth.method = "token"
auth.token = "${FRP_TOKEN}"

transport.tls.force = true

allowPorts = [
  { single = 12222 },
  { single = 13010 },
  { single = 18110 },
  { single = 19020 }
]

log.to = "console"
log.level = "info"
log.maxDays = 7
EOF
chown root:frp /etc/frp/frps.toml
chmod 0640 /etc/frp/frps.toml
install -m 0644 "$SCRIPT_DIR/frps.service" /etc/systemd/system/frps.service

/usr/local/bin/frps verify -c /etc/frp/frps.toml
systemctl daemon-reload
systemctl enable --now frps

if ! command -v caddy >/dev/null 2>&1; then
  if [[ ${PACKAGE_FAMILY} == apt ]]; then
    curl -1sLf https://dl.cloudsmith.io/public/caddy/stable/gpg.key \
      | gpg --dearmor \
      | tee /usr/share/keyrings/caddy-stable-archive-keyring.gpg >/dev/null
    curl -1sLf https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt \
      | tee /etc/apt/sources.list.d/caddy-stable.list >/dev/null
    chmod o+r /usr/share/keyrings/caddy-stable-archive-keyring.gpg
    chmod o+r /etc/apt/sources.list.d/caddy-stable.list
    apt-get update
    apt-get install -y caddy
  else
    if [[ -r /etc/os-release ]] && grep -q '^ID="\?alinux"\?$' /etc/os-release; then
      dnf copr enable -y @caddy/caddy "epel-8-$(uname -m)"
    else
      dnf copr enable -y @caddy/caddy
    fi
    dnf install -y caddy
  fi
fi

if [[ -f /etc/caddy/Caddyfile ]]; then
  cp -a /etc/caddy/Caddyfile "/etc/caddy/Caddyfile.backup.$(date +%Y%m%d-%H%M%S)"
fi
sed "s/fractal\.kevin0412\.top/${PUBLIC_DOMAIN}/g" "$SCRIPT_DIR/Caddyfile" \
  > /etc/caddy/Caddyfile
caddy fmt --overwrite /etc/caddy/Caddyfile
caddy validate --config /etc/caddy/Caddyfile
systemctl enable --now caddy
systemctl reload caddy

echo
echo "VPS installation complete."
echo "FRP token (transfer through your existing SSH session):"
cat /etc/frp/frps.token
echo
echo "Cloud ingress required: VPS SSH, TCP 80, TCP 443, TCP 7000."
echo "Do not expose TCP 12222, 13010, 18110, or 19020."
systemctl --no-pager --full status frps caddy || true
