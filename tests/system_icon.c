/*
 * system_icon — who does the System menu think it is running on?
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * **THIS IS THE DETECTION, RESOLUTION AND COVERAGE TEST, NOT A SCREENSHOT TEST.** `os-release` is parsed
 * correctly, `ID_LIKE` is tried in order, a family fallback is chosen when no exact mark exists, the hamburger is
 * the final answer rather than a crash, and every mark's coverage grid matches its golden hash and ink count. The
 * real `/etc/os-release` is read once at the end as a smoke test; the integration half lives in
 * `run-system-menu-icons.sh`.
 */
#include "osdetect.h"
#include "systemicon.h"

#include <stdio.h>
#include <string.h>

static int s_failed;
static int s_checks;

static void check(int ok, const char *what)
{
	s_checks++;
	printf("  %s   %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		s_failed++;
	}
}

static const char *resolve_text(const char *sysname, const char *os_release, const char *lsb_release)
{
	struct aws_os_identity identity;
	const struct aws_system_icon *icon;

	aws_os_identity_from_text(&identity, sysname, os_release, "/etc/os-release", lsb_release,
				  "/etc/lsb-release");
	icon = aws_system_menu_icon_resolve(&identity);
	return icon != NULL ? icon->id : "(null)";
}

static const char *family_text(const char *sysname, const char *os_release, const char *lsb_release)
{
	struct aws_os_identity identity;

	aws_os_identity_from_text(&identity, sysname, os_release, "/etc/os-release", lsb_release,
				  "/etc/lsb-release");
	return aws_system_menu_icon_family(&identity);
}

struct fixture {
	const char *sysname;
	const char *os_release;
	const char *lsb_release;
	const char *expected;
};

