/*
 * canParser.c
 *
 *  Created on: Feb 26, 2026
 *      Author: Sicris
 */

#include "string.h"
#include "main.h"
#include "canParser.h"
#include "frameParser.h"

#define CANTX_Q_SIZE    (8)

volatile uint32_t canTxRdPtr = 0;
volatile uint32_t canTxWrPtr = 0;
static CanTx_t canTxSto[CANTX_Q_SIZE];

extern FDCAN_HandleTypeDef hfdcan1;

static bool CAN_txQ_full()
{
    return (((canTxWrPtr + 1) % CANTX_Q_SIZE) == canTxRdPtr);
}

static bool CAN_txQ_empty()
{
    return (canTxWrPtr == canTxRdPtr);
}

bool CAN_Send(CanTx_t * pCanTx)
{
    if(pCanTx == (CanTx_t *)0) {
        return false;
    }

    if(CAN_txQ_full()) {
        return false;
    }

    uint32_t isrContext = __get_IPSR() & 0x3F;
    /* Enter Critical Section */
    if(isrContext == 0) {
        __disable_irq();
    }

    memcpy(&canTxSto[canTxWrPtr], pCanTx, sizeof(CanTx_t));
    canTxWrPtr = (canTxWrPtr + 1) % CANTX_Q_SIZE;

    /* Exit Critical Section */
    if(isrContext == 0) {
        __enable_irq();
    }

    return true;
}


void CANTX_Process(void)
{
    // Note: To avoid data race condition, this function is only
    // allowed to be called in Thread mode

    if ((__get_IPSR() & 0x3F) != 0) {
        // Not in thread mode
        Error_Handler();
    }

    if(hfdcan1.State == HAL_FDCAN_STATE_BUSY) {
        if(HAL_FDCAN_GetTxFifoFreeLevel(&hfdcan1) > 0) {
            if(!CAN_txQ_empty()) {
                CanTx_t canTx;

                /* Enter Critical Section */
                __disable_irq();

                memcpy(&canTx, &canTxSto[canTxRdPtr], sizeof(CanTx_t));
                canTxRdPtr = (canTxRdPtr + 1) % CANTX_Q_SIZE;

                /* Exit Critical Section */
                __enable_irq();

                HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &(canTx.header), canTx.data);
            }
        }
    }
}


void CANRX_Process(void)
{
    // Note: To avoid data race condition, this function is only
    // allowed to be called in Thread mode

    if ((__get_IPSR() & 0x3F) != 0) {
        // Not in thread mode
        Error_Handler();
    }

    if(hfdcan1.State == HAL_FDCAN_STATE_BUSY) {
        while(HAL_FDCAN_GetRxFifoFillLevel(&hfdcan1, FDCAN_RX_FIFO0) > 0) {
            FDCAN_RxHeaderTypeDef rxHeader;
            uint8_t rxData[CONFIG_CANFD_DATA_SIZE];

            if(HAL_FDCAN_GetRxMessage(&hfdcan1, FDCAN_RX_FIFO0, &rxHeader, rxData) == HAL_OK) {
                // Process received message
                /*
                 * RX_TYPE
                 *  bit0: 0 - CAN-CC
                 *        1 - CAN-FD
                 *
                 *  bit1: 0 - BRS_ON
                 *        1 - BRS_OFF
                 *
                 *  bit2: 0 - FDCAN_STANDARD_ID (11-bit identifier)
                 *        1 - FDCAN_EXTENDED_ID (29-bit identifier)
                 */
                uint8_t type = 0;
                if(rxHeader.FDFormat == FDCAN_FD_CAN) {
                    type |= 0x1;
                }
                if(rxHeader.BitRateSwitch == FDCAN_BRS_OFF) {
                    type |= 0x2;
                }
                if(rxHeader.IdType == FDCAN_EXTENDED_ID) {
                	type |= 0x4;
                }

                uint8_t dlc = 0;
                switch(rxHeader.DataLength) {
                    case FDCAN_DLC_BYTES_0:
                    case FDCAN_DLC_BYTES_1:
                    case FDCAN_DLC_BYTES_2:
                    case FDCAN_DLC_BYTES_3:
                    case FDCAN_DLC_BYTES_4:
                    case FDCAN_DLC_BYTES_5:
                    case FDCAN_DLC_BYTES_6:
                    case FDCAN_DLC_BYTES_7:
                    case FDCAN_DLC_BYTES_8:
                        dlc = (uint8_t)rxHeader.DataLength;
                        break;
                    case FDCAN_DLC_BYTES_12:
                        dlc = 12;
                        break;
                    case FDCAN_DLC_BYTES_16:
                        dlc = 16;
                        break;
                    case FDCAN_DLC_BYTES_20:
                        dlc = 20;
                        break;
                    case FDCAN_DLC_BYTES_24:
                        dlc = 24;
                        break;
                    case FDCAN_DLC_BYTES_32:
                        dlc = 32;
                        break;
                    case FDCAN_DLC_BYTES_48:
                        dlc = 48;
                        break;
                    case FDCAN_DLC_BYTES_64:
                        dlc = 64;
                        break;
                    default: {
                        dlc = 0;
                        break;
                    }
                }
                // Send upstream via CMD_SEND_UPSTREAM (0x11)
                const uint32_t FRAME_CMD_OFFSET = PAYLOAD_OFFSET;
                const uint32_t FRAME_TYPE_OFFSET = PAYLOAD_OFFSET + 1;
                const uint32_t FRAME_MSGID_OFFSET = PAYLOAD_OFFSET + 2;
                const uint32_t FRAME_DLC_OFFSET = PAYLOAD_OFFSET + 6;
                const uint32_t FRAME_DATA_OFFSET = PAYLOAD_OFFSET + 7;

                uint8_t sendBuffer[128];
                uint32_t length = 0;
                sendBuffer[FRAME_CMD_OFFSET] = CMD_SEND_UPSTREAM;
                length += 1;

                sendBuffer[FRAME_TYPE_OFFSET] = type;
                length += 1;

                sendBuffer[FRAME_MSGID_OFFSET] = (uint8_t)(rxHeader.Identifier & 0xFF);
                sendBuffer[FRAME_MSGID_OFFSET + 1] = (uint8_t)((rxHeader.Identifier >> 8) & 0xFF);
                sendBuffer[FRAME_MSGID_OFFSET + 2] = (uint8_t)((rxHeader.Identifier >> 16) & 0xFF);
                sendBuffer[FRAME_MSGID_OFFSET + 3] = (uint8_t)((rxHeader.Identifier >> 24) & 0xFF);
                length += 4;

                sendBuffer[FRAME_DLC_OFFSET] = dlc;
                length += 1;

                if(dlc > 0) {
                    memcpy(&sendBuffer[FRAME_DATA_OFFSET], rxData, dlc);
                    length += dlc;
                }

                length += FRAME_OVERHEAD;
                PARSER_SendFrame(sendBuffer, length);
            }
        }
    }
}
