/*
 * session-vt.c — the virtual terminal this compositor lives on.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * **A COMPOSITOR ON REAL HARDWARE OWNS A VT, AND UNTIL THIS FILE EXISTED THIS ONE DID NOT HAVE THE CONCEPT.** It could
 * be *started from* a text console — become that console's session by being run there — but it could not take a
 * terminal of its own, and nothing handled the kernel's request to give the display up. Both halves are the same
 * protocol and both are here:
 *
 *   CLAIMING. `VT_OPENQRY` asks the kernel which terminal is free, `VT_ACTIVATE` switches to it, `VT_WAITACTIVE` waits
 *   until the switch has actually happened (they are not the same moment — the notification is asynchronous and a
 *   compositor that modesets before the kernel has moved the console is drawing on somebody else's screen), and then
 *   `setsid` + `TIOCSCTTY` make it this process's controlling terminal. WITHOUT THE LAST STEP THE CLAIM IS HOLLOW: a
 *   process on no terminal is not the session the kernel hands the display to, and the DRM master it takes the next
 *   moment would be master of a screen nobody is looking at.
 *
 *   KEEPING IT. `VT_SETMODE(VT_PROCESS)` says "ask me before you take this terminal away" instead of the kernel
 *   switching underneath a process that believes it is still drawing. The kernel then sends `SIGUSR1` (release) and
 *   `SIGUSR2` (acquire), and each one must be answered with `VT_RELDISP` or the switch stalls — which is what an
 *   unanswered Ctrl-Alt-F1 looks like: a keyboard that has stopped working.
 *
 * **THE SIGNALS ARE NOT HANDLED HERE.** A signal handler that drops DRM master and redraws would be doing real work in
 * async-signal context, which is not a place to do it. They are registered on the Wayland event loop by `main.c`, in
 * the same way `SIGINT` already is, so a switch is handled by the ordinary render path at a point where the state it
 * touches is not being mutated.
 *
 * **AND IT NEEDS PRIVILEGE, WHICH IS WHY IT IS OPT-IN.** `/dev/tty0` is `0600 root:tty`, so claiming a terminal is root
 * (or `CAP_SYS_TTY_CONFIG`) and nothing else. `--launcher existing` — the default and the old behaviour — runs on
 * whatever terminal it was started from and needs no privilege at all; `--launcher vt` asks for one and says what it
 * could not do if it cannot.
 */

#define _GNU_SOURCE

#include "session-vt.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/vt.h>

static void vt_why(char *why, size_t size, const char *what)
{
	if (why != NULL && size > 0) {
		snprintf(why, size, "%s: %s", what, strerror(errno));
	}
}

/* **"/dev/tty0" IS THE CONSOLE'S OWN DOOR, NOT A TERMINAL.** The ioctls that ask about and switch terminals go through
 * it, and the terminal they act on is named by number; the terminal this process then LIVES on is `/dev/ttyN`, a
 * different file with a different job. Both are opened and both are kept — the first for the lifetime of the session
 * (a switch is asked through it), the second because it is the controlling terminal. */
/* **"NO TERMINAL HAS BEEN CLAIMED" HAS TO BE A STATE THIS STRUCT KNOWS.** A zeroed struct is not it: fd 0 is somebody's
 * stdin, and `number` 0 is not a terminal either — so an unclaimed session left as zeros looks, to every `>= 0` test,
 * exactly like a claimed one on fd 0, and the first switch negotiation is an ioctl against a pipe. MEASURED as
 * "could not ask the kernel to negotiate switches: Inappropriate ioctl for device" from a run that had not asked for a
 * terminal at all. */
void aws_vt_init(struct aws_session_vt *vt)
{
	memset(vt, 0, sizeof *vt);
	vt->console_fd = -1;
	vt->terminal_fd = -1;
	vt->number = -1;
	vt->previous = -1;
}

int aws_vt_claim(struct aws_session_vt *vt, char *why, size_t why_size)
{
	struct vt_stat state;
	char path[32];
	int number = -1;

	aws_vt_init(vt);

	vt->console_fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
	if (vt->console_fd < 0) {
		vt_why(why, why_size, "could not open /dev/tty0 — claiming a terminal needs root or CAP_SYS_TTY_CONFIG");
		return -1;
	}
	/* THE TERMINAL THIS PROCESS CAME FROM, SO EXIT CAN GO BACK TO IT. `VT_GETSTATE` answers both numbers at once: the
	 * active one and, in the same struct, the number of the last `VT_OPENQRY` — which is why the two requests are used
	 * together rather than each with its own ioctl. */
	if (ioctl(vt->console_fd, VT_GETSTATE, &state) == 0) {
		vt->previous = state.v_active;
	}
	if (ioctl(vt->console_fd, VT_OPENQRY, &number) < 0 || number <= 0) {
		vt_why(why, why_size, "the kernel has no free terminal to give (VT_OPENQRY)");
		aws_vt_release(vt);
		return -1;
	}
	vt->number = number;

	/* **SWITCH, THEN WAIT, AND THE ORDER IS THE POINT.** `VT_ACTIVATE` asks; `VT_WAITACTIVE` is what turns the ask into
	 * a fact — it blocks until the kernel has moved the console. A compositor that skipped the wait would take DRM
	 * master and modeset while another session's terminal was still the active one. */
	if (ioctl(vt->console_fd, VT_ACTIVATE, vt->number) < 0) {
		vt_why(why, why_size, "could not activate the free terminal (VT_ACTIVATE)");
		aws_vt_release(vt);
		return -1;
	}
	if (ioctl(vt->console_fd, VT_WAITACTIVE, vt->number) < 0) {
		vt_why(why, why_size, "the terminal never became active (VT_WAITACTIVE)");
		aws_vt_release(vt);
		return -1;
	}

	/* **AND THE TERMINAL BECOMES THIS PROCESS'S OWN.** `setsid` detaches from the session it was started in, and
	 * `TIOCSCTTY` on the new terminal claims it as the controlling one — the pair is what makes this the session the
	 * kernel's VT bookkeeping names when it asks who owns the screen. */
	snprintf(path, sizeof path, "/dev/tty%d", vt->number);
	vt->terminal_fd = open(path, O_RDWR | O_CLOEXEC);
	if (vt->terminal_fd < 0) {
		vt_why(why, why_size, "could not open the terminal it had just been given");
		aws_vt_release(vt);
		return -1;
	}
	if (setsid() < 0) {
		vt_why(why, why_size, "could not start a new session (setsid)");
		aws_vt_release(vt);
		return -1;
	}
	if (ioctl(vt->terminal_fd, TIOCSCTTY, 0) < 0) {
		vt_why(why, why_size, "could not take the terminal as the controlling one (TIOCSCTTY)");
		aws_vt_release(vt);
		return -1;
	}
	return 0;
}

