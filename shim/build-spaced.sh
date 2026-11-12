#!/bin/bash
# The shim for the realme 8i, as a raw arm64 Image for mtkloader (XNU_SHIM=1) to start: out/spaced/Image.
# The boot-args record (bootrec.c) is read only from the partition mp_ufsroot= names, never from userdata.
# CHECK_ABS=path/to/viva-check-abs.py checks that nothing needs an absolute address (the shim runs where it is put).
set -euo pipefail
cd "$(dirname "$0")"
CFLAGS="--target=aarch64-none-elf -O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra -DMT6781_SHIM -DSPACED"
LLD=${LLD:-ld.lld}
mkdir -p out/spaced
OBJS=()
for f in start.S mt6781.c console.c fdt.c holes.c loader.c bootrec.c; do
	o=out/spaced/${f%.*}.o
	clang $CFLAGS -c $f -o $o
	OBJS+=($o)
done
[ -z "${CHECK_ABS:-}" ] || python3 "$CHECK_ABS" out/spaced/mt6781.o out/spaced/console.o out/spaced/fdt.o out/spaced/holes.o out/spaced/loader.o out/spaced/bootrec.o
$LLD -T shim.ld --build-id=none -N "${OBJS[@]}" -o out/spaced/shim.elf
$LLD -T shim.ld --build-id=none -N "${OBJS[@]}" --oformat=binary -o out/spaced/Image
size=$(python3 -c "import struct,sys; d=open('out/spaced/Image','rb').read(64); print(struct.unpack_from('<Q', d, 16)[0])")
python3 -c "f=open('out/spaced/Image','r+b'); f.truncate($size)"
ls -l out/spaced/Image