static const struct fixture s_fixtures[] = {
	/* THE TWELVE EXACT MARKS. */
	{ "Linux", "ID=debian\n", NULL, "debian" },
	{ "Linux", "ID=ubuntu\nID_LIKE=debian\n", NULL, "ubuntu" },
	{ "Linux", "ID=linuxmint\nID_LIKE=\"ubuntu debian\"\n", NULL, "linuxmint" },
	{ "Linux", "ID=fedora\n", NULL, "fedora" },
	{ "Linux", "ID=rhel\nID_LIKE=fedora\n", NULL, "redhat" },
	{ "Linux", "ID=arch\nID_LIKE=archlinux\n", NULL, "archlinux" },
	{ "Linux", "ID=opensuse-tumbleweed\nID_LIKE=\"opensuse suse\"\n", NULL, "opensuse" },
	{ "Linux", "ID=alpine\n", NULL, "alpinelinux" },
	{ "FreeBSD", "ID=freebsd\n", NULL, "freebsd" },
	{ "OpenBSD", NULL, NULL, "openbsd" },
	{ "NetBSD", NULL, NULL, "netbsd" },
	{ "DragonFly", NULL, NULL, "freebsd" },

	/* THE FAMILY FALLBACKS. Each of these has no exact mark; the family must supply one. */
	{ "Linux", "ID=kali\nID_LIKE=debian\n", NULL, "debian" },
	{ "Linux", "ID=raspbian\nID_LIKE=debian\n", NULL, "debian" },
	{ "Linux", "ID=pop\nID_LIKE=\"ubuntu debian\"\n", NULL, "ubuntu" },
	{ "Linux", "ID=elementary\nID_LIKE=\"ubuntu debian\"\n", NULL, "ubuntu" },
	{ "Linux", "ID=nobara\nID_LIKE=fedora\n", NULL, "fedora" },
	{ "Linux", "ID=rocky\nID_LIKE=\"rhel centos fedora\"\n", NULL, "redhat" },
	{ "Linux", "ID=almalinux\nID_LIKE=\"rhel centos fedora\"\n", NULL, "redhat" },
	{ "Linux", "ID=manjaro\nID_LIKE=arch\n", NULL, "archlinux" },
	{ "Linux", "ID=endeavouros\nID_LIKE=arch\n", NULL, "archlinux" },
	{ "Linux", "ID=opensuse-leap\nID_LIKE=\"opensuse suse\"\n", NULL, "opensuse" },
	{ "Linux", "ID=opensuse\n", NULL, "opensuse" },
	{ "Linux", "ID=suse\n", NULL, "opensuse" },
	{ "Linux", "ID=postmarketos\nID_LIKE=alpine\n", NULL, "alpinelinux" },
	{ "FreeBSD", "ID=ghostbsd\nID_LIKE=freebsd\n", NULL, "freebsd" },
	{ "FreeBSD", "ID=nomadbsd\nID_LIKE=freebsd\n", NULL, "freebsd" },

	/* `ID_LIKE` IS TRIED IN ORDER: Ubuntu is closer than Debian and must win. */
	{ "Linux", "ID=zorin\nID_LIKE=\"ubuntu debian\"\n", NULL, "ubuntu" },

	/* NO EXACT MARK AND NO MARKED FAMILY: THE HAMBURGER, NOT A GENERIC LINUX MARK. */
	{ "Linux", "ID=gentoo\n", NULL, "hamburger" },
	{ "Linux", "ID=void\n", NULL, "hamburger" },
	{ "Linux", "ID=nixos\n", NULL, "hamburger" },
	{ "Linux", "ID=slackware\n", NULL, "hamburger" },
	{ "Linux", "ID=unknown\n", NULL, "hamburger" },
	{ "Linux", "ID=linux\n", NULL, "hamburger" },
	{ "Linux", "NAME=Something New\n", NULL, "hamburger" },
	{ "Linux", NULL, NULL, "hamburger" },
	{ "Darwin", NULL, NULL, "hamburger" },
	{ "MidnightBSD", NULL, NULL, "hamburger" },

	/* PARSING: QUOTES, CASE, THE OLDER LSB FILE, AND WHICH FILE WINS. */
	{ "Linux", "ID=\"debian\"\n", NULL, "debian" },
	{ "Linux", "ID='debian'\n", NULL, "debian" },
	{ "Linux", "ID=LinuxMint\n", NULL, "linuxmint" },
	{ "Linux", "ID=\n", "DISTRIB_ID=Ubuntu\n", "ubuntu" },
	{ "Linux", "NAME=Something\n", "DISTRIB_ID=Debian\n", "debian" },
	{ "Linux", "ID=debian\n", "DISTRIB_ID=Ubuntu\n", "debian" },
	{ "Linux", "ID=debian\nID_LIKE=\n", NULL, "debian" },
	{ "Linux", "ID=debian\nPRETTY_NAME=\"Debian GNU/Linux\"\n", NULL, "debian" },

	/* THE PLATFORM WINS A DISAGREEMENT. A BSD jail with a Debian userland is still a BSD. */
	{ "FreeBSD", "ID=debian\n", NULL, "freebsd" },
	{ "Linux", "ID=freebsd\n", NULL, "hamburger" },
};

static void test_fixtures(void)
{
	size_t i;

	printf("fixtures\n");
	for (i = 0; i < sizeof s_fixtures / sizeof s_fixtures[0]; i++) {
		const struct fixture *fixture = &s_fixtures[i];
		const char *got = resolve_text(fixture->sysname, fixture->os_release, fixture->lsb_release);
		char label[256];

		snprintf(label, sizeof label, "%s%s%s -> %s", fixture->sysname,
			 fixture->os_release != NULL ? " + os-release" : "",
			 fixture->lsb_release != NULL ? " + lsb-release" : "", fixture->expected);
		check(strcmp(got, fixture->expected) == 0, label);
		if (strcmp(got, fixture->expected) != 0) {
			printf("        expected %s, got %s\n", fixture->expected, got);
		}
	}
}

