/*
 * log.c - log file output for the gofish gopher daemon
 * Copyright (C) 2002 Sean MacLennan <seanm@seanm.ca>
 * $Revision: 1.7 $ $Date: 2002/09/27 03:16:50 $
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this project; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <netinet/in.h>
#include <errno.h>
#include "gopherd.h"


static FILE *fp;

int log_open(char *logname)
{
	return (fp = fopen(logname, "a")) != NULL;
}


// Common log file format
void log_hit(struct connection *conn, unsigned status)
{
	extern int ignore_local;
	char date[40], *name;
	time_t now;
	struct tm *t;


	if(ignore_local &&
	   (conn->addr & 0xffff0000) == 0xc0a80000 ||
		conn->addr == 0x7f000001) return;

	time(&now);
	t = localtime(&now);

	strftime(date, sizeof(date), "%d/%b/%Y:%T %z", t);

#ifdef USE_HTTP
	if(conn->http) {
		// http request
		name = conn->cmd + 4;
		while(*name == '/') ++name;
		strcat(name, " HTTP");
		conn->http = 0;
	}
	else
#endif
		name = conn->cmd ? (*conn->cmd ? conn->cmd : "/") : "[Empty]";


	// This is 400 chars max
	fprintf(fp, "%s - - [%s] \"GET %.300s\" %u %u\n",
			ntoa(conn->addr), date, name, status, conn->len);

	(void)fflush(fp);
}


void log_close()
{
	(void)fclose(fp);
}


static void send_errno(int sock, char *name, int errnum)
{
	char error[1024];

	if(*name == '\0')
		sprintf(error, "3'<root>' %.500s\r\n",
				strerror(errnum), errnum);
	else
		sprintf(error, "3'%.500s' %.500s\r\n",
				name, strerror(errnum), errnum);
	write(sock, error, strlen(error));
}


static struct errmsg {
	unsigned errnum;
	char *errstr;
} errors[] = {
	{ 408, "Request Timeout" },  // client may retry later
	{ 414, "Request-URL Too Large" },
	{ 500, "Server Error" },
	{ 503, "Server Unavailable" },
	{ 999, "Unknown error" } // marker
};
#define N_ERRORS	(sizeof(errors) / sizeof(struct errmsg))


// Only called from close_request
void send_error(struct connection *conn, unsigned error)
{
	char errstr[80];
	int i;

	if(error == 404) {
		send_errno(SOCKET(conn), conn->cmd, errno);
		return;
	}

	for(i = 0; i < N_ERRORS - 1 && error != errors[i].errnum; ++i) ;

	sprintf(errstr, "3%s [%d]\r\n", errors[i].errstr, error);
	write(SOCKET(conn), errstr, strlen(errstr));
}
