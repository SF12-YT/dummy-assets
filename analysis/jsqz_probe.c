/*
 * Exynos 9820 JSQZ one-shot probe
 * Includes a non-submitting --check-device preflight for /dev/jsqz.
 *
 * Uses an Exynos ION DMA-BUF (not USERPTR), fills a small valid NV21 frame,
 * and optionally submits exactly one JSQZ ioctl.
 *
 * Build on the S10e in Termux:
 *   clang -std=c11 -O2 -Wall -Wextra -o jsqz_probe jsqz_probe.c
 *
 * Safe preparation-only run:
 *   su -c './jsqz_probe'
 *
 * One hardware submission:
 *   su -c './jsqz_probe --execute grey'
 *
 * Other supported patterns: black, white, gradient, checker, luma-checker-4, noise, chroma, chroma-checker, chroma-checker-low, chroma-checker-tiny.
 *
 * The driver waits for hardware completion with wait_for_completion(), not a
 * bounded wait. Do not run --execute until the device is expected to be
 * functional and the user is prepared to reboot if the hardware never IRQs.
 */

#define _GNU_SOURCE
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define WIDTH 128u
#define HEIGHT 64u
#define Y_BYTES ((size_t)WIDTH * HEIGHT)
#define UV_BYTES (Y_BYTES / 2u)
#define FRAME_BYTES (Y_BYTES + UV_BYTES)

#define ION_HEAP_SYSTEM_MASK (1u << 0)
#define ION_FLAG_CACHED 1u

/* Legacy Exynos ION UAPI used by the target 4.14 kernel. */
#define ION_IOC_MAGIC 'I'
struct ion_allocation_data {
    uint64_t len;
    uint32_t heap_id_mask;
    uint32_t flags;
    uint32_t fd;
    uint32_t unused;
};
#define ION_IOC_ALLOC _IOWR(ION_IOC_MAGIC, 0, struct ion_allocation_data)

/* Standard DMA-BUF CPU cache synchronisation UAPI. */
struct dma_buf_sync {
    uint64_t flags;
};
#define DMA_BUF_SYNC_READ  (1u << 0)
#define DMA_BUF_SYNC_WRITE (1u << 1)
#define DMA_BUF_SYNC_START (0u << 2)
#define DMA_BUF_SYNC_END   (1u << 2)
#define DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct dma_buf_sync)

_Static_assert(sizeof(struct ion_allocation_data) == 24, "ION allocation ABI mismatch");
_Static_assert(ION_IOC_ALLOC == 0xC0184900u, "ION allocation ioctl mismatch");
_Static_assert(sizeof(struct dma_buf_sync) == 8, "DMA-BUF sync ABI mismatch");
_Static_assert(DMA_BUF_IOCTL_SYNC == 0x40086200u, "DMA-BUF sync ioctl mismatch");

#define HWSQZ_BUFFER_DMABUF 1u
#define JSQZ_MODE_NORMAL 0u
#define JSQZ_FUNCTION_LIVE 0u
#define JSQZ_INPUT_CS_NV21 0u

struct hwjsqz_img_info {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t cs;
};

struct hwsqz_buffer_64 {
    union {
        int32_t fd;
        unsigned long userptr;
    } handle;
    size_t len;
    uint8_t type;
    uint8_t reserved[7];
};

struct hwjsqz_config {
    uint32_t mode;
    uint32_t function;
};

struct hwjsqz_task_64 {
    struct hwjsqz_img_info info_out;
    struct hwsqz_buffer_64 buf_out[2];
    struct hwjsqz_config config;
    uint32_t buf_q[32];
    uint32_t buf_init_q[32];
    int32_t num_of_buf;
};

#define HWJSQZ_IOC_PROCESS _IOWR('M', 0, struct hwjsqz_task_64)

