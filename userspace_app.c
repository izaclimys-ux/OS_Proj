/*
 * userspace_app.c — User-space program for kb_analytics kernel module
 *
 * 1. Opens /dev/kb_analytics
 * 2. Sends  write() → "Hello World from the user space"
 * 3. Reads back    ← "Hello World from the kernel space"
 * 4. Prints both messages to stdout
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define DEVICE_PATH  "/dev/kb_analytics"
#define BUF_SIZE     256

int main(void)
{
    int  fd;
    char write_msg[] = "Hello World from the user space";
    char read_buf[BUF_SIZE];
    ssize_t bytes_written, bytes_read;

    printf("=== KB Analytics User-Space Program ===\n\n");

    /* Open the character device */
    fd = open(DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "ERROR: cannot open %s: %s\n",
                DEVICE_PATH, strerror(errno));
        fprintf(stderr, "Make sure the kernel module is loaded.\n");
        return EXIT_FAILURE;
    }

    /* ── write() system call ── */
    printf("[USER ] Sending write() → \"%s\"\n", write_msg);
    bytes_written = write(fd, write_msg, strlen(write_msg));
    if (bytes_written < 0) {
        fprintf(stderr, "ERROR: write() failed: %s\n", strerror(errno));
        close(fd);
        return EXIT_FAILURE;
    }
    printf("[USER ] write() sent %zd bytes\n\n", bytes_written);

    /* ── read() system call ── */
    memset(read_buf, 0, sizeof(read_buf));
    bytes_read = read(fd, read_buf, BUF_SIZE - 1);
    if (bytes_read < 0) {
        fprintf(stderr, "ERROR: read() failed: %s\n", strerror(errno));
        close(fd);
        return EXIT_FAILURE;
    }
    read_buf[bytes_read] = '\0';

    printf("[KERNEL] Received via read() → \"%s\"\n\n", read_buf);
    printf("Both messages exchanged successfully.\n");
    printf("Run   dmesg | tail -20   to see kernel-side log output.\n");

    close(fd);
    return EXIT_SUCCESS;
}
