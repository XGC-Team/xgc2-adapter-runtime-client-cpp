#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
temporary="$(mktemp -d)"
trap 'rm -rf "$temporary"' EXIT
fixture="${temporary}/fixture"
mock_bin="${temporary}/bin"
mkdir -p "${fixture}/.xgc2/scripts" "${fixture}/.xgc2/dependencies" "${mock_bin}"
cp "${repo_root}/.xgc2/scripts/fetch_protobuf_deb.sh" "${fixture}/.xgc2/scripts/"
cp "${repo_root}/.xgc2/dependencies/xgc2-protobuf.env" "${fixture}/.xgc2/dependencies/"
# Stub only the existing signed APT configuration boundary; never touch host APT.
cat > "${fixture}/.xgc2/scripts/configure_xgc2_apt.sh" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
[[ -z "${XGC2_APT_OVERLAY_URL+x}" && -z "${XGC2_APT_KEY_URL+x}" ]]
printf 'configure %s\n' "$1" >> "$MOCK_LOG"
exit "${MOCK_CONFIGURE_EXIT:-0}"
MOCK
cat > "${mock_bin}/curl" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
output=""; url=""
while (( $# )); do
  case "$1" in
    --output) output="$2"; shift 2 ;;
    https://*) url="$1"; shift ;;
    *) shift ;;
  esac
done
python3 - "$url" "$output" <<'JSON'
import hashlib
import json
import os
import re
import sys
from pathlib import Path
url, output = sys.argv[1:]
match = re.fullmatch(r"https://xgc2\.apt\.xiaokang\.ink/manifests/xgc2-protobuf/(focal|jammy|noble)/(amd64|arm64)/xgc2-protobuf-dev_0\.5\.0-17~\1\.json", url)
if not match:
    raise SystemExit("unexpected persistent manifest URL")
suite, arch = match.groups()
data = f"protobuf:{suite}".encode()
Path(output).write_text(json.dumps({
    "schema": "xgc2.release-artifact.v1", "product": "xgc2-protobuf",
    "source_sha": os.environ["MOCK_SOURCE_SHA"], "version": "0.5.0-17",
    "distribution": suite, "architecture": arch,
    "debs": [{"package": "xgc2-protobuf-dev", "version": f"0.5.0-17~{suite}",
              "architecture": "all", "sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}],
}))
JSON
MOCK
cat > "${mock_bin}/dpkg" <<'MOCK'
#!/usr/bin/env bash
[[ "$1" == --print-architecture ]]
printf '%s\n' "${MOCK_ARCHITECTURE:-amd64}"
MOCK
cat > "${mock_bin}/apt-get" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
[[ "$1" == download && "$2" == xgc2-protobuf-dev=0.5.0-17~* ]]
printf 'download %s\n' "$2" >> "$MOCK_LOG"
version="${2#*=}"; suite="${version#*~}"
printf 'protobuf:%s' "$suite" > "xgc2-protobuf-dev_${version}_all.deb"
if [[ "${MOCK_TAMPER:-0}" == 1 ]]; then
  printf tampered >> "xgc2-protobuf-dev_${version}_all.deb"
fi
MOCK
cat > "${mock_bin}/dpkg-deb" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
[[ "$1" == -f ]]
file="${2##*/}"; version="${file#xgc2-protobuf-dev_}"; version="${version%_all.deb}"
case "$3" in
  Package) printf 'xgc2-protobuf-dev\n' ;;
  Version) printf '%s\n' "$version" ;;
  Architecture) printf 'all\n' ;;
  *) exit 1 ;;
esac
MOCK
chmod +x "${mock_bin}"/*
export PATH="${mock_bin}:${PATH}" MOCK_LOG="${temporary}/calls.log"
export XGC2_APT_OVERLAY_URL=https://untrusted-overlay.invalid XGC2_APT_KEY_URL=https://untrusted-key.invalid
source "${repo_root}/.xgc2/dependencies/xgc2-protobuf.env"
export MOCK_SOURCE_SHA="$XGC2_PROTOBUF_STANDALONE_SOURCE_REF"
for suite in focal jammy noble; do
  : > "$MOCK_LOG"
  output="${temporary}/$suite"
  bash "${fixture}/.xgc2/scripts/fetch_protobuf_deb.sh" "$suite" "$output" > "${temporary}/$suite.stdout"
  [[ -f "$output/xgc2-protobuf-dev_0.5.0-17~${suite}_all.deb" ]]
  [[ "$(cat "$MOCK_LOG")" == "$(printf 'configure %s\ndownload xgc2-protobuf-dev=0.5.0-17~%s' "$suite" "$suite")" ]]
done
: > "$MOCK_LOG"
MOCK_ARCHITECTURE=arm64 bash "${fixture}/.xgc2/scripts/fetch_protobuf_deb.sh" focal "${temporary}/arm64" >/dev/null
: > "$MOCK_LOG"
if MOCK_SOURCE_SHA="$(printf 'd%.0s' {1..40})" bash "${fixture}/.xgc2/scripts/fetch_protobuf_deb.sh" focal "${temporary}/wrong-source" > /dev/null 2> "${temporary}/wrong-source.stderr"; then
  echo 'accepted a published package from another source SHA' >&2; exit 1
fi
grep -Fq 'source_sha does not match locked' "${temporary}/wrong-source.stderr"
[[ ! -s "$MOCK_LOG" ]]
if MOCK_TAMPER=1 bash "${fixture}/.xgc2/scripts/fetch_protobuf_deb.sh" focal "${temporary}/tampered" > /dev/null 2> "${temporary}/tampered.stderr"; then
  echo 'accepted Deb bytes that differ from the source-bound release' >&2; exit 1
fi
grep -Fq 'differs from the pinned release manifest' "${temporary}/tampered.stderr"
[[ -z "$(find "${temporary}/tampered" -type f -print -quit)" ]]
: > "$MOCK_LOG"
if MOCK_CONFIGURE_EXIT=17 bash "${fixture}/.xgc2/scripts/fetch_protobuf_deb.sh" focal "${temporary}/unsigned" > /dev/null 2>&1; then
  echo 'downloaded despite failed signing/index configuration' >&2; exit 1
fi
[[ "$(cat "$MOCK_LOG")" == 'configure focal' ]]
echo 'Pinned persistent protobuf fetch tests passed (focal/jammy/noble/all and source/hash/signing failures).'
