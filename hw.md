# Hardware Setup

There are two Raspberry Pi devices that you are able to access:

You need to ssh into the devices as the user tim and you have root access.

Each device has a set of interfaces (eth0 and wlan0) with both IPv4 and IPv6.
To use a specific interface and IP you can use:

 * ipv4.eth0.\<hostname\> -- IPv4 address on eth0 interface.
 * ipv6.wlan0.\<hostname\> -- IPv6 address on wlan0 interface.
 * eth0.\<hostname\> -- Both IPv4 and IPv6 addresses.
 * \<hostname\> -- All IPv4 and IPv6 addresses.

---

# rpi5-pmod.iot.welland.mithis.com

Raspberry Pi 5 Model B Rev 1.1 with a Digilent Pmod HAT Adapter.

| Property        | Value                                            |
|-----------------|--------------------------------------------------|
| SoC             | BCM2712 (4x Cortex-A76 @ 2.4 GHz)               |
| RAM             | 8 GB LPDDR4X                                     |
| OS              | Debian GNU/Linux 13 (trixie)                     |
| Kernel          | 6.12.47+rpt-rpi-2712 (aarch64)                  |
| GPIO controller | RP1 southbridge (via PCIe 2.0 x4)               |
| PIO             | 1 instance, 4 state machines, 200 MHz            |
| Serial          | `/dev/ttyAMA10`                                  |
| SPI             | `/dev/spidev10.0`                                |
| I2C             | `/dev/i2c-13`, `/dev/i2c-14`                     |
| HAT EEPROM      | Vendor: Digilent, Product: Pmod HAT Adaptor      |

# rpi4-pmod.iot.welland.mithis.com

Raspberry Pi 4 Model B Rev 1.5 with a Digilent Pmod HAT Adapter.

| Property        | Value                                            |
|-----------------|--------------------------------------------------|
| SoC             | BCM2711 (4x Cortex-A72 @ 1.8 GHz)               |
| RAM             | 4 GB LPDDR4                                      |
| OS              | Debian GNU/Linux 13 (trixie)                     |
| Kernel          | 6.12.47+rpt-rpi-v8 (aarch64)                    |
| GPIO controller | BCM2711 (direct memory-mapped)                   |
| PIO             | None (BCM2711 has no PIO block)                  |
| Serial          | None enabled (UART available via config.txt)     |
| SPI             | None enabled (SPI available via config.txt)      |
| I2C             | `/dev/i2c-20`, `/dev/i2c-21`                     |
| HAT EEPROM      | Vendor: Digilent, Product: Pmod HAT Adaptor      |

---

# Digilent Pmod HAT Adapter

The Digilent Pmod HAT Adapter (also branded as DesignSpark Pmod HAT, RS part 144-8419)
plugs into the Raspberry Pi 40-pin GPIO header and provides three 2x6-pin Pmod host
connectors: **JA**, **JB**, and **JC**.

Reference manual: https://digilent.com/reference/add-ons/pmod-hat/reference-manual

Key specifications:
- 3 x 2x6-pin (12-pin) Pmod host connectors
- 3.3V I/O on all Pmod pins (directly connected to RPi GPIOs, which are 3.3V)
- 16 mA current limit per Pmod GPIO
- 5V barrel jack for external power supply
- VCC on Pmod pins is 3.3V from the RPi's 3.3V rail

## GPIO Pin Mapping

