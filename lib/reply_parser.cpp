/****************************************************************************
** reply_parser *******************************************************
****************************************************************************
*
* lircd reply parser - parse command replies on socket.
*
* Copyright (c) 2015 Alec Leamas
*
* */

/**
 * @file reply_parser.cpp
 * This file implements the network command parser.
 */

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>


#include "lirc_log.h"
#include "reply_parser.h"


#ifndef PACKET_SIZE
#define PACKET_SIZE 256
#endif
#define WHITE_SPACE " \t"

static const logchannel_t logchannel = LOG_DISPATCH;

const int MAX_TICKS = 20;

void ReplyParser::reset()
{
	state = BEGIN;
	command = "";
	lines == "";
	last_line = "";
	success = false;
}


ReplyParser::ReplyParser()
{
	reset();
}


void ReplyParser::feed(const char* line)
{
	std::string input(line);
	size_t endpos = input.find_last_not_of(" \t\n\r");
	if (std::string::npos != endpos)
		input = input.substr(0, endpos + 1);
	last_line = input;
	switch (state) {		// FIXME: Convert to uppercase
		case BEGIN:
			state = input == "BEGIN" ? COMMAND : BAD_DATA;
			break;
		case COMMAND:
			if (input != "") {
				state = RESULT;
				command = input;
			} else {
				state = BAD_DATA;
			}
			break;
		case RESULT:
			if (input == "SUCCESS" || input == "ERROR") {
				state = DATA;
				success = input == "SUCCESS";
			} else {
				state = BAD_DATA;
			}
			break;
		case DATA:
			if (input == "DATA")
				state = LINE_COUNT;
			else
				state = input == "END" ? DONE : BAD_DATA;
			break;
		case LINE_COUNT:
			if (sscanf(input.c_str(), "%2d", &line_count) != 1)
				state = BAD_DATA;
			else
				state = LINES;
			break;
		case LINES:
			line_count -= 1;
			if (input != "")
				lines += input + "\n";
			else
				state = BAD_DATA;
			if (line_count <= 0)
				state = END;
			break;
		case END:
			state = input == "END" ? DONE : BAD_DATA;
			break;
		case DONE:
		case NO_DATA:
		case BAD_DATA:
			log_warn("ReplyParser: skipping data: %s", input);
			break;
	}
	if (state == BAD_DATA)
		log_warn("ReplyParser: Bad input: %s", input);
}


bool ReplyParser::is_completed()
{
	return state == DONE || state == NO_DATA || state == BAD_DATA;
}


enum ReplyParser::Result ReplyParser::get_result()
{
	switch (state) {
		case DONE:
			return success ? OK : FAIL;
		case BAD_DATA:
			return CANT_PARSE;
		case NO_DATA:
			return TIMEOUT;
		default:
			return INCOMPLETE;
	}
}