static void test_families(void)
{
	printf("families\n");
	check(strcmp(family_text("Linux", "ID=kali\n", NULL), "debian") == 0,
	      "Kali reports the Debian family");
	check(strcmp(family_text("Linux", "ID=linuxmint\n", NULL), "ubuntu") == 0,
	      "Linux Mint reports the Ubuntu family even though it has an exact mark");
	check(strcmp(family_text("Linux", "ID=rocky\n", NULL), "redhat") == 0,
	      "Rocky reports the Red Hat family");
	check(strcmp(family_text("Linux", "ID=manjaro\n", NULL), "arch") == 0,
	      "Manjaro reports the Arch family");
	check(strcmp(family_text("FreeBSD", "ID=ghostbsd\n", NULL), "freebsd") == 0,
	      "GhostBSD reports the FreeBSD family");
	check(strcmp(family_text("OpenBSD", NULL, NULL), "openbsd") == 0,
	      "OpenBSD's sysname reports the OpenBSD family");
	check(strcmp(family_text("DragonFly", NULL, NULL), "freebsd") == 0,
	      "DragonFly reports the FreeBSD family for now");
	check(strcmp(family_text("Darwin", NULL, NULL), "bsd") == 0,
	      "Darwin reports the generic BSD family");
	check(strcmp(family_text("Linux", "ID=gentoo\n", NULL), "gentoo") == 0,
	      "Gentoo reports its own family, which resolves to the hamburger");
	check(strcmp(family_text("Linux", "ID=unknown\n", NULL), "unknown") == 0,
	      "an unknown ID reports the unknown family");
}

static void test_names(void)
{
	printf("names\n");
	check(aws_system_icon_by_name("debian") == aws_system_icon_by_id("debian"),
	      "an exact icon id is accepted");
	check(aws_system_icon_by_name("mint") == aws_system_icon_by_id("linuxmint"),
	      "an alias (mint) resolves to its mark");
	check(aws_system_icon_by_name("arch") == aws_system_icon_by_id("archlinux"),
	      "a distro id (arch) resolves through its alias");
	check(aws_system_icon_by_name("suse") == aws_system_icon_by_id("opensuse"),
	      "a family name (suse) resolves to the family mark");
	check(aws_system_icon_by_name("rocky") == aws_system_icon_by_id("redhat"),
	      "a derivative id (rocky) resolves to the family mark");
	check(aws_system_icon_by_name("dragonfly") == aws_system_icon_by_id("freebsd"),
	      "the DragonFly alias resolves to the FreeBSD mark for now");
	check(aws_system_icon_by_name("DEBIAN") == aws_system_icon_by_id("debian"),
	      "an override name is normalized to lower case");
	check(aws_system_icon_by_name("hamburger") == aws_system_icon_by_id("hamburger"),
	      "the fallback is itself a valid name");
	check(aws_system_icon_by_name("no-such-mark") == NULL, "an unknown name is rejected");
	check(aws_system_icon_by_name("") == NULL, "an empty name is rejected");
	check(aws_system_icon_by_name(NULL) == NULL, "a NULL name is rejected");
}

static void test_table(void)
{
	size_t i;
	size_t j;
	int found_fallback = 0;
	int unique = 1;

	printf("table\n");
	check(aws_system_icon_count() == 12,
	      "the table carries eleven exact marks plus the hamburger; DragonFly shares FreeBSD");
	for (i = 0; i < aws_system_icon_count(); i++) {
		const struct aws_system_icon *icon = aws_system_icon_at(i);

		check(icon != NULL && icon->id != NULL && icon->id[0] != '\0',
		      "every table entry has an id");
		if (icon != NULL && strcmp(icon->id, AWS_SYSTEM_MENU_ICON_FALLBACK) == 0) {
			found_fallback = 1;
		}
		for (j = i + 1; icon != NULL && j < aws_system_icon_count(); j++) {
			const struct aws_system_icon *other = aws_system_icon_at(j);

			if (other != NULL && strcmp(icon->id, other->id) == 0) {
				unique = 0;
			}
		}
	}
	check(unique, "the icon ids are unique");
	check(found_fallback, "the hamburger fallback is in the table");
	check(aws_system_icon_at(aws_system_icon_count()) == NULL, "an out-of-range index is rejected");
}

