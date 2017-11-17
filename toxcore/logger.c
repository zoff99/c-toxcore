/*
 * Text logging abstraction.
 */

/*
 * Copyright © 2016-2017 The TokTok team.
 * Copyright © 2013,2015 Tox project.
 *
 * This file is part of Tox, the free peer to peer instant messenger.
 *
 * Tox is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Tox is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Tox.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "logger.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>


struct Logger {
    logger_cb *callback;
    void *context;
    void *userdata;
};


/**
 * Public Functions
 */
Logger *logger_new()
{
    return (Logger *)calloc(1, sizeof(Logger));
}

void logger_kill(Logger *log)
{
    free(log);
}

void logger_callback_log(Logger *log, logger_cb *function, void *context, void *userdata)
{
    log->callback = function;
    log->context  = context;
    log->userdata = userdata;
}

void logger_write(Logger *log, LOGGER_LEVEL level, const char *file, int line, const char *func, const char *format,
                  ...)
{
    if (!log || !log->callback) {
        return;
    }

    /* Format message */
    char msg[LOGGER_MAX_MSG_LENGTH];
    va_list args;
    va_start(args, format);
    vsnprintf(msg, sizeof msg, format, args);
    va_end(args);

    log->callback(log->context, level, file, line, func, msg, log->userdata);
}

char *logger_dumphex(const void* data, size_t size) {
	char ascii[17];
	size_t i, j;
	ascii[16] = '\0';

	long dump_size = 0;
	printf("logger_dumphex:001:size=%d\n", (int)size);

	for (i = 0; i < size; ++i) {
		dump_size = dump_size + 3;
		if (((unsigned char*)data)[i] >= ' ' && ((unsigned char*)data)[i] <= '~') {
			ascii[i % 16] = ((unsigned char*)data)[i];
		} else {
			ascii[i % 16] = '.';
		}

		if ((i+1) % 8 == 0 || i+1 == size) {
			dump_size = dump_size + 1;
			if ((i+1) % 16 == 0) {
				// printf("|  %s \n", ascii);
				dump_size = dump_size + 4 + 17 + 1;
			} else if (i+1 == size) {
				ascii[(i+1) % 16] = '\0';
				if ((i+1) % 16 <= 8) {
					dump_size = dump_size + 1;
				}
				for (j = (i+1) % 16; j < 16; ++j) {
					dump_size = dump_size + 3;
				}
				// printf("|  %s \n", ascii);
				dump_size = dump_size + 4 + 17 + 1;
			}
		}
	}

	printf("logger_dumphex:002:size=%d:dump_size=%d\n", (int)size, (int)dump_size);

	char *log_msg = calloc(1, (size_t)(dump_size * 2));
	char *log_msg_orig = log_msg;

	for (i = 0; i < size; ++i) {
		log_msg += sprintf(log_msg, "%02X ", ((unsigned char*)data)[i]);
		if (((unsigned char*)data)[i] >= ' ' && ((unsigned char*)data)[i] <= '~') {
			ascii[i % 16] = ((unsigned char*)data)[i];
		} else {
			ascii[i % 16] = '.';
		}
		if ((i+1) % 8 == 0 || i+1 == size) {
			log_msg += sprintf(log_msg, " ");
			if ((i+1) % 16 == 0) {
				log_msg += sprintf(log_msg, "|  %s \n", ascii);
			} else if (i+1 == size) {
				ascii[(i+1) % 16] = '\0';
				if ((i+1) % 16 <= 8) {
					log_msg += sprintf(log_msg, " ");
				}
				for (j = (i+1) % 16; j < 16; ++j) {
					log_msg += sprintf(log_msg, "   ");
				}
				log_msg += sprintf(log_msg, "|  %s \n", ascii);
			}
		}
	}

	printf("logger_dumphex:003:size=%d\n", (int)size);

	return log_msg_orig;
}
