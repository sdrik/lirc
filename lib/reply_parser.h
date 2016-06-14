/****************************************************************************
** reply_parser *******************************************************
****************************************************************************
*
* lircd command reply parser - parse command replies.
*
* Copyright (c) 2015 Alec Leamas
*
* */

/**
 * @file command_parser.cpp
 * This file implements the network command parser.
 */

#ifndef LIB_REPLY_PARSER_H_
#define LIB_REPLY_PARSER_H_

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <string>

#include "lirc_private.h"


extern const int MAX_TICKS;   // ReplyParser timeout.

class ReplyParser {
	private:
		enum states { BEGIN,
			      COMMAND,
			      RESULT,
			      DATA,
			      LINE_COUNT,
			      LINES,
			      END,
			      DONE,
			      NO_DATA,
			      BAD_DATA};

		enum states state;
		std::string command;
		std::string lines;
		std::string last_line;
		int line_count;
		bool success;

	public:
		ReplyParser();

		enum Result {OK, FAIL, CANT_PARSE, TIMEOUT, INCOMPLETE };

		enum Result get_result();

		/** Enter a line of data into parsing FSM. */
		void feed(const char* line);

		/** Reflects if parser needs more data to complete */
		bool is_completed();

		/** Reset to pristine state. */
		void reset();

		/** Reply command part, defined if is_completed() == true. */
		const std::string& get_command() { return command; }

		/** Data part of reply, defined if is_completed() == true. */
		const std::string& get_data() { return lines; }

		/** SUCCESS/ERROR part, defined if is_completed() == true. */
		bool get_success() { return success; }

		/** Last line of input. */
		const std::string& get_last_line() { return last_line; }
};

#endif  // LIB_REPLY_PARSER_H_
