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
#ifndef DAEMONS_LIRCD_MESSAGES_H_
#define DAEMONS_LIRCD_MESSAGES_H_

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif


#include "lirc_private.h"
#include <string>
#include <vector>
//#include "fd_list.h"

/**
 * Try to read len bytes from fd  into buffer. Returns -1 on errors, 0 on
 * timeout and otherwise number of read bytes. timeout_us < 0 implies a
 * blocking read.
 */
int read_timeout(int fd, char* buf, int len, int timeout_us);


/** Blocking write of all len bytes in buffer into fd. */
int write_socket(int fd, const char* buf, int len);


/** Blocking write of strlen(buf) bytes in buf  into fd. */
int write_socket_len(int fd, const char* buf);


/** Send a SUCCESS protocol package without any data. */
int send_success(int fd, const char* message);


/** Send a SUCCESS protocol package with nl-terminated lines as data.*/
int send_success(int fd, const char* message, const char* data);


/** Send a ERROR protocol message with an error message as data. */
int send_error(int fd, const char* message, const char* format_str, ...);


/** Send a SIGHUP protocol message to file descriptor. */
int send_sighup(int fd);


/**
 * Broadcast a presumably decoded event message to a list of client fds,
 * return list of unwritable client fds.
 */
std::vector<int> broadcast_message(const char* message,
				   const std::vector<int>& fds);

/** Return vector of {firstword, remainder} in nl-terminated str. */
std::vector<std::string> split_once(const char* str);

#endif  // DAEMONS_LIRCD_MESSAGES_H_
