#!/bin/bash
#============================================================================
# s_build.sh - S-Expression DSL build script
#
# Usage: ./s_build.sh <entry_point.lua> <output_dir>
#
# Example:
#   ./s_build.sh dsl_tests/complex_sequence/complex_sequence.lua dsl_tests/complex_sequence/
#
# DSL helper modules and s_compile.lua are expected in lua_dsl/ relative to this script.
#============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DSL_LIB_DIR="${SCRIPT_DIR}/lua_dsl"

if [ $# -lt 2 ]; then
    echo "Usage: $0 <entry_point.lua> <output_dir>"
    exit 1
fi

ENTRY_POINT="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
OUTPUT_DIR="$(mkdir -p "$2" && cd "$2" && pwd)"

if [ ! -f "${ENTRY_POINT}" ]; then
    echo "Error: '${ENTRY_POINT}' not found"
    exit 1
fi

cd "${DSL_LIB_DIR}"
luajit s_compile.lua "${ENTRY_POINT}" --helpers=s_engine_helpers.lua --all-bin --outdir="${OUTPUT_DIR}"