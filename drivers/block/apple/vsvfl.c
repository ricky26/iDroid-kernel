#include <linux/slab.h>
#include <linux/apple_flash.h>
#include "yaftl/yaftl_common.h"
#include "vsvfl.h"

typedef struct _vfl_vsvfl_context
{
	uint32_t usn_inc; // 0x000
	uint32_t usn_dec; // 0x004
	uint32_t ftl_type; // 0x008
	uint16_t usn_block; // 0x00C // current block idx
	uint16_t usn_page; // 0x00E // used pages
	uint16_t active_context_block; // 0x010
	uint16_t write_failure_count; // 0x012
	uint16_t bad_block_count; // 0x014
	uint8_t replaced_block_count[4]; // 0x016
	uint16_t num_reserved_blocks; // 0x01A
	uint16_t field_1C; // 0x01C
	uint16_t total_reserved_blocks; // 0x01E
	uint8_t field_20[6]; // 0x020
	uint16_t reserved_block_pool_map[820]; // 0x026
	uint16_t vfl_context_block[4]; // 0x68E
	uint16_t usable_blocks_per_bank; // 0x696
	uint16_t reserved_block_pool_start; // 0x698
	uint16_t control_block[3]; // 0x69A
	uint16_t scrub_list_length; // 0x6A0
	uint16_t scrub_list[20]; // 0x6A2
	uint32_t field_6CA[4]; // 0x6CA
	uint32_t vendor_type; // 0x6DA
	uint8_t field_6DE[204]; // 0x6DE
	uint16_t remapping_schedule_start; // 0x7AA
	uint8_t unk3[0x48];				// 0x7AC
	uint32_t version;				// 0x7F4
	uint32_t checksum1;				// 0x7F8
	uint32_t checksum2;				// 0x7FC
} __attribute__((packed)) vfl_vsvfl_context_t;

typedef struct _vfl_vsvfl_spare_data
{
	union
	{
		struct
		{
			uint32_t logicalPageNumber;
			uint32_t usn;
		} __attribute__ ((packed)) user;

		struct
		{
			uint32_t usnDec;
			uint16_t idx;
			uint8_t field_6;
			uint8_t field_7;
		} __attribute__ ((packed)) meta;
	};

	uint8_t type2;
	uint8_t type1;
	uint8_t eccMark;
	uint8_t field_B;
} __attribute__ ((packed)) vfl_vsvfl_spare_data_t;

static void virtual_to_physical_10001(vfl_vsvfl_device_t *_vfl, uint32_t _vBank, uint32_t _vPage, uint32_t *_pCE, uint32_t *_pPage)
{
	*_pCE = _vBank;
	*_pPage = _vPage;
}

static void physical_to_virtual_10001(vfl_vsvfl_device_t *_vfl, uint32_t _pCE, uint32_t _pPage, uint32_t *_vBank, uint32_t *_vPage)
{
	*_vBank = _pCE;
	*_vPage = _pPage;
}

static void virtual_to_physical_100014(vfl_vsvfl_device_t *_vfl, uint32_t _vBank, uint32_t _vPage, uint32_t *_pCE, uint32_t *_pPage)
{
	uint32_t pBank, pPage;

	pBank = _vBank / _vfl->geometry.num_ce;
	pPage = ((_vfl->geometry.pages_per_block - 1) & _vPage) | (2 * (~(_vfl->geometry.pages_per_block - 1) & _vPage));
	if (pBank & 1)
		pPage |= _vfl->geometry.pages_per_block;

	*_pCE = _vBank % _vfl->geometry.num_ce;
	*_pPage = pPage;
}

static void physical_to_virtual_100014(vfl_vsvfl_device_t *_vfl, uint32_t _pCE, uint32_t _pPage, uint32_t *_vBank, uint32_t *_vPage)
{
	uint32_t vBank, vPage;
	vBank = _vfl->geometry.pages_per_block & _pPage;
	vPage = ((_vfl->geometry.pages_per_block - 1) & _pPage) | (((_vfl->geometry.pages_per_block * -2) & _pPage) / 2);
	if(vBank)
		vBank = _vfl->geometry.num_ce;

	*_vBank = _pCE + vBank;
	*_vPage = vPage;
}

static void virtual_to_physical_150011(vfl_vsvfl_device_t *_vfl, uint32_t _vBank, uint32_t _vPage, uint32_t *_pCE, uint32_t *_pPage)
{
	uint32_t pBlock;

	pBlock = 2 * (_vPage / _vfl->geometry.pages_per_block);
	if(_vBank % (2 * _vfl->geometry.num_ce) >= _vfl->geometry.num_ce)
		pBlock++;

	*_pCE = _vBank % _vfl->geometry.num_ce;
	*_pPage = (_vfl->geometry.pages_per_block * pBlock) | (_vPage % 128);
}

static void physical_to_virtual_150011(vfl_vsvfl_device_t *_vfl, uint32_t _pCE, uint32_t _pPage, uint32_t *_vBank, uint32_t *_vPage)
{
	uint32_t pBlock;

	*_vBank = _pCE;
	pBlock = _pPage / _vfl->geometry.pages_per_block;
	if(pBlock % 2) {
		pBlock--;
		*_vBank = _vfl->geometry.num_ce + _pCE;
	}
	*_vPage = (_vfl->geometry.pages_per_block * (pBlock / 2)) | (_pPage % 128);
}

static error_t virtual_block_to_physical_block(vfl_vsvfl_device_t *_vfl, uint32_t _vBank, uint32_t _vBlock, uint32_t *_pBlock)
{
	uint32_t pCE, pPage;

	if(!_vfl->virtual_to_physical) {
		printk(KERN_ERR "vsvfl: virtual_to_physical hasn't been initialized yet!\r\n");
		return EINVAL;
	}

	_vfl->virtual_to_physical(_vfl, _vBank, _vfl->geometry.pages_per_block * _vBlock, &pCE, &pPage);
	*_pBlock = pPage / _vfl->geometry.pages_per_block;

	return 0;
}

static error_t physical_block_to_virtual_block(vfl_vsvfl_device_t *_vfl, uint32_t _pBlock, uint32_t *_vBank, uint32_t *_vBlock)
{
	uint32_t vBank, vPage;

	if(!_vfl->physical_to_virtual) {
		printk(KERN_ERR "vsvfl: physical_to_virtual hasn't been initialized yet!\r\n");
		return EINVAL;
	}

	_vfl->physical_to_virtual(_vfl, 0, _vfl->geometry.pages_per_block * _pBlock, &vBank, &vPage);
	*_vBank = vBank / _vfl->geometry.num_ce;
	*_vBlock = vPage / _vfl->geometry.pages_per_block;

	return 0;
}


static int vfl_is_good_block(uint8_t* badBlockTable, uint32_t block) {
	return (badBlockTable[block / 8] & (1 << (block % 8))) != 0;
}

