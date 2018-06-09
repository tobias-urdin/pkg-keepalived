/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        logging facility.
 *
 * Author:      Alexandre Cassen, <acassen@linux-vs.org>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Copyright (C) 2001-2017 Alexandre Cassen, <acassen@gmail.com>
 */

#include "config.h"

#include <stdio.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <memory.h>

#include "logger.h"
#include "bitops.h"
#include "utils.h"

/* Boolean flag - send messages to console as well as syslog */
static bool log_console = false;

/* File to write log messages to */
char *log_file_name;
static FILE *log_file;
bool always_flush_log_file;

void
enable_console_log(void)
{
	log_console = true;
}

void
set_flush_log_file(void)
{
	always_flush_log_file = true;
}

void
close_log_file(void)
{
	if (log_file) {
		fclose(log_file);
		log_file = NULL;
	}
}

void
open_log_file(const char *name, const char *prog, const char *namespace, const char *instance)
{
	const char *extn_start;
	const char *dir_end;
	size_t len;
	char *file_name;

	if (log_file) {
		fclose(log_file);
		log_file = NULL;
	}

	if (!name)
		return;

	len = strlen(name);
	if (prog)
		len += strlen(prog) + 1;
	if (namespace)
		len += strlen(namespace) + 1;
	if (instance)
		len += strlen(instance);

	file_name = MALLOC(len + 1);
	dir_end = strrchr(name, '/');
	extn_start = strrchr(dir_end ? dir_end : name, '.');
	strncpy(file_name, name, extn_start ? (size_t)(extn_start - name) : len);

	if (prog) {
		strcat(file_name, "_");
		strcat(file_name, prog);
	}
	if (namespace) {
		strcat(file_name, "_");
		strcat(file_name, namespace);
	}
	if (instance) {
		strcat(file_name, "_");
		strcat(file_name, instance);
	}
	if (extn_start)
		strcat(file_name, extn_start);

	log_file = fopen(file_name, "a");
	fcntl(fileno(log_file), F_SETFD, FD_CLOEXEC | fcntl(fileno(log_file), F_GETFD));
	fcntl(fileno(log_file), F_SETFL, O_NONBLOCK | fcntl(fileno(log_file), F_GETFL));

	FREE(file_name);
}

void
flush_log_file(void)
{
	if (log_file)
		fflush(log_file);
}

void
vlog_message(const int facility, const char* format, va_list args)
{
	char buf[MAX_LOG_MSG+1];

	vsnprintf(buf, sizeof(buf), format, args);

	if (log_file || (__test_bit(DONT_FORK_BIT, &debug) && log_console)) {
		/* timestamp setup */
		time_t t = time(NULL);
		struct tm tm;
		localtime_r(&t, &tm);
		char timestamp[64];
		strftime(timestamp, sizeof(timestamp), "%c", &tm);

		if (log_console && __test_bit(DONT_FORK_BIT, &debug))
			fprintf(stderr, "%s: %s\n", timestamp, buf);
		if (log_file) {
			fprintf(log_file, "%s: %s\n", timestamp, buf);
			if (always_flush_log_file)
				fflush(log_file);
		}
	}

	if (!__test_bit(NO_SYSLOG_BIT, &debug))
		syslog(facility, "%s", buf);
}

void
log_message(const int facility, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vlog_message(facility, format, args);
	va_end(args);
}

void
conf_write(FILE *fp, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	if (fp) {
		vfprintf(fp, format, args);
		fprintf(fp, "\n");
	}
	else
		vlog_message(LOG_INFO, format, args);

	va_end(args);
}
