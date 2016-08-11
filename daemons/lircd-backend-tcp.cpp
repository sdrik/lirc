/**
 * @file lircd.c
 * A tcp listener lirc backend
 *
 * Copyright (c) 2015 Alec Leamas
 *
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <time.h>

#include <vector>
#include <string>



#ifdef HAVE_SYSTEMD
#include "systemd/sd-daemon.h"
#endif

#if defined __APPLE__ || defined __FreeBSD__
#include <sys/ioctl.h>
#endif

#include "lirc_private.h"
#include "pidfile.h"
#include "lircd_messages.h"


#define DEBUG_HELP "Bad debug level: \"%s\"\n\n" \
	"Level could be ERROR, WARNING, NOTICE, INFO, DEBUG, TRACE, TRACE1,\n" \
	" TRACE2 or a number in the range 3..10.\n"


const char* const ARG_HELP =
	"lircd: invalid argument count\n"
	"lircd: lircd does not use a confile file. However, backends do.\n";


static const char* const help =
	"Usage: lircd-backend-std [options]\n"
	"\t -h --help\t\t\tDisplay this message\n"
	"\t -v --version\t\t\tDisplay version\n"
	"\t -O --options-file\t\tOptions file\n"
	"\t -n --nodaemon\t\t\tDon't fork to background\n"
	"\t -d --device=device\t\tOutput to given device\n"
	"\t -l --listen[=[address:]port]\tListen for network connections\n"
	"\t -P --pidfile=file\t\tDaemon pid file\n"
	"\t -L --logfile=file\t\tLog file path (default: use syslog)'\n"
	"\t -D[level] --loglevel[=level]\t'info', 'warning', 'notice', etc., or 3..10.\n";


static const struct option lircd_options[] = {
	{ "help",	    no_argument,       NULL, 'h' },
	{ "version",	    no_argument,       NULL, 'v' },
	{ "nodaemon",	    no_argument,       NULL, 'n' },
	{ "options-file",   required_argument, NULL, 'O' },
	{ "device",	    required_argument, NULL, 'd' },
	{ "listen",	    optional_argument, NULL, 'l' },
	{ "pidfile",	    required_argument, NULL, 'P' },
	{ "logfile",	    required_argument, NULL, 'L' },
	{ "debug",	    optional_argument, NULL, 'D' },  // compatibility
	{ "loglevel",	    optional_argument, NULL, 'D' },
	{ 0,		    0,		       0,    0	 }
};

struct options {
	int		nodaemon;
	const char*	backend_socket_path;
	const char*	pidfile_path;
	loglevel_t	loglevel;
	const char*	logfile;
	struct in_addr  interface;
	unsigned short	tcp_port;
};

struct files {
	int lircd_fd;   /**< Socket connected to lircd's backend socket. */
	int listen_fd;  /**< Socket where we listen to connecting clients. */
	int client_fd; 	/**< Socket created after accepting client. */
	int event_fd;   /** Socket connected to lircd button press fifo. */
	std::string client_address; /**< Client ip address from accept(U) */
	bool connected; /**< True if processing a lircd command. */
};


static const logchannel_t logchannel = LOG_DISPATCH;


/** Decoded command line options and args. */
static const struct options* options;

static void (*dosigterm)(int);

/** Parse -l host:port string. Returns success, sets iface/port or errmsg. */
static bool opt2host_port(const char*		optarg_arg,
			  struct in_addr* 	interface,
			  unsigned short*	port,
			  std::string*		errmsg)
{
	char optarg[128];
	char buff[128];

	strncpy(optarg, optarg_arg, sizeof(optarg) - 1);
	long p;
	char* endptr;
	char* sep = strchr(optarg, ':');
	const char* port_str = sep ? sep + 1 : optarg;

	p = strtol(port_str, &endptr, 10);
	if (!*optarg || *endptr || p < 1 || p > USHRT_MAX) {
		if (errmsg == 0)
			return false;
		snprintf(buff, sizeof(buff),
			"lircd-tcp-backend: bad port number \"%s\"\n",
			port_str);
		*errmsg = std::string(buff);
		return false;
	}
	*port = (unsigned short) p;
	if (sep) {
		*sep = '\0';
		if (!inet_aton(optarg, interface)) {
			if (errmsg == 0)
				return false;
			snprintf(buff, sizeof(buff),
				 "lircd-tcp-backend: bad address \"%s\"\n",
				 optarg);
			*errmsg = std::string(buff);
			return false;
		}
	}
	return true;
}


