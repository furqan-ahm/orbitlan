#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
OUTPUT="${ROOT}/dist/linux"
VERSION=${VERSION:-1.0.1}
EDITION=${EDITION:-community}

case "${EDITION}" in
  community)
    package_prefix="OrbitLan"
    ;;
  supporter)
    package_prefix="OrbitLan-Supporter"
    ;;
  *)
    echo "EDITION must be community or supporter." >&2
    exit 2
    ;;
esac

OUTPUT="${OUTPUT}/${EDITION}"

command -v go >/dev/null 2>&1 || {
  echo "Go 1.26 or newer is required." >&2
  exit 1
}

mkdir -p "${OUTPUT}"

for arch in amd64 arm64; do
  package="${package_prefix}-Linux-${arch}-v${VERSION}"
  stage=$(mktemp -d)
  package_dir="${stage}/${package}"
  mkdir -p "${package_dir}"

  (
    cd "${ROOT}/client/engine"
    CGO_ENABLED=0 GOOS=linux GOARCH="${arch}" \
      go build -trimpath -ldflags "-s -w -X main.defaultEdition=${EDITION}" \
      -o "${package_dir}/orbitlan" .
  )
  cp "${ROOT}/client/linux/README.md" "${package_dir}/"
  cp "${ROOT}/client/linux/install.sh" "${package_dir}/"
  cp "${ROOT}/client/linux/orbitlan.conf.example" "${package_dir}/"
  cp "${ROOT}/client/linux/orbitlan.service" "${package_dir}/"
  chmod 0755 "${package_dir}/orbitlan" "${package_dir}/install.sh"
  chmod 0644 "${package_dir}/README.md" "${package_dir}/orbitlan.conf.example" \
    "${package_dir}/orbitlan.service"
  sed -i "s/^ORBITLAN_EDITION=.*/ORBITLAN_EDITION=${EDITION}/" \
    "${package_dir}/orbitlan.conf.example"
  printf 'This package defaults to the OrbitLan %s edition.\n' "${EDITION}" \
    > "${package_dir}/EDITION.txt"

  tar -C "${stage}" -czf "${OUTPUT}/${package}.tar.gz" "${package}"
  (
    cd "${OUTPUT}"
    sha256sum "${package}.tar.gz" > "${package}.tar.gz.sha256"
  )
  find "${stage}" -depth -delete
done

echo "Linux ${EDITION} packages written to ${OUTPUT}"