_Static_assert(sizeof(struct hwjsqz_img_info) == 16, "image info ABI mismatch");
_Static_assert(sizeof(struct hwsqz_buffer_64) == 24, "DMA-BUF descriptor ABI mismatch");
_Static_assert(offsetof(struct hwjsqz_task_64, buf_out) == 16, "task buf offset mismatch");
_Static_assert(offsetof(struct hwjsqz_task_64, config) == 64, "task config offset mismatch");
_Static_assert(offsetof(struct hwjsqz_task_64, buf_q) == 72, "task QT offset mismatch");
_Static_assert(offsetof(struct hwjsqz_task_64, buf_init_q) == 200, "task init QT offset mismatch");
_Static_assert(offsetof(struct hwjsqz_task_64, num_of_buf) == 328, "task count offset mismatch");
_Static_assert(sizeof(struct hwjsqz_task_64) == 336, "native JSQZ ioctl size mismatch");
_Static_assert(HWJSQZ_IOC_PROCESS == 0xC1504D00u, "native JSQZ ioctl number mismatch");

static int allocate_ion_dmabuf(int ion_fd, size_t length)
{
    struct ion_allocation_data allocation = {
        .len = length,
        .heap_id_mask = ION_HEAP_SYSTEM_MASK,
        .flags = ION_FLAG_CACHED,
        .fd = 0,
        .unused = 0,
    };

    if (ioctl(ion_fd, ION_IOC_ALLOC, &allocation) < 0) {
        fprintf(stderr, "ION_IOC_ALLOC failed: %s (errno=%d)\n",
                strerror(errno), errno);
        return -1;
    }

    if (allocation.fd > INT_MAX) {
        fprintf(stderr, "ION returned an invalid DMA-BUF fd: %u\n", allocation.fd);
        close((int32_t)allocation.fd);
        errno = EOVERFLOW;
        return -1;
    }

    return (int)allocation.fd;
}

static int sync_dmabuf(int dma_fd, uint64_t flags)
{
    struct dma_buf_sync sync = { .flags = flags };
    return ioctl(dma_fd, DMA_BUF_IOCTL_SYNC, &sync);
}

static uint32_t next_noise(uint32_t *state)
{
    *state = (*state * 1664525u) + 1013904223u;
    return *state;
}

static int fill_nv21(uint8_t *frame, const char *pattern)
{
    uint32_t seed = 0x9820cafeu;

    if (strcmp(pattern, "grey") && strcmp(pattern, "black") &&
        strcmp(pattern, "white") && strcmp(pattern, "gradient") &&
        strcmp(pattern, "checker") && strcmp(pattern, "luma-checker-4") &&
        strcmp(pattern, "noise") &&
        strcmp(pattern, "chroma") && strcmp(pattern, "chroma-checker") &&
        strcmp(pattern, "chroma-checker-low") && strcmp(pattern, "chroma-checker-tiny")) {
        fprintf(stderr, "Unknown pattern '%s'\n", pattern);
        return -1;
    }

    for (uint32_t y = 0; y < HEIGHT; ++y) {
        for (uint32_t x = 0; x < WIDTH; ++x) {
            uint8_t value = 128;

            if (!strcmp(pattern, "black"))
                value = 16;
            else if (!strcmp(pattern, "white"))
                value = 235;
            else if (!strcmp(pattern, "gradient"))
                value = (uint8_t)(16u + (219u * x) / (WIDTH - 1u));
            else if (!strcmp(pattern, "checker"))
                value = (((x / 8u) + (y / 8u)) & 1u) ? 235 : 16;
            else if (!strcmp(pattern, "luma-checker-4"))
                value = (((x / 4u) + (y / 4u)) & 1u) ? 192 : 64;
            else if (!strcmp(pattern, "noise"))
                value = (uint8_t)(16u + (next_noise(&seed) % 220u));

            frame[(size_t)y * WIDTH + x] = value;
        }
    }

    /*
     * NV21 chroma is interleaved VU.
     * Keep every pattern neutral except the chroma-only tests.
     * "luma-checker-4" alternates Y=64/192 in 4x4 tiles (mean Y=128),
     * so each 8x8 region contains four alternating quadrants.
     * "chroma" is a constant V=192/U=64 offset (already tested).
     * "chroma-checker" alternates VU=(192,64) and (64,192) across 8x8
     * luma blocks (±64 around neutral), with luma fixed at 128.
     * "chroma-checker-low" uses VU=(160,96)/(96,160), ±32.
     * "chroma-checker-tiny" uses VU=(144,112)/(112,144), ±16.
     * Both retain the same geometry. Chroma is 4:2:0,
     * so each 8x8 luma block corresponds to a 4x4 chroma-sample tile.
     */
    memset(frame + Y_BYTES, 128, UV_BYTES);
    if (!strcmp(pattern, "chroma")) {
        for (size_t i = 0; i < UV_BYTES; i += 2u) {
            frame[Y_BYTES + i] = 192;     /* V */
            frame[Y_BYTES + i + 1u] = 64; /* U */
        }
    } else if (!strcmp(pattern, "chroma-checker") ||
               !strcmp(pattern, "chroma-checker-low") ||
               !strcmp(pattern, "chroma-checker-tiny")) {
        const unsigned int contrast =
            !strcmp(pattern, "chroma-checker-tiny") ? 16u :
            !strcmp(pattern, "chroma-checker-low") ? 32u : 64u;
        const uint8_t v_a = (uint8_t)(128u + contrast);
        const uint8_t u_a = (uint8_t)(128u - contrast);
        const uint8_t v_b = (uint8_t)(128u - contrast);
        const uint8_t u_b = (uint8_t)(128u + contrast);

        for (uint32_t cy = 0; cy < HEIGHT / 2u; ++cy) {
            for (uint32_t cx = 0; cx < WIDTH / 2u; ++cx) {
                const int high_vu = (((cx / 4u) + (cy / 4u)) & 1u) != 0;
                const size_t offset = Y_BYTES + (size_t)cy * WIDTH + (size_t)cx * 2u;
                frame[offset] = high_vu ? v_b : v_a;       /* V */
                frame[offset + 1u] = high_vu ? u_b : u_a; /* U */
            }
        }
    }
    return 0;
}

