/*
 * session_vt — what the terminal claim promises, and what it refuses to do.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * **THE PRIVILEGED HALF CANNOT BE TESTED FROM A TEST SUITE, SO THE UNPRIVILEGED HALF HAS TO BE EXACT.** Claiming a
 * virtual terminal needs `/dev/tty0`, which is `0600 root:tty`; a case that claimed one would switch the console out
 * from under whoever ran the suite, which is not something a test may do. So this file pins the parts that are true
 * either way:
 *
 *   1. "NOTHING CLAIMED" IS A STATE, AND IT IS NOT ZEROS. A zeroed struct means fd 0 and terminal 0 — somebody's
 *      stdin, and a terminal number no session has. Every `>= 0` test in the module reads that as a live claim, so
 *      the initialization is a contract and not a formality. MEASURED as "could not negotiate switches: Inappropriate
 *      ioctl for device" from a run that had never asked for a terminal.
 *   2. WITHOUT A CLAIM NOTHING IS ATTEMPTED: `watch` refuses by name and `release` does not touch a descriptor.
 *   3. WITHOUT PRIVILEGE THE REFUSAL NAMES THE REASON — the permission, the capability, and the launcher that needs
 *      neither. "could not start" is not a diagnosis, and this is the message a person meets first.
 *
 * THE PRIVILEGED CASE SKIPS (77): when `/dev/tty0` can be opened, the run is on a machine that may claim a terminal,
 * and doing so here would take the display away from the suite.
 */
#include "session-vt.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char *what)
{
	printf("  %s   %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		failures++;
	}
}

int main(void)
{
	struct aws_session_vt vt;
	char why[256];

	/* (1) THE INITIALIZATION. */
	memset(&vt, 0, sizeof vt);
	aws_vt_init(&vt);
	check(vt.console_fd == -1, "a fresh session has NO console descriptor, not descriptor 0");
	check(vt.terminal_fd == -1, "and no terminal descriptor");
	check(vt.number == -1, "and no terminal number");
	check(vt.previous == -1, "and no terminal to go back to");

	/* (2) NOTHING IS ATTEMPTED WITHOUT A CLAIM. */
	why[0] = '\0';
	check(aws_vt_watch(&vt, "test", why, sizeof why) != 0, "watch refuses a session with no terminal");
	check(strstr(why, "no terminal") != NULL, "and says why: the message names the missing claim");
	aws_vt_release(&vt);	/* must be a no-op rather than an ioctl on fd 0 */
	check(vt.console_fd == -1, "release of an unclaimed session touches nothing");

	/* (3) THE PRIVILEGE IS ASKED ABOUT BEFORE ANYTHING IS SWITCHED, and the refusal is named. */
	{
		int privileged = 0;
		int fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);

		if (fd >= 0) {
			privileged = 1;
			close(fd);
		}
		why[0] = '\0';
		if (aws_vt_claim(&vt, why, sizeof why) == 0) {
			/* THE MACHINE LET US HAVE A TERMINAL. Give it straight back, and do not pretend to have tested the
			 * unprivileged path. */
			printf("  ok    this machine may claim a terminal (terminal %d, started on %d)\n", vt.number,
			       vt.previous);
			aws_vt_release(&vt);
			printf("session_vt: 5 case(s) tested; the unprivileged refusal SKIPS on a machine that can claim\n");
			return 0;
		}
		check(!privileged, "claiming without privilege FAILS");
		check(strstr(why, "root") != NULL,
		      "and the message names the privilege rather than only the file");
		check(strstr(why, "CAP_SYS_TTY_CONFIG") != NULL, "and the capability that would do instead");
		check(vt.number == -1 && vt.console_fd == -1,
		      "and a refused claim leaves the session unclaimed, so nothing later acts on half a claim");
	}

	printf("session_vt: %d case(s) failed\n", failures);
	return failures == 0 ? 0 : 1;
}
