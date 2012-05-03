#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/sched.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include "vfl.h"
#include "ftl.h"

// 2^9 = 512
#define SECTOR_SHIFT 9

#ifdef YUSTAS_FIXME
extern NANDData* NANDGeometry;
#endif

static struct
{
	vfl_device_t *h2fmi_vfl_device;
	ftl_device_t *h2fmi_ftl_device;

	spinlock_t lock;
	struct gendisk* gd;
	struct block_device* bdev;
	struct request_queue* queue;
	int sectorSize;
	int pageShift;
	int majorNum;

	struct request* req;
	bool processing;
	u8* bounceBuffer;
} apple_a4_block_device;

static void ftl_workqueue_handler(struct work_struct* work);

DECLARE_WORK(ftl_workqueue, &ftl_workqueue_handler);
static struct workqueue_struct* ftl_wq;

static void apple_a4_block_scatter_gather(struct request* req, bool gather)
{
	unsigned int offset = 0;
	struct req_iterator iter;
	struct bio_vec *bvec;
	unsigned int i = 0;
	size_t size;
	void *buf;

	rq_for_each_segment(bvec, req, iter) {
		unsigned long flags;
		//printk("%s:%u: bio %u: %u segs %u sectors from %lu\n",
		//		__func__, __LINE__, i, bio_segments(iter.bio),
		//		bio_sectors(iter.bio), (unsigned long) iter.bio->bi_sector);

		size = bvec->bv_len;
		buf = bvec_kmap_irq(bvec, &flags);
		if (gather)
			memcpy(apple_a4_block_device.bounceBuffer + offset, buf, size);
		else
			memcpy(buf, apple_a4_block_device.bounceBuffer + offset, size);
		offset += size;
		flush_kernel_dcache_page(bvec->bv_page);
		bvec_kunmap_irq(buf, &flags);
		i++;
	}

	//printk("scatter_gather total: %d / %d\n", offset, NANDGeometry->pagesPerSuBlk * NANDGeometry->bytesPerPage);

}

static void ftl_workqueue_handler(struct work_struct* work)
{
	unsigned long flags;
	bool dir_out;
	int ret;

	//printk("ftl_workqueue_handler enter\n");

	while(true)
	{
		u32 lpn;
		u32 numPages;
		u32 remainder;

		//printk("ftl_workqueue_handler loop\n");

		spin_lock_irqsave(&apple_a4_block_device.lock, flags);
		if(apple_a4_block_device.req == NULL || apple_a4_block_device.processing)
		{
			spin_unlock_irqrestore(&apple_a4_block_device.lock, flags);
			//printk("ftl_workqueue_handler exit\n");
			return;
		}

		apple_a4_block_device.processing = true;
		spin_unlock_irqrestore(&apple_a4_block_device.lock, flags);

		if(apple_a4_block_device.req->cmd_type == REQ_TYPE_FS)
		{
			lpn = blk_rq_pos(apple_a4_block_device.req) >> (apple_a4_block_device.pageShift - SECTOR_SHIFT);
			numPages = blk_rq_bytes(apple_a4_block_device.req) / apple_a4_block_device.sectorSize;
			remainder = numPages * apple_a4_block_device.sectorSize - blk_rq_bytes(apple_a4_block_device.req);

			if(remainder)
			{
				printk("apple_a4_block: requested not page aligned number of bytes (%d bytes)\n", blk_rq_bytes(apple_a4_block_device.req));
				blk_end_request_all(apple_a4_block_device.req, -EINVAL);
			} else
			{
				if(rq_data_dir(apple_a4_block_device.req))
				{
					dir_out = true;
					apple_a4_block_scatter_gather(apple_a4_block_device.req, true);
				} else
				{
					dir_out = false;
				}


				if(dir_out)
				{
					//printk("FTL_Write enter: %p\n", apple_a4_block_device.req);
#ifdef YUSTAS_FIXME
					ret = FTL_Write(lpn, numPages, apple_a4_block_device.bounceBuffer);
#endif
					//printk("FTL_Write exit: %p\n", apple_a4_block_device.req);
				} else
				{
					//printk("FTL_Read enter: %p\n", apple_a4_block_device.req);
#ifdef YUSTAS_FIXME
					ret = FTL_Read(lpn, numPages, apple_a4_block_device.bounceBuffer);
#endif
					//printk("FTL_Read exit: %p\n", apple_a4_block_device.req);
				}

				if(!dir_out)
				{
					apple_a4_block_scatter_gather(apple_a4_block_device.req, false);
				}

				blk_end_request_all(apple_a4_block_device.req, ret);
			}
		} else
		{
			blk_end_request_all(apple_a4_block_device.req, -EINVAL);
		}
		spin_lock_irqsave(&apple_a4_block_device.lock, flags);
		apple_a4_block_device.processing = false;
		apple_a4_block_device.req = blk_fetch_request(apple_a4_block_device.queue);
		spin_unlock_irqrestore(&apple_a4_block_device.lock, flags);
	}
}

