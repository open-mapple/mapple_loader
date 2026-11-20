#!/bin/bash
# The Qualcomm shim as a raw arm64 Image: build.sh BOARD [compiler arguments] -> out/BOARD/Image.
# BOARD=sweet (Redmi Note 10 Pro) or BOARD=sunfish (Pixel 4a). mka packs it with the payload into the boot image
# ABL starts (device.conf's bootimg_* keys); the stock DTB comes from the phone's own boot.img there.
# CHECK_ABS=path/to/check-abs.py checks that nothing needs an absolute address (ABL puts the image anywhere).
# clang, not ios-sweet's aarch64-linux-gnu-gcc (none on the Mac): -fno-jump-tables keeps its switch lookup tables, which
# hold absolute addresses, out.
set -euo pipefail
cd "$(dirname "$0")"
BOARD=${1:?usage: build.sh sweet|sunfish [compiler arguments]}
shift
CFLAGS="--target=aarch64-none-elf -O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-jump-tables -Wall -Wextra $*"
case $BOARD in
sweet) ;;
sunfish) CFLAGS="$CFLAGS -DBOARD_SUNFISH" ;;
*) echo "unknown BOARD=$BOARD" >&2; exit 1 ;;
esac
LLD=${LLD:-ld.lld}
O=out/$BOARD
mkdir -p "$O"
OBJS=()
for f in start.S shim.c console.c mmu.c fdt.c loader.c panel.c ufs.c strcmp.c; do
	o=$O/$(basename "${f%.*}").o
	clang $CFLAGS -c "$f" -o "$o"
	OBJS+=("$o")
done
# The image runs wherever ABL puts it with no relocation pass: only the header's image_size (start.o, linked at 0)
# may be an absolute address.
[ -z "${CHECK_ABS:-}" ] || python3 "$CHECK_ABS" "${OBJS[@]:1}"
$LLD -T shim.ld --build-id=none -N "${OBJS[@]}" -o "$O/shim.elf"
$LLD -T shim.ld --build-id=none -N "${OBJS[@]}" --oformat=binary -o "$O/Image"
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$(python3 -c "import struct; print(struct.unpack_from('<Q', open('$O/Image', 'rb').read(64), 16)[0])")
python3 -c "f = open('$O/Image', 'r+b'); f.truncate($size)"
ls -l "$O/Image"
