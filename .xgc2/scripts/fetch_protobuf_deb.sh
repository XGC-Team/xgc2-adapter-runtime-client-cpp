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
  *)
    echo "unsupported protobuf distribution: ${distribution}" >&2
    exit 2
    ;;
esac

# shellcheck source=../dependencies/xgc2-protobuf.env
source "${repo_root}/.xgc2/dependencies/xgc2-protobuf.env"
locked_source_ref="${XGC2_PROTOBUF_STANDALONE_SOURCE_REF:-}"
if [[ ! "${locked_source_ref}" =~ ^[0-9a-f]{40}$ ]]; then
  echo "protobuf standalone source lock must be a full SHA" >&2
  exit 1
fi

mkdir -p "${output_dir}"
if find "${output_dir}" -mindepth 1 -print -quit | grep -q .; then
  echo "protobuf output directory must be empty: ${output_dir}" >&2
  exit 1
fi

if [[ -n "${XGC2_APT_OVERLAY_URL:-}" ]]; then
  "${repo_root}/.xgc2/scripts/configure_xgc2_apt.sh" \
    --download-protobuf "${distribution}" "${output_dir}"
  exit 0
fi

for command in gh unzip dpkg-deb; do
  command -v "${command}" >/dev/null || {
    echo "missing required artifact tool: ${command}" >&2
    exit 1
  }
done

repository="XGC-Team/xgc2-protobuf"
artifact_name="xgc2-protobuf-${distribution}-all"
run_metadata="$(
  gh run list \
    --repo "${repository}" \
    --workflow ci.yml \
    --branch master \
    --commit "${locked_source_ref}" \
    --event push \
    --status success \
    --limit 1 \
    --json databaseId,headSha \
    --jq '.[0] | [.databaseId, .headSha] | @tsv'
)"
IFS=$'\t' read -r run_id run_head_sha <<< "${run_metadata}"
if [[ ! "${run_id:-}" =~ ^[0-9]+$ || ! "${run_head_sha:-}" =~ ^[0-9a-f]{40}$ ]]; then
  echo "no successful protobuf push CI run found for locked source ${locked_source_ref}" >&2
  exit 1
fi
if [[ "${run_head_sha}" != "${locked_source_ref}" ]]; then
  echo "protobuf run ${run_id} head SHA ${run_head_sha} does not match locked source ${locked_source_ref}" >&2
  exit 1
fi

artifact_id="$(
  gh api \
    "repos/${repository}/actions/runs/${run_id}/artifacts?per_page=100" \
    --jq ".artifacts[] | select(.name == \"${artifact_name}\" and (.expired | not)) | .id"
)"
if [[ ! "${artifact_id}" =~ ^[0-9]+$ ]]; then
  echo "run ${run_id} has no live ${artifact_name} artifact" >&2
  exit 1
fi

temporary="$(mktemp -d)"
cleanup() {
  rm -rf "${temporary}"
}
trap cleanup EXIT

gh api "repos/${repository}/actions/artifacts/${artifact_id}/zip" \
  > "${temporary}/artifact.zip"
unzip -q "${temporary}/artifact.zip" -d "${temporary}/artifact"
mapfile -t protobuf_debs < <(
  find "${temporary}/artifact" -type f \
    -name 'xgc2-protobuf-dev_*.deb' -print | sort
)
if [[ "${#protobuf_debs[@]}" -ne 1 ]]; then
  echo "${artifact_name} must contain exactly one protobuf Deb; found ${#protobuf_debs[@]}" >&2
  exit 1
fi
if [[ "$(dpkg-deb -f "${protobuf_debs[0]}" Package)" != "xgc2-protobuf-dev" ]]; then
  echo "protobuf artifact has the wrong Debian package identity" >&2
  exit 1
fi

expected_version="${XGC2_PROTOBUF_STANDALONE_DEB_VERSION}~${distribution}"
if [[ "$(dpkg-deb -f "${protobuf_debs[0]}" Version)" != "${expected_version}" ||
      "$(dpkg-deb -f "${protobuf_debs[0]}" Architecture)" != all ]]; then
  echo "protobuf artifact must be ${expected_version} (Architecture all)" >&2
  exit 1
fi
install -m 0644 "${protobuf_debs[0]}" "${output_dir}/"
echo "Fetched ${artifact_name} from successful protobuf run ${run_id} at ${run_head_sha}:"
dpkg-deb -f "${output_dir}/$(basename "${protobuf_debs[0]}")" \
  Package Version Architecture
