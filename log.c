/*
 * log.c - log file output for the gofish gopher daemon
 * Copyright (C) 2002 Sean MacLennan <seanm@seanm.ca>
 * $Revision: 1.3 $ $Date: 2002/08/25 01:48:32 $
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
void log_hit(unsigned ip, char *name, unsigned status, unsigned bytes)
{
	extern int ignore_local;
	char date[40];
	time_t now;
	struct tm *t;

	if(ignore_local &&
	   ((htonl(ip) & 0xffff0000) == 0xc0a80000 ||
		htonl(ip) == 0x7f000001)) return;

	time(&now);
	t = localtime(&now);

	strftime(date, sizeof(date), "%d/%b/%Y:%T %z", t);

	// This is 400 chars max
	fprintf(fp, "%s - - [%s] \"GET %.300s\" %u %u\n",
			ntoa(ip), date, *name ? name : "/", status, bytes);

	(void)fflush(fp);
}


void log_close()
{
	(void)fclose(fp);
}


void send_errno(int sock, char *name, int errnum)
{
	char error[1024];

	if(*name == '\0')
		sprintf(error, "3'<root>' %.500s\tfake\t%d\r\n",
				strerror(errnum), errnum);
	else
		sprintf(error, "3'%.500s' %.500s\tfake\t%d\r\n",
				name, strerror(errnum), errnum);
	write(sock, error, strlen(error));
}


void send_error(int sock, char *errstr)
{
	char error[520];

	sprintf(error, "3%.500s\terror.host\t0\r\n", errstr);
	write(sock, error, strlen(error));
}


void send_http_error(int sock)
{
	char error[500];

	strcpy(error, "HTTP/1.1 501 Not Implemented\r\n\r\n"
		   "Sorry!\r\n\r\n"
		   "This is a gopher server, not a web server.\r\n");
	write(sock, error, strlen(error));
}
