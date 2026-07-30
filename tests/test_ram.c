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

/* ram.c parses /proc/<pid>/stat, whose second field is an executable name in
 * brackets that may itself contain brackets and spaces — the classic way this
 * parse goes wrong, and a wrong parse here reads a random number as a memory
 * footprint and throws a window across the screen with it. The process tree is
 * checked against a child this test forks itself, because "the browser's total
 * includes its renderers" is the entire point of the tree walk. */

#include "test.h"
#include "ram.h"

#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static void test_own_footprint(void) {
    CASE("own rss");
    ram_snapshot();

    /* This very process is running, so it must be in the snapshot with a
     * plausible size — a test binary is more than nothing and less than a
     * machine's worth of memory. */
    double self = ram_own_mb(getpid());
    CHECK(self > 0.0);
    CHECK(self < 4096.0);

    /* Nobody's pid, and nothing to say about it. Zero rather than a guess:
     * server_mass_sync reads that as "leave this window's weight alone". */
    CHECK_DBL(ram_own_mb(-1), 0.0, 1e-9);
    CHECK_DBL(ram_tree_mb(0), 0.0, 1e-9);
}

static void test_tree_includes_children(void) {
    CASE("tree");

    /* A child that does nothing but stay alive, so the parent's TREE is
     * measurably bigger than the parent alone — which is what makes a browser
     * heavy while each of its processes is ordinary. */
    pid_t child = fork();
    if (child == 0) {
        pause();
        _exit(0);
    }
    CHECK(child > 0);
    if (child <= 0) return;

    /* fork() returns before the child is necessarily scheduled, and a process
     * with no /proc entry yet would simply be missing from the snapshot. */
    for (int i = 0; i < 100; i++) {
        ram_snapshot();
        if (ram_own_mb(child) > 0.0) break;
        usleep(10000);
    }

    double own  = ram_own_mb(getpid());
    double tree = ram_tree_mb(getpid());
    CHECK(own > 0.0);
    CHECK(tree > own);                       /* the child is counted */
    CHECK(tree >= own + ram_own_mb(child) - 1e-9);

    /* The child is a leaf, so its tree is itself. */
    CHECK_DBL(ram_tree_mb(child), ram_own_mb(child), 1e-9);

    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
}

static void test_unsampled_is_unknown(void) {
    CASE("no snapshot");
    /* A query before any snapshot must answer "unknown" and not crash: the
     * arrays are empty, and a lookup into them is exactly the kind of thing
     * that reads off the end if the count is not respected. */
    CHECK_DBL(ram_tree_mb(getpid()), 0.0, 1e-9);
}

int main(void) {
    /* Before ram_snapshot has ever run, on purpose. */
    test_unsampled_is_unknown();
    test_own_footprint();
    test_tree_includes_children();
    return t_report("ram");
}
