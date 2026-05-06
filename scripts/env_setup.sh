#!/usr/bin/env bash
# scripts/env_setup.sh — derive toolchain paths from this script's location.
#
# Source this from the project root: `source ./scripts/env_setup.sh`.
# Works regardless of where the repo is cloned (no hardcoded /home/atman).
#
# Override the toolchain root with TOOLCHAIN_PREFIX=/path/to/toolchain
# before sourcing if you keep the cross-compiler outside the source tree.

# Resolve the repo root from this script's location.
_grahaos_script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_grahaos_repo_root="$(cd "${_grahaos_script_dir}/.." && pwd)"

export PREFIX="${TOOLCHAIN_PREFIX:-${_grahaos_repo_root}/toolchain}"
export TARGET="x86_64-elf"
export PATH="${PREFIX}/bin:${PATH}"

# Regenerate Meson cross-files from .ini.in templates so the absolute
# toolchain path matches the resolved PREFIX. Templates use ${PREFIX} as
# the placeholder; envsubst (gettext) does the substitution.
if command -v envsubst >/dev/null 2>&1; then
    for tmpl in "${_grahaos_script_dir}"/cross-x86_64-elf*.ini.in; do
        [ -f "$tmpl" ] || continue
        out="${tmpl%.in}"
        PREFIX="$PREFIX" envsubst '${PREFIX}' < "$tmpl" > "$out"
    done
fi

unset _grahaos_script_dir _grahaos_repo_root