static uint32_t remap_block(vfl_vsvfl_device_t *_vfl, uint32_t _ce, uint32_t _block, uint32_t *_isGood)
{
	int pwDesPbn;
	printk(KERN_DEBUG "vsvfl: remap_block: CE %d, block %d\r\n", _ce, _block);

	if(vfl_is_good_block(_vfl->bbt[_ce], _block))
		return _block;

	printk(KERN_DEBUG "vsvfl: remapping block...\r\n");

	if(_isGood)
		_isGood = 0;

	for(pwDesPbn = 0; pwDesPbn < _vfl->geometry.blocks_per_ce - _vfl->contexts[_ce].reserved_block_pool_start * _vfl->geometry.banks_per_ce; pwDesPbn++)
	{
		if(_vfl->contexts[_ce].reserved_block_pool_map[pwDesPbn] == _block)
		{
			uint32_t vBank, vBlock, pBlock;

			/*
			if(pwDesPbn >= _vfl->geometry.blocks_per_ce)
				printk(KERN_ERR "ftl: Destination physical block for remapping is greater than number of blocks per CE!");
			*/

			vBank = _ce + _vfl->geometry.num_ce * (pwDesPbn / (_vfl->geometry.blocks_per_bank_vfl - _vfl->contexts[_ce].reserved_block_pool_start));
			vBlock = _vfl->contexts[_ce].reserved_block_pool_start + (pwDesPbn % (_vfl->geometry.blocks_per_bank_vfl - _vfl->contexts[_ce].reserved_block_pool_start));

			if(virtual_block_to_physical_block(_vfl, vBank, vBlock, &pBlock))
				printk(KERN_DEBUG "PANIC!!!: vfl: failed to convert virtual reserved block to physical\r\n");

			return pBlock;
		}
	}

	printk(KERN_ERR "vfl: failed to remap CE %d block 0x%04x\r\n", _ce, _block);
	return _block;
}

static error_t virtual_page_number_to_physical(vfl_vsvfl_device_t *_vfl, uint32_t _vpNum, uint32_t* _ce, uint32_t* _page) {
	uint32_t ce, vBank, ret, bank_offset, pBlock;

	vBank = _vpNum % _vfl->geometry.banks_total;
	ce = vBank % _vfl->geometry.num_ce;

	ret = virtual_block_to_physical_block(_vfl, vBank, _vpNum / _vfl->geometry.pages_per_sublk, &pBlock);

	if(ret)
		return ret;

	pBlock = remap_block(_vfl, ce, pBlock, 0);

	bank_offset = _vfl->geometry.bank_address_space * (pBlock / _vfl->geometry.blocks_per_bank);

	*_ce = ce;
	*_page = _vfl->geometry.pages_per_block_2 * (bank_offset + (pBlock % _vfl->geometry.blocks_per_bank))
			+ ((_vpNum % _vfl->geometry.pages_per_sublk) / _vfl->geometry.banks_total);

	return 0;
}

static void vfl_checksum(void* data, int size, uint32_t* a, uint32_t* b)
{
	int i;
	uint32_t* buffer = (uint32_t*) data;
	uint32_t x = 0;
	uint32_t y = 0;
	for(i = 0; i < (size / 4); i++) {
		x += buffer[i];
		y ^= buffer[i];
	}

	*a = x + 0xAABBCCDD;
	*b = y ^ 0xAABBCCDD;
}

static int vfl_gen_checksum(vfl_vsvfl_device_t *_vfl, int ce)
{
	vfl_checksum(&_vfl->contexts[ce], (uint32_t)&_vfl->contexts[ce].checksum1 - (uint32_t)&_vfl->contexts[ce],
			&_vfl->contexts[ce].checksum1, &_vfl->contexts[ce].checksum2);
	return 0;
}

static int vfl_check_checksum(vfl_vsvfl_device_t *_vfl, int ce)
{
	static int counter = 0;
	uint32_t checksum1;
	uint32_t checksum2;

	counter++;

	vfl_checksum(&_vfl->contexts[ce], (uint32_t)&_vfl->contexts[ce].checksum1 - (uint32_t)&_vfl->contexts[ce],
			&checksum1, &checksum2);

	// Yeah, this looks fail, but this is actually the logic they use
	if(checksum1 == _vfl->contexts[ce].checksum1)
		return 1;

	if(checksum2 != _vfl->contexts[ce].checksum2)
		return 1;

	return 0;
}

static error_t vsvfl_store_vfl_cxt(vfl_vsvfl_device_t *_vfl, uint32_t _ce);
static int is_block_in_scrub_list(vfl_vsvfl_device_t *_vfl, uint32_t _ce, uint32_t _block) {
	uint32_t i;

	for (i = 0; i < _vfl->contexts[_ce].scrub_list_length; i++) {
		if (_vfl->contexts[_ce].scrub_list[i] == _block)
			return 1;
	}

	return 0;
}

static int add_block_to_scrub_list(vfl_vsvfl_device_t *_vfl, uint32_t _ce, uint32_t _block) {
	if(is_block_in_scrub_list(_vfl, _ce, _block))
			return 0;

	if(_vfl->contexts[_ce].scrub_list_length > 0x13) {
		printk(KERN_ERR "vfl: too many scrubs!\r\n");
		return 0;
	}

	if(!vfl_check_checksum(_vfl, _ce))
		printk(KERN_DEBUG "PANIC!!!: vfl_add_block_to_scrub_list: failed checksum\r\n");

	_vfl->contexts[_ce].scrub_list[_vfl->contexts[_ce].scrub_list_length++] = _block;
	vfl_gen_checksum(_vfl, _ce);
	return vsvfl_store_vfl_cxt(_vfl, _ce);
}

static error_t vfl_vsvfl_write_single_page(vfl_device_t *_vfl, uint32_t dwVpn, uint8_t* buffer, uint8_t* spare, int _scrub)
{
	vfl_vsvfl_device_t *vfl = container_of(_vfl, vfl_vsvfl_device_t, vfl);

	uint32_t pCE = 0, pPage = 0;
	int ret;

	ret = virtual_page_number_to_physical(vfl, dwVpn, &pCE, &pPage);

	if(ret) {
		printk(KERN_ERR "vfl_vsvfl_write_single_page: virtual_page_number_to_physical returned an error (dwVpn %d)!\r\n", dwVpn);
		return ret;
	}

	ret = apple_nand_write_page(pCE, pPage, buffer, spare);

	if(ret) {
		if(!vfl_check_checksum(vfl, pCE))
			printk(KERN_DEBUG "PANIC!!!: vfl_vsfl_write_single_page: failed checksum\r\n");

		vfl->contexts[pCE].write_failure_count++;
		vfl_gen_checksum(vfl, pCE);

		// TODO: add block map support
		// vsvfl_mark_page_as_bad(pCE, pPage, ret);

		if(_scrub)
			add_block_to_scrub_list(vfl, pCE, pPage / vfl->geometry.pages_per_block); // Something like that, I think

		return ret;
	}

	return 0;
}

