/*
 *  linux/drivers/video/mdacon.c -- Low level MDA based console driver
 *
 *	(c) 1998 Andrew Apted <ajapted@netspace.net.au>
 *
 *      including portions (c) 1995-1998 Patrick Caulfield.
 *
 *      slight improvements (c) 2000 Edward Betts <edward@debian.org>
 *
 *  This file is based on the VGA console driver (vgacon.c):
 *	
 *	Created 28 Sep 1997 by Geert Uytterhoeven
 *
 *	Rewritten by Martin Mares <mj@ucw.cz>, July 1998
 *
 *  and on the old console.c, vga.c and vesa_blank.c drivers:
 *
 *	Copyright (C) 1991, 1992  Linus Torvalds
 *			    1995  Jay Estabrook
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of this archive for
 *  more details.
 */

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/tty.h>
#include <linux/string.h>
#include <linux/malloc.h>
#include <linux/vt_kern.h>
#include <linux/vt_buffer.h>
#include <linux/spinlock.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/init.h>

#include "vga.h"

static spinlock_t mda_lock = SPIN_LOCK_UNLOCKED;

/* description of the hardware layout */

static unsigned long	mda_vram_base;		/* Base of video memory */
static unsigned long	mda_vram_len;		/* Size of video memory */
static unsigned int	mda_num_columns;	/* Number of text columns */
static unsigned int	mda_num_lines;		/* Number of text lines */

static unsigned int	mda_index_port;		/* Register select port */
static unsigned int	mda_value_port;		/* Register value port */
static unsigned int	mda_mode_port;		/* Mode control port */
static unsigned int	mda_status_port;	/* Status and Config port */
static unsigned int	mda_gfx_port;		/* Graphics control port */

/* current hardware state */

static int	mda_origin_loc=-1;
static int	mda_cursor_loc=-1;
static int	mda_cursor_size_from=-1;
static int	mda_cursor_size_to=-1;

static enum { TYPE_MDA, TYPE_HERC, TYPE_HERCPLUS, TYPE_HERCCOLOR } mda_type;
static char *mda_type_name;

/* MDA register values
 */

#define MDA_CURSOR_BLINKING	0x00
#define MDA_CURSOR_OFF		0x20
#define MDA_CURSOR_SLOWBLINK	0x60

#define MDA_MODE_GRAPHICS	0x02
#define MDA_MODE_VIDEO_EN	0x08
#define MDA_MODE_BLINK_EN	0x20
#define MDA_MODE_GFX_PAGE1	0x80

#define MDA_STATUS_HSYNC	0x01
#define MDA_STATUS_VSYNC	0x80
#define MDA_STATUS_VIDEO	0x08

#define MDA_CONFIG_COL132	0x08
#define MDA_GFX_MODE_EN		0x01
#define MDA_GFX_PAGE_EN		0x02


/*
 * MDA could easily be classified as "pre-dinosaur hardware".
 */

static void write_mda_b(unsigned int val, unsigned char reg)
{
	unsigned long flags;

	spin_lock_irqsave(&mda_lock, flags);

	outb_p(reg, mda_index_port); 
	outb_p(val, mda_value_port);

	spin_unlock_irqrestore(&mda_lock, flags);
}

static void write_mda_w(unsigned int val, unsigned char reg)
{
	unsigned long flags;

	spin_lock_irqsave(&mda_lock, flags);
	
	outb_p(reg,   mda_index_port); outb_p(val >> 8,   mda_value_port);
	outb_p(reg+1, mda_index_port); outb_p(val & 0xff, mda_value_port);

	spin_unlock_irqrestore(&mda_lock, flags);
}

static int test_mda_b(unsigned char val, unsigned char reg)
{
	unsigned long flags;

	spin_lock_irqsave(&mda_lock, flags);

	outb_p(reg, mda_index_port); 
	outb(val, mda_value_port);

	udelay(20); val = (inb_p(mda_value_port) == val);

	spin_unlock_irqrestore(&mda_lock, flags);
	return val;
}

static inline void mda_set_origin(unsigned int location)
{
	if (mda_origin_loc == location)
		return;

	write_mda_w(location >> 1, 0x0c);

	mda_origin_loc = location;
}

static inline void mda_set_cursor(unsigned int location) 
{
	if (mda_cursor_loc == location)
		return;

	write_mda_w(location >> 1, 0x0e);

	mda_cursor_loc = location;
}

static inline void mda_set_cursor_size(int from, int to)
{
	if (mda_cursor_size_from==from && mda_cursor_size_to==to)
		return;
	
	if (from > to) {
		write_mda_b(MDA_CURSOR_OFF, 0x0a);	/* disable cursor */
	} else {
		write_mda_b(from, 0x0a);	/* cursor start */
		write_mda_b(to,   0x0b);	/* cursor end */
	}

	mda_cursor_size_from = from;
	mda_cursor_size_to   = to;
}