The pin mapping below is from the
[DesignSpark.Pmod HAT.py source code](https://github.com/DesignSparkRS/DesignSpark.Pmod/blob/master/DesignSpark/Pmod/HAT.py),
which is the authoritative software definition. All GPIO numbers use BCM numbering.

### Pmod Connector JA (SPI + GPIO)

JA top row (pins 1-6) carries SPI0 with chip select CE0.
JA bottom row (pins 7-12) carries PCM/PWM signals.

| Pmod Pin | BCM GPIO | RPi 40-pin Header | Alternate Function(s)          |
|----------|----------|--------------------|--------------------------------|
| JA1      | GPIO8    | Pin 24             | **SPI0_CE0**                   |
| JA2      | GPIO10   | Pin 19             | **SPI0_MOSI**                  |
| JA3      | GPIO9    | Pin 21             | **SPI0_MISO**                  |
| JA4      | GPIO11   | Pin 23             | **SPI0_SCLK**                  |
| JA5      | GND      | Pin 25 (GND)       | Ground                         |
| JA6      | VCC      | 3.3V               | Power (3.3V)                   |
| JA7      | GPIO19   | Pin 35             | PCM_FS / PWM1 / SPI1_MISO     |
| JA8      | GPIO21   | Pin 40             | PCM_DOUT / GPCLK1 / SPI1_SCLK |
| JA9      | GPIO20   | Pin 38             | PCM_DIN / GPCLK0 / SPI1_MOSI  |
| JA10     | GPIO18   | Pin 12             | PCM_CLK / PWM0 / SPI1_CE0     |
| JA11     | GND      | Pin 39 (GND)       | Ground                         |
| JA12     | VCC      | 3.3V               | Power (3.3V)                   |

### Pmod Connector JB (SPI + I2C + GPIO)

JB top row (pins 1-6) carries SPI0 with chip select CE1 (shares MOSI/MISO/CLK with JA).
JB bottom row (pins 7-12) carries I2C1 on pins 9-10.

| Pmod Pin | BCM GPIO | RPi 40-pin Header | Alternate Function(s)          |
|----------|----------|--------------------|--------------------------------|
| JB1      | GPIO7    | Pin 26             | **SPI0_CE1**                   |
| JB2      | GPIO10   | Pin 19             | **SPI0_MOSI** (shared with JA) |
| JB3      | GPIO9    | Pin 21             | **SPI0_MISO** (shared with JA) |
| JB4      | GPIO11   | Pin 23             | **SPI0_SCLK** (shared with JA) |
| JB5      | GND      | Pin 25 (GND)       | Ground                         |
| JB6      | VCC      | 3.3V               | Power (3.3V)                   |
| JB7      | GPIO26   | Pin 37             | GPIO only                      |
| JB8      | GPIO13   | Pin 33             | PWM1                           |
| JB9      | GPIO3    | Pin 5              | **I2C1_SCL** (1.8kΩ pull-up)  |
| JB10     | GPIO2    | Pin 3              | **I2C1_SDA** (1.8kΩ pull-up)  |
| JB11     | GND      | Pin 39 (GND)       | Ground                         |
| JB12     | VCC      | 3.3V               | Power (3.3V)                   |

### Pmod Connector JC (UART + GPIO)

JC top row (pins 1-6) carries UART0.
JC bottom row (pins 7-12) carries GPCLK/PWM signals.

| Pmod Pin | BCM GPIO | RPi 40-pin Header | Alternate Function(s)          |
|----------|----------|--------------------|--------------------------------|
| JC1      | GPIO16   | Pin 36             | **UART0_CTS** / SPI1_CE2      |
| JC2      | GPIO14   | Pin 8              | **UART0_TXD**                  |
| JC3      | GPIO15   | Pin 10             | **UART0_RXD**                  |
| JC4      | GPIO17   | Pin 11             | **UART0_RTS** / SPI1_CE1      |
| JC5      | GND      | Pin 25 (GND)       | Ground                         |
| JC6      | VCC      | 3.3V               | Power (3.3V)                   |
| JC7      | GPIO4    | Pin 7              | GPCLK0                         |
| JC8      | GPIO12   | Pin 32             | PWM0                           |
| JC9      | GPIO5    | Pin 29             | GPCLK1                         |
| JC10     | GPIO6    | Pin 31             | GPCLK2                         |
| JC11     | GND      | Pin 39 (GND)       | Ground                         |
| JC12     | VCC      | 3.3V               | Power (3.3V)                   |

### Unused GPIO Pins

Five RPi GPIO pins are **not connected** to any Pmod connector and remain available
for other purposes: **GPIO22**, **GPIO23**, **GPIO24**, **GPIO25**, **GPIO27**.

### Shared Bus Constraints

**SPI0 bus sharing:** JA (pins 1-4) and JB (pins 1-4) share the SPI0 MOSI, MISO, and
SCLK lines (GPIO10, GPIO9, GPIO11). They differ only in chip select: JA uses CE0
(GPIO8) and JB uses CE1 (GPIO7). This means:
- Two SPI devices can be used simultaneously (one on JA, one on JB) using different
  chip selects
- If JA or JB is used for non-SPI GPIO purposes, the shared pins conflict and the
  other port's top row becomes unusable

**I2C1 bus:** JB pins 9-10 carry I2C1 (GPIO2/SDA, GPIO3/SCL). These pins have
hardware 1.8kΩ pull-up resistors to 3.3V on the RPi board itself.

**UART0:** JC pins 1-4 carry the primary UART. On RPi4, this is the PL011 UART
(typically `/dev/ttyAMA0` when enabled). On RPi5, UART routing differs due to RP1.

---

# Pmod Interface Types

The Pmod standard defines several interface types that specify the signal assignments
on the 6-pin (1x6) or 12-pin (2x6) connectors. The specification is maintained by
Digilent:
https://digilent.com/reference/_media/reference/pmod/pmod-interface-specification-1_3_1.pdf

## Physical Connector Layout

```
  12-pin (2x6) Pmod connector        6-pin (1x6) Pmod connector
  (active-low nSS side = pin 1)

  ┌─────────────────────────┐         ┌─────────────────────┐
  │ Pin6  Pin5  Pin4  Pin3  │         │ Pin6 Pin5 Pin4 Pin3 │
  │ (VCC) (GND)             │         │(VCC)(GND)           │
  │ Pin12 Pin11 Pin10 Pin9  │         │ Pin2 Pin1           │
  │ (VCC) (GND)             │         └─────────────────────┘
  │ Pin2  Pin1              │
  └─────────────────────────┘

  Physical pin numbering (active-low nSS end = pin 1):

  Top row:     Pin 1 | Pin 2 | Pin 3 | Pin 4 | Pin 5 (GND) | Pin 6 (VCC)
  Bottom row:  Pin 7 | Pin 8 | Pin 9 | Pin10 | Pin11 (GND) | Pin12 (VCC)
```

## Type 1 -- GPIO

General-purpose bidirectional I/O. All signal pins are directly connected to
host GPIOs with no protocol requirement.

| Pin | Signal | Direction      |
|-----|--------|----------------|
| 1   | IO1    | Bidirectional  |
| 2   | IO2    | Bidirectional  |
| 3   | IO3    | Bidirectional  |
| 4   | IO4    | Bidirectional  |
| 5   | GND    | Power          |
| 6   | VCC    | Power          |

**Type 1A (expanded GPIO):** Uses the full 12-pin connector. Pins 7-10 are
additional GPIO (IO5-IO8), with pins 11 (GND) and 12 (VCC).

## Type 2 -- SPI

Serial Peripheral Interface. The host acts as SPI master.

| Pin | Signal | Direction      | Description                    |
|-----|--------|----------------|--------------------------------|
| 1   | CS     | Host → Pmod   | Chip select (active low)       |
| 2   | MOSI   | Host → Pmod   | Master Out, Slave In           |
| 3   | MISO   | Pmod → Host   | Master In, Slave Out           |
| 4   | SCK    | Host → Pmod   | Serial clock                   |
| 5   | GND    | Power          |                                |
| 6   | VCC    | Power          |                                |

**Type 2A (expanded SPI):** Uses the full 12-pin connector. Bottom row provides
additional signals:

| Pin | Signal | Direction      | Description                    |
|-----|--------|----------------|--------------------------------|
| 7   | INT    | Pmod → Host   | Interrupt (active low), optional|
| 8   | RESET  | Host → Pmod   | Reset (active low), optional   |
| 9   | CS2    | Host → Pmod   | Second chip select, optional   |
| 10  | CS3    | Host → Pmod   | Third chip select, optional    |
| 11  | GND    | Power          |                                |
| 12  | VCC    | Power          |                                |

Pins 7-10 may also be used as general-purpose GPIO when the optional SPI
signals are not needed.

## Type 3 -- UART

Universal Asynchronous Receiver/Transmitter.

| Pin | Signal | Direction      | Description                    |
|-----|--------|----------------|--------------------------------|
| 1   | CTS    | Pmod → Host   | Clear To Send (flow control)   |
| 2   | TXD    | Host → Pmod   | Transmit data                  |
| 3   | RXD    | Pmod → Host   | Receive data                   |
| 4   | RTS    | Host → Pmod   | Request To Send (flow control) |
| 5   | GND    | Power          |                                |
| 6   | VCC    | Power          |                                |

**Type 3A (expanded UART):** Uses the full 12-pin connector. Bottom row provides
additional signals (typically GPIO or a second UART channel).

Note: CTS/RTS flow control signals (pins 1 and 4) are optional. A minimal UART
Pmod only needs TXD (pin 2) and RXD (pin 3).

## Type 4 -- H-Bridge

Motor driver (single H-bridge). Note: Type 4 is deprecated.

| Pin | Signal | Direction      | Description                    |
|-----|--------|----------------|--------------------------------|
| 1   | DIR    | Host → Pmod   | Direction control              |
| 2   | EN     | Host → Pmod   | Enable (PWM input)             |
| 3   | SA     | Pmod → Host   | Sensor A (encoder feedback)    |
| 4   | SB     | Pmod → Host   | Sensor B (encoder feedback)    |
| 5   | GND    | Power          |                                |
| 6   | VCC    | Power          |                                |

## Type 5 -- Dual H-Bridge

Dual motor driver. Note: Type 5 is deprecated.

Uses the full 12-pin connector with two H-bridge channels.

## Type 6 -- I2C

Inter-Integrated Circuit bus.

| Pin | Signal    | Direction      | Description                    |
|-----|-----------|----------------|--------------------------------|
| 1   | INT/GPIO  | Pmod → Host   | Interrupt (optional) or GPIO   |
| 2   | RESET/GPIO| Host → Pmod   | Reset (optional) or GPIO       |
| 3   | SCL       | Host → Pmod   | I2C clock                      |
| 4   | SDA       | Bidirectional  | I2C data                       |
| 5   | GND       | Power          |                                |
| 6   | VCC       | Power          |                                |

**Type 6A (expanded I2C):** Uses the full 12-pin connector. Bottom row provides
additional signals (typically GPIO, second I2C channel, or SPI for dual-interface
devices).

---

# Pmod HAT Connector-to-Type Compatibility

Based on the GPIO mappings and alternate functions, each Pmod connector on the HAT
supports specific Pmod interface types:

| Connector | Top Row (pins 1-4)        | Bottom Row (pins 7-10)     | Supported Pmod Types           |
|-----------|---------------------------|----------------------------|--------------------------------|
| **JA**    | SPI0 (CE0, MOSI, MISO, CLK) | PCM / PWM / SPI1        | Type 2 (SPI), Type 1 (GPIO)   |
| **JB**    | SPI0 (CE1, MOSI, MISO, CLK) | GPIO, PWM1, I2C1 (SCL, SDA) | Type 2 (SPI), Type 6 (I2C on bottom), Type 1 (GPIO) |
| **JC**    | UART0 (CTS, TXD, RXD, RTS)  | GPCLK, PWM0             | Type 3 (UART), Type 1 (GPIO)  |

### Type 2 (SPI) on JA

| Pmod Type 2 Pin | Signal | JA Pin | BCM GPIO | RPi Function   |
|-----------------|--------|--------|----------|----------------|
| Pin 1 (CS)      | CS     | JA1    | GPIO8    | SPI0_CE0       |
| Pin 2 (MOSI)    | MOSI   | JA2    | GPIO10   | SPI0_MOSI      |
| Pin 3 (MISO)    | MISO   | JA3    | GPIO9    | SPI0_MISO      |
| Pin 4 (SCK)     | SCK    | JA4    | GPIO11   | SPI0_SCLK      |

### Type 2 (SPI) on JB

| Pmod Type 2 Pin | Signal | JB Pin | BCM GPIO | RPi Function   |
|-----------------|--------|--------|----------|----------------|
| Pin 1 (CS)      | CS     | JB1    | GPIO7    | SPI0_CE1       |
| Pin 2 (MOSI)    | MOSI   | JB2    | GPIO10   | SPI0_MOSI      |
| Pin 3 (MISO)    | MISO   | JB3    | GPIO9    | SPI0_MISO      |
| Pin 4 (SCK)     | SCK    | JB4    | GPIO11   | SPI0_SCLK      |

### Type 3 (UART) on JC

| Pmod Type 3 Pin | Signal | JC Pin | BCM GPIO | RPi Function   |
|-----------------|--------|--------|----------|----------------|
| Pin 1 (CTS)     | CTS    | JC1    | GPIO16   | UART0_CTS      |
| Pin 2 (TXD)     | TXD    | JC2    | GPIO14   | UART0_TXD      |
| Pin 3 (RXD)     | RXD    | JC3    | GPIO15   | UART0_RXD      |
| Pin 4 (RTS)     | RTS    | JC4    | GPIO17   | UART0_RTS      |

### Type 6 (I2C) on JB bottom row

| Pmod Type 6 Pin | Signal    | JB Pin | BCM GPIO | RPi Function   |
|-----------------|-----------|--------|----------|----------------|
| Pin 1 (INT)     | INT/GPIO  | JB7    | GPIO26   | GPIO           |
| Pin 2 (RESET)   | RESET/GPIO| JB8    | GPIO13   | PWM1 / GPIO    |
| Pin 3 (SCL)     | SCL       | JB9    | GPIO3    | I2C1_SCL       |
| Pin 4 (SDA)     | SDA       | JB10   | GPIO2    | I2C1_SDA       |

---

# Inter-RPi Jumper Cable Connections

The two RPi devices are connected via jumper cables between their Pmod connectors.
All three ports are connected straight-through: JA-to-JA, JB-to-JB, and JC-to-JC.

Since both RPi devices use the same Pmod HAT Adapter, the **same BCM GPIO number**
on each side is connected together. For example, RPi5 GPIO8 (JA pin 1) is wired
to RPi4 GPIO8 (JA pin 1).

## Complete Jumper Connection Map

### JA connections (RPi5 ↔ RPi4)

| Pmod Pin | Signal      | RPi5 GPIO | RPi4 GPIO | Notes                    |
|----------|-------------|-----------|-----------|--------------------------|
| JA1      | SPI0_CE0    | GPIO8     | GPIO8     | SPI chip select 0        |
| JA2      | SPI0_MOSI   | GPIO10    | GPIO10    | Shared with JB2          |
| JA3      | SPI0_MISO   | GPIO9     | GPIO9     | Shared with JB3          |
| JA4      | SPI0_SCLK   | GPIO11    | GPIO11    | Shared with JB4          |
| JA5      | GND         | --        | --        | Ground                   |
| JA6      | VCC         | --        | --        | 3.3V (DO NOT jumper VCC) |
| JA7      | PCM_FS/PWM1 | GPIO19    | GPIO19    |                          |
| JA8      | PCM_DOUT    | GPIO21    | GPIO21    |                          |
| JA9      | PCM_DIN     | GPIO20    | GPIO20    |                          |
| JA10     | PCM_CLK/PWM0| GPIO18    | GPIO18    |                          |
| JA11     | GND         | --        | --        | Ground                   |
| JA12     | VCC         | --        | --        | 3.3V (DO NOT jumper VCC) |

### JB connections (RPi5 ↔ RPi4)

| Pmod Pin | Signal      | RPi5 GPIO | RPi4 GPIO | Notes                    |
|----------|-------------|-----------|-----------|--------------------------|
| JB1      | SPI0_CE1    | GPIO7     | GPIO7     | SPI chip select 1        |
| JB2      | SPI0_MOSI   | GPIO10    | GPIO10    | Shared with JA2          |
| JB3      | SPI0_MISO   | GPIO9     | GPIO9     | Shared with JA3          |
| JB4      | SPI0_SCLK   | GPIO11    | GPIO11    | Shared with JA4          |
| JB5      | GND         | --        | --        | Ground                   |
| JB6      | VCC         | --        | --        | 3.3V (DO NOT jumper VCC) |
| JB7      | GPIO        | GPIO26    | GPIO26    |                          |
| JB8      | PWM1        | GPIO13    | GPIO13    |                          |
| JB9      | I2C1_SCL    | GPIO3     | GPIO3     | Has 1.8kΩ pull-up        |
| JB10     | I2C1_SDA    | GPIO2     | GPIO2     | Has 1.8kΩ pull-up        |
| JB11     | GND         | --        | --        | Ground                   |
| JB12     | VCC         | --        | --        | 3.3V (DO NOT jumper VCC) |

### JC connections (RPi5 ↔ RPi4)

| Pmod Pin | Signal      | RPi5 GPIO | RPi4 GPIO | Notes                    |
|----------|-------------|-----------|-----------|--------------------------|
| JC1      | UART0_CTS   | GPIO16    | GPIO16    |                          |
| JC2      | UART0_TXD   | GPIO14    | GPIO14    |                          |
| JC3      | UART0_RXD   | GPIO15    | GPIO15    |                          |
| JC4      | UART0_RTS   | GPIO17    | GPIO17    |                          |
| JC5      | GND         | --        | --        | Ground                   |
| JC6      | VCC         | --        | --        | 3.3V (DO NOT jumper VCC) |
| JC7      | GPCLK0      | GPIO4     | GPIO4     |                          |
| JC8      | PWM0        | GPIO12    | GPIO12    |                          |
| JC9      | GPCLK1      | GPIO5     | GPIO5     |                          |
| JC10     | GPCLK2      | GPIO6     | GPIO6     |                          |
| JC11     | GND         | --        | --        | Ground                   |
| JC12     | VCC         | --        | --        | 3.3V (DO NOT jumper VCC) |

### Unique GPIO connections (excluding shared SPI bus pins)

After de-duplicating the shared SPI0 lines (GPIO9, GPIO10, GPIO11), the complete
set of unique GPIO-to-GPIO connections between the two RPi devices is:

| BCM GPIO | Pmod Pin(s)   | Function                |
|----------|---------------|-------------------------|
| GPIO2    | JB10          | I2C1_SDA                |
| GPIO3    | JB9           | I2C1_SCL                |
| GPIO4    | JC7           | GPCLK0                  |
| GPIO5    | JC9           | GPCLK1                  |
| GPIO6    | JC10          | GPCLK2                  |
| GPIO7    | JB1           | SPI0_CE1                |
| GPIO8    | JA1           | SPI0_CE0                |
| GPIO9    | JA3, JB3      | SPI0_MISO               |
| GPIO10   | JA2, JB2      | SPI0_MOSI               |
| GPIO11   | JA4, JB4      | SPI0_SCLK               |
| GPIO12   | JC8           | PWM0                    |
| GPIO13   | JB8           | PWM1                    |
| GPIO14   | JC2           | UART0_TXD               |
| GPIO15   | JC3           | UART0_RXD               |
| GPIO16   | JC1           | UART0_CTS / SPI1_CE2    |
| GPIO17   | JC4           | UART0_RTS / SPI1_CE1    |
| GPIO18   | JA10          | PCM_CLK / PWM0 / SPI1_CE0 |
| GPIO19   | JA7           | PCM_FS / PWM1 / SPI1_MISO |
| GPIO20   | JA9           | PCM_DIN / SPI1_MOSI     |
| GPIO21   | JA8           | PCM_DOUT / SPI1_SCLK    |
| GPIO26   | JB7           | GPIO                    |

That is **21 unique GPIO connections** between the two RPi devices (or 24 Pmod
signal pins total, with 3 duplicated via the shared SPI0 bus).

---

# Verification

Run `verify_pmod_connections.py` from the local machine to verify all jumper cable
connections between the two RPi devices. The script tests each GPIO connection in
both directions (RPi5 → RPi4 and RPi4 → RPi5) by driving a pin high/low on one
side and reading it on the other.

```
uv run verify_pmod_connections.py
```

See `verify_pmod_connections.py` for details.
