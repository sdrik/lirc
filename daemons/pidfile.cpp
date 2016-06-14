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
 * @file pidfile.cpp
 * Implements a unique process instance lock using a regular pidfile.
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>


#include "lirc_log.h"
#include "pidfile.h"


static const logchannel_t logchannel = LOG_LIB;

Pidfile::Pidfile(const char* path)
{
	strncpy(this->path, path, sizeof(this->path) - 1);
	other_pid = -1;
}



Pidfile::lock_result Pidfile::lock()
{
	int fd;


	fd = open(path, O_RDWR | O_CREAT, 0644);
	if (fd > 0)
		f = fdopen(fd, "r+");
	if (fd == -1 || f == NULL)
		return CANT_CREATE;
	if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
		if (fscanf(f, "%d\n", &other_pid) > 0)
			return LOCKED_BY_OTHER;
		else
			return CANT_PARSE;
	}
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
	rewind(f);
	(void)fprintf(f, "%d\n", getpid());
	(void)fflush(f);
	if (ftruncate(fileno(f), ftell(f)) != 0)
		log_perror_warn("lircd: ftruncate()");
	return OK;
}


void Pidfile::close()
{
	fclose(f);
	(void)unlink(path);
}


void Pidfile::update(pid_t pid)
{
	rewind(f);
	fprintf(f, "%d\n", pid);
	fflush(f);
	if (ftruncate(fileno(f), ftell(f)) != 0)
		log_perror_warn("lircd: ftruncate()");
}
