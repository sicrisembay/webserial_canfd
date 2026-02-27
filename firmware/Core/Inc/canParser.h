/*
 * canParser.h
 *
 *  Created on: Feb 26, 2026
 *      Author: Sicris
 */

#ifndef INC_CANPARSER_H_
#define INC_CANPARSER_H_

#define CONFIG_CANFD_DATA_SIZE      (64)

typedef struct {
    FDCAN_TxHeaderTypeDef header;
    uint8_t data[CONFIG_CANFD_DATA_SIZE];
} CanTx_t;

bool CAN_Send(CanTx_t * pCanTx);
void CANTX_Process(void);
void CANRX_Process(void);

#endif /* INC_CANPARSER_H_ */
