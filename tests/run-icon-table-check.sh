# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later
# Portions of this file may derive from GNUstep; see THIRD-PARTY.md.

#!/bin/bash
# The generated table must be current. Edit the TSVs, run the generator, and commit both.
set -u

PYTHON="${1:?usage: run-icon-table-check.sh PYTHON ROOT}"
ROOT="${2:?}"
TEMP="$(mktemp -d)"
trap 'rm -rf "${TEMP}"' EXIT

if ! "${PYTHON}" "${ROOT}/tools/gen-icon-table.py" --output-dir "${TEMP}" >/dev/null; then
	echo "  [FAIL] the generator rejected the icon data"
	exit 1
fi

for FILE in systemicon-table.c systemicon-data.c systemicon-data.h; do
	if ! cmp -s "${ROOT}/src/${FILE}" "${TEMP}/${FILE}"; then
		echo "  [FAIL] src/${FILE} is stale; run: python3 tools/gen-icon-table.py"
		diff -u "${ROOT}/src/${FILE}" "${TEMP}/${FILE}" | head -40
		exit 1
	fi
done

echo "  [ok]   the generated table and vector data are current"
exit 0
