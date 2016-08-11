
/**
 * @file backends-commands
 * This file implements backend socket commands
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <getopt.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/file.h>
#include <pwd.h>
#include <poll.h>

#if defined(__linux__)
#include <linux/input.h>
#include "lirc/input_map.h"
#endif

#ifdef HAVE_SYSTEMD
#include "systemd/sd-daemon.h"
#endif

#if defined __APPLE__ || defined __FreeBSD__
#include <sys/ioctl.h>
#endif

#include "lirc_private.h"

#include "lircd_messages.h"
#include "backend-commands.h"
#include "line_buffer.h"

#define WHITE_SPACE " \t"

static const logchannel_t logchannel = LOG_APP;

static struct repeat_ctx* repeat_ctx = NULL;

static int events_fd = -1;

static struct ir_remote* remotes = NULL;

static LineBuffer lineBuffer;


static int list(int fd, const char* message, const char* arguments);
static int set_transmitters(int fd, const char* message, const char* arguments);
static int set_inputlog(int fd, const char* message, const char* arguments);
static int send_once(int fd, const char* message, const char* arguments);
static int drv_option(int fd, const char* message, const char* arguments);
static int send_start(int fd, const char* message, const char* arguments);
static int send_stop(int fd, const char* message, const char* arguments);
static int send_core(int fd, const char* message, const char* arguments, int once);
static int version(int fd, const char* message, const char* arguments);
static int get_backend_info(int fd, const char* message, const char* arguments);
static int set_data_socket(int fd, const char* message, const char* arguments);


enum protocol_string_num {
	P_BEGIN = 0,
	P_DATA,
	P_END,
	P_ERROR,
	P_SUCCESS,
	P_SIGHUP
};


static const char* const protocol_string[] = {
	"BEGIN\n",
	"DATA\n",
	"END\n",
	"ERROR\n",
	"SUCCESS\n",
	"SIGHUP\n"
};


int get_events_fd() { return events_fd; }


void set_events_fd(int fd) { events_fd = fd; }


struct ir_remote* get_remotes() { return remotes; }


void set_remotes(struct ir_remote* r) { remotes = r; }


void commands_init(struct repeat_ctx* ctx)
{
	repeat_ctx = ctx;
}


static int parse_rc(int fd,
  		    const char* message, const char* arguments_arg,
		    struct ir_remote** remote, struct ir_ncode** code,
		    unsigned int* reps, int n, int* err)
{
	char* name = NULL;
	char* command = NULL;
	char* repeats;
	char* end_ptr = NULL;
	char arguments[128];

	*remote = NULL;
	*code = NULL;
	*err = 1;
	if (arguments_arg == NULL)
		goto arg_check;
	strncpy(arguments, arguments_arg, sizeof(arguments) - 1);

	name = strtok(arguments, WHITE_SPACE);
	if (name == NULL)
		goto arg_check;
	*remote = get_ir_remote(remotes, name);
	if (*remote == NULL)
		return send_error(fd, message,
				  "unknown remote: \"%s\"\n", name);
	command = strtok(NULL, WHITE_SPACE);
	if (command == NULL)
		goto arg_check;
	*code = get_code_by_name(*remote, command);
	if (*code == NULL)
		return send_error(fd, message,
				  "unknown command: \"%s\"\n", command);
	if (reps != NULL) {
		repeats = strtok(NULL, WHITE_SPACE);
		if (repeats != NULL) {
			*reps = strtol(repeats, &end_ptr, 10);
			if (*end_ptr || *reps < 0)
				return send_error(fd, message,
						  "bad send packet (reps/eol)\n");
			if (*reps > repeat_ctx->repeat_max)
				return send_error (fd, message,
						   "too many repeats: \"%d\" > \"%u\"\n",
						   *reps, repeat_ctx->repeat_max);
		} else {
			*reps = -1;
		}
	}
	if (strtok(NULL, WHITE_SPACE) != NULL)
		return send_error(fd, message, "bad send packet (trailing ws)\n");
arg_check:
	if (n > 0 && *remote == NULL)
		return send_error(fd, message, "remote missing\n");
	if (n > 1 && *code == NULL)
		return send_error(fd, message, "code missing\n");
	*err = 0;
	return 1;
}



int send_remote_list(int fd, const char* message)
{
	char buffer[PACKET_SIZE + 1];
	struct ir_remote* all;
	int n, len;

	n = 0;
	all = remotes;
	while (all) {
		n++;
		all = all->next;
	}

	if (!(write_socket_len(fd, protocol_string[P_BEGIN])
	       && write_socket_len(fd, message)
	       && write_socket_len(fd, protocol_string[P_SUCCESS]))) {
			return 0;
	}
	if (n == 0)
		return write_socket_len(fd, protocol_string[P_END]);
	sprintf(buffer, "%d\n", n);
	if (!(write_socket_len(fd, protocol_string[P_DATA])
	      && write_socket_len(fd, buffer))) {
		return 0;
	}
	all = remotes;
	while (all) {
		len = snprintf(buffer, PACKET_SIZE + 1, "%s\n", all->name);
		if (len >= PACKET_SIZE + 1)
			len = sprintf(buffer, "name_too_long\n");
		if (write_socket(fd, buffer, len) < len)
			return 0;
		all = all->next;
	}
	return write_socket_len(fd, protocol_string[P_END]);
}


int send_remote(int fd, const char* message, struct ir_remote* remote)
{
	struct ir_ncode* codes;
	char buffer[PACKET_SIZE + 1];
	int n, len;

	n = 0;
	codes = remote->codes;
	if (codes != NULL) {
		while (codes->name != NULL) {
			n++;
			codes++;
		}
	}
	if (!(write_socket_len(fd, protocol_string[P_BEGIN])
	       && write_socket_len(fd, message)
	       && write_socket_len(fd, protocol_string[P_SUCCESS]))) {
			return 0;
	}
	if (n == 0)
		return write_socket_len(fd, protocol_string[P_END]);
	sprintf(buffer, "%d\n", n);
	if (!(write_socket_len(fd, protocol_string[P_DATA])
	      && write_socket_len(fd, buffer))) {
		return 0;
	}
	codes = remote->codes;
	while (codes->name != NULL) {
		// NOLINTNEXTLINE
		len = snprintf(buffer, PACKET_SIZE, "%016llx %s\n",
			      (unsigned long long)codes->code, codes->name);    //NOLINT
		if (len >= PACKET_SIZE + 1)
			len = sprintf(buffer, "code_too_long\n");
		if (write_socket(fd, buffer, len) < len)
			return 0;
		codes++;
	}
	return write_socket_len(fd, protocol_string[P_END]);
}


int send_name(int fd, const char* message, struct ir_ncode* code)
{
	char buffer[PACKET_SIZE + 1];
	int len;

	if (!(write_socket_len(fd, protocol_string[P_BEGIN])
	       && write_socket_len(fd, message)
	       && write_socket_len(fd, protocol_string[P_SUCCESS])
	       && write_socket_len(fd, protocol_string[P_DATA]))) {
			return 0;
	}
	// NOLINTNEXTLINE
	len = snprintf(buffer, PACKET_SIZE,
		       "1\n%016llx %s\n",
		       (unsigned long long)code->code, code->name);
	if (len >= PACKET_SIZE + 1)
		len = sprintf(buffer, "1\ncode_too_long\n");
	if (write_socket(fd, buffer, len) < len)
		return 0;
	return write_socket_len(fd, protocol_string[P_END]);
}


static int list(int fd, const char* message, const char* arguments)
{
	struct ir_remote* remote;
	struct ir_ncode* code;
	int err;
	int r;

	r = parse_rc(fd, message, arguments, &remote, &code, 0, 0, &err);
	if (r == 0)
		return 0;
	if (err)
		return 1;

	if (remote == NULL)
		return send_remote_list(fd, message);
	if (code == NULL)
		return send_remote(fd, message, remote);
	return send_name(fd, message, code);
}


static int
set_transmitters(int fd, const char* message, const char* arguments_arg)
{
	char arguments[128];
	char* next_arg = NULL;
	char* end_ptr;
	__u32 next_tx_int = 0;
	__u32 next_tx_hex = 0;
	__u32 channels = 0;
	int r = 0;
	unsigned int i;

	if (arguments_arg == NULL)
		return send_error(fd, message, "no arguments given\n");
	if (curr_driver->send_mode == 0)
		return send_error(fd, message,
				  "hardware does not support sending\n");
	if (curr_driver->drvctl_func == NULL
	    || !(curr_driver->features & LIRC_CAN_SET_TRANSMITTER_MASK)) {
		return send_error(
			fd, message,
			"hardware does not support multiple transmitters\n");
	}

	strncpy(arguments, arguments_arg, sizeof(arguments) - 1);
	next_arg = strtok(arguments, WHITE_SPACE);
	if (next_arg == NULL)
		return send_error(fd, message, "no arguments given\n");
	do {
		next_tx_int = strtoul(next_arg, &end_ptr, 10);
		if (*end_ptr || next_tx_int == 0
		    || (next_tx_int == ULONG_MAX && errno == ERANGE)) {
			return send_error(fd, message, "invalid argument\n");
		}
		if (next_tx_int > MAX_TX)
			return send_error(
				fd, message,
				"cannot support more than %d transmitters\n",
				MAX_TX);
		next_tx_hex = 1;
		for (i = 1; i < next_tx_int; i++)
			next_tx_hex = next_tx_hex << 1;
		channels |= next_tx_hex;
	} while ((next_arg = strtok(NULL, WHITE_SPACE)) != NULL);

	r = curr_driver->drvctl_func(LIRC_SET_TRANSMITTER_MASK, &channels);
	if (r < 0)
		return send_error(fd, message,
				  "error - could not set transmitters\n");
	if (r > 0)
		return send_error(fd, message,
				  "error - maximum of %d transmitters\n", r);
	return send_success(fd, message);
}


static int get_backend_info(int fd, const char* message, const char* arguments)
{
	char buff[128];

	snprintf(buff, sizeof(buff), "std %d %s %s\n",
		 getpid(), curr_driver->name, curr_driver->device);
	return send_success(fd, message, buff);
}


/** Trim leading and trailing space in s, return new string. */
static char* trim(char* s)
{
	char* end;

	while (isspace(*s) && *s != '\0')
		s += 1;
	end = s + strlen(s) - 1;
	while (end > s && isspace(*end))
		end -= 1;
	if (isspace(*end))
		*end = '\0';
	return s;
}


