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

## Commands

- **PixelRadio Carrier** — turn the transmitter's carrier on or off.

## License

GPLv2 — see [LICENSE](LICENSE).
