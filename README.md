# WebSerial CAN-FD Bridge

A USB-to-CAN/CAN-FD bridge firmware for STM32G431C8TX microcontroller that enables communication with CAN and CAN-FD buses via a USB CDC (Virtual COM Port) interface.

## Overview

This project implements a bidirectional bridge between USB and CAN/CAN-FD networks. It allows host applications to send and receive CAN frames through a simple USB serial interface, making it ideal for CAN bus monitoring, diagnostics, and development.

### Key Features

- **USB CDC Virtual COM Port** - Appears as a standard serial port on the host system
- **CAN and CAN-FD Support** - Full support for both CAN Classic and CAN-FD frames
- **Bit Rate Switch (BRS)** - CAN-FD frames with faster data phase transmission
- **Extended Frame Support** - Both Standard (11-bit) and Extended (29-bit) identifiers
- **Frame Protocol** - Structured communication protocol with timestamp and sequence tracking
- **Error Handling** - CAN error monitoring and statistics reporting
- **Real-time Operation** - Ring buffer architecture for efficient data handling

## Hardware

- **Microcontroller:** STM32G431C8TX
  - ARM Cortex-M4 core with FPU
  - 128KB Flash, 32KB SRAM
  - FDCAN peripheral with CAN-FD support
  - USB 2.0 Full-speed device interface

- **Peripherals Used:**
  - FDCAN1 - CAN/CAN-FD interface
  - USB Device (CDC class)
  - TIM2 - Timestamping timer

## Project Structure

```
webserial_canfd/
├── firmware/                    # STM32 firmware source code
│   ├── Core/
│   │   ├── Inc/                # Header files
│   │   │   ├── canParser.h     # CAN message handling
│   │   │   ├── frameParser.h   # Frame protocol parser
│   │   │   ├── main.h
│   │   │   └── UTIL_ringbuf.h  # Ring buffer utilities
│   │   └── Src/                # Source files
│   │       ├── canParser.c
│   │       ├── frameParser.c
│   │       ├── main.c
│   │       └── UTIL_ringbuf.c
│   ├── USB_Device/             # USB CDC implementation
│   │   ├── App/
│   │   └── Target/
│   ├── Drivers/                # STM32 HAL and CMSIS drivers
│   ├── Middlewares/            # STM32 USB Device Library
│   ├── FRAME_SPECIFICATION.md  # Detailed protocol documentation
│   └── webserial_canfd.ioc    # STM32CubeMX configuration
├── docs/                       # Additional documentation
└── README.md                   # This file
```

## Communication Protocol

The firmware uses a custom frame-based protocol for communication over USB. Each frame consists of:

| Field      | Size    | Description                              |
|------------|---------|------------------------------------------|
| TAG        | 1 byte  | Start of frame marker (0xFF)             |
| Length     | 2 bytes | Total frame length (little-endian)       |
| Timestamp  | 4 bytes | 10μs resolution timestamp                |
| Packet Seq | 2 bytes | Sequential frame counter                 |
| Payload    | N bytes | Command and data                         |
| Checksum   | 1 byte  | Two's complement checksum                |

**Frame overhead:** 10 bytes  
**Maximum frame size:** 1023 bytes  
**Maximum CAN-FD data:** 64 bytes

### Supported Commands

| Command | ID   | Description                           |
|---------|------|---------------------------------------|
| GET_DEVICE_ID | 0x00 | Query device identifier          |
| CAN_START     | 0x01 | Start CAN controller             |
| CAN_STOP      | 0x02 | Stop CAN controller              |
| DEVICE_RESET  | 0x03 | Reset device                     |
| SEND_DOWNSTREAM | 0x10 | Transmit CAN frame to bus      |
| SEND_UPSTREAM  | 0x11 | Received CAN frame (from bus)   |
| PROTOCOL_STATUS | 0x12 | Get protocol status            |
| GET_CAN_STATS  | 0x13 | Query CAN error statistics      |
| RESET_CAN_STATS | 0x14 | Clear CAN error counters       |