/** Set the socket used to send decoded events to lircd. */
static int set_data_socket(int fd, const char* message, const char* arguments)
{
	char argbuff[128];
	const char* arg;

	if (arguments == NULL) {
		arg = "(null)";
	} else {
		strncpy(argbuff, arguments, sizeof(argbuff) - 1);
		arg = trim(argbuff);
	}
	char buff[128];

	if (events_fd >= 0) {
		log_notice("Re-opening new events fifo.");
		close(events_fd);
		events_fd = -1;
	}
	events_fd = open(arg, O_WRONLY);
	if (events_fd < 0){
		send_error(fd, message,
			   "Cannot open event fifo %s", arg);
		return 0;
	}
	snprintf(buff, sizeof(buff), "%s\n", message);
	char* ws = strchr(buff, ' ');
	if (ws != NULL) {
		*ws = '\n';
		ws += 1;
		if (ws < (buff + sizeof(buff) - 1))
			*ws = '\0';
	}
	send_success(fd, buff);
	return 1;
}


static int send_once(int fd, const char* message, const char* arguments)
{
	return send_core(fd, message, arguments, 1);
}


static int send_start(int fd, const char* message, const char* arguments)
{
	return send_core(fd, message, arguments, 0);
}


static int
send_core(int fd, const char* message, const char* arguments, int once)
{
	struct ir_remote* remote;
	struct ir_ncode* code;
	unsigned int reps;
	int err;
	int r;

	log_debug("Sending once, msg: %s, args: %s, once: %d",
		  message, arguments, once);
	if (curr_driver->send_mode == 0)
		return send_error(fd, message,
				  "hardware does not support sending\n");

	r = parse_rc(fd, message, arguments, &remote, &code,
		     once ? &reps : NULL, 2, &err);
	if (r == 0)
		return 0;
	if (err)
		return 1;
	if (once) {
		if (repeat_remote != NULL)
			return send_error(fd, message, "busy: repeating\n");
	} else {
		if (repeat_remote != NULL)
			return send_error(fd, message, "already repeating\n");
	}
	if (has_toggle_mask(remote))
		remote->toggle_mask_state = 0;
	if (has_toggle_bit_mask(remote))
		remote->toggle_bit_mask_state =
			(remote->toggle_bit_mask_state ^
					remote->toggle_bit_mask);
	code->transmit_state = NULL;
	struct timespec before_send;
	clock_gettime(CLOCK_MONOTONIC, &before_send);
	if (!send_ir_ncode(remote, code, 1))
		return send_error(fd, message, "transmission failed\n");
	gettimeofday(&remote->last_send, NULL);
	remote->last_code = code;
	if (once)
		remote->repeat_countdown = max(remote->repeat_countdown, reps);
	else
		/* you've been warned, now we have a limit */
		remote->repeat_countdown = repeat_ctx->repeat_max;
	if (remote->repeat_countdown > 0 || code->next != NULL) {
		repeat_remote = remote;
		repeat_code = code;
		if (once) {
			*(repeat_ctx->repeat_message) = strdup(message);
			if (repeat_ctx->repeat_message == NULL) {
				repeat_remote = NULL;
				repeat_code = NULL;
				return send_error(fd, message,
						  "out of memory\n");
			}
			*(repeat_ctx->repeat_fd) = fd;
		} else if (!send_success(fd, message)) {
			repeat_remote = NULL;
			repeat_code = NULL;
			return 0;
		}
		repeat_ctx->schedule_repeat_timer(&before_send);
		return 1;
	} else {
		return send_success(fd, message);
	}
}


