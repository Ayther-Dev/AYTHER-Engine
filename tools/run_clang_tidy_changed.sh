#!/usr/bin/env bash
# Runs clang-tidy over the C/C++ translation units a change actually touches.
# The check list, the blocking subset, and the header filter all live in the
# repository's .clang-tidy, so CI, an IDE, and a local run agree by default.
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <base-revision> <build-directory>" >&2
    exit 2
fi

base_revision=$1
build_directory=$2
head_revision=${GITHUB_SHA:-HEAD}
clang_tidy=${CLANG_TIDY:-clang-tidy}
line_filter=$(python3 tools/clang_tidy_line_filter.py \
    "${base_revision}" "${head_revision}")

if [[ ! -f "${build_directory}/compile_commands.json" ]]; then
    echo "No compile_commands.json in ${build_directory}." >&2
    exit 1
fi

mapfile -t changed_sources < <(
    git diff --name-only --diff-filter=ACMR "${base_revision}" "${head_revision}" -- \
        '*.c' '*.cc' '*.cpp' '*.cxx' |
        while IFS= read -r source; do
            [[ -f "${source}" ]] && printf '%s\n' "${source}"
        done
)

if [[ ${#changed_sources[@]} -eq 0 ]]; then
    echo "No modified C/C++ translation units require clang-tidy."
    exit 0
fi

"${clang_tidy}" --version

status=0
for source in "${changed_sources[@]}"; do
    # A source outside the build's compilation database has no flags to lint
    # with; skipping it beats guessing at them and reporting invented findings.
    if ! grep -qF "\"$(basename "${source}")\"" \
        "${build_directory}/compile_commands.json" 2>/dev/null &&
       ! grep -qF "${source}" "${build_directory}/compile_commands.json"; then
        echo "clang-tidy: skipping ${source} (not in the compilation database)"
        continue
    fi
    echo "clang-tidy: ${source}"
    "${clang_tidy}" -p "${build_directory}" \
        --line-filter="${line_filter}" "${source}" || status=1
done

exit "${status}"
