/*
 * kb_module.c — Keyboard Analytics Loadable Kernel Module
 *
 * Features:
 *  - Detects USB device plug/unplug events
 *  - Exposes a character device (/dev/kb_analytics) for user-space read()/write()
 *  - Hooks into the Linux input subsystem to track keyboard events:
 *      key press counts, typing speed, most-used keys, hotkey combos
 *  - Reports analytics via /proc/kb_stats
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/usb.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/input.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/string.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("KB Analytics Project");
MODULE_DESCRIPTION("USB hotplug + keyboard analytics kernel module");
MODULE_VERSION("1.0");

/* ──────────────────────────────────────────────
 * Character device bookkeeping
 * ────────────────────────────────────────────── */
#define DEVICE_NAME   "kb_analytics"
#define CLASS_NAME    "kb_class"
#define BUF_SIZE      256

static int    major_number;
static struct class  *kb_class  = NULL;
static struct device *kb_device = NULL;
static struct cdev    kb_cdev;
static dev_t          kb_dev;

/* Shared message buffers (kernel ↔ user space) */
static char   rx_buf[BUF_SIZE];   /* data written by user space  */
static char   tx_buf[BUF_SIZE];   /* data read  by user space    */
static int    tx_len = 0;

static DEFINE_SPINLOCK(buf_lock);

/* ──────────────────────────────────────────────
 * Keyboard analytics state
 * ────────────────────────────────────────────── */
#define NUM_KEYS  256

static unsigned long key_counts[NUM_KEYS];   /* per-keycode press count  */
static unsigned long total_keypresses;
static ktime_t       last_press_time;        /* for inter-key interval   */
static unsigned long interval_sum_us;        /* sum of intervals (µs)    */
static unsigned long interval_count;

/* Hotkey combo detection: track modifier state */
static bool mod_ctrl  = false;
static bool mod_shift = false;
static bool mod_alt   = false;
static unsigned long hotkey_count;           /* any Ctrl/Alt combo       */

static DEFINE_SPINLOCK(analytics_lock);

/* ──────────────────────────────────────────────
 * Input handler — taps into every keyboard event
 * ────────────────────────────────────────────── */
static void kb_event(struct input_handle *handle,
                     unsigned int type, unsigned int code, int value)
{
    ktime_t now;
    unsigned long flags;

    /* Only care about key events; value 1 = press, 0 = release, 2 = repeat */
    if (type != EV_KEY)
        return;

    spin_lock_irqsave(&analytics_lock, flags);

    /* Track modifier state */
    if (code == KEY_LEFTCTRL  || code == KEY_RIGHTCTRL)
        mod_ctrl  = (value != 0);
    if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT)
        mod_shift = (value != 0);
    if (code == KEY_LEFTALT   || code == KEY_RIGHTALT)
        mod_alt   = (value != 0);

    if (value == 1) {   /* key press (not repeat, not release) */
        if (code < NUM_KEYS)
            key_counts[code]++;
        total_keypresses++;

        /* Typing speed: measure interval since last press */
        now = ktime_get();
        if (interval_count > 0) {
            long delta_us = (long)ktime_to_us(ktime_sub(now, last_press_time));
            if (delta_us > 0 && delta_us < 5000000L) { /* ignore >5 s gaps */
                interval_sum_us += (unsigned long)delta_us;
                interval_count++;
            }
        } else {
            interval_count = 1;
        }
        last_press_time = now;

        /* Hotkey detection: any non-modifier key pressed with Ctrl or Alt */
        if ((mod_ctrl || mod_alt) &&
            code != KEY_LEFTCTRL  && code != KEY_RIGHTCTRL &&
            code != KEY_LEFTALT   && code != KEY_RIGHTALT  &&
            code != KEY_LEFTSHIFT && code != KEY_RIGHTSHIFT)
        {
            hotkey_count++;
            pr_info("kb_analytics: hotkey detected — ctrl=%d shift=%d alt=%d key=%u\n",
                    mod_ctrl, mod_shift, mod_alt, code);
        }
    }

    spin_unlock_irqrestore(&analytics_lock, flags);
}

