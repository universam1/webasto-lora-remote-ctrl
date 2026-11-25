#!/bin/sh
# Runs all host unit tests (lib/webasto_core, lib/rt, oled page logic, codecs). No hardware needed.
# Usage: tools/run_native_tests.sh [-f test_name ...]
# Without -f every suite test/test_* is run. The filter below keeps PlatformIO from picking up
# build output under test/native_project/.pio as if it were test suites.
set -e
cd "$(dirname "$0")/../test/native_project"
if [ "$#" -eq 0 ]; then
  set -- -f 'test_*'
fi
exec pio test -e native "$@"
