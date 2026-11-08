<h1 align="center">DVB-Tx_edge-QAM</h1>

<p align="center">
  <b>Quatre multiplex DVB-T sortis d'une petite carte SDR.</b><br>
  Un modulateur DVB-T complet dans le FPGA d'un HamGeek Fishball : envoyez du MPEG-TS sur le réseau, la télé sort sur le coaxial.
</p>

<p align="center">
  <a href="README.md">English version</a> ·
  <a href="#installation">Installation</a> ·
  <a href="#démarrage-rapide">Démarrage rapide</a> ·
  <a href="#mesures">Mesures</a> ·
  <a href="#compilation">Compilation</a>
</p>

<p align="center">
  <img alt="licence" src="https://img.shields.io/badge/licence-GPL--3.0--or--later-blue">
  <img alt="carte" src="https://img.shields.io/badge/carte-HamGeek%20Fishball%207010-informational">
  <img alt="fpga" src="https://img.shields.io/badge/FPGA-Zynq%20XC7Z010%20%7C%20AD9363-orange">
  <img alt="norme" src="https://img.shields.io/badge/DVB--T-4%20%C3%97%2031%2C7%20Mbit%2Fs-success">
</p>

---

DVB-Tx_edge-QAM transforme un HamGeek Fishball (Zynq XC7Z010 + AD9363, Ethernet gigabit) en petit
**edgeQAM** : le PC n'envoie que des flux de transport en UDP, et la carte fait tout le reste dans son
FPGA, de la dispersion d'énergie aux symboles OFDM, puis émet jusqu'à quatre canaux DVB-T adjacents.
Aucun IQ ne passe sur le réseau et les cœurs ARM restent presque inoccupés.

- **Quatre multiplex à la fois** : 4 × 31,668 Mbit/s = 126,7 Mbit/s de TS sur quatre canaux de
  8 MHz adjacents (474, 482, 490 et 498 MHz par défaut), sur une seule sortie de l'AD9363.
- **Tout dans le FPGA** : PRBS, RS(204,188), entrelaceur de Forney, code convolutif 7/8,
  entrelaceurs bit et symbole, 64-QAM, pilotes continus et dispersés, TPS, IFFT de 8192 points et
  intervalle de garde. Une seule chaîne de codage est partagée dans le temps par les quatre multiplex.
