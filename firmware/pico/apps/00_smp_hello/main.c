#include <stdio.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

static void core_task(void *arg) {
    const char *name = pcTaskGetName(NULL);
    uint32_t counter = 0;
    for (;;) {
        UBaseType_t core = portGET_CORE_ID();
        printf("%s tick=%lu core=%u\n", name, (unsigned long)counter++, (unsigned)core);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void vApplicationMallocFailedHook(void) { for(;;); }

void vApplicationGetIdleTaskMemory(StaticTask_t **ppxTaskTCB,
                                   StackType_t **ppxTaskStack,
                                   uint32_t *pulStackSize) {
    static StaticTask_t tcb;
    static StackType_t stack[configMINIMAL_STACK_SIZE];
    *ppxTaskTCB = &tcb;
    *ppxTaskStack = stack;
    *pulStackSize = configMINIMAL_STACK_SIZE;
}

void vApplicationGetPassiveIdleTaskMemory(StaticTask_t **ppxTaskTCB,
                                          StackType_t **ppxTaskStack,
                                          uint32_t *pulStackSize,
                                          BaseType_t xCoreID) {
    static StaticTask_t tcb[configNUMBER_OF_CORES - 1];
    static StackType_t stack[configNUMBER_OF_CORES - 1][configMINIMAL_STACK_SIZE];
    *ppxTaskTCB = &tcb[xCoreID];
    *ppxTaskStack = stack[xCoreID];
    *pulStackSize = configMINIMAL_STACK_SIZE;
}

void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTaskTCB,
                                    StackType_t **ppxTaskStack,
                                    uint32_t *pulStackSize) {
    static StaticTask_t tcb;
    static StackType_t stack[configTIMER_TASK_STACK_DEPTH];
    *ppxTaskTCB = &tcb;
    *ppxTaskStack = stack;
    *pulStackSize = configTIMER_TASK_STACK_DEPTH;
}

int main(void) {
    stdio_init_all();

    TaskHandle_t t0, t1;
    xTaskCreate(core_task, "core0", configMINIMAL_STACK_SIZE * 2, NULL, tskIDLE_PRIORITY + 1, &t0);
    xTaskCreate(core_task, "core1", configMINIMAL_STACK_SIZE * 2, NULL, tskIDLE_PRIORITY + 1, &t1);
    vTaskCoreAffinitySet(t0, 1u << 0);
    vTaskCoreAffinitySet(t1, 1u << 1);

    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}
