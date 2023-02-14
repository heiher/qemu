/*
 * Copyright (C) 2022, hev <r@hev.cc>
 *
 * License: GNU GPL, version 2 or later.
 *   See the COPYING file in the top-level directory.
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/mman.h>

#define SHM_COUNT_SIZE    (128 * 1024)
#define SHM_INAME_SIZE    (128 * 1024)
#define SHM_TOTAL_SIZE    (SHM_COUNT_SIZE + SHM_INAME_SIZE)

typedef struct {
    uint64_t count;
    uint64_t iname_off;
} Counter;

int main(int argc, char *argv[])
{
    char path[256];
    void *shm_ptr;
    int shm_fd;
    int reset;
    int off;

    if (argc < 2) {
        fprintf(stderr, "%s target [reset]\n", argv[0]);
        return -1;
    }

    snprintf(path, sizeof(path), "/dev/shm/insncounts.%s", argv[1]);
    shm_fd = open(path, O_RDWR);
    if (shm_fd < 0) {
        fprintf(stderr, "Open shared memory file failed!\n");
        return -1;
    }

    shm_ptr = mmap(NULL, SHM_TOTAL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                   shm_fd, 0);
    if (shm_ptr == MAP_FAILED) {
        fprintf(stderr, "Map shared memory file failed!\n");
        close(shm_fd);
        return -1;
    }

    if (argc > 2)
        reset = 1;
    else
        reset = 0;

    for (off = 0; off < SHM_COUNT_SIZE; off += sizeof(Counter)) {
        Counter *counter = shm_ptr + off;

        if (counter->iname_off == 0)
            break;

        if (reset) {
            counter->count = 0;
        } else {
            const char *name = shm_ptr + counter->iname_off;
            const char *fmt = "    %-12s\t%"PRId64"\n";

            printf(fmt, name, counter->count);
        }
    }

    munmap(shm_ptr, SHM_TOTAL_SIZE);
    close(shm_fd);

    return 0;
}
