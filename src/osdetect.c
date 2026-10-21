/*
 * osdetect.c — the probe ladder behind `struct aws_os_identity`.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed.
 * SPDX-License-Identifier: MIT
 *
 * THE ORDER IS THE POINT. `/etc/os-release` is the freedesktop answer and the best one; `/usr/lib/os-release` is
 * its vendor fallback; `/etc/lsb-release` is the older answer; and the per-distro release files are the last
 * resort for systems that never adopted `os-release` at all. `uname` is not a fallback for the distro — it is the
 * PLATFORM, and on a BSD it is often the only reliable answer because there may be no `os-release` on disk.
 *
 * NOTHING HERE SHELLS OUT. `uname(2)` is a syscall, the files are read with `fopen`, and a missing or malformed
 * file is an ordinary answer rather than an error: the compositor still has to start, and the hamburger is a
 * perfectly good System menu.
 */

#include "osdetect.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#define AWS_OS_TEXT_MAX 8192

struct parsed_release {
	char id[64];
	char id_like[256];
	char name[128];
	int got_id;
};

static void copy_string(char *destination, size_t capacity, const char *source)
{
	if (capacity == 0) {
		return;
	}
	if (source == NULL) {
		destination[0] = '\0';
		return;
	}
	snprintf(destination, capacity, "%s", source);
}

static char *trim(char *text)
{
	char *end;

	while (*text != '\0' && isspace((unsigned char)*text)) {
		text++;
	}
	end = text + strlen(text);
	while (end > text && isspace((unsigned char)end[-1])) {
		end--;
	}
	*end = '\0';
	return text;
}

/* THE QUOTES ARE PART OF THE VALUE AND NOT PART OF THE ANSWER. `os-release` permits shell quoting; a double-quoted
 * value may also escape the quote, the backslash, `$` and a backtick, which is the small subset this parser needs
 * to unwrap. */
static void unquote(char *value)
{
	size_t length = strlen(value);
	char quote;

	if (length < 2) {
		return;
	}
	quote = value[0];
	if ((quote != '"' && quote != '\'') || value[length - 1] != quote) {
		return;
	}
	value[length - 1] = '\0';
	memmove(value, value + 1, length - 1);
	if (quote == '"') {
		char *read = value;
		char *write = value;

		while (*read != '\0') {
			if (*read == '\\' &&
			    (read[1] == '"' || read[1] == '\\' || read[1] == '$' || read[1] == '`')) {
				read++;
			}
			*write++ = *read++;
		}
		*write = '\0';
	}
}

/* `ID` IS ONE TOKEN. Everything outside the permitted alphabet is dropped rather than becoming a path component
 * later, and the value is lower-cased because the spec says it is already lower case and a file that is not should
 * still resolve. */
