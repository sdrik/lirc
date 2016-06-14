/******************************************************************
** commands *******************************************************
*******************************************************************
*
* lircd commands library: send commands and receive replies.
*
* Copyright (c) 2015 Alec Leamas
*
* */

/**
 * @file command_parser.cpp
 * This file implements the network command parser.
 */
#ifndef DAEMONS_COMMANDS_H_
#define DAEMONS_COMMANDS_H_

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif


#include "lirc_private.h"




int read_timeout(int fd, char* buf, int len, int timeout_us);

int write_socket(int fd, const char* buf, int len);

int write_socket_len(int fd, const char* buf);


int send_success(int fd, const char* message);

int send_simple_reply(int fd, const char* message, const char* data);

int send_error(int fd, const char* message, const char* format_str, ...);

int send_sighup(int fd);

int get_command(int fd);

#endif  // DAEMONS_COMMANDS_H_
