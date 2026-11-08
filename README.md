<h1 align="center">DVB-Tx_edge-QAM</h1>

<p align="center">
  <b>Four DVB-T multiplexes from one small SDR board.</b><br>
  A complete DVB-T modulator in the FPGA of a HamGeek Fishball: send MPEG-TS over the network, get TV on the coax.
</p>

<p align="center">
  <a href="README.fr.md">Version française</a> ·
  <a href="#install">Install</a> ·
  <a href="#quick-start">Quick start</a> ·
  <a href="#measurements">Measurements</a> ·
  <a href="#building-from-source">Building</a>
</p>

<p align="center">
  <img alt="license" src="https://img.shields.io/badge/license-GPL--3.0--or--later-blue">
  <img alt="board" src="https://img.shields.io/badge/board-HamGeek%20Fishball%207010-informational">
  <img alt="fpga" src="https://img.shields.io/badge/FPGA-Zynq%20XC7Z010%20%7C%20AD9363-orange">
  <img alt="standard" src="https://img.shields.io/badge/DVB--T-4%20%C3%97%2031.7%20Mbit%2Fs-success">
</p>

---

DVB-Tx_edge-QAM turns a HamGeek Fishball (Zynq XC7Z010 + AD9363, gigabit Ethernet) into a small
**edgeQAM**: the PC only sends transport streams over UDP, and the board does everything else in its
FPGA, from energy dispersal to the OFDM symbols, then transmits up to four adjacent DVB-T channels.
No IQ crosses the network and the ARM cores stay mostly idle.

- **Four multiplexes at once**: 4 × 31.668 Mbit/s = 126.7 Mbit/s of TS on four adjacent 8 MHz
  channels (474, 482, 490 and 498 MHz by default), from one AD9363 output.
- **All in the FPGA**: PRBS, RS(204,188), Forney interleaver, 7/8 convolutional code, bit and
  symbol interleavers, 64-QAM, continual and scattered pilots, TPS, 8192-point IFFT and guard
  interval. One coding chain is time-shared between the four multiplexes.
