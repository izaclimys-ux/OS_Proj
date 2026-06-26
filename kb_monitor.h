/*
* kbmonitor.h — shared declarations for the /dev/kbmonitor module.
 *
 * Every member file includes this so the whole team agrees on the
 * types and signatures. Single source of truth for the cross-file seams.
 */
#ifndef KBMONITOR_H
#define KBMONITOR_H

#include <linux/atomic.h>
#include <linux/fs.h>

/* ---- Shared counters (defined in member3_notifier.c) -------------------
 * keystroke_count = presses + releases (autorepeat excluded)
 * key_press_count = presses only
 * so Member 2's "releases = total - presses" is exact.
 */
extern atomic_t keystroke_count;
extern atomic_t key_press_count;

/* ---- Member 3: notifier lifecycle (called from the master init/exit) --- */
int  kbmonitor_start_notifier(void);
void kbmonitor_stop_notifier(void);

/* ---- Member 2: VFS read handler (wired into file_operations.read) ------ */
ssize_t device_read(struct file *filp, char __user *buffer,
            size_t len, loff_t *offset);

#endif /* KBMONITOR_H */