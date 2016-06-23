/**********************************************************************
** commands.cpp *******************************************************
***********************************************************************
*
* lircd message - read and write command packets.
*
* Copyright (c) 2015 Alec Leamas
*
* */

/**
 * @file commands.cpp
 * @brief Read and write command packets.
 */

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "lircd_messages.h"


#ifndef PACKET_SIZE
#define PACKET_SIZE 256
#endif
#define WHITE_SPACE " \t"

static const logchannel_t logchannel = LOG_DISPATCH;

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


static int count_newlines(const char* s, int  maxsize = -1)
{
	int n = 0;

	if (maxsize == -1)
		maxsize = strlen(s);
	for (int i = 0; i < maxsize; i++) {
		if (s[i] == '\0')
			break;
		if (s[i] == '\n')
			n += 1;
	}
	return n;
}


static void strip_trailing_nl(char* buff)
{
	char* nl = strrchr(buff, '\n');
	if (nl != NULL)
		*nl = '\0';
}



/* A safer write(), since sockets might not write all but only some of the
 * bytes requested */
int write_socket(int fd, const char* buf, int len)
{
	int done, todo = len;

	while (todo) {
		done = write(fd, buf, todo);
		if (done <= 0)
			return done;
		buf += done;
		todo -= done;
	}
	return len;
}


int write_socket_len(int fd, const char* buf)
{
	int len;

	len = strlen(buf);
	if (write_socket(fd, buf, len) < len)
		return 0;
	return 1;
}


int read_timeout(int fd, char* buf, int len, int timeout_us)
{
	int ret, n;
	struct pollfd  pfd = {fd, POLLIN, 0};  // fd, events, revents
	int timeout = timeout_us > 0 ? timeout_us/1000 : -1;


	/* CAVEAT: (from libc documentation)
	 * Any signal will cause `select' to return immediately.  So if your
	 * program uses signals, you can't rely on `select' to keep waiting
	 * for the full time specified.  If you want to be sure of waiting
	 * for a particular amount of time, you must check for `EINTR' and
	 * repeat the `select' with a newly calculated timeout based on the
	 * current time.  See the example below.
	 *
	 * The timeout is not recalculated here although it should, we keep
	 * waiting as long as there are EINTR.
	 */
	do {
		ret = poll(&pfd, 1, timeout);
	} while (ret == -1 && errno == EINTR);
	if (ret == -1) {
		log_perror_err("read_timeout: poll() failed");
		return -1;
	}
	if (ret == 0)
		return 0;       /* timeout */
	n = read(fd, buf, len);
	if (n == -1) {
		log_perror_err("read_timeout: read() failed");
		return -1;
	}
	return n;
}


int send_success(int fd, const char* message)
{
	log_debug("Sending success");
	std::string s("");

	s += protocol_string[P_BEGIN]
		+ std::string(message)
		+ protocol_string[P_SUCCESS]
		+ protocol_string[P_END];
	log_debug("Sending success: \"%s\"", s.c_str());
	return write_socket(fd, s.c_str(), s.size());
}


int send_success(int fd, const char* message, const char* data)
{
	// FIXME: Handle newline in message
	char buff[128];
        char line_count[32];
	char* nl;
	std::string s("");

	strncpy(buff, message, sizeof(buff) - 1);
	nl = strrchr(buff, '\n');
	if (nl != NULL && *(nl + 1) == '\0')
		*nl = '\0';
	snprintf(line_count, sizeof(line_count),
		 "%d\n", count_newlines(data));
	s += protocol_string[P_BEGIN] + std::string(buff) + "\n";
	s += protocol_string[P_SUCCESS];
	s += protocol_string[P_DATA];
	s += line_count;
	s += data;
	s += protocol_string[P_END];
	log_trace("Sending output: %s", s.c_str());
	return write_socket(fd, s.c_str(), s.size());
}

int send_error(int fd, const char* message_arg, const char* format_str, ...)
{
	log_debug("Sending error");
	char message[PACKET_SIZE + 1];
	char lines[4];
	char buffer[PACKET_SIZE + 1];
	int n;
	va_list ap;

	va_start(ap, format_str);
	vsprintf(buffer, format_str, ap);
	va_end(ap);

	strncpy(message, message_arg, sizeof(message));
	strip_trailing_nl(message);
	strip_trailing_nl(buffer);

	n  = count_newlines(buffer) + 1;
	snprintf(lines, sizeof(lines), "%d\n", n);

	std::string s("");
	s += std::string(protocol_string[P_BEGIN])
		+ message + "\n"
		+ protocol_string[P_ERROR]
		+ protocol_string[P_DATA]
		+ lines
		+ std::string(buffer) + "\n"
		+ protocol_string[P_END];
	log_error("Sending error reply to %d: %s", fd, s.c_str());
	return write_socket(fd, s.c_str(), s.size());
}


int send_sighup(int fd)
{
	std::string s("");

        s += std::string(protocol_string[P_BEGIN])
		+ protocol_string[P_SIGHUP]
		+  protocol_string[P_END];
	log_debug("Sending sighup.");
	return write_socket(fd, s.c_str(), s.size());
}


/** Broadcast message to list of fds. Returns list of unwritable fds. */
std::vector<int> broadcast_message(const char* message,
				   const std::vector<int>& fds)
{
	int len = strlen(message);
	std::vector<int> bad_fds;

	for (auto it = fds.begin(); it != fds.end(); it++) {
		log_trace("writing to client %d: %s", *it, message);
		if (write_socket(*it, message, len) < len)
			bad_fds.push_back(*it);
	}
	return bad_fds;
}


/** Return vector of {firstword, remainder} in nl-terminated str. */
std::vector<std::string> split_once(const char* str)
{
	std::vector<std::string> result;
	if (str == NULL || *str =='\0')
		return result;
	char* buff = reinterpret_cast<char*>(alloca(strlen(str) + 2));
	strncpy(buff, str, strlen(str));

	char* token = strtok(buff, " \t\r\n");
	if (token == NULL || *token == '\0')
		return result;
	result.push_back(token);

	token = strtok(NULL, "\n");
	if (token == NULL || *token == '\0')
		return result;
	result.push_back(token);
	return result;
}


