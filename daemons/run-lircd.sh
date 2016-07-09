#!/bin/sh
./lircd --pidfile var/lircd.pid  --output var/lircd.socket -DDebug -L var/lircd.log  --nodaemon -Ddebug
