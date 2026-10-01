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

### ESP32-S3 SuperMini

The SuperMini (ESP32-S3FH4R2: 4 MB flash, 2 MB quad PSRAM, one USB-C) works
too. Its variant puts I2S on the edge with the power pins, so the DAC wires to
one side:

| PCM5102A | SuperMini |
|---|---|
| `VIN` / `GND` | `5V` / `GND` |
| `BCK` | **GPIO9** |
| `LCK` | **GPIO11** |
| `DIN` | **GPIO10** |

`SCK`, `FMT`, `XSMT` and the rest are set as in the table above. Build and flash:

```sh
idf.py -B build-supermini -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.supermini" \
       -D SDKCONFIG=build-supermini/sdkconfig -p /dev/ttyACM0 build flash monitor
```

Its only port is native USB, so the console is USB-Serial/JTAG
(`/dev/ttyACM0`). If flashing can't connect, hold BOOT, tap RST, and release
BOOT.

### USB ports (N16R8)

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
cp sdkconfig.defaults.local.example sdkconfig.defaults.local   # WiFi, stream URL, broker; gitignored
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

`sdkconfig.defaults` is only applied when `sdkconfig` doesn't exist. After
editing either defaults file, `rm sdkconfig` (or use `idf.py menuconfig`).

Image: about 1.15 MB against 3 MB OTA slots (1.9 MB on the SuperMini).
`dependencies.lock` is committed and the registry components are pinned,
so a later build uses the same versions.

Host tests for the plain-C parts (the Ogg page walker, and the
buffer-depth controller over simulated days):

```sh
make -C test/host
```

`tools/fake-icecast` serves an Ogg Opus file the way Icecast serves a live
mount: header pages, then a byte-sized burst from a real-time "live"
position, then real-time pacing, with an optional forced disconnect.
Point a board at it with the MQTT `url` command to test buffering
against VBR swings and joins during silence, without a real source:

```sh
tools/fake-icecast programme.opus --drop-at 450    # http://<host>:8765/stream
```

## Configuration

`idf.py menuconfig` → **downlink**:

| Option | Default | |
|---|---|---|
| `DL_DEVICE_ID` | `downlink` | DHCP hostname, portal AP `downlink-<id>`, MQTT topic segment; give each board its own |
| `DL_BOARD` | `n16r8` | `supermini` in that variant; reported in the status |
| `DL_STREAM_URL` | `http://icecast.example:8000/stream` | placeholder; set yours in `sdkconfig.defaults.local` |
| `DL_I2S_BCK_GPIO` / `WS` / `DOUT` | 4 / 5 / 6 | |
| `DL_VOLUME` | 70 | digital gain, 0.5 dB per step below 100 (70 = −15 dB, 0 = mute) |
| `DL_RING_KB` | 512 | compressed ring in PSRAM: 30 s of loud 128 kbps audio, far more of quiet |
| `DL_PREBUFFER_MS` | 2000 | audio buffered before playing, and after an underrun |
| `DL_BUFFER_MS` | 4000 | buffer depth held against clock drift |
| `DL_BUFFER_MAX_MS` | 12000 | above this, catch up (decode and discard) to `DL_BUFFER_MS` |
| `DL_STALL_S` | 10 | no bytes for this long means reconnect |
| `DL_MAX_BACKOFF_S` | 30 | reconnect backoff ceiling |

Stream URL, volume, WiFi (`NETLINK_SSID0`/`PASS0`, under **Network link**)
and the broker (`UPLINK_*`, under **Uplink**) are all **seeds**: copied to
NVS on first boot, after which NVS is the truth and the portal or MQTT
commands change them. So an update to an image built without your local
file (a future public OTA release) keeps a board's settings. To re-seed a
board from the build, erase NVS (`idf.py erase-flash`, then flash).

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
Leaving the password blank for a saved network keeps its stored password.
Under *Advanced* are the priority and device id. Saving reboots the board.

