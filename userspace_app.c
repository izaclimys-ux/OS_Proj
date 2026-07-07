/*
 * userspace_app.c — User-space program for kb_analytics kernel module
 *
 * Demonstrates two interactions with /dev/kb_analytics:
 *
 * Part 1 — Hello-world exchange (required by the assignment spec):
 *   write() → "Hello World from the user space"
 *   read()  ← "Hello World from the kernel space"
 *
 * Part 2 — GET_STATS analytics command (advanced feature — Member 2):
 *   write() → "GET_STATS"
 *   read()  ← live analytics summary: total presses, typing speed (kpm),
 *              average key interval (µs), hotkey combos, top-3 keys by keycode.
 *
 * The kernel module resets the file position (f_pos) to 0 on every write(),
 * so no lseek() is needed between the two interactions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define DEVICE_PATH   "/dev/kb_analytics"
#define BUF_SIZE      512   /* matches the enlarged kernel BUF_SIZE */
#define CMD_GET_STATS "GET_STATS"

/* print_separator — cosmetic helper for readable output */
static void print_separator(void)
{
    printf("────────────────────────────────────────────────────────\n");
}

int main(void)
{
    int     fd;
    char    read_buf[BUF_SIZE];
    ssize_t bytes_written, bytes_read;

    printf("\n=== KB Analytics User-Space Program ===\n\n");

    /* Open the character device in read-write mode.
     * The kernel's dev_open() logs the event via pr_info(). */
    fd = open(DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "ERROR: cannot open %s: %s\n",
                DEVICE_PATH, strerror(errno));
        fprintf(stderr, "Make sure the kernel module is loaded "
                        "(sudo insmod kb_module.ko).\n");
        return EXIT_FAILURE;
    }

    /* ═══════════════════════════════════════════════════════════════════════
     * PART 1: Hello-world write()/read() exchange
     *
     * This is the baseline interaction required by the CSC1107 assignment:
     * the user-space program sends a message to the kernel and reads back
     * the kernel's reply, all through the /dev/kb_analytics character device.
     * ═══════════════════════════════════════════════════════════════════════ */
    print_separator();
    printf("PART 1: Hello-World write()/read() Exchange\n");
    print_separator();

    /* write() system call:
     * The kernel's dev_write() receives this string via copy_from_user(),
     * logs it with pr_info(), and prepares the reply in tx_buf. */
    const char *hello_msg = "Hello World from the user space";
    printf("[USER  ] write() → \"%s\"\n", hello_msg);

    bytes_written = write(fd, hello_msg, strlen(hello_msg));
    if (bytes_written < 0) {
        fprintf(stderr, "ERROR: write() failed: %s\n", strerror(errno));
        close(fd);
        return EXIT_FAILURE;
    }
    printf("[USER  ] write() sent %zd bytes\n\n", bytes_written);

    /* read() system call:
     * The kernel's dev_read() copies tx_buf → user space via copy_to_user().
     * The kernel resets f_pos to 0 on write(), so we start reading from
     * the beginning of the freshly prepared reply buffer. */
    memset(read_buf, 0, sizeof(read_buf));
    bytes_read = read(fd, read_buf, BUF_SIZE - 1);
    if (bytes_read < 0) {
        fprintf(stderr, "ERROR: read() failed: %s\n", strerror(errno));
        close(fd);
        return EXIT_FAILURE;
    }
    read_buf[bytes_read] = '\0';

    printf("[KERNEL] read()  ← \"%s\"\n", read_buf);
    printf("\nHello-world exchange complete.\n");

    /* ═══════════════════════════════════════════════════════════════════════
     * PART 2: GET_STATS — live analytics retrieval via write()/read()
     *
     * Advanced feature (Member 2 — Kernel-to-User Communication Lead):
     *
     * Sending "GET_STATS" to dev_write() causes the kernel module to:
     *   1. Snapshot live counters (total presses, speed, top keys) while
     *      holding analytics_lock — safe from concurrent kb_event() updates.
     *   2. Format the snapshot into a compact string with snprintf() — no
     *      kernel buffer overflow is possible.
     *   3. Store the result in tx_buf and reset the file position to 0.
     *
     * The following read() then retrieves this data via copy_to_user(),
     * demonstrating how a char device can act as a live data interface —
     * not just a fixed hello-world channel.
     *
     * Note: the kernel already resets f_pos to 0 in dev_write(), so no
     * lseek() is required between Part 1 and Part 2.
     * ═══════════════════════════════════════════════════════════════════════ */
    printf("\n");
    print_separator();
    printf("PART 2: GET_STATS — Live Analytics via write()/read()\n");
    print_separator();

    printf("[USER  ] write() → \"%s\"\n", CMD_GET_STATS);

    bytes_written = write(fd, CMD_GET_STATS, strlen(CMD_GET_STATS));
    if (bytes_written < 0) {
        fprintf(stderr, "ERROR: write(GET_STATS) failed: %s\n", strerror(errno));
        close(fd);
        return EXIT_FAILURE;
    }
    printf("[USER  ] write() sent %zd bytes\n\n", bytes_written);

    /* The kernel resets f_pos to 0 when it processes GET_STATS, so this
     * read() starts at the beginning of the newly formatted analytics string. */
    memset(read_buf, 0, sizeof(read_buf));
    bytes_read = read(fd, read_buf, BUF_SIZE - 1);
    if (bytes_read < 0) {
        fprintf(stderr, "ERROR: read() after GET_STATS failed: %s\n",
                strerror(errno));
        close(fd);
        return EXIT_FAILURE;
    }
    read_buf[bytes_read] = '\0';

    printf("[KERNEL] read()  ← \"%s\"\n\n", read_buf);

    /* Key-code reference (Linux input event codes) */
    printf("Key code reference: A=30  S=31  D=32  E=18  T=20\n"
           "                    SPACE=57  ENTER=28  BACKSPACE=14\n\n");

    printf("Tip: run   cat /proc/kb_stats   for the full formatted report.\n");
    printf("     run   dmesg | grep kb_analytics   to see kernel-side log.\n");

    close(fd);   /* triggers dev_release() in the kernel */
    return EXIT_SUCCESS;
}
