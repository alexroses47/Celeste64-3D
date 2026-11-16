#!/bin/zsh
# Convierte los assets (si se pide) y compila celeste64.3dsx con el romfs actualizado.
#   ./build.sh            solo compila
#   ./build.sh convert    reconvierte todo y compila
#   ./build.sh cia        compila y además arma celeste64.cia (para instalar con FBI)
set -e
cd "${0:A:h}"
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
export PATH=/opt/devkitpro/tools/bin:$PATH

if [[ "$1" == "convert" ]]; then
	rm -rf romfs
	tools/.venv/bin/python -I tools/convert.py ext/Celeste64/Content romfs
	tools/.venv/bin/python -I tools/audio.py ext/Celeste64/Content romfs
fi

# make no sigue los cambios del romfs: forzar el reempaquetado
rm -f celeste64.3dsx
make

if [[ "$1" == "cia" ]]; then
	MAKEROM=ext/ctrtools/Project_CTR/makerom/bin/makerom
	BANNERTOOL=ext/ctrtools/3ds-bannertool/build/bannertool
	for t in $MAKEROM $BANNERTOOL; do
		[[ -x $t ]] || { echo "falta $t (ver README)"; exit 1; }
	done
	mkdir -p build/cia
	$BANNERTOOL makebanner -i meta/banner.png -a meta/banner.wav -o build/cia/banner.bnr
	$BANNERTOOL makesmdh -s "Celeste 64 3DS" -l "Unofficial fan port of Celeste 64" \
		-p "Not made or endorsed by the Celeste team" -i meta/icon.png -o build/cia/icon.icn
	$MAKEROM -f cia -o celeste64.cia -target t -exefslogo -elf celeste64.elf -rsf meta/app.rsf \
		-icon build/cia/icon.icn -banner build/cia/banner.bnr -DAPP_ROMFS=romfs -major 1 -minor 2 -micro 0
	ls -lh celeste64.cia
fi
