/*
 * config.c - read the config file for the gofish gopher daemon
 * Copyright (C) 2000,2002  Sean MacLennan <seanm@seanm.ca>
 * $Revision: 1.3 $ $Date: 2002/08/25 01:48:32 $
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this project; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <assert.h>
#include <syslog.h>
#include <signal.h>
#include <errno.h>
#include <sys/time.h>

#include "gopherd.h"


char *root_dir = GOPHER_ROOT;
char *logfile = GOPHER_LOGFILE;
char *pidfile = GOPHER_PIDFILE;
char *hostname = GOPHER_HOST;
int   port = GOPHER_PORT;
uid_t uid = GOPHER_UID;
gid_t gid = GOPHER_GID;
int   ignore_local = IGNORE_LOCAL;


// If we are already out of memory, we are in real trouble
char *must_strdup(char *str)
{
	char *new = strdup(str);
	if(!new) {
		syslog(LOG_ERR, "read_config: out of memory");
		exit(1);
	}
	return new;
}


// only set if a number specified
void must_strtol(char *str, int *value)
{
	char *end;
	long n = strtol(str, &end, 0);
	if(str != end)
		*value = (int)n;
}


int read_config(char *fname)
{
	FILE *fp;
	char line[100], *s, *p;

	if((fp = fopen(fname, "r")) == NULL) {
		if(errno == ENOENT && strcmp(fname, GOPHER_CONFIG) == 0)
			return 0;
		syslog(LOG_WARNING, "%s: %m", fname);
		return 1;
	}

	while(fgets(line, sizeof(line), fp)) {
		if(!isalpha(*line)) continue;

		for(p = line + strlen(line); isspace(*(p - 1)); --p) ;
		*p = '\0';

		if((p = strchr(line, '=')) == NULL) {
			printf("Bad line '%s'\n", line);
			continue;
		}
		s = p++;

		while(isspace(*(s - 1))) --s;
		*s++ = '\0';

		while(isspace(*p)) ++p;
		if(*p == '\0') {
			printf("No value for '%s'\n", line);
			continue;
		}

		if(strcmp(line, "root") == 0)
			root_dir = must_strdup(p);
		else if(strcmp(line, "logfile") == 0)
			logfile = must_strdup(p);
		else if(strcmp(line, "pidfile") == 0)
			pidfile = must_strdup(p);
		else if(strcmp(line, "port") == 0)
			must_strtol(p, &port);
		else if(strcmp(line, "uid") == 0)
			must_strtol(p, (int*)&uid);
		else if(strcmp(line, "gid") == 0)
			must_strtol(p, (int*)&gid);
		else if(strcmp(line, "no_local") == 0)
			must_strtol(p, &ignore_local);
		else if(strcmp(line, "host") == 0)
			hostname = must_strdup(p);
		else
			printf("Unknown config '%s'\n", line);
	}

	fclose(fp);

	// Make sure hostname is set to something
	// Make sure it is malloced
	if(*hostname == '\0') {
		if(gethostname(line, sizeof(line) - 1)) {
			puts("Warning: setting hostname to localhost.\n"
				 "This is probably not what you want.");
			strcpy(line, "localhost");
		}
		if((hostname = strdup(line)) == NULL) {
			printf("Out of memory\n");
			return 1;
		}
	}

	return 0;
}
