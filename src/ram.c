/*
 * fwm — a Wayland compositor
 * Copyright (C) 2026 Ilu
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

/* Reading application memory footprints out of /proc. See ram.h. */
#include "ram.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Enough for any desktop machine; a system with more processes than this simply
 * has the tail of the list ignored, which costs a window a little weight and
 * nothing else. */
#define RAM_MAX_PROCS 8192

/* A pid chain longer than this is a corrupt snapshot (pid reuse between the
 * readdir and the reads can in principle produce a cycle), and walking one
 * forever would hang the compositor. */
#define RAM_MAX_DEPTH 64

struct Proc {
    pid_t  pid;
    pid_t  ppid;
    double mb;
};

/* Sorted by pid, so the ancestor walk below can find a parent by bisection
 * instead of scanning the whole table for every step of every chain. */
static struct Proc g_proc[RAM_MAX_PROCS];
static int g_count;

static int cmp_proc(const void *a, const void *b) {
    pid_t pa = ((const struct Proc *)a)->pid;
    pid_t pb = ((const struct Proc *)b)->pid;
    return pa < pb ? -1 : (pa > pb ? 1 : 0);
}

static int find_proc(pid_t pid) {
    int lo = 0, hi = g_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (g_proc[mid].pid == pid) return mid;
        if (g_proc[mid].pid < pid) lo = mid + 1;
        else                       hi = mid - 1;
    }
    return -1;
}

/* Parent pid and resident pages out of /proc/<pid>/stat.
 *
 * Parsed from the LAST ')' rather than by counting fields from the start: the
 * second field is the executable name in parentheses and may itself contain
 * spaces and brackets, which is the classic way this parse goes wrong. After
 * that bracket the fields are state (3), ppid (4), ... rss (24). */
static int read_stat(pid_t pid, pid_t *ppid, long *rss_pages) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return 0;
    buf[n] = '\0';

    char *p = strrchr(buf, ')');
    if (!p) return 0;
    p++;

    /* Field 3 is the first token here, so field k is token k - 3. */
    long ppid_v = 0, rss_v = 0;
    int got = 0;
    char *save = NULL;
    int idx = 0;
    for (char *tok = strtok_r(p, " \t\n", &save); tok; tok = strtok_r(NULL, " \t\n", &save)) {
        if (idx == 1)  { ppid_v = strtol(tok, NULL, 10); got |= 1; }
        if (idx == 21) { rss_v  = strtol(tok, NULL, 10); got |= 2; break; }
        idx++;
    }
    if (got != 3) return 0;

    *ppid = (pid_t)ppid_v;
    *rss_pages = rss_v < 0 ? 0 : rss_v;
    return 1;
}

void ram_snapshot(void) {
    g_count = 0;

    DIR *d = opendir("/proc");
    if (!d) return;

    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;
    double page_mb = (double)page / (1024.0 * 1024.0);

    struct dirent *e;
    while ((e = readdir(d)) != NULL && g_count < RAM_MAX_PROCS) {
        /* Only the numeric entries are processes, and "." / ".." fail this on
         * the first character. */
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        char *end = NULL;
        long v = strtol(e->d_name, &end, 10);
        if (!end || *end != '\0' || v <= 0) continue;

        pid_t ppid = 0;
        long rss = 0;
        /* A process that exited between the readdir and the read is normal, not
         * an error: skip it and keep going. */
        if (!read_stat((pid_t)v, &ppid, &rss)) continue;

        g_proc[g_count].pid  = (pid_t)v;
        g_proc[g_count].ppid = ppid;
        g_proc[g_count].mb   = (double)rss * page_mb;
        g_count++;
    }
    closedir(d);

    qsort(g_proc, (size_t)g_count, sizeof(g_proc[0]), cmp_proc);
}

double ram_own_mb(pid_t pid) {
    if (pid <= 0) return 0.0;
    int i = find_proc(pid);
    return i < 0 ? 0.0 : g_proc[i].mb;
}

double ram_tree_mb(pid_t pid) {
    if (pid <= 0) return 0.0;
    if (find_proc(pid) < 0) return 0.0;

    /* Every process is walked up toward init and counted against `pid` if it
     * passes through it. Done this way round rather than descending the tree
     * because /proc gives us parents, not children, and building the child
     * lists would cost more than the walk does.
     *
     * Shared pages are counted once per process that maps them, so a browser's
     * total is generous. That is the correct kind of wrong here: the point of
     * the mode is that the memory hog is visibly the heavy thing on screen. */
    double total = 0.0;
    for (int i = 0; i < g_count; i++) {
        pid_t cur = g_proc[i].pid;
        int idx = i;
        for (int depth = 0; depth < RAM_MAX_DEPTH; depth++) {
            if (cur == pid) { total += g_proc[i].mb; break; }
            pid_t parent = g_proc[idx].ppid;
            if (parent <= 0) break;             /* reached init/kthreadd */
            idx = find_proc(parent);
            if (idx < 0) break;                 /* parent gone: chain ends here */
            cur = parent;
        }
    }
    return total;
}
