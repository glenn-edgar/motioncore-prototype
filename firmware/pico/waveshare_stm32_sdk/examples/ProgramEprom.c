/*
舵机参数编程
*/

#include "stm32f10x.h"
#include "SCServo.h"
#include "uart.h"
#include "wiring.h"

void setup()
{
	Uart_Init(115200);
	delay(1000);
	unLockEprom(1);//打开EPROM保存功能
  writeByte(1, SMS_STS_ID, 2);//ID
	LockEprom(2);//关闭EPROM保存功能
}

void loop()
{

}