static uint64_t fnv1a64(const uint8_t *data, size_t count)
{
	uint64_t hash = 1469598103934665603ULL;
	size_t i;

	for (i = 0; i < count; i++) {
		hash ^= data[i];
		hash *= 1099511628211ULL;
	}
	return hash;
}

static void test_coverage(void)
{
	static const int full_rows[] = { 1, 2, 7, 8, 13, 14 };
	static const int empty_rows[] = { 4, 5, 10, 11 };
	struct aws_system_icon blank;
	struct aws_system_icon *hamburger;
	const uint8_t *coverage;
	size_t i;
	int all_zero = 1;

	printf("coverage\n");
	hamburger = aws_system_icon_by_id("hamburger");
	check(hamburger != NULL, "the hamburger has an icon object");
	coverage = aws_system_icon_coverage(hamburger);
	check(coverage != NULL, "the hamburger has a coverage grid");
	check(hamburger->paths != NULL && hamburger->path_count == 1,
	      "the hamburger is one vector path, not three hard-coded bars");

	for (i = 0; i < sizeof full_rows / sizeof full_rows[0]; i++) {
		char label[96];

		snprintf(label, sizeof label, "box row %d is fully ink", full_rows[i]);
		check(coverage[full_rows[i] * AWS_SYSTEM_ICON_PIXELS + 8] == 255, label);
	}
	for (i = 0; i < sizeof empty_rows / sizeof empty_rows[0]; i++) {
		char label[96];

		snprintf(label, sizeof label, "box row %d is the bar between bars", empty_rows[i]);
		check(coverage[empty_rows[i] * AWS_SYSTEM_ICON_PIXELS + 8] == 0, label);
	}
	check(coverage[0] > 0 && coverage[0] < 255, "the top edge is anti-aliased, not a hard step");
	check(coverage[3 * AWS_SYSTEM_ICON_PIXELS + 8] > 0 &&
		      coverage[3 * AWS_SYSTEM_ICON_PIXELS + 8] < 255,
	      "the bottom edge of the first bar is anti-aliased");
	check(coverage[1 * AWS_SYSTEM_ICON_PIXELS] == 255 &&
		      coverage[1 * AWS_SYSTEM_ICON_PIXELS + 15] == 255,
	      "the bars reach both sides of the box");
	check(aws_system_icon_coverage(hamburger) == coverage, "the coverage grid is cached");
	check(fnv1a64(coverage, AWS_SYSTEM_ICON_COVERAGE_BYTES) == 0xa447ca5f9cd6dc23ULL,
	      "the hamburger's coverage matches the golden hash");

	memset(&blank, 0, sizeof blank);
	blank.id = "blank";
	blank.fill = AWS_ICON_FILL_EVEN_ODD;
	coverage = aws_system_icon_coverage(&blank);
	check(coverage != NULL, "an icon with no paths still answers a grid");
	for (i = 0; i < AWS_SYSTEM_ICON_COVERAGE_BYTES; i++) {
		if (coverage[i] != 0) {
			all_zero = 0;
		}
	}
	check(all_zero, "and that grid is empty, not a crash");
	check(aws_system_icon_coverage(NULL) == NULL, "a NULL icon answers NULL");
}

struct icon_golden {
	const char *id;
	uint64_t hash;
	unsigned ink;
};

/* THE PER-MARK GOLDEN. A hash catches a path change, a fill-rule change, or a rasterizer change; the ink count
 * catches an icon that becomes a single dot without moving the hash far enough to notice. Update these deliberately
 * when the `.icon` data changes. */