- **Bit-exact**: the HLS core produces the same symbols as the C engine of
  [dvbtx](https://github.com/TrusterBSW/DVB-Tx), itself checked against GNU Radio gr-dtv.
- **One UDP port per multiplex** (1001 to 1004), RTP accepted, multicast too. A multiplex with no
  stream stays on air, filled with null packets.
- **Clocked by the board**: the modulator runs on the AD9363 crystal and the network side follows
  it. [dvbtx](https://github.com/TrusterBSW/DVB-Tx) ≥ 0.3.0 drives it natively (radio `driver = "edgeqam"`),
  steering its rate on the board's buffers.
- **Small HTTP API and mDNS**: status in JSON, frequency, level and multiplexes changed live; the
  board announces itself as `_dvbmod._udp`.
- **Ready to flash**: one zip for the microSD card, settings in a text file on the card.

> [!WARNING]
> **Closed coax only.** Transmitting on broadcast frequencies is illegal. This image is meant for a
> cable between the board and a TV or a private coaxial network, with attenuators.

## Contents

- [Install](#install)
- [Quick start](#quick-start)
- [How it works](#how-it-works)
- [Configuration](#configuration)
- [HTTP API and mDNS](#http-api-and-mdns)
- [With dvbtx](#with-dvbtx)
- [Measurements](#measurements)
- [Limits and notes](#limits-and-notes)
- [Building from source](#building-from-source)
- [Credits](#credits)
- [License](#license)

## Install

You need a **HamGeek Fishball 7010** (Zynq XC7Z010, AD9363 in LVDS, SD boot). Do not use this image
on a PlutoSDR, a Pluto+ or a LibreSDR: their FPGA and wiring differ.

1. Format a microSD card in FAT32 (one partition).
2. Unzip [`releases/DVB-Tx_edge-QAM-fishball7010-sd.zip`](releases/) at the root of the card
   (check it first: `sha256sum -c DVB-Tx_edge-QAM-fishball7010-sd.zip.sha256`).
3. Edit `edgeqam.conf` on the card if needed (frequency, level, inputs), insert it and power on.

The board takes its address by DHCP on Ethernet. SSH: `root` / `analog`. To go back to the
original firmware, put the previous card content back.

## Quick start

Any MPEG-TS sender works, as long as each stream is **constant bit rate at 31 668 449 bit/s**
(null packets included). With [TSDuck](https://tsduck.io) and a file muxed at that rate:

```bash
tsp -I file --infinite channel1.ts -P regulate -O ip 192.168.1.50:1001      # multiplex 1
tsp -I file --infinite channel2.ts -P regulate -O ip 192.168.1.50:1002      # multiplex 2
```

A test multiplex with ffmpeg:

```bash
ffmpeg -re -f lavfi -i testsrc2=size=1280x720:rate=25 -f lavfi -i sine \
  -c:v libx264 -b:v 6M -maxrate 6M -bufsize 3M -c:a mp2 \
  -f mpegts -muxrate 31668449 -metadata service_name="Test" udp://192.168.1.50:1001?pkt_size=1316
```

Then scan with a TV or a DVB-T stick on the cable (474–498 MHz). The board's state:

```bash
curl http://192.168.1.50:8080/status
```

## How it works

```
 PC                               Fishball 7010
 TS (UDP 1001..1004) ──Ethernet──▶ dvbt_tx (ARM) ──TX DMA──▶ FPGA: dvbt_bridge ─▶ dvbt_mod (HLS, 100 MHz)
                                    ▲  HTTP API, mDNS             │                  PRBS, RS, Forney, 7/8,
                                    │                             │                  interleavers, 64-QAM,
                                    └── buffer level, counters ◀──┘                  pilots, TPS, IFFT 8192, GI
                                                                                          │ 36.57 MS/s
                                                                     AD9363 TX1A ◀── DAC ◀┘  (LO = centre)
```

- **dvbt_tx** (ARM) receives one UDP stream per multiplex, interleaves their packets in the standard
  TX DMA buffer of the AD9363 driver, and inserts a null packet when a stream has nothing ready.
- **dvbt_bridge** (Verilog) diverts that DMA stream to the modulator and feeds the DAC with its
  output, in place of the usual IQ path.
- **dvbt_mod** (Vitis HLS) codes the four multiplexes with one shared chain and places them on the
  bins of a single 8192-point IFFT at 4 × 64/7 MS/s. A phase rotation of the IFFT input moves the
  guard interval to the front of the output, so no symbol buffer is needed.
- The whole design replaces the receive/display side of the original firmware: no Maia, no web UI.
  The radio's other uses are untouched by the modulator when it is disabled.

## Configuration

`edgeqam.conf` at the root of the card, read at boot:

| Key | Default | Meaning |
|---|---|---|
| `ENABLE` | `1` | start the modulator at boot |
| `NMUX` | `4` | number of multiplexes (adjacent 8 MHz channels) |
| `FREQ` | `474000000` | centre of **multiplex 1** in Hz; multiplex k is at FREQ + (k−1) × 8 MHz |
| `ATT` | `30` | TX attenuation in dB (0 = full power, 89.75 = off) |
| `DISABLE` | `0` | multiplexes taken off the air (bit mask: 1 = mux 1, 2 = mux 2, 4 = mux 3, 8 = mux 4) |
| `INPUT0`…`INPUT3` | `udp:1001`…`udp:1004` | `udp:PORT`, `udp:GROUP:PORT` (multicast) or `file:/path.ts` |
| `HTTP_PORT` | `8080` | port of the HTTP API |

Fixed in the FPGA: DVB-T 2K, 64-QAM, code rate 7/8, guard interval 1/32, non-hierarchical, 8 MHz,
cell id 0 signalled.

## HTTP API and mDNS

- `GET /status`: JSON with the uptime, frequencies, attenuation, sample rate, symbols per second,
  FPGA counters (null packets, DAC underflows, IFFT overflows, sync errors), temperatures, and per
  multiplex the packets received, nulls inserted, packets dropped and the buffer level.
- `GET /set?freq=HZ&att=DB&disable=MASK&save=1`: applied live (a few ms); `save=1` writes the values
  back to `edgeqam.conf`. `freq` is the centre of multiplex 1. The reply gives the applied values.
- mDNS: service `_dvbmod._udp` named `edgeQAM <host>`, TXT `std=dvbt mode=2k qam=64 cr=7/8 gi=1/32
  bw=8 rate=31668449 nmux ports freq1 disable api` (`avahi-browse -rpt _dvbmod._udp`).

## With dvbtx

[dvbtx](https://github.com/TrusterBSW/DVB-Tx) multiplexes live streams, cameras and files into
complete DVB multiplexes (PSI/SI, programme guide, PCRs restamped) and drives this board as one of
its radios. It is the recommended source: the board itself does no multiplexing, no PCR correction
and no rate adaptation.

**Compatibility**

| | |
|---|---|
| dvbtx version | **≥ 0.3.0 (upcoming)**: the `edgeqam` radio driver is not in dvbtx 0.2.0 |
| Standard | DVB-T 2K, 64-QAM, 7/8, GI 1/32, 8 MHz only; dvbtx refuses other carrier parameters on this radio |
| Multiplexes | 1 to 4 per board, on adjacent 8 MHz channels (multiplex k = FREQ + (k−1) × 8 MHz) |
| Discovery | mDNS `_dvbmod._udp` (`address = "auto"`) or a fixed IP address |
| Clock | dvbtx follows the board's crystal (buffer level read 4 times a second), PCRs stamped for it |
| Settings | dvbtx sets the frequency and disables the unused multiplexes (`tune = true`, default); the TX level stays the board's own setting |
| Documentation | [dvbtx README, *An edgeQAM board*](https://github.com/TrusterBSW/DVB-Tx#an-edgeqam-board-fpga-modulator-on-the-network) and `man 5 dvbtx.toml`, section *edgeQAM boards* |

```toml
[[radio]]
name = "fishball"
driver = "edgeqam"
address = "auto"          # found by mDNS; or the board's IP, e.g. "192.168.1.50"

[[carrier]]
name = "Mux 1"
radio = "fishball"
channel = 21              # 474 MHz = the board's multiplex 1; channel 22 would be its multiplex 2
  [[carrier.service]]
  name = "News"
  source = "https://example.org/live.m3u8"
```

Options of the radio: `tune = false` (dvbtx leaves the board's frequency and multiplexes alone),
`buffer_ms` (buffer target on the board, 100 ms by default; more over a less steady network),
`center = <Hz>` on a carrier instead of `channel`. A complete four-multiplex example is
`configs/edgeqam-4mux.toml` in dvbtx.

Without dvbtx, any sender of constant bit rate MPEG-TS works (see [Quick start](#quick-start)), but
then the stream's rate must match the board's crystal, otherwise the board inserts null packets or
drops packets, and the PCRs are not corrected.

## Measurements

HamGeek Fishball 7010, mode 1R1T, copper heatsink without fan.

| Test | Result |
|---|---|
| HLS core against the dvbtx C engine (C simulation, 150 symbols, 2 and 4 multiplexes) | bit-exact, 66–69 dB (16-bit quantisation) |
| Bitstream (4 multiplexes, Vivado 2023.1) | timing met, 72 % LUT, 58 % BRAM, 54 % DSP of the XC7Z010 |
| Rate on the board | 4 329 OFDM symbols/s, 4 × 31.668 Mbit/s, 0 DAC underflow |
| AD9363 digital loopback, software demodulation (gr-dtv) of each multiplex | MER 52 dB, 0 uncorrectable packet, TS identical to the source |
| 4 UDP streams at full rate for 90 s (wired) | 1 895 054 packets per multiplex sent and received, none lost |
| RF on the coax, HackRF One centred on each multiplex, gr-dtv | 4 multiplexes, 0 uncorrectable packet; MER 22–23 dB, flat across the channel (limit of the 8-bit HackRF) |
| Stream from dvbtx, RF reception | service decoded (H.264 720p + AAC, EPG), PCRs within 1 ms, rate locked to the board within 2 ppm |
| CPU (2 × Cortex-A9) | dvbt_tx about 57 % of one core for 4 UDP streams |
| Temperature | Zynq 70 °C steady with 4 multiplexes (original firmware idle: 77 °C) |

## Limits and notes

- **DVB-T only**, parameters fixed (DVB-T2 does not fit in the XC7Z010). Adjacent channels only.
- At boot the AD9363 starts at 89.75 dB of attenuation (devicetree), then `ATT` is applied.
- Tested in 1R1T mode only (forced in `uEnv.txt`).
- The board's LO sits between multiplexes 2 and 3 and leaks a small carrier at that frequency; it did
  not affect reception.
- Send the streams over a wired link: on Wi-Fi, stalls of a few hundred ms empty the buffers and
  the board fills the gap with null packets.
- The test receivers were a HackRF One with gr-dtv and the dvbtx monitoring; a common RTL2832U
  stick did not lock in our tests and was not investigated further.

## Building from source

Tools: Vivado and Vitis HLS 2023.1, an `arm-linux-gnueabihf` cross compiler, GNU Radio 3.10 with
gr-dtv for the checks.

1. **HLS core** (`src/hls/v3`): `NMUX=4 STEP=csim vitis_hls -f run_hls.tcl` checks it against a
   dvbtx reference made with `src/gen/dvbt_multi_ref.c`; `STEP=export` packages the IP. The ROM
   tables come from `src/gen/gen_tables.c`.
2. **Bitstream**: the F5OEO fork of [Maia SDR](https://github.com/F5OEO/maia-sdr), branch `refactor`
   (fbc289b), with the XC7Z010 pin fix; copy `src/hdl/*` into `maia-hdl/projects/pluto/`,
   `src/hdl/project/*` into `maia-hdl/projects/fishball_edgeqam4/`, the exported IP into
   `maia-hdl/ip/edgeqam/`, then `make` in the project. `bootgen -process_bitstream bin` gives
   `system_top.bin`.
3. **dvbt_tx** (`src/sw`): `arm-linux-gnueabihf-gcc -O2 -o dvbt_tx dvbt_tx.c -liio -lpthread`
   against libiio 0.26.
4. **Ramdisk**: the Tezuka ramdisk with a cpio appended holding `/usr/sbin/dvbt_tx` and
   `/etc/init.d/S99edgeqam` (`src/rootfs`), rewrapped with `mkimage -T ramdisk -C none`.

`tools/` holds the checks used for the measurements: `dvbt_check.py` (MER, pilots, TPS of a loopback
capture), `dvbt_rf_check.py` (MER per carrier of a real RF capture), `gr_dvbt_rx.py` (software
receiver to TS), `mux_extract.py`, `ts_check.py`, `ts_udp_send.c` (constant-rate UDP sender) and
`meas_mer.sh`.

## Credits

- [Tezuka](https://github.com/F5OEO/tezuka_fw) (Evariste F5OEO): firmware, kernel, ramdisk and the
  board support for the Fishball; boot environment by Christos Nikolaou SV1EIA.
- [Maia SDR](https://maia-sdr.org) (Daniel Estévez) and its F5OEO fork: the FPGA project the
  bitstream is built from.
- [Analog Devices HDL](https://github.com/analogdevicesinc/hdl): AD9361 core, DMA and FIFOs.
- [dvbtx](https://github.com/TrusterBSW/DVB-Tx): the DVB-T engine the FPGA core is ported from.

## License

GPL-3.0-or-later for the code of this repository. The bitstream and the image also contain parts
under their own licences (Analog Devices HDL, Maia SDR, Tezuka, Linux). © 2026 BILLY Rémi.
