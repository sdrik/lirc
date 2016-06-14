// FIXME: Create ctrl, backend and data sockets in same dir as lircd.
/****************************************************************************
** lircd.c *****************************************************************
****************************************************************************
*
* lircd - LIRC dispatcher
*
* Copyright (c) 2015 Alec Leamas
*
*/

/**
 * @file lircd.c
 * This file implements the main dispatcher daemon lircd.
 *
 * The dispatcher works as a broker between connected clients and one or more
 * backends. It has three well-known socket interfaces:
 *
 *   - The lircd interface is what the clients connects to. It has the same
 *     command interface as described in the 0.9.x lircd manpage,
 *   - The backend interface is what the backends connects to. When a backend
 *     connects a registration sequence is initated.
 *   - The control interface is used to send commands to specific backends.
 *     A new tool irtool exposes this as a command line application.
 *
 *  The dispatcher basically does three things:
 *
 *   - Any decoded event from any backend is broadcasted to all clients.
 *   - A command from a client is forwarded to the default backend.
 *   - A command from the control interface is forwarded to the
 *     designated backend (e. g., send-once) or handled by lircd (e. g.,
 *     list-backends).
 *
 *  The default backend is the last registered backend. It can
 *  be inspected and changed through the control interface.
 *
 *  Some former options, notably --connect, --listen and --uinput are
 *  implemented as separate clients or backends.
 *
 *  ------------------
 *
 *  Backends exists in two states: registered/unregistered. They are
 *  created unregistered, and becomes registered after the  GET-ID
 *  and SET-DATA-SOCKET commands from lircd to backend. The command and
 *  data channels have a fixed relation.
 *
 *  When a command is initiated from a client the client and backend becomes
 *  connected. While connected,  lircd will not accept more commands and will
 *  also not broadcast keypress events to the client. Connections are closed
 *  after an END line from the backend or a timeout.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>

#include <vector>
#include <atomic>

#ifdef HAVE_SYSTEMD
#include "systemd/sd-daemon.h"
#endif

#if defined __APPLE__ || defined __FreeBSD__
#include <sys/ioctl.h>
#endif

#include "lirc_private.h"

#ifdef HAVE_INT_GETGROUPLIST_GROUPS
#define lirc_gid int
#else
#define lirc_gid gid_t
#endif

#ifdef DARWIN
#include <mach/mach_time.h>
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC SYSTEM_CLOCK
int clock_gettime(int clk_id, struct timespec *t){
	static mach_timebase_info_data_t timebase = {0};
	if (timebase.numer == 0)
		mach_timebase_info(&timebase);

	double time = static_cast<double>(mach_absolute_time());
	double numer = static_cast<double>(timebase.numer);
	double denom = static_cast<double>(timebase.denom);

	tv.>tv_nsec = (time * numer) / denom;>			// NOLINT
	tv.>tv_sec = (time * numer) / (denom * 1e9);		// NOLINT
	return 0;
}
#endif


/****************************************************************************
** lircd.h *****************************************************************
****************************************************************************
*
*/

#define DEBUG_HELP "Bad debug level: \"%s\"\n\n" \
	"Level could be ERROR, WARNING, NOTICE, INFO, DEBUG, TRACE, TRACE1,\n" \
	" TRACE2 or a number in the range 3..10.\n"

#include "commands.h"
#include "reply_parser.h"
#include "pidfile.h"
#include "lircd_options.h"
#include "fd_list.h"

#ifndef PACKET_SIZE
#define PACKET_SIZE 256
#endif
#define WHITE_SPACE " \t"

static const logchannel_t logchannel = LOG_DISPATCH;
static const int COMMAND_TIMEOUT_TICKS = 20;

/* Forwards referenced in directive definition below. */

static int get_default_backend_cmd(
	int fd, const char* message, const char* arguments);
static int list_backends_cmd(
	int fd, const char* msg, const char* args);
static int set_default_backend_cmd
	(int fd, const char* message, const char* arguments);
static int set_inputlog_cmd(
	int fd, const char* message, const char* arguments);
static int simulate_cmd(
	int fd, const char* message, const char* arguments);
static int send_once_cmd(
	int fd, const char* message, const char* arguments);
static int send_start_cmd(
	int fd, const char* message, const char* arguments);
static int send_stop_cmd(
	int fd, const char* message, const char* arguments);
static int stop_backend_cmd(
	int fd, const char* message, const char* arguments);
static int list_remotes_cmd(
	int fd, const char* message, const char* arguments);
static int list_codes_cmd(
	int fd, const char* message, const char* arguments);
static int set_transmitters_cmd(
	int fd, const char* message, const char* arguments);
static int version_cmd(int fd, const char* message, const char* arguments);

struct protocol_directive {
	const char* name;
	int (*function)(int fd, const char* message, const char* arguments);
};


static const struct protocol_directive directives[] = {
	{ "LIST_BACKENDS",	 list_backends_cmd	    },
	{ "STOP_BACKEND",	 stop_backend_cmd	    },
	{ "SET_DEFAULT_BACKEND", set_default_backend_cmd    },
	{ "GET_DEFAULT_BACKEND", get_default_backend_cmd    },
	{ "SET-INPUTLOG",	 set_inputlog_cmd	    },
	{ "SEND_ONCE",	    	 send_once_cmd		    },
	{ "SEND_START",	    	 send_start_cmd		    },
	{ "SEND_STOP",	    	 send_stop_cmd		    },
	{ "LIST_REMOTES", 	 list_remotes_cmd	    },
	{ "LIST_CODES", 	 list_codes_cmd		    },
	{ "VERSION",		 version_cmd		    },
	{ "SIMULATE",		 simulate_cmd		    },
	{ "SET_TRANSMITTERS",	 set_transmitters_cmd	    },
	{ NULL,			 NULL			    }
};

