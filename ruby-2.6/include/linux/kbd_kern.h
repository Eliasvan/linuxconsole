#ifndef _KBD_KERN_H
#define _KBD_KERN_H

#include <linux/tty.h>
#include <linux/kd.h>
#include <linux/interrupt.h>
#include <linux/keyboard.h>
#include <linux/input.h>

extern struct tasklet_struct keyboard_tasklet;

extern int shift_state;

extern char *func_table[MAX_NR_FUNC];
extern char func_buf[];
extern char *funcbufptr;
extern int  funcbufsize, funcbufleft;

#define KBD_DEFMODE	((1 << VC_REPEAT) | (1 << VC_META))

/*
 * Some laptops take the 789uiojklm,. keys as number pad when NumLock is on.
 * This seems a good reason to start with NumLock off. On PC9800 and HIL keyboards
 * of PARISC machines however there is no NumLock key and everyone expects the keypad
 * to be used for numbers.
 */
#if defined(CONFIG_X86_PC9800)
#define KBD_DEFLEDS	(1 << VC_NUMLOCK)
#else
#define KBD_DEFLEDS	0
#endif
#define KBD_DEFLOCK	0

struct vc_data;
		    
/*
 * kbd->xxx contains the VC-local things (flag settings etc..)
 *
 * Note: externally visible are LED_SCR, LED_NUM, LED_CAP defined in kd.h
 *       The code in KDGETLED / KDSETLED depends on the internal and
 *       external order being the same.
 *
 * Note: lockstate is used as index in the array key_map.
 */
struct kbd_struct {

	unsigned char lockstate;
/* 8 modifiers - the names do not have any meaning at all;
   they can be associated to arbitrarily chosen keys */
#define VC_SHIFTLOCK	KG_SHIFT	/* shift lock mode */
#define VC_ALTGRLOCK	KG_ALTGR	/* altgr lock mode */
#define VC_CTRLLOCK	KG_CTRL		/* control lock mode */
#define VC_ALTLOCK	KG_ALT		/* alt lock mode */
#define VC_SHIFTLLOCK	KG_SHIFTL	/* shiftl lock mode */
#define VC_SHIFTRLOCK	KG_SHIFTR	/* shiftr lock mode */
#define VC_CTRLLLOCK	KG_CTRLL	/* ctrll lock mode */
#define VC_CTRLRLOCK	KG_CTRLR	/* ctrlr lock mode */
	unsigned char slockstate;	/* for `sticky' Shift, Ctrl, etc. */

	unsigned char ledmode:2;	/* one 2-bit value */
#define LED_SHOW_FLAGS	0	/* traditional state */
#define LED_SHOW_IOCTL	1	/* only change leds upon ioctl */
#define LED_SHOW_MEM	2	/* `heartbeat': peek into memory */

	unsigned char ledflagstate:4;	/* flags, not lights */
	unsigned char default_ledflagstate:4;
#define VC_SCROLLOCK	0	/* scroll-lock mode */
#define VC_NUMLOCK	1	/* numeric lock mode */
#define VC_CAPSLOCK	2	/* capslock mode */
#define VC_KANALOCK	3	/* kanalock mode */

	unsigned char kbdmode:2;	/* one 2-bit value */
#define VC_XLATE	0	/* translate keycodes using keymap */
#define VC_MEDIUMRAW	1	/* medium raw (keycode) mode */
#define VC_RAW		2	/* raw (scancode) mode */
#define VC_UNICODE	3	/* Unicode mode */

	unsigned char modeflags:5;
#define VC_APPLIC	0	/* application key mode */
#define VC_CKMODE	1	/* cursor key mode */
#define VC_REPEAT	2	/* keyboard repeat */
#define VC_CRLF		3	/* 0 - enter sends CR, 1 - enter sends CRLF */
#define VC_META		4	/* 0 - meta, 1 - meta=prefix with ESC */
};

extern int kbd_init(void);

extern unsigned char getledstate(struct vc_data *vc);
extern void setledstate(struct vc_data *vc, unsigned int led);

extern int do_poke_blanked_console;

extern void (*kbd_ledfunc) (unsigned int led);

static inline void set_leds(void)
{
	tasklet_schedule(&keyboard_tasklet);
}

static inline int get_kbd_mode(struct kbd_struct * kbd, int flag)
{
	return ((kbd->modeflags >> flag) & 1);
}

static inline int get_kbd_led(struct kbd_struct * kbd, int flag)
{
	return ((kbd->ledflagstate >> flag) & 1);
}

static inline void set_kbd_mode(struct kbd_struct * kbd, int flag)
{
	kbd->modeflags |= 1 << flag;
}

static inline void set_kbd_led(struct kbd_struct * kbd, int flag)
{
	kbd->ledflagstate |= 1 << flag;
}

static inline void clr_kbd_mode(struct kbd_struct * kbd, int flag)
{
	kbd->modeflags &= ~(1 << flag);
}

static inline void clr_kbd_led(struct kbd_struct * kbd, int flag)
{
	kbd->ledflagstate &= ~(1 << flag);
}

static inline void chg_kbd_lock(struct kbd_struct * kbd, int flag)
{
	kbd->lockstate ^= 1 << flag;
}

static inline void chg_kbd_slock(struct kbd_struct * kbd, int flag)
{
	kbd->slockstate ^= 1 << flag;
}

static inline void chg_kbd_mode(struct kbd_struct * kbd, int flag)
{
	kbd->modeflags ^= 1 << flag;
}

static inline void chg_kbd_led(struct kbd_struct * kbd, int flag)
{
	kbd->ledflagstate ^= 1 << flag;
}

#define U(x) ((x) ^ 0xf000)

/* keyboard.c */
struct vc_data;

int getkeycode(struct input_handle *handle, unsigned int scancode);
int setkeycode(struct input_handle *handle, unsigned int scancode, unsigned int keycode);
void kd_mksound(struct input_handle *handle, unsigned int hz, unsigned int ticks);
void kd_nosound(unsigned long private);
int kbd_rate(struct input_handle *handle, struct kbd_repeat *rep);
void puts_queue(struct vc_data *vc, char *cp);
void compute_shiftstate(void);

/* defkeymap.c */

extern unsigned int keymap_count;

#endif