static int save_qtable(const char *path, const uint32_t words[32])
{
    FILE *file;
    size_t written;

    if (!path)
        return 0;
    file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "Cannot create %s: %s\n", path, strerror(errno));
        return -1;
    }
    written = fwrite(words, sizeof(uint32_t), 32, file);
    if (fclose(file) != 0 || written != 32) {
        fprintf(stderr, "Failed writing quantisation table to %s\n", path);
        return -1;
    }
    printf("Saved 128-byte table: %s\n", path);
    return 0;
}

int main(int argc, char **argv)
{
    int execute = 0;
    int check_device = 0;
    const char *pattern = "grey";
    const char *output_path = NULL;
    int pattern_seen = 0;
    int ion_fd = -1;
    int dma_fd = -1;
    int jsqz_fd = -1;
    uint8_t *mapping = MAP_FAILED;
    struct hwjsqz_task_64 task;
    int result = EXIT_FAILURE;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--execute")) {
            execute = 1;
        } else if (!strcmp(argv[i], "--check-device")) {
            check_device = 1;
        } else if (!strcmp(argv[i], "--help")) {
            printf("Usage: %s [--check-device | --execute] [grey|black|white|gradient|checker|luma-checker-4|noise|chroma|chroma-checker|chroma-checker-low|chroma-checker-tiny] [output.bin]\n", argv[0]);
            printf("Default: prepares and frees a DMA-BUF without opening /dev/jsqz.\n");
            return EXIT_SUCCESS;
        } else if (!pattern_seen) {
            pattern = argv[i];
            pattern_seen = 1;
        } else if (!output_path) {
            output_path = argv[i];
        } else {
            fprintf(stderr, "Too many arguments\n");
            return EXIT_FAILURE;
        }
    }

    if (execute && check_device) {
        fprintf(stderr, "--execute and --check-device are mutually exclusive\n");
        return EXIT_FAILURE;
    }

    printf("Target frame: NV21 %ux%u, stride=0, length=%zu bytes\n",
           WIDTH, HEIGHT, FRAME_BYTES);
    printf("Pattern: %s; mode=NORMAL; function=LIVE\n", pattern);
    printf("Native task size=%zu, ioctl=0x%08lx\n",
           sizeof(task), (unsigned long)HWJSQZ_IOC_PROCESS);

    ion_fd = open("/dev/ion", O_RDWR | O_CLOEXEC);
    if (ion_fd < 0) {
        fprintf(stderr, "open /dev/ion failed: %s (errno=%d)\n",
                strerror(errno), errno);
        goto cleanup;
    }

    /* ION_IOC_ALLOC returns a DMA-BUF fd directly on this 4.14 ION ABI. */
    dma_fd = allocate_ion_dmabuf(ion_fd, FRAME_BYTES);
    if (dma_fd < 0)
        goto cleanup;

    mapping = mmap(NULL, FRAME_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, dma_fd, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap of ION DMA-BUF failed: %s\n", strerror(errno));
        goto cleanup;
    }

    if (sync_dmabuf(dma_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE) < 0) {
        fprintf(stderr, "DMA-BUF sync START/WRITE failed: %s (errno=%d)\n",
                strerror(errno), errno);
        goto cleanup;
    }
    if (fill_nv21(mapping, pattern) != 0) {
        (void)sync_dmabuf(dma_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
        goto cleanup;
    }
    if (sync_dmabuf(dma_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) < 0) {
        fprintf(stderr, "DMA-BUF sync END/WRITE failed: %s (errno=%d)\n",
                strerror(errno), errno);
        goto cleanup;
    }

    printf("ION DMA-BUF allocated, mapped and populated successfully (fd=%d).\n", dma_fd);

    if (check_device) {
        jsqz_fd = open("/dev/jsqz", O_RDWR | O_CLOEXEC);
        if (jsqz_fd < 0) {
            fprintf(stderr, "open /dev/jsqz failed: %s (errno=%d)\n",
                    strerror(errno), errno);
            goto cleanup;
        }
        puts("JSQZ device open succeeded; closing without submitting an ioctl.");
        close(jsqz_fd);
        jsqz_fd = -1;
        result = EXIT_SUCCESS;
        goto cleanup;
    }

    if (!execute) {
        puts("DRY RUN: /dev/jsqz was not opened and no hardware ioctl was submitted.");
        result = EXIT_SUCCESS;
        goto cleanup;
    }

    memset(&task, 0, sizeof(task));
    task.info_out.width = WIDTH;
    task.info_out.height = HEIGHT;
    task.info_out.stride = 0;
    task.info_out.cs = JSQZ_INPUT_CS_NV21;
    task.buf_out[0].handle.fd = dma_fd;
    task.buf_out[0].len = FRAME_BYTES;
    task.buf_out[0].type = HWSQZ_BUFFER_DMABUF;
    task.config.mode = JSQZ_MODE_NORMAL;
    task.config.function = JSQZ_FUNCTION_LIVE;
    task.num_of_buf = 1;

    jsqz_fd = open("/dev/jsqz", O_RDWR | O_CLOEXEC);
    if (jsqz_fd < 0) {
        fprintf(stderr, "open /dev/jsqz failed: %s (errno=%d)\n",
                strerror(errno), errno);
        goto cleanup;
    }

    puts("Submitting exactly one JSQZ ioctl...");
    if (ioctl(jsqz_fd, HWJSQZ_IOC_PROCESS, &task) < 0) {
        fprintf(stderr, "JSQZ ioctl failed: %s (errno=%d)\n",
                strerror(errno), errno);
        goto cleanup;
    }

    puts("JSQZ ioctl returned success.");
    puts("Result words (first 16: luma, next 16: chroma):");
    for (unsigned int i = 0; i < 32; ++i)
        printf("%02u: %08" PRIx32 "%s", i, task.buf_q[i], (i % 4u == 3u) ? "\n" : "  ");

    if (save_qtable(output_path, task.buf_q) != 0)
        goto cleanup;

    result = EXIT_SUCCESS;

cleanup:
    if (jsqz_fd >= 0)
        close(jsqz_fd);
    if (mapping != MAP_FAILED)
        munmap(mapping, FRAME_BYTES);
    if (dma_fd >= 0)
        close(dma_fd);
    if (ion_fd >= 0)
        close(ion_fd);
    return result;
}