static const int HEARTBEAT_US = 50000;

typedef void (*SignalHandler)();
std::atomic<SignalHandler> signal_handler(0);

static const struct options_t* options;

static FdList*  fdList(0);

static Pidfile* pidfile(0);

static int default_backend = -1;


/** Parse and format the odd argument format for SIMULATE command. */
class Simvalues {
	private:
		unsigned int scancode;
		unsigned int repeat;
		char keysym[32];
		char remote[64];
		char trash[32];

	public:
		/** Parse a <remote> <keysym> <repeat> <scancode> line, */
		bool parse(const char* input) {
			int r;
			r = sscanf(input, "%64s %32s %d %x",
				   remote, keysym, &repeat, &scancode);
			return r == 4;
		}

		/** Format as requried by SIMULATE. */
		std::string to_string() {
			char buff[255];
			snprintf(buff, sizeof(buff), "%016x %02x %s %s",
				 scancode, repeat, keysym, remote);
			return std::string(buff);
		}
};


int get_command(int fd)
{
	int length;
	char buffer[PACKET_SIZE + 1], backup[PACKET_SIZE + 1];
	char* end;
	int packet_length, i;
	char* directive;

	length = read_timeout(fd, buffer, PACKET_SIZE, 0);
	packet_length = 0;
	while (length > packet_length) {
		buffer[length] = 0;
		end = strchr(buffer, '\n');
		if (end == NULL) {
			log_error("bad send packet: \"%s\"", buffer);
			/* remove clients that behave badly */
			return 0;
		}
		end[0] = 0;
		log_trace("received command: \"%s\"", buffer);
		packet_length = strlen(buffer) + 1;

		strcpy(backup, buffer);
		strcat(backup, "\n");

		/* remove DOS line endings */
		end = strrchr(buffer, '\r');
		if (end && end[1] == 0)
			*end = 0;

		directive = strtok(buffer, WHITE_SPACE);
		if (directive == NULL) {
			if (!send_error(fd, backup, "bad send packet\n"))
				return 0;
			goto skip;
		}
		for (i = 0; directives[i].name != NULL; i++) {
			if (strcasecmp(directive,
				       directives[i].name) == 0) {
				if (!directives[i].function(fd,
							    backup,
							    strtok(NULL, "")))
					return 0;
				goto skip;
			}
		}
		if (!send_error(fd, backup,
				"unknown directive: \"%s\"\n", directive))
			return 0;
skip:
		if (length > packet_length) {
			int new_length;

			memmove(buffer,
				buffer + packet_length,
				length - packet_length + 1);
			if (strchr(buffer, '\n') == NULL) {
				new_length =
					read_timeout(fd, buffer + length - packet_length,
						     PACKET_SIZE - (length - packet_length), 5);
				if (new_length > 0)
					length = length - packet_length + new_length;
				else
					length = new_length;
			} else {
				length -= packet_length;
			}
			packet_length = 0;
		}
	}

	if (length == 0)        /* EOF: connection closed by client */
		return 0;
	return 1;
}


/*
 * Mark client as as expecting command data from backend, and backend to
 * return data to client socket. Client == 0 implies the local client which
 * only is marked at the backend side.
 */
static bool connect_fds(int client_fd, int backend_fd)
{
	log_debug("Connecting client %d to %d", client_fd, backend_fd);
	ItemIterator backend = fdList->find_fd(backend_fd);
	if (backend == fdList-> end())
		return false;
	if (client_fd == 0) {
		backend->connected_to = 0;
		return true;
	}
	ItemIterator client = fdList->find_fd(client_fd);
	backend->connected_to = client_fd;
	if (client == fdList-> end())
		return false;
	client->connected_to = backend_fd;
	client->ticks = COMMAND_TIMEOUT_TICKS;
	return true;
}


/** Dissolve relation created by connect() given any of the two parties. */
static bool disconnect_fds(int fd)
{
	ItemIterator me;
	ItemIterator other;

	log_debug("Disconnecting : %d", fd);
	me = fdList->find_fd(fd);
	if (me == fdList->end())
		return false;
	if (me->connected_to == 0) {
		/** Local lircd client is not in list. */
		me->connected_to = -1;
		return true;
	}
	me->ticks = -1;
	if (me->connected_to == -1)
		return false;
	other = fdList->find_fd(me->connected_to);
	me->connected_to = -1;
	if (other == fdList->end())
		return false;
	other->connected_to = -1;
	other->ticks = -1;
	return true;
}


/**
 * Send message to all connected clients unless they are processing a cmd,
 * remove faulty clients. Returns true.
 */
static bool broadcast_message(const char* message, int fd)
{
	int len = strlen(message);
	ItemIterator it;

	it = fdList->begin();
	while (it != fdList->end()) {
		if (it->kind != FdItem::CLIENT_STREAM) {
			it += 1;
			continue;
		}
		if (it->connected_to != -1) {   // connected in command mode
			it += 1;
			continue;
		}
		log_trace("writing to client %d: %s", it->fd, message);
		if (write_socket(it->fd, message, len) < len)
			it = fdList->remove_client(it->fd);
		else
			it += 1;
	}
	return true;
}