Unattended, it closes after 5 minutes and netlink gets a full 90 s to find
a known network before it can reopen. That way a router reboot at a remote
site costs a few minutes, not an outage until someone power-cycles the
board (tspl-station's 30-minute portal and immediate reopen did that).

## Behaviour

- **Reconnects.** A dropped WiFi link pauses the stream task until netlink
  is back. A dead or stalled connection retries with backoff from 1 s up to
  30 s, and resets to 1 s after a connection that lasted 30 s. The ring
  keeps playing through short gaps.
- **Joining mid-stream.** Icecast sends a new listener the stored header
  pages (sequence 0, 1) and then live pages (sequence 625, …).
  micro-ogg-demuxer rejects that gap outright, so the page walker renumbers
  sequences as bytes arrive. The CRC is left stale, since the decoder runs
  with CRC checks off.
- **Stream restarts.** Every new connection, and every Ogg BOS page within
  one (a chained stream, as when the source restarts), is recorded as a
  byte offset. The player resets the decoder exactly there, so a reconnect
  never feeds the decoder a torn page. If the decoder loses the stream some
  other way, it skips to the next boundary and forces a reconnect.
- **One output clock.** The player task writes a 10 ms block to the DAC
  every 10 ms, and the blocking I2S write is the clock. The music source
  hands over what it has decoded and the rest is silence, so a rebuffer, a
  reconnect or a catch-up never stalls the output. Catch-up and reconnect
  dedupe decode and discard under a time budget per block (8 ms, and none
  when the output is running late).
- **Underruns.** 100 ms with nothing to play counts as an underrun, and the
  player waits for the prebuffer to refill. Shorter gaps play as silence.
- **Buffering is measured in audio, not bytes.** Opus is variable-bitrate:
  a quiet passage drops from ~130 kbps to ~3 kbps, so a byte count says
  nothing about how much is buffered. (A byte-based first version, on a one-hour
  soak, prebuffered 85 s of silence and ended up minutes behind live.) Every
  Ogg page carries a granule position, an exact sample count. The stream
  task records the newest one written, and the player walks the pages it
  decodes. Within one connection and logical stream, the difference is the
  audio buffered, to within a page (~100 ms), at any bitrate.
- **Clock drift.** The source's sound card and this board's I2S clock
  differ by tens of ppm, which uncorrected means a rebuffer every 10–40
  hours, or a backlog that grows until Icecast drops the connection. The
  player holds the buffer at `DL_BUFFER_MS` (4 s): when a 30 s average
  leaves ±0.5 s of it, it drops or repeats one stereo sample every 50 ms
  (about 400 ppm) until it's back within ±0.125 s. One 21 µs sample at a
  time is inaudible.
- **Backlog catch-up.** Icecast's connect burst is sized in bytes too:
  joining during a silent passage hands over minutes of audio at once.
  Above `DL_BUFFER_MAX_MS` the player decodes and discards, faster than
  real time, until it's back at the target. That's one jump, almost always
  through silence, instead of staying minutes behind live.
  `test/host/test_drift.c` simulates three days per drift case from −200
  to +200 ppm (held within ±0.5 s), plus joining during silence (one
  catch-up, then steady).
- **Self-rescue.** A supervisor reboots the board when it's wedged in a
  way nothing else recovers from: audio waiting in the ring for 60 s with
  none reaching the DAC, or under 16 KB of internal heap for 10 s. The
  task watchdog panics (and reboots) on a CPU hung for 5 s instead of only
  printing. The next status says why: `reset` from the chip, and
  `rebootCause` when the firmware chose to (`player stall`, `low memory`,
  `mqtt command`, `portal save`).
- **Metadata.** Icecast doesn't interleave ICY metadata into Ogg streams.
  Titles come from the stream's OpusTags (`TITLE`, `ARTIST`), so they
  update when a new chain starts. The request still sends `Icy-MetaData: 1`,
  and if a server answers with `icy-metaint` (MP3 mounts), the blocks are
  stripped and `StreamTitle` is logged.

### Remote management (MQTT)

For a board deployed out of reach. The connection is outbound only, so it
works behind any site's NAT with no port forwarding. The broker settings are
a build seed (`CONFIG_UPLINK_URI/USER/PASS` in `sdkconfig.defaults.local`),
copied to NVS on first boot like the WiFi credentials. Topics sit under
`downlink/<device id>/`:

| Topic | |
|---|---|
| `status` | retained health, every 30 s and on every state change; the will sets `{"online":false}` if the board vanishes (5 s delay hides WiFi roams) |
| `cmd` | commands in |
| `cmd/result` | one reply per command |

```sh
mosquitto_sub -h <broker> -u … -P … -t 'downlink/#' -v
mosquitto_pub -h <broker> -u … -P … -t downlink/downlink/cmd -m '{"cmd":"url","url":"http://host:8000/mount"}'
```

| Command | |
|---|---|
| `{"cmd":"status"}` | publish status now |
| `{"cmd":"url","url":"http://…"}` | switch streams now (reconnects), saved to NVS |
| `{"cmd":"volume","value":0-100}` | live (glides over 50 ms, no click), saved to NVS |
| `{"cmd":"reboot"}` | reboot after replying |

Status is also published when the state changes or a new stream error
appears (at most once a second). A status looks like:

```json
{"state":"playing","title":"Artist - Title","url":"http://…/stream","board":"supermini",
 "rebootCause":"mqtt command",
 "stream":{"connected":true,"kbps":132,"ringKB":48,"ringPct":9,"connects":1,"drops":0,"http":200},
 "player":{"underruns":0,"resets":1,"errors":0,"playedS":40,"volume":70,"bufferMs":4010,
           "drift":{"mode":"idle","targetMs":4000,"dropped":0,"repeated":0,"catchups":0,"skippedS":0}},
 "stackFree":{"main":3824,"stream":6048,"player":4668,"mqtt_pub":2708,"mqtt_task":4460,"wifi_join":2568},
 "wifi":{"ssid":"…","rssi":-51,"ip":"…","drops":0,"roams":0},
 "online":true,"fw":"0bfe001","uptime":42,"reset":"software","timeSynced":true,
 "mqttConnects":1,"heapFree":85907,"heapMin":52548,"psramFree":1348660,"timestamp":"…"}
```

What to watch:

| Field | |
|---|---|
| `state` | same as the LED: `portal`, `portal_client`, `no_wifi`, `wrong_codec`, `connecting`, `buffering`, `playing` |
| `stream.lastError` | present only when not streaming: `HTTP 404`, `connect failed: …`, `stalled: …`, `stream is Ogg Vorbis, not Opus` |
| `reset` / `rebootCause` | why the last boot happened; `panic`, `task_wdt` and `brownout` are the ones to worry about |
| `player.bufferMs` | audio buffered; sits near `drift.targetMs` (absent while a boundary is in the ring) |
| `audio.blockAvgUs` / `blockMaxUs` | CPU per 10 ms output block (decode and mix), average and the worst of the last 30 s; ~1–2 ms is normal, ~6 ms during a catch-up, and the DMA covers up to ~80 ms |
| `player.underruns` | should stay flat; rising means the network can't keep up |
| `player.drift` | `dropped`/`repeated` grow slowly in one direction for good (that's the clock difference being absorbed); `catchups` counts backlog jumps |
| `heapMin`, `stackFree` | lowest free memory seen; a number trending toward zero is a crash that hasn't happened yet |

### Status LED

The onboard WS2812 (GPIO48 on both boards) shows the first state that
applies. Brightness is `DL_LED_BRIGHTNESS` (15%).

| LED | State |
|---|---|
| magenta pulse / solid | portal open / a phone is on it |
| red | no WiFi |
| slow red pulse | the mount isn't Opus (switch RUMP's codec) |
| yellow pulse | WiFi up, stream not connected (retrying) |
| blue | connected, buffering |
| green | playing |

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
| `main/main.c` | startup, LED state, MQTT status and commands, the supervisor |
| `main/stream.c` | HTTP client task, redirects, ICY demux, ring writer, boundaries, `lastError` |
| `main/ogg_sniff.c` | Ogg page walker: BOS offsets, codec check, OpusTags, sequence renumbering |
| `main/player.cpp` | music source (ring → `micro_opus::OggOpusDecoder` → depth control) and the 10 ms output loop |
| `main/mixer.c` | output-stage gain: per-sample volume ramps (plain C, host-tested) |
| `main/drift.c` | buffer-depth control: drift and catch-up (plain C, host-tested) |
| `main/audio_out.c` | `i2s_std` setup for the PCM5102A |
| `main/settings.c` | device id, stream URL, volume in NVS (`downlink` namespace) |
| `components/netlink` | WiFi station with a credential table (from tspl-station) |
| `components/provision` | captive portal (from tspl-station, plus a stream URL field) |
| `components/uplink` | MQTT: retained status, will, commands (slimmed from tspl-station) |
| `components/led` | WS2812 status LED (from tspl-station) |
| `test/host` | host tests: `make -C test/host` |
| `tools/fake-icecast` | bench Icecast stand-in: burst, real-time pacing, forced drops |

## Not yet

- MP3 via `esphome/micro-mp3`, with the decoder chosen by `Content-Type`.
- OTA from GitHub Releases (the partition tables already have the slots).
- MQTT over TLS (8883). Plain 1883 sends the broker password in the clear,
  which matters once the board is on someone else's network.