static int send_stop(int fd, const char* message, const char* arguments)
{
	struct ir_remote* remote;
	struct ir_ncode* code;
	struct itimerval repeat_timer;
	int err;
	int r;

	r = parse_rc(fd, message, arguments, &remote, &code, 0, 0, &err);
	if (r == 0)
		return 0;
	if (err)
		return 1;

	if (repeat_remote && repeat_code) {
		int done;

		if (remote &&
		    strcasecmp(remote->name, repeat_remote->name) != 0)
			return send_error(fd, message,
					  "specified remote does not match\n");
		if (code && strcasecmp(code->name, repeat_code->name) != 0)
			return send_error(fd, message,
					  "specified code does not match\n");

		done = repeat_ctx->repeat_max -
				repeat_remote->repeat_countdown;
		if (done < repeat_remote->min_repeat) {
			/* we still have some repeats to do */
			repeat_remote->repeat_countdown =
				repeat_remote->min_repeat - done;
			return send_success(fd, message);
		}
		repeat_timer.it_value.tv_sec = 0;
		repeat_timer.it_value.tv_usec = 0;
		repeat_timer.it_interval.tv_sec = 0;
		repeat_timer.it_interval.tv_usec = 0;

		setitimer(ITIMER_REAL, &repeat_timer, NULL);

		repeat_remote->toggle_mask_state = 0;
		repeat_remote = NULL;
		repeat_code = NULL;
		/* clin!=0, so we don't have to deinit hardware */
		// alrm = 0;  FIXME: Handle open/close backend.
		return send_success(fd, message);
	} else {
		return send_error(fd, message, "not repeating\n");
	}
}