#ifndef MODULE
void __init mdacon_setup(char *str, int *ints)
{
	return;
}
#endif

#ifdef MODULE
static int mda_detect(void)
#else
static int __init mda_detect(void)
#endif
{
	int count=0;
	u16 *p, p_save;
	u16 *q, q_save;

	/* do a memory check */

	p = (u16 *) mda_vram_base;
	q = (u16 *) (mda_vram_base + 0x01000);

	p_save = scr_readw(p); q_save = scr_readw(q);

	scr_writew(0xAA55, p); if (scr_readw(p) == 0xAA55) count++;
	scr_writew(0x55AA, p); if (scr_readw(p) == 0x55AA) count++;
	scr_writew(p_save, p);

	if (count != 2) {
		return 0;
	}

	/* check if we have 4K or 8K */

	scr_writew(0xA55A, q); scr_writew(0x0000, p);
	if (scr_readw(q) == 0xA55A) count++;
	
	scr_writew(0x5AA5, q); scr_writew(0x0000, p);
	if (scr_readw(q) == 0x5AA5) count++;

	scr_writew(p_save, p); scr_writew(q_save, q);
	
	if (count == 4) {
		mda_vram_len = 0x02000;
	}
	
	/* Ok, there is definitely a card registering at the correct
	 * memory location, so now we do an I/O port test.
	 */

	/* Edward: These two mess `tests' mess up my cursor on bootup */

	/* cursor low register */
	if (! test_mda_b(0x66, 0x0f)) {
		return 0;
	}

	/* cursor low register */
	if (! test_mda_b(0x99, 0x0f)) {
		return 0;
	}

	/* See if the card is a Hercules, by checking whether the vsync
	 * bit of the status register is changing.  This test lasts for
	 * approximately 1/10th of a second.
	 */
	
	p_save = q_save = inb_p(mda_status_port) & MDA_STATUS_VSYNC;

	for (count=0; count < 50000 && p_save == q_save; count++) {
		q_save = inb(mda_status_port) & MDA_STATUS_VSYNC;
		udelay(2);
	}

	if (p_save != q_save) {
		switch (inb_p(mda_status_port) & 0x70) {
			case 0x10:
				mda_type = TYPE_HERCPLUS;
				mda_type_name = "HerculesPlus";
				break;
			case 0x50:
				mda_type = TYPE_HERCCOLOR;
				mda_type_name = "HerculesColor";
				break;
			default:
				mda_type = TYPE_HERC;
				mda_type_name = "Hercules";
				break;
		}
	}

	return 1;
}

#ifdef MODULE
static void mda_initialize(void)
#else
static void __init mda_initialize(void)
#endif
{
	write_mda_b(97, 0x00);		/* horizontal total */
	write_mda_b(80, 0x01);		/* horizontal displayed */
	write_mda_b(82, 0x02);		/* horizontal sync pos */
	write_mda_b(15, 0x03);		/* horizontal sync width */

	write_mda_b(25, 0x04);		/* vertical total */
	write_mda_b(6,  0x05);		/* vertical total adjust */
	write_mda_b(25, 0x06);		/* vertical displayed */
	write_mda_b(25, 0x07);		/* vertical sync pos */

	write_mda_b(2,  0x08);		/* interlace mode */
	write_mda_b(13, 0x09);		/* maximum scanline */
	write_mda_b(12, 0x0a);		/* cursor start */
	write_mda_b(13, 0x0b);		/* cursor end */

	write_mda_w(0x0000, 0x0c);	/* start address */
	write_mda_w(0x0000, 0x0e);	/* cursor location */

	outb_p(MDA_MODE_VIDEO_EN | MDA_MODE_BLINK_EN, mda_mode_port);
	outb_p(0x00, mda_status_port);
	outb_p(0x00, mda_gfx_port);
}

#ifdef MODULE
static const char *mdacon_startup(struct vt_struct *vt, int init)
#else
static const char __init *mdacon_startup(struct vt_struct *vt, int init)
#endif
{
	mda_num_columns = 80;
	mda_num_lines   = 25;

	mda_vram_base = VGA_MAP_MEM(0xb0000);
	mda_vram_len  = 0x01000;

	mda_index_port  = 0x3b4;
	mda_value_port  = 0x3b5;
	mda_mode_port   = 0x3b8;
	mda_status_port = 0x3ba;
	mda_gfx_port    = 0x3bf;

	mda_type = TYPE_MDA;
	mda_type_name = "MDA";

	if (! mda_detect()) {
		printk("mdacon: MDA card not detected.\n");
		return NULL;
	}

	if (mda_type != TYPE_MDA) {
		mda_initialize();
	}

	/* cursor looks ugly during boot-up, so turn it off */
	mda_set_cursor(mda_vram_len - 1);

	vt->default_mode->vc_cols = mda_num_columns;
	vt->default_mode->vc_rows = mda_num_lines;

	printk("mdacon: %s with %ldK of memory detected.\n",
		mda_type_name, mda_vram_len/1024);
	MOD_INC_USE_COUNT;
	return "MDA-2";
}

