/*****************************************************************
* fd_list  *******************************************************
******************************************************************
*
* FdList - Book-keeping data for open sockets.
*
* Copyright (c) 2015 Alec Leamas
*
*/

/**
 * @file fd_list.cpp
 * Implements a container with data for each open file descriptor.
 */


#include <poll.h>
#include <sys/types.h>

#include <vector>

#include "fd_list.h"
#include "reply_parser.h"

static void init(FdItem* item)
{
	item->fd = -1;
	item->kind = FdItem::UNDEFINED;
	item->pid = 0;
	item->peer = -1;
	item->connected_to = -1;
	item->replyParser = new ReplyParser();
	item->id = "undef";
	item->expected = "NONE";
	item->ticks = -1;
}


FdItem::FdItem()
{
	init(this);
}


FdItem::FdItem(int fd, fd_kind kind, int pid)
{
	init(this);
	this->fd = fd;
	this->kind = kind;
	this->pid = pid;
}


FdList::FdList(int client_fd, int backend_fd, int ctrl_fd)
{
	FdItem client_item(client_fd, FdItem::CLIENT);
	fd_list.push_back(client_item);

	FdItem backend_item(backend_fd, FdItem::BACKEND);
	fd_list.push_back(backend_item);

	FdItem ctrl_item(ctrl_fd, FdItem::CTRL);
	fd_list.push_back(ctrl_item);
}


static bool item_fd_equals(const FdItem& item, int fd)
{
	return item.fd == fd;
}


ItemIterator FdList::find(int what, bool (*cond)(const FdItem&, int))
{
	ItemIterator it;

	for (it = fd_list.begin(); it != fd_list.end(); it += 1) {
		if (cond((*it), what))
			return it;
	}
	return it;
}


ItemIterator FdList::find(const char* what,
			  bool (*cond)(const FdItem&, const char*))
{
	ItemIterator it;

	for (it = fd_list.begin(); it != fd_list.end(); it += 1) {
		if (cond((*it), what))
			return it;
	}
	return it;
}


ItemIterator FdList::find_fd(int fd)
{
	return find(fd, item_fd_equals);
}


void FdList::add_backend(int cmd_fd, int data_fd)
{
	FdItem data_item(data_fd, FdItem::BACKEND_DATA);
	data_item.peer = cmd_fd;
	fd_list.push_back(data_item);

	FdItem cmd_item(cmd_fd, FdItem::BACKEND_CMD);
	cmd_item.peer = data_fd;
	fd_list.push_back(cmd_item);
}


void FdList::add_client(int client_fd)
{
	FdItem client_item(client_fd, FdItem::CLIENT_STREAM);
	fd_list.push_back(client_item);
}


ItemIterator FdList::remove_client(int fd)
{
	ItemIterator it = find_fd(fd);
	if (it == fd_list.end())
		return it;
	return fd_list.erase(it);
}


void FdList::add_ctrl_client(int client_fd)
{
	FdItem client_item(client_fd, FdItem::CTRL_STREAM);
	fd_list.push_back(client_item);
}


/** Remove fd and possible related peer. Returns FdList.end() if not found. */
ItemIterator FdList::remove_backend(int fd)
{
	ItemIterator it = find_fd(fd);
	if (it == fd_list.end())
		return it;
	if ((*it).peer != -1) {
		ItemIterator pi = find_fd((*it).peer);
		if (pi == fd_list.end())
			return pi;
		fd_list.erase(pi);
	}
	fd_list.erase(it);
	return fd_list.begin();
}


void FdList::get_pollfds(std::vector<FdItem>* items,
			 std::vector<struct pollfd>* pollfds)
{
	items->clear();
	pollfds->clear();
	items->resize(fd_list.size());
	pollfds->resize(fd_list.size());

	for (size_t i = 0; i < fd_list.size(); i += 1) {
		(*pollfds)[i].fd = fd_list[i].fd;
		(*pollfds)[i].events = POLLIN;
		(*pollfds)[i].revents = 0;
		(*items)[i] = fd_list[i];
	}
}
