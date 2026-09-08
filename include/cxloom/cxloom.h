#pragma once

#include "cxloom/cxloom_mem.h"

#ifdef __cplusplus
#include "cxloom/loommem.h"
#endif

// Public CXLoom umbrella header. The cl_pthread_* APIs expose only pthread-like
// lifecycle operations; placement, queues and execution-host decisions remain
// internal to LoomPar.