/** Setup option defaults, respecting lircd.conf. */
static void lircd_add_defaults(void)
{
	char level[4];

	snprintf(level, sizeof(level), "%d", lirc_log_defaultlevel());

	const char* const defaults[] = {
		"lircd:nodaemon",	"False",
		"lircd:output",		LIRCD ".backend"
		"lircd:pidfile",	PIDFILE,
		"lircd:logfile",	"syslog",
		"lircd:debug",		level,
		0,			0
	};
	options_add_defaults(defaults);
}


/** Parse the command line options, store in lirc_options db. */
static void parse_options(int argc, char** const argv)
{
	int c;
	loglevel_t loglevel_opt;
	const char* opt;

	const char* optstring = "O:d:hvnP:l:L:D::";

	strncpy(progname, "lircd-backend-tcp", sizeof(progname));
	optind = 1;
	lircd_add_defaults();
	while ((c = getopt_long(argc, argv, optstring, lircd_options, NULL))
	       != -1) {
		switch (c) {
		case 'h':
			fputs(help, stdout);
			exit(EXIT_SUCCESS);
		case 'v':
			printf("lircd %s\n", VERSION);
			exit(EXIT_SUCCESS);
		case 'd':
			options_set_opt("lircd:output", optarg);
			break;
		case 'O':
			break;
		case 'n':
			options_set_opt("lircd:nodaemon", "True");
			break;
		case 'P':
			options_set_opt("lircd:pidfile", optarg);
			break;
		case 'l': if (true) {
				struct in_addr h;
				unsigned short p;
				std::string errmsg;
				if (!opt2host_port(optarg, &h, &p, &errmsg)) {
					fputs(errmsg.c_str(), stderr);
					exit(EXIT_FAILURE);
				}
				options_set_opt("lircd:listen_hostport",
						optarg);
				break;
			}
		case 'L':
			options_set_opt("lircd:logfile", optarg);
			break;
		case 'o':
			options_set_opt("lircd:output", optarg);
			break;
		case 'D':
			loglevel_opt = (loglevel_t) options_set_loglevel(
				optarg ? optarg : "debug");
			if (loglevel_opt == LIRC_BADLEVEL) {
				fprintf(stderr, DEBUG_HELP, optarg);
				exit(EXIT_FAILURE);
			}
			break;
		default:
			printf("Usage: lircd-backend-tcp [options]\n");
			exit(EXIT_FAILURE);
		}
	}
	if (optind != argc) {
		fputs(ARG_HELP, stderr);
		exit(EXIT_FAILURE);
	}
	opt = options_getstring("lircd:debug");
	if (options_set_loglevel(opt) == LIRC_BADLEVEL) {
		fprintf(stderr, "Bad configuration loglevel:%s\n", opt);
		fprintf(stderr, DEBUG_HELP, optarg);
		fputs("Falling back to 'info'\n", stderr);
		options_set_opt("lircd:debug", "info");
	}
}


/** Build a struct options based on values in lircd_options db. */
const struct options* const get_options(int argc, char** argv)
{
	char buff[128];

	static struct options options = { 0 };
	int r;

	options_load(argc, argv, NULL, parse_options);
	options.logfile = options_getstring("lircd:logfile");
	options.nodaemon = options_getboolean("lircd:nodaemon");
	snprintf(buff, sizeof(buff), "%s", options_getstring("lircd:output"));
	options.backend_socket_path = strdup(buff);
	options.pidfile_path = options_getstring("lircd:pidfile");
	options.loglevel = (loglevel_t) options_getint("lircd:debug");
	const char* hostport = options_getstring("lircd:listen_hostport");
	opt2host_port(hostport, &options.interface, &options.tcp_port, 0);
	if (options_getstring("lircd:listen") != NULL) {
		const char* opt = options_getstring("lircd:listen_hostport");
		std::string errmsg;
		if (opt) {
			r = opt2host_port(opt, &options.interface,
					  &options.tcp_port, &errmsg);
			if (r != 0) {
				fputs(errmsg.c_str(), stderr);
				exit(EXIT_FAILURE);
			}
		} else {
			options.tcp_port = LIRC_INET_PORT;
		}
	}
	return (const struct options* const) &options;
}


