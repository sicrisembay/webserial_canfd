#ifndef FRAME_PARSER_H
#define FRAME_PARSER_H

#include "stdint.h"

/*
 * Frame Format
 *   TAG        : 1 byte
 *   Length     : 2 bytes
 *   Timestamp  : 4 bytes
 *   Packet Seq : 2 bytes
 *   Payload    : N Bytes
 *   Checksum   : 1 byte
 */
#define TAG_SOF                 (0xff)  //!< The value of the tag byte for a command packet.

#define TAG_OFFSET              (0)
#define LEN_OFFSET              (1)
#define TIMESTAMP_OFFSET        (3)
#define PACKET_SEQ_OFFSET       (7)
#define PAYLOAD_OFFSET          (9)
#define FRAME_OVERHEAD          (10)   /* 10 bytes */

/*
 * Payload Format
 *
 */
#define CMD_GET_DEVICE_ID       (0x00)
#define CMD_CAN_START           (0x01)
#define CMD_CAN_STOP            (0x02)
#define CMD_SEND_DOWNSTREAM     (0x10)
#define CMD_SEND_UPSTREAM       (0x11)

void PARSER_Store(uint8_t *pBuf, uint32_t len);
void PARSER_Process();
void PARSER_GetTxBlock(uint8_t * pBuf, uint32_t * pSize);
void PARSER_SendFrame(uint8_t *pBuf, uint32_t len);

#endif /* FRAME_PARSER_H */
