#ifndef _S2E_BASE_INSTRUCTIONS_H
#define _S2E_BASE_INSTRUCTIONS_H

#define OPCODE_TYPE long
#define OPCODE_ENCODE(x) (0xff000000 | ((x & 0xff) << 16))

#include "arm-common.h"


#endif /* _S2E_BASE_INSTRUCTIONS_H */
