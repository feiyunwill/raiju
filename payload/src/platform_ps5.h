#ifndef RAIJU_PLATFORM_PS5_H
#define RAIJU_PLATFORM_PS5_H

#include "raiju/platform.h"

/**
 * The console implementation of the platform seam: notifications through the
 * system's toast service, disk usage through statfs, the LAN address from the
 * interface list, the firmware from the kernel, randomness from the libc's
 * CSPRNG, the engine started in-process, and the CA bundle at
 * /data/raiju/ca.pem. Escalation is a no-op: #2 and #46 measured that the
 * loader's context already writes the data root (ADR 0006).
 */
const raiju_platform *raiju_platform_ps5(void);

#endif
