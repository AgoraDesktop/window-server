/*
 * session-vt.h — the virtual terminal this compositor lives on.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See `session-vt.c`: claiming a terminal, negotiating its switches, and giving it back. `--launcher vt` is the only
 * caller, and it is opt-in because the claim needs root.
 */
#ifndef AWS_SESSION_VT_H
#define AWS_SESSION_VT_H

#include <stddef.h>

struct aws_session_vt {
	int console_fd;		/* /dev/tty0: the door the VT ioctls go through */
	int terminal_fd;		/* /dev/ttyN: the terminal itself, this process's controlling one */
	int number;		/* which terminal we claimed */
	int previous;		/* which one we came from, to go back to on exit */
};

/* Put the struct into its "nothing claimed" state. Called before anything else touches it — a zeroed struct means
 * fd 0 and terminal 0, which every `>= 0` test reads as a live claim. */
void aws_vt_init(struct aws_session_vt *vt);

/* **CLAIM THE TERMINAL THE CALLER NAMED, AND NEVER ONE IT PICKED ITSELF.** `wanted` must be a terminal number the
 * person chose (`--vt N`); 0 or less is refused, because "the first free terminal" is a getty's on any machine with a
 * display manager and taking it moves a person off their session. The terminal currently active is refused too.
 * 0 on success; on failure `why` says what the kernel or the permission said, because "could not start" is not a
 * diagnosis. */
int aws_vt_claim(struct aws_session_vt *vt, int wanted, char *why, size_t why_size);

/* Ask the kernel to negotiate switches (VT_PROCESS). After this, SIGUSR1 means "give the display up" and SIGUSR2 means
 * "it is yours again" — and each has to be answered with `aws_vt_acknowledge` or the switch stalls. */
int aws_vt_watch(struct aws_session_vt *vt, const char *program, char *why, size_t why_size);

/* Answer the kernel. `release` says the display has been given up; `acquire` says it has been taken back. They are
 * different values of `VT_RELDISP`, and answering the acquire with the release value stalls the terminal. */
void aws_vt_ack_release(struct aws_session_vt *vt);
void aws_vt_ack_acquire(struct aws_session_vt *vt);

/* Put the terminal back, close it, and switch the console to where it came from. */
void aws_vt_release(struct aws_session_vt *vt);

#endif /* AWS_SESSION_VT_H */
