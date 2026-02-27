#include "frameParser.h"
#include "main.h"
#include "UTIL_ringbuf.h"
#include "canParser.h"

#define FRAME_RX_SIZE       (1024)
#define FRAME_TX_SIZE       (512)

extern FDCAN_HandleTypeDef hfdcan1;

volatile uint32_t rdPtr = 0;
volatile uint32_t wrPtr = 0;
static uint8_t rxFrameBuffer[FRAME_RX_SIZE];

extern tRingBufObject usbTxRb;
static uint16_t packetSeq = 0;

static void _ProcessValidFrame(const uint32_t index, uint32_t len)
{
    uint8_t responseBuffer[128];
    uint32_t respLen = 0;
    uint8_t cmd;
    uint32_t i;
    bool sts = false;
    uint8_t u8Param;
    uint32_t u32Param;

    /* Command */
    cmd = rxFrameBuffer[(index + PAYLOAD_OFFSET) % FRAME_RX_SIZE];

    switch(cmd) {
        case CMD_GET_DEVICE_ID: {
            respLen = 0;
            responseBuffer[PAYLOAD_OFFSET + respLen++] = CMD_GET_DEVICE_ID;
            responseBuffer[PAYLOAD_OFFSET + respLen++] = 0xAC;
            respLen += FRAME_OVERHEAD;
            PARSER_SendFrame(responseBuffer, respLen);
            break;
        }

        case CMD_CAN_START: {
            bool hasError = false;
            hasError = (HAL_OK != HAL_FDCAN_Start(&hfdcan1));

            respLen = 0;
            responseBuffer[PAYLOAD_OFFSET + respLen++] = CMD_CAN_START;
            responseBuffer[PAYLOAD_OFFSET + respLen++] = hasError ? 0x1 : 0x00;
            respLen += FRAME_OVERHEAD;
            PARSER_SendFrame(responseBuffer, respLen);
            break;
        }

        case CMD_CAN_STOP: {
            bool hasError = false;
            hasError = (HAL_OK != HAL_FDCAN_Stop(&hfdcan1));

            respLen = 0;
            responseBuffer[PAYLOAD_OFFSET + respLen++] = CMD_CAN_STOP;
            responseBuffer[PAYLOAD_OFFSET + respLen++] = hasError ? 0x1 : 0x00;
            respLen += FRAME_OVERHEAD;
            PARSER_SendFrame(responseBuffer, respLen);
            break;
        }

        case CMD_SEND_DOWNSTREAM: {
            /*
             * TX_TYPE
             *  bit0: 0 - CAN-CC
             *        1 - CAN-FD
             *
             *  bit1: 0 - BRS_ON (valid if bit0 is 1)
             *        1 - BRS_OFF
             *
             *  bit2: 0 - FDCAN_STANDARD_ID (11-bit identifier)
             *        1 - FDCAN_EXTENDED_ID (29-bit identifier)
             */
            const uint32_t FRAME_TX_TYPE_OFFSET = (index + PAYLOAD_OFFSET + 1) % FRAME_RX_SIZE;
            const uint32_t FRAME_TX_MSGID_OFFSET = (index + PAYLOAD_OFFSET + 2) % FRAME_RX_SIZE;
            const uint32_t FRAME_TX_DLC_OFFSET = (index + PAYLOAD_OFFSET + 6) % FRAME_RX_SIZE;
            const uint32_t FRAME_TX_DATA_OFFSET = (index + PAYLOAD_OFFSET + 7) % FRAME_RX_SIZE;

            CanTx_t canTx = {0};
            bool hasError = false;
            const uint8_t type = rxFrameBuffer[FRAME_TX_TYPE_OFFSET];
            const uint8_t dlc = rxFrameBuffer[FRAME_TX_DLC_OFFSET];
            uint32_t identifier = rxFrameBuffer[FRAME_TX_MSGID_OFFSET];
            identifier |= ((uint32_t)rxFrameBuffer[(FRAME_TX_MSGID_OFFSET + 1) % FRAME_RX_SIZE] << 8);
            identifier |= ((uint32_t)rxFrameBuffer[(FRAME_TX_MSGID_OFFSET + 2) % FRAME_RX_SIZE] << 16);
            identifier |= ((uint32_t)rxFrameBuffer[(FRAME_TX_MSGID_OFFSET + 3) % FRAME_RX_SIZE] << 24);

            canTx.header.Identifier = identifier;
            if((type & 0x4) == 0) {
                canTx.header.IdType = FDCAN_STANDARD_ID;  // 11-bit identifier
            } else {
                canTx.header.IdType = FDCAN_EXTENDED_ID;  // 29-bit identifier
            }
            canTx.header.TxFrameType = FDCAN_DATA_FRAME;
            canTx.header.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
            if((type & 0x1) == 0) {
                // CAN Classic
                canTx.header.FDFormat = FDCAN_CLASSIC_CAN;
                if((type & 0x2) == 0) {
                    hasError = true;  // CAN-CC doesn't support BRS
                } else {
                    canTx.header.BitRateSwitch = FDCAN_BRS_OFF;
                }

                if(dlc > 8) {
                    hasError = true;  // CAN-CC max DLC is 8
                } else {
                    canTx.header.DataLength = dlc;
                }
            } else {
                // FD
                canTx.header.FDFormat = FDCAN_FD_CAN;
                if((type & 0x2) == 0) {
                    canTx.header.BitRateSwitch = FDCAN_BRS_ON;
                } else {
                    canTx.header.BitRateSwitch = FDCAN_BRS_OFF;
                }

                if(dlc > 64) {
                    hasError = true; // CAN-FD max DLC is 64
                } else {
                    if(dlc <= 8) {
                        canTx.header.DataLength = dlc;
                    } else if(dlc <= 12) {
                        canTx.header.DataLength = FDCAN_DLC_BYTES_12;
                    } else if(dlc <= 16) {
                        canTx.header.DataLength = FDCAN_DLC_BYTES_16;
                    } else if(dlc <= 20) {
                        canTx.header.DataLength = FDCAN_DLC_BYTES_20;
                    } else if(dlc <= 24) {
                        canTx.header.DataLength = FDCAN_DLC_BYTES_24;
                    } else if(dlc <= 32) {
                        canTx.header.DataLength = FDCAN_DLC_BYTES_32;
                    } else if(dlc <= 48) {
                        canTx.header.DataLength = FDCAN_DLC_BYTES_48;
                    } else if(dlc <= 64) {
                        canTx.header.DataLength = FDCAN_DLC_BYTES_64;
                    } else {
                        hasError = true;;
                    }
                }
            }
            canTx.header.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
            canTx.header.MessageMarker = 0;
            if(hasError != true) {
                if(dlc > 0) {
                    for(uint32_t i = 0; i < dlc; i++) {
                        canTx.data[i] = rxFrameBuffer[(FRAME_TX_DATA_OFFSET + i) % FRAME_RX_SIZE];
                    }
                }
                if(CAN_Send(&canTx) != true) {
                    hasError = true;
                }
            }


            // Reply
            respLen = 0;
            responseBuffer[PAYLOAD_OFFSET + respLen++] = CMD_SEND_DOWNSTREAM;
            responseBuffer[PAYLOAD_OFFSET + respLen++] = hasError ? 1 : 0;
            respLen += FRAME_OVERHEAD;
            PARSER_SendFrame(responseBuffer, respLen);

            break;
        }
        default:
            break;
    }
}

