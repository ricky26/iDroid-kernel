#include <linux/slab.h>
#include "yaftl_mem.h"

#define ROUND_UP(a, b) ((((a) + (b) - 1)/(b)) * (b))

void* yaftl_alloc(size_t size)
{
#ifdef YUSTAS_FIXME
	void* buffer = memalign(0x40, size);
#else
	void* buffer = kzalloc(size, GFP_KERNEL);
#endif

	if (!buffer)
		printk(KERN_ERR "PANIC!!!: yaftl_alloc failed\r\n");

#ifdef YUSTAS_FIXME
	memset(buffer, 0, size);
#endif
	return buffer;
}

void bufzone_init(bufzone_t* _zone)
{
	_zone->buffer = 0;
	_zone->endOfBuffer = 0;
	_zone->size = 0;
	_zone->numAllocs = 0;
	_zone->numRebases = 0;
	_zone->paddingsSize = 0;
	_zone->state = 1;
}

// returns the sub-buffer offset
void* bufzone_alloc(bufzone_t* _zone, size_t size)
{
	size_t oldSizeRounded;

	if (_zone->state != 1)
		printk(KERN_ERR "PANIC!!!: bufzone_alloc: bad state\r\n");

	oldSizeRounded = ROUND_UP(_zone->size, 64);
	_zone->paddingsSize = _zone->paddingsSize + (oldSizeRounded - _zone->size);
	_zone->size = oldSizeRounded + size;
	_zone->numAllocs++;

	return (void*)oldSizeRounded;
}

error_t bufzone_finished_allocs(bufzone_t* _zone)
{
	uint8_t* buff;

	if (_zone->state != 1) {
		printk(KERN_ERR "bufzone_finished_allocs: bad state\n");
		return EINVAL;
	}

	_zone->size = ROUND_UP(_zone->size, 64);
		printk(KERN_ERR "%s:%d size: %d\n", __FUNCTION__, __LINE__, _zone->size);
	buff = yaftl_alloc(_zone->size);

	if (!buff) {
		printk(KERN_ERR "bufzone_finished_alloc: No buffer.\n");
		return EINVAL;
	}

	_zone->buffer = (uint32_t)buff;
	_zone->endOfBuffer = (uint32_t)(buff + _zone->size);
	_zone->state = 2;

	return 0;
}

void* bufzone_rebase(bufzone_t* _zone, void* buf)
{
	_zone->numRebases++;
	return buf + _zone->buffer;
}

error_t bufzone_finished_rebases(bufzone_t* _zone)
{
	if (_zone->numAllocs != _zone->numRebases) {
		printk(KERN_ERR "WMR_BufZone_FinishedRebases: _zone->numAllocs != _zone->numRebases\r\n");
		return EINVAL;
	}

	return 0;
}