/* **"ASK ME BEFORE YOU TAKE IT", AND THE ANSWER IS `VT_RELDISP`.** `VT_PROCESS` moves the switching protocol into this
 * process: the kernel sends `SIGUSR1` when it wants the terminal and waits for `VT_RELDISP(ACK)` before it takes it,
 * and `SIGUSR2` when the terminal is coming back. `VT_ACKACQ` makes the acquire side explicit too, so a resume is a
 * handshake rather than a race.
 *
 * WITHOUT THIS A SWITCH IS A SURPRISE: the console moves, this process goes on composing into a CRTC it no longer owns,
 * and coming back leaves the screen showing whatever the kernel felt like — the "keyboard stopped working" shape of a
 * wedged VT. */
int aws_vt_watch(struct aws_session_vt *vt, const char *program, char *why, size_t why_size)
{
	struct vt_mode mode;

	if (vt->console_fd < 0 || vt->number < 0) {
		if (why != NULL && why_size > 0) {
			snprintf(why, why_size, "no terminal has been claimed");
		}
		return -1;
	}
	memset(&mode, 0, sizeof mode);
	mode.mode = VT_PROCESS;
	mode.waitv = 0;
	mode.relsig = SIGUSR1;		/* "give the display up" */
	mode.acqsig = SIGUSR2;		/* "it is yours again" */
	/* THE KERNEL'S OWN `VT_RELDISP` REPLY ARRIVES AS A SIGNAL, and `SIGUSR1`/`SIGUSR2` are addressed to the PROCESS —
	 * so the handlers are registered on the event loop by the caller, which is where the display can actually be
	 * released. */
	(void)program;
	if (ioctl(vt->console_fd, VT_SETMODE, &mode) < 0) {
		vt_why(why, why_size, "could not ask the kernel to negotiate switches (VT_SETMODE)");
		return -1;
	}
	return 0;
}

/* **THE TWO ANSWERS, AND THEY ARE DIFFERENT NUMBERS.** `VT_RELDISP` takes one of three: 0 refuses, 1 acknowledges the
 * release, and `VT_ACKACQ` acknowledges the acquire. Answering the acquire with 1 is the classic mistake — the kernel is
 * told "yes, you may take it" when what it asked was "you have it back", and the terminal stalls. */
void aws_vt_ack_release(struct aws_session_vt *vt)
{
	if (vt->console_fd >= 0) {
		(void)ioctl(vt->console_fd, VT_RELDISP, 1);
	}
}

void aws_vt_ack_acquire(struct aws_session_vt *vt)
{
	if (vt->console_fd >= 0) {
		(void)ioctl(vt->console_fd, VT_RELDISP, VT_ACKACQ);
	}
}

/* **GIVING IT BACK, WHICH IS THREE THINGS AND NOT ONE.** The terminal is put out of `VT_PROCESS` first, so the kernel
 * stops asking a process that is on its way out; the terminal is closed; and the console is switched back to whatever
 * this session came from — because leaving a dark screen in front of a person who pressed Ctrl-C is not an exit, it is
 * a hang with a message. */
void aws_vt_release(struct aws_session_vt *vt)
{
	if (vt == NULL) {
		return;
	}
	if (vt->console_fd >= 0 && vt->number >= 0 && vt->terminal_fd >= 0) {
		struct vt_mode mode;

		memset(&mode, 0, sizeof mode);
		mode.mode = VT_AUTO;
		(void)ioctl(vt->console_fd, VT_SETMODE, &mode);
		/* BACK TO WHERE WE CAME FROM — but never to ourselves, and never when there was nothing there. */
		if (vt->previous > 0 && vt->previous != vt->number) {
			(void)ioctl(vt->console_fd, VT_ACTIVATE, vt->previous);
			(void)ioctl(vt->console_fd, VT_WAITACTIVE, vt->previous);
		}
	}
	if (vt->terminal_fd >= 0) {
		close(vt->terminal_fd);
		vt->terminal_fd = -1;
	}
	if (vt->console_fd >= 0) {
		close(vt->console_fd);
		vt->console_fd = -1;
	}
}
