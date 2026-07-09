// POSIX declarations (poll, localtime_r) under -std=c11; must precede headers.
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <signal.h>

#define DEVICE_PATH   "/dev/kb_analytics"
#define BUF_SIZE      512   /* matches the enlarged kernel BUF_SIZE */
#define CMD_GET_STATS "GET_STATS"

// print_separator — cosmetic helper for readable output
static void print_separator(void)
{
    printf("────────────────────────────────────────────────────────\n");
}

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int signo) { (void)signo; g_stop = 1; }

static void hhmmss(char *dst, size_t n)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(dst, n, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

// Watch mode: block in poll() (0% CPU) and print activity the instant a key is
// pressed. Run with "./userspace_app poll" (or "watch"); Ctrl-C to stop.
static int run_watch_mode(void)
{
    int  fd;
    char tstamp[16];

    signal(SIGINT, on_sigint);

    fd = open(DEVICE_PATH, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "ERROR: cannot open %s: %s\n",
                DEVICE_PATH, strerror(errno));
        fprintf(stderr, "Make sure the kernel module is loaded "
                        "(sudo insmod kb_module.ko).\n");
        return EXIT_FAILURE;
    }

    print_separator();
    printf("WATCH MODE: poll()-driven keyboard activity monitor\n");
    print_separator();
    printf("Sleeping in poll() — press any key to wake me. Ctrl-C to stop.\n\n");

    while (!g_stop) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int rc;

        rc = poll(&pfd, 1, -1);   /* block until readable; 0% CPU while asleep */

        if (rc < 0) {
            if (errno == EINTR)
                break;
            fprintf(stderr, "ERROR: poll() failed: %s\n", strerror(errno));
            close(fd);
            return EXIT_FAILURE;
        }

        if (pfd.revents & POLLIN) {
            char ev[BUF_SIZE];
            ssize_t n = read(fd, ev, sizeof(ev) - 1);
            if (n < 0) {
                if (errno == EINTR)
                    break;
                fprintf(stderr, "ERROR: read() failed: %s\n", strerror(errno));
                close(fd);
                return EXIT_FAILURE;
            }
            ev[n > 0 ? n : 0] = '\0';
            if (n > 0 && ev[n - 1] == '\n') ev[n - 1] = '\0';

            hhmmss(tstamp, sizeof(tstamp));
            printf("[%s] activity detected  ←  %s\n", tstamp, ev);
            fflush(stdout);
        }
    }

    printf("\nWatch mode stopped. Closing device.\n");
    close(fd);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    int     fd;
    char    read_buf[BUF_SIZE];
    ssize_t bytes_written, bytes_read;

    /* "./userspace_app poll" (or "watch") runs the poll monitor; no args runs
     * the original Hello-World + GET_STATS demo (what run.sh invokes). */
    if (argc > 1 && (strcmp(argv[1], "poll")  == 0 ||
                     strcmp(argv[1], "watch") == 0)) {
        return run_watch_mode();
    }

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

    // PART 1: Hello-world write()/read() exchange
    print_separator();
    printf("PART 1: Hello-World write()/read() Exchange\n");
    print_separator();

    // write() system call
    const char *hello_msg = "Hello World from the user space";
    printf("[USER  ] write() → \"%s\"\n", hello_msg);

    bytes_written = write(fd, hello_msg, strlen(hello_msg));
    if (bytes_written < 0) {
        fprintf(stderr, "ERROR: write() failed: %s\n", strerror(errno));
        close(fd);
        return EXIT_FAILURE;
    }
    printf("[USER  ] write() sent %zd bytes\n\n", bytes_written);

    // read() system call:
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

    // PART 2: GET_STATS
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

    // Key-code reference (Linux input event codes)
    printf("Key code reference: A=30  S=31  D=32  E=18  T=20\n"
           "                    SPACE=57  ENTER=28  BACKSPACE=14\n\n");

    printf("Tip: run   cat /proc/kb_stats   for the full formatted report.\n");
    printf("     run   dmesg | grep kb_analytics   to see kernel-side log.\n");

    close(fd); 
    return EXIT_SUCCESS;
}
