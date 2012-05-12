/**
 * Copyright (c) 2011 Richard Ian Taylor.
 *
 * This file is part of the iDroid Project. (http://www.idroidproject.org).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/fb.h>
#include <linux/dma-mapping.h>
#include <linux/mm.h>

#include <plat/display-pipe.h>
#include <mach/regs-display-pipe.h>

//#define DISPLAY_PIPE_DUMP_DEGISTERS

struct s5l_display_pipe_ui
{
	struct s5l_display_pipe_state *state;
	struct fb_info *info;

	void *buffer;
	size_t buffer_size;

	u32 index;
	void *__iomem regs;

	u32 pseudo_palette[16];
};

static int display_pipe_setcolreg(unsigned _idx,
		unsigned _r, unsigned _g, unsigned _b, unsigned _a,
		struct fb_info *_info)
{
	if(_idx < 16)
	{
		u32 *pal = _info->pseudo_palette;
		pal[_idx] = (_r << 16) | (_b << 8) | _g;

		return 0;
	}

	return 1;
}

static struct fb_ops display_pipe_fb_ops = {
	.owner		= THIS_MODULE,

	.fb_setcolreg	= display_pipe_setcolreg,
	.fb_fillrect	= cfb_fillrect,
	.fb_copyarea	= cfb_copyarea,
	.fb_imageblit	= cfb_imageblit,
};

#ifdef DISPLAY_PIPE_DUMP_REGISTERS
static void display_pipe_dump_registers(struct s5l_display_pipe_state *_state)
{
	int i;

	printk("Display Pipe regs:\n");

	printk("control regs:\n");
	for(i = 0; i < 24; i++)
		printk("0x%08x: 0x%08x\n", _state->regs + 0x1000 + (i*4), readl(_state->regs + 0x1000 + (i*4)));

	printk("blend regs:\n");
	for(i = 0; i < 26; i++)
		printk("0x%08x: 0x%08x\n", _state->regs + 0x1000 + (i*4), readl(_state->regs + 0x2000 + (i*4)));

	printk("ui0 regs:\n");
	for(i = 0; i < 31; i++)
		printk("0x%08x: 0x%08x\n", _state->regs + 0x1000 + (i*4), readl(_state->regs + 0x4000 + (i*4)));

	printk("ui1 regs:\n");
	for(i = 0; i < 31; i++)
		printk("0x%08x: 0x%08x\n", _state->regs + 0x1000 + (i*4), readl(_state->regs + 0x5000 + (i*4)));
}
#endif

struct resource *display_pipe_get_resource(struct s5l_display_pipe_info *_info, unsigned int _type, int _idx)
{
	int i;
	for (i = 0; i < _info->num_resources; i++) {
		struct resource *r = &_info->resource[i];

		if (_type == resource_type(r) && _idx-- == 0)
			return r;
	}
	return NULL;
}
EXPORT_SYMBOL_GPL(display_pipe_get_resource);

int display_pipe_configure_window(struct s5l_display_pipe_state *_state, u32 _idx,
	u32 _width, u32 _height, u8 _bpp)
{
	u32 bpp_reg;
	struct s5l_display_pipe_ui *ui = _state->uis[_idx];
	struct fb_info *fbinfo;
	bool reg = false;

	if(!ui)
	{
		reg = true;

		fbinfo = framebuffer_alloc(sizeof(struct s5l_display_pipe_ui), &_state->pdev->dev);
		if(!fbinfo)
		{
			printk(KERN_ERR "display_pipe: failed to allocate UI.\n");
			return -ENOMEM;
		}
		
		ui = fbinfo->par;
		ui->info = fbinfo;
		ui->state = _state;
		ui->index = _idx;
		ui->regs = _state->regs + S5L_DPUIx(_idx);

		fbinfo->fix.type	= FB_TYPE_PACKED_PIXELS;
		fbinfo->fix.visual = FB_VISUAL_TRUECOLOR;
		fbinfo->fix.accel	= FB_ACCEL_NONE;
		fbinfo->var.activate	= FB_ACTIVATE_NOW;
		fbinfo->var.vmode	= FB_VMODE_NONINTERLACED;
		fbinfo->var.bits_per_pixel = 0;
		fbinfo->var.xres = 0;
		fbinfo->var.yres = 0;
		fbinfo->fbops		= &display_pipe_fb_ops;
		fbinfo->flags		= FBINFO_FLAG_DEFAULT;
		fbinfo->pseudo_palette  = &ui->pseudo_palette;

		fbinfo->var.red.offset = 16;
		fbinfo->var.red.length = 8;
		fbinfo->var.green.offset = 8;
		fbinfo->var.green.length = 8;
		fbinfo->var.blue.offset = 0,
		fbinfo->var.blue.length = 8;

		if(fb_alloc_cmap(&fbinfo->cmap, 256, 0) == 0)
			fb_set_cmap(&fbinfo->cmap, fbinfo);
		else
			printk(KERN_ERR "display_pipe: failed to allocate colour-map!\n");
	}
	else
		fbinfo = ui->info;

	if(_bpp != fbinfo->var.bits_per_pixel)
	{
		switch(_bpp)
		{
		case 32:
			bpp_reg = 0;
			break;

		case 16:
			bpp_reg = 4;
			break;

		default:
			printk("%s: Invalid bpp %d!\n", __func__, _bpp);
			return -EINVAL;
		}

		writel((bpp_reg << 8) | 1, ui->regs + S5L_DPUICTL);
		fbinfo->var.bits_per_pixel = _bpp;
	}

	printk("%s: bpp done.\n", __func__);

	if(_width != fbinfo->var.xres
			|| _height != fbinfo->var.yres)
	{
		u32 fsize = (_bpp * _width * _height)/8;
		dma_addr_t addr;

		if(ui->buffer)
			dma_free_writecombine(&_state->pdev->dev, fbinfo->fix.smem_len,
					ui->buffer, fbinfo->fix.smem_start);

		fbinfo->fix.line_length = (_width * _bpp) / 8;
		fbinfo->fix.smem_len = fsize;
		ui->buffer = dma_alloc_writecombine(&_state->pdev->dev, fbinfo->fix.smem_len,
				&addr, GFP_KERNEL);
		if(!ui->buffer)
			printk(KERN_ERR "display_pipe: failed to allocate framebuffer!\n");

		memset(ui->buffer, 0xff, fbinfo->fix.smem_len);

		writel(addr, ui->regs + S5L_DPUIBUF);
		writel(((_width * (_bpp/8)) &~ 0x3F) | 2, ui->regs + S5L_DPUILEN);
		writel(0, ui->regs + S5L_DPUIUNK0);
		writel((_width << 16) | _height, ui->regs + S5L_DPUISIZE);
		writel(1, ui->regs + S5L_DPUIUNK1);
		writel(0x200060, ui->regs + S5L_DPUIUNK2);
		writel(32, ui->regs + S5L_DPUIUNK3);

		writel(0xFFFF0202, _state->regs + S5L_DPUI0ALPH); // TODO: Figure out this for UI-1.

		fbinfo->fix.smem_start = addr;
		fbinfo->screen_base = ui->buffer;
		fbinfo->var.xres_virtual = fbinfo->var.xres = _width;
		fbinfo->var.yres_virtual = fbinfo->var.yres = _height;
	}
	
	printk("%s: fb done.\n", __func__);

	writel(S5L_DPCTL_UIxEN(_idx) | readl(_state->regs + S5L_DPCTL),
			_state->regs + S5L_DPCTL);

	printk("%s: regs done.\n", __func__);

	if(reg)
		register_framebuffer(fbinfo);
	return 0;
}
EXPORT_SYMBOL_GPL(display_pipe_configure_window);

static int display_pipe_probe(struct platform_device *_dev)
{
	struct s5l_display_pipe_info *info = _dev->dev.platform_data;
	struct s5l_display_pipe_state *state;
	int ret = 0;
	struct resource *res;
	u32 src;

	printk("%s\n", __func__);

	if(info->index == 0)
		src = 0x18001f0;
	else if(info->index == 1)
		src = 0x1c001f0;
	else
	{
		dev_err(&_dev->dev, "invalid display-pipe index %d!\n", info->index);
		return -EIO;
	}

	state = kzalloc(sizeof(struct s5l_display_pipe_state), GFP_KERNEL);
	if(!state)
		return -ENOMEM;

	state->info = info;
	state->pdev = _dev;
	state->clk = clk_get(&_dev->dev, info->clk);
	if(!state->clk || IS_ERR(state->clk))
	{
		ret = -EINVAL;
		goto fail_state;
	}	

	clk_enable(state->clk);

	res = platform_get_resource(_dev, IORESOURCE_MEM, 0);
	if(!res)
	{
		dev_err(&_dev->dev, "no register block specified for display-pipe!\n");
		ret = -EINVAL;
		goto fail_clk;
	}

	state->regs = ioremap(res->start, resource_size(res));
	if(!state->regs)
	{
		dev_err(&_dev->dev, "failed to ioremap register block!\n");
		ret = -EIO;
		goto fail_clk;
	}

	platform_set_drvdata(_dev, state);

	writel(readl(state->regs + S5L_DPCPFDMA) | S5L_DPCPFDMA_AUTO, state->regs + S5L_DPCPFDMA);
	writel((readl(state->regs + S5L_DPCPFDMA) & 0xFFFFF8FF) | 0x100, state->regs + S5L_DPCPFDMA);
	writel((readl(state->regs + S5L_DPCPFDMA) & 0xF800FFFF) | 0x4000000, state->regs + S5L_DPCPFDMA);
	writel((info->width << 16) | info->height, state->regs + S5L_DPOSIZE);

	writel(src, state->regs + S5L_DPFIFOCFG);
	writel(0x90, state->regs + 0x2060);
	writel(0x13880801, state->regs + 0x105C);
	writel(0xBFF00000, state->regs + S5L_DPUNDERC);

	/*if(info->driver->init)
	{
		ret = info->driver->init(state);
		if(ret < 0)
			goto fail_drv;
	}*/

	display_pipe_configure_window(state, 0, info->width, info->height, 32); // Framebuffer is always RGB888

