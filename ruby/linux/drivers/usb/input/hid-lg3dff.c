/*
 * $$
 *
 *  Force feedback support for hid-compliant devices of the Logitech *3D family
 *
 *  Copyright (c) 2002 Johann Deneux
 */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * Should you need to contact me, the author, you can do so by
 * e-mail - mail your message to <deneux@ifrance.com>
 */

#include <linux/input.h>
#include <linux/sched.h>

#define DEBUG
#include <linux/usb.h>

#include <linux/circ_buf.h>

#include "hid.h"

#define RUN_AT(t) (jiffies + (t))

/* Frequency at which the rendering is updated */
#define FREQ (HZ/20)

/* Effect status */
#define EFFECT_STARTED 0     /* Effect is going to play after some time
				(ff_replay.delay) */
#define EFFECT_PLAYING 1     /* Effect is being played */
#define EFFECT_USED    2

/* Check that the current process can access an effect */
#define CHECK_OWNERSHIP(i, l) \
        (i>=0 && i<N_EFFECTS \
        && test_bit(EFFECT_USED, l->effects[i].flags) \
        && (current->pid == 0 \
            || l->effects[i].owner == current->pid))

#define N_EFFECTS 8

struct lg3d_effect {
	pid_t owner;

	struct ff_effect effect;   /* Description of the effect */

	unsigned int count;        /* Number of times left to play */
	unsigned long flags[1];

	unsigned long started_at;  /* When the effect started to play */
};

struct lg3d_device {
	struct hid_device* hid;

	struct urb* urbffout;        /* Output URB used to send ff commands */
	struct usb_ctrlrequest ffcr; /* ff commands use control URBs */
	char buf[8];

	struct lg3d_effect effects[N_EFFECTS];
	spinlock_t lock;             /* device-level lock. Having locks on
					a per-effect basis could be nice, but
					isn't really necessary */
	struct timer_list timer;
	unsigned long last_time;     /* Last time the timer handler was
					executed */
};

static void hid_lg3d_ctrl_out(struct urb *urb);
static void hid_lg3d_exit(struct hid_device* hid);
static int hid_lg3d_event(struct hid_device *hid, struct input_dev *input,
			  unsigned int type, unsigned int code, int value);
static int hid_lg3d_flush(struct input_dev *input, struct file *file);
static int hid_lg3d_upload_effect(struct input_dev *input,
				  struct ff_effect *effect);
static int hid_lg3d_erase(struct input_dev *input, int id);
static void hid_lg3d_timer(unsigned long timer_data);


int hid_lg3d_init(struct hid_device* hid)
{
	struct lg3d_device *private;

	/* Private data */
	hid->ff_private = kmalloc(sizeof(struct lg3d_device), GFP_KERNEL);
	private = hid->ff_private;
	if (!private) return -1;

	memset(private, 0, sizeof(struct lg3d_device));

	hid->ff_private = private;

	private->hid = hid;
	spin_lock_init(&private->lock);

	/* Timer for the periodic update task */
	init_timer(&private->timer);
	private->timer.data = (unsigned long)private;
	private->timer.function = hid_lg3d_timer;

	/* Event and exit callbacks */
	hid->ff_exit = hid_lg3d_exit;
	hid->ff_event = hid_lg3d_event;

	/* USB init */
	if (!(private->urbffout = usb_alloc_urb(0, GFP_KERNEL))) {
		kfree(hid->ff_private);
		return -1;
	}

	usb_fill_control_urb(private->urbffout, hid->dev, 0,
			     (void*) &private->ffcr, private->buf, 8,
			     hid_lg3d_ctrl_out, hid);
	dbg("Created ff output control urb");

	/* Input init */
	hid->input.upload_effect = hid_lg3d_upload_effect;
	hid->input.flush = hid_lg3d_flush;
	set_bit(FF_CONSTANT, hid->input.ffbit);
	set_bit(EV_FF, hid->input.evbit);
	hid->input.ff_effects_max = N_EFFECTS;

	printk(KERN_INFO "Force feedback for Logitech *3D devices by Johann Deneux <deneux@ifrance.com>\n");

	/* Start the update task */
	add_timer(&private->timer);  /*TODO: only run the timer when at least
				       one effect is playing */

	return 0;
}

