/*
广播写例子
*/

#include "stm32f10x.h"
#include "SCServo.h"
#include "uart.h"
#include "wiring.h"

void setup(void)
{
	Uart_Init(115200);
  delay(1000);
}

void loop(void)
{
  WritePosEx(0xfe, 4095, 2250, 50);//舵机(广播),以最高速度V=2250步/秒,加速度A=50(50*100步/秒^2),运行至P1=4095
  delay(2270);//[(P1-P0)/V]*1000+[V/(A*100)]*1000
	
  WritePosEx(0xfe, 0, 2250, 50);//舵机(广播),以最高速度V=2250步/秒,加速度A=50(50*100步/秒^2),运行至P1=0
  delay(2270);//[(P1-P0)/V]*1000+[V/(A*100)]*1000
}