/** SIGTERM/SIGUSR1 helper, called from main loop. Cleans up and exits. */
void dosigterm_sig(int sig)
{
	ItemIterator it;

	signal(SIGALRM, SIG_IGN);
	log_notice("caught signal");

	repeat_remote = NULL;
	if (fdList != 0) {
		for (it = fdList->begin(); it != fdList->end(); it += 1) {
			shutdown(it->fd, SHUT_RDWR);
			close(it->fd);
		}
	}
	pidfile->close();
	lirc_log_close();
	signal(sig, SIG_DFL);
	if (sig == SIGUSR1)
		exit(0);
	raise(sig);
}


void dosigterm() { dosigterm_sig(SIGTERM); }


void dosigusr1() { dosigterm_sig(SIGUSR1); }


/** SIGTERM signal handler: setup  dosigterm_sig() calling in main loop. */
void sigterm(int sig) { signal_handler = dosigterm; }


/** SIGUSR1 signal handler: setup  dosigterm_sig() calling in main loop. */
void sigusr1(int sig) { signal_handler = dosigusr1; }


/** SIGHUP handler: Re-read config file. */
static void dosighup()
{
	ItemIterator it;

	/* reopen logfile first */
	if (lirc_log_reopen() != 0) {
		/* can't print any error messagees */
		dosigterm();
	}

	it = fdList->begin();
	while (it != fdList->end()) {
		if (it->kind != FdItem::CLIENT_STREAM) {
			it += 1;
			continue;
		}
		if (send_sighup(it->fd))
			it += 1;
		else
			it = fdList->remove_client(it->fd);
	}
}


/** SIGHUP signal handler: setup so dosighup() is called  in main loop. */
void sighup(int sig) { signal_handler = dosighup; }


/** Decrement the tick counter on each fd, handle timeouts. */
void dosigalrm()
{
	ItemIterator it;

	for (it = fdList->begin(); it != fdList->end(); it += 1) {
		if (it->kind != FdItem::CLIENT_STREAM &&
		    it->kind != FdItem::CTRL_STREAM) {
			continue;
		}
		if (it->ticks <= 0)
			continue;
		log_trace("dosigalrm: ticks on %d (%d)", it->fd, it ->ticks);
		it->ticks -= 1;
		if (it->ticks > 0)
			continue;
		log_debug("dosigalrm: timeout on %d", it->fd);
		send_error(it->fd, it->expected.c_str(), "TIMEOUT");
		disconnect_fds(it->fd);
		log_debug("Timeout: disconnecting %d", it->fd);
		it->ticks = -1;
	}
}


/** SIGALRM signal handler: setup so dosigalrm() is called  in main loop. */
void sigalrm(int sig) { signal_handler = dosigalrm; }


/** fdLlist::find argument: locates proper backend) */
bool find_backend_by_id(const FdItem& item, const char* what)
{
	return strcmp(what, item.id.c_str()) == 0;
}


/** Return vector of {firstword, remainder} in nl-terminated str. */
std::vector<std::string> split_once(const char* str)
{
	std::vector<std::string> result;
	if (str == NULL || *str =='\0')
		return result;
	char* buff = reinterpret_cast<char*>(alloca(strlen(str) + 2));
	strncpy(buff, str, strlen(str));

	char* token = strtok(buff, " \t\n");
	if (token == NULL || *token == '\0')
		return result;
	result.push_back(token);

	token = strtok(NULL, "\n");
	if (token == NULL || *token == '\0')
		return result;
	result.push_back(token);
	return result;
}


/** Handle VERSION command: return version. */
static int version_cmd(int fd, const char* message, const char* arguments)
{
	char buffer[PACKET_SIZE + 1];

	snprintf(buffer, sizeof(buffer), "1\n%s\n", VERSION);
	return send_simple_reply(fd, message, buffer);
}


/** Create a faked decoded butten press event: remote code count raw. */
static int simulate_cmd(int fd, const char* msg, const char* args)
{
	std::vector<std::string> commands = split_once(msg);
	std::vector<std::string> arguments = split_once(args);

	if (arguments.size() != 2)  {
		send_error(fd, msg, "Bad arguments: %s", args);
		return 0;
	}
	ItemIterator backend = fdList->find(arguments[0].c_str(),
					    find_backend_by_id);
	if (backend == fdList->end()) {
		send_error(fd, msg, "No such backend: %s", backend);
		return 0;
	}
	Simvalues simvalues;
	if (!simvalues.parse(arguments[1].c_str())) {
		send_error(fd, msg, "Cannot parse input: %s", arguments[1]);
		return 0;
	}
	backend->expected = commands[0];
	connect_fds(fd, backend->fd);
	ItemIterator client = fdList->find_fd(fd);
	if (client == fdList->end()) {
		send_error(fd, msg, "Internal error: send_cmd: bad fd");
		return 0;
	}
	commands[0] += " " + simvalues.to_string() + "\n";
	log_debug("Backend %s command: %s", backend->id, commands[0].c_str());
	write_socket(backend->fd, commands[0].c_str(), commands[0].size());
	return 1;
}


/** GET_DEFAULT_BACKEND command: return default backend; possible errors. */
static int get_default_backend_cmd(int fd, const char* msg, const char* args)
{
	log_debug("Sending default backend.");
	if (default_backend == -1) {
		send_error(fd, "GET_DEFAULT_BACKEND", "None");
		return 1;
	}
	ItemIterator it = fdList->find_fd(default_backend);
	if (it == fdList->end()) {
		send_error(fd, "GET_DEFAULT_BACKEND", "Internal error");
		log_warn("Cannot lookup default backend.");
		return 1;
	}
	send_simple_reply(fd, "GET_DEFAULT_BACKEND", (it->id + "\n").c_str());
	return 1;
}