static int version(int fd, const char* message, const char* arguments)
{
	return send_success(fd, message, VERSION);
}


static int drv_option(int fd, const char* message, const char* arguments)
{
	struct option_t option;
	int r;

	r = sscanf(arguments, "%32s %64s", option.key, option.value);
	if (r != 2) {
		return send_error(fd, message,
				  "Illegal argument (protocol error): %s",
				  arguments);
	}
	r = curr_driver->drvctl_func(DRVCTL_SET_OPTION,
				     reinterpret_cast<void*>(&option));
	if (r != 0) {
		log_warn("Cannot set driver option");
		return send_error(fd, message,
				  "Cannot set driver option %d", errno);
	}
	return send_success(fd, message);
}


static int set_inputlog(int fd, const char* message, const char* arguments)
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


static bool check_directive(int fd,
			    const struct protocol_directive* directive,
			    const char* line)
{
	std::vector<std::string> words = split_once(line);
	if (strcasecmp(words[0].c_str(), directive->name) != 0)
		return false;
	if (!directive->function(fd, words[0].c_str(), words[1].c_str()))
		log_debug("Error processing %s", line);
	return true;
}


int get_command(int fd)
{
	int length;
	char buffer[PACKET_SIZE + 1];

	length = read_timeout(fd, buffer, PACKET_SIZE, 0);
	if (length < 0) {
		log_perror_warn("Cannot read command input.");
		return 0;
	}
	log_trace("Got command input: %s", buffer);
	lineBuffer.append(buffer, length);
	while (lineBuffer.has_lines()) {
		std::string line = lineBuffer.get_next_line();
		if (line == "") {
			send_error(fd, line.c_str(), "bad send packet\n");
			log_debug("Empty command line");
			continue;
		}
		log_debug("Processing command: \"%s\"", line.c_str());
		for (int i = 0; directives[i].name != NULL; i++) {
			if (check_directive(fd, &directives[i], line.c_str()))
				return 1;
		}
		if (!send_error(fd, line.c_str(),
				"unknown directive: \"%s\"\n", line.c_str())) {
			return 0;
		}
	}
	return 1;
}


const struct protocol_directive directives[] = {
       { "LIST",             list             },
       { "SEND_ONCE",        send_once        },
       { "SEND_START",       send_start       },
       { "SEND_STOP",        send_stop        },
       { "SET_INPUTLOG",     set_inputlog     },
       { "DRV_OPTION",       drv_option       },
       { "VERSION",          version          },
       { "SET_TRANSMITTERS", set_transmitters },
       { "GET_BACKEND_INFO", get_backend_info },
       { "SET_DATA_SOCKET",  set_data_socket  },
       { NULL,               NULL             }
};
