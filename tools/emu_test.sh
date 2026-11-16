#!/bin/zsh
# Prueba automática en Azahar: escribe el guion, espera N capturas, cierra el emulador
# (siempre, con -9: Azahar ignora SIGTERM) y arma una cuadrícula con las capturas.
#   tools/emu_test.sh <salida.png> "<línea1>" "<línea2>" ...
#   líneas: "spawn", "cp N", "bench", "x y z yaw pitch", "player"
set -u
OUT=$1; shift
SD="$HOME/Library/Application Support/Azahar/sdmc"
ROOT="${0:A:h:h}"
print -l -- "$@" > "$SD/c64_auto.txt"
rm -f "$SD"/c64_shot_*.bmp(N) "$SD/c64_log.txt"
N=$#
LAST="$SD/c64_shot_$((N - 1)).bmp"

~/Applications/Azahar.app/Contents/MacOS/azahar "${C64_GAME:-$ROOT/celeste64.3dsx}" > /dev/null 2>&1 &
P=$!
for i in {1..150}; do
	[[ -f "$LAST" ]] && break
	grep -q "FIN" "$SD/c64_log.txt" 2>/dev/null && break
	/bin/sleep 1
done
/bin/sleep 1
kill -9 $P 2>/dev/null
wait $P 2>/dev/null
pgrep -f "MacOS/azahar" > /dev/null && echo "AVISO: sigue abierto un Azahar" || echo "(emulador cerrado)"

cat "$SD/c64_log.txt"
"$ROOT/tools/.venv/bin/python" -I - "$OUT" "$SD" <<'EOF'
import sys, glob
from PIL import Image
out, sd = sys.argv[1], sys.argv[2]
fs = sorted(glob.glob(sd + '/c64_shot_*.bmp'), key=lambda f: int(f.rsplit('_', 1)[1][:-4]))
if fs:
    cols = 2 if len(fs) > 1 else 1
    rows = (len(fs) + cols - 1) // cols
    g = Image.new('RGB', (400 * cols, 240 * rows))
    for i, f in enumerate(fs):
        g.paste(Image.open(f), ((i % cols) * 400, (i // cols) * 240))
    g.save(out)
    print(f'{len(fs)} capturas -> {out}')
EOF
