#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <focal|jammy|noble> <output-dir>" >&2
  exit 2
fi

distribution="$1"
output_dir="$2"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
case "${distribution}" in
  focal|jammy|noble) ;;
  *) echo "unsupported protobuf distribution: ${distribution}" >&2; exit 2 ;;
esac

for command in apt-get curl dpkg dpkg-deb python3; do
  command -v "${command}" >/dev/null || {
    echo "missing required dependency tool: ${command}" >&2
    exit 1
  }
done

# shellcheck source=../dependencies/xgc2-protobuf.env
source "${repo_root}/.xgc2/dependencies/xgc2-protobuf.env"
locked_source_ref="${XGC2_PROTOBUF_STANDALONE_SOURCE_REF:-}"
package_version="${XGC2_PROTOBUF_STANDALONE_PACKAGE_VERSION:-}"
protocol_pattern="${XGC2_PROTOBUF_PROTOCOL_VERSION//./\\.}"
if [[ ! "${locked_source_ref}" =~ ^[0-9a-f]{40}$ ||
      ! "${package_version}" =~ ^${protocol_pattern}-[0-9]+$ ]]; then
  echo "protobuf dependency requires an exact source SHA and protocol package version" >&2
  exit 1
fi
version="${package_version}~${distribution}"
architecture="$(dpkg --print-architecture)"
base_url="https://xgc2.apt.xiaokang.ink"

mkdir -p "${output_dir}"
if find "${output_dir}" -mindepth 1 -print -quit | grep -q .; then
  echo "protobuf output directory must be empty: ${output_dir}" >&2
  exit 1
fi

temporary="$(mktemp -d)"
cleanup() { rm -rf "${temporary}"; }
trap cleanup EXIT
mkdir -p "${temporary}/download"
manifest="${temporary}/release-manifest.json"
curl --fail --location --silent --show-error --proto '=https' --tlsv1.2 \
  "${base_url}/manifests/xgc2-protobuf/${distribution}/${architecture}/xgc2-protobuf-dev_${version}.json" \
  --output "${manifest}"
manifest_bytes="$(python3 - "${manifest}" "${locked_source_ref}" "${package_version}" "${distribution}" "${architecture}" <<'VERIFY'
import json
import re
import sys
from pathlib import Path
path, source, version, distribution, architecture = sys.argv[1:]
document = json.loads(Path(path).read_text())
expected = {
    "schema": "xgc2.release-artifact.v1", "product": "xgc2-protobuf",
    "source_sha": source, "version": version,
    "distribution": distribution, "architecture": architecture,
}
for field, value in expected.items():
    if document.get(field) != value:
        raise SystemExit(f"protobuf release manifest {field} does not match locked {value}")
debs = document.get("debs", [])
if len(debs) != 1:
    raise SystemExit("protobuf release manifest must contain exactly one Deb")
deb = debs[0]
if (deb.get("package") != "xgc2-protobuf-dev" or
        deb.get("version") != f"{version}~{distribution}" or
        deb.get("architecture") != "all" or
        not re.fullmatch(r"[0-9a-f]{64}", deb.get("sha256", "")) or
        not isinstance(deb.get("size"), int) or deb["size"] <= 0):
    raise SystemExit("protobuf release manifest contains an invalid Deb identity")
print(deb["sha256"], deb["size"])
VERIFY
)"
read -r expected_sha expected_size <<< "${manifest_bytes}"

# This entry is used on ephemeral CI hosts or private build containers. Keep
# signing/index configuration at its existing owner; never install dependencies
# or configure APT on a developer host through this script.
env -u XGC2_APT_OVERLAY_URL -u XGC2_APT_KEY_URL \
  bash "${repo_root}/.xgc2/scripts/configure_xgc2_apt.sh" "${distribution}"
(cd "${temporary}/download"; apt-get download "xgc2-protobuf-dev=${version}")
mapfile -t debs < <(find "${temporary}/download" -maxdepth 1 -type f -name 'xgc2-protobuf-dev_*.deb')
if [[ ${#debs[@]} -ne 1 ]]; then
  echo "signed APT download must produce exactly one protobuf Deb" >&2
  exit 1
fi
actual_sha="$(sha256sum "${debs[0]}" | cut -d' ' -f1)"
if [[ "${actual_sha}" != "${expected_sha}" ||
      "$(stat -c %s "${debs[0]}")" != "${expected_size}" ||
      "$(dpkg-deb -f "${debs[0]}" Package)" != "xgc2-protobuf-dev" ||
      "$(dpkg-deb -f "${debs[0]}" Version)" != "${version}" ||
      "$(dpkg-deb -f "${debs[0]}" Architecture)" != "all" ]]; then
  echo "signed protobuf Deb differs from the pinned release manifest" >&2
  exit 1
fi
install -m 0644 "${debs[0]}" "${output_dir}/"
echo "Fetched published xgc2-protobuf-dev ${version} at locked source ${locked_source_ref}"