static int kb_connect(struct input_handler *handler,
                      struct input_dev *dev,
                      const struct input_device_id *id)
{
    struct input_handle *handle;
    int ret;

    handle = kzalloc(sizeof(*handle), GFP_KERNEL);
    if (!handle)
        return -ENOMEM;

    handle->dev     = dev;
    handle->handler = handler;
    handle->name    = "kb_analytics_handle";

    ret = input_register_handle(handle);
    if (ret) { kfree(handle); return ret; }

    ret = input_open_device(handle);
    if (ret) { input_unregister_handle(handle); kfree(handle); return ret; }

    pr_info("kb_analytics: connected to input device: %s\n", dev->name);
    return 0;
}

static void kb_disconnect(struct input_handle *handle)
{
    pr_info("kb_analytics: disconnected from input device\n");
    input_close_device(handle);
    input_unregister_handle(handle);
    kfree(handle);
}

static const struct input_device_id kb_ids[] = {
    { .flags = INPUT_DEVICE_ID_MATCH_EVBIT,
      .evbit = { BIT_MASK(EV_KEY) } },
    { },
};
MODULE_DEVICE_TABLE(input, kb_ids);

static struct input_handler kb_handler = {
    .event      = kb_event,
    .connect    = kb_connect,
    .disconnect = kb_disconnect,
    .name       = "kb_analytics",
    .id_table   = kb_ids,
};

/* ──────────────────────────────────────────────
 * USB hotplug notifier
 * ────────────────────────────────────────────── */
static int kb_usb_probe(struct usb_interface *intf,
                        const struct usb_device_id *id)
{
    struct usb_device *udev = interface_to_usbdev(intf);
    pr_info("kb_analytics: USB device plugged in — vendor=0x%04x product=0x%04x\n",
            le16_to_cpu(udev->descriptor.idVendor),
            le16_to_cpu(udev->descriptor.idProduct));
    return 0;
}

static void kb_usb_disconnect(struct usb_interface *intf)
{
    pr_info("kb_analytics: USB device unplugged\n");
}

/* Match all USB devices */
static const struct usb_device_id kb_usb_table[] = {
    { USB_DEVICE_INFO(USB_CLASS_HID, 0, 0) },
    { USB_DEVICE_INFO(USB_CLASS_MASS_STORAGE, 0, 0) },
    { USB_DEVICE_INFO(USB_CLASS_VIDEO, 0, 0) },
    { USB_DEVICE_INFO(USB_CLASS_STILL_IMAGE, 0, 0) },
    { }
};
MODULE_DEVICE_TABLE(usb, kb_usb_table);

static struct usb_driver kb_usb_driver = {
    .name       = "kb_analytics_usb",
    .probe      = kb_usb_probe,
    .disconnect = kb_usb_disconnect,
    .id_table   = kb_usb_table,
};

/* ──────────────────────────────────────────────
 * Character device file operations
 * ────────────────────────────────────────────── */
static int dev_open(struct inode *inodep, struct file *filep)
{
    pr_info("kb_analytics: character device opened\n");
    return 0;
}

static int dev_release(struct inode *inodep, struct file *filep)
{
    pr_info("kb_analytics: character device closed\n");
    return 0;
}

/* User space → kernel: receive a text message */
static ssize_t dev_write(struct file *filep, const char __user *buf,
                         size_t len, loff_t *offset)
{
    unsigned long flags;
    size_t copy_len = min(len, (size_t)(BUF_SIZE - 1));

    spin_lock_irqsave(&buf_lock, flags);
    if (copy_from_user(rx_buf, buf, copy_len)) {
        spin_unlock_irqrestore(&buf_lock, flags);
        return -EFAULT;
    }
    rx_buf[copy_len] = '\0';
    pr_info("kb_analytics: received from user space: %s\n", rx_buf);

    /* Prepare the kernel-space reply */
    snprintf(tx_buf, BUF_SIZE, "Hello World from the kernel space");
    tx_len = strlen(tx_buf);
    spin_unlock_irqrestore(&buf_lock, flags);

    return (ssize_t)copy_len;
}

/* Kernel → user space: send the prepared reply */
static ssize_t dev_read(struct file *filep, char __user *buf,
                        size_t len, loff_t *offset)
{
    unsigned long flags;
    int ret;

    spin_lock_irqsave(&buf_lock, flags);
    if (*offset >= tx_len) {
        spin_unlock_irqrestore(&buf_lock, flags);
        return 0;   /* EOF */
    }
    len = min(len, (size_t)(tx_len - (int)*offset));
    ret = copy_to_user(buf, tx_buf + *offset, len);
    if (ret) {
        spin_unlock_irqrestore(&buf_lock, flags);
        return -EFAULT;
    }
    *offset += len;
    spin_unlock_irqrestore(&buf_lock, flags);
    return (ssize_t)len;
}