static int apple_a4_block_busy(struct request_queue *q)
{
	int ret = (apple_a4_block_device.req == NULL) ? 0 : 1;
	printk("apple_a4_block_busy: %d\n", ret);
	return ret;
}

static int apple_a4_block_getgeo(struct block_device* bdev, struct hd_geometry* geo)
{
#ifdef YUSTAS_FIXME
	long size = (NANDGeometry->pagesPerSuBlk * NANDGeometry->userSuBlksTotal) * (apple_a4_block_device.sectorSize >> SECTOR_SHIFT);

	geo->heads = 64;
	geo->sectors = 32;
	geo->cylinders = size / (geo->heads * geo->sectors);
#endif

	return 0;
}

static int apple_a4_block_open(struct block_device* bdev, fmode_t mode)
{
	return 0;
}

static int apple_a4_block_release(struct gendisk *disk, fmode_t mode)
{
#ifdef YUSTAS_FIXME
	ftl_sync();
#endif
	return 0;
}

static int apple_a4_block_ioctl(struct block_device *bdev, fmode_t mode, unsigned int cmd, unsigned long arg)
{
	switch(cmd)
	{
		case BLKFLSBUF:
#ifdef YUSTAS_FIXME
			ftl_sync();
#endif
			return 0;
		default:
			return -ENOTTY;
	}
}

static void apple_a4_block_request(struct request_queue* q)
{
	if(apple_a4_block_device.req)
	{
		//printk("not queueing request due to busy\n");
		return;
	}

	apple_a4_block_device.req = blk_fetch_request(q);

	//printk("scheduling work: %p\n", apple_a4_block_device.req);
	queue_work(ftl_wq, &ftl_workqueue);
}

static struct block_device_operations apple_a4_block_fops =
{
	.owner		= THIS_MODULE,
	.getgeo		= apple_a4_block_getgeo,
	.open		= apple_a4_block_open,
	.release	= apple_a4_block_release,
	.ioctl		= apple_a4_block_ioctl
};

