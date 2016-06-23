/*****************************************************************
** pidfile *******************************************************
******************************************************************
*
* pidfile - Unique process instance lock using a file.
*
* Copyright (c) 2015 Alec Leamas.
*
*/

/**
 * @file pidfile.h
 * @brief Implements a unique process instance lock using a regular pidfile.
 * @ingroup private_api
 */

#ifndef DAEMONS_PIDFILE_H_
#define DAEMONS_PIDFILE_H_

#include <sys/types.h>


/** A classic pidfile, ensures there is only one instance. */
class Pidfile {
	private:
		FILE* f = NULL;
		char path[256];

	public:
		enum lock_result {
		    OK = 0,
		    CANT_CREATE,
		    LOCKED_BY_OTHER,
		    CANT_PARSE
		};

		static Pidfile* instance();

		Pidfile() {}

		/** Create and lock the pidfile, updates otherpid if busy. */
		lock_result lock(const char* path);

		/** Release the lock and remove file. */
		void close();

		/** Other pid holding lock when result == LOCKED_BY_OTHER. */
		pid_t other_pid;

		/** Update the pid written in pidfile, keeping the lock. */
		void update(pid_t pid);
};

#endif  //  DAEMONS_PIDFILE_H_
