#!/usr/bin/env bash
# Checks clang-format on every changed C/C++ source and runs clang-tidy over
# every changed translation unit that belongs to the configured build.
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <base-revision> <build-directory>" >&2
    exit 2
fi

base_revision=$1
build_directory=$2
head_revision=${GITHUB_SHA:-HEAD}
clang_tidy=${CLANG_TIDY:-clang-tidy}
clang_format=${CLANG_FORMAT:-clang-format}
line_filter=$(python3 tools/clang_tidy_line_filter.py \
    "${base_revision}" "${head_revision}")

if [[ ! -f "${build_directory}/compile_commands.json" ]]; then
    echo "No compile_commands.json in ${build_directory}." >&2
    exit 1
fi

mapfile -t changed_format_sources < <(
    git diff --name-only --diff-filter=ACMR "${base_revision}" "${head_revision}" -- \
        '*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hh' '*.hpp' '*.hxx' |
        while IFS= read -r source; do
            [[ -f "${source}" ]] && printf '%s\n' "${source}"
        done
)

if [[ ${#changed_format_sources[@]} -gt 0 ]]; then
    "${clang_format}" --version
    "${clang_format}" --dry-run --Werror "${changed_format_sources[@]}"
else
    echo "No modified C/C++ sources require clang-format."
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
    # Standalone reference experiments have their own strict builds and are not
    # members of the native target graph represented by this database.
    if ! grep -qF "\"$(basename "${source}")\"" \
        "${build_directory}/compile_commands.json" 2>/dev/null &&
       ! grep -qF "${source}" "${build_directory}/compile_commands.json"; then
        echo "clang-tidy: ${source} is not a configured native target"
        continue
    fi
    echo "clang-tidy: ${source}"
    "${clang_tidy}" -p "${build_directory}" \
        --line-filter="${line_filter}" "${source}" || status=1
done

exit "${status}"
