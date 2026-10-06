#!/usr/bin/env bash

set -euo pipefail

download_output=""
if [[ "${1:-}" == --download-protobuf ]]; then
  [[ $# -eq 3 ]] || { echo "usage: $0 --download-protobuf <suite> <output-dir>" >&2; exit 2; }
  distribution="$2"
  download_output="$3"
else
  distribution="${1:-${PACKAGE_DISTRIBUTION:-}}"
fi
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

# Fetch a scoped provider on the host without installing it or changing host APT.
# The build containers consume only this local Deb with their network disabled.
if [[ -n "${download_output}" ]]; then
  script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  # shellcheck source=../dependencies/xgc2-protobuf.env
  source "${script_dir}/../dependencies/xgc2-protobuf.env"
  if [[ ! "${XGC2_DEPENDENCY_SET_DIGEST:-}" =~ ^[0-9a-f]{64}$ ]]; then
    echo "staging overlay requires a dependency-set digest" >&2
    exit 1
  fi
  python3 - "${overlay_url}" <<'PYURL'
import sys
from urllib.parse import urlsplit
url = urlsplit(sys.argv[1])
if url.scheme != "https" or not url.netloc or url.username or url.password or url.query or url.fragment or (url.path.rstrip("/") and not url.path.startswith("/staging/")):
    raise SystemExit("scoped protobuf requires the explicit central HTTPS APT source")
PYURL
  for command in curl gpg apt-get dpkg dpkg-deb python3; do
    command -v "${command}" >/dev/null || { echo "missing scoped fetch tool: ${command}" >&2; exit 1; }
  done
  temporary="$(mktemp -d)"
  trap 'rm -rf "${temporary}"' EXIT
  mkdir -p "${temporary}/gnupg" "${temporary}/lists/partial" "${temporary}/archives/partial" "${temporary}/debs" "${temporary}/log"
  export GNUPGHOME="${temporary}/gnupg"
  curl -fsSL "${key_url}" -o "${temporary}/keyring.gpg"
  gpg --batch --show-keys --with-colons "${temporary}/keyring.gpg" \
    | grep -q '^fpr:.*:2A8E11B36F56D307ADF626D85E5FDC30979EA43F:$'
  printf 'deb [signed-by=%s] %s %s main\n' "${temporary}/keyring.gpg" "${overlay_url}" "${distribution}" > "${temporary}/sources.list"
  cat > "${temporary}/apt.conf" <<APTCONF
Dir::Etc::sourcelist "${temporary}/sources.list";
Dir::Etc::sourceparts "-";
Dir::Etc::parts "-";
Dir::Etc::main "-";
Dir::State::status "/dev/null";
Dir::State::lists "${temporary}/lists";
Dir::Cache::archives "${temporary}/archives";
Dir::Cache::pkgcache "";
Dir::Cache::srcpkgcache "";
Dir::Log "${temporary}/log";
APT::Sandbox::User "$(id -un)";
APTCONF
  expected_version="${XGC2_PROTOBUF_STANDALONE_DEB_VERSION}~${distribution}"
  APT_CONFIG="${temporary}/apt.conf" apt-get update
  (
    cd "${temporary}/debs"
    APT_CONFIG="${temporary}/apt.conf" apt-get download "xgc2-protobuf-dev=${expected_version}"
  )
  mapfile -t debs < <(find "${temporary}/debs" -maxdepth 1 -type f -name 'xgc2-protobuf-dev_*.deb' | sort)
  [[ "${#debs[@]}" -eq 1 ]] || { echo "overlay must supply exactly one protobuf Deb" >&2; exit 1; }
  architecture="$(dpkg --print-architecture)"
  curl -fsSL "${overlay_url}/manifests/xgc2-protobuf/${distribution}/${architecture}/xgc2-protobuf-dev_${expected_version}.json" \
    -o "${temporary}/manifest.json"
  python3 - "${temporary}/manifest.json" "${debs[0]}" "${distribution}" "${architecture}" \
    "${expected_version}" "${XGC2_PROTOBUF_STANDALONE_DEB_VERSION}" \
    "${XGC2_PROTOBUF_STANDALONE_SOURCE_REF}" "${overlay_url}" <<'PYIDENTITY'
import hashlib, json, pathlib, re, subprocess, sys
from urllib.parse import urlsplit
manifest_path, deb_path, suite, target_arch, version, base_version, source, apt_source = sys.argv[1:]
manifest = json.loads(pathlib.Path(manifest_path).read_text())
expected = {"schema": "xgc2.release-artifact.v1", "product": "xgc2-protobuf", "distribution": suite, "version": base_version, "source_sha": source}
if any(manifest.get(key) != value for key, value in expected.items()) or manifest.get("architecture") not in (target_arch, "all"):
    raise SystemExit("staged protobuf manifest does not match exact provider/source/release")
stage_path = urlsplit(apt_source).path.rstrip("/")
if stage_path.startswith("/staging/") and manifest.get("release_id") != stage_path.split("/")[-1]:
    raise SystemExit("staged protobuf manifest belongs to another release")
if not re.fullmatch(r"[0-9a-f]{64}", manifest.get("release_lock_digest", "")):
    raise SystemExit("scoped protobuf manifest has no valid central lock identity")
deb = pathlib.Path(deb_path)
actual = [subprocess.check_output(["dpkg-deb", "-f", str(deb), field], text=True).strip() for field in ("Package", "Version", "Architecture")]
if actual != ["xgc2-protobuf-dev", version, "all"]:
    raise SystemExit("staged protobuf Deb has the wrong package/version/architecture")
rows = [row for row in manifest.get("debs", []) if row.get("package") == "xgc2-protobuf-dev"]
expected_deb = {"file": deb.name, "package": actual[0], "version": version, "architecture": "all", "size": deb.stat().st_size, "sha256": hashlib.sha256(deb.read_bytes()).hexdigest()}
if len(rows) != 1 or any(rows[0].get(key) != value for key, value in expected_deb.items()):
    raise SystemExit("staged protobuf Deb differs from exact manifest digest/control")
PYIDENTITY
  install -m 0644 "${debs[0]}" "${download_output}/"
  printf 'Fetched scoped Proto %s from %s at source %s; dependency set %s\n' \
    "${expected_version}" "${overlay_url}" "${XGC2_PROTOBUF_STANDALONE_SOURCE_REF}" "${XGC2_DEPENDENCY_SET_DIGEST}"
  exit 0
fi

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