static error_t vfl_vsvfl_read_single_page(vfl_device_t *_vfl, uint32_t dwVpn, uint8_t* buffer, uint8_t* spare, int empty_ok, int* refresh_page, uint32_t disable_aes)
{
	vfl_vsvfl_device_t *vfl = container_of(_vfl, vfl_vsvfl_device_t, vfl);
	uint32_t pCE = 0, pPage = 0;
	int ret;


	if(refresh_page)
		*refresh_page = 0;

	//VFLData1.field_8++;
	//VFLData1.field_20++;

	ret = virtual_page_number_to_physical(vfl, dwVpn, &pCE, &pPage);

	if(ret) {
		printk(KERN_ERR "vfl_vsvfl_read_single_page: virtual_page_number_to_physical returned an error (dwVpn %d)!\r\n", dwVpn);
		return ret;
	}

	// Hack to get reading by absolute page number.
	ret = apple_nand_read_page(pCE, pPage, buffer, spare, disable_aes);

	if(!empty_ok && ret == ENOENT)
		ret = EIO;
	else if(empty_ok && ret == ENOENT) {
		if(spare)
			memset(spare, 0xFF, vfl->geometry.bytes_per_spare);

		return 1;
	}

	if(ret == EINVAL || ret == EIO)
	{
		ret = apple_nand_read_page(pCE, pPage, buffer, spare, disable_aes);
		if(!empty_ok && ret == ENOENT)
			return EIO;

		if(ret == EINVAL || ret == EIO)
			return ret;
	}

	if(ret == ENOENT && spare)
		memset(spare, 0xFF, vfl->geometry.bytes_per_spare);

	return ret;
}

static error_t vsvfl_write_vfl_cxt_to_flash(vfl_vsvfl_device_t *_vfl, uint32_t _ce) {
	int fails = 0;
	int i;
	vfl_vsvfl_context_t *curVFLCxt;
	uint32_t curPage;
	uint8_t* pageBuffer;
	uint8_t* spareBuffer;

	if(_ce >= _vfl->geometry.num_ce)
		return EINVAL;

	if(!vfl_check_checksum(_vfl, _ce))
		printk(KERN_DEBUG "PANIC!!!: vsvfl_write_vfl_cxt_to_flash: failed checksum\r\n");

//	posix_memalign(&pageBuffer, 0x40, _vfl->geometry.bytes_per_page);
//	posix_memalign(&spareBuffer, 0x40, _vfl->geometry.bytes_per_spare);
	pageBuffer = kzalloc(_vfl->geometry.bytes_per_page, GFP_KERNEL);
	spareBuffer = kmalloc(_vfl->geometry.bytes_per_spare, GFP_KERNEL);
	if(pageBuffer == NULL || spareBuffer == NULL) {
		printk(KERN_ERR "vfl: cannot allocate page and spare buffer\r\n");
		return ENOMEM;
	}
//	memset(pageBuffer, 0x0, _vfl->geometry.bytes_per_page);

	curVFLCxt = &_vfl->contexts[_ce];
	curVFLCxt->usn_inc = _vfl->current_version++;
	curPage = curVFLCxt->usn_page;
	curVFLCxt->usn_page += 8;
	curVFLCxt->usn_dec -= 1;
	vfl_gen_checksum(_vfl, _ce);

	memcpy(pageBuffer, curVFLCxt, 0x800);
	for (i = 0; i < 8; i++) {
		uint32_t bankStart;
		uint32_t blockOffset;
		int status;
		memset(spareBuffer, 0xFF, _vfl->geometry.bytes_per_spare);
		((uint32_t*)spareBuffer)[0] = curVFLCxt->usn_dec;
		spareBuffer[8] = 0;
		spareBuffer[9] = 0x80;
		bankStart = (curVFLCxt->vfl_context_block[curVFLCxt->usn_block] / _vfl->geometry.blocks_per_bank) * _vfl->geometry.bank_address_space;
		blockOffset = curVFLCxt->vfl_context_block[curVFLCxt->usn_block] % _vfl->geometry.blocks_per_bank;
		status = apple_nand_write_page(_ce, (bankStart + blockOffset) * _vfl->geometry.pages_per_block_2 + curPage + i, pageBuffer, spareBuffer);
		if(status) {
			printk(KERN_ERR "vfl_write_vfl_cxt_to_flash: Failed write\r\n");
			kfree(pageBuffer);
			kfree(spareBuffer);
			// vsvfl_mark_page_as_bad(_ce, (bankStart + blockOffset) * _vfl->geometry.pages_per_block_2 + curPage + i, status);
			return EIO;
		}
	}
	for (i = 0; i < 8; i++) {
		uint32_t bankStart = (curVFLCxt->vfl_context_block[curVFLCxt->usn_block] / _vfl->geometry.blocks_per_bank) * _vfl->geometry.bank_address_space;
		uint32_t blockOffset = curVFLCxt->vfl_context_block[curVFLCxt->usn_block] % _vfl->geometry.blocks_per_bank;
		if(apple_nand_read_page(_ce, (bankStart + blockOffset) * _vfl->geometry.pages_per_block_2 + curPage + i, pageBuffer, spareBuffer, 0)) {
			//vsvfl_store_block_map_single_page(_ce, (bankStart + blockOffset) * _vfl->geometry.pages_per_block_2 + curPage + i);
			fails++;
			continue;
		}
		if(memcmp(pageBuffer, curVFLCxt, 0x6E0) || ((uint32_t*)spareBuffer)[0] != curVFLCxt->usn_dec || spareBuffer[8] || spareBuffer[9] != 0x80)
			fails++;
	}
	kfree(pageBuffer);
	kfree(spareBuffer);
	if(fails > 3)
		return EIO;
	else
		return 0;
}

static error_t vfl_vsvfl_write_context(vfl_device_t *_vfl, uint16_t *_control_block)
{
	vfl_vsvfl_device_t *vfl = container_of(_vfl, vfl_vsvfl_device_t, vfl);
	uint32_t ce = vfl->current_version % vfl->geometry.num_ce;
	uint32_t i;

	// check and update cxt of each CE
	for(i = 0; i < vfl->geometry.num_ce; i++) {
		if(vfl_check_checksum(vfl, i) == 0)
			printk(KERN_DEBUG "PANIC!!!: vsvfl: VFLCxt has bad checksum.\r\n");
		memmove(vfl->contexts[i].control_block, _control_block, 6);
		vfl_gen_checksum(vfl, i);
	}

	// write cxt on the ce with the oldest cxt
	if(vsvfl_store_vfl_cxt(vfl, ce)) {
		printk(KERN_ERR "vsvfl: context write fail!\r\n");
		return EIO;
	}

	return 0;
}

