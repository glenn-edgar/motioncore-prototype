#ifndef S_ENGINE_EXCEPTION_H
#define S_ENGINE_EXCEPTION_H
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>

/* Exception macro - captures file, function, line, and message */
#define EXCEPTION(msg) \
    cfl_exception_handler(__FILE__, __func__, __LINE__, (msg))


/* Exception handler - prints error and spins until watchdog fires */
void cfl_exception_handler(const char* file, const char* func, uint16_t line, const char* msg);

#ifdef __cplusplus
}
#endif
#endif 