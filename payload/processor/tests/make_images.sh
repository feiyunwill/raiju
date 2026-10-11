#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
	echo "Usage: $0 <output_dir>" >&2
	exit 1
fi

OUT="$1"
SMP_COMMIT=3530d8d88ea1fa0d907545eda7ac95d4240a0213
WORK="$(mktemp -d)"
GAME="$WORK/game"

mkdir -p "$GAME/sce_sys" "$OUT"
printf '\177ELF' > "$GAME/eboot.bin"
printf '{"titleId":"PPSA24003","localizedParameters":{"en-US":{"titleName":"Raiju"}}}' > "$GAME/sce_sys/param.json"
i=0
while [ "$i" -lt 300 ]; do
	printf 'x' > "$GAME/sce_sys/filler_with_a_rather_long_file_name_$i.bin"
	i=$((i + 1))
done

curl -fsSL "https://raw.githubusercontent.com/drakmor/ShadowMountPlus/$SMP_COMMIT/mkexfat.sh" -o "$WORK/mkexfat.sh"
sudo sh "$WORK/mkexfat.sh" "$GAME" "$OUT/smp.exfat" > /dev/null
makefs -t ffs -o version=2,bsize=65536,fsize=65536,minfree=0 -s 64m "$OUT/smp.ffpkg" "$GAME" > /dev/null

sudo rm -rf "$WORK"
echo "$OUT/smp.exfat:$OUT/smp.ffpkg"