static error_t vsvfl_store_vfl_cxt(vfl_vsvfl_device_t *_vfl, uint32_t _ce) {
	vfl_vsvfl_context_t *curVFLCxt;
	if(_ce >= _vfl->geometry.num_ce)
		printk(KERN_DEBUG "PANIC!!!: vfl: Can't store VFLCxt on non-existent CE\r\n");

	curVFLCxt = &_vfl->contexts[_ce];
	if(curVFLCxt->usn_page + 8 > _vfl->geometry.pages_per_block || vsvfl_write_vfl_cxt_to_flash(_vfl, _ce)) {
		int startBlock = curVFLCxt->usn_block;
		int nextBlock = (curVFLCxt->usn_block + 1) % 4;
		while(startBlock != nextBlock) {
			if(curVFLCxt->vfl_context_block[nextBlock] != 0xFFFF) {
				int fail = 0;
				int i;
				for (i = 0; i < 4; i++) {
					uint32_t bankStart = (curVFLCxt->vfl_context_block[nextBlock] / _vfl->geometry.blocks_per_bank) * _vfl->geometry.bank_address_space;
					uint32_t blockOffset = curVFLCxt->vfl_context_block[nextBlock] % _vfl->geometry.blocks_per_bank;
					int status = apple_nand_erase_block(_ce, (bankStart + blockOffset) * _vfl->geometry.pages_per_block);
					if(!status)
						break;
					//vsvfl_mark_bad_vfl_block(_vfl, _ce, curVFLCxt->vfl_context_block[nextBlock], status);
					if(i == 3)
						fail = 1;
				}
				if(!fail) {
					int result;
					if(!vfl_check_checksum(_vfl, _ce))
						printk(KERN_DEBUG "PANIC!!!: vsvfl_store_vfl_cxt: failed checksum\r\n");
					curVFLCxt->usn_block = nextBlock;
					curVFLCxt->usn_page = 0;
					vfl_gen_checksum(_vfl, _ce);
					result = vsvfl_write_vfl_cxt_to_flash(_vfl, _ce);
					if(!result)
						return result;
				}
			}
			nextBlock = (nextBlock + 1) % 4;
		}
		return EIO;
	}
	return 0;
}

static error_t vsvfl_replace_bad_block(vfl_vsvfl_device_t *_vfl, uint32_t _ce, uint32_t _block){
	int i;
	uint32_t vBank = 0, vBlock;
	uint16_t drbc[16]; // dynamic replaced block count
	vfl_vsvfl_context_t *curVFLCxt = &_vfl->contexts[_ce];
	uint32_t reserved_blocks = _vfl->geometry.blocks_per_ce - (curVFLCxt->reserved_block_pool_start * _vfl->geometry.banks_per_ce);

	_vfl->bbt[_ce][_block >> 3] &= ~(1 << (_block & 7));

	for (i = 0; i < reserved_blocks; i++) {
		uint32_t reserved_blocks_per_bank;
		uint32_t bank;
		uint32_t block_number;
		uint32_t pBlock;
		if(curVFLCxt->reserved_block_pool_map[i] != _block)
			continue;

		reserved_blocks_per_bank = _vfl->geometry.blocks_per_bank - curVFLCxt->reserved_block_pool_start;
		bank = _ce + _vfl->geometry.num_ce * (i / reserved_blocks_per_bank);
		block_number = curVFLCxt->reserved_block_pool_start + (i % reserved_blocks_per_bank);
		virtual_block_to_physical_block(_vfl, bank, block_number, &pBlock);
		_vfl->bbt[_ce][pBlock] &= ~(1 << (pBlock & 7));
	}

	physical_block_to_virtual_block(_vfl, _block, &vBank, &vBlock);
	while(curVFLCxt->replaced_block_count[vBank] < (_vfl->geometry.blocks_per_bank_vfl - curVFLCxt->reserved_block_pool_start)) {
		uint32_t weirdBlock = curVFLCxt->replaced_block_count[vBank] + (_vfl->geometry.blocks_per_bank_vfl - curVFLCxt->reserved_block_pool_start) * vBank;
		if(curVFLCxt->reserved_block_pool_map[weirdBlock] == 0xFFF0) {
			curVFLCxt->reserved_block_pool_map[weirdBlock] = _block;
			curVFLCxt->replaced_block_count[vBank]++;
			return 0;
		}
		vBank++;
	}

	for (i = 0; i < _vfl->geometry.banks_per_ce; i++) {
		drbc[i] = i;
	}
	if(_vfl->geometry.banks_per_ce != 1) {
		for(i = 0; i < (_vfl->geometry.banks_per_ce - 1); i++) {
			int j;
			for (j = i + 1; j < _vfl->geometry.banks_per_ce; j++) {
				if(curVFLCxt->replaced_block_count[drbc[j]] < curVFLCxt->replaced_block_count[i]) {
					drbc[j] = i;
					drbc[i] = j;
				}
			}
		}
	}
	for (i = 0; i < _vfl->geometry.banks_per_ce; i++) {
		while(curVFLCxt->replaced_block_count[drbc[i]] < (_vfl->geometry.blocks_per_bank_vfl - curVFLCxt->reserved_block_pool_start)) {
			uint32_t weirdBlock = curVFLCxt->replaced_block_count[drbc[i]] + (_vfl->geometry.blocks_per_bank_vfl - curVFLCxt->reserved_block_pool_start) * vBank;
			if(curVFLCxt->reserved_block_pool_map[weirdBlock] == 0xFFF0) {
				curVFLCxt->reserved_block_pool_map[weirdBlock] = _block;
				curVFLCxt->replaced_block_count[drbc[i]]++;
				return 0;
			}
			i++;
		}
	}
	printk(KERN_DEBUG "PANIC!!!: vsvfl_replace_bad_block: Failed to replace block\r\n");
	return EIO;
}

