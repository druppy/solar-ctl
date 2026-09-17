# Solar control

[![firmware build](https://github.com/druppy/solar-ctl/actions/workflows/firmware.yml/badge.svg)](https://github.com/druppy/solar-ctl/actions/workflows/firmware.yml)
[![release](https://img.shields.io/github/v/release/druppy/solar-ctl?label=firmware)](https://github.com/druppy/solar-ctl/releases/latest)

Simple project that use a RPi to communicate with a Deye inverter, and display simple states on a local display, and propagates as much knowledge as possible to the HA energy module.

This is a project made to explore the possibilities of home assistant energy system, to communicate with the inverter using modbus and communicate this to HA.

Systems like SolarAssistant exists, but this is an opportunity to learn.

## Inverter controller

Inverter controller is the application the handle communication and control to the inverter and also basic display communication

## FW

Yocto Wrynose FW for RPi 2 / Zero, including the inverter controller too, to make a clean FW for a RPi.

## TFT Display 

- 2.79 Inch 142×428 
- Chip NV3007 TFT LCD Display Module 
- Should work directly with RPi
