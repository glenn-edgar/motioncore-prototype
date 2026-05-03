#ifndef		_UART_H
#define		_UART_H

#include <stdint.h>
extern void Uart_Init(uint32_t baudRate);
extern void Uart_Flush(void);
extern int16_t Uart_Read(void);
extern void Uart_Send(uint8_t *buf , uint8_t len);

#endif
