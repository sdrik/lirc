/**
 * @file lircd_commands
 * Tools to read commands from clients and execute them
 *
 */

#ifndef DAEMONS_BACKEND_COMMANDS_H_
#define DAEMONS_BACKEND_COMMANDS_H_

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

#ifndef max
#define max(a, b) (unsigned int)a > (unsigned int)b ? (a) : (b)
#endif

/* set_transmitters only supports 32 bit int */
#define MAX_TX (CHAR_BIT * sizeof(__u32))

struct protocol_directive {
	const char* name;
	int (*function)(int fd, const char* message, const char* arguments);
};

struct repeat_ctx {
	char** repeat_message;
	int* repeat_fd;
	void (*schedule_repeat_timer)(struct timespec* when);
	unsigned int repeat_max;
};


/** List of commands and their associated handler. */
extern const struct protocol_directive directives[];

/** The lircd input fifo, opened by set_backend_fifo(). */
int get_events_fd();

/** The list of parsed remotes in config file. */
struct ir_remote* get_remotes();

/** Update the list of parsed remotes in config file. */
void set_remotes(struct ir_remote* remotes);


void commands_init(struct repeat_ctx* ctx);

/** Read data from fd and possibly process commands therein. */
int get_command(int fd);


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

#endif  //  DAEMONS_BACKEND_COMMANDS_H_