/** LIST_BACKENDS command: Always succeeds, but might return no values. */
static int list_backends_cmd(int fd, const char* msg, const char* args)
{
	ItemIterator it;
	std::string backends("");

	for (it = fdList->begin(); it != fdList->end(); it += 1) {
		if (it->kind != FdItem::BACKEND_CMD)
			continue;
		backends += it->id + "\n";
	}
	send_simple_reply(fd, msg, backends.c_str());
	return 1;
}


/** LIST_REMOTES command: dispatch to correct backend. */
static int list_remotes_cmd(int fd, const char* msg, const char* args) {
	std::vector<std::string> commands = split_once(msg);
	std::vector<std::string> arguments = split_once(args);

	if (arguments.size() != 1)  {
		send_error(fd, msg, "Bad arguments: %s", args);
		return 0;
	}
	ItemIterator backend = fdList->find(arguments[0].c_str(),
					    find_backend_by_id);
	if (backend == fdList->end()) {
		send_error(fd, msg, "No such backend: %s", backend);
		return 0;
	}
	backend->expected = commands[0];
	connect_fds(fd, backend->fd);
	ItemIterator client = fdList->find_fd(fd);
	if (client == fdList->end()) {
		send_error(fd, msg, "Internal error: send_cmd: bad fd");
		return 0;
	}
	commands[0] += "\n";
	log_debug("Backend %s command: %s", backend->id, commands[0].c_str());
	write_socket(backend->fd, commands[0].c_str(), commands[0].size());
	return 1;
}


/** LIST_CODES command: dispatch to correct backend. */
static int list_codes_cmd(int fd, const char* msg, const char* args) {
	std::vector<std::string> commands = split_once(msg);
	std::vector<std::string> arguments = split_once(args);

	if (arguments.size() != 2)  {
		send_error(fd, msg, "Bad arguments: %s", args);
		return 0;
	}
	ItemIterator backend = fdList->find(arguments[0].c_str(),
					    find_backend_by_id);
	if (backend == fdList->end()) {
		send_error(fd, msg, "No such backend: %s", backend);
		return 0;
	}
	backend->expected = commands[0];
	connect_fds(fd, backend->fd);
	ItemIterator client = fdList->find_fd(fd);
	if (client == fdList->end()) {
		send_error(fd, msg, "Internal error: send_cmd: bad fd");
		return 0;
	}
	commands[0] += " "  + arguments[1] + "\n";
	log_debug("Backend %s command: %s", backend->id, commands[0].c_str());
	write_socket(backend->fd, commands[0].c_str(), commands[0].size());
	return 1;
}


/** SET_DEFAULT_BACKEND command. */
static int set_default_backend_cmd(int fd, const char* msg, const char* args)
{
	ItemIterator it;

	std::string new_backend(args);
	new_backend.erase(new_backend.find_last_not_of(" \n\r\t") + 1);

	for (it = fdList->begin(); it != fdList->end(); it += 1) {
		if (it->id == new_backend)
			break;
	}
	if (it == fdList->end()) {
		log_warn("set-default-backend: No such backend: %s", args);
		send_error(fd, msg, "No such backend: %s\n", args);
		return 0;
	}
	default_backend = it->fd;
	send_success(fd, msg);
	return 1;
}


/** SET-INPUTLOG command: enable/disable logging if decoded data. */
static int set_inputlog_cmd(int fd, const char* message, const char* arguments)
{
	char buff[128];
	FILE* f;
	int r;

	r = sscanf(arguments, "%128s", buff);
	if (r != 1) {
		return send_error(fd, message,
				  "Illegal argument (protocol error): %s",
				  arguments);
	}
	if (strcasecmp(buff, "null") == 0) {
		rec_buffer_set_logfile(NULL);
		return send_success(fd, message);
	}
	f = fopen(buff, "w");
	if (f == NULL) {
		log_warn("Cannot open input logfile: %s", buff);
		return send_error(fd, message,
				  "Cannot open input logfile: %s (errno: %d)",
				  buff, errno);
	}
	rec_buffer_set_logfile(f);
	return send_success(fd, message);
}


/** STOP_BACKEND command */
static int stop_backend_cmd(int fd, const char* msg, const char* argstring)
{
	std::vector<std::string> commands = split_once(msg);
	std::vector<std::string> arguments = split_once(argstring);
	ItemIterator backend = fdList->find(arguments[0].c_str(),
					    find_backend_by_id);
	if (backend == fdList->end()) {
		send_error(fd, msg, "No such backend: %s", backend);
		return 0;
	}
	if (arguments.size() != 1)  {
		send_error(fd, msg, "Bad arguments: %s", argstring);
		return 0;
	}
	backend->expected = commands[0];
	connect_fds(fd, backend->fd);
	ItemIterator client = fdList->find_fd(fd);
	if (client == fdList->end()) {
		send_error(fd, msg,
			   "Internal error: stop_backend_cmd: bad fd");
		return 0;
	}
	commands[0] += "\n";
	log_debug("Backend %s command: %s", backend->id, commands[0].c_str());
	write_socket(backend->fd, commands[0].c_str(), commands[0].size());
	return 1;
}


/** SEND_ONCE, SEND_START, SEND_STOP commands: dispatch to backend. */
static int send_cmd(int fd, const char* message, const char* argument)
{
	std::vector<std::string> commands = split_once(message);
	std::vector<std::string> arguments = split_once(argument);

	ItemIterator backend = fdList->find(arguments[0].c_str(),
					    find_backend_by_id);
	if (backend == fdList->end()) {
		send_error(fd, message, "No such backend: %s", backend);
		return 0;
	}
	if (arguments.size() != 2)  {
		send_error(fd, message, "Bad arguments: %s", argument);
		return 0;
	}
	backend->expected = commands[0];
	connect_fds(fd, backend->fd);
	ItemIterator client = fdList->find_fd(fd);
	if (client == fdList->end()) {
		send_error(fd, message, "Internal error: send_cmd: bad fd");
		return 0;
	}
	commands[0] += " "  + arguments[1] + "\n";
	log_debug("Backend %s command: %s", backend->id, commands[0].c_str());
	write_socket_len(backend->fd, commands[0].c_str());
	return 1;
}


