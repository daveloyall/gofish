/*
 * http.c - http handler for the gofish gopher daemon
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
 * along with XEmacs; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <sys/utsname.h>
#include <sys/stat.h>

#include "gofish.h"
#include "version.h"

/* SAM Much of this can be shared with http.c. But I am keeping it
 * seperate for now to make it easier to code.
 */

#define MAX_SERVER_STRING	600
static char *server_str;

static char *mime_html = "text/html; charset=iso-8859-1";


#define HTML_HEADER	"<html><body><center><table width=\"80%\"><tr><td><pre>\n"

#define HTML_TRAILER "</pre></table></center></body>\n"

#define HTML_INDEX_FILE	"index.html"
#define HTML_INDEX_TYPE	mime_html


#if 0
static int isdir(char *name);
#endif


inline int write_out(int fd, char *buf, int len)
{
	return write(fd, buf, len);
}


inline int write_str(int fd, char *str)
{
	return write_out(fd, str, strlen(str));
}


static void unquote(char *str)
{
	char *p, quote[3], *e;
	int n;

	for(p = str; (p = strchr(p, '%')); ) {
		quote[0] = *(p + 1);
		quote[1] = *(p + 2);
		quote[2] = '\0';
		n = strtol(quote, &e, 16);
		if(e == (quote + 2)) {
			*p++ = (char)n;
			memmove(p, p + 2, strlen(p + 2) + 1);
		}
		else
			++p; // skip over %
	}
}


static char *msg_400 =
"Your browser sent a request that this server could not understand.";

static char *msg_404 =
"The requested URL was not found on this server.";

static char *msg_414 =
"The requested URL was too large.";

static char *msg_500 =
"An internal server error occurred. Try again later.";


/* This is a very specialized build_response just for errors.
   The request field is for the 301 errors.
*/
static int http_error1(struct connection *conn, int status, char *request)
{
	char str[MAX_LINE + MAX_LINE + MAX_SERVER_STRING + 512];
	char *title, *p, *msg;

	switch(status) {
	case 301:
		// Be nice and give the moved address.
		title = "301 Moved Permanently";
		sprintf(str,
				"The document has moved <a href=\"/%s/\">here</a>.",
				request);
		if((msg = strdup(str)) == NULL) {
			syslog(LOG_WARNING, "http_error: Out of memory.");
			close_connection(conn, status);
			return 1;
		}
		break;
	case 400:
		title = "400 Bad Request";
		msg = msg_400;
		break;
	case 403:
		title = "403 Forbidden";
		msg = msg_404;
		break;
	case 404:
		title = "404 Not Found";
		msg = msg_404;
		break;
	case 414:
		title = "414 Request URL Too Large";
		msg = msg_414;
		break;
	case 500:
		title = "500 Server Error";
		msg = msg_500;
		break;
	default:
		syslog(LOG_ERR, "Unknow error status %d", status);
		title = "500 Unknown";
		msg = msg_500;
		break;
	}

	sprintf(str,
			"HTTP/1.0 %s\r\n"
			"Server: %s"
			"Content-Type: text/html\r\n",
			title, server_str);

	if(status == 301) {
		// we must add the *real* location
		p = str + strlen(str);
		sprintf(p, "Location: %s\r\n", request); // SAM fix
	}

	strcat(str, "\r\n");

	// Build the html body
	p = str + strlen(str);
	sprintf(p,
			"<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\r\n"
			"<title>%s</title>\r\n"
			"</head><body><h1>%s</h1>\r\n"
			"<p>%s\r\n"
			"</body></html>\r\n",
			title, title, msg);

	if(status == 301) free(msg);

	if((conn->http_header = strdup(str)) == NULL) {
		syslog(LOG_WARNING, "http_error: Out of memory.");
		if(status == 302) free(msg);
		close_connection(conn, status);
		return 1;
	}

	conn->status = status;

	conn->iovs[0].iov_base = conn->http_header;
	conn->iovs[0].iov_len  = strlen(conn->http_header);
	conn->n_iovs = 1;

	set_writeable(conn);

	return 0;
}


