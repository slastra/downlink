# downlink

ESP-IDF firmware that plays an Icecast **Ogg Opus** stream (RUMP's Opus
codec) through a **PCM5102A** I2S DAC into an amp. The listening end of
RUMP: RUMP is the uplink, this is the downlink.

```
Icecast ──HTTP──▶ stream task ──▶ 512 KB ring ──▶ player task ──▶ I2S ──▶ PCM5102A ──▶ amp
                  (core 0)        (PSRAM)         (core 1)
                  ICY strip,                      OggOpusDecoder,
                  Ogg page sniff                  volume
```

The DAC takes raw PCM. The ESP decodes, so the codec only has to match
what the Icecast mount serves. This build decodes **Ogg Opus only**. MP3
support can come later from `esphome/micro-mp3`, chosen by `Content-Type`.

## Hardware

- **ESP32-S3-WROOM-1U N16R8** dev board, the same one as tspl-station
  (YD-ESP32-S3 layout): 16 MB flash, 8 MB octal PSRAM, two USB-C ports.
- **PCM5102A** breakout (the common purple GY-PCM5102 board).
- Line out to an amp.

### Wiring

| PCM5102A pin | ESP32-S3 | Notes |
|---|---|---|
| `VIN` | `5V` | The board's LDOs take 3.3–5 V. 5 V gives the analog rail headroom. |
| `GND` | `GND` | |
| `BCK` | **GPIO4** | bit clock, 64 fs (3.072 MHz at 48 kHz) |
| `LCK` / `LRCK` | **GPIO5** | word select, 48 kHz |
| `DIN` | **GPIO6** | serial data |
| `SCK` | `GND` | No MCLK. The DAC's PLL runs off BCK. |
| `FMT` | `GND` | I2S (Philips) format |
| `XSMT` | `3V3` | soft-mute off (high = play) |
| `FLT` | `GND` | normal-latency filter |
| `DEMP` | `GND` | de-emphasis off |
| `LOUT` / `ROUT` / `AGND` | amp input | or the board's 3.5 mm jack |

On the purple board, the four config pins also have solder pads on the
back: H1 = FLT, H2 = DEMP, H3 = XSMT, H4 = FMT. Bridge each to L or H to
match the table instead of wiring them. **XSMT must be high**. A floating or
low XSMT gives silence with everything else looking healthy.

All three I2S GPIOs can be changed in menuconfig. Stay off these pins:

- 0 (BOOT, used for the portal)
- 3, 45, 46 (straps)
- 19, 20 (USB)
- 26–37 (flash and octal PSRAM)
- 43, 44 (UART0 console)
- 48 (WS2812)

### USB ports

Flash and monitor through the **UART bridge** port (`UART`/`COM`,
`/dev/ttyUSB0` or `ttyACM0` depending on the bridge chip). The console is on
UART0, same as tspl-station. The native USB port is unused.

## RUMP side

Set RUMP's codec to **Opus** (Server tab). Vorbis is its default. Both
arrive as `application/ogg`, so the stream task reads the codec magic from
the first page and refuses Vorbis before a byte reaches the decoder:

```
E stream: stream is Ogg Vorbis, not Opus; this build decodes Ogg Opus only (set RUMP's codec to Opus)
```

It then retries at the backoff ceiling (30 s), so switching RUMP to Opus
recovers on its own.

## Build

ESP-IDF **v6.1**.

```sh
. ~/Projects/Esp/esp-idf-v6.1/export.sh      # bash; from fish: bash -c '. … && idf.py …'
cd ~/Projects/Esp/downlink
cp sdkconfig.defaults.local.example sdkconfig.defaults.local   # WiFi + stream URL, gitignored
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

`sdkconfig.defaults` is only applied when `sdkconfig` doesn't exist. After
editing either defaults file, `rm sdkconfig` (or use `idf.py menuconfig`).

Image: about 1.05 MB against 3 MB OTA slots.

## Configuration

`idf.py menuconfig` → **downlink**:

| Option | Default | |
|---|---|---|
| `DL_DEVICE_ID` | `downlink` | DHCP hostname, portal AP `downlink-<id>` |
| `DL_STREAM_URL` | `http://icecast.example:8000/stream` | placeholder; set yours in `sdkconfig.defaults.local`, or in the portal (stored in NVS) |
| `DL_I2S_BCK_GPIO` / `WS` / `DOUT` | 4 / 5 / 6 | |
| `DL_VOLUME` | 70 | digital gain, 0.5 dB per step below 100 (70 = −15 dB, 0 = mute) |
| `DL_RING_KB` | 512 | compressed ring in PSRAM, about 30 s at 128 kbps |
| `DL_PREBUFFER_KB` | 32 | fill before playing and after an underrun, about 2 s at 128 kbps |
| `DL_STALL_S` | 10 | no bytes for this long means reconnect |
| `DL_MAX_BACKOFF_S` | 30 | reconnect backoff ceiling |

