/*
 * vfl.c
 *
 * Copyright 2010 iDroid Project
 *
 * This file is part of iDroid. An android distribution for Apple products.
 * For more information, please visit http://www.idroidproject.org/.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/slab.h>
#include <linux/apple_flash.h>
#include <mach/map.h>
#include "vfl.h"
#include "vsvfl.h"

void vfl_init(vfl_device_t *_vfl)
{
	memset(_vfl, 0, sizeof(*_vfl));
}

void vfl_cleanup(vfl_device_t *_vfl)
{
}

vfl_device_t *vfl_allocate(void)
{
	vfl_device_t *ret = kmalloc(sizeof(*ret), GFP_KERNEL);
	vfl_init(ret);
	return ret;
}

error_t vfl_open(vfl_device_t *_vfl)
{
	if(!_vfl->open)
		return -ENOENT;

	return _vfl->open(_vfl);
}

void vfl_close(vfl_device_t *_vfl)
{
	if(!_vfl->close)
		return;

	_vfl->close(_vfl);
}

error_t vfl_read_single_page(vfl_device_t *_vfl, uint32_t _page, uint8_t* buffer, uint8_t* spare,
		int empty_ok, int* refresh_page, uint32_t disable_aes)
{
	if(!_vfl->read_single_page)
		return -ENOENT;

	return _vfl->read_single_page(_vfl, _page, buffer, spare, empty_ok, refresh_page, disable_aes);
}

error_t vfl_write_single_page(vfl_device_t *_vfl, uint32_t _page, uint8_t* buffer, uint8_t* spare,
		int _scrub)
{
	if(!_vfl->write_single_page)
		return -ENOENT;

	return _vfl->write_single_page(_vfl, _page, buffer, spare, _scrub);
}

error_t vfl_erase_single_block(vfl_device_t *_vfl, uint32_t _block, int _replace_bad_block)
{
	if(!_vfl->erase_single_block)
		return -ENOENT;

	return _vfl->erase_single_block(_vfl, _block, _replace_bad_block);
}

error_t vfl_write_context(vfl_device_t *_vfl, uint16_t *_control_block)
{
	if(!_vfl->write_context)
		return -ENOENT;

	return _vfl->write_context(_vfl, _control_block);
}

uint16_t *vfl_get_ftl_ctrl_block(vfl_device_t *_vfl)
{
	if(!_vfl->get_ftl_ctrl_block) {
		return NULL;
	}

	return _vfl->get_ftl_ctrl_block(_vfl);
}

error_t vfl_get_info(vfl_device_t *_vfl, vfl_info_t _item, void *_result, size_t _sz)
{
	if(!_vfl->get_info) {
		return -ENOENT;
	}

	return _vfl->get_info(_vfl, _item, _result, _sz);
}

uint32_t chipid_get_nand_epoch(void)
{
	uint32_t ret = (__raw_readl(VA_CHIPID) >> 9) & 0x7f;
	return ret ? ret : 1;
}

error_t vfl_detect(vfl_device_t **_vfl)
{
	uint8_t sigbuf[264];
	uint32_t flags;

	error_t ret;

	ret = apple_nand_special_page(0, "NANDDRIVERSIGN\0\0", sigbuf, sizeof(sigbuf));
	if(ret)
		return ret;


	// Starting from iOS5 there's a change in behaviour at chipid_get_nand_epich().
	if((!chipid_get_nand_epoch() && sigbuf[0] != '1' && sigbuf[0] != '2') || sigbuf[3] != 'C'
			|| sigbuf[1] > '1' || sigbuf[2] > '1'
			 || sigbuf[4] > 6)
	{
		printk(KERN_ERR "vfl: Incompatible signature.\n");
		return -ENOENT;
	}

	flags = *(uint32_t*)&sigbuf[4];

	if(sigbuf[1] == '1')
	{
		int whitening;
		printk(KERN_INFO "vfl: Detected VSVFL.\n");
		*_vfl = &vfl_vsvfl_device_allocate()->vfl;
		if(!*_vfl)
			return -ENOENT;

		whitening = flags & 0x10000;
		if(apple_nand_set_data_whitening(whitening)
				&& whitening) {
			printk(KERN_ERR "vfl: Failed to enable data whitening!\n");
			return -ENOENT;
		}
	}
	else if(sigbuf[1] == '0')
	{
		printk(KERN_ERR "vfl: Detected old-style VFL.\r\n");
		printk(KERN_ERR "vfl: Standard VFL support not included!\r\n");
		return -ENOENT;
	}
	else
	{
		printk(KERN_ERR "vfl: No valid VFL signature found!\r\n");
		return -ENOENT;
	}

	return vfl_open(*_vfl);
}
