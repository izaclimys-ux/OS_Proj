/*
 * member3_notifier.c — Interrupt & Logic (Member 3)
 *
 * Owns the keyboard notifier and the two shared counters that Member 2's
 * read handler reports. Exposes start/stop helpers instead of claiming
 * module_init/exit, because a multi-file .ko may have only ONE of each --
 * the master file (member4_main.c) calls these from its init/exit.
 */

#include <linux/kernel.h>     /* printk(), KERN_* */
#include <linux/keyboard.h>   /* register_keyboard_notifier(), KBD_KEYCODE */
#include <linux/notifier.h>   /* struct notifier_block, NOTIFY_OK */
#include <linux/atomic.h>     /* atomic_t */

#include "kbmonitor.h"

/*
 * Shared counters. NOT static -- Member 2 links against these via 'extern'
 * (declared in kbmonitor.h). External linkage is all that's needed inside
 * one module; EXPORT_SYMBOL is only for cross-module use.
 */
atomic_t keystroke_count = ATOMIC_INIT(0);   /* presses + releases          */
atomic_t key_press_count = ATOMIC_INIT(0);   /* presses only                */

#define LOG_EVERY 10   /* emit one dmesg line every N presses (spec: 10/50) */

static int kbd_notifier_call(struct notifier_block *nb,
			     unsigned long action, void *data)
{
	struct keyboard_notifier_param *param = data;

	if (action == KBD_KEYCODE) {
		if (param->down == 1) {
			/* genuine key press: bump both counters */
			int presses = atomic_inc_return(&key_press_count);

			atomic_inc(&keystroke_count);

			if (presses % LOG_EVERY == 0)
				printk(KERN_INFO
				       "kbmonitor: %d key presses entered\n",
				       presses);
		} else if (param->down == 0) {
			/* release: counts toward total, not toward presses */
			atomic_inc(&keystroke_count);
		}
		/* param->down == 2 (autorepeat) ignored on purpose */
	}

	return NOTIFY_OK;   /* never NOTIFY_STOP: stay invisible to the user */
}

static struct notifier_block kbd_nb = {
	.notifier_call = kbd_notifier_call,
};

/* ---- Lifecycle helpers called by the master init/exit ------------------ */
int kbmonitor_start_notifier(void)
{
	return register_keyboard_notifier(&kbd_nb);
}

void kbmonitor_stop_notifier(void)
{
	unregister_keyboard_notifier(&kbd_nb);
}