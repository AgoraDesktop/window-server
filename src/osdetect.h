/*
 * osdetect.h — what operating system the server is running on, said as data.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * THE IDENTITY IS PARSED ONCE, AT STARTUP, AND THEN ONLY READ. A compositor that asked `uname` or opened
 * `/etc/os-release` while compositing would be doing file I/O in the frame loop, and the answer cannot change
 * while the process runs. The probe ladder and the parsing live in `osdetect.c`; this header is the shape a
 * caller — and a test — needs.
 *
 * `ID` AND `ID_LIKE` ARE THE FREEDESKTOP `os-release` FIELDS, normalized: lower case, quoted values unwrapped,
 * and `ID_LIKE` reduced to a single space-separated list in the order the file gave it. `NAME` is kept for
 * diagnostics only; it is never matched against.
 */

#ifndef AGORA_OSDETECT_H
#define AGORA_OSDETECT_H

#include <stddef.h>

struct aws_os_identity {
	char sysname[64];	/* uname(2): Linux, FreeBSD, OpenBSD, ... */
	char id[64];		/* os-release ID, normalized, or "" */
	char id_like[256];	/* os-release ID_LIKE, normalized, or "" */
	char name[128];		/* NAME / PRETTY_NAME, for the log only */
	const char *source;	/* where the ID came from: a path, "uname", or a probe */
	int is_bsd;		/* uname says this is a BSD (including Darwin) */
};

/* ZERO A NEW IDENTITY, with `source` pointing at a literal rather than at freed memory. */
void aws_os_identity_init(struct aws_os_identity *identity);

/* THE REAL PROBE: uname, os-release, lsb-release, then the distro release files. Returns 0 unless `identity`
 * is NULL; a failed probe is not an error, it is an identity with no ID and the hamburger fallback. */
int aws_os_identity_read(struct aws_os_identity *identity);

/* THE PURE HALF, FOR TESTS: parse the texts a probe would have read. `os_release` and `lsb_release` may be NULL.
 * `source` names where `os_release` came from; `lsb_source` names where `lsb_release` came from. */
void aws_os_identity_from_text(struct aws_os_identity *identity, const char *sysname,
			       const char *os_release, const char *os_release_source,
			       const char *lsb_release, const char *lsb_source);

#endif