static const struct icon_golden s_icon_golden[] = {
	{ "alpinelinux", 0xcecfe9489b671c79ULL, 180 },
	{ "archlinux", 0x5ffd3b05441e3425ULL, 99 },
	{ "debian", 0x0caf57fd36fb2958ULL, 91 },
	{ "fedora", 0x3410d015b525ee11ULL, 223 },
	{ "freebsd", 0x2d7e074f6adc8933ULL, 204 },
	{ "linuxmint", 0x3460f12e02895badULL, 175 },
	{ "netbsd", 0x756d93143027f78cULL, 102 },
	{ "openbsd", 0xa0ed3feba265f2e1ULL, 137 },
	{ "opensuse", 0xb68d29e6c7d76d87ULL, 155 },
	{ "redhat", 0x0a4c3906e72dfb14ULL, 142 },
	{ "ubuntu", 0x1e29664bb47d42d2ULL, 137 },
	{ "hamburger", 0xa447ca5f9cd6dc23ULL, 192 },
};

static void test_mark_coverage(void)
{
	size_t i;

	printf("mark coverage\n");
	for (i = 0; i < sizeof s_icon_golden / sizeof s_icon_golden[0]; i++) {
		const struct icon_golden *golden = &s_icon_golden[i];
		struct aws_system_icon *icon = aws_system_icon_by_id(golden->id);
		const uint8_t *coverage;
		unsigned ink = 0;
		size_t pixel;
		char label[128];

		check(icon != NULL && icon->paths != NULL && icon->path_count == 1,
		      "the mark has one vector path");
		if (icon == NULL) {
			continue;
		}
		coverage = aws_system_icon_coverage(icon);
		for (pixel = 0; pixel < AWS_SYSTEM_ICON_COVERAGE_BYTES; pixel++) {
			if (coverage[pixel] != 0) {
				ink++;
			}
		}
		snprintf(label, sizeof label, "%s: ink pixel count is %u", golden->id, golden->ink);
		check(ink == golden->ink, label);
		snprintf(label, sizeof label, "%s: coverage hash matches the golden", golden->id);
		check(fnv1a64(coverage, AWS_SYSTEM_ICON_COVERAGE_BYTES) == golden->hash, label);
		snprintf(label, sizeof label, "%s: uses the %s fill rule", golden->id,
			 strcmp(golden->id, "hamburger") == 0 ? "even-odd" : "nonzero");
		check(icon->fill == (strcmp(golden->id, "hamburger") == 0 ? AWS_ICON_FILL_EVEN_ODD
								       : AWS_ICON_FILL_NONZERO),
		      label);
	}
}

static void test_host(void)
{
	struct aws_os_identity identity;
	const struct aws_system_icon *icon;

	printf("host\n");
	check(aws_os_identity_read(&identity) == 0, "the real probe runs");
	check(identity.source != NULL && identity.source[0] != '\0', "the real probe names its source");
	icon = aws_system_menu_icon_resolve(&identity);
	check(icon != NULL && icon->id != NULL, "the real identity resolves to some mark");
	check(aws_system_menu_icon_family(&identity) != NULL, "the real identity resolves to some family");
	printf("        host: sysname=%s ID=%s ID_LIKE=%s family=%s mark=%s source=%s\n",
	       identity.sysname, identity.id[0] != '\0' ? identity.id : "(none)",
	       identity.id_like[0] != '\0' ? identity.id_like : "(none)",
	       aws_system_menu_icon_family(&identity), icon != NULL ? icon->id : "(null)",
	       identity.source);
}

int main(void)
{
	test_fixtures();
	test_families();
	test_names();
	test_table();
	test_coverage();
	test_mark_coverage();
	test_host();

	printf("system_icon: %d check(s), %d failed\n", s_checks, s_failed);
	return s_failed == 0 ? 0 : 1;
}
