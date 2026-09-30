#!/bin/sh
# Downloads Tom Harte's SingleStepTests for the 6502 and the WDC 65C02 (≈ 2 GB)
# into ${HARTE_65X02_DIR:-~/.cache/65x02} (sparse clone: 6502/v1, wdc65c02/v1),
# for the cpu_harte_* CTest tests (skipped when absent).
set -e
DIR="${HARTE_65X02_DIR:-$HOME/.cache/65x02}"
if [ -d "$DIR/.git" ]; then
    git -C "$DIR" pull -q
else
    git clone -q --depth 1 --filter=blob:none --sparse https://github.com/SingleStepTests/65x02 "$DIR"
    git -C "$DIR" sparse-checkout set wdc65c02/v1 6502/v1
fi
du -sh "$DIR/wdc65c02/v1" "$DIR/6502/v1"