void PARSER_Store(uint8_t *pBuf, uint32_t len)
{
    uint32_t i = 0;
    for(i = 0; i < len; i++) {
        rxFrameBuffer[wrPtr] = pBuf[i];
        wrPtr = (wrPtr + 1) % FRAME_RX_SIZE;
    }
}

void PARSER_Process()
{
    uint32_t availableBytes = 0;
    uint32_t length = 0;
    uint32_t idx = 0;
    uint8_t sum = 0;

    while(wrPtr != rdPtr) {
        /* Check start of command TAG */
        if(TAG_SOF != rxFrameBuffer[rdPtr])
        {
            // Skip character
            rdPtr = (rdPtr + 1) % FRAME_RX_SIZE;
            continue;
        }

        /* Get available bytes in the buffer */
        if(wrPtr >= rdPtr) {
            availableBytes = wrPtr - rdPtr;
        } else {
            availableBytes = (wrPtr + FRAME_RX_SIZE) - rdPtr;
        }
        if(availableBytes < 3) {
            /*
             * Minimum of three bytes to proceed
             * 1byte(TAG) + 2bytes(Length)
             */
            break;
        }
        // See if the packet size byte is valid.  A command packet must be at
        // least four bytes and can not be larger than the receive buffer size.
        length = (uint32_t)(rxFrameBuffer[(rdPtr+1)%FRAME_RX_SIZE]) +
                ((uint32_t)(rxFrameBuffer[(rdPtr+2)%FRAME_RX_SIZE]) << 8);

        if((length < 4) || (length > (FRAME_RX_SIZE-1)))
        {
            // The packet size is too small. Minimum packet size is 4 bytes
            // 1byte(TAG) + 2bytes(Length) + 1byte(Checksum)

            // The packet size is too large, so either this is not the start of
            // a packet or an invalid packet was received.  Skip this start of
            // command packet tag.
            rdPtr = (rdPtr + 1) % FRAME_RX_SIZE;

            // Keep scanning for a start of command packet tag.
            continue;
        }

        // If the entire command packet is not in the receive buffer then stop
        if(availableBytes < length)
        {
            break;
        }

        // The entire command packet is in the receive buffer, so compute its
        // checksum.
        for(idx = 0, sum = 0; idx < length; idx++)
        {
            sum += rxFrameBuffer[(rdPtr + idx)%FRAME_RX_SIZE];
        }

        // Skip this packet if the checksum is not correct (that is, it is
        // probably not really the start of a packet).
        if(sum != 0)
        {
            // Skip this character
            rdPtr = (rdPtr + 1) % FRAME_RX_SIZE;

            // Keep scanning for a start of command packet tag.
            continue;
        }

        // A valid command packet was received, so process it now.
        _ProcessValidFrame(rdPtr, length);

        // Done with processing this command packet.
        rdPtr = (rdPtr + length) % FRAME_RX_SIZE;
    }
}