static int send_once_cmd(int fd, const char* message, const char* arguments)
{
	return send_cmd(fd, message, arguments);
}


static int send_start_cmd(int fd, const char* message, const char* arguments)
{
	return send_cmd(fd, message, arguments);
}


static int send_stop_cmd(int fd, const char* message, const char* arguments)
{
	return send_cmd(fd, message, arguments);
}


/** SET_TRANSMITTERS command: dispatch to correct backend. */
static int set_transmitters_cmd(int fd, const char* msg, const char* args) {
	std::vector<std::string> commands = split_once(msg);
	std::vector<std::string> arguments = split_once(args);

	if (arguments.size() != 2)  {
		send_error(fd, msg, "Bad arguments: %s", args);
		return 0;
	}
	ItemIterator backend = fdList->find(arguments[0].c_str(),
					    find_backend_by_id);
	if (backend == fdList->end()) {
		send_error(fd, msg, "No such backend: %s", backend);
		return 0;
	}
	backend->expected = commands[0];
	connect_fds(fd, backend->fd);
	ItemIterator client = fdList->find_fd(fd);
	if (client == fdList->end()) {
		send_error(fd, msg,
			   "Internal error: set_transmitters: bad fd");
		return 0;
	}
	commands[0] += " "  + arguments[1] + "\n";
	log_debug("Backend %s command: %s", backend->id, commands[0].c_str());
	write_socket(backend->fd, commands[0].c_str(), commands[0].size());
	return 1;
}


/** Set socket linger opts to that close(sock) don't waits for completion. */
static void nolinger(int sock)
{
	static const struct linger linger = { 0, 0 };
	const int lsize = sizeof(struct linger);

	setsockopt(sock, SOL_SOCKET, SO_LINGER,
		   reinterpret_cast<const void*>(&linger), lsize);
}


/**  Create cmd - data backend peer relation, quits silently on errors. */
static void connect_peers(int client_fd, int backend_fd)
{
	ItemIterator client;
	ItemIterator backend;

	backend = fdList->find_fd(backend_fd);
	if (backend == fdList-> end())
		return;
	backend->peer = client_fd;
	if (client_fd == 0)
		return;
	client = fdList->find_fd(client_fd);
	if (client == fdList-> end())
		return;
	client->peer = backend_fd;
}


/** Return data socket path for given fd */
static void get_backend_data_path(int fd, std::string* path)
{
	char buff[128];

	snprintf(buff, sizeof(buff), "%s-data-%d",
		 options->client_socket_path, fd);
	std::string s(buff);
	*path = s;
}


static bool find_backend_by_type(const FdItem& item, int what)
{
	return item.kind == FdItem::BACKEND_CMD;
}


/** Find a random, new default client if available. */
static void find_new_default_backend()
{
	ItemIterator item = fdList->find(0, find_backend_by_type);
	if (item == fdList->end())
		default_backend = -1;
	else
		default_backend = item->fd;
	log_debug("New default backend: %d", default_backend);
}


/** Remove client with given fd from list. */
void remove_client(int fd)
{
	if (fdList->remove_client(fd) == fdList->end()) {
		log_notice("internal error in remove_client: no such fd");
		return;
	}
	shutdown(fd, 2);
	close(fd);
	log_info("removed client");
}


/** Accept socket connnection and invoke add_func() with new fd as argument. */
void add_client(int sock, void (*add_func)(int fd))
{
	int fd;
	socklen_t clilen;
	struct sockaddr client_addr;
	int flags;

	clilen = sizeof(client_addr);
	fd = accept(sock, (struct sockaddr*)&client_addr, &clilen);
	if (fd == -1) {
		log_perror_err("accept() failed for new client");
		dosigterm();
	}
	nolinger(fd);
	flags = fcntl(fd, F_GETFL, 0);
	if (flags != -1)
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	if (client_addr.sa_family != AF_UNIX) {
		log_warn("Non-Unix soocket connection: what?");
		return;
	}
	log_trace("Adding new client: %d", fd);
	add_func(fd);
}


/**
 *  Add the command channel to fdList. Create the data fifo with a name
 *  which can be retreived when the backend returns with reply
 *  for the GET-ID command.
 */
void add_backend(int sock)
{
	const char* const GET_ID_CMD = "GET-ID\n";
	int data_fd;
	int cmd_fd;
	int flags;
	struct sockaddr client_addr;
	FdItem item;
	std::string path;

	socklen_t clilen = sizeof(client_addr);

	cmd_fd = accept(sock, (struct sockaddr*)&client_addr, &clilen);
	if (cmd_fd == -1) {
		log_perror_err("accept() failed for new backend");
		dosigterm();
	}
	nolinger(cmd_fd);
	flags = fcntl(cmd_fd, F_GETFL, 0);
	if (flags != -1)
		fcntl(cmd_fd, F_SETFL, flags | O_NONBLOCK);
	if (client_addr.sa_family != AF_UNIX) {
		log_warn("Non-Unix soocket connection: WTF?");
		return;
	}
	get_backend_data_path(cmd_fd, &path);

	if (access(path.c_str(), F_OK) == 0)
		unlink(path.c_str());

	if (mkfifo(path.c_str(), 0666) == -1) {
		log_perror_err("Cannot setup backend fifo %s", path.c_str());
		return;
	}
	data_fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
	if (data_fd == -1) {
		log_perror_err("Cannot open backend fifo");
		return;
	}
	log_debug("Waiting for event input on %s", path.c_str());
	fdList->add_backend(cmd_fd, data_fd);
	connect_peers(cmd_fd, data_fd);
	ItemIterator it = fdList->find_fd(cmd_fd);
	it->connected_to = 0;
	write_socket(cmd_fd, GET_ID_CMD, sizeof(GET_ID_CMD));
}


