#ifndef  VFL_VSVFL_H
#define  VFL_VSVFL_H

#include "vfl.h"
#include <linux/apple_flash.h>

/**
 * @file
 *
 * This file is where the VSVFL VFL implementation is defined.
 *
 * VSVFL is the newer VFL implementation, which caters to
 * newer devices using VSVFL on their NAND.
 *
 * This includes devices from the iPhone 3GS onwards.
 */

typedef struct _vfl_vsvfl_geometry
{
	uint16_t pages_per_block;
	uint16_t pages_per_block_2;
	uint16_t pages_per_sublk;
	uint32_t pages_total;
	uint32_t some_page_mask;
	uint32_t some_sublk_mask;
	uint16_t blocks_per_ce;
	uint16_t bytes_per_page;
#ifdef YUSTAS_FIXME
	uint32_t num_ecc_bytes;
#endif
	uint32_t bytes_per_spare;
#ifdef YUSTAS_FIXME
	uint32_t one;
#endif
	uint16_t num_ce;
#ifdef YUSTAS_FIXME
	uint16_t ecc_bits;
#endif
	uint16_t reserved_blocks;
	uint16_t vfl_blocks;
	uint16_t some_crazy_val;
	uint16_t fs_start_block;
	uint32_t unk;
	uint32_t banks_per_ce;
	uint16_t banks_total;
	uint16_t bank_address_space;
	uint32_t blocks_per_bank;
	uint32_t blocks_per_bank_vfl;
} vfl_vsvfl_geometry_t;

struct _vfl_vsvfl_context;
struct _vfl_vsvfl_device;

typedef struct VSVFLStats {
	uint64_t counter0;
	uint64_t counter1;
	uint64_t counter2;
	uint64_t counter3;
	uint64_t counter4;
	uint64_t counter5;
	uint64_t counter6;
	uint64_t counter7;
	uint64_t counter8;
	uint64_t counter9;
	uint64_t counter10;
	uint64_t counter11;
} __attribute__((packed)) VSVFLStats;

// VSVFL conversion functions prototypes
typedef void (*vsvfl_virtual_to_physical_t)(struct _vfl_vsvfl_device *_vfl, uint32_t _vBank, uint32_t _vPage, uint32_t *_pCE, uint32_t *_pPage);

typedef void (*vsvfl_physical_to_virtual_t)(struct _vfl_vsvfl_device *_vfl, uint32_t, uint32_t, uint32_t *, uint32_t *);

// VFL-VSVFL Device Struct
/**
 * This is the structure for the VSVFL device.
 *
 * @implements _vfl_device
 * @ingroup VFL
 */
typedef struct _vfl_vsvfl_device
{
	vfl_device_t vfl;

	uint32_t current_version;
	struct _vfl_vsvfl_context *contexts;
	vfl_vsvfl_geometry_t geometry;
	uint8_t *bbt[16];

	uint32_t *pageBuffer;
	uint16_t *chipBuffer;
	uint16_t *blockBuffer;

	vsvfl_virtual_to_physical_t virtual_to_physical;
	vsvfl_physical_to_virtual_t physical_to_virtual;

	VSVFLStats *stats;
} vfl_vsvfl_device_t;

// VFL-VFL Functions
error_t vfl_vsvfl_device_init(vfl_vsvfl_device_t *_vfl);
void vfl_vsvfl_device_cleanup(vfl_vsvfl_device_t *_vfl);

vfl_vsvfl_device_t *vfl_vsvfl_device_allocate(void);

#endif //VFL_VSVFL_H
