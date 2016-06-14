/*

  irsend -  application for sending IR-codes via lirc

  Copyright (C) 1998 Christoph Bartelmus (lirc@bartelmus.de)

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA

*/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <getopt.h>
#include <errno.h>
#include <limits.h>

#include <sstream>
#include <string>
#include <vector>

#include "lirc_log.h"
#include "lirc_client.h"

#include "reply_parser.h"

static const logchannel_t logchannel = LOG_APP;

static const char* const help =
	"\nSynopsis:\n"
	"    irtool [options] send <remote> <code> [code...]\n"
	"    irtool [options] send-start remote code\n"
	"    irtool [options] send-stop remote code\n"
	"    irtool [options] set-default-backend\n"
	"    irtool [options] get-default-backend\n"
	"    irtool [options] stop-backend\n"
	"    irtool [options] list-backends\n"
	"    irtool [options] list-remotes\n"
	"    irtool [options] list-codes <remote>\n"
	"    irtool [options] set-transmitters remote num [num...]\n"
	"    irtool [options] simulate  <remote> <keysym> [scancode]\n"
	"\n"
	"Options:\n"
	"    -h --help\t\t\tDisplay usage summary\n"
	"    -v --version\t\tDisplay version\n"
	"    -b --backend=backend\tUse given lircd backend\n"
	"    -d --device=device\t\tUse given socket [" LIRCD ".control]\n"
	"    -# --count=n\t\tSend command n times\n";

static const struct option long_options[] = {
	{ "help",    no_argument,	NULL, 'h' },
	{ "version", no_argument,	NULL, 'v' },
	{ "device",  required_argument, NULL, 'd' },
	{ "backend", required_argument, NULL, 'b' },
	{ "count",   required_argument, NULL, '#' },
	{ 0,	     0,			0,    0	  }
};


struct cmdline {
	int count;
	std::string backend;
	std::string device;
	std::vector<std::string> argv;
};

struct command {
	const char* const name;
	void (*func)(const struct cmdline& cmdline, FILE* f);
};


static std::string get_default_backend(FILE* f)
{
	char buffer[128];
	ReplyParser replyParser;

	fputs("GET_DEFAULT_BACKEND\n", f);
	while (!feof(f)) {
		if (fgets(buffer, sizeof(buffer), f) == NULL)
			break;
		replyParser.feed(buffer);
		if (replyParser.is_completed())
			break;
	}
	if (replyParser.get_result() == ReplyParser::OK) {
		std::string s("");
		s += replyParser.get_data();
		s.erase(s.find_last_not_of(" \n\r\t") + 1);
		return s;
	} else {
	    fputs("Error running command\n", stderr);
	    return "";
	}
}

std::string get_backend(const struct cmdline& cmdline, FILE* f)
{
	std::string backend(cmdline.backend);
	if (backend == "") {
		backend = get_default_backend(f);
		if (backend == "") {
			fputs("Cannot get default backend\n", stderr);
			exit(EXIT_FAILURE);
		}
	}
	return backend;
}

ReplyParser get_reply(FILE* f)
{
	char buffer[128];
	ReplyParser parser;

	while (fgets(buffer, sizeof(buffer), f) != NULL) {
		parser.feed(buffer);
		if (parser.is_completed())
			break;
	}
	return parser;
}


void send_cmd(const struct cmdline& cmdline, FILE* f)
{
	if (cmdline.argv.size() < 2) {
		fputs("send: At least <remote> and <code> needed.\n", stderr);
		exit(EXIT_FAILURE);
	}

	std::string backend(get_backend(cmdline, f));
	std::ostringstream cmd;
	cmd << "SEND_ONCE " << backend << " ";
	for (size_t i = 0; i < cmdline.argv.size(); i += 1)
		cmd << cmdline.argv[i] << " ";
	cmd << cmdline.count << std::endl;
	fputs(cmd.str().c_str(), f);

	ReplyParser replyParser = get_reply(f);
	if (replyParser.get_result() != ReplyParser::OK) {
		fputs("Error running command\n", stderr);
		exit(EXIT_FAILURE);
	}
	exit(EXIT_SUCCESS);
}


void send_start_cmd(const struct cmdline& cmdline, FILE* f)
{
	if (cmdline.argv.size() != 2) {
		fputs("Usage: end-start <remote> <code>.\n", stderr);
		exit(EXIT_FAILURE);
	}
	std::string backend(get_backend(cmdline, f));
	std::ostringstream cmd;
	cmd << "SEND_START " << backend << " "
		<< cmdline.argv[0] << " "
		<< cmdline.argv[1] << std::endl;
	fputs(cmd.str().c_str(), f);

	ReplyParser replyParser = get_reply(f);
	if (replyParser.get_result() != ReplyParser::OK) {
		fputs("Error running command\n", stderr);
		exit(EXIT_FAILURE);
	}
	exit(EXIT_SUCCESS);
}