/** Setup a local, network listening socket. */
int setup_socket(const char* path, int permissions = 0666 )
{
	int fd;
	struct sockaddr_un serv_addr;
	struct stat statbuf;
	int r;
	bool new_socket = true;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1) {
		log_perror_err("Could not create socket");
		return -1;
	}
	/*
	 * Get owner, permissions, etc.
	 * so new socket can be the same since we
	 * have to delete the old socket.
	 */
	r = stat(path, &statbuf);
	if (r == -1 && errno != ENOENT) {
		perrorf("Could not get file information for %s\n", path);
		return -1;
	}
	if (r != -1) {
		new_socket = false;
		r = unlink(path);
		if (r == -1) {
			perrorf("Could not delete %s", path);
			close(fd);
			return -1;
		}
	}
	serv_addr.sun_family = AF_UNIX;
	strcpy(serv_addr.sun_path, path);
	r = bind(fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
	if (r == -1) {
		perrorf("Could not assign address to socket%s", path);
		close(fd);
		return -1;
	}
	if (new_socket) {
		chmod(path, permissions);
	} else if (chmod(path, statbuf.st_mode) == -1
		   || chown(path, statbuf.st_uid, statbuf.st_gid) == -1) {
		perrorf("Could not set file permissions on %s", path);
		close(fd);
		return -1;
	}
	listen(fd, 3);
	return fd;
}


/** Handle reply from backend after issuing GET-ID command. */
void handle_get_id_reply(int fd)
{
	pid_t pid;
	char name[32];
	char where[64];
	std::string path;

	ItemIterator it = fdList->find_fd(fd);
	std::string reply = it->replyParser->get_data();
	int r = sscanf(reply.c_str(), "%6d %32s %64s", &pid, name, where);
	if (r != 3) {
		log_error("Cannot register backend.");  // FIXME
		log_debug("Command: %s", reply.c_str());
		return;
	}
	it->id = std::string(name) + "@" + std::string(where);

	get_backend_data_path(fd, &path);
	std::string cmd("SET-DATA-SOCKET ");
	cmd +=  path + "\n";
	write_socket(fd, cmd.c_str(), cmd.size());
}


/** Handle reply from backend after issuing SET-DATA-SOCKET command. */
void handle_data_socket_reply(int fd)
{
	ItemIterator it = fdList->find_fd(fd);
	if (it == fdList->end()) {
		log_warn("handle_data_socket: Cannot lookup fd.");
		return;
	}
	if (it->replyParser->get_success()) {
		log_debug("Final backend registration on %d", fd);
		default_backend = fd;
	} else {
		log_error("Backend data channel setup error: %s",
			   it->replyParser->get_last_line());
	}
	disconnect_fds(fd);
}


/** Handle a backend reply to a command from lircd. */
bool handle_local_reply(const char* message, int fd)
{
	std::string reply;

	ItemIterator backend = fdList->find_fd(fd);
	backend->replyParser->feed(message);
	if (backend->replyParser->is_completed())
	{
		if (backend->replyParser->get_result() == ReplyParser::OK) {
			std::string cmd = backend->replyParser->get_command();
			if (cmd == "GET-ID") {
				handle_get_id_reply(fd);
			} else if (cmd == "SET-DATA-SOCKET") {
				handle_data_socket_reply(fd);
			} else {
				log_warn("Unknown backend reply: %s", cmd);
			}
		} else {
			log_error("Cannot handle backend reply: %s",
				  backend->replyParser->get_last_line());
		}
		backend->replyParser->reset();
	}
	return true;
}


/**
 * Replies from backend, routed to the connected_to socket in the backend's
 * FdItem. Disconnect when finding '^END' Disconnect when finding '^END'
 */
bool handle_backend_line(const char* line, int fd)
{
	ItemIterator it;
	char buffer[PACKET_SIZE + 1];
	int r;

	it = fdList->find_fd(fd);
	if (it == fdList->end())
		return false;
	if (it->connected_to < 0) {
		log_error("Unexpected reply from backend: %s", line);
		r = read(fd, buffer, sizeof(buffer));
		if (r < 0)
			log_perror_err("Disconnected backend?!")
		else
			log_debug("Discarding input: %d", r);
		return false;
	}
	if (it->connected_to == 0) {
		return handle_local_reply(line, fd);
	}
	write_socket(it->connected_to, line, strlen(line));
	if (strncmp("END", line, 3) == 0)
		disconnect_fds(fd);
	return true;
}


/**
 * Client lines input are commands to the default backend. Replies from
 * backend are routed using fditem.connected_to
 */
