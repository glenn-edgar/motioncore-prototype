# Full help
luajit s_compile.lua --help

# Generate all outputs (headers + binary)
luajit s_compile.lua <input.lua> --all-bin --outdir=<dir>

# Generate only text outputs (no binary)
luajit s_compile.lua <input.lua> --all --outdir=<dir>

# Generate specific outputs
luajit s_compile.lua <input.lua> --header=<file.h>
luajit s_compile.lua <input.lua> --records=<file_records.h>
luajit s_compile.lua <input.lua> --user=<file_user_functions.h>
luajit s_compile.lua <input.lua> --reg=<file_registration.c>
luajit s_compile.lua <input.lua> --debug=<file_debug.h>
luajit s_compile.lua <input.lua> --binary=<file.bin>
luajit s_compile.lua <input.lua> --binary-h=<file_bin.h>

# With helpers
luajit s_compile.lua <input.lua> --helpers=s_engine_helpers.lua --all-bin

# Debug dump
luajit s_compile.lua <input.lua> --dump

# 64-bit mode
luajit s_compile.lua <input.lua> --64bit --all-bin

# 32-bit mode (default)
luajit s_compile.lua <input.lua> --32bit --all-bin

