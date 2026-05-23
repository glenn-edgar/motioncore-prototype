# Vendored CMSIS-DSP (rfft_fast f32 subset, length 1024)

These files are a **copy** of a minimal subset of ARM-software/CMSIS-DSP needed
for `spectral.c`'s averaged power-spectrum path (1024-point real FFT, Hamming
window, `cmplx_mag_squared`). Do not edit them here — fix bugs upstream and
re-lift.

## Source
- Upstream: <https://github.com/ARM-software/CMSIS-DSP>
- Pin: tag **`v1.16.2`** (or a later tagged release). Record the actual SHA
  used after the first lift.
- License: Apache-2.0.
- Lift to be performed Pi-side (build host); see "Lift procedure" below.

## Pi-side lift procedure (one-time)

```bash
cd /tmp
git clone --depth 1 --branch v1.16.2 https://github.com/ARM-software/CMSIS-DSP.git
DST=~/motioncore-prototype/ra4m1/apps/register_dongle/vendor/cmsis_dsp

# headers (only the ones the listed sources reach for)
mkdir -p "$DST/Include" "$DST/Source/TransformFunctions" \
         "$DST/Source/CommonTables" "$DST/Source/BasicMathFunctions" \
         "$DST/Source/ComplexMathFunctions"

cp CMSIS-DSP/Include/arm_math.h                     "$DST/Include/"
cp CMSIS-DSP/Include/arm_math_types.h               "$DST/Include/"
cp CMSIS-DSP/Include/arm_math_memory.h              "$DST/Include/"
cp CMSIS-DSP/Include/arm_common_tables.h            "$DST/Include/"
cp CMSIS-DSP/Include/arm_const_structs.h            "$DST/Include/"
cp CMSIS-DSP/Include/arm_helium_utils.h             "$DST/Include/"
cp -r CMSIS-DSP/Include/dsp                         "$DST/Include/"

# sources (rfft 1024 path + helpers)
cp CMSIS-DSP/Source/TransformFunctions/arm_rfft_fast_init_f32.c   "$DST/Source/TransformFunctions/"
cp CMSIS-DSP/Source/TransformFunctions/arm_rfft_fast_f32.c        "$DST/Source/TransformFunctions/"
cp CMSIS-DSP/Source/TransformFunctions/arm_cfft_f32.c             "$DST/Source/TransformFunctions/"
cp CMSIS-DSP/Source/TransformFunctions/arm_cfft_init_f32.c        "$DST/Source/TransformFunctions/"
cp CMSIS-DSP/Source/TransformFunctions/arm_cfft_radix8_f32.c      "$DST/Source/TransformFunctions/"
cp CMSIS-DSP/Source/TransformFunctions/arm_bitreversal2.c         "$DST/Source/TransformFunctions/"

cp CMSIS-DSP/Source/CommonTables/arm_common_tables.c              "$DST/Source/CommonTables/"
cp CMSIS-DSP/Source/CommonTables/arm_common_tables_f16.c          "$DST/Source/CommonTables/" 2>/dev/null || true
cp CMSIS-DSP/Source/CommonTables/arm_const_structs.c              "$DST/Source/CommonTables/"

# (Optional) helper used for the per-bin magnitude (we don't call it — manual
# loop is cheaper than the function-call overhead at N/2 = 512 iterations on
# M4F. Listed in case we add it later.)
# cp CMSIS-DSP/Source/ComplexMathFunctions/arm_cmplx_mag_squared_f32.c "$DST/Source/ComplexMathFunctions/"
```

After the first lift, record the exact CMSIS-DSP tag/SHA below.

## Files (filled in after first lift)
- Tag/SHA used: _to be filled in_
- Lifted on:    _to be filled in_

## Build wiring
The Makefile in `ra4m1/apps/register_dongle/` adds:

```make
CMSIS_DSP := $(CURDIR)/vendor/cmsis_dsp
INC       += $(CMSIS_DSP)/Include
SRC_C     += arm_rfft_fast_init_f32.c arm_rfft_fast_f32.c \
             arm_cfft_f32.c arm_cfft_init_f32.c arm_cfft_radix8_f32.c \
             arm_bitreversal2.c \
             arm_common_tables.c arm_const_structs.c
vpath %.c $(CMSIS_DSP)/Source/TransformFunctions \
          $(CMSIS_DSP)/Source/CommonTables
CFLAGS    += -DARM_MATH_CM4 -DARM_MATH_DSP -D__FPU_PRESENT=1 \
             -D__FPU_USED=1 -DARM_MATH_LOOPUNROLL
```

`ARM_MATH_CM4` selects the M4 path; `ARM_MATH_DSP` enables the M4's DSP
single-cycle multiply-accumulates; `__FPU_USED=1` enables the FPU intrinsics.
These are also set when the BSP includes `bsp_api.h` (FSP already enables the
FPU at startup).

## What we use
- `arm_rfft_fast_init_f32(&inst, 1024)` — at mode entry, once.
- `arm_rfft_fast_f32(&inst, in, out, 0)` — per frame; in-place OK
  (`in == out` is supported), N/2-complex packed output.
- The CommonTables provide the precomputed twiddles + bit-reversal indices
  for length-1024.

## Slimming opportunity
Length-1024 only needs the 1024 / 512 / 256 / 128 / 64 / 32 / 16 / 8 cfft
table sets. The CommonTables `.c` files compile-gate the full table set on
`ARM_TABLE_*` macros; defining only `ARM_TABLE_TWIDDLECOEF_RFFT_F32_1024`
plus the cfft tables it pulls in would shrink the flash footprint. Not done
yet — leave the full tables in until we're chasing flash bytes.
