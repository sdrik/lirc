/*****************************************************************
* fd_list  *******************************************************
******************************************************************
*
* FdList - Book-keeping data for open sockets.
*
* Copyright (c) 2015 Alec Leamas
*
*/


#ifndef DAEMONS_FD_LIST_H_
#define DAEMONS_FD_LIST_H_

/**
 * @file fd_list.h
 * @brief Data for all sockets
 * @ingroup private_api
 */


#include <vector>
#include <string>

#include "line_buffer.h"
#include "reply_parser.h"


/**
 * Housekeeping data for each socket.  Sockets are related with connections
 * and peers. A client socket is connected to a backend while processing a
 * command. This is reflected in the connected_to field in both sockets.
 * Backend sockets are always connected in CMD/DATA pairs; this is
 * reflected in the peer field. All these uses -1 for no connection,
 * 0 for the local, internal lircd client and values > 0 for a
 * connected fd.
 *
 * The timeout counter is armed by setting it > 0; it's decremented in each
 * heartbeat tick, triggering interrupt when becoming 0.
 */

class FdItem {
	public:
		/** Type of sockets tracked. */
		enum fd_kind {
			CLIENT,		/**< Where clients connects. */
			BACKEND,	/**< Where backends connects.*/
			CTRL,		/**< Where control apps connects. */
			BACKEND_DATA,	/**< Decoded backend events. */
			BACKEND_CMD,	/**< Commands to/from from backend. */
			CLIENT_STREAM,	/**< Command/data to/from client. */
			CTRL_STREAM,	/**< Commands to/from ctrl sock. */
			UNDEFINED
		};

		fd_kind kind;
		int	     fd;
		int	     connected_to;   /**< Command connection, or -1*/
		int 	     peer;           /**< Backend DATA/CMD relation. */
		int	     pid;    	     /**< The backend pid, or -1. */
		std::string  id;             /**< Backend id: driver@device. */
		ReplyParser* replyParser;
		std::string  expected;       /**< Expected backend command. */
		int	     ticks;          /**< Timeout counter. */
		LineBuffer   lineBuffer;     /**< Input line buffering. */

		FdItem(int fd, fd_kind kind, pid_t pid = -1);
		FdItem();
};

typedef std::vector<FdItem> FdItemVector;

typedef FdItemVector::iterator FdItemIterator;

typedef bool (*fd_int_predicate)(const FdItem&, int);
typedef bool (*fd_str_predicate)(const FdItem&, const char*);

/**
 *
 * The socket list. The first three items are the well-known addresses where
 * clients and backends connects. The rest is dynamically created sockets.
 * Data structures are designed to make get_poll_fds() fast; other operations
 * are potentially slow.
 *
 */
class FdList {
	protected:
		std::vector<FdItem> fd_list;

	public:
		FdList(int client_fd, int backend_fd, int ctrl_fd);

		void add_backend(int cmd_fd, int data_fd);

		/**
		 * Remove fd and possible related peer. Returns
		 * FdList.end() if not found.
		 */
		FdItemIterator remove_backend(int fd);

		void add_client(int client_fd);
		void add_ctrl_client(int client_fd);
		FdItemIterator remove_client(int fd);

		int size() { return fd_list.size(); }
		FdItemIterator begin() { return fd_list.begin(); }
		FdItemIterator end() { return fd_list.end(); }
		FdItem& item_at(int i) { return fd_list[i]; }

		int client_socket() { return fd_list[0].fd; }
		int backend_socket() { return fd_list[1].fd; }
		int ctrl_socket() { return fd_list[2].fd; }

		FdItemIterator find_fd(int fd);
		FdItemIterator find(int what, fd_int_predicate);
		FdItemIterator find(const char* what, fd_str_predicate);
		std::vector<int> select_fds(int what, fd_int_predicate);

		/** Return snapshot of current items + corresponding pollfd */
		void get_pollfds(std::vector<FdItem>* items,
				 std::vector<struct pollfd>* pollfds);
};

#endif  // DAEMONS_FD_LIST_H_
