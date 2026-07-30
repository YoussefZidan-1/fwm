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

#ifndef FWM_RAM_H
#define FWM_RAM_H

#include <sys/types.h>

/*
 * How much memory an application is using, for [physics] mass = "ram".
 *
 * Split into a snapshot and a lookup because a browser is not one process. The
 * question "how much RAM is Chrome using" can only be answered by adding up a
 * whole process tree, and every window on screen wants that answer at the same
 * moment — so /proc is walked once and then queried per window, rather than
 * once per window.
 */

/* Walk /proc and remember every process's parent and resident size. Costs one
 * readdir plus one small read per process (a few ms on a busy machine), so it
 * belongs on a timer of seconds, not in the frame loop. */
void ram_snapshot(void);

/* Resident memory of `pid` plus every descendant of it, in MB, from the last
 * snapshot. 0 if the pid was not in it — which is also what an empty snapshot
 * returns for everything, so a caller that never called ram_snapshot sees
 * "unknown" rather than "weightless". */
double ram_tree_mb(pid_t pid);

/* Resident memory of one process only, in MB. Mostly for tests: the tree sum
 * for a leaf process must equal its own footprint. */
double ram_own_mb(pid_t pid);

#endif /* FWM_RAM_H */