/* For all but 301 errors */
int http_error(struct connection *conn, int status)
{
	return http_error1(conn, status, "bogus");
}


static int http_build_response(struct connection *conn, char *type)
{
	char str[1024], *p;
	int len;

	strcpy(str, "HTTP/1.1 200 OK\r\n");
	strcat(str, server_str);
	p = str;
	if(type) {
		p += strlen(p);
		sprintf(p, "Content-Type: %s\r\n", type);
	}
	p += strlen(p);
	len = conn->len;
	if(conn->html_header) len += strlen(conn->html_header);
	if(conn->html_trailer) len += strlen(conn->html_trailer);
	sprintf(p, "Content-Length: %d\r\n\r\n", len);

	if((conn->http_header = strdup(str)) == NULL) {
		// Just closing the connection is the best we can do
		close_connection(conn, 500);
		return 1;
	}

	conn->status = 200;

	return 0;
}

static int go_popen(const char *command, const char *path)
{
	int fd[2];
	pid_t child;

	if(pipe(fd) < 0) {
		perror("pipe"); // SAM
		return -1;
	}

	if((child = fork()) == 0) {
		// child
		char *envp[10];
		char env[1024];

		close(fd[0]);
		dup2(fd[1], 1);
		dup2(fd[1], 2);

		strcpy(env, "PATH=/usr/bin:/bin");
		envp[0] = strdup(env);
		sprintf(env, "SCRIPT_NAME=%s", command);
		envp[1] = strdup(env);
		sprintf(env, "PATH_INFO=/%s", path);
		envp[2] = strdup(env);
		envp[3] = NULL;

		if(execle("./viewcvs.cgi", NULL, envp) < 0) // SAM hardcoded
			perror("execle");

		exit(1); // paranoia
	}

	if(child == -1) {
		perror("fork"); // SAM
		return -1;
	}

	close(fd[1]);
	return fd[0];
}


struct mark {
	char *pos;
	char data;
};

#define MSAVE(m, p) (m)->pos = (p); (m)->data = *(p)
#define MRESTORE(m) *(m)->pos = (m)->data


