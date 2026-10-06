#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
locked_source_ref="952ed81c7ef0a9a7650f6d0d72ac8deb4a93f453"

# shellcheck source=../dependencies/xgc2-protobuf.env
source "${repo_root}/.xgc2/dependencies/xgc2-protobuf.env"
if [[ "${XGC2_PROTOBUF_STANDALONE_SOURCE_REF}" != "${locked_source_ref}" ]]; then
  echo "protobuf fetch test source lock is stale" >&2
  exit 1
fi

temporary="$(mktemp -d)"
cleanup() {
  rm -rf "${temporary}"
}
trap cleanup EXIT

mock_bin="${temporary}/bin"
mkdir -p "${mock_bin}"

cat > "${mock_bin}/gh" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
printf '%q ' "$@" >> "${MOCK_GH_LOG}"
printf '\n' >> "${MOCK_GH_LOG}"
if [[ "${1:-}" == "run" && "${2:-}" == "list" ]]; then
  printf '32658339664\t%s\n' "${MOCK_RUN_HEAD_SHA}"
  exit 0
fi
if [[ "${1:-}" == "api" && "${2:-}" == *'/artifacts?per_page=100' ]]; then
  printf '9498077992\n'
  exit 0
fi
if [[ "${1:-}" == "api" && "${2:-}" == *'/actions/artifacts/9498077992/zip' ]]; then
  printf 'mock artifact zip'
  exit 0
fi
echo "unexpected gh invocation: $*" >&2
exit 1
MOCK

