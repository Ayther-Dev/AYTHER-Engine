#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <build-directory> <output-directory>" >&2
    exit 2
fi

build_directory=$1
output_directory=$2
profile_directory="${build_directory}/coverage-profiles"
mkdir -p "${output_directory}"

find_llvm_tool() {
    local requested=$1
    local fallback=$2
    if [[ -n "${requested}" ]]; then
        command -v "${requested}"
        return
    fi
    if command -v "${fallback}" >/dev/null 2>&1; then
        command -v "${fallback}"
        return
    fi
    command -v "${fallback}-${LLVM_TOOLS_VERSION:-18}"
}

llvm_profdata=$(find_llvm_tool "${LLVM_PROFDATA:-}" llvm-profdata)
llvm_cov=$(find_llvm_tool "${LLVM_COV:-}" llvm-cov)

mapfile -t profiles < <(find "${profile_directory}" -type f -name '*.profraw' -print)
if [[ ${#profiles[@]} -eq 0 ]]; then
    echo "No LLVM coverage profiles were produced." >&2
    exit 1
fi

"${llvm_profdata}" merge -sparse "${profiles[@]}" -o "${output_directory}/cpp.profdata"

mapfile -t binaries < <(find "${build_directory}/bin" -maxdepth 1 -type f -executable -print)
if [[ ${#binaries[@]} -eq 0 ]]; then
    echo "No instrumented native executables were found." >&2
    exit 1
fi

objects=()
for binary in "${binaries[@]:1}"; do
    objects+=(--object "${binary}")
done

# Keep every first-party file in the denominator. Only vendored/generated
# sources are omitted; see docs/COVERAGE.md for the audited exclusion list.
# Both separators: llvm-cov reports native paths, so a '/'-only pattern silently
# excludes nothing on Windows and the vendored tree lands in the denominator.
ignore='(^|[/\])(third_party|vcpkg_installed|_deps|CMakeFiles)[/\]'
"${llvm_cov}" export "${binaries[0]}" "${objects[@]}" \
    --instr-profile="${output_directory}/cpp.profdata" \
    --format=lcov \
    --ignore-filename-regex="${ignore}" \
    > "${output_directory}/cpp.lcov"

"${llvm_cov}" report "${binaries[0]}" "${objects[@]}" \
    --instr-profile="${output_directory}/cpp.profdata" \
    --ignore-filename-regex="${ignore}" \
    | tee "${output_directory}/summary.txt"

"${llvm_cov}" show "${binaries[0]}" "${objects[@]}" \
    --instr-profile="${output_directory}/cpp.profdata" \
    --format=html \
    --output-dir="${output_directory}/html" \
    --show-directory-coverage \
    --ignore-filename-regex="${ignore}"
