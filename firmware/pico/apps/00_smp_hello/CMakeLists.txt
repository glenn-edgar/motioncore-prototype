cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w CACHE STRING "Board type")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Platform")

include(pico_sdk_import.cmake)

if(NOT DEFINED ENV{FREERTOS_KERNEL_PATH})
    message(FATAL_ERROR "FREERTOS_KERNEL_PATH env var must point to the Raspberry Pi FreeRTOS-Kernel fork")
endif()
set(FREERTOS_KERNEL_PATH $ENV{FREERTOS_KERNEL_PATH})
include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake)

project(00_smp_hello C CXX ASM)
pico_sdk_init()

add_executable(00_smp_hello main.c)

target_include_directories(00_smp_hello PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(00_smp_hello
    pico_stdlib
    pico_async_context_freertos
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)

pico_enable_stdio_usb(00_smp_hello 1)
pico_enable_stdio_uart(00_smp_hello 0)
pico_add_extra_outputs(00_smp_hello)
