/*
 * log.c - log file output for the gofish gopher daemon
 * Copyright (C) 2002 Sean MacLennan <seanm@seanm.ca>
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
#include <syslog.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>

#include "gofish.h"


static FILE *log_fp;
static char *log_name;
static int log_inuse = 0;
static int log_must_reopen = 0;


static void log_reopen(int sig)
{
	if(log_inuse) {
		log_must_reopen = 1;
		return;
	}

	log_must_reopen = 0;

	fclose(log_fp);
	log_fp = fopen(log_name, "a");

	syslog(LOG_WARNING, "Log file reopened.");
}


int log_open(char *logname)
{
	log_name = logname; // save for reopen

	signal(SIGUSR1, log_reopen);

	return (log_fp = fopen(logname, "a")) != NULL;
}


// Common log file format
void log_hit(struct connection *conn, unsigned status)
{
	char common[80], *p;
	time_t now;
	struct tm *t;

	if(!log_fp) return; // nowhere to write!

	if(ignore_local &&
	   ((conn->addr & 0xffff0000) == 0xc0a80000 ||
		conn->addr == 0x7f000001)) return;

	if(log_must_reopen)
		log_reopen(SIGUSR1);

	time(&now);
	t = localtime(&now);

	// Get some of the fixed length common stuff out of the way
	strcpy(common, ntoa(conn->addr));
	p = common + strlen(common);
	strftime(p, sizeof(common) - 30, " - - [%d/%b/%Y:%T %z] \"", t);
	strcat(p, conn->http == HTTP_HEAD ? "HEAD" : "GET");

	if(conn->http) {
		char *request, *e;

		if((p = strchr(conn->cmd, '\r')) ||
		   (p = strchr(conn->cmd, '\n'))) {// paranoia
			*p++ = '\0';
			e = p;
		} else {
			p = "?";
			e = "";
		}

		// SAM Save this?
		request = conn->cmd;
		request += 4;
		while(isspace((int)*request)) ++request;
		if(*request == '/') ++request;

		if(combined_log) {
			char *referer, *agent;

			if((referer = conn->referer)) {
				for(referer += 8; isspace((int)*referer); ++referer) ;
				if((p = strchr(referer, '\r')) ||
				   (p = strchr(referer, '\n')))
					*p = '\0';
				else {
					syslog(LOG_DEBUG, "Bad referer '%s'", referer);
					referer = "-";
				}
			} else {
				syslog(LOG_DEBUG, "No referer");
				referer = "-";
			}

			if((agent = conn->user_agent)) {
				for(agent += 12; isspace((int)*agent); ++agent) ;
				if((p = strchr(agent, '\r')) ||
				   (p = strchr(agent, '\n')))
					*p = '\0';
				else {
					syslog(LOG_DEBUG, "Bad agent '%s'", agent);
					agent = "-";
				}
			} else {
				syslog(LOG_DEBUG, "No agent");
				agent = "-";
			}

			// This is 500 + hostname chars max
			log_inuse = 1;
			if(virtual_hosts)
				fprintf(log_fp,
						"%s %s/%.200s\" %u %u \"%.100s\" \"%.100s\"\n",
						common, conn->host, request, status, conn->len,
						referer, agent);
			else
				fprintf(log_fp,
						"%s /%.200s\" %u %u \"%.100s\" \"%.100s\"\n",
						common, request, status, conn->len, referer, agent);
		} else {
			// This is 600 + hostname chars max
			// SAM 600???
			log_inuse = 1;
			if(virtual_hosts)
				fprintf(log_fp, "%s %s/%.200s\" %u %u\n",
						common, conn->host, request, status, conn->len);
			else
				fprintf(log_fp, "%s /%.200s\" %u %u\n",
						common, request, status, conn->len);
		}
	} else {
		char *name = conn->cmd ? (*conn->cmd ? conn->cmd : "/") : "[Empty]";

		// This is 400 chars max
		log_inuse = 1;
		fprintf(log_fp, "%s %.300s\" %u %u\n", common, name, status, conn->len);
	}

	// every path
	fflush(log_fp);
	log_inuse = 0;

	if(log_must_reopen)
		log_reopen(SIGUSR1);
}


void log_close()
{
	(void)fclose(log_fp);
}


static void send_errno(int sock, char *name, int errnum)
{
	char error[1024];

	if(*name == '\0')
		sprintf(error, "3'<root>' %.500s (%d)\r\n",
				strerror(errnum), errnum);
	else
		sprintf(error, "3'%.500s' %.500s (%d)\r\n",
				name, strerror(errnum), errnum);
	while(write(sock, error, strlen(error)) < 0 && errno == EINTR) ;
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
	while(write(SOCKET(conn), errstr, strlen(errstr)) < 0 && errno == EINTR) ;
}
