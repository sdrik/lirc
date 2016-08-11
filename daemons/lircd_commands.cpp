/**
 * @file lircd_commands
 * Reads commands from clients end executes them
 *
 * @copyright (c) 2016 Alec leamas
 *
 */

#include <errno.h>
#include <string.h>
#include <sys/types.h>

#include <vector>

#include "lirc_private.h"

#include "lircd_messages.h"
#include "fd_list.h"

#include "lircd_commands.h"
#define WHITE_SPACE " \t"

static const logchannel_t logchannel = LOG_DISPATCH;

static const int COMMAND_TIMEOUT_TICKS = 20;    /**< Command timeout (ticks).*/

/** Forward */
extern const struct protocol_directive directives[];

/** Book-keeping data for all active sockets. */
static FdList*  fdList(0);

/** Current default client file descriptor. */
static int default_backend = -1;


/** Parse and format the odd argument format for SIMULATE command. */
class Simvalues {
	private:
		unsigned int scancode;
		unsigned int repeat;
		char keysym[32];
		char remote[64];

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


/** Setup global scope. */
void commands_init(FdList* fdListArg)
{
	fdList = fdListArg;
	default_backend = -1;
}


int commands_get_backend() { return default_backend; }


void commands_set_backend(int fd) { default_backend = fd; }


/**
 * Mark client as as expecting command data from backend, and backend to
 * return data to client socket. Client == 0 implies the local client which
 * only is marked at the backend side.
 */
bool connect_fds(int client_fd, int backend_fd)
{
	log_debug("Connecting client %d to %d", client_fd, backend_fd);
	FdItemIterator backend = fdList->find_fd(backend_fd);
	if (backend == fdList-> end())
		return false;
	if (client_fd == 0) {
		backend->connected_to = 0;
		return true;
	}
	FdItemIterator client = fdList->find_fd(client_fd);
	backend->connected_to = client_fd;
	if (client == fdList-> end())
		return false;
	client->connected_to = backend_fd;
	client->ticks = COMMAND_TIMEOUT_TICKS;
	return true;
}


/** Dissolve relation created by connect() given any of the two parties. */
bool disconnect_fds(int fd)
{
	FdItemIterator me;
	FdItemIterator other;

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
 * Sends message to all connected clients unless they are processing a cmd,
 * removes faulty clients. Returns true.
 */
bool broadcast_message(const char* message, int fd)
{
	std::vector<int> fds;

	FdItemIterator it = fdList->begin();
	while (it != fdList->end()) {
		if (it->kind != FdItem::CLIENT_STREAM) {
			it += 1;
			continue;
		}
		if (it->connected_to != -1) {   // connected in command mode
			it += 1;
			continue;
		}
		fds.push_back(it->fd);
		it += 1;
	}

	std::vector<int> bad_fds = broadcast_message(message, fds);
	for (auto it = bad_fds.begin(); it != bad_fds.end(); it++)
		fdList->remove_client(*it);
	return true;
}


/**
 *  Check argument count and return backend reflecting first arg after
 *  connecting fd.
 */
static FdItemIterator setup_backend_cmd(int fd,
				      const std::vector<std::string>& args,
				      const char* msg,
				      size_t argcount)
{
	if (argcount == 0 && args.size() < 1)  {
		send_error(fd, msg, "Missing backend: \"%s\"", args);
		return fdList->end();
	}
	if (argcount != 0 && args.size() != argcount)  {
		send_error(fd, msg, "Bad arguments: %s", args);
		return fdList->end();
	}
	auto find_func =
		[](const FdItem& item, const char* what) ->
			bool {return strcmp(what, item.id.c_str()) == 0; };
	FdItemIterator backend = fdList->find(args[0].c_str(), find_func);
	if (backend == fdList->end()) {
		send_error(fd, msg, "No such backend: %s", backend);
		return fdList->end();
	}
	connect_fds(fd, backend->fd);
	return backend;
}


/** Handle VERSION command: return version. */
static int version_cmd(int fd, const char* message, const char* arguments)
{
	char buffer[PACKET_SIZE + 1];

	snprintf(buffer, sizeof(buffer), "1\n%s\n", VERSION);
	return send_success(fd, message, buffer);
}


/** Create a faked decoded butten press event: remote code count raw. */
static int simulate_cmd(int fd, const char* msg, const char* args)
{
	std::vector<std::string> commands = split_once(msg);
	std::vector<std::string> arguments = split_once(args);

	FdItemIterator backend = setup_backend_cmd(fd, arguments, msg, 2);
	if (backend == fdList->end())
		return 0;
	backend->expected = commands[0];
	Simvalues simvalues;
	if (!simvalues.parse(arguments[1].c_str())) {
		send_error(fd, msg, "Cannot parse input: %s", arguments[1]);
		disconnect_fds(fd);
		return 0;
	}
	FdItemIterator client = fdList->find_fd(fd);
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
	FdItemIterator it = fdList->find_fd(default_backend);
	if (it == fdList->end()) {
		send_error(fd, "GET_DEFAULT_BACKEND", "Internal error");
		log_warn("Cannot lookup default backend.");
		return 1;
	}
	send_success(fd, "GET_DEFAULT_BACKEND", (it->id + "\n").c_str());
	return 1;
}


/** LIST_BACKENDS command: Always succeeds, but might return no values. */
static int list_backends_cmd(int fd, const char* msg, const char* args)
{
	FdItemIterator it;
	std::string backends("");

	for (it = fdList->begin(); it != fdList->end(); it += 1) {
		if (it->kind != FdItem::BACKEND_CMD)
			continue;
		backends += it->id + "\n";
	}
	send_success(fd, msg, backends.c_str());
	return 1;
}


/** LIST_REMOTES command: dispatch to correct backend. */
static int list_remotes_cmd(int fd, const char* msg, const char* args) {
	std::vector<std::string> commands = split_once(msg);
	std::vector<std::string> arguments = split_once(args);

	FdItemIterator backend = setup_backend_cmd(fd, arguments, msg, 1);
	if (backend == fdList->end())
		return 0;
	backend->expected = commands[0];
	FdItemIterator client = fdList->find_fd(fd);
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

	FdItemIterator backend = setup_backend_cmd(fd, arguments, msg, 2);
	if (backend == fdList->end())
		return 0;
	backend->expected = commands[0];
	FdItemIterator client = fdList->find_fd(fd);
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
	FdItemIterator it;

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

	FdItemIterator backend = setup_backend_cmd(fd, arguments, msg, 1);
	if (backend == fdList->end())
		return 0;
	backend->expected = commands[0];
	FdItemIterator client = fdList->find_fd(fd);
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
static int send_cmd(int fd, const char* msg, const char* argument)
{
	std::vector<std::string> commands = split_once(msg);
	std::vector<std::string> arguments = split_once(argument);

	FdItemIterator backend = setup_backend_cmd(fd, arguments, msg, 2);
	if (backend == fdList->end())
		return 0;
	backend->expected = commands[0];
	FdItemIterator client = fdList->find_fd(fd);
	if (client == fdList->end()) {
		send_error(fd, msg, "Internal error: send_cmd: bad fd");
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

	FdItemIterator backend = setup_backend_cmd(fd, arguments, msg, 0);
	if (backend == fdList->end())
		return 0;
	backend->expected = commands[0];
	FdItemIterator client = fdList->find_fd(fd);
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


/**
 * Break input into lines and invoke line_handler(line, fd) for each line.
 * The line_handler func returns true if the socket is functional and can be
 * used; same goes for get_line().
 */
bool get_line(int fd, LineBuffer* lineBuffer, LineHandler line_handler)
{
	int length;
	char buffer[PACKET_SIZE + 1];

	length = read_timeout(fd, buffer, PACKET_SIZE, 5);
	if (length < 0 ) {
		log_debug("get_line: No data from read_timeout()");
		return false;
	}
	lineBuffer->append(buffer, length);
	log_trace("Received input on %d: '%s'", fd, lineBuffer->c_str());
	while (lineBuffer->has_lines()) {
		std::string line = lineBuffer->get_next_line();
		if (!line_handler(line.c_str(), fd))
			return false;
	}
	return true;
}


const struct protocol_directive directives[64] = {
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
