/****************************************************************************
** lircd.cpp ***************************************************************
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
 * The dispatcher works as a broker between clients and one or more
 * backends. It has three well-known socket interfaces:
 *
 *   - The lircd interface is what the clients connects to. It has the
 *     legacy interface described in the 0.9.4 lircd manpage,
 *   - The backend interface is what the backends connects to. When a backend
 *     connects a registration sequence is initated by lircd.
 *   - The control interface is used to send commands to specific backends.
 *     A new tool irtool exposes this as a command line application.
 *
 *  For each connected backend there is also a named pipe where the backend sends
 *  decoded events to lircd.
 *
 *  Lircd basically does three things:
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
 *  created unregistered, and becomes registered after the GET_BACKEND_INFO
 *  and SET_DATA_SOCKET commands from lircd to backend. The command and
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
#include "pidfile.h"

#include "fd_list.h"
#include "lircd_commands.h"
#include "lircd_messages.h"
#include "lircd_options.h"
#include "reply_parser.h"

#define WHITE_SPACE " \t"

static const logchannel_t logchannel = LOG_DISPATCH;

static const int COMMAND_TIMEOUT_TICKS = 20;    /**< Command timeout (ticks).*/
static const int HEARTBEAT_US = 50000;		/**< Timer tick length. */


/** Forward */
extern const struct protocol_directive directives[];

/** Set by signal handlers, executed in main loop. */
std::atomic<void (*)()> signal_handler(0);

/** Decoded command line options and args. */
static const struct options_t* options;

/** Book-keeping data for all active sockets. */
static FdList*  fdList(0);

/** SIGTERM/SIGUSR1 helper, called from main loop. Cleans up and exits. */
void dosigterm(int sig)
{
	ItemIterator it;

	signal(SIGALRM, SIG_IGN);
	log_notice("caught signal");
	if (fdList != 0) {
		for (auto it = fdList->begin(); it != fdList->end(); it++) {
			shutdown(it->fd, SHUT_RDWR);
			close(it->fd);
		}
	}
	Pidfile::instance()->close();
	lirc_log_close();
	signal(sig, SIG_DFL);
	if (sig == SIGUSR1)
		exit(0);
	raise(sig);
}


/** SIGTERM signal handler: setup  dosigterm() calling in main loop. */
void sigterm(int sig)
{
	signal_handler = []() -> void {dosigterm(SIGTERM); };
}


/** SIGUSR1 signal handler: setup  dosigterm() calling in main loop. */
void sigusr1(int sig)
{
	signal_handler = []() -> void {dosigterm(SIGUSR1); };
}