static const struct file_operations fops = {
    .owner   = THIS_MODULE,
    .open    = dev_open,
    .release = dev_release,
    .read    = dev_read,
    .write   = dev_write,
};

/* ──────────────────────────────────────────────
 * /proc/kb_stats — analytics report
 * ────────────────────────────────────────────── */

/* Linux keycode → printable name (sparse; only common keys shown) */
static const char *keyname(unsigned int code)
{
    static const char *names[256] = {
        [KEY_A]='A'?NULL:NULL,  /* filled below via switch */
    };
    switch (code) {
        case KEY_A: return "A"; case KEY_B: return "B";
        case KEY_C: return "C"; case KEY_D: return "D";
        case KEY_E: return "E"; case KEY_F: return "F";
        case KEY_G: return "G"; case KEY_H: return "H";
        case KEY_I: return "I"; case KEY_J: return "J";
        case KEY_K: return "K"; case KEY_L: return "L";
        case KEY_M: return "M"; case KEY_N: return "N";
        case KEY_O: return "O"; case KEY_P: return "P";
        case KEY_Q: return "Q"; case KEY_R: return "R";
        case KEY_S: return "S"; case KEY_T: return "T";
        case KEY_U: return "U"; case KEY_V: return "V";
        case KEY_W: return "W"; case KEY_X: return "X";
        case KEY_Y: return "Y"; case KEY_Z: return "Z";
        case KEY_SPACE:     return "SPACE";
        case KEY_ENTER:     return "ENTER";
        case KEY_BACKSPACE: return "BACKSPACE";
        case KEY_TAB:       return "TAB";
        case KEY_LEFTCTRL:  return "L-CTRL";
        case KEY_RIGHTCTRL: return "R-CTRL";
        case KEY_LEFTSHIFT: return "L-SHIFT";
        case KEY_RIGHTSHIFT:return "R-SHIFT";
        case KEY_LEFTALT:   return "L-ALT";
        case KEY_RIGHTALT:  return "R-ALT";
        case KEY_1: return "1"; case KEY_2: return "2";
        case KEY_3: return "3"; case KEY_4: return "4";
        case KEY_5: return "5"; case KEY_6: return "6";
        case KEY_7: return "7"; case KEY_8: return "8";
        case KEY_9: return "9"; case KEY_0: return "0";
        case KEY_ESC:    return "ESC";
        case KEY_DELETE: return "DELETE";
        default:         return NULL;
    }
    (void)names;
}

static int stats_show(struct seq_file *m, void *v)
{
    unsigned long flags;
    unsigned long counts_copy[NUM_KEYS];
    unsigned long total, sum_us, count, hkcount;

    spin_lock_irqsave(&analytics_lock, flags);
    memcpy(counts_copy, key_counts, sizeof(key_counts));
    total   = total_keypresses;
    sum_us  = interval_sum_us;
    count   = interval_count;
    hkcount = hotkey_count;
    spin_unlock_irqrestore(&analytics_lock, flags);

    seq_puts(m, "=== Keyboard Analytics Report ===\n\n");
    seq_printf(m, "Total key presses : %lu\n", total);

    /* Typing speed in keys-per-minute */
    if (count > 1) {
        unsigned long avg_us = sum_us / (count - 1);
        unsigned long kpm    = (avg_us > 0) ? (60000000UL / avg_us) : 0;
        seq_printf(m, "Avg interval (µs) : %lu\n", avg_us);
        seq_printf(m, "Typing speed (kpm): %lu\n", kpm);
    } else {
        seq_puts(m, "Typing speed      : not enough data yet\n");
    }

    seq_printf(m, "Hotkey combos     : %lu\n\n", hkcount);

    /* Top-10 most-used keys */
    seq_puts(m, "--- Most-Used Keys (top 10) ---\n");
    {
        /* Simple selection sort over sparse array */
        unsigned int top_code[10];
        unsigned long top_cnt[10];
        int i, j, found;
        bool used[NUM_KEYS];
        memset(used, 0, sizeof(used));

        for (i = 0; i < 10; i++) {
            unsigned long best = 0;
            int best_code = -1;
            for (j = 0; j < NUM_KEYS; j++) {
                if (!used[j] && counts_copy[j] > best) {
                    best = counts_copy[j]; best_code = j;
                }
            }
            if (best_code < 0) break;
            top_code[i] = (unsigned int)best_code;
            top_cnt[i]  = best;
            used[best_code] = true;
            found = i + 1;
            (void)found;
        }
        for (i = 0; i < 10; i++) {
            const char *name = keyname(top_code[i]);
            if (top_cnt[i] == 0) break;
            if (name)
                seq_printf(m, "  %-12s : %lu\n", name, top_cnt[i]);
            else
                seq_printf(m, "  keycode %-4u : %lu\n", top_code[i], top_cnt[i]);
        }
    }

    seq_puts(m, "\n--- All Key Press Counts ---\n");
    {
        int i;
        for (i = 0; i < NUM_KEYS; i++) {
            if (counts_copy[i] == 0) continue;
            const char *name = keyname(i);
            if (name)
                seq_printf(m, "  %-12s : %lu\n", name, counts_copy[i]);
            else
                seq_printf(m, "  keycode %-4d : %lu\n", i, counts_copy[i]);
        }
    }
    return 0;
}