bool handle_client_line(const char* line, int fd)
{
	char buff[PACKET_SIZE + 1];
	const char* cmdptr;

	strncpy(buff, line, sizeof(buff) - 1);
	cmdptr = strtok(buff, " \t\n\r");
	if (cmdptr == NULL) {
		log_notice("Empty client line.");
		return false;
	}
	ItemIterator client = fdList->find_fd(fd);
	if (client == fdList->end()) {
		log_warn("handle_client_line: Cannot lookup fd.");
		return false;
	}
	ItemIterator backend = fdList->find_fd(default_backend);
	if (backend != fdList->end()) {
		backend->replyParser->reset();
		backend->expected = std::string(cmdptr);
		connect_fds(fd, default_backend);
		write_socket(default_backend, line, strlen(line));
	} else {
		log_notice("No backend available, fd: %d", fd);
		send_error(fd, cmdptr,
			   "Backend unavailable, current: %d",
			   default_backend);
	}
	return true;
}


/**
 * Control commands, processed by lircd or forwarded to designated
 * backend.
 */
bool handle_ctrl_cmd(const char* line, int fd)
{
	char buff[PACKET_SIZE + 1];
	const char* directive;
	const char* const DELIM = " \t\n\r";
	ItemIterator item = fdList->end() + 1;

	strncpy(buff, line, sizeof(buff) - 1);
	directive = strtok(buff, DELIM);
	if (directive == NULL) {
		log_notice("Empty line from client");
		return true;
	}
	int ix;
	for (ix = 0; directives[ix].name != NULL; ix++) {
		if (strcasecmp(directive, directives[ix].name) == 0) {
			item = fdList->find_fd(fd);
			break;
		}
	}
	if (item == fdList->end() + 1) {
		log_notice("Unknown command: %s", directive);
		send_error(fd, directive, "Unknown command: %s", directive);
		return true;
	}
	if (item == fdList->end()) {
		log_warn("Internal error: cannot lookup fd");
		send_error(fd, directive, "Internal error: bad fd");
		return true;
	}
	item->expected = directive;
	directives[ix].function(fd, line, strtok(NULL, ""));
	return true;
}


/**
 * Break input into lines annd invoke line_handler(line, fd) for each line.
 * The line_handler func returns true if the socket is functional and can be
 * used.
 *
 * @bug  Recursive reads blocks main loop.
 */
static int get_line(int fd, bool (*line_handler)(const char* line, int fd))
{
	int length;
	char buffer[PACKET_SIZE + 1];
	char* end;
	int packet_length;
	std::string s;
	size_t pos;

	length = read_timeout(fd, buffer, PACKET_SIZE, 5);
	packet_length = 0;
	while (length > packet_length) {
		buffer[length] = 0;
		end = strchr(buffer, '\n');
		if (end == NULL) {
			log_error("bad send packet: \"%s\"", buffer);
			/* remove clients that behave badly */
			return 0;
		}
		s = std::string(buffer, end - buffer + 1);
		log_trace("Received input on %d: '%s'", fd, s.c_str());
		packet_length = s.size();

		/* remove DOS line endings */
		pos = s.rfind("\r");
		if (pos == s.size() - 1)
			s = s.substr(0, s.size() - 1);

		if (!line_handler(s.c_str(), fd))
			return 0;

		if (length > packet_length) {
			int new_length;

			memmove(buffer, buffer + packet_length, length - packet_length + 1);
			if (strchr(buffer, '\n') == NULL) {
				new_length =
					read_timeout(fd, buffer + length - packet_length,
						     PACKET_SIZE - (length - packet_length), 5);
				if (new_length > 0)
					length = length - packet_length + new_length;
				else
					length = new_length;
			} else {
				length -= packet_length;
			}
			packet_length = 0;
		}
	}
	return length != 0;     /* length == 0 -> EOF: connection closed by client */
}


void fdlist_add_client(int fd) { fdList->add_client(fd); }


void fdlist_add_ctrl_client(int fd) { fdList->add_ctrl_client(fd); }


void remove_and_log(int fd, const char* why)
{
	log_debug("Removing fd (%s): %d", why, fd);
	fdList->remove_client(fd);    // FIXME: Make remove generic.
}


/** Invoke proper action for a socket with pending data. */
void process_item_input(FdItem item)
{
	switch (item.kind) {
	case FdItem::UNDEFINED:
		log_warn("Strange client state: (%d)", item.fd);
		break;
	case FdItem::CLIENT:
		log_debug("Registering client");
        	add_client(fdList->client_socket(), fdlist_add_client);
		break;
	case FdItem::BACKEND:
		log_debug("Registering backend");
        	add_backend(fdList->backend_socket());
		break;
	case FdItem::CTRL:
		log_debug("Registering control client");
        	add_client(fdList->ctrl_socket(), fdlist_add_ctrl_client);
		break;
	case FdItem::BACKEND_DATA:
		if (!get_line(item.fd, broadcast_message)) {
			remove_and_log(item.fd,
				       "backend_data: get_line() fails");
			find_new_default_backend();
		}
		break;
	case FdItem::BACKEND_CMD:
		if (!get_line(item.fd, handle_backend_line)) {
			remove_and_log(item.fd,
				       "backend_cmd: get_line() fails");
			find_new_default_backend();
		}
		break;
	case FdItem::CLIENT_STREAM:
		if (!get_line(item.fd, handle_client_line))
			remove_and_log(item.fd, "client: get_line() fails");
		break;
	case FdItem::CTRL_STREAM:
		if (!get_line(item.fd, handle_ctrl_cmd))
			remove_and_log(item.fd, "control: get_line() fails");
		break;
	}
}