For detailed protocol specifications, see [FRAME_SPECIFICATION.md](firmware/FRAME_SPECIFICATION.md).

## Building the Firmware

### Prerequisites

- **STM32CubeIDE** or **ARM GCC toolchain**
- **STM32CubeMX** (optional, for hardware configuration)
- **ST-LINK** programmer/debugger

### Build Steps

#### Using STM32CubeIDE

1. Open STM32CubeIDE
2. Import the project: `File` → `Open Projects from File System`
3. Select the `firmware` directory
4. Build the project: `Project` → `Build All`
5. The output binary will be in `Debug/webserial_canfd.elf`

#### Using Command Line (Make)

```bash
cd firmware/Debug
make all
```

### Flashing

1. Connect ST-LINK to the STM32G431C8TX
2. Flash using STM32CubeProgrammer or from STM32CubeIDE:
   - `Run` → `Debug` or `Run` → `Run`

## Usage

### Connection

1. Flash the firmware to the STM32G431 board
2. Connect the CAN/CAN-FD bus to the FDCAN1 pins (typically PA11/PA12)
3. Connect the USB cable to your computer
4. The device will enumerate as a Virtual COM Port

### Communication Example

To send a standard CAN frame (ID=0x123) with 2 data bytes:

```
Frame structure:
[TAG] [LEN_L] [LEN_H] [TS0-3] [SEQ0-1] [CMD] [TYPE] [ID0-3] [DLC] [DATA...] [CHKSUM]
0xFF  0x11    0x00    ...     ...      0x10  0x00   0x23... 0x02  0x11 0x22  [calc]
```

### Host Integration

The USB CDC interface can be accessed using standard serial port libraries:
- **Python:** `pyserial`
- **JavaScript:** `Web Serial API` (browser-based)
- **C/C++:** `libserial`, platform-specific APIs
- **Any language:** Standard COM port/tty device access

## CAN Bus Configuration

CAN timing and configuration parameters are set in [webserial_canfd.ioc](firmware/webserial_canfd.ioc) and can be modified using STM32CubeMX:

- **Nominal Bit Rate:** Configurable (typically 500 kbps for CAN, 1 Mbps for CAN-FD)
- **Data Bit Rate:** Configurable for CAN-FD (typically 2-5 Mbps)
- **Sample Point:** Adjustable
- **Filters:** Configurable receive filters

## Error Handling

The firmware tracks CAN bus errors and maintains statistics:
- **TX Error Counter** - Transmission errors
- **RX Error Counter** - Reception errors  
- **Passive Error Counter** - Error passive state occurrences

Error statistics can be queried using the `GET_CAN_STATS` command.

## Development

### Architecture

The firmware uses a modular polling-based architecture:

```
main loop:
  ├─ CDC_ProcessTx()    - USB transmission
  ├─ PARSER_Process()   - Frame parsing and command dispatch
  ├─ CANTX_Process()    - CAN transmission queue
  ├─ CANRX_Process()    - CAN reception and forwarding
  └─ CANErr_Process()   - Error monitoring
```

### Key Components

- **frameParser.c** - Implements the frame protocol parser and command dispatcher
- **canParser.c** - Handles CAN message transmission, reception, and error management
- **UTIL_ringbuf.c** - Efficient circular buffer for USB/CAN data queuing
- **usbd_cdc_if.c** - USB CDC interface implementation

## License

This project uses STMicroelectronics software components that are licensed under their respective licenses. See individual source files for license information.

The STM32 HAL drivers and USB middleware are provided by STMicroelectronics under permissive licenses.

## Author

Created by Sicris, February 2026

## Contributing

Contributions are welcome! Please ensure:
- Code follows the existing style
- Changes are tested on hardware
- Documentation is updated accordingly

## Related Projects

This firmware is designed to work with WebSerial-based host applications that can communicate directly with USB CDC devices from a web browser using the Web Serial API.

## Support

For issues, questions, or feature requests, please open an issue in the project repository.