static void mdacon_init(struct vc_data *vc)
{
	vc->vc_complement_mask = 0x0800;	 /* reverse video */

	vc->vc_cols = vc->display_fg->default_mode->vc_cols;
	vc->vc_rows = vc->display_fg->default_mode->vc_rows;
}

static void mdacon_deinit(struct vc_data *vc)
{
	/* con_set_default_unimap(c->vc_num); 

	MOD_DEC_USE_COUNT; */
}

static inline u16 mda_convert_attr(u16 ch)
{
	u16 attr = 0x0700;

	/* Underline and reverse-video are mutually exclusive on MDA.
	 * Since reverse-video is used for cursors and selected areas,
	 * it takes precedence. 
	 */

	if (ch & 0x0800)	attr = 0x7000;	/* reverse */
	else if (ch & 0x0400)	attr = 0x0100;	/* underline */

	return ((ch & 0x0200) << 2) | 		/* intensity */ 
		(ch & 0x8000) |			/* blink */ 
		(ch & 0x00ff) | attr;
}

static u8 mdacon_build_attr(struct vc_data *c, u8 color, u8 intensity, 
			    u8 blink, u8 underline, u8 reverse)
{
	/* The attribute is just a bit vector:
	 *
	 *	Bit 0..1 : intensity (0..2)
	 *	Bit 2    : underline
	 *	Bit 3    : reverse
	 *	Bit 7    : blink
	 */

	return (intensity & 3) |
		((underline & 1) << 2) |
		((reverse   & 1) << 3) |
		((blink     & 1) << 7);
}

static void mdacon_invert_region(struct vc_data *c, u16 *p, int count)
{
	for (; count > 0; count--) {
		scr_writew(scr_readw(p) ^ 0x0800, p);
		p++;
	}
}

#define MDA_ADDR(x,y)  ((u16 *) mda_vram_base + (y)*mda_num_columns + (x))

static void mdacon_putc(struct vc_data *c, int ch, int y, int x)
{
	scr_writew(mda_convert_attr(ch), MDA_ADDR(x, y));
}

static void mdacon_putcs(struct vc_data *c, const unsigned short *s,
		         int count, int y, int x)
{
	u16 *dest = MDA_ADDR(x, y);

	for (; count > 0; count--) {
		scr_writew(mda_convert_attr(scr_readw(s++)), dest++);
	}
}

static void mdacon_clear(struct vc_data *vc, int y, int x, 
			  int height, int width)
{
	u16 *dest = MDA_ADDR(x, y);
	u16 eattr = mda_convert_attr(vc->vc_video_erase_char);

	if (width <= 0 || height <= 0)
		return;

	if (x==0 && width == vc->vc_cols) {
		scr_memsetw(dest, eattr, height*width*2);
	} else {
		for (; height > 0; height--, dest += vc->vc_cols)
			scr_memsetw(dest, eattr, width*2);
	}
}
                        
static void mdacon_bmove(struct vc_data *c, int sy, int sx, 
			 int dy, int dx, int height, int width)
{
	u16 *src, *dest;

	if (width <= 0 || height <= 0)
		return;
		
	if (sx==0 && dx==0 && width == vc->vc_cols) {
		scr_memmovew(MDA_ADDR(0,dy), MDA_ADDR(0,sy), height*width*2);

	} else if (dy < sy || (dy == sy && dx < sx)) {
		src  = MDA_ADDR(sx, sy);
		dest = MDA_ADDR(dx, dy);

		for (; height > 0; height--) {
			scr_memmovew(dest, src, width*2);
			src  += vc->vc_cols;
			dest += vc->vc_cols;
		}
	} else {
		src  = MDA_ADDR(sx, sy+height-1);
		dest = MDA_ADDR(dx, dy+height-1);

		for (; height > 0; height--) {
			scr_memmovew(dest, src, width*2);
			src  -= vc->vc_cols;
			dest -= vc->vc_cols;
		}
	}
}

static int mdacon_switch(struct vc_data *c)
{
	return 1;	/* redrawing needed */
}

static int mdacon_set_palette(struct vc_data *vc, unsigned char *table)
{
	return -EINVAL;
}

static int mdacon_blank(struct vc_data *vc, int blank)
{
	if (mda_type == TYPE_MDA) {
		if (blank) 
			scr_memsetw((void *)mda_vram_base, 
				mda_convert_attr(vc->vc_video_erase_char),
				vc->vc_screenbuf_size);
		/* Tell console.c that it has to restore the screen itself */
		return 1;
	} else {
		if (blank)
			outb_p(0x00, mda_mode_port);	/* disable video */
		else
			outb_p(MDA_MODE_VIDEO_EN | MDA_MODE_BLINK_EN, 
				mda_mode_port);
		return 0;
	}
}