void send_stop_cmd(const struct cmdline& cmdline, FILE* f)
{
	if (cmdline.argv.size() != 2) {
		fputs("Usage: send-stop <remote> <code>.\n", stderr);
		exit(EXIT_FAILURE);
	}
	std::string backend(get_backend(cmdline, f));
	std::ostringstream cmd;
	cmd << "SEND_STOP " << backend << " "
		<< cmdline.argv[0] << " "
		<< cmdline.argv[1] << std::endl;
	fputs(cmd.str().c_str(), f);

	ReplyParser replyParser = get_reply(f);
	if (replyParser.get_result() != ReplyParser::OK) {
		fputs("Error running command\n", stderr);
		exit(EXIT_FAILURE);
	}
	exit(EXIT_SUCCESS);
}


void set_default_backend_cmd(const struct cmdline& cmdline, FILE* f)
{
	if (cmdline.argv.size() != 1) {
		fputs("Usage: set-default-backend <backend>\n", stderr);
		exit(EXIT_FAILURE);
	}

	std::ostringstream cmd;
	cmd << "SET_DEFAULT_BACKEND " << cmdline.argv[0] << std::endl;
	fputs(cmd.str().c_str(), f);

	ReplyParser parser = get_reply(f);
	if (parser.get_result() != ReplyParser::OK) {
		fprintf(stderr, "Error running command: %s\n",
			parser.get_data().c_str());
		exit(EXIT_FAILURE);
	}
}


void get_default_backend_cmd(const struct cmdline& cmdline, FILE* f)
{
	std::string backend = get_default_backend(f);
	if (backend == "") {
		fputs("Cannot retrieve default backend.\n", stderr);
		exit(EXIT_FAILURE);
	}
	puts(backend.c_str());
	exit(EXIT_SUCCESS);
}


void stop_backend_cmd(const struct cmdline& cmdline, FILE* f)
{
	if (cmdline.argv.size() != 0) {
		fputs("Usage: [-b backend] stop-backend\n", stderr);
		exit(EXIT_FAILURE);
	}
	std::string backend(get_backend(cmdline, f));
	std::ostringstream cmd;
	cmd << "STOP_BACKEND " << backend << std::endl;
	fputs(cmd.str().c_str(), f);
	ReplyParser parser = get_reply(f);
	if (parser.get_result() != ReplyParser::OK) {
		fprintf(stderr,
			"Error running command: %s\n",
			parser.get_data().c_str());
		exit(EXIT_FAILURE);
	}
	exit(EXIT_SUCCESS);
}


void list_backends_cmd(const struct cmdline& cmdline, FILE* f)
{
	if (cmdline.argv.size() != 0) {
		fputs("Usage: list-backends\n", stderr);
		exit(EXIT_FAILURE);
	}

	std::ostringstream cmd;
	cmd << "LIST_BACKENDS" << std::endl;
	fputs(cmd.str().c_str(), f);

	ReplyParser parser = get_reply(f);
	if (parser.get_result() != ReplyParser::OK) {
		fprintf(stderr, "Error running command: %s\n",
			parser.get_data().c_str());
		exit(EXIT_FAILURE);
	}
	fputs(parser.get_data().c_str(), stdout);
	exit(EXIT_SUCCESS);
}


void list_remotes_cmd(const struct cmdline& cmdline, FILE* f)
{
	if (cmdline.argv.size() != 0) {
		fputs("Usage: [-b backend] list-remotes\n", stderr);
		exit(EXIT_FAILURE);
	}

	std::ostringstream cmd;
	std::string backend(get_backend(cmdline, f));
	cmd << "LIST_REMOTES "  << backend << std::endl;
	fputs(cmd.str().c_str(), f);

	ReplyParser parser = get_reply(f);
	if (parser.get_result() != ReplyParser::OK) {
		fprintf(stderr, "Error running command: %s\n",
			parser.get_data().c_str());
		exit(EXIT_FAILURE);
	}
	fputs(parser.get_data().c_str(), stdout);
	exit(EXIT_SUCCESS);
}


void list_codes_cmd(const struct cmdline& cmdline, FILE* f)
{
	if (cmdline.argv.size() != 1) {
		fputs("Usage: [-b backend] list-codes <remote>\n", stderr);
		exit(EXIT_FAILURE);
	}
	std::string remote = cmdline.argv[0];
	remote.erase(remote.find_last_not_of(" \n\r\t") + 1);
	std::string backend(get_backend(cmdline, f));

	std::ostringstream cmd;
	cmd << "LIST_CODES " << backend << " " << remote << std::endl;
	fputs(cmd.str().c_str(), f);

	ReplyParser parser = get_reply(f);
	if (parser.get_result() != ReplyParser::OK) {
		fprintf(stderr, "Error running command: %s\n",
			parser.get_data().c_str());
		exit(EXIT_FAILURE);
	}
	fputs(parser.get_data().c_str(), stdout);
	exit(EXIT_SUCCESS);
}