/** SIGHUP handler: Re-read config file. */
static void dosighup()
{
	ItemIterator it;

	/* reopen logfile first */
	if (lirc_log_reopen() != 0) {
		/* can't print any error messagees */
		dosigterm(SIGTERM);
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


/** Set socket opts so that close(sock) doesn't wait for completion. */
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


/** fdList->find() argument. */
static bool find_backend_by_type(const FdItem& item, int what)
{
	return item.kind == FdItem::BACKEND_CMD;
}


/** Find a random, new default client if available. */
static void find_new_default_backend()
{
	ItemIterator item = fdList->find(0, find_backend_by_type);
	if (item == fdList->end())
		commands_set_backend(-1);
	else
		commands_set_backend(item->fd);
	log_debug("New default backend: %d", commands_get_backend());
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
		dosigterm(SIGTERM);
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
 *  for the GET_BACKEND_INFO command.
 */
void add_backend(int sock)
{
	const std::string GET_INFO_CMD("GET_BACKEND_INFO\n");
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
		dosigterm(SIGTERM);
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
	write_socket(cmd_fd, GET_INFO_CMD.c_str(), GET_INFO_CMD.size());
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


/** Handle reply from backend after issuing GET_BACKEND_INFO command. */
void handle_get_backend_info_reply(int fd)
{
	using std::string;

	pid_t pid;
	char name[32];
	char where[64];
	char backend_type[32];
	string path;

	ItemIterator it = fdList->find_fd(fd);
	string reply = it->replyParser->get_data();
	int r = sscanf(reply.c_str(),
		       "%32s %6d %32s %64s",
		       backend_type, &pid, name, where);
	if (r != 4) {
		log_error("Cannot register backend.");  // FIXME
		log_debug("Command: %s", reply.c_str());
		return;
	}
	it->id = string(name) + "@" + string(where);

	get_backend_data_path(fd, &path);
	string cmd("SET_DATA_SOCKET ");
	cmd +=  path + "\n";
	write_socket(fd, cmd.c_str(), cmd.size());
}


/** Handle reply from backend after issuing SET_DATA_SOCKET command. */
void handle_data_socket_reply(int fd)
{
	ItemIterator it = fdList->find_fd(fd);
	if (it == fdList->end()) {
		log_warn("handle_data_socket: Cannot lookup fd.");
		return;
	}
	if (it->replyParser->get_success()) {
		commands_set_backend(fd);
		std::string path;
		get_backend_data_path(fd, &path);
		unlink(path.c_str());
		log_debug("Final backend registration on %s(%d), removing %s",
			  it->id.c_str(), fd, path.c_str());
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
			if (cmd == "GET_BACKEND_INFO") {
				handle_get_backend_info_reply(fd);
			} else if (cmd == "SET_DATA_SOCKET") {
				handle_data_socket_reply(fd);
			} else {
				log_warn("Unknown backend reply: %s", cmd.c_str());
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
	ItemIterator backend = fdList->find_fd(commands_get_backend());
	if (backend != fdList->end()) {
		backend->replyParser->reset();
		backend->expected = std::string(cmdptr);
		connect_fds(fd, commands_get_backend());
		write_socket(commands_get_backend(), line, strlen(line));
	} else {
		log_notice("No backend available, fd: %d", fd);
		send_error(fd, cmdptr,
			   "Backend unavailable, current: %d",
			   commands_get_backend());
	}
	return true;
}


/**
 * Control commands, processed by lircd. Possibly forwarded to designated
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
		if (!get_line(item.fd, &item.lineBuffer, broadcast_message)) {
			remove_and_log(item.fd,
				       "backend_data: get_line() fails");
			find_new_default_backend();
		}
		break;
	case FdItem::BACKEND_CMD:
		if (!get_line(item.fd, &item.lineBuffer, handle_backend_line)) {
			remove_and_log(item.fd,
				       "backend_cmd: get_line() fails");
			find_new_default_backend();
		}
		break;
	case FdItem::CLIENT_STREAM:
		if (!get_line(item.fd, &item.lineBuffer, handle_client_line))
			remove_and_log(item.fd, "client: get_line() fails");
		break;
	case FdItem::CTRL_STREAM:
		if (!get_line(item.fd, &item.lineBuffer, handle_ctrl_cmd))
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
		dosigterm(SIGTERM);
	}
	umask(0);
	Pidfile::instance()->update(getpid());
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
	Pidfile* pidfile = Pidfile::instance();

	/* create pid lockfile in /var/run */
        result = pidfile->lock(options->pidfile_path);
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
		Pidfile::instance()->close();
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
			Pidfile::instance()->close();
			exit(EXIT_FAILURE);
		}
	}
	backend_sock_fd = setup_socket(options->backend_socket_path, 0666);
	if (backend_sock_fd == -1) {
		perrorf("Could not setup socket %s",
			options->backend_socket_path);
		Pidfile::instance()->close();
		shutdown(client_sock_fd, SHUT_RDWR);
		exit(EXIT_FAILURE);
	}
	ctrl_sock_fd = setup_socket(options->ctrl_socket_path, 0666);
	if (ctrl_sock_fd == -1) {
		perrorf("Could not setup socket %s",
			options->ctrl_socket_path);
		shutdown(client_sock_fd, SHUT_RDWR);
		shutdown(backend_sock_fd, SHUT_RDWR);
		Pidfile::instance()->close();
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

	if (!options->nodaemon)
		daemonize();
	log_notice("lircd ready, using %s", options->client_socket_path);

	start_heartbeat();
	commands_init(fdList);

	main_loop(options, 0);

	/* Not reached */
	return EXIT_SUCCESS;
};