static error_t vfl_vsvfl_erase_single_block(vfl_device_t *_vfl, uint32_t _vbn, int _replaceBadBlock) {
	vfl_vsvfl_device_t *vfl = container_of(_vfl, vfl_vsvfl_device_t, vfl);
	uint32_t bank;
	uint32_t status = EINVAL;

	// In order to erase a single virtual block, we have to erase the matching
	// blocks across all banks.
	for (bank = 0; bank < vfl->geometry.banks_total; bank++) {
		uint32_t pBlock, pCE, blockRemapped;

		// Find the physical block before bad-block remapping.
		virtual_block_to_physical_block(vfl, bank, _vbn, &pBlock);
		pCE = bank % vfl->geometry.num_ce;
		vfl->blockBuffer[bank] = pBlock;

		if (is_block_in_scrub_list(vfl, pCE, pBlock)) {
			vfl_vsvfl_context_t *curVFLCxt;
			vsvfl_replace_bad_block(vfl, pCE, pBlock);
			vfl_gen_checksum(vfl, pCE);
			curVFLCxt = &vfl->contexts[pCE];
			if(is_block_in_scrub_list(vfl, pCE, vfl->blockBuffer[bank])) {
				int i;
				for (i = 0; i < curVFLCxt->scrub_list_length; i++) {
					if(curVFLCxt->scrub_list[i] != vfl->blockBuffer[bank])
						continue;
					if(!vfl_check_checksum(vfl, pCE))
						printk(KERN_DEBUG "PANIC!!!: vfl_erase_single_block: failed checksum\r\n");
					curVFLCxt->scrub_list[i] = 0;
					curVFLCxt->scrub_list_length--;
					if(i != curVFLCxt->scrub_list_length && curVFLCxt->scrub_list_length != 0)
						curVFLCxt->scrub_list[i] = curVFLCxt->scrub_list[curVFLCxt->scrub_list_length];
					vfl_gen_checksum(vfl, pCE);
					vsvfl_store_vfl_cxt(vfl, pCE);
					break;
				}
			} else
				printk(KERN_ERR "vfl_erase_single_block: Failed checking for block in scrub list\r\n");
		}

		// Remap the block and calculate its physical number (considering bank address space).
		blockRemapped = remap_block(vfl, pCE, pBlock, 0);
		vfl->blockBuffer[bank] = blockRemapped % vfl->geometry.blocks_per_bank
			+ (blockRemapped / vfl->geometry.blocks_per_bank) * vfl->geometry.bank_address_space;
	}

	// TODO: H2FMI erase multiple blocks. Currently we erase the blocks one by one.
	// Actually, the block buffer is used for erase multiple blocks, so we won't use it here.
	for (bank = 0; bank < vfl->geometry.banks_total; bank++) {
		uint32_t pBlock, pCE, tries;

		virtual_block_to_physical_block(vfl, bank, _vbn, &pBlock);
		pCE = bank % vfl->geometry.num_ce;

		// Try to erase each block at most 3 times.
		for (tries = 0; tries < 3; tries++) {
			uint32_t blockRemapped, bankStart, blockOffset;

			blockRemapped = remap_block(vfl, pCE, pBlock, 0);
			bankStart = (blockRemapped / vfl->geometry.blocks_per_bank) * vfl->geometry.bank_address_space;
			blockOffset = blockRemapped % vfl->geometry.blocks_per_bank;

			status = apple_nand_erase_block(pCE, (bankStart + blockOffset) * vfl->geometry.pages_per_block);
			if (status == 0)
				break;

			// TODO: add block map support.
			//mark_bad_block(vfl, pCE, pBlock, status);
			printk(KERN_ERR "vfl: failed erasing physical block %d on bank %d. status: 0x%08x\r\n",
				blockRemapped, bank, status);

			if (!_replaceBadBlock)
				return EINVAL;

			// Bad block management at erasing should actually be like this (improvised \o/)
			vsvfl_replace_bad_block(vfl, pCE, bankStart + blockOffset);
			if(!vfl_check_checksum(vfl, pCE))
				printk(KERN_DEBUG "PANIC!!!: vfl_erase_single_block: failed checksum\r\n");
			vfl->contexts[pCE].bad_block_count++;
			vfl_gen_checksum(vfl, pCE);
			vsvfl_store_vfl_cxt(vfl, pCE);
		}
	}

	if (status)
		printk(KERN_DEBUG "PANIC!!!: vfl: failed to erase virtual block %d!\r\n", _vbn);

	return 0;
}

static vfl_vsvfl_context_t* get_most_updated_context(vfl_vsvfl_device_t *vfl) {
	int ce = 0;
	int max = 0;
	vfl_vsvfl_context_t* cxt = NULL;

	for(ce = 0; ce < vfl->geometry.num_ce; ce++)
	{
		int cur = vfl->contexts[ce].usn_inc;
		if(max <= cur)
		{
			max = cur;
			cxt = &vfl->contexts[ce];
		}
	}

	return cxt;
}

static uint16_t* VFL_get_FTLCtrlBlock(vfl_device_t *_vfl)
{
	vfl_vsvfl_device_t *vfl = container_of(_vfl, vfl_vsvfl_device_t, vfl);

	vfl_vsvfl_context_t *cxt = get_most_updated_context(vfl);

	if(cxt)
		return cxt->control_block;
	else
		return NULL;
}

static uint32_t next_power_of_two(uint32_t n)
{
	uint32_t val = 1 << (31 - __builtin_clz(n));
	if (n % val)
		val *= 2;
	return val;
}