static void hid_lg3d_exit(struct hid_device* hid)
{
	struct lg3d_device *lg3d = hid->ff_private;

	del_timer_sync(&lg3d->timer);

	if (lg3d->urbffout) {
		usb_unlink_urb(lg3d->urbffout);
		usb_free_urb(lg3d->urbffout);
	}
}

static int hid_lg3d_event(struct hid_device *hid, struct input_dev* input,
			  unsigned int type, unsigned int code, int value)
{
	struct lg3d_device *lg3d = hid->ff_private;
	struct lg3d_effect *effect = lg3d->effects + code;
	unsigned long flags;

	if (type != EV_FF)                return -EINVAL;
       	if (!CHECK_OWNERSHIP(code, lg3d)) return -EACCES;
	if (value < 0)                    return -EINVAL;

	spin_lock_irqsave(&lg3d->lock, flags);
	
	if (value > 0) {
		if (test_bit(EFFECT_STARTED, effect->flags)) {
			spin_unlock_irqrestore(&lg3d->lock, flags);
			return -EBUSY;
		}
		if (test_bit(EFFECT_PLAYING, effect->flags)) {
			spin_unlock_irqrestore(&lg3d->lock, flags);
			return -EBUSY;
		}

		effect->count = value;

		if (effect->effect.replay.delay) {
			set_bit(EFFECT_STARTED, effect->flags);
		} else {
			set_bit(EFFECT_PLAYING, effect->flags);
		}
		effect->started_at = jiffies;
	}
	else { /* value == 0 */
		clear_bit(EFFECT_STARTED, effect->flags);
		clear_bit(EFFECT_PLAYING, effect->flags);
	}

	spin_unlock_irqrestore(&lg3d->lock, flags);

	return 0;
}

/* Erase all effects this process owns */
static int hid_lg3d_flush(struct input_dev *dev, struct file *file)
{
	struct hid_device *hid = dev->private;
	struct lg3d_device *lg3d = hid->ff_private;
	int i;

	for (i=0; i<dev->ff_effects_max; ++i) {

		/*NOTE: no need to lock here. The only times EFFECT_USED is
		  modified is when effects are uploaded or when an effect is
		  erased. But a process cannot close its dev/input/eventX fd
		  and perform ioctls on the same fd all at the same time */
		if ( current->pid == lg3d->effects[i].owner
		     && test_bit(EFFECT_USED, lg3d->effects[i].flags)) {
			
			if (hid_lg3d_erase(dev, i))
				warn("erase effect %d failed", i);
		}

	}

	return 0;
}

static int hid_lg3d_erase(struct input_dev *dev, int id)
{
	struct hid_device *hid = dev->private;
	struct lg3d_device *lg3d = hid->ff_private;
	unsigned long flags;

	if (!CHECK_OWNERSHIP(id, lg3d)) return -EACCES;

	spin_lock_irqsave(&lg3d->lock, flags);
	lg3d->effects[id].flags[0] = 0;
	spin_unlock_irqrestore(&lg3d->lock, flags);

	return 0;
}

static int hid_lg3d_upload_effect(struct input_dev* input,
				  struct ff_effect* effect)
{
	struct hid_device *hid = input->private;
	struct lg3d_device *lg3d = hid->ff_private;
	struct lg3d_effect new;
	int id;
	unsigned long flags;
	
	dbg("ioctl rumble");

	if (!test_bit(effect->type, input->ffbit)) return -EINVAL;

	if (effect->type != FF_CONSTANT) return -EINVAL;

	spin_lock_irqsave(&lg3d->lock, flags);

	if (effect->id == -1) {
		int i;

		for (i=0; i<N_EFFECTS && test_bit(EFFECT_USED, lg3d->effects[i].flags); ++i);
		if (i >= N_EFFECTS) {
			spin_unlock_irqrestore(&lg3d->lock, flags);
			return -ENOSPC;
		}

		effect->id = i;
		lg3d->effects[i].owner = current->pid;
		lg3d->effects[i].flags[0] = 0;
		set_bit(EFFECT_USED, lg3d->effects[i].flags);
	}
	else if (!CHECK_OWNERSHIP(effect->id, lg3d)) {
		spin_unlock_irqrestore(&lg3d->lock, flags);
		return -EACCES;
	}

