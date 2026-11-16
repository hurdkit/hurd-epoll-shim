#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 7778777
# Run TAP test programs; fail on a non-zero exit, a "not ok" line or a plan
# that does not match the number of results.  QUIET=1 prints summaries only.
status=0
for t in "$@"; do
	out=$("$t" 2>&1)
	rc=$?
	plan=$(printf '%s\n' "$out" | sed -n 's/^1\.\.\([0-9][0-9]*\)$/\1/p' | head -n 1)
	ran=$(printf '%s\n' "$out" | grep -c -E '^(not )?ok [0-9]')
	bad=$(printf '%s\n' "$out" | grep -c '^not ok')
	skips=$(printf '%s\n' "$out" | grep -c '# SKIP')
	if [ -z "$QUIET" ]; then
		printf '%s\n' "$out"
	fi
	if [ "$rc" -ne 0 ] || [ -z "$plan" ] || [ "$plan" != "$ran" ] || [ "$bad" -ne 0 ]; then
		echo "FAIL $t: exit $rc, plan ${plan:-none}, ran $ran, failed $bad"
		if [ -n "$QUIET" ]; then
			printf '%s\n' "$out" | grep -E '^not ok|^#' | sed 's/^/    /'
		fi
		status=1
	else
		echo "PASS $t: $ran tests ($skips skipped)"
	fi
done
exit $status