static inline error_t vfl_vsvfl_setup_geometry(vfl_vsvfl_device_t *_vfl)
{
#define nand_load(what, where) _vfl->geometry.where = apple_nand_get_info(what)

//	uint32_t val = 1;
	uint16_t z;
	uint32_t mag = 1;
	uint16_t a;

	nand_load(NAND_BLOCKS_PER_CE, blocks_per_ce);
	nand_load(NAND_BLOCKS_PER_BANK, blocks_per_bank);
	nand_load(NAND_BANKS_PER_CE, banks_per_ce);
	nand_load(NAND_PAGE_SIZE, bytes_per_page);
	nand_load(NAND_BANK_ADDRESS_SPACE, bank_address_space);
	_vfl->geometry.num_ce = apple_nand_get_num_ce();
	nand_load(NAND_PAGES_PER_BLOCK, pages_per_block);
	_vfl->geometry.pages_per_block_2 = next_power_of_two(_vfl->geometry.pages_per_block);
#ifdef YUSTAS_FIXME
	nand_load(, ecc_bits);
#endif

	printk(KERN_DEBUG "blocks per ce: 0x%08x\r\n", _vfl->geometry.blocks_per_ce);
	printk(KERN_DEBUG "blocks per bank: 0x%0x\r\n", _vfl->geometry.blocks_per_bank);
	printk(KERN_DEBUG "banks per ce: 0x%08x\r\n", _vfl->geometry.banks_per_ce);
	printk(KERN_DEBUG "bytes per page: 0x%08x\r\n", _vfl->geometry.bytes_per_page);
	printk(KERN_DEBUG "bank address space: 0x%08x\r\n", _vfl->geometry.bank_address_space);
	printk(KERN_DEBUG "num ce: 0x%08x\r\n", _vfl->geometry.num_ce);
	printk(KERN_DEBUG "pages per block: 0x%08x\r\n", _vfl->geometry.pages_per_block);
	printk(KERN_DEBUG "pages per block2: 0x%08x\r\n", _vfl->geometry.pages_per_block_2);
#ifdef YUSTAS_FIXME
	printk(KERN_ERR "ecc bits: 0x%08x\r\n", _vfl->geometry.ecc_bits);
#endif

	z = _vfl->geometry.blocks_per_ce;
	mag = 1;
	while(z != 0 && mag < z) mag <<= 1;
	mag >>= 10;

	a = (mag << 7) - (mag << 3) + mag;
	_vfl->geometry.some_page_mask = a;
	printk(KERN_DEBUG "some_page_mask: 0x%08x\r\n", _vfl->geometry.some_page_mask);

	_vfl->geometry.pages_total = z * _vfl->geometry.pages_per_block * _vfl->geometry.num_ce;
	printk(KERN_DEBUG "pages_total: 0x%08x\r\n", _vfl->geometry.pages_total);

	_vfl->geometry.pages_per_sublk = _vfl->geometry.pages_per_block * _vfl->geometry.banks_per_ce * _vfl->geometry.num_ce;
	printk(KERN_DEBUG "pages_per_sublk: 0x%08x\r\n", _vfl->geometry.pages_per_sublk);

	_vfl->geometry.some_sublk_mask =
		_vfl->geometry.some_page_mask * _vfl->geometry.pages_per_sublk;
	printk(KERN_DEBUG "some_sublk_mask: 0x%08x\r\n", _vfl->geometry.some_sublk_mask);

	_vfl->geometry.banks_total = _vfl->geometry.num_ce * _vfl->geometry.banks_per_ce;
	printk(KERN_DEBUG "banks_total: 0x%08x\r\n", _vfl->geometry.banks_total);

#ifdef YUSTAS_FIXME
	ret = nand_load(NAND_OOB_SIZE, num_ecc_bytes);
	if(ret)
		return EINVAL;

	printk(KERN_ERR "num_ecc_bytes: 0x%08x\r\n", _vfl->geometry.num_ecc_bytes);
#endif

	nand_load(NAND_OOB_ALLOC, bytes_per_spare);
	printk(KERN_DEBUG "bytes_per_spare: 0x%08x\r\n", _vfl->geometry.bytes_per_spare);

#ifdef YUSTAS_FIXME
	ret = nand_load(diReturnOne, one);
	if(ret)
		return EINVAL;

	printk(KERN_ERR "one: 0x%08x\r\n", _vfl->geometry.one);
#endif

	if(_vfl->geometry.num_ce != 1)
	{
		_vfl->geometry.some_crazy_val =	_vfl->geometry.blocks_per_ce
			- 27 - _vfl->geometry.reserved_blocks - _vfl->geometry.some_page_mask;
	}
	else
	{
		_vfl->geometry.some_crazy_val =	_vfl->geometry.blocks_per_ce - 27
			- _vfl->geometry.some_page_mask - (_vfl->geometry.reserved_blocks & 0xFFFF);
	}

	printk(KERN_DEBUG "some_crazy_val: 0x%08x\r\n", _vfl->geometry.some_crazy_val);

	_vfl->geometry.vfl_blocks = _vfl->geometry.some_crazy_val + 4;
	printk(KERN_DEBUG "vfl_blocks: 0x%08x\r\n", _vfl->geometry.vfl_blocks);

	_vfl->geometry.fs_start_block = _vfl->geometry.vfl_blocks + _vfl->geometry.reserved_blocks;
	printk(KERN_DEBUG "fs_start_block: 0x%08x\r\n", _vfl->geometry.fs_start_block);

#ifdef YUSTAS_FIXME
	nand->set(nand, diVendorType, val);

	val = 0x10001;
	nand->set(nand, NAND_BANKS_PER_CE_VFL, val);
#endif

#undef nand_load

	return 0;
}

