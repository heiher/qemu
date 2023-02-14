/*
 * Copyright (C) 2022, 2023, hev <r@hev.cc>
 *
 * License: GNU GPL, version 2 or later.
 *   See the COPYING file in the top-level directory.
 */

/*
 * WARNING: This plugin is *not* Thread-safe!
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <glib.h>

#include <qemu-plugin.h>

#define SHM_COUNT_SIZE    (128 * 1024)
#define SHM_INAME_SIZE    (128 * 1024)
#define SHM_TOTAL_SIZE    (SHM_COUNT_SIZE + SHM_INAME_SIZE)

typedef struct {
    uint64_t count;
    uint64_t iname_off;
} Counter;

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static GHashTable *insns;
static int shm_count_off;
static int shm_iname_off;
static char iname_end;
static int iname_off;
static void *shm_ptr;
static int shm_fd;
static uint64_t low_bound;
static uint64_t high_bound;

static int plugin_init(const qemu_info_t *info)
{
    char path[256];

    if (strstr(info->target_name, "loongarch64")) {
        iname_off = 8 + 3; /* %08x<sp><sp><sp> */
        iname_end = '\t';
    } else if (strstr(info->target_name, "aarch64")) {
        iname_off = 0;
        iname_end = ' ';
    } else if (strstr(info->target_name, "riscv64")) {
        iname_off = 18;
        iname_end = ' ';
    } else {
        fprintf(stderr, "Target %s is unsupported!\n", info->target_name);
        return -1;
    }

    snprintf(path, sizeof(path), "/dev/shm/insncounts.%s", info->target_name);
    shm_fd = open(path, O_RDWR | O_CREAT, 0644);
    if (shm_fd < 0) {
        fprintf(stderr, "Open shared memory file failed!\n");
        return -1;
    }

    if (ftruncate(shm_fd, 0) < 0) {
        fprintf(stderr, "Shrink shared memory file failed!\n");
        close(shm_fd);
        return -1;
    }

    if (ftruncate(shm_fd, SHM_TOTAL_SIZE) < 0) {
        fprintf(stderr, "Expand shared memory file failed!\n");
        close(shm_fd);
        return -1;
    }

    shm_ptr = mmap(NULL, SHM_TOTAL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                   shm_fd, 0);
    if (shm_ptr == MAP_FAILED) {
        fprintf(stderr, "Map shared memory file failed!\n");
        close(shm_fd);
        return -1;
    }

    shm_iname_off = SHM_COUNT_SIZE;
    insns = g_hash_table_new(g_str_hash, g_str_equal);

    return 0;
}

static void insns_foreach (gpointer key, gpointer value, gpointer data)
{
    const char *fmt = "    %-12s\t%"PRId64"\n";
    GString *report = data;
    Counter *counter = value;
    char *name = key;

    g_string_append_printf(report, fmt, name, counter->count);
}

static void plugin_exit(qemu_plugin_id_t id, void *p)
{
    g_autoptr(GString) report = g_string_new("Collected:\n");

    g_hash_table_foreach(insns, insns_foreach, report);
    qemu_plugin_outs(report->str);

    g_hash_table_unref(insns);
    munmap(shm_ptr, SHM_TOTAL_SIZE);
    close(shm_fd);
}

static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    size_t i;

    for (i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t vaddr = qemu_plugin_insn_vaddr(insn);
        char *disas;

        if ((vaddr < low_bound) || (vaddr > high_bound))
            continue;

        disas = qemu_plugin_insn_disas(insn);
        if (*disas != 'A') { /* Skip 'Address 0x???? is out of bounds.' */
            char *name = disas + iname_off;
            char *p = strchr(name, iname_end);
            Counter *counter;

            *p = '\0';
            counter = g_hash_table_lookup(insns, name);
            if (!counter) {
                int iname_len;

                counter = shm_ptr + shm_count_off;
                counter->iname_off = shm_iname_off;
                counter->count = 0;

                iname_len = p - name + 1;
                memcpy(shm_ptr + shm_iname_off, name, iname_len);
                name = shm_ptr + shm_iname_off;

                shm_count_off += sizeof(Counter);
                shm_iname_off += iname_len;

                g_hash_table_insert(insns, name, counter);
            }

            qemu_plugin_register_vcpu_insn_exec_inline(insn,
                        QEMU_PLUGIN_INLINE_ADD_U64, &counter->count, 1);
        }
        g_free(disas);
    }
}

QEMU_PLUGIN_EXPORT
int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
                        int argc, char **argv)
{
    int i;

    for (i = 0; i < argc; i++) {
        char *opt = argv[i];
        g_autofree char **tokens = g_strsplit(opt, "=", -1);

        if (g_strcmp0(tokens[0], "low") == 0) {
            low_bound = g_ascii_strtoull(tokens[1], NULL, 16);
        } else if (g_strcmp0(tokens[0], "high") == 0) {
            high_bound = g_ascii_strtoull(tokens[1], NULL, 16);
        } else {
            fprintf(stderr, "option parsing failed: %s\n", opt);
            return -1;
        }
    }

    if (plugin_init(info) < 0)
        return -1;

    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);

    return 0;
}
