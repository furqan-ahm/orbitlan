#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
  echo "Run this installer as root: sudo ./install.sh" >&2
  exit 1
fi

SOURCE_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ ! -x "${SOURCE_DIR}/orbitlan" ]]; then
  echo "The orbitlan binary is missing from ${SOURCE_DIR}." >&2
  exit 1
fi

install -D -m 0755 "${SOURCE_DIR}/orbitlan" /usr/local/bin/orbitlan
install -d -m 0700 /etc/orbitlan /var/lib/orbitlan
if [[ ! -f /etc/orbitlan/orbitlan.conf ]]; then
  install -m 0600 "${SOURCE_DIR}/orbitlan.conf.example" /etc/orbitlan/orbitlan.conf
fi
install -m 0644 "${SOURCE_DIR}/orbitlan.service" /etc/systemd/system/orbitlan.service
systemctl daemon-reload

echo "OrbitLan is installed but has not been started."
echo "1. Edit /etc/orbitlan/orbitlan.conf and set ORBITLAN_CODE."
echo "2. Run: sudo systemctl enable --now orbitlan"
echo "3. Check: sudo systemctl status orbitlan"
