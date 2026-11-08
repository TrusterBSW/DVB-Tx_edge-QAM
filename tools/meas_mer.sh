#!/bin/bash
# meas_mer.sh LNA VGA CENTRE_MHZ TAG [FILTRE_HZ] : capture HackRF centree sur un multiplex -> TEI + profil de MER
# (HackRF sur la sortie TX1A, par cable ; fichiers temporaires dans ./rx)
T=$(cd "$(dirname "$0")" && pwd)
mkdir -p rx
lna=$1 vga=$2 c=$3 tag=$4 bw=${5:-9000000}
rm -f rx/$tag.cs8
timeout 25 hackrf_transfer -r rx/$tag.cs8 -f ${c}000000 -s 18285714 -n 9142857 -b $bw -l $lna -g $vga -a 0 >/dev/null 2>&1
[ -s rx/$tag.cs8 ] || { echo "$tag: capture ratee"; exit 1; }
python3 -c "
import numpy as np
x=np.fromfile('rx/$tag.cs8',dtype=np.int8).astype(np.int16)
p=np.mean(x.astype(float)**2)*2/128**2
print('== $tag ${c} MHz lna $lna vga $vga bw $bw: rms/voie', round(float(np.sqrt(np.mean(x.astype(float)**2))),1), 'LSB, %.1f dBFS' % (10*np.log10(p)), ', ecretage', int(np.sum(np.abs(x)>=127)))
(x*64).astype(np.int16).tofile('rx/$tag.cs16')"
rm -f rx/$tag.cs8 rx/$tag.ts
nice python3 $T/mux_extract.py rx/$tag.cs16 2 0 rx/${tag}_m.cs16 0 >/dev/null; rm -f rx/$tag.cs16
timeout 300 nice python3 $T/gr_dvbt_rx.py rx/${tag}_m.cs16 rx/$tag.ts 2>/dev/null >/dev/null
python3 $T/ts_check.py rx/$tag.ts | cut -c1-60
nice python3 $T/dvbt_rf_check.py rx/${tag}_m.cs16 --groups 12 2>&1 | tail -14
rm -f rx/${tag}_m.cs16
