# fpp-PixelRadio

RDS and audio control for the **Pixel Radio** FM transmitter, for
[Falcon Player (FPP)](https://github.com/FalconChristmas/fpp).

Keeps the transmitter's RDS text in step with whatever FPP is playing, so listeners see the current
song on their car radio.

## Features

- Pushes RDS RadioText to the transmitter, updated as the playlist advances.
- Station name (PS) and station ID, with a configurable interval for how often the ID is shown.
- Up to three rotating RDS messages with a configurable cycle time.
- Sets the transmit frequency and RDS program type.
- Configurable idle behaviour for when no playlist is running.

## Hardware

A Pixel Radio transmitter reachable over the network. The plugin drives it over HTTP at the
configured IP address and port, rather than over I2C or USB.

## Installation

Install from **Content Setup → Plugins** in the FPP web UI, then restart FPPD.

## Configuration

**Input/Output Setup → Pixel Radio** in the FPP web UI.

**Pixel Radio Device**

- *IP Address* / *Port* — where to reach the transmitter.
- *Frequency* — transmit frequency.
- *Playlist Idle Behavior* — what to do when no playlist is playing.

**RDS Settings**

- *Program Type* — RDS program type code.
- *Station Code* / *Station ID* — call sign and identifier.
- *Station ID Time* — how often to show the station ID.
- *RDS*, *RDS2*, *RDS3* — up to three RadioText messages.
- *RDS Cycle Time* — how long each message is displayed.

**Logging**

FPP's own *Plugin* log level is shared by every plugin, so this plugin can also keep its own log,
`logs/fpp-PixelRadio.log`, viewable and downloadable under **Content Setup → File Manager → Logs**.
Changes take effect when saved; no FPPD restart is needed.

- *Write PixelRadio Log File* — master switch for the file (off by default).
- *Log Commands Sent to Radio* — *Off*; *Configuration Only* (frequency, station code, program type,
  RDS start/stop, mute, carrier); or *Everything*, which adds the station ID and RDS text sent on
  every cycle along with the radio's response.
- *Log RDS Text Changes* — the station ID and RDS text each time it changes, e.g. on a new song.
- *Log Radio Online/Offline* — one line when the radio stops responding and one when it is back.
- *Log Setting Changes* — each setting saved on the page, with its new value.
- *Max Log File Size (KB)* — at this size the log is moved to `fpp-PixelRadio.log.1` and a new one
  started, so it never uses more than twice this.

Whatever these are set to, `fppd.log` still gets the radio going offline (warning) and coming back
(info), and anything it got before.

## Commands

- **PixelRadio Carrier** — turn the transmitter's carrier on or off.

## License

GPLv2 — see [LICENSE](LICENSE).
