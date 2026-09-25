#!/usr/bin/env bash
set -euo pipefail

version="${1:?usage: scripts/publish-release.sh <version> [release notes]}"
notes="${2:-OrbitLan ${version}}"
tag="v${version}"
zip_path="${ORBITLAN_ZIP_PATH:-dist/OrbitLan.zip}"
manifest_path="${ORBITLAN_MANIFEST_PATH:-dist/manifest.json}"
private_key="${ORBITLAN_SIGNING_KEY:?set ORBITLAN_SIGNING_KEY to the private PEM path}"
public_key="${ORBITLAN_SIGNING_PUBLIC_KEY:?set ORBITLAN_SIGNING_PUBLIC_KEY to the public PEM path}"
download_url="https://github.com/furqan-ahm/orbitlan/releases/download/${tag}/OrbitLan.zip"

test -f "$zip_path"
test -f "$private_key"
test -f "$public_key"

sha256=$(sha256sum "$zip_path" | awk '{print $1}')
payload=$(printf '%s\n%s\n%s' "$version" "$download_url" "$sha256")
signature=$(printf '%s' "$payload" | openssl dgst -sha256 -sign "$private_key" | base64 -w0)

signature_file=$(mktemp)
trap 'rm -f "$signature_file"' EXIT
printf '%s' "$signature" | base64 -d > "$signature_file"
printf '%s' "$payload" | openssl dgst -sha256 -verify "$public_key" -signature "$signature_file" >/dev/null

mkdir -p "$(dirname "$manifest_path")"
node -e '
  const fs = require("fs");
  const [path, version, url, sha256, notes, sig] = process.argv.slice(1);
  fs.writeFileSync(path, JSON.stringify({ version, url, sha256, notes, sig }, null, 2) + "\n");
' "$manifest_path" "$version" "$download_url" "$sha256" "$notes" "$signature"

gh release create "$tag" \
  "$zip_path#OrbitLan.zip" \
  "$manifest_path#manifest.json" \
  --title "OrbitLan ${version}" \
  --notes "$notes" \
  --target main

printf 'Published %s\n' "$tag"