	id = effect->id;
	new = lg3d->effects[id];

	new.effect = *effect;
	new.effect.replay = effect->replay;

	/* If we updated an effect that was being played, we need to remake
	   the rumble effect */
	if (test_bit(EFFECT_STARTED, lg3d->effects[id].flags)
	    || test_bit(EFFECT_STARTED, lg3d->effects[id].flags)) {

		/* Changing replay parameters is not allowed (for the time
		   being) */
		if (new.effect.replay.delay != lg3d->effects[id].effect.replay.delay
		    || new.effect.replay.length != lg3d->effects[id].effect.replay.length) {
			spin_unlock_irqrestore(&lg3d->lock, flags);
			return -ENOSYS;
		}

		lg3d->effects[id] = new;

	} else {
		lg3d->effects[id] = new;
	}

	spin_unlock_irqrestore(&lg3d->lock, flags);
	return 0;
}

static void hid_lg3d_ctrl_out(struct urb *urb)
{
	struct hid_device *hid = urb->context;

	if (urb->status)
		warn("hid_irq_ffout status %d received", urb->status);

}

static void hid_lg3d_timer(unsigned long timer_data)
{
	struct lg3d_device *lg3d = (struct lg3d_device*)timer_data;
	struct hid_device *hid = lg3d->hid;
	unsigned long flags;
	int x, y;
	int i;
	int err;

	x = 0;
	y = 0;
	spin_lock_irqsave(&lg3d->lock, flags);

 	for (i=0; i<N_EFFECTS; ++i) {
		struct lg3d_effect* effect = lg3d->effects +i;

		if (test_bit(EFFECT_PLAYING, effect->flags)) {
			if (effect->effect.type == FF_CONSTANT) {
				x = y = 0x60;   /*TODO: actual implementation */
			}

			/* One run of the effect is finished playing */
			if (time_after(jiffies,
					effect->started_at
					+ effect->effect.replay.delay*HZ/1000
					+ effect->effect.replay.length*HZ/1000)) {
				if (--effect->count <= 0)
					clear_bit(EFFECT_PLAYING, effect->flags);
				else {
					if (effect->effect.replay.length != 0) {
						clear_bit(EFFECT_PLAYING, effect->flags);
						set_bit(EFFECT_STARTED, effect->flags);
					}
					effect->started_at = jiffies;
				}
			}

		} else if (test_bit(EFFECT_STARTED, lg3d->effects[i].flags)) {
			/* Check if we should start playing the effect */
			if (time_after(jiffies,
					lg3d->effects[i].started_at
					+ lg3d->effects[i].effect.replay.delay*HZ/1000)) {
				clear_bit(EFFECT_STARTED, lg3d->effects[i].flags);
				set_bit(EFFECT_PLAYING, lg3d->effects[i].flags);
			}
		}
 	}

 	spin_unlock_irqrestore(&lg3d->lock, flags);

	lg3d->urbffout->pipe = usb_sndctrlpipe(hid->dev, 0);
	lg3d->ffcr.bRequestType = USB_TYPE_CLASS | USB_DIR_OUT | USB_RECIP_INTERFACE;
	lg3d->urbffout->transfer_buffer_length = lg3d->ffcr.wLength = 8;
	lg3d->ffcr.bRequest = 9;
	lg3d->ffcr.wValue = 0x0200;    /*NOTE: Potential problem with 
					 little/big endian */
	lg3d->ffcr.wIndex = 0;
	
	lg3d->urbffout->dev = hid->dev;
	
	lg3d->buf[0] = 0x51;
	lg3d->buf[1] = 0x08;
	lg3d->buf[2] = x;
	lg3d->buf[3] = y;

	if ((err=usb_submit_urb(lg3d->urbffout, GFP_ATOMIC)))
		warn("usb_submit_urb returned %d", err);

	add_timer(&lg3d->timer);
}
