#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
dependency_lock="${script_dir}/../dependencies/xgc2-protobuf.env"
if [[ ! -f "${dependency_lock}" ]]; then
  echo "missing protobuf dependency lock: ${dependency_lock}" >&2
  exit 1
fi
# shellcheck source=../dependencies/xgc2-protobuf.env
source "${dependency_lock}"

if [[ ! "${XGC2_PROTOBUF_PROTOCOL_VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "invalid protobuf protocol version: ${XGC2_PROTOBUF_PROTOCOL_VERSION}" >&2
  exit 1
fi

distribution="${1:-${PACKAGE_DISTRIBUTION:-}}"
if [[ -z "${distribution}" && -r /etc/os-release ]]; then
  # shellcheck disable=SC1091
  . /etc/os-release
  distribution="${VERSION_CODENAME:-${UBUNTU_CODENAME:-}}"
fi
case "${distribution}" in
  focal|jammy|noble) ;;
  *)
    echo "unsupported protobuf dependency distribution: ${distribution:-<empty>}" >&2
    exit 1
    ;;
esac

protocol_version_pattern="${XGC2_PROTOBUF_PROTOCOL_VERSION//./\\.}"
deb_dir="${PROTOBUF_DEB_DIR:-}"

if [[ -n "${deb_dir}" ]]; then
  mapfile -t debs < <(find "${deb_dir}" -maxdepth 1 -type f -name 'xgc2-protobuf-dev_*.deb' | sort)
  if [[ "${#debs[@]}" -ne 1 ]]; then
    echo "PROTOBUF_DEB_DIR must contain exactly one xgc2-protobuf-dev deb" >&2
    exit 1
  fi
  dpkg -i "${debs[0]}"
elif dpkg -s xgc2-protobuf-dev >/dev/null 2>&1; then
  :
else
  echo "xgc2-protobuf-dev is not installed. Fetch the sibling deb on the host and set PROTOBUF_DEB_DIR; do not apt-get in product CI." >&2
  exit 1
fi

installed_version="$(dpkg-query -W -f='${Version}' xgc2-protobuf-dev)"
if [[ ! "${installed_version}" =~ ^${protocol_version_pattern}-[0-9]+~${distribution}$ ]]; then
  echo "installed xgc2-protobuf-dev ${installed_version} is outside the ${XGC2_PROTOBUF_PROTOCOL_VERSION} protocol line for ${distribution}" >&2
  exit 1
fi