void set_transmitters_cmd(const struct cmdline& cmdline, FILE* f)
{
	if (cmdline.argv.size() == 0) {
		fputs("Usage: [-b backend] set-transmitters <nr> [nr...]\n",
		      stderr);
		exit(EXIT_FAILURE);
	}
	std::string mask = cmdline.argv[0];
	mask.erase(mask.find_last_not_of(" \n\r\t") + 1);
	std::string backend(get_backend(cmdline, f));

	std::ostringstream cmd;
	cmd << "SET_TRANSMITTERS " << backend;
	for (size_t i = 0; i < cmdline.argv.size(); i += 1)
		cmd << " " << cmdline.argv[i];
	cmd << std::endl;
	fputs(cmd.str().c_str(), f);

	ReplyParser parser = get_reply(f);
	if (parser.get_result() != ReplyParser::OK) {
		fprintf(stderr, "Error running command: %s\n",
			parser.get_data().c_str());
		exit(EXIT_FAILURE);
	}
	exit(EXIT_SUCCESS);
}


void simulate_cmd(const struct cmdline& cmdline, FILE* f)
{
	if (cmdline.argv.size() < 2 || cmdline.argv.size() > 3) {
		fputs("Usage: simulate <remote> <code> [scancode]\n", stderr);
		exit(EXIT_FAILURE);
	}
	std::string backend(get_backend(cmdline, f));
	std::ostringstream cmd;
	cmd << "SIMULATE " << backend << " "
		<< cmdline.argv[0] << " "
		<< cmdline.argv[1] << " "
		<< cmdline.count << " ";
	cmd << (cmdline.argv.size() == 3 ? cmdline.argv[2] : "0");
	cmd << std::endl;
	fputs(cmd.str().c_str(), f);

	ReplyParser parser = get_reply(f);
	if (parser.get_result() != ReplyParser::OK) {
		fprintf(stderr, "Error running command: %s\n",
			parser.get_data().c_str());
		exit(EXIT_FAILURE);
	}
	exit(EXIT_SUCCESS);
}


const struct command commands[] = {
	{ "send",			send_cmd },
	{ "send-start",			send_start_cmd },
	{ "send-stop",			send_stop_cmd },
	{ "set-default-backend",	set_default_backend_cmd },
	{ "get-default-backend",	get_default_backend_cmd },
	{ "stop-backend",		stop_backend_cmd },
	{ "list-backends",		list_backends_cmd },
	{ "list-remotes",		list_remotes_cmd },
	{ "list-codes", 		list_codes_cmd },
	{ "set-transmitters",		set_transmitters_cmd },
	{ "simulate",			simulate_cmd },
	{ 0,				0 }
};


void get_commandline(int argc, char** argv, struct cmdline* cmdline)
{
	cmdline->count = 1;
	cmdline->backend = "";
	cmdline->device = std::string(LIRCD) + ".control";
	int c;

	while (1) {
		c = getopt_long(argc, argv, "hvd:b:#:", long_options, NULL);
		if (c == -1)
			break;
		switch (c) {
		case 'h':
			fputs(help, stdout);
			exit(EXIT_SUCCESS);
		case 'v':
			printf("irtool %s\n", VERSION);
			exit(EXIT_SUCCESS);
		case 'd':
			cmdline->device = std::string(optarg);
			break;
		case 'b':
			cmdline->backend = std::string(optarg);
			break;
		case '#':
			char* end;
			cmdline->count = strtoul(optarg, &end, 10);
			if (!*optarg || *end) {
				fprintf(stderr,
					"irtool: invalid count value: %s\n",
					optarg);
				exit(EXIT_FAILURE);
			}
			break;
		default:
			fputs("Illegal command!\n", stderr);
			exit(EXIT_FAILURE);
		}
	}
	while (optind < argc) {
		cmdline->argv.push_back(argv[optind]);
		optind += 1;
	}
}


int main(int argc, char** argv)
{
	int fd;
	FILE* f;
	struct cmdline cmdline;
	const struct command* next_command;
	bool done = false;

	get_commandline(argc, argv, &cmdline);
	if (cmdline.argv.size() == 0) {
		fprintf(stderr, "Not enough arguments\n");
		return EXIT_FAILURE;
	}
	fd = lirc_get_local_socket(cmdline.device.c_str(), 0);
	if (fd < 0) {
		perrorf("Cannot open socket %s", cmdline.device.c_str());
		exit(EXIT_FAILURE);
	}
	f = fdopen(fd, "w+");
	if (f == NULL) {
		log_perror_err("Cannot fdopen control socket.");
		perrorf("Cannot fdopen socket on %s", cmdline.device.c_str());
		exit(EXIT_FAILURE);
	}
	next_command = reinterpret_cast<const struct command*>(&commands);
	while (next_command->name != 0) {
		if (cmdline.argv[0] == next_command->name) {
			cmdline.argv.erase(cmdline.argv.begin());
			next_command->func(cmdline, f);
			done = true;
			break;
		}
		next_command += 1;
	}
	if (!done) {
		fprintf(stderr,
			"Illegal command: %s (use --help for more info)\n",
			cmdline.argv[0].c_str());
		exit(EXIT_FAILURE);
	}
	close(fd);
	return EXIT_SUCCESS;
}
