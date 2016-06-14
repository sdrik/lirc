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
 * @file lircd_options.h
 * Parses command line and config file, builds the options struct.
 */

#ifndef DAEMONS_LIRCD_OPTIONS_H_
#define DAEMONS_LIRCD_OPTIONS_H_

struct options_t {
	int		nodaemon;
	int		allow_simulate;
	const char*	client_socket_path;
	const char*	ctrl_socket_path;
	const char*	backend_socket_path;
	const char*	pidfile_path;
	int		client_socket_permissions;
	loglevel_t	loglevel;
	const char*	logfile;
};


const struct options_t* const get_options(int argc, char** argv);

#endif  // DAEMONS_LIRCD_OPTIONS_H_