static int mdacon_font_op(struct vc_data *vc, struct console_font_op *op)
{
	return -ENOSYS;
}

static int mdacon_scrolldelta(struct vc_data *vc, int lines)
{
	return 0;
}

static void mdacon_cursor(struct vc_data *vc, int mode)
{
	if (mode == CM_ERASE) {
		mda_set_cursor(mda_vram_len - 1);
		return;
	}

	mda_set_cursor(vc->vc_y * vc->vc_cols*2 + vc->vc_x*2);

	switch (vc->vc_cursor_type & 0x0f) {

		case CUR_LOWER_THIRD:	mda_set_cursor_size(10, 13); break;
		case CUR_LOWER_HALF:	mda_set_cursor_size(7,  13); break;
		case CUR_TWO_THIRDS:	mda_set_cursor_size(4,  13); break;
		case CUR_BLOCK:		mda_set_cursor_size(1,  13); break;
		case CUR_NONE:		mda_set_cursor_size(14, 13); break;
		default:		mda_set_cursor_size(12, 13); break;
	}
}

static int mdacon_scroll(struct vc_data *vc, int t, int b, int dir, int lines)
{
	u16 eattr = mda_convert_attr(vc->vc_video_erase_char);

	if (!lines)
		return 0;

	if (lines > vc->vc_rows)   /* maximum realistic size */
		lines = vc->vc_rows;

	switch (dir) {

	case SM_UP:
		scr_memmovew(MDA_ADDR(0,t), MDA_ADDR(0,t+lines),
				(b-t-lines)*vc->vc_cols*2);
		scr_memsetw(MDA_ADDR(0,b-lines), eattr,
				lines*vc->vc_cols*2);
		break;

	case SM_DOWN:
		scr_memmovew(MDA_ADDR(0,t+lines), MDA_ADDR(0,t),
				(b-t-lines)*vc->vc_cols*2);
		scr_memsetw(MDA_ADDR(0,t), eattr, lines*vc->vc_cols*2);
		break;
	}

	return 0;
}


/*
 *  The console `switch' structure for the MDA based console
 */

const struct consw mda_con = {
	con_startup:		mdacon_startup,
	con_init:		mdacon_init,
	con_deinit:		mdacon_deinit,
	con_clear:		mdacon_clear,
	con_putc:		mdacon_putc,
	con_putcs:		mdacon_putcs,
	con_cursor:		mdacon_cursor,
	con_scroll:		mdacon_scroll,
	con_bmove:		mdacon_bmove,
	con_switch:		mdacon_switch,
	con_blank:		mdacon_blank,
	con_font_op:		mdacon_font_op,
	con_set_palette:	mdacon_set_palette,
	con_scrolldelta:	mdacon_scrolldelta,
	con_build_attr:		mdacon_build_attr,
	con_invert_region:	mdacon_invert_region,
};

#ifdef MODULE
void mda_console_init(void)
#else
void __init mda_console_init(void)
#endif
{
        const char *display_desc = NULL;
        struct vt_struct *vt;
	struct vc_data *vc;
        int i;

	/* Allocate the memory we need for this VT */
        vt = (struct vt_struct *) kmalloc(sizeof(struct vt_struct),GFP_KERNEL);
        if (!vt) return;
	vt->default_mode = (struct vc_data *) kmalloc(sizeof(struct vc_data), GFP_KERNEL);
	if (!vt->default_mode) {
		kfree(vt);
		return;
	}
	vc = (struct vc_data *) kmalloc(sizeof(struct vc_data), GFP_KERNEL);
	if (!vc) {
		kfree(vt->default_mode);
		kfree(vt);
		return;
	}
	vt->kmalloc = 1;
	vt->vt_sw = &mda_con;
	vt->vcs.vc_cons[0] = vc;
#ifdef MODULE
        display_desc = create_vt(vt, 1);
#else
	display_desc = create_vt(vt, 0);
#endif
	vc->vc_screenbuf = (long)kmalloc(vc->vc_screenbuf_size, GFP_KERNEL);
        if (!display_desc || !vc->vc_screenbuf) {
		kfree(vt->vcs.vc_cons[0]);
		kfree(vt->default_mode);
                kfree(vt);
		return;
        }
	vc_init(vc, !vt->vt_sw->con_save_screen);			
        printk("Console: mono %s %dx%d",display_desc, vc->vc_cols, vc->vc_rows);
        return;
}

#ifdef MODULE

int init_module(void)
{
	mda_console_init();
	return 0;
}

void cleanup_module(void)
{
	/* release_vt(&mda_vt); */
}

#endif
