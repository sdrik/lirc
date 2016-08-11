/**
 * @file lircd_commands
 * Tools to read commands from clients and execute them
 *
 */

#ifndef DAEMONS_LIRCD_COMMANDS_H_
#define DAEMONS_LIRCD_COMMANDS_H_

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>

#include <vector>

#include "lirc_private.h"

#include "lircd_messages.h"
#include "reply_parser.h"
#include "pidfile.h"
#include "fd_list.h"


struct protocol_directive {
	const char* name;
	int (*function)(int fd, const char* message, const char* arguments);
};


/** List of commands and their associated handler. */
extern const struct protocol_directive directives[];


/** Set the default backend fd. */
void commands_set_backend(int fd);


/** Get the default backend fd. */
int commands_get_backend();


/**
 * Mark client as expecting command data from backend, and backend to
 * return data to client socket. Client == 0 implies the local client which
 * only is marked at the backend side.
 */
bool connect_fds(int client_fd, int backend_fd);


/** Dissolve relation created by connect() given any of the two parties. */
bool disconnect_fds(int fd);


/**
 * Sends message to all connected clients unless they are processing a cmd,
 * removes faulty clients. Returns true.
 */
bool broadcast_message(const char* message, int fd);


/**
 * Break input into lines and invoke line_handler(line, fd) for each line.
 * The line_handler func returns true if the socket is functional and can be
 * used.
 *
 * @bug  Recursive reads blocks main loop.
 */

typedef bool(*LineHandler)(const char* line, int fd);
bool get_line(int fd, LineBuffer* lineBuffer, LineHandler line_handler);


/** Init module global scope. */
void commands_init(FdList* list);

#endif  // DAEMONS_LIRCD_COMMANDS_H_