static int apple_a4_block_probe(struct platform_device *pdev)
{
	int i;

	ftl_wq = create_workqueue("apple_a4_ftl_worker");

	if (vfl_detect(&apple_a4_block_device.h2fmi_vfl_device)) {
		printk("apple_a4-block: failed to open VFL\n");
		return -ENODEV;
	}
	if (ftl_detect(&apple_a4_block_device.h2fmi_ftl_device, 
				apple_a4_block_device.h2fmi_vfl_device)) {
		printk("apple_a4-block: failed to open FTL\n");
		return -ENODEV;
	}
#ifdef YUSTAS_FIXME
	for(i = 0; i < 31; ++i)
	{
		if((1 << i) == NANDGeometry->bytesPerPage)
		{
			apple_a4_block_device.pageShift = i;
			break;
		}
	}

	spin_lock_init(&apple_a4_block_device.lock);

	apple_a4_block_device.processing = false;
	apple_a4_block_device.req = NULL;

	apple_a4_block_device.bounceBuffer = (u8*) kmalloc(NANDGeometry->pagesPerSuBlk * NANDGeometry->bytesPerPage, GFP_KERNEL | GFP_DMA);
	if(!apple_a4_block_device.bounceBuffer)
		return -EIO;

	apple_a4_block_device.sectorSize = NANDGeometry->bytesPerPage;
	apple_a4_block_device.majorNum = register_blkdev(0, "nand");

	apple_a4_block_device.gd = alloc_disk(5);
	if(!apple_a4_block_device.gd)
		goto out_unregister;

	apple_a4_block_device.gd->major = apple_a4_block_device.majorNum;
	apple_a4_block_device.gd->first_minor = 0;
	apple_a4_block_device.gd->fops = &apple_a4_block_fops;
	apple_a4_block_device.gd->private_data = &apple_a4_block_device;
	strcpy(apple_a4_block_device.gd->disk_name, "nand0");

	apple_a4_block_device.queue = blk_init_queue(apple_a4_block_request, &apple_a4_block_device.lock);
	if(!apple_a4_block_device.queue)
		goto out_put_disk;

	blk_queue_lld_busy(apple_a4_block_device.queue, apple_a4_block_busy);
	blk_queue_bounce_limit(apple_a4_block_device.queue, BLK_BOUNCE_ANY);
	blk_queue_max_hw_sectors(apple_a4_block_device.queue, NANDGeometry->pagesPerSuBlk * (apple_a4_block_device.sectorSize >> SECTOR_SHIFT));
	blk_queue_max_segment_size(apple_a4_block_device.queue, NANDGeometry->pagesPerSuBlk * apple_a4_block_device.sectorSize);
	blk_queue_physical_block_size(apple_a4_block_device.queue, apple_a4_block_device.sectorSize);
	blk_queue_logical_block_size(apple_a4_block_device.queue, apple_a4_block_device.sectorSize);
	apple_a4_block_device.gd->queue = apple_a4_block_device.queue;

	set_capacity(apple_a4_block_device.gd, (NANDGeometry->pagesPerSuBlk * NANDGeometry->userSuBlksTotal) * (apple_a4_block_device.sectorSize >> SECTOR_SHIFT));
	add_disk(apple_a4_block_device.gd);

#endif
	printk("apple_a4-block: block device registered with major num %d\n", apple_a4_block_device.majorNum);

	return 0;

out_put_disk:
	put_disk(apple_a4_block_device.gd);

out_unregister:
	unregister_blkdev(apple_a4_block_device.majorNum, "nand");
	kfree(apple_a4_block_device.bounceBuffer);

	return -ENOMEM;
}

static int apple_a4_block_remove(struct platform_device *pdev)
{
	del_gendisk(apple_a4_block_device.gd);
	put_disk(apple_a4_block_device.gd);
	blk_cleanup_queue(apple_a4_block_device.queue);
	unregister_blkdev(apple_a4_block_device.majorNum, "nand");
	flush_workqueue(ftl_wq);
	kfree(apple_a4_block_device.bounceBuffer);
#ifdef YUSTAS_FIXME
	ftl_sync();
#endif
	printk("apple_a4-block: block device unregistered\n");
	return 0;
}

static void apple_a4_block_shutdown(struct platform_device *pdev)
{
#ifdef YUSTAS_FIXME
	ftl_sync();
#endif
}

static struct platform_driver apple_a4_block_driver = {
	.probe = apple_a4_block_probe,
	.remove = apple_a4_block_remove,
	.suspend = NULL, /* optional but recommended */
	.resume = NULL,   /* optional but recommended */
	.shutdown = apple_a4_block_shutdown,
	.driver = {
		.owner = THIS_MODULE,
		.name = "apple_a4-block",
	},
};

static struct platform_device apple_a4_block_dev = {
	.name = "apple_a4-block",
	.id = -1,
};

    /*
     *  Setup
     */

static int __init apple_a4_block_init(void)
{
	int ret;

	ret = platform_driver_register(&apple_a4_block_driver);

	if (!ret) {
		ret = platform_device_register(&apple_a4_block_dev);

		if (ret != 0) {
			platform_driver_unregister(&apple_a4_block_driver);
		}
	}

	return ret;
}

static void __exit apple_a4_block_exit(void)
{
	platform_device_unregister(&apple_a4_block_dev);
	platform_driver_unregister(&apple_a4_block_driver);
}

module_init(apple_a4_block_init);
module_exit(apple_a4_block_exit);