void PARSER_SendFrame(uint8_t *pBuf, uint32_t len)
{
    uint32_t i;
    uint8_t sum = 0;
    /*
     * Start of Frame
     */
    pBuf[TAG_OFFSET] = TAG_SOF;
    /*
     * Set Length
     */
    pBuf[LEN_OFFSET] = (uint8_t)(len & 0xFF);
    pBuf[LEN_OFFSET + 1] = (uint8_t)((len >> 8) & 0xFF);
    /*
     * Set Timestamp
     */
    extern TIM_HandleTypeDef htim2;
    uint32_t timestamp = __HAL_TIM_GET_COUNTER(&htim2);
    pBuf[TIMESTAMP_OFFSET] = (uint8_t)(timestamp & 0xFF);
    pBuf[TIMESTAMP_OFFSET + 1] = (uint8_t)((timestamp >> 8) & 0xFF);
    pBuf[TIMESTAMP_OFFSET + 2] = (uint8_t)((timestamp >> 16) & 0xFF);
    pBuf[TIMESTAMP_OFFSET + 3] = (uint8_t)((timestamp >> 24) & 0xFF);

    /*
     * Set Packet Sequence
     */
    pBuf[PACKET_SEQ_OFFSET] = (uint8_t)(packetSeq & 0xFF);
    pBuf[PACKET_SEQ_OFFSET + 1] = (uint8_t)((packetSeq >> 8) & 0xFF);
    packetSeq++;

    /*
     * Calculate Checksum
     */
    for(i = 0; i < (len-1); i++) {
        sum += pBuf[i];
    }
    pBuf[i] = (uint8_t)((~sum) + 1);
    /*
     * Write to Tx Buffer
     */
    if(len <= UTIL_RingBufFree(&usbTxRb)) {
        UTIL_RingBufWrite(&usbTxRb, pBuf, len);
    }
}