/** Set socket opts so that close(sock) doesn't wait for completion. */
static void nolinger(int sock)
{
	static const struct linger linger = { 0, 0 };
	const int lsize = sizeof(struct linger);

	setsockopt(sock, SOL_SOCKET, SO_LINGER,
		   reinterpret_cast<const void*>(&linger), lsize);
}


/** Accept socket connnection  and update files->client*. */
static bool add_client(struct files* files)
{
	int fd;
	socklen_t clilen;
	struct sockaddr_in client_addr;
	int enable = 1;

	clilen = sizeof(client_addr);
	fd = accept(files->listen_fd, (struct sockaddr*)&client_addr, &clilen);
	if (fd == -1) {
		log_perror_err("accept() failed for new client");
		// dosigterm(SIGTERM);
		return false;
	}
	nolinger(fd);
	(void)setsockopt(fd,
			 SOL_SOCKET, SO_KEEPALIVE, &enable, sizeof(enable));
	files->client_address = std::string(inet_ntoa(client_addr.sin_addr));
	files->client_fd = fd;
	log_trace("Adding new client: %d", fd);
	return true;
}


/** Setup a local network listening socket, update files->listen_fd */
static bool
setup_socket(struct in_addr address, unsigned short port, struct files* files)
{
	int fd;
	struct sockaddr_in serv_addr;
	int r;
	int enable = 1;

	fd = socket(PF_INET, SOCK_STREAM, IPPROTO_IP);
	if (fd == -1) {
		log_perror_err("Could not create TCP/IP socket");
		return false;
	}
	(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr = address;
	serv_addr.sin_port = htons(port);
	r = bind(fd, (struct sockaddr*) &serv_addr, sizeof(serv_addr));
	if (r == -1) {
		log_perror_err("Could not assign address to socket");
		close(fd);
		return false;
	}
	listen(fd, 3);
	nolinger(fd);
	files->listen_fd = fd;
	return fd;
}


void log_and_exit(int fd, const char* why)
{
	log_error(why);
	exit(EXIT_FAILURE);
}


/** Read data from lircd and send to client, sets files->connected. */
bool handle_lircd_input(struct files* files)
{
	char buff[1024];
	int r;
	r = read(files->lircd_fd, buff, sizeof(buff));
	if (r == 0) {
		log_warn("Empty read from lircd");
		return false;
	}
	write_socket(files->client_fd, buff, r);
	files->connected = true;
	return true;
}


/**
 * Read available data from client and send to lircd, possibly
 * unset files->connected  if there is an END\n.
 */
bool handle_client_input(struct files* files)
{
	char buff[1024];
	int r;
	r = read(files->client_fd, buff, sizeof(buff));
	if (r == 0) {
		log_warn("Empty read from client");
		return false;
	}
	if (files->connected)
		write_socket(files->lircd_fd, buff, r);
	else
		write_socket(files->event_fd, buff, r);
	if (strstr(buff, "END\n") != NULL)
		files->connected = false;
	return true;
}


/** Main loop: do poll(), handle signals and sockets with pending data. */
static int main_loop(const struct options* options, struct files* files)
{
	int r;
	std::vector<struct pollfd> pollfds;

	while (1) {
		pollfds.push_back({files->client_fd, POLLIN, 0});
		pollfds.push_back({files->lircd_fd, POLLIN, 0});
		do {
			r = poll(&pollfds[0], pollfds.size(), -1);
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
				log_and_exit(pollfds[i].fd, "POLLERR");
			if ((pollfds[i].revents & POLLNVAL) != 0)
				log_and_exit(pollfds[i].fd, "POLLNVAL");
			if ((pollfds[i].revents & POLLIN) != 0) {
				if (pollfds[i].fd == files->client_fd)
					handle_client_input(files);
				else if (pollfds[i].fd == files->lircd_fd)
					handle_lircd_input(files);
				else
					log_warn("Input on unknown socket");
			}
			if ((pollfds[i].revents & POLLHUP) != 0)
				log_and_exit(pollfds[i].fd, "POLLHUP");
		}
	}
}


/** Send reply to GET_BACKEND:INFO command. */
static void send_backend_info(const struct files* files)
{
	std::string info =
		"tcp-backend " + std::to_string(getpid()) + " "
		+ files->client_address + " "
		+ std::to_string(files->client_fd) + "\n";
	send_success(files->lircd_fd, "GET_BACKEND_INFO", info.c_str());
}


/** Handle SET_DATA_SOCKET command, update files->event_fd. */
static bool set_data_socket(std::vector<std::string> words,
			    struct files* files)
{
	if (words.size() < 2) {
		log_perror_warn("Malformed SET_DATA_SOCKET command");
		send_error(files->lircd_fd, "SET_DATA_SOCKET",
			   "Malformed command");
		return false;
	}
	files->event_fd = open(words[1].c_str(), O_WRONLY);
	if (files->event_fd == -1) {
		log_perror_warn("Cannot open decoded events fifo");
		send_error(files->lircd_fd, "SET_DATA_SOCKET",
			   "Cannot open fifo %s", words[1].c_str());
		return false;
	}
	send_success(files->lircd_fd, "SET_DATA_SOCKET");
	return true;
}


/** Main function in forked child handling client. */
static bool run_client(const struct options* options, struct files* files)
{
	FILE* f;
	int fd;
	char buff[128];
	std::vector<std::string> words;
	char*  r;

	fd = dup(files->lircd_fd);
	f = fdopen(fd, "r");
	if (f == NULL) {
		log_perror_err("Cannot fdopen client fd");
		return false;
	}
	while (true) {
		r = fgets(buff, sizeof(buff), f);
		if (r == NULL) {
			log_perror_err("Error in initialization sequence");
			return false;
		}
		words = split_once(buff);
		if (words.size() == 0) {
			log_notice("Empty initialization command");
			continue;
		}
		if (words[0] == "GET_BACKEND_INFO") {
			send_backend_info(files);  // FIXME.
		} else if (words[0] == "SET_DATA_SOCKET") {
			bool ok = set_data_socket(words, files);
			fclose(f);
			if (!ok)
				return false;
			main_loop(options, files);
		} else {
			log_warn("Unknown initialization command: %s",
				 words[0]);
			continue;
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


/** Creates global pidfile and obtains the lock on it. Exits on errors */
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


/** Run run_client() in isolated process using double fork(). */
static void fork_child(const struct options* options, struct files* files)
{
	pid_t pid1;
	pid_t pid2;

	pid1 = fork();
	if (pid1 < 0) {
		log_perror_err("Cannot fork");
		perror("Cannot fork()");
		exit(EXIT_FAILURE);
	}
	if (pid1 == 0) {
		// child
		pid2 = fork();
		if (pid2 < 0) {
			log_perror_err("Cannot do secondary fork()");
			exit(EXIT_FAILURE);
		}
		if (pid2 == 0) {
			log_debug("Execing run_client()");
			run_client(options, files);
			/* not reached */
			log_perror_err("run_client() exited");
			fputs("run_client() exited\n", stderr);
		} else {
			waitpid(pid2, NULL, WNOHANG);
			exit(0);
		}
	} else {
		// parent
		return;
	}
}


/**
 * Return a socket connected to the lircd output socket, typically
 * /var/run/lirc/lircd. Update files-> lircd_fd.
 */
int connect_to_lircd(const struct options* options, struct files* files)
{
	struct sockaddr_un addr;
	int fd;

	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path,
		options->backend_socket_path,
		sizeof(addr.sun_path) - 1);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1) {
		log_perror_err("socket");
		return false;
	}
	if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
		log_perror_err("Cannot connect to socket %s", addr.sun_path);
		return false;
	}
	files->lircd_fd = fd;
	return true;
}


int main(int argc, char** argv)
{
	std::string client_address;
	struct files files = {0};

	options = get_options(argc, argv);
	if (options->logfile != NULL)
		lirc_log_set_file(options->logfile);
	lirc_log_open("lircd", options->nodaemon, options->loglevel);
	create_pidfile();

	signal(SIGPIPE, SIG_IGN);
	if (!options->nodaemon)
		daemonize();
	if (!setup_socket(options->interface, options->tcp_port, &files)) {
		log_error("Cannot initialize backend.");
		perrorf("Could not setup socket %s, path");
		Pidfile::instance()->close();
		return EXIT_FAILURE;
	}
	log_notice("lircd-backend-tcp ready, using %s and %d",
		   options->backend_socket_path,
		   options->tcp_port);
	while (true) {
		if (!add_client(&files)) {
			log_error("Cannot connect to client, exiting.");
			break;
		}
		if (!connect_to_lircd(options, &files)) {
			log_error("Cannot connect to lircd, exiting.");
			break;
		}
		fork_child(options, &files);
	}
	Pidfile::instance()->close();
	return EXIT_FAILURE;
};