WiFi lives under **Network link (WiFi station)**. `NETLINK_SSID0`/`PASS0`
seed the NVS credential table on first boot only. After that, NVS is the
source of truth.

## WiFi and the captive portal

`components/netlink` and `components/provision` come from tspl-station.
netlink stores up to 8 networks in NVS and picks the best in range by
priority, then RSSI. It re-knocks on band-steering rejections, roams below
−70 dBm, and backs off up to 30 s.

The portal opens an open AP **`downlink-<id>`** at `http://192.168.4.1/`
when:

- no known network has been seen for 90 s, or
- **BOOT is held for 3 s** (hold again to close it).

The page lists nearby networks and takes the password and **stream URL**.
Under *Advanced* are the priority and device id. Saving reboots the board.

## Behaviour

- **Reconnects.** A dropped WiFi link pauses the stream task until netlink
  is back. A dead or stalled connection retries with backoff from 1 s up to
  30 s, and resets to 1 s after a connection that lasted 30 s. The ring
  keeps playing through short gaps.
- **Stream restarts.** Every new connection, and every Ogg BOS page within
  one (a chained stream, as when the source restarts), is recorded as a
  byte offset. The player resets the decoder exactly there, so a reconnect
  never feeds the decoder a torn page. If the decoder loses the stream some
  other way, it skips to the next boundary and forces a reconnect.
- **Underruns.** The DMA clears to silence (no looping buzz). The player
  logs the underrun and waits for the prebuffer to refill.
- **Metadata.** Icecast doesn't interleave ICY metadata into Ogg streams.
  Titles come from the stream's OpusTags (`TITLE`, `ARTIST`), so they
  update when a new chain starts. The request still sends `Icy-MetaData: 1`,
  and if a server answers with `icy-metaint` (MP3 mounts), the blocks are
  stripped and `StreamTitle` is logged.

### Serial log

```
I stream: connected: type="application/ogg" name="..." br=? metaint=0
I stream: OpusTags: vendor="..."
I stream: now playing (OpusTags): Artist - Title
I player: prebuffered 33 KB, playing
I player: decoding Opus: 48000 Hz, 2 ch, pre-skip 312
I main: wifi=up ssid="..." rssi=-52 ch=6 ip=192.168.11.x drops=0 roams=0
I main: stream=up in=129kbps ring=41/512KB connects=1 drops=0 title="Artist - Title"
I main: player=playing underruns=0 resets=1 errors=0 heap=210K psram=7300K
```

The last three lines are a heartbeat every 30 s.

## Layout

| Path | |
|---|---|
| `main/stream.c` | HTTP client task, redirects, ICY demux, ring writer, boundaries |
| `main/ogg_sniff.c` | passive Ogg page walker: BOS offsets, OpusTags |
| `main/player.cpp` | ring → `micro_opus::OggOpusDecoder` → volume → I2S |
| `main/audio_out.c` | `i2s_std` setup for the PCM5102A |
| `main/settings.c` | device id and stream URL in NVS (`downlink` namespace) |
| `components/netlink` | WiFi station with a credential table (from tspl-station) |
| `components/provision` | captive portal (from tspl-station, plus a stream URL field) |

## Not yet

- MP3 via `esphome/micro-mp3`, with the decoder chosen by `Content-Type`.
- A status LED on the WS2812 (GPIO48), like tspl-station's.
- Volume at runtime (portal or console) instead of only at build time.
- OTA (the partition table already has the slots).
- Clock drift between the source and the DAC is absorbed by the ring, not
  corrected. At about 50 ppm, that's roughly one rebuffer every several hours in
  the worst direction.
