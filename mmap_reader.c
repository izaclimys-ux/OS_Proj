/*
 * mmap_reader.c — User-space reader for the kb_analytics shared stats page
 *
 * ADVANCED FEATURE (Virtual Memory):
 *
 * This program maps the kernel's stats page directly into its own address
 * space with mmap(). After the single mmap() call, every stat read is just a
 * plain memory access — there is NO read()/write() system call and NO
 * copy_to_user() per read. The kernel and this process share one physical
 * page through two separate page-table mappings; when the kernel updates a
 * counter on a key press, this program sees the new value on its very next
 * dereference.
 *
 * Contrast with userspace_app.c, which uses write("GET_STATS") + read() and
 * therefore crosses the kernel/user boundary (and copies) on every request.
 *
 * Build : make         
 * Run   : sudo ./mmap_reader        
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE   /* required for usleep() under -std=c11 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>

#define DEVICE_PATH "/dev/kb_analytics"

/*
 * MUST match struct kb_shared_stats in kb_module.c byte-for-byte.
 * Fixed-width types guarantee the layout is identical on both sides.
 */
struct kb_shared_stats {
    uint64_t total_presses;
    uint64_t typing_speed_kpm;
    uint64_t avg_interval_us;
    uint64_t hotkey_count;
    uint64_t last_keycode;
    uint32_t seq;      /* reserved for stage-two seqlock */
    uint32_t _pad;
};

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int signo) { (void)signo; g_stop = 1; }

int main(void)
{
    int   fd;
    long  page_size = sysconf(_SC_PAGESIZE);
    const volatile struct kb_shared_stats *stats;
    void *map;

    signal(SIGINT, on_sigint);

    /* Open read-only: the shared page is a read-only window for user space. */
    fd = open(DEVICE_PATH, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "ERROR: cannot open %s: %s\n",
                DEVICE_PATH, strerror(errno));
        fprintf(stderr, "Is the module loaded? (sudo insmod kb_module.ko)\n");
        return EXIT_FAILURE;
    }

    /*
     * The single mmap() that sets everything up. PROT_READ + MAP_SHARED asks
     * the kernel (via our dev_mmap handler) to map its stats page here.
     * Offset 0 selects the start of that page.
     */
    map = mmap(NULL, (size_t)page_size, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "ERROR: mmap() failed: %s\n", strerror(errno));
        close(fd);
        return EXIT_FAILURE;
    }

    /* We can close the fd now — the mapping stays valid until munmap(). */
    close(fd);

    stats = (const volatile struct kb_shared_stats *)map;

    printf("=== KB Analytics — mmap live reader ===\n");
    printf("Mapped the kernel stats page (no read() syscalls from here on).\n");
    printf("Type on the keyboard; values update live. Ctrl-C to stop.\n\n");

    while (!g_stop) {
        /*
         * Plain memory reads — no system call. 'volatile' forces the compiler
         * to re-fetch each field from the shared page every loop instead of
         * caching it in a register.
         */
        printf("\rpresses=%-8llu  kpm=%-5llu  avg_us=%-8llu  hotkeys=%-4llu  last_key=%-4llu",
               (unsigned long long)stats->total_presses,
               (unsigned long long)stats->typing_speed_kpm,
               (unsigned long long)stats->avg_interval_us,
               (unsigned long long)stats->hotkey_count,
               (unsigned long long)stats->last_keycode);
        fflush(stdout);
        usleep(200000);   /* 200 ms refresh — purely for display, not for data */
    }

    printf("\n\nStopping. Unmapping shared page.\n");
    munmap(map, (size_t)page_size);
    return EXIT_SUCCESS;
}
