/*
 * member2_test_stub.c — Standalone build stub for Member 2
 *
 * PURPOSE: lets Member 2 compile and syntax-check member2_read.c
 * independently before Member 1 and Member 3 finish their parts.
 *
 * This file is NOT submitted. Member 4 does NOT include it in the
 * final kbmonitor.c. It exists only so Member 2 can run:
 *
 *   make -C /lib/modules/$(uname -r)/build M=$PWD modules
 *
 * and confirm the read function compiles cleanly on the Raspberry Pi.
 *
 * HOW TO USE (on Raspberry Pi):
 *   1. Copy member2_read.c and this file + Makefile.test to ~/member2_test/
 *   2. Run: make -f Makefile.test
 *   3. sudo insmod member2_test.ko
 *   4. sudo dmesg | tail   (should show "kbmonitor: stub module loaded")
 *   5. sudo rmmod member2_test
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/atomic.h>
#include <linux/uaccess.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Member 2 - Kernel-to-User Communication");
MODULE_DESCRIPTION("Standalone test stub for device_read()");

/*
 * Stub definitions for the variables Member 3 will provide.
 * In the final module these lines are REMOVED and replaced by
 * Member 3's actual definitions.
 */
atomic_t keystroke_count = ATOMIC_INIT(42);   /* fake value for testing */
atomic_t key_press_count = ATOMIC_INIT(30);   /* fake value for testing */

/* Pull in the actual read implementation */
#include "member2_read.c"

/*
 * Minimal file_operations so the module is loadable.
 * Member 1 replaces this with the full struct that also has .open / .release.
 */
static struct file_operations stub_fops = {
    .owner = THIS_MODULE,
    .read  = device_read,
};

static int __init stub_init(void)
{
    printk(KERN_INFO "kbmonitor: stub module loaded — "
           "device_read() compiled successfully\n");
    printk(KERN_INFO "kbmonitor: stub counters: total=%d, presses=%d\n",
           atomic_read(&keystroke_count),
           atomic_read(&key_press_count));
    (void)stub_fops; /* suppress unused-variable warning */
    return 0;
}

static void __exit stub_exit(void)
{
    printk(KERN_INFO "kbmonitor: stub module unloaded\n");
}

module_init(stub_init);
module_exit(stub_exit);