static void normalize_id(const char *value, char *destination, size_t capacity)
{
	size_t used = 0;

	if (capacity == 0) {
		return;
	}
	while (*value != '\0' && isspace((unsigned char)*value)) {
		value++;
	}
	while (*value != '\0' && !isspace((unsigned char)*value)) {
		unsigned char character = (unsigned char)*value++;

		if (character >= 'A' && character <= 'Z') {
			character = (unsigned char)(character - 'A' + 'a');
		}
		if ((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
		    character == '.' || character == '_' || character == '-') {
			if (used + 1 < capacity) {
				destination[used++] = (char)character;
			}
		}
	}
	destination[used] = '\0';
}

/* `ID_LIKE` IS A SPACE-SEPARATED LIST IN ORDER OF CLOSENESS. The order is kept: the resolver tries each token in
 * turn, so `ID_LIKE="ubuntu debian"` prefers Ubuntu and falls back to Debian. */
static void normalize_list(const char *value, char *destination, size_t capacity)
{
	size_t used = 0;

	if (capacity == 0) {
		return;
	}
	destination[0] = '\0';
	while (*value != '\0') {
		char token[64];
		size_t token_length = 0;

		while (*value != '\0' && isspace((unsigned char)*value)) {
			value++;
		}
		if (*value == '\0') {
			break;
		}
		while (*value != '\0' && !isspace((unsigned char)*value)) {
			unsigned char character = (unsigned char)*value++;

			if (character >= 'A' && character <= 'Z') {
				character = (unsigned char)(character - 'A' + 'a');
			}
			if ((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
			    character == '.' || character == '_' || character == '-') {
				if (token_length + 1 < sizeof token) {
					token[token_length++] = (char)character;
				}
			}
		}
		token[token_length] = '\0';
		if (token_length == 0) {
			continue;
		}
		if (used > 0 && used + 1 < capacity) {
			destination[used++] = ' ';
		}
		for (size_t i = 0; token[i] != '\0' && used + 1 < capacity; i++) {
			destination[used++] = token[i];
		}
	}
	destination[used] = '\0';
}

static void parse_release_text(const char *text, const char *id_key, struct parsed_release *parsed)
{
	char buffer[AWS_OS_TEXT_MAX];
	char *line;

	if (text == NULL) {
		return;
	}
	snprintf(buffer, sizeof buffer, "%s", text);
	line = buffer;
	while (line != NULL && *line != '\0') {
		char *next = strchr(line, '\n');
		char *key;
		char *value;
		char *equals;

		if (next != NULL) {
			*next++ = '\0';
		}
		key = trim(line);
		if (*key == '\0' || *key == '#') {
			line = next;
			continue;
		}
		if (strncmp(key, "export ", 7) == 0) {
			key = trim(key + 7);
		}
		equals = strchr(key, '=');
		if (equals == NULL) {
			line = next;
			continue;
		}
		*equals = '\0';
		value = trim(equals + 1);
		key = trim(key);
		unquote(value);

		if (strcmp(key, id_key) == 0) {
			normalize_id(value, parsed->id, sizeof parsed->id);
			parsed->got_id = parsed->id[0] != '\0';
		} else if (strcmp(key, "ID_LIKE") == 0) {
			normalize_list(value, parsed->id_like, sizeof parsed->id_like);
		} else if (strcmp(key, "NAME") == 0) {
			copy_string(parsed->name, sizeof parsed->name, value);
		} else if (strcmp(key, "PRETTY_NAME") == 0 || strcmp(key, "DISTRIB_DESCRIPTION") == 0) {
			if (parsed->name[0] == '\0') {
				copy_string(parsed->name, sizeof parsed->name, value);
			}
		}
		line = next;
	}
}

static int string_equal_ci(const char *a, const char *b)
{
	while (*a != '\0' && *b != '\0') {
		unsigned char left = (unsigned char)tolower((unsigned char)*a++);
		unsigned char right = (unsigned char)tolower((unsigned char)*b++);

		if (left != right) {
			return 0;
		}
	}
	return *a == '\0' && *b == '\0';
}

static int contains_ci(const char *haystack, const char *needle)
{
	size_t length = strlen(needle);

	if (length == 0) {
		return 1;
	}
	for (; *haystack != '\0'; haystack++) {
		size_t i;

		for (i = 0; i < length; i++) {
			if (haystack[i] == '\0') {
				return 0;
			}
			if (tolower((unsigned char)haystack[i]) != tolower((unsigned char)needle[i])) {
				break;
			}
		}
		if (i == length) {
			return 1;
		}
	}
	return 0;
}

static const char *bsd_id_for_sysname(const char *sysname)
{
	if (string_equal_ci(sysname, "FreeBSD")) {
		return "freebsd";
	}
	if (string_equal_ci(sysname, "OpenBSD")) {
		return "openbsd";
	}
	if (string_equal_ci(sysname, "NetBSD")) {
		return "netbsd";
	}
	if (string_equal_ci(sysname, "DragonFly")) {
		return "dragonfly";
	}
	if (string_equal_ci(sysname, "MidnightBSD")) {
		return "midnightbsd";
	}
	if (string_equal_ci(sysname, "HardenedBSD")) {
		return "hardenedbsd";
	}
	if (string_equal_ci(sysname, "Darwin")) {
		return "darwin";
	}
	return NULL;
}

static int id_is_bsd(const char *id)
{
	return strcmp(id, "freebsd") == 0 || strcmp(id, "openbsd") == 0 || strcmp(id, "netbsd") == 0 ||
	       strcmp(id, "dragonfly") == 0 || strcmp(id, "dragonflybsd") == 0 ||
	       strcmp(id, "midnightbsd") == 0 || strcmp(id, "hardenedbsd") == 0 ||
	       strcmp(id, "ghostbsd") == 0 || strcmp(id, "nomadbsd") == 0 || strcmp(id, "darwin") == 0 ||
	       strcmp(id, "bsd") == 0;
}

static int read_text_file(const char *path, char *buffer, size_t capacity)
{
	FILE *file;
	size_t got;

	if (capacity == 0) {
		return -1;
	}
	file = fopen(path, "r");
	if (file == NULL) {
		return -1;
	}
	got = fread(buffer, 1, capacity - 1, file);
	buffer[got] = '\0';
	fclose(file);
	return 0;
}

/* THE RELEASE FILES ARE THE LAST RESORT AND THEY ARE NAMED ONE AT A TIME. A directory scan would be a guessing
 * game over somebody else's filesystem; a list of the files the distros actually ship is a table a reader can
 * check. The `redhat-release` file carries several distros, so it is parsed rather than merely detected. */
static const char *probe_release_file(char *id, size_t capacity)
{
	char text[AWS_OS_TEXT_MAX];
	const char *probed = NULL;

	if (read_text_file("/etc/redhat-release", text, sizeof text) == 0) {
		if (contains_ci(text, "Rocky")) {
			probed = "rocky";
		} else if (contains_ci(text, "AlmaLinux")) {
			probed = "almalinux";
		} else if (contains_ci(text, "CentOS")) {
			probed = "centos";
		} else if (contains_ci(text, "Oracle")) {
			probed = "oracle";
		} else if (contains_ci(text, "Scientific")) {
			probed = "scientific";
		} else if (contains_ci(text, "Fedora")) {
			probed = "fedora";
		} else if (contains_ci(text, "Red Hat")) {
			probed = "rhel";
		}
		if (probed != NULL) {
			copy_string(id, capacity, probed);
			return "/etc/redhat-release";
		}
	}
	if (access("/etc/fedora-release", F_OK) == 0) {
		copy_string(id, capacity, "fedora");
		return "/etc/fedora-release";
	}
	if (access("/etc/debian_version", F_OK) == 0) {
		copy_string(id, capacity, "debian");
		return "/etc/debian_version";
	}
	if (access("/etc/arch-release", F_OK) == 0) {
		copy_string(id, capacity, "arch");
		return "/etc/arch-release";
	}
	if (access("/etc/gentoo-release", F_OK) == 0) {
		copy_string(id, capacity, "gentoo");
		return "/etc/gentoo-release";
	}
	if (access("/etc/SuSE-release", F_OK) == 0 || access("/etc/SuSE-brand", F_OK) == 0) {
		copy_string(id, capacity, "opensuse");
		return "/etc/SuSE-release";
	}
	if (access("/etc/alpine-release", F_OK) == 0) {
		copy_string(id, capacity, "alpine");
		return "/etc/alpine-release";
	}
	if (access("/etc/slackware-version", F_OK) == 0) {
		copy_string(id, capacity, "slackware");
		return "/etc/slackware-version";
	}
	if (access("/etc/nixos", F_OK) == 0) {
		copy_string(id, capacity, "nixos");
		return "/etc/nixos";
	}
	return NULL;
}

void aws_os_identity_init(struct aws_os_identity *identity)
{
	if (identity == NULL) {
		return;
	}
	memset(identity, 0, sizeof *identity);
	identity->source = "uname";
}

void aws_os_identity_from_text(struct aws_os_identity *identity, const char *sysname,
			       const char *os_release, const char *os_release_source,
			       const char *lsb_release, const char *lsb_source)
{
	struct parsed_release operating_system;
	struct parsed_release lsb;
	const char *bsd_id;

	if (identity == NULL) {
		return;
	}
	memset(&operating_system, 0, sizeof operating_system);
	memset(&lsb, 0, sizeof lsb);
	aws_os_identity_init(identity);
	copy_string(identity->sysname, sizeof identity->sysname, sysname);
	bsd_id = bsd_id_for_sysname(identity->sysname);
	identity->is_bsd = bsd_id != NULL;

	parse_release_text(os_release, "ID", &operating_system);
	parse_release_text(lsb_release, "DISTRIB_ID", &lsb);

	if (operating_system.got_id) {
		copy_string(identity->id, sizeof identity->id, operating_system.id);
		copy_string(identity->id_like, sizeof identity->id_like, operating_system.id_like);
		copy_string(identity->name, sizeof identity->name, operating_system.name);
		identity->source = os_release_source != NULL ? os_release_source : "os-release";
	} else if (lsb.got_id) {
		copy_string(identity->id, sizeof identity->id, lsb.id);
		copy_string(identity->id_like, sizeof identity->id_like, lsb.id_like);
		copy_string(identity->name, sizeof identity->name, lsb.name);
		identity->source = lsb_source != NULL ? lsb_source : "lsb-release";
	} else {
		copy_string(identity->name, sizeof identity->name,
			    operating_system.name[0] != '\0' ? operating_system.name : lsb.name);
		identity->source = "uname";
	}

	/* **`uname` OWNS THE PLATFORM; `os-release` OWNS THE DISTRO.** A FreeBSD jail with a Debian userland, or a
	 * Linux chroot on a BSD host, must not make the bar claim the wrong family. When the two disagree the sysname
	 * wins and the disagreement is recorded in `source`, where `--dump-os-identity` can show it. */
	if (identity->is_bsd) {
		if (identity->id[0] != '\0' && !id_is_bsd(identity->id)) {
			identity->id[0] = '\0';
			identity->id_like[0] = '\0';
			identity->source = "uname (os-release ID ignored: BSD/Linux mismatch)";
		}
		if (identity->id[0] == '\0' && bsd_id != NULL) {
			copy_string(identity->id, sizeof identity->id, bsd_id);
			identity->source = "uname";
		}
	} else if (identity->id[0] != '\0' && id_is_bsd(identity->id)) {
		identity->id[0] = '\0';
		identity->id_like[0] = '\0';
		identity->source = "uname (os-release ID ignored: BSD/Linux mismatch)";
	}
}

int aws_os_identity_read(struct aws_os_identity *identity)
{
	struct utsname uts;
	char sysname[64] = "";
	char os_text[AWS_OS_TEXT_MAX];
	char lsb_text[AWS_OS_TEXT_MAX];
	const char *os_source = NULL;
	const char *lsb_source = NULL;
	int have_os = 0;
	int have_lsb = 0;

	if (identity == NULL) {
		return -1;
	}
	if (uname(&uts) == 0) {
		copy_string(sysname, sizeof sysname, uts.sysname);
	}
	if (read_text_file("/etc/os-release", os_text, sizeof os_text) == 0) {
		have_os = 1;
		os_source = "/etc/os-release";
	} else if (read_text_file("/usr/lib/os-release", os_text, sizeof os_text) == 0) {
		have_os = 1;
		os_source = "/usr/lib/os-release";
	}
	if (read_text_file("/etc/lsb-release", lsb_text, sizeof lsb_text) == 0) {
		have_lsb = 1;
		lsb_source = "/etc/lsb-release";
	}

	aws_os_identity_from_text(identity, sysname, have_os ? os_text : NULL, os_source,
				  have_lsb ? lsb_text : NULL, lsb_source);

	if (identity->id[0] == '\0') {
		char probed_id[64];
		const char *probed_source = probe_release_file(probed_id, sizeof probed_id);

		if (probed_source != NULL) {
			copy_string(identity->id, sizeof identity->id, probed_id);
			identity->source = probed_source;
		}
	}
	return 0;
}
