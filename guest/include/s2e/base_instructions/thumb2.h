#ifndef _S2E_BASE_INSTRUCTIONS_H
#define _S2E_BASE_INSTRUCTIONS_H

#define OPCODE_TYPE long
#define OPCODE_ENCODE(x) (0xa000f7f0 | ((x & 0xff) << 16))

#include "arm-common.h"

#endif /* _S2E_BASE_INSTRUCTIONS_H */
