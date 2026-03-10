# Frame Specification

## Overview

This document describes the communication frame format used for the WebSerial CAN-FD firmware. The frame protocol provides a structured way to transmit commands and data between the host (via USB) and the STM32G4 device.

## Frame Structure

Each frame consists of the following fields:

| Field         | Offset | Size (bytes) | Description                           |
|---------------|--------|--------------|---------------------------------------|
| TAG           | 0      | 1            | Start of Frame marker (0xFF)          |
| Length        | 1      | 2            | Total frame length (little-endian)    |
| Timestamp     | 3      | 4            | Timestamp in 10us resolution (little-endian) |
| Packet Seq    | 7      | 2            | Packet sequence number (little-endian)|
| Payload       | 9      | N            | Command and data payload              |
| Checksum      | 9+N    | 1            | Two's complement checksum             |

**Frame Overhead:** 10 bytes (excluding payload)

**Minimum Frame Size:** 10 bytes (TAG + Length + Timestamp + Packet Seq + Checksum, with no payload). This matches `FRAME_OVERHEAD` — the parser waits for at least 10 bytes before attempting to parse and rejects any declared length below 10.

**Maximum Frame Size:** 1023 bytes (limited by FRAME_RX_SIZE buffer)

## Field Descriptions

### TAG (Start of Frame)
- **Value:** `0xFF` (255 decimal)
- **Purpose:** Identifies the start of a new frame
- **Note:** The parser scans for this byte to synchronize with incoming frames

### Length
- **Format:** 16-bit unsigned integer, little-endian
- **Value:** Total number of bytes in the frame, including TAG, Length, and Checksum
- **Range:** 10 to 1023 bytes (enforced by `FRAME_OVERHEAD` constant)

### Timestamp
- **Format:** 32-bit unsigned integer, little-endian
- **Source:** Hardware timer (TIM2) counter value
- **Purpose:** Provides timing information for received CAN messages and events
- **Resolution:** Depends on TIM2 configuration

### Packet Sequence
- **Format:** 16-bit unsigned integer, little-endian
- **Purpose:** Sequential counter for tracking packets
- **Behavior:** Increments with each transmitted frame, wraps around at 65535
- **Note:** Only relevant for transmitted frames; received frames may contain any value

