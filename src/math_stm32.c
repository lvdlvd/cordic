/*
 * math_stm32.c — device backend: drives the STM32G4 CORDIC coprocessor.
 *
 * Uses zero-overhead mode (RM0440 §17.3.6): CSR is written, arguments go
 * to WDATA, and the RDATA read inserts AHB wait states until the result
 * is ready — no polling, no interrupts.
 *
 * PRECONDITION: the application must enable the CORDIC peripheral clock
 * (RCC AHB1 enable, CORDICEN) before calling any function in this
 * library. The library performs no clock or reset management itself.
 *
 * Concurrency: the CORDIC is a global resource. Per the project's runtime
 * guarantee (single thread, no floating point in IRQ handlers) no locking
 * is performed here. If that guarantee ever changes, wrap
 * cordic_backend_run in a critical section.
 *
 * Register definitions match the project's peripheral header style and
 * are kept self-contained so this file has no external dependencies.
 */

#include <stdint.h>
#include "cordic_port.h"

#define __IO volatile
#define __I  volatile const

struct cm_cordic_regs {
    __IO uint32_t CSR;     /* 0x00 control/status   */
    __IO uint32_t WDATA;   /* 0x04 argument(s), w/o */
    __I  uint32_t RDATA;   /* 0x08 result(s),   r/o */
};

#define CM_CORDIC   ((struct cm_cordic_regs *)0x40020C00u)

void cordic_backend_init(void)
{
    /* Intentionally empty: peripheral clock enable is the application's
     * responsibility (see PRECONDITION above). */
}

void cordic_backend_run(uint32_t csr, const int32_t args[2], int32_t res[2])
{
    CM_CORDIC->CSR = csr;
    CM_CORDIC->WDATA = (uint32_t)args[0];
    if (csr & CM_CSR_NARGS)
        CM_CORDIC->WDATA = (uint32_t)args[1];

    /* read stalls until the calculation completes */
    res[0] = (int32_t)CM_CORDIC->RDATA;
    if (csr & CM_CSR_NRES)
        res[1] = (int32_t)CM_CORDIC->RDATA;
}
