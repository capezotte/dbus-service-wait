#!/bin/sh
set -f
exec ${CC:-cc} ${DBUSFLAGS-$(pkg-config --libs --cflags dbus-1)} $CFLAGS $LDFLAGS "$@" -o dbus-service-wait dbus-service-wait.c