static error_t vfl_vsvfl_open(vfl_device_t *_vfl)
{
	vfl_vsvfl_device_t *vfl = container_of(_vfl, vfl_vsvfl_device_t, vfl);
	uint32_t ce = 0;
	uint32_t vendorType;
	uint32_t banksPerCE;
	uint32_t bank, i;
	uint32_t num_reserved;
	uint32_t num_non_reserved;
	vfl_vsvfl_context_t *latestCxt;
	int ret;

	ret = vfl_vsvfl_setup_geometry(vfl);
	if(ret)
		return ret;

	vfl->contexts = kzalloc(vfl->geometry.num_ce * sizeof(vfl_vsvfl_context_t), GFP_KERNEL);

	vfl->pageBuffer = (uint32_t*) kmalloc(vfl->geometry.pages_per_block * sizeof(uint32_t), GFP_KERNEL);
	vfl->chipBuffer = (uint16_t*) kmalloc(vfl->geometry.pages_per_block * sizeof(uint16_t), GFP_KERNEL);
	vfl->blockBuffer = (uint16_t*) kmalloc(vfl->geometry.banks_total * sizeof(uint16_t), GFP_KERNEL);
	vfl->stats = kzalloc(sizeof(VSVFLStats), GFP_KERNEL);

	for(ce = 0; ce < vfl->geometry.num_ce; ce++) {
		vfl_vsvfl_context_t *curVFLCxt;
		uint8_t* pageBuffer;
		uint8_t* spareBuffer;
		int i;
		int minUsn;
		int VFLCxtIdx;
		int page;
		int last;

//		vfl->bbt[ce] = (uint8_t*) kmalloc(CEIL_DIVIDE(vfl->geometry.blocks_per_ce, 8), GFP_KERNEL);
		vfl->bbt[ce] = (uint8_t*) kmalloc((vfl->geometry.blocks_per_ce + 7) / 8, GFP_KERNEL);

		printk(KERN_ERR "vsvfl: Checking CE %d.\r\n", ce);

		if(apple_nand_special_page(ce, "DEVICEINFOBBT\0\0\0",
						vfl->bbt[ce], (vfl->geometry.blocks_per_ce + 7) / 8))
		{
			printk(KERN_ERR "vsvfl: Failed to find DEVICEINFOBBT!\r\n");
			return EIO;
		}

		if(ce >= vfl->geometry.num_ce)
			return EIO;

		curVFLCxt = &vfl->contexts[ce];
		pageBuffer = kmalloc(vfl->geometry.bytes_per_page, GFP_KERNEL);
		spareBuffer = kmalloc(vfl->geometry.bytes_per_spare, GFP_KERNEL);
		if(pageBuffer == NULL || spareBuffer == NULL) {
			printk(KERN_ERR "ftl: cannot allocate page and spare buffer\r\n");
			return ENOMEM;
		}

		// Any VFLCxt page will contain an up-to-date list of all blocks used to store VFLCxt pages. Find any such
		// page in the system area.

		for(i = vfl->geometry.reserved_blocks; i < vfl->geometry.fs_start_block; i++) {
			// so pstBBTArea is a bit array of some sort
			if(!(vfl->bbt[ce][i / 8] & (1 << (i  & 0x7))))
				continue;

			if(!apple_nand_read_page(ce, i * vfl->geometry.pages_per_block, pageBuffer, spareBuffer, 0))
			{
				/*
				 * YUSTAS_FIXME: type of VFL cxt is 0x91
				 * Is that correct?
				 */
//				SpareData* spareData = (SpareData*) spareBuffer;
//				if(spareData->field_8 != 0x94 && spareData->type == 0x91) {
					memcpy(curVFLCxt->vfl_context_block, ((vfl_vsvfl_context_t*)pageBuffer)->vfl_context_block,
							sizeof(curVFLCxt->vfl_context_block));
					break;
//				}
			}
		}

//			print_hex_dump(KERN_INFO, "data: ", DUMP_PREFIX_OFFSET, 32,
//					1, pageBuffer, vfl->geometry.bytes_per_page, true);
//			print_hex_dump(KERN_INFO, "spare: ", DUMP_PREFIX_OFFSET, 32,
//					1, spareBuffer, vfl->geometry.bytes_per_spare, true);
		if(i == vfl->geometry.fs_start_block) {
			printk(KERN_ERR "vsvfl: cannot find readable VFLCxtBlock\r\n");
			kfree(pageBuffer);
			kfree(spareBuffer);
			return EIO;
		}

		// Since VFLCxtBlock is a ringbuffer, if blockA.page0.spare.usnDec < blockB.page0.usnDec, then for any page a
		// in blockA and any page b in blockB, a.spare.usNDec < b.spare.usnDec. Therefore, to begin finding the
		// page/VFLCxt with the lowest usnDec, we should just look at the first page of each block in the ring.
		minUsn = 0xFFFFFFFF;
		VFLCxtIdx = 4;
		for(i = 0; i < 4; i++) {
			uint16_t block = curVFLCxt->vfl_context_block[i];
			vfl_vsvfl_spare_data_t *spareData;
			if(block == 0xFFFF)
				continue;

			if(apple_nand_read_page(ce, block * vfl->geometry.pages_per_block, pageBuffer, spareBuffer, 0))
				continue;

			spareData = (vfl_vsvfl_spare_data_t*)spareBuffer;

			if(spareData->meta.usnDec > 0 && spareData->meta.usnDec <= minUsn) {
				minUsn = spareData->meta.usnDec;
				VFLCxtIdx = i;
			}
		}

		if(VFLCxtIdx == 4) {
			printk(KERN_ERR "vsvfl: cannot find readable VFLCxtBlock index in spares\r\n");
			kfree(pageBuffer);
			kfree(spareBuffer);
			return EIO;
		}

		// VFLCxts are stored in the block such that they are duplicated 8 times. Therefore, we only need to
		// read every 8th page, and nand_readvfl_cxt_page will try the 7 subsequent pages if the first was
		// no good. The last non-blank page will have the lowest spare.usnDec and highest usnInc for VFLCxt
		// in all the land (and is the newest).
		page = 8;
		last = 0;
		for(page = 8; page < vfl->geometry.pages_per_block; page += 8) {
			if(apple_nand_read_page(ce, curVFLCxt->vfl_context_block[VFLCxtIdx] * vfl->geometry.pages_per_block +  page, pageBuffer, spareBuffer, 0) != 0) {
				break;
			}

			last = page;
		}

		if(apple_nand_read_page(ce, curVFLCxt->vfl_context_block[VFLCxtIdx] * vfl->geometry.pages_per_block + last, pageBuffer, spareBuffer, 0) != 0) {
			printk(KERN_ERR "vsvfl: cannot find readable VFLCxt\n");
			kfree(pageBuffer);
			kfree(spareBuffer);
			return -1;
		}

		// Aha, so the upshot is that this finds the VFLCxt and copies it into vfl->contexts
		memcpy(&vfl->contexts[ce], pageBuffer, sizeof(vfl_vsvfl_context_t));

		// This is the newest VFLCxt across all CEs
		if(curVFLCxt->usn_inc >= vfl->current_version) {
			vfl->current_version = curVFLCxt->usn_inc;
		}

		kfree(pageBuffer);
		kfree(spareBuffer);

		// Verify the checksum
		if(vfl_check_checksum(vfl, ce) == 0)
		{
			printk(KERN_ERR "vsvfl: VFLCxt has bad checksum.\r\n");
			return EIO;
		}
	}

	// retrieve some global parameters from the latest VFL across all CEs.
	latestCxt = get_most_updated_context(vfl);

	// Then we update the VFLCxts on every ce with that information.
	for(ce = 0; ce < vfl->geometry.num_ce; ce++) {
		// Don't copy over own data.
		if(&vfl->contexts[ce] != latestCxt) {
			// Copy the data, and generate the new checksum.
			memcpy(vfl->contexts[ce].control_block, latestCxt->control_block, sizeof(latestCxt->control_block));
			vfl->contexts[ce].usable_blocks_per_bank = latestCxt->usable_blocks_per_bank;
			vfl->contexts[ce].reserved_block_pool_start = latestCxt->reserved_block_pool_start;
			vfl->contexts[ce].ftl_type = latestCxt->ftl_type;
			memcpy(vfl->contexts[ce].field_6CA, latestCxt->field_6CA, sizeof(latestCxt->field_6CA));

			vfl_gen_checksum(vfl, ce);
		}
	}

	// Vendor-specific virtual-from/to-physical functions.
	// Note: support for some vendors is still missing.
	vendorType = vfl->contexts[0].vendor_type;

	if(!vendorType) {
#ifdef YUSTAS_FIXME
		if(nand_device_get_info(_nand, diVendorType, &vendorType, sizeof(vendorType)))
			return EIO;
#else
		vendorType = 0x10001;
#endif
	}

	switch(vendorType) {
	case 0x10001:
		vfl->geometry.banks_per_ce = 1;
		vfl->virtual_to_physical = virtual_to_physical_10001;
		vfl->physical_to_virtual = physical_to_virtual_10001;
		break;
	
	case 0x100010:
	case 0x100014:
	case 0x120014:
		vfl->geometry.banks_per_ce = 2;
		vfl->virtual_to_physical = virtual_to_physical_100014;
		vfl->physical_to_virtual = physical_to_virtual_100014;
		break;

	case 0x150011:
		vfl->geometry.banks_per_ce = 2;
		vfl->virtual_to_physical = virtual_to_physical_150011;
		vfl->physical_to_virtual = physical_to_virtual_150011;
		break;

	default:
		printk(KERN_ERR "vsvfl: unsupported vendor 0x%06x\r\n", vendorType);
		return EIO;
	}

#ifdef YUSTAS_FIXME
	if(nand_device_set_info(_nand, diVendorType, &vendorType, sizeof(vendorType)))
		return EIO;
#endif

	vfl->geometry.pages_per_sublk = vfl->geometry.pages_per_block * vfl->geometry.banks_per_ce * vfl->geometry.num_ce;
	vfl->geometry.banks_total = vfl->geometry.num_ce * vfl->geometry.banks_per_ce;
	vfl->geometry.blocks_per_bank_vfl = vfl->geometry.blocks_per_ce / vfl->geometry.banks_per_ce;

	banksPerCE = vfl->geometry.banks_per_ce;
//	_nand->set(_nand, NAND_BANKS_PER_CE_VFL, banksPerCE);

	printk(KERN_ERR "vsvfl: detected chip vendor 0x%06x\r\n", vendorType);

	// Now, discard the old scfg bad-block table, and set it using the VFL context's reserved block pool map.
	num_reserved = vfl->contexts[0].reserved_block_pool_start;
	num_non_reserved = vfl->geometry.blocks_per_bank_vfl - num_reserved;

	for(ce = 0; ce < vfl->geometry.num_ce; ce++) {
//		memset(vfl->bbt[ce], 0xFF, CEIL_DIVIDE(vfl->geometry.blocks_per_ce, 8));
		memset(vfl->bbt[ce], 0xFF, (vfl->geometry.blocks_per_ce + 7) / 8);

		for(bank = 0; bank < banksPerCE; bank++) {
			for(i = 0; i < num_non_reserved; i++) {
				uint16_t mapEntry = vfl->contexts[ce].reserved_block_pool_map[bank * num_non_reserved + i];
				uint32_t pBlock;

				if(mapEntry == 0xFFF0)
					continue;

				if(mapEntry < vfl->geometry.blocks_per_ce) {
					pBlock = mapEntry;
				} else if(mapEntry > 0xFFF0) {
					virtual_block_to_physical_block(vfl, ce + bank * vfl->geometry.num_ce, num_reserved + i, &pBlock);
				} else {
					printk(KERN_DEBUG "PANIC!!!: vsvfl: bad map table: CE %d, entry %d, value 0x%08x\r\n",
						ce, bank * num_non_reserved + i, mapEntry);
				}

				vfl->bbt[ce][pBlock / 8] &= ~(1 << (pBlock % 8));
			}
		}
	}

	printk(KERN_ERR "vsvfl: VFL successfully opened!\r\n");

	return 0;
}