/** Main loop: do poll(), handle signals and sockets with pending data. */
static int main_loop(const struct options_t* options, unsigned long maxusec)
{
	int r;
	std::vector<struct pollfd> pollfds;
	std::vector<FdItem> items;

	log_notice("lircd ready, using %s", options->client_socket_path);
	while (1) {
		fdList->get_pollfds(&items, &pollfds);
		do {
			if (signal_handler != 0) {
				signal_handler();
				signal_handler = 0;
			}
			r = poll(&pollfds[0], pollfds.size(), HEARTBEAT_US/1000);
			if (r == -1 && errno != EINTR) {
				log_perror_err("poll()() failed");
				raise(SIGTERM);
				continue;
			}
		} while (r == -1 && errno == EINTR);
		if (r == 0)
			continue;
		for (size_t i = 0; i < pollfds.size(); i += 1) {
			if ((pollfds[i].revents & POLLERR) != 0)
				remove_and_log(items[i].fd, "POLLERR");
			if ((pollfds[i].revents & POLLNVAL) != 0)
				remove_and_log(items[i].fd, "POLLNVAL");
			if ((pollfds[i].revents & POLLIN) != 0)
				process_item_input(items[i]);
			if ((pollfds[i].revents & POLLHUP) != 0)
				remove_and_log(items[i].fd, "POLLHUP");
		}
	}
}


/** Daemonize: close stdin/stdout, fork a new process. */
void daemonize(void)
{
	if (daemon(0, 0) == -1) {
		log_perror_err("daemon() failed");
		dosigterm();
	}
	umask(0);
	pidfile->update(getpid());
}


/** Start the heartbeat SIGALRM signalling each HEARTBEAT_US. */
void start_heartbeat()
{
	struct itimerval itimer;

	itimer.it_interval.tv_sec = 0;
	itimer.it_interval.tv_usec = HEARTBEAT_US;
	itimer.it_value.tv_sec = 0;
	itimer.it_value.tv_usec = HEARTBEAT_US;
	setitimer(ITIMER_REAL, &itimer, NULL);
}


/** Creates global pidfile and obtain the lock on it. Exits on errors */
void create_pidfile()
{
	Pidfile::lock_result result;

	/* create pid lockfile in /var/run */
	pidfile = new Pidfile(options->pidfile_path);
        result = pidfile->lock();
        switch (result) {
	case Pidfile::OK:
		break;
	case Pidfile::CANT_CREATE:
		perrorf("Can't open or create %s", options->pidfile_path);
		exit(EXIT_FAILURE);
	case Pidfile::LOCKED_BY_OTHER:
		fprintf(stderr,
			"lircd: There seems to already be a lircd process with pid %d\n",
			pidfile->other_pid);
		fprintf(stderr,
			"lircd: Otherwise delete stale lockfile %s\n",
			options->pidfile_path);
		exit(EXIT_FAILURE);
	case Pidfile::CANT_PARSE:
		fprintf(stderr, "lircd: Invalid pidfile %s encountered\n",
			options->pidfile_path);
		exit(EXIT_FAILURE);
	}
}


/** Start server: Setup the three well-known sockets. */
void start_server(const struct options_t* options)
{
	int client_sock_fd = -1;
	int ctrl_sock_fd = -1;
	int backend_sock_fd = -1;

#ifdef HAVE_SYSTEMD
	int n = sd_listen_fds(0);

	if (n > 1) {
		fprintf(stderr, "Too many file descriptors received.\n");
		pidfile->close();
		exit(EXIT_FAILURE);
	} else if (n == 1) {
		client_sock_fd = SD_LISTEN_FDS_START + 0;
	}
#endif
	if (client_sock_fd == -1) {
		client_sock_fd =
			setup_socket(options->client_socket_path,
			             options->client_socket_permissions);
		if (client_sock_fd == -1) {
			perrorf("Could not setup socket %s, path");
			pidfile->close();
			exit(EXIT_FAILURE);
		}
	}
	backend_sock_fd = setup_socket(options->backend_socket_path, 0666);
	if (backend_sock_fd == -1) {
		perrorf("Could not setup socket %s",
			options->backend_socket_path);
		pidfile->close();
		shutdown(client_sock_fd, SHUT_RDWR);
		exit(EXIT_FAILURE);
	}
	ctrl_sock_fd = setup_socket(options->ctrl_socket_path, 0666);
	if (ctrl_sock_fd == -1) {
		perrorf("Could not setup socket %s",
			options->ctrl_socket_path);
		shutdown(client_sock_fd, SHUT_RDWR);
		shutdown(backend_sock_fd, SHUT_RDWR);
		pidfile->close();
		exit(EXIT_FAILURE);
	}
	fdList = new FdList(client_sock_fd, backend_sock_fd, ctrl_sock_fd);
	log_trace("started server sockets %s", options->client_socket_path);
}


void setup_signal_handlers()
{
	struct sigaction act;

	signal(SIGPIPE, SIG_IGN);

	act.sa_handler = sigterm;
	sigfillset(&act.sa_mask);
	act.sa_flags = SA_RESTART;
	sigaction(SIGTERM, &act, NULL);
	sigaction(SIGINT, &act, NULL);

	act.sa_handler = sigusr1;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_RESTART;
	sigaction(SIGUSR1, &act, NULL);

	act.sa_handler = sigalrm;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_RESTART;
	sigaction(SIGALRM, &act, NULL);

	act.sa_handler = sighup;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_RESTART;
	sigaction(SIGHUP, &act, NULL);
}


int main(int argc, char** argv)
{
	options = get_options(argc, argv);
	if (options->logfile != NULL)
		lirc_log_set_file(options->logfile);
	lirc_log_open("lircd", options->nodaemon, options->loglevel);

	create_pidfile();
	start_server(options);

	setup_signal_handlers();

	/* ready to accept connections */
	if (!options->nodaemon)
		daemonize();

	start_heartbeat();

	main_loop(options, 0);

	/* Not reached */
	return EXIT_SUCCESS;
};
