#!/bin/sh
./lircd --pidfile var/lircd.pid  --output var/lircd.socket -L var/lircd.log  --nodaemon -Ddebug
