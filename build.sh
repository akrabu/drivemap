#!/bin/sh
set -eu
cd "$(dirname "$0")"
exec clang -O2 -Wall -Wextra -Wpedantic -std=c11 -o drivemap drivemap.c
