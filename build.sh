#!/bin/env bash

set -xe
CFLAGS="-Wall -Wextra -ggdb"

cc $CFLAGS -o server server.c
