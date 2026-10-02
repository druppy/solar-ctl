# Solar control

[![firmware build](https://github.com/druppy/solar-ctl/actions/workflows/firmware.yml/badge.svg)](https://github.com/druppy/solar-ctl/actions/workflows/firmware.yml)
[![release](https://img.shields.io/github/v/release/druppy/solar-ctl?label=firmware)](https://github.com/druppy/solar-ctl/releases/latest)

Simple project that uses an RPi Zero W to communicate with a Deye inverter, display simple states on a local display, and propagate as much knowledge as possible to the HA energy module.

This is a project made to explore the possibilities of the home assistant energy system, to communicate with the inverter using modbus and communicate this to HA.

Systems like SolarAssistant exist, but this is an opportunity to learn.

## Inverter controller

Inverter controller is the application that handles communication with and control of the inverter, and also basic display communication. It lives in `inv_ctl/` (empty for now — will land as an `inv-ctl` recipe in the image).

## FW

Yocto Wrynose firmware for the **Raspberry Pi Zero W** (also runs on the plain
Zero / Zero 2 W with a machine change — see `fw/kas-rpi0.yml`), including the
inverter controller, to make a clean FW for the RPi. Build instructions:
[`fw/README.md`](fw/README.md).

Note that this firmware is a wild overkill for this application, but I needed to demonstrate that it was in fact possible to
make a production grade FW for a simple RPi Zero, and when it is doable here, it may also work for other platforms too. 

### Security warning

The images built in github trust exactly one swupdate signing key: the maintainer private key, which lives only in the
GitHub Actions secrets and is never committed — the public half is what gets baked into the image. That means anyone can
flash a release image and update it with that release's swu files, but nobody else can sign new ones. If you want a
private FW, build it locally and make your own key pair (`fw/tools/swu-keygen.sh`).

Also, there is no disk encryption and no secure boot in the current version, it should be doable and later this may be added.

## TFT Display

- 2.79 Inch 142×428
- Chip NV3007 TFT LCD Display Module ([TZT 142×428 Resolution 8 Pin SPI Full Color Screen Panel](https://www.aliexpress.com/item/1005012658981762.html))
- Works directly off the RPi header at 3.3 V: SPI0 (SCI→GPIO11, SDA→GPIO10, CS→GPIO8) plus RES/DC/BL on GPIO24/25/18 — full pin-by-pin wiring in [fw/README.md](fw/README.md#nv3007-279-tft-142428-spi-enabled-by-default)
- Driven by the in-kernel `panel-mipi-dbi` driver via our `nv3007` DT overlay; the panel stays dark until the vendor NV3007 init-sequence firmware file is baked into the image (fw/README.md explains the format and how to test a blob on the run).
