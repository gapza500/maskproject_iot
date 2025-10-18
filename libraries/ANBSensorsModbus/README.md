# ANBSensorsModbus<!--! {#mainpage} -->

<!--! @tableofcontents -->

<!--! @m_footernavigation -->

<!--! @if GITHUB -->

- [ANBSensorsModbus](#anbsensorsmodbus)
  - [The EnviroDIY ANBSensorsModbus Library](#the-envirodiy-anbsensorsmodbus-library)
    - [Using Modbus](#using-modbus)
    - [RS232 or RS485](#rs232-or-rs485)
    - [Modbus Irregularities](#modbus-irregularities)
  - [Library installation](#library-installation)
  - [Contributing](#contributing)
  - [License](#license)
  - [Acknowledgments](#acknowledgments)

<!--! @endif -->

## The EnviroDIY ANBSensorsModbus Library<!--! {#mainpage_intro} -->

This library is for communicating with pH sensors manufactured by [ANB Sensors](https://www.anbsensors.com/) using Modbus.
These sensors are marketed as next-generation calibration-free pH sensors.
The sensors can be operated autonomously when powered by an external battery or they can be controlled by an external controller.

ANB Sensors provides [documentation for their sensors on their website](https://www.anbsensors.com/newdocs/docs/intro).

### Using Modbus

The ANB sensors have two control interfaces: Modbus and a custom "Serial Terminal" interface.
For this library, we use the Modbus interface for the sensors.

We chose Modbus because:

- It allows connection with multiple devices simultaneously over the same serial line.
- It's a common communication standard with already existing Arduino library support.
- It requires much shorter commands than the "Serial Terminal" interface for the ANB sensors.

### RS232 or RS485

Most of ANB sensors have both RS232 and RS485 connections.
*Both* communication protocols (Modbus and Serial Terminal) are half-duplex and can be used over *either* interface.
But, *only a single interface can be used at one time* - so you must chose one or the other.
In developing this library, we primarily used the RS485 connection, but did do some testing over the RS232 lines to confirm matching behavior.
There are many comparisons of RS232 and RS485 available online to help you decide which you would like to use.

We chose RS485 because:

- It allows connection with multiple devices simultaneously over the same serial line.
- It allows more stable communication over long wires.
- We had RS485-to-TTL adapters on hand.

>[!tip]
> To use one of the communication interfaces, hold both wires of the other interface at ground or leave them free-floating.
> That is, to use the RS485 interface, you should connect both the Tx and Rx wires of the RS232 to ground.
> The wires could also be snipped and left disconnected.

### Modbus Irregularities

>[!note]
> If you are using this library, we've already worked around all of the Modbus irregularities we've found.
> You only need to worry about this if you want to call Modbus commands on the sensor directly.

There are some irregularities within the Modbus map and the responses of the ANB Sensors to Modbus commands.
The sensor responds to all of its documented modbus commands to give the values as if they were in the expected registers, but it doesn't give the same responses for each register when asking for registers in bulk.
If you send a command to ask for registers in any way beyond the documented requests, you will receive an error response.

In single commands the values are in these registers:

- 0x0000: pH (two registers)
- 0x0002: temperature (two registers)
- 0x0004: salinity (two registers)
- 0x0006: specific conductance (two registers)
- 0x0008: health (one register)
- 0x0009: status + diagnostic (one register)
- 0x000A: Serial number (3 registers)
- ... other things
- 0x0140: raw conductivity (two registers)

When pulling the 11 registers starting from 0 in bulk, you get:

- 0x0000: pH (two registers)
- 0x0002: temperature (two registers)
- 0x0004: salinity (two registers)
- 0x0006: specific conductance (two registers)
- 0x0008: health + status + diagnostic (all in one register)
- 0x0009: raw conductivity (two registers)

The Modbus instructions from ANB sensors also state that the "source" for all of the measurement setting functions and some of the administrative functions are input (read only) registers, but in all cases the functions use commands for **holding** (read/write) registers.
Most of the registers in question are, however, **write only** from within the modbus interface - attempting to read the registers will give an illegal function error.

## Library installation

This library is available through both the Arduino and PlatformIO library registries.
[Here is the PlatformIO registry page.](https://registry.platformio.org/libraries/envirodiy/ANBSensorsModbus)
Use the Arduino IDE to find the library in that registry.
The build and ingest logs for this library into the Arduino library registry are available [here](https://downloads.arduino.cc/libraries/logs/github.com/EnviroDIY/ANBSensorsModbus/).

## Contributing<!--! {#mainpage_contributing} -->

Open an [issue](https://github.com/EnviroDIY/ANBSensorsModbus/issues) to suggest and discuss potential changes/additions.
Feel free to open issues about any bugs you find or any sensors you would like to have added.

## License<!--! {#mainpage_license} -->

Software sketches and code are released under the BSD 3-Clause License -- See [LICENSE.md](https://github.com/EnviroDIY/ANBSensorsModbus/blob/master/LICENSE.md) file for details.

Documentation is licensed as [Creative Commons Attribution-ShareAlike 4.0](https://creativecommons.org/licenses/by-sa/4.0/) (CC-BY-SA) copyright.

Hardware designs shared are released, unless otherwise indicated, under the [CERN Open Hardware License 1.2](http://www.ohwr.org/licenses/cern-ohl/v1.2) (CERN_OHL).

## Acknowledgments<!--! {#mainpage_acknowledgments} -->

[EnviroDIY](http://envirodiy.org/)™ is presented by the Stroud Water Research Center, with contributions from a community of enthusiasts sharing do-it-yourself ideas for environmental science and monitoring.

[Sara Damiano](https://github.com/SRGDamia1) is the primary developer of the EnviroDIY ANBSensorsModbus library, with input from many [other contributors](https://github.com/EnviroDIY/ANBSensorsModbus/graphs/contributors).

This project has benefited from the support from the following funding sources:

- Stroud Water Research Center endowment
