/*
 * member2_read.c — Kernel-to-User Communication (Member 2)
 *
 * This file implements the .read file operation for /dev/kbmonitor.
 * It is designed to be merged into the final kbmonitor.c by Member 4.
 *
 * INTERFACE CONTRACT
 * ------------------
 * Provides:
 *   device_read()  — to be assigned to .read in file_operations (Member 1)
 *
 * Requires (defined by Member 3):
 *   extern atomic_t keystroke_count;
 *   extern atomic_t key_press_count;   /* presses only, not releases */
 *
 * How Member 1 wires this in:
 *   static struct file_operations fops = {
 *       .owner   = THIS_MODULE,
 *       .open    = device_open,     // Member 1
 *       .release = device_release,  // Member 1
 *       .read    = device_read,     // Member 2  <-- this file
 *   };
 */

#include <linux/kernel.h>   /* printk(), KERN_* levels */
#include <linux/fs.h>       /* file_operations, ssize_t, loff_t */
#include <linux/uaccess.h>  /* copy_to_user() — the only safe kernel→user copy */
#include <linux/atomic.h>   /* atomic_t, atomic_read() */

/*
 * External declarations: these variables are owned and incremented by Member 3.
 * Using atomic_t guarantees that reads and increments from different kernel
 * contexts (notifier callback vs. this read path) are race-free without
 * needing an explicit spinlock.
 */
extern atomic_t keystroke_count;   /* total key events seen */
extern atomic_t key_press_count;   /* key-down events only (filtered by Member 3) */

/* Internal kernel-space formatting buffer.
 * 256 bytes is generous for a two-integer statistics string. */
#define KBMON_BUF_SIZE 256

/*
 * device_read() — Called by the kernel VFS layer whenever user-space invokes
 *                 read() on /dev/kbmonitor.
 *
 * @filp:   the open file struct (kernel-managed; we don't touch it directly)
 * @buffer: __user pointer to the caller's buffer — NEVER dereference directly
 * @len:    number of bytes the caller wants
 * @offset: current file position; used to implement EOF semantics
 *
 * Returns: bytes written to user space on success, negative errno on error.
 *
 * Design notes:
 *   - We format the stats string entirely in kernel space (kbuf) first.
 *     This means no user pointer is ever passed to snprintf(), avoiding a
 *     class of kernel bugs where a bad user pointer causes a kernel oops.
 *   - copy_to_user() is mandatory here (not memcpy). It validates the user
 *     address, handles copy-on-write page faults, and cannot be bypassed.
 *   - The *offset trick: on the first call *offset is 0, so we write data
 *     and advance *offset. On a second call *offset > 0 so we return 0
 *     (EOF). This makes standard C read() / cat /dev/kbmonitor work
 *     correctly — both terminate after one block of data.
 */
static ssize_t device_read(struct file *filp, char __user *buffer,
                            size_t len, loff_t *offset)
{
    char   kbuf[KBMON_BUF_SIZE];
    int    kbuf_len;
    int    total_events;
    int    press_events;

    /*
     * EOF guard: if offset is already past zero the caller has already
     * received all available data in this "file". Return 0 to signal EOF.
     * The caller resets offset to 0 on the next open(), so repeated
     * cat /dev/kbmonitor calls each get a fresh read.
     */
    if (*offset > 0)
        return 0;

    /*
     * Take a consistent snapshot of both counters in kernel space.
     * atomic_read() is a single load with the appropriate barrier —
     * safe to call from any context including softirq.
     */
    total_events = atomic_read(&keystroke_count);
    press_events = atomic_read(&key_press_count);

    /*
     * Format the statistics entirely inside kbuf (kernel memory).
     * snprintf() returns the number of characters that WOULD have been
     * written if the buffer were unlimited — we use that as our length.
     */
    kbuf_len = snprintf(kbuf, sizeof(kbuf),
                        "=== /dev/kbmonitor statistics ===\n"
                        "Total key events  : %d\n"
                        "Key press events  : %d\n"
                        "Key release events: %d\n"
                        "=================================\n",
                        total_events,
                        press_events,
                        total_events - press_events);

    /*
     * Guard: if the kernel buffer overflowed (shouldn't happen with the
     * format above, but defensive coding is required in kernel code).
     */
    if (kbuf_len >= KBMON_BUF_SIZE) {
        printk(KERN_WARNING "kbmonitor: read buffer overflow truncated output\n");
        kbuf_len = KBMON_BUF_SIZE - 1;
    }

    /* Clamp to what the caller's buffer can actually hold. */
    if ((size_t)kbuf_len > len)
        kbuf_len = (int)len;

    /*
     * copy_to_user(to, from, n):
     *   - 'to'   is the __user destination (never write to this directly)
     *   - 'from' is our safe kernel buffer kbuf
     *   - 'n'    is the byte count
     *
     * Returns 0 on full success, or the number of bytes NOT copied.
     * Any non-zero return means a fault occurred in user space — we map
     * that to -EFAULT, which is the standard errno for bad user addresses.
     */
    if (copy_to_user(buffer, kbuf, kbuf_len)) {
        printk(KERN_WARNING "kbmonitor: copy_to_user() failed — bad user address\n");
        return -EFAULT;
    }

    /*
     * Advance the file offset so the next call to read() returns EOF (0).
     * This is how all sequential character devices signal "no more data".
     */
    *offset += kbuf_len;

    printk(KERN_INFO "kbmonitor: read() sent %d bytes to user space "
           "(total=%d, presses=%d)\n",
           kbuf_len, total_events, press_events);

    return (ssize_t)kbuf_len;
}