cat > "${mock_bin}/unzip" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
destination=""
while (( $# > 0 )); do
  if [[ "$1" == "-d" ]]; then
    destination="$2"
    shift 2
  else
    shift
  fi
done
test -n "${destination}"
mkdir -p "${destination}"
: > "${destination}/xgc2-protobuf-dev_0.5.0-19~focal_all.deb"
MOCK

cat > "${mock_bin}/dpkg-deb" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" != "-f" ]]; then
  exit 1
fi
if [[ $# -eq 3 ]]; then
  case "${3:-}" in
    Package) printf 'xgc2-protobuf-dev\n'; exit 0 ;;
    Version) printf '%s\n' "${MOCK_PROTO_VERSION:-0.5.0-19~focal}"; exit 0 ;;
    Architecture) printf '%s\n' "${MOCK_PROTO_ARCHITECTURE:-all}"; exit 0 ;;
  esac
fi
printf 'Package: xgc2-protobuf-dev\nVersion: 0.5.0-19~focal\nArchitecture: all\n'
MOCK

chmod +x "${mock_bin}/gh" "${mock_bin}/unzip" "${mock_bin}/dpkg-deb"
export MOCK_GH_LOG="${temporary}/gh.log"

mismatch_output="${temporary}/mismatch"
if PATH="${mock_bin}:${PATH}" MOCK_RUN_HEAD_SHA="$(printf 'd%.0s' {1..40})" \
    "${repo_root}/.xgc2/scripts/fetch_protobuf_deb.sh" focal "${mismatch_output}" \
    > "${temporary}/mismatch.stdout" 2> "${temporary}/mismatch.stderr"; then
  echo "protobuf fetch accepted a successful run from the wrong head SHA" >&2
  exit 1
fi
grep -Fq "does not match locked source ${locked_source_ref}" "${temporary}/mismatch.stderr"
if grep -Fq '/actions/runs/' "${MOCK_GH_LOG}"; then
  echo "protobuf fetch inspected artifacts before validating the run head SHA" >&2
  exit 1
fi

: > "${MOCK_GH_LOG}"
success_output="${temporary}/success"
PATH="${mock_bin}:${PATH}" MOCK_RUN_HEAD_SHA="${locked_source_ref}" \
  "${repo_root}/.xgc2/scripts/fetch_protobuf_deb.sh" focal "${success_output}" \
  > "${temporary}/success.stdout"

test -f "${success_output}/xgc2-protobuf-dev_0.5.0-19~focal_all.deb"
grep -Fq -- "--commit ${locked_source_ref}" "${MOCK_GH_LOG}"
grep -Fq -- '--event push' "${MOCK_GH_LOG}"
grep -Fq -- '--status success' "${MOCK_GH_LOG}"
grep -Fq -- '--json databaseId\,headSha' "${MOCK_GH_LOG}"
grep -Fq "run 32658339664 at ${locked_source_ref}" "${temporary}/success.stdout"

if PATH="${mock_bin}:${PATH}" MOCK_RUN_HEAD_SHA="${locked_source_ref}" MOCK_PROTO_VERSION="0.5.0-18~focal" \
    "${repo_root}/.xgc2/scripts/fetch_protobuf_deb.sh" focal "${temporary}/wrong-version" \
    > "${temporary}/wrong-version.stdout" 2> "${temporary}/wrong-version.stderr"; then
  echo "standalone fetch accepted Proto18 for the exact Proto19 source contract" >&2
  exit 1
fi
grep -Fq 'must be 0.5.0-19~focal' "${temporary}/wrong-version.stderr"

cat > "${mock_bin}/curl" <<'MOCK'
#!/usr/bin/env python3
import hashlib, json, os, pathlib, sys
args = sys.argv[1:]
url = next(arg for arg in args if arg.startswith("https://"))
output = pathlib.Path(args[args.index("-o") + 1])
with open(os.environ["MOCK_CURL_LOG"], "a") as log:
    log.write(url + "\n")
if url.endswith(".gpg"):
    output.write_text("fixture key")
else:
    assert url.startswith(os.environ.get("MOCK_APT_SOURCE", "https://overlay.example/staging/test-sdk16") + "/manifests/xgc2-protobuf/focal/amd64/")
    version = os.environ.get("MOCK_PROTO_VERSION", "0.5.0-19~focal")
    body = b"scoped artifact\n"
    digest = hashlib.sha256(body).hexdigest()
    if os.environ.get("MOCK_BAD_HASH"):
        digest = "0" * 64
    output.write_text(json.dumps({
        "schema": "xgc2.release-artifact.v1", "product": "xgc2-protobuf",
        "version": "0.5.0-19", "source_sha": os.environ.get("MOCK_PROVIDER_SOURCE", os.environ["MOCK_RUN_HEAD_SHA"]),
        "distribution": "focal", "architecture": "amd64", "release_id": os.environ.get("MOCK_RELEASE_ID", "test-sdk16"), "release_lock_digest": "c" * 64,
        "debs": [{"file": "xgc2-protobuf-dev_" + version + "_all.deb", "package": "xgc2-protobuf-dev",
                  "version": version, "architecture": "all", "size": len(body), "sha256": digest}]}))
MOCK
cat > "${mock_bin}/gpg" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
printf 'fpr:::::::::%s:\n' "${MOCK_KEY_FINGERPRINT:-2A8E11B36F56D307ADF626D85E5FDC30979EA43F}"
MOCK
cat > "${mock_bin}/dpkg" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
[[ "$*" == --print-architecture ]]
printf 'amd64\n'
MOCK
cat > "${mock_bin}/apt-get" <<'MOCK'
#!/usr/bin/env python3
import json, os, pathlib, re, sys
config = pathlib.Path(os.environ["APT_CONFIG"])
content = config.read_text()
source_path = re.search(r'Dir::Etc::sourcelist "([^"]+)";', content).group(1)
sources = pathlib.Path(source_path).read_text()
assert os.environ.get("MOCK_APT_SOURCE", "https://overlay.example/staging/test-sdk16") + " focal main" in sources
assert "xgc2.apt.xiaokang.ink" not in sources
assert 'Dir::Etc::sourceparts "-";' in content
assert 'Dir::State::status "/dev/null";' in content
assert str(config.parent) in re.search(r'Dir::State::lists "([^"]+)";', content).group(1)
with open(os.environ["MOCK_APT_LOG"], "a") as log:
    log.write(json.dumps({"args": sys.argv[1:], "config": str(config), "sources": sources}) + "\n")
if sys.argv[1:] == ["update"]:
    if os.environ.get("MOCK_OVERLAY_FAILURE"):
        raise SystemExit("fixture signed overlay unavailable")
elif sys.argv[1:] == ["download", "xgc2-protobuf-dev=0.5.0-19~focal"]:
    version = os.environ.get("MOCK_PROTO_VERSION", "0.5.0-19~focal")
    pathlib.Path("xgc2-protobuf-dev_" + version + "_all.deb").write_bytes(b"scoped artifact\n")
else:
    raise SystemExit("unexpected host APT operation: " + repr(sys.argv[1:]))
MOCK
chmod +x "${mock_bin}/curl" "${mock_bin}/gpg" "${mock_bin}/dpkg" "${mock_bin}/apt-get"
export MOCK_CURL_LOG="${temporary}/curl.log" MOCK_APT_LOG="${temporary}/apt.log"
: > "${MOCK_GH_LOG}"
: > "${MOCK_CURL_LOG}"
: > "${MOCK_APT_LOG}"
overlay="https://overlay.example/staging/test-sdk16"
digest="$(printf 'a%.0s' {1..64})"
PATH="${mock_bin}:${PATH}" MOCK_RUN_HEAD_SHA="${locked_source_ref}" \
  XGC2_APT_OVERLAY_URL="${overlay}" XGC2_DEPENDENCY_SET_DIGEST="${digest}" \
  "${repo_root}/.xgc2/scripts/fetch_protobuf_deb.sh" focal "${temporary}/scoped-success" \
  > "${temporary}/scoped-success.stdout"
test -f "${temporary}/scoped-success/xgc2-protobuf-dev_0.5.0-19~focal_all.deb"
test ! -s "${MOCK_GH_LOG}"
grep -Fq "Fetched scoped Proto 0.5.0-19~focal from ${overlay}" "${temporary}/scoped-success.stdout"
python3 - "${MOCK_APT_LOG}" <<'PYCHECK'
import json, pathlib, sys
calls = [json.loads(line) for line in pathlib.Path(sys.argv[1]).read_text().splitlines()]
assert [call["args"] for call in calls] == [["update"], ["download", "xgc2-protobuf-dev=0.5.0-19~focal"]]
assert all(not pathlib.Path(call["config"]).exists() for call in calls), "temporary APT configuration leaked"
PYCHECK

# Central explicitly supplies production APT when upstream nodes are verified,
# rather than staged. This is the selected source, never an implicit fallback.
PATH="${mock_bin}:${PATH}" MOCK_RUN_HEAD_SHA="${locked_source_ref}" \
  MOCK_APT_SOURCE="https://overlay.example" XGC2_APT_OVERLAY_URL="https://overlay.example" \
  XGC2_DEPENDENCY_SET_DIGEST="${digest}" \
  "${repo_root}/.xgc2/scripts/fetch_protobuf_deb.sh" focal "${temporary}/explicit-production" \
  > "${temporary}/explicit-production.stdout"
test -f "${temporary}/explicit-production/xgc2-protobuf-dev_0.5.0-19~focal_all.deb"
test ! -s "${MOCK_GH_LOG}"

for failure in missing-digest old-version foreign-source foreign-release bad-hash bad-key unavailable; do
  case "${failure}" in
    missing-digest) overrides=(XGC2_DEPENDENCY_SET_DIGEST=) ;;
    old-version) overrides=(MOCK_PROTO_VERSION=0.5.0-18~focal) ;;
    foreign-source) overrides=(MOCK_PROVIDER_SOURCE="$(printf 'b%.0s' {1..40})") ;;
    foreign-release) overrides=(MOCK_RELEASE_ID=another-release) ;;
    bad-hash) overrides=(MOCK_BAD_HASH=1) ;;
    bad-key) overrides=(MOCK_KEY_FINGERPRINT="$(printf '0%.0s' {1..40})") ;;
    unavailable) overrides=(MOCK_OVERLAY_FAILURE=1) ;;
  esac
  if env PATH="${mock_bin}:${PATH}" MOCK_RUN_HEAD_SHA="${locked_source_ref}" \
      XGC2_APT_OVERLAY_URL="${overlay}" XGC2_DEPENDENCY_SET_DIGEST="${digest}" \
      "${overrides[@]}" "${repo_root}/.xgc2/scripts/fetch_protobuf_deb.sh" focal "${temporary}/scoped-${failure}" \
      > "${temporary}/scoped-${failure}.stdout" 2> "${temporary}/scoped-${failure}.stderr"; then
    echo "scoped protobuf accepted ${failure}" >&2
    exit 1
  fi
  test ! -s "${MOCK_GH_LOG}"
  test -z "$(find "${temporary}/scoped-${failure}" -type f -name '*.deb' -print -quit)"
done

echo "Pinned protobuf artifact and scoped overlay fetch tests passed (12 cases)."