inline void auto_store(void *_ptr, size_t _sz, uint32_t _val)
{
	switch(_sz)
	{
	case 0:
		return;

	case 1:
		*((uint8_t*)_ptr) = _val;
		return;

	case 2:
		*((uint16_t*)_ptr) = _val;
		return;

	case 4:
		*((uint32_t*)_ptr) = _val;
		return;
	}
}

static error_t vfl_vsvfl_get_info(vfl_device_t *_vfl, vfl_info_t _item, void *_result, size_t _sz)
{
	vfl_vsvfl_device_t *vfl = container_of(_vfl, vfl_vsvfl_device_t, vfl);

	if(_sz > 4 || _sz == 3) {
		return EINVAL;
	}

	switch(_item) {
	case diPagesPerBlockTotalBanks:
		auto_store(_result, _sz, vfl->geometry.pages_per_sublk);
		return 0;

	case diSomeThingFromVFLCXT:
		auto_store(_result, _sz, vfl->contexts[0].usable_blocks_per_bank);
		return 0;

	case diFTLType:
		auto_store(_result, _sz, vfl->contexts[0].ftl_type);
		return 0;

	case diBytesPerPageFTL:
		auto_store(_result, _sz, apple_nand_get_info(NAND_PAGE_SIZE));
		return 0;

	case diMetaBytes0xC:
		auto_store(_result, _sz, 0xC);
		return 0;

	case diUnkn20_1:
		auto_store(_result, _sz, 1);
		return 0;

	case diTotalBanks:
		auto_store(_result, _sz, apple_nand_get_info(NAND_BANKS_PER_CE/*_VFL*/) 
				* apple_nand_get_num_ce());
		return 0;

	default:
		return ENOENT;
	}
}

#if 0
static struct apple_nand *vfl_vsvfl_get_device(vfl_device_t *_vfl)
{
	vfl_vsvfl_device_t *vfl = container_of(_vfl, vfl_vsvfl_device_t, vfl);
	return vfl->device;
}
#endif
static void* *vfl_vsvfl_get_stats(vfl_device_t *_vfl, uint32_t *size)
{
	vfl_vsvfl_device_t *vfl = container_of(_vfl, vfl_vsvfl_device_t, vfl);
	if(size)
		*size = sizeof(VSVFLStats);
	return (void*)vfl->stats;
}

error_t vfl_vsvfl_device_init(vfl_vsvfl_device_t *_vfl)
{
	memset(_vfl, 0, sizeof(*_vfl));

	//memset(&VFLData1, 0, sizeof(VFLData1));

	_vfl->current_version = 0;
	_vfl->vfl.open = vfl_vsvfl_open;

//	_vfl->vfl.get_device = vfl_vsvfl_get_device;
	_vfl->vfl.get_stats = vfl_vsvfl_get_stats;

	_vfl->vfl.read_single_page = vfl_vsvfl_read_single_page;

	_vfl->vfl.write_single_page = vfl_vsvfl_write_single_page;

	_vfl->vfl.erase_single_block = vfl_vsvfl_erase_single_block;

	_vfl->vfl.write_context = vfl_vsvfl_write_context;

	_vfl->vfl.get_ftl_ctrl_block = VFL_get_FTLCtrlBlock;

	_vfl->vfl.get_info = vfl_vsvfl_get_info;

	memset(&_vfl->geometry, 0, sizeof(_vfl->geometry));

#if defined(CONFIG_CPU_S5L8930) && !defined(CONFIG_MACH_IPAD_1G)
	_vfl->geometry.reserved_blocks = 16;
#else
	_vfl->geometry.reserved_blocks = 1;
#endif

	return 0;
}

void vfl_vsvfl_device_cleanup(vfl_vsvfl_device_t *_vfl)
{
	uint32_t i;

	if(_vfl->contexts)
		kfree(_vfl->contexts);

	for (i = 0; i < sizeof(_vfl->bbt) / sizeof(void*); i++) {
		if(_vfl->bbt[i])
			kfree(_vfl->bbt[i]);
	}

	if(_vfl->pageBuffer)
		kfree(_vfl->pageBuffer);

	if(_vfl->chipBuffer)
		kfree(_vfl->chipBuffer);

	if(_vfl->blockBuffer)
		kfree(_vfl->blockBuffer);

	memset(_vfl, 0, sizeof(*_vfl));
}

vfl_vsvfl_device_t *vfl_vsvfl_device_allocate()
{
	vfl_vsvfl_device_t *ret = kzalloc(sizeof(*ret), GFP_KERNEL);
	vfl_vsvfl_device_init(ret);
	return ret;
}

