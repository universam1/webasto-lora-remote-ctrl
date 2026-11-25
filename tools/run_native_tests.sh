#!/bin/sh
# Runs all host unit tests (lib/webasto_core, lib/rt, oled page logic). No hardware needed.
set -e
cd "$(dirname "$0")/../test/native_project"
exec pio test -e native "$@"