#ifdef DISPLAY_PIPE_DUMP_REGISTERS
	display_pipe_dump_registers(state);
#endif

	printk("display-pipe: %s registered!\n", info->driver->name);

	goto exit;

#if 0
fail_drv:
	platform_set_drvdata(_dev, NULL);
#endif

fail_clk:
	clk_put(state->clk);

fail_state:
	kfree(state);

exit:
	return ret;
}

static int display_pipe_remove(struct platform_device *_dev)
{
	struct s5l_display_pipe_state *state = platform_get_drvdata(_dev);
	struct s5l_display_pipe_info *info = state->info;

	if(info->driver->shutdown)
		info->driver->shutdown(state);

	return 0;
}

static void display_pipe_shutdown(struct platform_device *_dev)
{
	struct s5l_display_pipe_state *state = platform_get_drvdata(_dev);
	struct s5l_display_pipe_info *info = state->info;

	if(info->driver->shutdown)
		info->driver->shutdown(state);
}

#ifdef CONFIG_PM
static int display_pipe_suspend(struct platform_device *_dev, pm_message_t _state)
{
	int ret = 0;
	struct s5l_display_pipe_state *state = platform_get_drvdata(_dev);
	struct s5l_display_pipe_info *info = state->info;

	if(info->driver->suspend)
		ret = info->driver->suspend(state, _state);

	return ret;
}

static int display_pipe_resume(struct platform_device *_dev)
{
	int ret = 0;
	struct s5l_display_pipe_state *state = platform_get_drvdata(_dev);
	struct s5l_display_pipe_info *info = state->info;

	if(info->driver->resume)
		ret = info->driver->resume(state);

	return ret;
}
#else
#define display_pipe_suspend	NULL
#define display_pipe_resume		NULL
#endif

static struct platform_driver display_pipe_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "s5l-display-pipe",
	},

	.probe = display_pipe_probe,
	.remove = display_pipe_remove,
	.shutdown = display_pipe_shutdown,

	.suspend = display_pipe_suspend,
	.resume = display_pipe_resume,
};

static int __init display_pipe_init(void)
{
	return platform_driver_register(&display_pipe_driver);
}
module_init(display_pipe_init);

static void __exit display_pipe_exit(void)
{
	platform_driver_unregister(&display_pipe_driver);
}
module_exit(display_pipe_exit);

MODULE_DESCRIPTION("S5L Dual Display Driver");
MODULE_AUTHOR("Richard Ian Taylor");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:s5l-display-pipe");
