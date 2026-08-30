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

mapfile -t profiles < <(find "${profile_directory}" -type f -name '*.profraw' -print)
if [[ ${#profiles[@]} -eq 0 ]]; then
    echo "No LLVM coverage profiles were produced." >&2
    exit 1
fi

llvm-profdata merge -sparse "${profiles[@]}" -o "${output_directory}/cpp.profdata"

mapfile -t binaries < <(find "${build_directory}/bin" -maxdepth 1 -type f -executable -print)
if [[ ${#binaries[@]} -eq 0 ]]; then
    echo "No instrumented native executables were found." >&2
    exit 1
fi

objects=()
for binary in "${binaries[@]:1}"; do
    objects+=(--object "${binary}")
done

ignore='(third_party|vcpkg_installed|_deps|tests|tools)/'
llvm-cov export "${binaries[0]}" "${objects[@]}" \
    --instr-profile="${output_directory}/cpp.profdata" \
    --format=lcov \
    --ignore-filename-regex="${ignore}" \
    > "${output_directory}/cpp.lcov"

llvm-cov report "${binaries[0]}" "${objects[@]}" \
    --instr-profile="${output_directory}/cpp.profdata" \
    --ignore-filename-regex="${ignore}" \
    | tee "${output_directory}/summary.txt"