static int stats_open(struct inode *inode, struct file *file)
{
    return single_open(file, stats_show, NULL);
}

static const struct proc_ops stats_fops = {
    .proc_open    = stats_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

/* ──────────────────────────────────────────────
 * Module init / exit
 * ────────────────────────────────────────────── */
static int __init kb_module_init(void)
{
    int ret;

    pr_info("kb_analytics: loading module\n");

    /* 1. Allocate character device region */
    ret = alloc_chrdev_region(&kb_dev, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("kb_analytics: alloc_chrdev_region failed: %d\n", ret);
        return ret;
    }
    major_number = MAJOR(kb_dev);

    /* 2. Init and add cdev */
    cdev_init(&kb_cdev, &fops);
    kb_cdev.owner = THIS_MODULE;
    ret = cdev_add(&kb_cdev, kb_dev, 1);
    if (ret) {
        pr_err("kb_analytics: cdev_add failed: %d\n", ret);
        goto err_region;
    }

    /* 3. Create device class and device node */
    kb_class = class_create(CLASS_NAME);
    if (IS_ERR(kb_class)) {
        ret = PTR_ERR(kb_class);
        pr_err("kb_analytics: class_create failed: %d\n", ret);
        goto err_cdev;
    }

    kb_device = device_create(kb_class, NULL, kb_dev, NULL, DEVICE_NAME);
    if (IS_ERR(kb_device)) {
        ret = PTR_ERR(kb_device);
        pr_err("kb_analytics: device_create failed: %d\n", ret);
        goto err_class;
    }

    /* 4. Register USB driver */
    ret = usb_register(&kb_usb_driver);
    if (ret) {
        pr_err("kb_analytics: usb_register failed: %d\n", ret);
        goto err_device;
    }

    /* 5. Register input handler */
    ret = input_register_handler(&kb_handler);
    if (ret) {
        pr_err("kb_analytics: input_register_handler failed: %d\n", ret);
        goto err_usb;
    }

    /* 6. Create /proc/kb_stats */
    if (!proc_create("kb_stats", 0444, NULL, &stats_fops)) {
        pr_err("kb_analytics: proc_create failed\n");
        ret = -ENOMEM;
        goto err_input;
    }

    memset(key_counts, 0, sizeof(key_counts));
    total_keypresses = 0;
    interval_sum_us  = 0;
    interval_count   = 0;
    hotkey_count     = 0;

    pr_info("kb_analytics: module loaded — /dev/%s (major %d), /proc/kb_stats\n",
            DEVICE_NAME, major_number);
    return 0;

err_input:  input_unregister_handler(&kb_handler);
err_usb:    usb_deregister(&kb_usb_driver);
err_device: device_destroy(kb_class, kb_dev);
err_class:  class_destroy(kb_class);
err_cdev:   cdev_del(&kb_cdev);
err_region: unregister_chrdev_region(kb_dev, 1);
    return ret;
}

static void __exit kb_module_exit(void)
{
    remove_proc_entry("kb_stats", NULL);
    input_unregister_handler(&kb_handler);
    usb_deregister(&kb_usb_driver);
    device_destroy(kb_class, kb_dev);
    class_destroy(kb_class);
    cdev_del(&kb_cdev);
    unregister_chrdev_region(kb_dev, 1);
    pr_info("kb_analytics: module unloaded\n");
}

module_init(kb_module_init);
module_exit(kb_module_exit);
