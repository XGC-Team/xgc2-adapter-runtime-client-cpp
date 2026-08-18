#!/usr/bin/env bash

set -euo pipefail

distribution="${1:-${PACKAGE_DISTRIBUTION:-}}"
if [[ -z "${distribution}" && -r /etc/os-release ]]; then
  # shellcheck disable=SC1091
  . /etc/os-release
  distribution="${VERSION_CODENAME:-${UBUNTU_CODENAME:-}}"
fi
case "${distribution}" in
  focal|jammy|noble) ;;
  *)
    echo "unsupported XGC2 APT distribution: ${distribution:-<empty>}" >&2
    exit 1
    ;;
esac

if [[ "${EUID}" -eq 0 ]]; then
  sudo_cmd=()
else
  sudo_cmd=(sudo)
fi

production_url="https://xgc2.apt.xiaokang.ink"
overlay_url="${XGC2_APT_OVERLAY_URL:-}"
overlay_url="${overlay_url%/}"
key_url="${XGC2_APT_KEY_URL:-https://xgc2.apt.xiaokang.ink/xgc2-archive-keyring.gpg}"

"${sudo_cmd[@]}" apt-get update
for command in curl gpg update-ca-certificates; do
  if ! command -v "${command}" >/dev/null; then
    echo "XGC2 build image is missing APT setup tool: ${command}" >&2
    exit 1
  fi
done
curl -fsSL "${key_url}" -o /tmp/xgc2-archive-keyring.gpg
gpg --show-keys --with-fingerprint --with-colons \
  /tmp/xgc2-archive-keyring.gpg 2>&1 \
  | grep -q '^fpr:.*:2A8E11B36F56D307ADF626D85E5FDC30979EA43F:$'
"${sudo_cmd[@]}" install -d -m 0755 /etc/apt/keyrings
"${sudo_cmd[@]}" install -m 0644 /tmp/xgc2-archive-keyring.gpg \
  /etc/apt/keyrings/xgc2-archive-keyring.gpg
echo "deb [signed-by=/etc/apt/keyrings/xgc2-archive-keyring.gpg] ${production_url} ${distribution} main" \
  | "${sudo_cmd[@]}" tee /etc/apt/sources.list.d/xgc2.list >/dev/null
if [[ -n "${overlay_url}" ]]; then
  echo "deb [signed-by=/etc/apt/keyrings/xgc2-archive-keyring.gpg] ${overlay_url} ${distribution} main" \
    | "${sudo_cmd[@]}" tee /etc/apt/sources.list.d/00-xgc2-release-train.list >/dev/null
fi
"${sudo_cmd[@]}" apt-get update
