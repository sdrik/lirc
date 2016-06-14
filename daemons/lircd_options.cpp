/***********************************************************************
** lircd_options *******************************************************
************************************************************************
*
* lircd options - parse command line and config files, build options struct.
*
* Copyright (c) 2015 Alec Leamas.
*
*/

/**
 * @file lircd_options.cpp
 * Parses command line and config file, builds the options struct.
 */

#include   <unistd.h>
#include   <getopt.h>

#include   "paths.h"
#include   "lirc_config.h"
#include   "config.h"
#include   "lirc_private.h"

#include   "lircd_options.h"

static const char* const help =
	"Usage: lircd [options] <config-file>\n"
	"\t -h --help\t\t\tDisplay this message\n"
	"\t -v --version\t\t\tDisplay version\n"
	"\t -O --options-file\t\tOptions file\n"
	"\t -n --nodaemon\t\t\tDon't fork to background\n"
	"\t -p --permission=mode\t\tFile permissions for " LIRCD "\n"
	"\t -o --output=socket\t\tOutput socket filename\n"
	"\t -P --pidfile=file\t\tDaemon pid file\n"
	"\t -L --logfile=file\t\tLog file path (default: use syslog)'\n"
	"\t -D[level] --loglevel[=level]\t'info', "
				"'warning', 'notice', etc., or 3..10.\n"
	"\t -a --allow-simulate\t\tAccept SIMULATE command\n";


static const struct option lircd_options[] = {
	{ "help",	    no_argument,       NULL, 'h' },
	{ "version",	    no_argument,       NULL, 'v' },
	{ "nodaemon",	    no_argument,       NULL, 'n' },
	{ "options-file",   required_argument, NULL, 'O' },
	{ "permission",	    required_argument, NULL, 'p' },
	{ "output",	    required_argument, NULL, 'o' },
	{ "pidfile",	    required_argument, NULL, 'P' },
	{ "logfile",	    required_argument, NULL, 'L' },
	{ "loglevel",	    optional_argument, NULL, 'D' },
	{ "allow-simulate", no_argument,       NULL, 'a' },
	{ 0,		    0,		       0,    0	 }
};

#define DEBUG_HELP "Bad debug level: \"%s\"\n\n" \
	"Level could be ERROR, WARNING, NOTICE, INFO, DEBUG, TRACE, TRACE1,\n" \
	" TRACE2 or a number in the range 3..10.\n"

const char* const ARG_HELP =
	"lircd: invalid argument count\n"
	"lircd: lircd does not use a confile file. However, backends do.\n";

/* cut'n'paste from fileutils-3.16: */
#define isodigit(c) ((c) >= '0' && (c) <= '7')


static int oatoi(const char* s)
{
	register int i;

	if (*s == 0)
		return -1;
	for (i = 0; isodigit(*s); ++s)
		i = i * 8 + *s - '0';
	if (*s)
		return -1;
	return i;
}


static void lircd_add_defaults(void)
{
	char level[4];

	snprintf(level, sizeof(level), "%d", lirc_log_defaultlevel());

	const char* const defaults[] = {
		"lircd:nodaemon",	"False",
		"lircd:permission",	DEFAULT_PERMISSIONS,
		"lircd:output",		LIRCD,
		"lircd:pidfile",	PIDFILE,
		"lircd:logfile",	"syslog",
		"lircd:debug",		level,
		"lircd:allow-simulate",	"False",
		(const char*)NULL,	(const char*)NULL
	};
	options_add_defaults(defaults);
}


static void lircd_parse_options(int argc, char** const argv)
{
	int c;
	loglevel_t loglevel_opt;
	const char* opt;

	const char* optstring = "A:e:O:hvnpi:H:d:o:U:P:l::L:c:r::aR:D::Y"
#       if defined(__linux__)
				"u"
#       endif
	;							// NOLINT

	strncpy(progname, "lircd", sizeof(progname));
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
		case 'O':
			break;
		case 'n':
			options_set_opt("lircd:nodaemon", "True");
			break;
		case 'p':
			if (oatoi(optarg) == -1) {
				fprintf(stderr,
					"lircd: Invalid mode %s\n", optarg);
				fputs("lircd: Falling back to 666", stderr);
				options_set_opt("lircd:permission", "666");
			} else {
				options_set_opt("lircd:permission", optarg);
			}
			break;
		case 'P':
			options_set_opt("lircd:pidfile", optarg);
			break;
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
		case 'a':
			options_set_opt("lircd:allow-simulate", "True");
			break;
		default:
			printf("Usage: %s [options] [config-file]\n", progname);
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


const struct options_t* const get_options(int argc, char** argv)
{
	char buff[128];

	static struct options_t options = { 0 };   // FIXME - classify
	options_load(argc, argv, NULL, lircd_parse_options);
	options.logfile = options_getstring("lircd:logfile");
	options.nodaemon = options_getboolean("lircd:nodaemon");
	options.client_socket_path = options_getstring("lircd:output");
	snprintf(buff, sizeof(buff),
		 "%s.control", options.client_socket_path);
	options.ctrl_socket_path = strdup(buff);
	snprintf(buff, sizeof(buff),
		 "%s.backend", options.client_socket_path);
	options.backend_socket_path = strdup(buff);
	options.client_socket_permissions =
		oatoi(options_getstring("lircd:permission"));
	options.pidfile_path = options_getstring("lircd:pidfile");
	options.loglevel = (loglevel_t) options_getint("lircd:debug");
	options.allow_simulate = options_getboolean("lircd:allow-simulate");
	return (const options_t* const) &options;
}