int cgi_get(struct connection *conn)
{
	char *e;
	int fd;
	struct mark save;
	char *request = conn->cmd;
	char *path, *p;

	// SAM Does a cgi HEAD make sense?
	conn->http = *request == 'H' ? HTTP_HEAD : HTTP_GET;

	// This works for both GET and HEAD
	request += 4;
	while(isspace((int)*request)) ++request;

	if((e = strstr(request, "HTTP/")) == NULL)
		// probably a local lynx request
		return http_error(conn, 400);

	while(*(e - 1) == ' ') --e;
	MSAVE(&save, e);
	*e++ = '\0';

	if(*request == '/') ++request;

	unquote(request);

	// real http request or bad gateway request
	if(combined_log) {
		// Save these up front for logging
		conn->referer = strstr(e, "Referer:");
		conn->user_agent = strstr(e, "User-Agent:");
	}

	if(virtual_hosts) {
		char *host;
		int rc;

		if((host = strstr(e, "Host:"))) {
			// isolate the host - ignore the port (if any)
			for(host += 5; isspace((int)*host); ++host) ;
			for(e = host; *e && !isspace((int)*e) && *e != ':'; ++e) ;
			*e = '\0';
		}

		if(!host || !*host) {
			syslog(LOG_WARNING, "Request with no host '%s'", request);
			MRESTORE(&save);
			return http_error(conn, 403);
		}

		// root it
		--host;
		*host = '/';

		conn->host = host;

		// SAM Is this an expensive call?
		// SAM Cache current?
		rc = chdir(host);

		if(rc) {
			syslog(LOG_WARNING, "host '%s': %m", host);
			MRESTORE(&save);
			return http_error(conn, 404);
		}

		if(verbose) printf("Http request %s '%s'\n", host + 1, request);
	}
	else if(verbose) printf("Http request '%s'\n", request);


	// SAM hardcoded
	if(strncmp(request, "cgi-bin/", 8) == 0)
		request += 8;
	else {
#if 0
		char redirect[1024]; // SAM

		printf("PROBLEMS: not a cgi-bin request\n"); // SAM DBG
		sprintf(redirect, "http://opus/%s", request);
		http_error1(conn, 301, redirect);
		return 0;
#else
		printf("Open %s direct\n", request); // SAM DBG
		if((fd = open(request, O_RDONLY)) < 0)
			perror(request); // SAM DBG
		goto send_it;
#endif
	}

	if((path = strchr(request, '/')))
		*path++ = '\0';
	else
		path = "";

	while((p = strstr(path, request))) path = p + strlen(request) + 1;

	printf("Request '%s' Path '%s'\n", request, path); // SAM DBG


	if(*request == '\0') {
		printf("NO REQUEST\n");
		http_error(conn, 403);
		return 0;
	}

	// SAM EVIL HACK FOR NOW - read into file and send file
	// We should read from pipe and send to socket
	{
		int pfd, n;
		char line[1024];

		if((pfd = go_popen(request, path)) < 0) {
			http_error(conn, 403);
			return 0;
		}

		sprintf(line, ".gofish-cgi-XXXXXX");
		if((fd = mkstemp(line)) < 0) {
			perror("mkstemp");
			http_error(conn, 500);
			return 0;
		}

#if 0
		if((conn->outname = strdup(line)) == NULL) {
			printf("out of memory\n");
			http_error(conn, 500);
			return 0;
		}
#endif

		while((n = read(pfd, line, sizeof(line))) > 0)
			write(fd, line, n);

		close(pfd);
	}

 send_it:
	if(fd < 0) {
		syslog(LOG_WARNING, "%s: %m", request);
		MRESTORE(&save);
		return http_error(conn, 404);
	}

	MRESTORE(&save);

	conn->len = lseek(fd, 0, SEEK_END);

	if(http_build_response(conn, mime_html)) {
		syslog(LOG_WARNING, "Out of memory");
		return -1;
	}

	conn->iovs[0].iov_base = conn->http_header;
	conn->iovs[0].iov_len  = strlen(conn->http_header);

	if(conn->http == HTTP_HEAD) {
		// no body to send
		close(fd);

		conn->len = 0;
		conn->n_iovs = 1;
		set_writeable(conn);

		return 0;
	}

	conn->buf = mmap_get(conn, fd);

	close(fd); // done with this

	// Zero length files will fail
	if(conn->buf == NULL && conn->len) {
		syslog(LOG_ERR, "mmap: %m");
		return http_error(conn, 500);
	}

	if(conn->html_header) {
		conn->iovs[1].iov_base = conn->html_header;
		conn->iovs[1].iov_len  = strlen(conn->html_header);
	}

	if(conn->buf) {
		conn->iovs[2].iov_base = conn->buf;
		conn->iovs[2].iov_len  = conn->len;
	}

	if(conn->html_trailer) {
		conn->iovs[3].iov_base = conn->html_trailer;
		conn->iovs[3].iov_len  = strlen(conn->html_trailer);
	}

	conn->len =
		conn->iovs[0].iov_len +
		conn->iovs[1].iov_len +
		conn->iovs[2].iov_len +
		conn->iovs[3].iov_len;

	conn->n_iovs = 4;

	set_writeable(conn);

	return 0;
}


#if 0
static int isdir(char *name)
{
	struct stat sbuf;

	if(stat(name, &sbuf) == -1) return 0;
	return S_ISDIR(sbuf.st_mode);
}
#endif


int http_init()
{
	char str[600];
	struct utsname uts;

	uname(&uts);

	sprintf(str, "Server: GoFish/%.8s (%.512s)\r\n", GOFISH_VERSION, uts.sysname);

	if(!(server_str = strdup(str))) {
		syslog(LOG_ERR, "http_init: Out of memory");
		exit(1);
	}

	return 0;
}


void http_cleanup()
{
	if(server_str) free(server_str);
}