- **Identique bit à bit** : le cœur HLS produit les mêmes symboles que le moteur C de
  [dvbtx](https://github.com/TrusterBSW/DVB-Tx), lui-même vérifié contre GNU Radio gr-dtv.
- **Un port UDP par multiplex** (1001 à 1004), RTP accepté, multicast aussi. Un multiplex sans flux
  reste à l'antenne, rempli de paquets nuls.
- **Cadencé par la carte** : le modulateur tourne sur le quartz de l'AD9363 et le côté réseau s'y
  cale. [dvbtx](https://github.com/TrusterBSW/DVB-Tx) ≥ 0.3.0 le pilote directement (radio
  `driver = "edgeqam"`) en asservissant son débit sur les tampons de la carte.
- **Petite API HTTP et mDNS** : état en JSON, fréquence, niveau et multiplex changés en direct ; la
  carte s'annonce en `_dvbmod._udp`.
- **Prêt à flasher** : un zip pour la carte microSD, réglages dans un fichier texte sur la carte.

> [!WARNING]
> **Câble fermé uniquement.** Émettre sur les fréquences de radiodiffusion est interdit. Cette image
> est faite pour un câble entre la carte et un téléviseur ou un réseau coaxial privé, avec des
> atténuateurs.

## Sommaire

- [Installation](#installation)
- [Démarrage rapide](#démarrage-rapide)
- [Fonctionnement](#fonctionnement)
- [Configuration](#configuration)
- [API HTTP et mDNS](#api-http-et-mdns)
- [Avec dvbtx](#avec-dvbtx)
- [Mesures](#mesures)
- [Limites et remarques](#limites-et-remarques)
- [Et le DVB-T2 ?](#et-le-dvb-t2-)
- [Compilation](#compilation)
- [Remerciements](#remerciements)
- [Licence](#licence)

## Installation

Il faut un **HamGeek Fishball 7010** (Zynq XC7Z010, AD9363 en LVDS, démarrage sur carte SD). Ne pas
utiliser cette image sur un PlutoSDR, un Pluto+ ou un LibreSDR : leur FPGA et leur câblage diffèrent.

1. Formater une carte microSD en FAT32 (une seule partition).
2. Décompresser [`releases/DVB-Tx_edge-QAM-fishball7010-sd.zip`](releases/) à la racine de la carte
   (vérifier d'abord : `sha256sum -c DVB-Tx_edge-QAM-fishball7010-sd.zip.sha256`).
3. Modifier si besoin `edgeqam.conf` sur la carte (fréquence, niveau, entrées), l'insérer, alimenter.

La carte prend son adresse en DHCP sur l'Ethernet. SSH : `root` / `analog`. Pour revenir au
firmware d'origine, remettre le contenu précédent de la carte.

## Démarrage rapide

N'importe quel émetteur MPEG-TS convient, pourvu que chaque flux soit **à débit constant de
31 668 449 bit/s** (paquets nuls compris). Avec [TSDuck](https://tsduck.io) et un fichier multiplexé à
ce débit :

```bash
tsp -I file --infinite chaine1.ts -P regulate -O ip 192.168.1.50:1001      # multiplex 1
tsp -I file --infinite chaine2.ts -P regulate -O ip 192.168.1.50:1002      # multiplex 2
```

Un multiplex de test avec ffmpeg :

```bash
ffmpeg -re -f lavfi -i testsrc2=size=1280x720:rate=25 -f lavfi -i sine \
  -c:v libx264 -b:v 6M -maxrate 6M -bufsize 3M -c:a mp2 \
  -f mpegts -muxrate 31668449 -metadata service_name="Test" udp://192.168.1.50:1001?pkt_size=1316
```

Lancer ensuite une recherche de chaînes sur un téléviseur ou une clé DVB-T reliée par le câble
(474–498 MHz). L'état de la carte :

```bash
curl http://192.168.1.50:8080/status
```

## Fonctionnement

```
 PC                               Fishball 7010
 TS (UDP 1001..1004) ──Ethernet──▶ dvbt_tx (ARM) ──DMA TX──▶ FPGA : dvbt_bridge ─▶ dvbt_mod (HLS, 100 MHz)
                                    ▲  API HTTP, mDNS             │                   PRBS, RS, Forney, 7/8,
                                    │                             │                   entrelaceurs, 64-QAM,
                                    └── tampons, compteurs ◀──────┘                   pilotes, TPS, IFFT 8192, GI
                                                                                           │ 36,57 MS/s
                                                                      AD9363 TX1A ◀── DAC ◀┘  (LO au centre)
```

- **dvbt_tx** (ARM) reçoit un flux UDP par multiplex, entrelace leurs paquets dans le tampon DMA TX
  standard du pilote AD9363, et insère un paquet nul quand un flux n'a rien de prêt.
- **dvbt_bridge** (Verilog) détourne ce flux DMA vers le modulateur et alimente le DAC avec sa
  sortie, à la place du chemin IQ habituel.
- **dvbt_mod** (Vitis HLS) code les quatre multiplex avec une seule chaîne partagée et les place sur
  les bins d'une IFFT unique de 8192 points à 4 × 64/7 MS/s. Une rotation de phase de l'entrée de
  l'IFFT ramène l'intervalle de garde en tête de la sortie : pas besoin de tampon de symbole.
- Le design remplace la partie réception/affichage du firmware d'origine : ni Maia, ni interface web.
  Quand le modulateur est désactivé, le chemin IQ normal de la radio fonctionne comme avant.

## Configuration

`edgeqam.conf` à la racine de la carte, lu au démarrage :

| Clé | Défaut | Rôle |
|---|---|---|
| `ENABLE` | `1` | lancer le modulateur au démarrage |
| `NMUX` | `4` | nombre de multiplex (canaux de 8 MHz adjacents) |
| `FREQ` | `474000000` | centre du **multiplex 1** en Hz ; le multiplex k est à FREQ + (k−1) × 8 MHz |
| `ATT` | `30` | atténuation TX en dB (0 = pleine puissance, 89,75 = coupé) |
| `DISABLE` | `0` | multiplex retirés de l'antenne (masque : 1 = mux 1, 2 = mux 2, 4 = mux 3, 8 = mux 4) |
| `INPUT0`…`INPUT3` | `udp:1001`…`udp:1004` | `udp:PORT`, `udp:GROUPE:PORT` (multicast) ou `file:/chemin.ts` |
| `HTTP_PORT` | `8080` | port de l'API HTTP |
| `NAME` | vide | nom du service mDNS (vide = `edgeQAM <hôte>`) ; à régler s'il y a plusieurs cartes |

Figé dans le FPGA : DVB-T 2K, 64-QAM, rendement 7/8, intervalle de garde 1/32, non hiérarchique,
8 MHz, cell id 0 signalé.

## API HTTP et mDNS

- `GET /status` : JSON avec la durée de fonctionnement, les fréquences, l'atténuation, la cadence, les
  symboles par seconde, les compteurs du FPGA (paquets nuls, sous-débits DAC, débordements de l'IFFT,
  erreurs de synchro), les températures et, par multiplex, les paquets reçus, les nuls insérés, les
  paquets jetés et le remplissage du tampon.
- `GET /set?freq=HZ&att=DB&disable=MASQUE&save=1` : appliqué en direct (quelques ms) ; `save=1`
  réécrit les valeurs dans `edgeqam.conf`. `freq` est le centre du multiplex 1. La réponse donne les
  valeurs appliquées.
- mDNS : service `_dvbmod._udp` nommé `edgeQAM <hôte>` (ou `NAME` de `edgeqam.conf`), TXT
  `std=dvbt mode=2k qam=64 cr=7/8 gi=1/32 bw=8 rate=31668449 nmux ports freq1 disable api id`
  (`avahi-browse -rpt _dvbmod._udp`). `id` est l'adresse MAC Ethernet de la carte, unique et stable.
- **Plusieurs cartes** : toutes les cartes Tezuka s'appellent `plutoplus`, donc avahi renomme la
  seconde (`edgeQAM plutoplus #2`, `plutoplus-2.local`) dans un ordre qui dépend de celle qui démarre
  en premier. Donner à chaque carte son propre `NAME` dans `edgeqam.conf`, et choisir une carte par
  `NAME` ou `id` (ou utiliser des adresses IP fixes). Dans dvbtx : `serial = "<id>"` sur la radio (voir plus bas).

## Avec dvbtx

[dvbtx](https://github.com/TrusterBSW/DVB-Tx) multiplexe flux en direct, caméras et fichiers en
multiplex DVB complets (PSI/SI, guide des programmes, PCR réécrits) et pilote cette carte comme l'une de
ses radios. C'est la source conseillée : la carte elle-même ne multiplexe pas, ne corrige pas les PCR et
n'adapte pas le débit.

**Compatibilité**

| | |
|---|---|
| Version de dvbtx | **≥ 0.3.0** ([dvbtx_0.3.0_amd64.deb](https://github.com/TrusterBSW/DVB-Tx/raw/main/releases/dvbtx_0.3.0_amd64.deb)) ; le pilote de radio `edgeqam` n'est pas dans la 0.2.0 |
| Norme | DVB-T 2K, 64-QAM, 7/8, GI 1/32, 8 MHz uniquement ; dvbtx refuse d'autres paramètres de porteuse sur cette radio |
| Multiplex | 1 à 4 par carte, sur des canaux de 8 MHz adjacents (multiplex k = FREQ + (k−1) × 8 MHz) |
| Découverte | mDNS `_dvbmod._udp` (`address = "auto"`) ou une adresse IP fixe |
| Plusieurs cartes | `serial = "<id>"` sur la radio : l'`id` du TXT (adresse MAC Ethernet), le nom d'hôte ou le `NAME` de la carte ; `dvbtx devices` les liste. Avec plusieurs cartes et sans `serial`, dvbtx refuse au lieu d'en choisir une |
| Horloge | dvbtx suit le quartz de la carte (remplissage des tampons lu 4 fois par seconde), PCR horodatés en conséquence |
| Réglages | dvbtx règle la fréquence et désactive les multiplex inutilisés (`tune = true`, par défaut) ; le niveau d'émission reste celui de la carte |
| Documentation | [README de dvbtx, *An edgeQAM board*](https://github.com/TrusterBSW/DVB-Tx#an-edgeqam-board-fpga-modulator-on-the-network) (même section dans README.fr.md) et `man 5 dvbtx.toml`, section *edgeQAM boards* |

```toml
[[radio]]
name = "fishball"
driver = "edgeqam"
address = "auto"          # trouvée par mDNS ; ou l'IP de la carte, par ex. "192.168.1.50"
serial = "0060881fa308"   # seulement avec plusieurs cartes : id du TXT, nom d'hôte ou NAME

[[carrier]]
name = "Mux 1"
radio = "fishball"
channel = 21              # 474 MHz = le multiplex 1 de la carte ; le canal 22 serait son multiplex 2
  [[carrier.service]]
  name = "Infos"
  source = "https://example.org/live.m3u8"
```

Options de la radio : `tune = false` (dvbtx ne touche ni à la fréquence ni aux multiplex de la carte),
`buffer_ms` (remplissage visé sur la carte, 100 ms par défaut ; davantage sur un réseau moins régulier),
`center = <Hz>` sur une porteuse au lieu de `channel`. Un exemple complet à quatre multiplex est
`configs/edgeqam-4mux.toml` dans dvbtx.

Sans dvbtx, n'importe quel émetteur MPEG-TS à débit constant convient (voir
[Démarrage rapide](#démarrage-rapide)), mais le débit du flux doit alors coller au quartz de la carte :
sinon la carte insère des paquets nuls ou jette des paquets, et les PCR ne sont pas corrigés.

## Mesures

HamGeek Fishball 7010, mode 1R1T, dissipateur cuivre sans ventilateur.

| Essai | Résultat |
|---|---|
| Cœur HLS contre le moteur C de dvbtx (simulation C, 150 symboles, 2 et 4 multiplex) | identique bit à bit, 66–69 dB (quantification 16 bits) |
| Bitstream (4 multiplex, Vivado 2023.1) | timing respecté, 72 % des LUT, 58 % des BRAM, 54 % des DSP du XC7Z010 |
| Débit sur la carte | 4 329 symboles OFDM/s, 4 × 31,668 Mbit/s, 0 sous-débit DAC |
| Rebouclage numérique de l'AD9363, démodulation logicielle (gr-dtv) de chaque multiplex | MER 52 dB, 0 paquet non corrigible, TS identique à la source |
| 4 flux UDP à plein débit pendant 90 s (filaire) | 1 895 054 paquets par multiplex envoyés et reçus, aucune perte |
| RF sur le câble, HackRF One centré sur chaque multiplex, gr-dtv | 4 multiplex, 0 paquet non corrigible ; MER 22–23 dB, plate sur le canal (limite du HackRF 8 bits) |
| Flux venant de dvbtx, réception RF | service décodé (H.264 720p + AAC, guide), PCR à moins de 1 ms, débit calé sur la carte à 2 ppm près |
| **Téléviseur sur le câble** (carte à 30 dB d'atténuation, 4 multiplex venant de dvbtx, chaînes en direct) | la recherche de chaînes trouve les 4 multiplex et leurs 15 services avec noms et numéros ; chaînes regardées normalement |
| CPU (2 × Cortex-A9) | dvbt_tx environ 57 % d'un cœur pour 4 flux UDP |
| Température | Zynq stable à 70 °C avec 4 multiplex (firmware d'origine au repos : 77 °C) |

## Limites et remarques

- **DVB-T seulement**, paramètres figés (le DVB-T2 ne tient pas dans le XC7Z010). Canaux adjacents
  seulement.
- Au démarrage, l'AD9363 part à 89,75 dB d'atténuation (devicetree), puis `ATT` est appliqué.
- Testé en mode 1R1T uniquement (forcé dans `uEnv.txt`).
- L'oscillateur local de la carte est entre les multiplex 2 et 3 et y laisse une petite porteuse de
  fuite ; elle n'a pas gêné la réception.
- Envoyer les flux par une liaison filaire : en Wi-Fi, des coupures de quelques centaines de ms
  vident les tampons et la carte comble le trou avec des paquets nuls.
- Récepteurs utilisés : un téléviseur grand public (recherche de chaînes et visionnage), un HackRF One
  avec gr-dtv pour les mesures ; une clé RTL2832U courante ne s'est pas verrouillée lors de nos essais,
  sans investigation plus poussée.

## Et le DVB-T2 ?

Pas sur cette carte. Un modulateur DVB-T2 demande, en plus de ce qu'utilise le DVB-T, un codeur BCH et
un codeur LDPC de 64 800 bits, des entrelaceurs de cellules et temporel, un entrelaceur fréquentiel sur
tout le symbole, le symbole P1 et la signalisation L1. Estimation pour **un** multiplex DVB-T2
(8 MHz, un seul PLP), non synthétisée :

| | XC7Z010 (cette carte) | DVB-T2, mode 8K | DVB-T2, mode 32K | XC7Z020 |
|---|---|---|---|---|
| LUT | 17 600 | ~23 000 | ~26 000 | 53 200 |
| BRAM (36 Kbit) | 60 | ~60 | ~100 | 140 |
| DSP | 80 | ~35 | ~40 | 220 |

Ces totaux comprennent la base de la carte (système processeur, cœur AD9361, DMA : environ 5 000 LUT
et 10 BRAM). Le codeur LDPC (~6 000 LUT) et, en mode 32K, l'IFFT de 32 768 points et l'entrelaceur
fréquentiel (~70 BRAM à eux deux) dominent. Sur le XC7Z010, un seul multiplex représente déjà environ
130 % des LUT, et en mode 32K bien plus que les BRAM disponibles. Une carte **XC7Z020** prise en charge
par Tezuka (un Fishball 7020 ou un LibreSDR) accueillerait un multiplex DVB-T2 en mode 32K avec de la
marge (environ 50 % des LUT et 70 % des BRAM), peut-être deux en mode 8K. Un multiplex DVB-T2 de 8 MHz
transporte environ 40 à 50 Mbit/s (256-QAM 5/6), contre 127 Mbit/s pour les quatre multiplex DVB-T de
cette image. Non prévu.

## Compilation

Outils : Vivado et Vitis HLS 2023.1, un compilateur croisé `arm-linux-gnueabihf`, GNU Radio 3.10 avec
gr-dtv pour les vérifications.

1. **Cœur HLS** (`src/hls/v3`) : `NMUX=4 STEP=csim vitis_hls -f run_hls.tcl` le compare à une
   référence dvbtx produite par `src/gen/dvbt_multi_ref.c` ; `STEP=export` empaquette l'IP. Les tables
   ROM viennent de `src/gen/gen_tables.c`.
2. **Bitstream** : le fork F5OEO de [Maia SDR](https://github.com/F5OEO/maia-sdr), branche `refactor`
   (fbc289b), avec la correction des broches du XC7Z010 ; copier `src/hdl/*` dans
   `maia-hdl/projects/pluto/`, `src/hdl/project/*` dans `maia-hdl/projects/fishball_edgeqam4/`, l'IP
   exportée dans `maia-hdl/ip/edgeqam/`, puis `make` dans le projet. `bootgen -process_bitstream bin`
   donne `system_top.bin`.
3. **dvbt_tx** (`src/sw`) : `arm-linux-gnueabihf-gcc -O2 -o dvbt_tx dvbt_tx.c -liio -lpthread`
   avec libiio 0.26.
4. **Ramdisk** : le ramdisk Tezuka suivi d'une archive cpio contenant `/usr/sbin/dvbt_tx` et
   `/etc/init.d/S99edgeqam` (`src/rootfs`), réemballé avec `mkimage -T ramdisk -C none`.

`tools/` contient les vérifications utilisées pour les mesures : `dvbt_check.py` (MER, pilotes, TPS
d'une capture en rebouclage), `dvbt_rf_check.py` (MER par porteuse d'une capture RF réelle),
`gr_dvbt_rx.py` (récepteur logiciel jusqu'au TS), `mux_extract.py`, `ts_check.py`, `ts_udp_send.c`
(émetteur UDP à débit constant) et `meas_mer.sh`.

## Remerciements

- [Tezuka](https://github.com/F5OEO/tezuka_fw) (Evariste F5OEO) : firmware, noyau, ramdisk et support
  du Fishball ; environnement de démarrage de Christos Nikolaou SV1EIA.
- [Maia SDR](https://maia-sdr.org) (Daniel Estévez) et son fork F5OEO : le projet FPGA dont le
  bitstream est issu.
- [Analog Devices HDL](https://github.com/analogdevicesinc/hdl) : cœur AD9361, DMA et FIFO.
- [dvbtx](https://github.com/TrusterBSW/DVB-Tx) : le moteur DVB-T dont le cœur FPGA est porté.

## Licence

GPL-3.0-or-later pour le code de ce dépôt. Le bitstream et l'image contiennent aussi des éléments sous
leurs propres licences (Analog Devices HDL, Maia SDR, Tezuka, Linux). © 2026 BILLY Rémi.