### Payload
- **Variable length:** 0 to N bytes
- **Contents:** Command byte followed by command-specific data
- **Format:** See [Payload Format](#payload-format) section

### Checksum
- **Algorithm:** Two's complement checksum
- **Calculation:** 
  1. Sum all bytes from TAG to the last payload byte
  2. Take the two's complement: `checksum = (~sum) + 1`
- **Verification:** Sum of all bytes including checksum should equal 0

## Payload Format

The first byte of the payload is always a command identifier:

### Command: Get Device ID (0x00)

**Request:**
```
Payload: 0x00
```

**Response:**
```
Payload[0]: 0x00 (CMD_GET_DEVICE_ID)
Payload[1]: 0xAC (Device ID)
Payload[2]: VERSION_MAJOR (firmware major version)
Payload[3]: VERSION_MINOR (firmware minor version)
Payload[4]: VERSION_PATCH (firmware patch version)
```


### Command: CAN Start (0x01)

Starts the FDCAN peripheral.

**Request:**
```
Payload[0]: 0x01 (CMD_CAN_START)
```

**Response:**
```
Payload[0]: 0x01 (CMD_CAN_START)
Payload[1]: Status (0 = HAL_OK, non-zero = HAL error code)
```

### Command: CAN Stop (0x02)

Stops the FDCAN peripheral.

**Request:**
```
Payload[0]: 0x02 (CMD_CAN_STOP)
```

**Response:**
```
Payload[0]: 0x02 (CMD_CAN_STOP)
Payload[1]: Status (0 = HAL_OK, non-zero = HAL error code)
```

### Command: Device Reset (0x03)

Performs a system reset via `NVIC_SystemReset()`. No response is sent; the device resets immediately.

**Request:**
```
Payload[0]: 0x03 (CMD_DEVICE_RESET)
```

**Response:** None (device resets immediately)

### Command: Send Downstream (0x10)

Transmits a CAN or CAN-FD frame to the bus.

**Request:**
```
Payload[0]: 0x10 (CMD_SEND_DOWNSTREAM)
Payload[1]: TX_TYPE (see TX_TYPE format below)
Payload[2-5]: Message ID (32-bit, little-endian)
Payload[6]: DLC (Data Length Code)
Payload[7..7+DLC-1]: CAN data bytes
```

**TX_TYPE Format (bit flags):**
- **Bit 0:** Frame format
  - `0` = CAN Classic (CAN-CC)
  - `1` = CAN-FD
- **Bit 1:** Bit Rate Switch (valid only for CAN-FD)
  - `0` = BRS ON (faster data phase)
  - `1` = BRS OFF
- **Bit 2:** Identifier type
  - `0` = Standard ID (11-bit)
  - `1` = Extended ID (29-bit)
- **Bits 3-7:** Reserved (set to 0)

**DLC Values:**
- CAN Classic: 0-8 bytes
- CAN-FD: 0-64 bytes (valid values: 0-8, 12, 16, 20, 24, 32, 48, 64)

**Response:**
```
Payload[0]: 0x10 (CMD_SEND_DOWNSTREAM)
Payload[1]: Status (0 = success, 1 = error)
```

**Error Conditions:**
- CAN Classic with DLC > 8
- CAN-FD with DLC > 64
- CAN Classic with BRS ON (invalid combination)

### Command: Send Upstream (0x11)

Transmits received CAN or CAN-FD frames from the bus to the host. This command is generated automatically by the device when a CAN message is received and processed by `CANRX_Process()`.

**Direction:** Device → Host (automatic notification)

**Message Format:**
```
Payload[0]: 0x11 (CMD_SEND_UPSTREAM)
Payload[1]: RX_TYPE (frame type and identifier format)
Payload[2-5]: Message ID (32-bit, little-endian)
Payload[6]: DLC (Data Length Code)
Payload[7..7+DLC-1]: CAN data bytes
```

**RX_TYPE Format (bit flags):**
- **Bit 0:** Frame format
  - `0` = CAN Classic (CAN-CC)
  - `1` = CAN-FD
- **Bit 1:** Bit Rate Switch
  - `0` = BRS ON (faster data phase)
  - `1` = BRS OFF
- **Bit 2:** Identifier type
  - `0` = Standard ID (11-bit)
  - `1` = Extended ID (29-bit)
- **Bits 3-7:** Reserved (set to 0)

**Processing Flow:**
1. `CANRX_Process()` polls the FDCAN RX FIFO in thread mode using `HAL_FDCAN_GetRxFifoFillLevel()`
2. When messages are available, retrieves them using `HAL_FDCAN_GetRxMessage()`
3. Converts HAL FDCAN header to RX_TYPE byte format
4. Converts HAL DLC constants (e.g., `FDCAN_DLC_BYTES_12`) to actual byte counts
5. Constructs frame payload with command, type, ID, DLC, and data
6. Sends frame upstream via `PARSER_SendFrame()`

**DLC Conversion:**
- HAL provides DLC as enumerated constants (`FDCAN_DLC_BYTES_0` through `FDCAN_DLC_BYTES_64`)
- Constants 0-8 map directly to byte values (0-8)
- Extended DLC values: 12, 16, 20, 24, 32, 48, 64 bytes for CAN-FD frames

**Note:** This command has no request message - it is only sent by the device as a notification when CAN messages are received.

### Command: Protocol Status (0x12)

Sent automatically by the device (via `CANErr_Process()`) whenever the FDCAN protocol status changes. This is a device-to-host unsolicited notification.

**Direction:** Device → Host (automatic notification)

**Message Format:**
```
Payload[0]: 0x12 (CMD_PROTOCOL_STATUS)
Payload[1]: LastErrorCode
Payload[2]: DataLastErrorCode
Payload[3]: Activity
Payload[4]: Flags byte
  bit0: ErrorPassive
  bit1: Warning
  bit2: BusOff
  bit3: RxESIflag
  bit4: RxBRSflag
  bit5: RxFDFflag
  bit6: ProtocolException
  bit7: Reserved
Payload[5]: TDCvalue
```

**Note:** This command has no request message. An initial status frame is sent once on the first call to `CANErr_Process()`, and subsequently whenever any monitored field changes.

### Command: Get CAN Stats (0x13)

Retrieves CAN error counters and frame loss statistics. This frame is sent both as a response to a host request and as an unsolicited notification from the device.

**Request:**
```
Payload[0]: 0x13 (CMD_GET_CAN_STATS)
```

**Response / Unsolicited Notification:**
```
Payload[0]:     0x13 (CMD_GET_CAN_STATS)
Payload[1-2]:   TxErrorCnt (uint16_t, little-endian)
Payload[3-4]:   TxErrorCntMax (uint16_t, little-endian)
Payload[5-6]:   RxErrorCnt (uint16_t, little-endian)
Payload[7-8]:   RxErrorCntMax (uint16_t, little-endian)
Payload[9-10]:  PassiveErrorCnt (uint16_t, little-endian)
Payload[11-12]: stat_downstream_packet_loss_cnt (uint16_t, little-endian)
Payload[13-14]: stat_upstream_packet_loss_cnt (uint16_t, little-endian)
Payload[15-16]: stat_rx_buffer_overflow_cnt (uint16_t, little-endian)
Payload[17]:    Status (0 = success)
```

**Unsolicited Notification Triggers (Device → Host):**
The device automatically sends a stats frame via `CAN_stat_send()` from `CANErr_Process()` whenever:
- A protocol status change is detected (`statusChanged`): error code change, ErrorPassive/Warning/BusOff/RxESIflag/ProtocolException transition
- A new error count maximum is reached (`maxValChanged`): `TxErrorCntMax` or `RxErrorCntMax` updated to a new high-water mark

### Command: Reset CAN Stats (0x14)

Resets all CAN error counters and frame loss statistics to zero.

**Request:**
```
Payload[0]: 0x14 (CMD_RESET_CAN_STATS)
```

**Response:**
```
Payload[0]: 0x14 (CMD_RESET_CAN_STATS)
Payload[1]: Status (0 = success)
```

### Command: Enter DFU (0xF0)

Triggers a reset into the STM32 ROM USB DFU bootloader. Upon receiving this command, the firmware writes a magic word to a reserved RAM location (`.noinit` section) and immediately calls `NVIC_SystemReset()`. On the next boot, `main()` detects the magic word before any peripheral initialisation and jumps to the factory ROM DFU bootloader at `0x1FFF0000`.

Once in DFU mode, the device re-enumerates on the USB host as a standard DFU device (`idVendor=0x0483`, `idProduct=0xDF11`) and new firmware can be flashed using `dfu-util` or STM32CubeProgrammer.

**Request:**
```
Payload[0]: 0xF0 (CMD_ENTER_DFU)
```

**Response:** None — the device resets immediately after setting the DFU flag.

**Host-side flashing example:**
```bash
# Using dfu-util
dfu-util -a 0 -s 0x08000000:leave -D firmware.bin

# Using STM32CubeProgrammer CLI
STM32_Programmer_CLI -c port=USB1 -d firmware.bin 0x08000000 -v -g 0x08000000
```

**Notes:**
- The magic word is cleared before the jump so the device boots normally after flashing.
- On Windows, the WinUSB driver must be installed for the DFU device (e.g., via [Zadig](https://zadig.akeo.ie/)).
- See [docs/DFU_IMPLEMENTATION.md](../docs/DFU_IMPLEMENTATION.md) for full implementation details.

## Frame Examples

### Example 1: Get Device ID Request

```
Byte:  [0]   [1]   [2]   [3]   [4]   [5]   [6]   [7]   [8]   [9]   [10]
Value: 0xFF  0x0B  0x00  0x00  0x00  0x00  0x00  0x00  0x00  0x00  0xF6
       TAG   LEN(L) LEN(H) TS    TS    TS    TS   SEQ(L) SEQ(H) CMD  CHKSUM
```

**Breakdown:**
- TAG: 0xFF
- Length: 11 bytes (0x000B)
- Timestamp: 0x00000000
- Packet Seq: 0x0000
- Command: 0x00 (Get Device ID)
- Checksum: 0xF6 (two's complement of byte sum 0x0A)

### Example 2: Send Standard CAN Frame (ID=0x123, Data=[0x11, 0x22])

```
TX_TYPE = 0x00 (CAN Classic, BRS OFF, Standard ID)
Message ID = 0x00000123
DLC = 2

Frame bytes:
[0]   [1]   [2]   [3]   [4]   [5]   [6]   [7]   [8]   [9]   [10]  [11]  [12]  [13]  [14]  [15]  [16]  [17]  [18]
0xFF  0x13  0x00  TS0   TS1   TS2   TS3   SEQ0  SEQ1  0x10  0x00  0x23  0x01  0x00  0x00  0x02  0x11  0x22  CHKSUM
TAG   LEN(L) LEN(H) TS    TS    TS    TS   SEQ(L) SEQ(H) CMD  TYPE  ID0   ID1   ID2   ID3   DLC  DATA0 DATA1 CHKSUM

TAG: 0xFF
Length: 19 bytes (0x0013)
Payload: 0x10 (CMD_SEND_DOWNSTREAM), 0x00 (TX_TYPE), 0x23 0x01 0x00 0x00 (ID), 0x02 (DLC), 0x11 0x22 (Data)
```

### Example 3: Send Extended CAN-FD Frame with BRS

```
TX_TYPE = 0x05 (CAN-FD, BRS ON, Extended ID)
           Binary: 0000 0101
           Bit 0: 1 (CAN-FD)
           Bit 1: 0 (BRS ON)
           Bit 2: 1 (Extended ID)

Message ID = 0x12345678
DLC = 12

Frame structure:
- TAG: 0xFF
- Length: 23 bytes
- Timestamp: Variable
- Packet Seq: Variable
- Payload[0]: 0x10 (CMD_SEND_DOWNSTREAM)
- Payload[1]: 0x05 (TX_TYPE)
- Payload[2-5]: 0x78 0x56 0x34 0x12 (Message ID, little-endian)
- Payload[6]: 0x0C (DLC = 12)
- Payload[7-18]: 12 data bytes
- Checksum: Calculated
```

## Frame Processing

### Receiving Frames

1. **Buffer Management:** Incoming bytes are stored in a circular buffer (FRAME_RX_SIZE = 1024 bytes)
2. **TAG Detection:** Parser scans for TAG byte (0xFF)
3. **Overhead Guard:** Wait until at least `FRAME_OVERHEAD` (10) bytes are available before reading the length field; break and wait for more data otherwise
4. **Length Validation:** Read length from bytes 1–2 and check it is within valid range (`FRAME_OVERHEAD` to 1023, i.e. 10 to 1023); skip TAG byte and keep scanning if invalid
5. **Buffer Check:** Verify entire frame (`length` bytes) is available in buffer; break and wait for more data otherwise
6. **Checksum Validation:** Sum all `length` bytes; discard frame (skip TAG, keep scanning) if sum ≠ 0
7. **Frame Processing:** Pass the validated frame to `_ProcessValidFrame()` and advance `rdPtr` by `length`

### Transmitting Frames

1. **Frame Assembly:** Application fills payload data
2. **Header Population:** TAG, Length, Timestamp, and Packet Sequence are added automatically
3. **Checksum Calculation:** Checksum is computed and appended
4. **Transmission:** Frame is written to USB TX ring buffer

## Implementation Notes

- **Endianness:** All multi-byte fields use little-endian byte order
- **Buffer Size:** RX buffer is 1024 bytes, TX buffer is 512 bytes
- **Thread Safety:** Frame parser uses volatile pointers for buffer management
- **Error Handling:** Invalid frames are discarded and parser continues scanning for next TAG
- **Wraparound:** Circular buffer handles wraparound automatically using modulo arithmetic

## References

- Implementation: [frameParser.c](Core/Src/frameParser.c)
- Header definitions: [frameParser.h](Core/Inc/frameParser.h)
- CAN interface: [canParser.h](Core/Inc/canParser.h)
