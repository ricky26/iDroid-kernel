#ifndef FTL_YAFTL_H
#define FTL_YAFTL_H

#include "../ftl.h"
#include "../vfl.h"
#include "yaftl_common.h"

typedef struct _ftl_yaftl_device {
	ftl_device_t ftl;
} ftl_yaftl_device_t;

ftl_yaftl_device_t *ftl_yaftl_device_allocate(void);

#endif // FTL_YAFTL_H
