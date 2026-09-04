#pragma once

#include "cxloom/cxloom_mem.h"

#ifdef __cplusplus
#include "cxloom/loommem.h"
#endif

// LoomPar's cl_thread_* API will be added here as its distributed execution
// paths become functional. Keeping this umbrella header stable lets C users
// include one public CXLoom interface from the start.
