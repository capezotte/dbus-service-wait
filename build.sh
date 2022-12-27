#!/bin/sh
DBUSFLAGS=$(pkg-config --libs --cflags dbus-1) || exit 1

${CC:-cc} $DBUSFLAGS $CFLAGS $LDFLAGS "$@" -o dbus-service-wait dbus-service-wait.c
