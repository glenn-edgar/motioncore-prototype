/*
同步写例子
*/

#include "stm32f10x.h"
#include "SCServo.h"
#include "uart.h"
#include "wiring.h"

uint8_t ID[2];
int16_t Position[2];
uint16_t Speed[2];
uint8_t ACC[2];

void setup(void)
{
	Uart_Init(115200);
  delay(1000);
  ID[0] = 1;
  ID[1] = 2;
}

void loop(void)
{
	Speed[0] = 1500;
	Speed[1] = 1500;
	ACC[0] = 50;
	ACC[1] = 50;
	Position[0] = 4095;
  Position[1] = 4095;
  SyncWritePosEx(ID, 2, Position, Speed, ACC);//舵机(ID1/ID2),以最高速度V=1500步/秒,,加速度A=50(50*100步/秒^2),运行至P1=4095
  delay(2270);//((P1-P0)/V)*1000+(V/(A*100))*1000
	
  Position[0] = 0;
  Position[1] = 0;
  SyncWritePosEx(ID, 2, Position, Speed, ACC);//舵机(ID1/ID2),以最高速度V=1500步/秒,加速度A=50(50*100步/秒^2),运行至P1=0
  delay(2270);//((P1-P0)/V)*1000+(V/(A*100))*1000
}
