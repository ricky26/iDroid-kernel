#include <linux/module.h>
#include <linux/slab.h>
#include <linux/apple_flash.h>

//
// NAND
//
#define TOTAL_CHIPS_COUNT	2
struct apple_nand *chips[TOTAL_CHIPS_COUNT];
int total_chips = 0;
int banks_per_ce_vfl;
int vendor_type;

struct apple_nand* get_apple_nand(u16 ce)
{
	if (ce >= total_chips) {
		printk(KERN_ERR "%s: wrong chip number.\n", __FUNCTION__);
		return NULL;
	}
	return chips[ce];
}

int apple_nand_set_data_whitening(int whitening)
{
	int i;
	for (i = 0; i < total_chips; i++)
	{
		struct apple_nand *nand = get_apple_nand(i);
		if (nand->set_whitening(nand, whitening))
			return -ENOENT;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(apple_nand_set_data_whitening);

int register_apple_nand(struct apple_nand *_nd)
{
	/* TODO: for now each bus has only one chip !*/
	if (total_chips == TOTAL_CHIPS_COUNT)
		return -ENOMEM;

	chips[total_chips++] = _nd;
	return 0;
}
EXPORT_SYMBOL_GPL(register_apple_nand);

void remove_apple_nand(struct apple_nand *_nd)
{
}
EXPORT_SYMBOL_GPL(remove_apple_nand);

int apple_nand_get_info(int info)
{
	struct apple_nand *nand = get_apple_nand(0);

	switch(info)
	{
	case NAND_NUM_CE:
		return total_chips;
	case NAND_BANKS_PER_CE_VFL:
		return banks_per_ce_vfl;
	case NAND_VENDOR_TYPE:
		return vendor_type;
	default:
		return nand->get(nand, info);
	}
}
EXPORT_SYMBOL_GPL(apple_nand_get_info);

int apple_nand_set_info(int info, int val)
{
	switch(info)
	{
	case NAND_BANKS_PER_CE_VFL:
		banks_per_ce_vfl = val;
		return 0;
	case NAND_VENDOR_TYPE:
		vendor_type = val;
		return 0;
	default:
		return -EINVAL;
	}
}
EXPORT_SYMBOL_GPL(apple_nand_set_info);

int apple_nand_special_page(u16 _ce, char _page[16],
		uint8_t* _buffer, size_t _amt)
{
	struct apple_nand *_nd = get_apple_nand(_ce);
	int pagesz = _nd->get(_nd, NAND_PAGE_SIZE);
	int bpce = _nd->get(_nd, NAND_BLOCKS_PER_CE);
	int bpb = _nd->get(_nd, NAND_BLOCKS_PER_BANK);
	int ppb = _nd->get(_nd, NAND_PAGES_PER_BLOCK);
	int baddr = _nd->get(_nd, NAND_BANK_ADDRESS_SPACE);

	u8 *buf = kzalloc(pagesz, GFP_KERNEL);
	int lowestBlock = bpce - (bpce/10);
	int block;
	int ret;

	for(block = bpce-1; block >= lowestBlock; block--)
	{
		int page;
		int badCount = 0;

		int realBlock = (block/bpb) * baddr	+ (block % bpb);
		for(page = 0; page < ppb; page++)
		{
			if(badCount > 2)
				break;

			ret = apple_nand_read_page(_ce,
					(realBlock * ppb) + page, buf, 0, 0);
			if(ret < 0)
			{
				if(ret != -ENOENT)
					badCount++;

				continue;
			}

			//print_hex_dump(KERN_INFO, "h2fmi special-page: ", DUMP_PREFIX_OFFSET, 32,
			//		1, buf, pagesz, true);

			if(memcmp(buf, _page, sizeof(_page)) == 0)
			{
				if(_buffer)
				{
					size_t size = min(((size_t)((size_t*)buf)[13]), _amt);
					memcpy(_buffer, buf + 0x38, size);
				}

				kfree(buf);
				return 0;
			}
		}
	}

	printk(KERN_ERR "apple_nand: failed to find special page %s.\n", _page);
	kfree(buf);
	return -ENOENT;
}
EXPORT_SYMBOL_GPL(apple_nand_special_page);

int apple_nand_read_page(u16 _ce, page_t _page,
		uint8_t *_data, uint8_t *_oob, int disable_aes)
{
	struct scatterlist sg_buf, sg_oob;
	struct apple_nand *_nd = get_apple_nand(_ce);
	u16 chip = 0;

	if(!_nd->read)
		return -EPERM;

	sg_init_one(&sg_buf, _data, _nd->get(_nd, NAND_PAGE_SIZE));
	sg_init_one(&sg_oob, _oob, _nd->get(_nd, NAND_OOB_ALLOC));

	return _nd->read(_nd, 1, &chip, &_page, &sg_buf, 1, &sg_oob, 1, disable_aes);
}
EXPORT_SYMBOL_GPL(apple_nand_read_page);

int apple_nand_write_page(u16 _ce, page_t _page,
		const uint8_t *_data, const uint8_t *_oob)
{
	struct scatterlist sg_buf, sg_oob;
	struct apple_nand *_nd = get_apple_nand(_ce);
	u16 chip = 0;

	if(!_nd->write)
		return -EPERM;

	sg_init_one(&sg_buf, _data, _nd->get(_nd, NAND_PAGE_SIZE));
	sg_init_one(&sg_oob, _oob, _nd->get(_nd, NAND_OOB_ALLOC));

	return _nd->write(_nd, 1, &chip, &_page, &sg_buf, 1, &sg_oob, 1);
}
EXPORT_SYMBOL_GPL(apple_nand_write_page);

int apple_nand_erase_block(u16 _ce, page_t _page)
{
	struct apple_nand *_nd = get_apple_nand(_ce);
	u16 chip = 0;
	if(!_nd->erase)
		return -EPERM;

	return _nd->erase(_nd, 1, &chip, &_page);
}
EXPORT_SYMBOL_GPL(apple_nand_erase_block);

//
// VFL
//

int register_apple_vfl(struct apple_vfl *_vfl)
{
	return 0;
}
EXPORT_SYMBOL_GPL(register_apple_vfl);

void remove_apple_vfl(struct apple_vfl *_vfl)
{
}
EXPORT_SYMBOL_GPL(remove_apple_vfl);

int apple_vfl_read_page(struct apple_vfl *_vfl, page_t _page,
		uint8_t *_data, uint8_t *_oob)
{
	struct scatterlist sg_buf, sg_oob;

	if(!_vfl->read)
		return -EPERM;

	sg_init_one(&sg_buf, _data, _vfl->get(_vfl, NAND_PAGE_SIZE));
	sg_init_one(&sg_oob, _oob, _vfl->get(_vfl, NAND_OOB_ALLOC));

	return _vfl->read(_vfl, 1, &_page, &sg_buf, 1, &sg_oob, 1);
}
EXPORT_SYMBOL_GPL(apple_vfl_read_page);

int apple_vfl_write_page(struct apple_vfl *_vfl, page_t _page,
		const uint8_t *_data, const uint8_t *_oob)
{
	struct scatterlist sg_buf, sg_oob;

	if(!_vfl->write)
		return -EPERM;

	sg_init_one(&sg_buf, _data, _vfl->get(_vfl, NAND_PAGE_SIZE));
	sg_init_one(&sg_oob, _oob, _vfl->get(_vfl, NAND_OOB_ALLOC));

	return _vfl->write(_vfl, 1, &_page, &sg_buf, 1, &sg_oob, 1);
}
EXPORT_SYMBOL_GPL(apple_vfl_write_page);

//
// FTL
//

int register_apple_ftl(struct apple_ftl *_ftl)
{
	return 0;
}
EXPORT_SYMBOL_GPL(register_apple_ftl);

void remove_apple_ftl(struct apple_ftl *_ftl)
{
}
EXPORT_SYMBOL_GPL(remove_apple_ftl);

MODULE_AUTHOR("Ricky Taylor <rickytaylor26@gmail.com>");
MODULE_DESCRIPTION("API for Apple Mobile Device NAND.");
MODULE_LICENSE("GPL");
