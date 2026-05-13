#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "s_engine_exception.h"


static void cfl_uint16_to_str(uint16_t value, char* buffer) {
    char temp[6];  // Max 5 digits + null
    int i = 0;
    
    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }
    
    while (value > 0) {
        temp[i++] = '0' + (value % 10);
        value /= 10;
    }
    
    // Reverse into output buffer
    int j;
    for (j = 0; j < i; j++) {
        buffer[j] = temp[i - 1 - j];
    }
    buffer[j] = '\0';
}
void cfl_exception_handler(const char* file, const char* func, uint16_t line, const char* msg) {
    char line_str[6];
    
    /* Disable interrupts - system is going down */
    //DISABLE_INTERRUPTS();
    
    /* Print exception info using puts (safer than printf if heap is corrupted) */
    puts("\n");
    puts("************************************");
    puts("***         EXCEPTION            ***");
    puts("************************************");
    
    puts("File: ");
    puts(file);
    
    puts("Func: ");
    puts(func);
    
    puts("Line: ");
    cfl_uint16_to_str(line, line_str);
    puts(line_str);
    
    puts("Msg:  ");
    puts(msg);
    
    puts("************************************");
    puts("*** Waiting for watchdog reset   ***");
    puts("************************************");
    //abort();
    /* Spin forever - watchdog will fire */
    while (1) {
        /* Optional: Toggle GPIO/LED for visual indication */
        __asm volatile ("nop");
    }
}