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
#include <time.h>
#include <sys/utsname.h>
#include <sys/stat.h>

#include "gopherd.h"
#include "version.h"

#ifdef USE_HTTP

// Does not always return errors
// Does not proxy external links
// Maybe implement buffering in write_out
// Better binary mime handling
// Better image mime handling

// Max server string 600
static char *server_str;
static char *mime_html = "text/html; charset=iso-8859-1";


#define HTML_HEADER	"<html><body><center><table width=\"80%\"><tr><td><pre>\n"

#define HTML_TRAILER "</pre></table></center></body>\n"

#define HTML_INDEX_FILE	"index.html"
#define HTML_INDEX_TYPE	mime_html


extern int smart_open(char *name, char *type);

static int isdir(char *name);

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


static char *msg_404 =
"<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\r\n"
"<title>404 Not Found</title>\r\n"
"</head><body><h1>Not Found</h1>\r\n"
"<p>The requested URL was not found on this server.\r\n"
"</body></html>\r\n";

static char *msg_500 =
"<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\r\n"
"<title>500 Server Error</title>\r\n"
"</head><body><h1>Server Error</h1>\r\n"
"<p>An internal server error occurred. Try again later.\r\n"
"</body></html>\r\n";


static int http_error(struct connection *conn, int status)
{
	char str[1024], *p, *msg;
	int len;

	strcpy(str, "HTTP/1.0 ");
	switch(status) {
	case 404:
		strcat(str, "404 Not Found\r\n");
		msg = msg_404;
		break;
	case 500:
		strcat(str, "500 Server Error\r\n");
		msg = msg_500;
		break;
	default:
		p = str + strlen(str);
		sprintf(p, "%d Unknown\r\n", status);
		msg = msg_500;
		break;
	}

	strcat(str, server_str);
	strcat(str, "Content-Type: text/html\r\n");

	len = strlen(msg);
	p = str + strlen(str);
	sprintf(p, "Content-Length: %d\r\n", len);

	strcat(str, "\r\n");

	if((conn->http_header = strdup(str)) == NULL) {
		// Just closing the connection is the best we can do
		syslog(LOG_WARNING, "http_error: Out of memory.");
		close_request(conn, status);
		return 1;
	}

	conn->status = status;

	conn->iovs[0].iov_base = conn->http_header;
	conn->iovs[0].iov_len  = strlen(conn->http_header);
	conn->iovs[1].iov_base = msg;
	conn->iovs[1].iov_len  = len;
	conn->n_iovs = 2;

	set_writeable(conn);

	return 0;
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
		close_request(conn, 500);
		return 1;
	}

	conn->status = 200;

	return 0;
}


static void out_external(int out, char *host, char *port, char type, char *url)
{
	++url;
	write_str(out, host);
	write_str(out, ":");
	write_str(out, port);
	write_str(out, "/");
	if(type == '1' && (strcmp(url, "/") == 0 || !*url)) {
	} else {
		write_out(out, &type, 1);
		write_str(out, url);
	}
}


static int http_dir_line(int out, char *line)
{
	char *desc, *url, *host, *port;
	char *p, *icon;
	char buf[100];

	for(desc = p = line + 1; *p && *p != '\t'; ++p) ;
	*p++ = '\0';
	for(url = p; *p && *p != '\t'; ++p) ;
	*p++ = '\0';
	for(host = p; *p && *p != '\t'; ++p) ;
	*p++ = '\0';
	for(port = p; *p && *p != '\t'; ++p) ;
	*p++ = '\0';

	write_str(out, "  <tr>\n");

	if(*line == 'i') {
		write_str(out, "   <td>&nbsp;<td>");
		write_str(out, line + 1);
		write_out(out, "\n", 1);
	} else {
		switch(*line) {
		case '0': icon = "text"; break;
		case '1': icon = "menu"; break;
		case '9': icon = "binary"; break;
		case 'h': icon = "html"; break;
		case 'I': icon = "image"; break;
		default:  icon = "unknown"; break;
		}

		sprintf(buf,
				"   <td width=%d>"
				"<img src=\"/g/icons/gopher_%s.gif\" "
				"width=%d height=%d alt=\"[%s]\">\n   <td>",
				icon_width + 4, icon, icon_width, icon_height, icon);
		write_str(out, buf);

		if(strcmp(host, hostname) && strcmp(host, "localhost")) {
			write_str(out, desc);
			write_str(out, " <a href=\"gopher://");
			out_external(out, host, port, *line, url);
			write_str(out, "\">[gopher]</a>\n");
			write_str(out, " <a href=\"http://");
			out_external(out, host, port, *line, url);
			write_str(out, "\">[http]</a>\n");
		} else {
			write_str(out, "<a href=\"/");
			write_out(out, line, 1);
			write_str(out, url + 1);
			write_str(out, "\">");
			write_str(out, desc);
			write_str(out, "</a>\n");
		}
	}

	return 0;
}


#define BUFSIZE		2048

// return the outfd or -1 for error
static int http_directory(struct connection *conn, int fd, char *dir)
{
	int out;
	char buffer[BUFSIZE + 1], outname[20];
	char *buf, *p, *s;
	int n, len, left;


	sprintf(outname, ".gofish-XXXXXX");
	if((out = mkstemp(outname)) == -1) {
		syslog(LOG_ERR, "%s: %m", outname);
		return -1;
	}
	if((conn->outname = strdup(outname)) == NULL) {
		syslog(LOG_ERR, "Out of memory");
		return -1;
	}

	if(*dir == '\0' || strcmp(dir, "/") == 0) dir = "[Root]";

	// The table inside a table works better for Opera
	sprintf(buffer,
			"<!DOCTYPE HTML PUBLIC "
			"\"-//W3C//DTD HTML 4.01 Transitional//EN\">\n"
			"<html>\n<head><title>%.80s</title></head>\n"
			"<body bgcolor=white>\n\n"
			"<table border=0 width=\"80%%\" align=center>\n"
			"<tr><td><h1>%.80s</h1>\n"
			"<tr><td><hr>\n"
			"<tr><td>\n <table border=0>\n",
			dir, dir);
	write_str(out, buffer);


	buf = buffer;
	len = BUFSIZE;
	while((n = read(fd, buf, len)) > 0) {
		buf[n] = '\0';
		for(s = buffer; (p = strchr(s, '\n')); s = p) {
			if(p > buffer && *(p - 1) == '\r') *(p - 1) = '\0';
			*p++ = '\0';

			http_dir_line(out, s); // do it
		}

		if(s == buffer) {
			syslog(LOG_WARNING, "%s: line too long", dir);
			return -1;
		}

		// Move the leftover bytes down
		left = n - (s - buf);
		memmove(buffer, s, left);

		// update for next read
		buf = buffer + left;
		len = BUFSIZE - left;
	}

	if(n < 0) {
		syslog(LOG_ERR, "%s: read error", dir);
		return -1;
	}

	write_str(out, " </table>\n<tr><td><hr>\n"
			  "<tr><td><small>"
			  "<a href=\"http://gofish.sourceforge.net/\">"
			  "GoFish " GOFISH_VERSION
			  "</a> gopher to http gateway.</small>\n"
			  "</table>\n"
			  "\n</body>\n</html>\n");

	close(fd);

	return out;
}


int http_get(struct connection *conn)
{
	char *e;
	int fd;
	char *mime, type;
	char *request = conn->cmd;

	conn->http = *request == 'H' ? HTTP_HEAD : HTTP_GET;

	// This works for both GET and HEAD
	request += 4;
	while(isspace(*request)) ++request;

	if(!(e = strstr(request, "HTTP/"))) {
		// Lynx does not add HTTP!
		if((e = strchr(request, '\n')) == NULL)
			e = request + strlen(request); // paranoia
		if(*(e - 1) == '\r') --e;
	}

	while(*(e - 1) == ' ') --e;
	*e++ = '\0';

	if(*request == '/') ++request;

	unquote(request);

	if((fd = smart_open(request, &type)) >= 0) {
		// valid gopher request
		if(verbose) printf("Gopher request '%s'\n", request);
		switch(type) {
		case '1':
			if((fd = http_directory(conn, fd, request)) < 0)
				return http_error(conn, 500);
			mime = mime_html;
			break;
		case '0':
			// htmlize it
			conn->html_header  = HTML_HEADER;
			conn->html_trailer = HTML_TRAILER;
			mime = "text/html";
			break;
		case '4':
		case '5':
		case '6':
		case '9':
			if((mime = find_mime(request)) == NULL)
			   mime = "application/octet-stream";
			break;
		case 'g': mime = "image/gif"; break;
		case 'h': mime = mime_html; break;
		case 'I': mime = find_mime(request); break;
		default:
			// Safe default - let the user handle it
			mime = "application/octet-stream";
			syslog(LOG_WARNING, "Bad file type %c", type);
			break;
		}
	} else {
		// real http request
#ifdef VIRTUAL_HOSTS
		char *host;

		if((host = strstr(e, "Host:"))) {
			// isolate the host - ignore the port (if any)
			for(host += 5; isspace(*host); ++host) ;
			for(e = host; *e && !isspace(*e) && *e != ':'; ++e) ;
			*e = '\0';
		}

		if(!host || !*host) {
			syslog(LOG_WARNING, "Request with no host '%s'", request);
			return http_error(conn, 404);
		}

		// root it
		--host;
		*host = '/';

		// SAM Is this an expensive call?
		if(chdir(host)) {
			syslog(LOG_WARNING, "No directory for host '%s'", host);
			return http_error(conn, 404);
		}
		if(verbose) printf("Http request %s '%s'\n", host + 1, request);
#else
		if(verbose) printf("Http request '%s'\n", request);
#endif
		if(*request) {
			if(isdir(request)) {
				char dirname[MAX_LINE + 20], *p;

				strcpy(dirname, request);
				p = dirname + strlen(dirname);
				if(*(p - 1) != '/') *p++ = '/';
				strcpy(p, HTML_INDEX_FILE);
				fd = open(dirname, O_RDONLY);
				mime = HTML_INDEX_TYPE;
			} else {
				fd = open(request, O_RDONLY);
				mime = find_mime(request);
			}
		} else {
			fd = open(HTML_INDEX_FILE, O_RDONLY);
			mime = HTML_INDEX_TYPE;
		}
	}

	if(fd < 0) {
		syslog(LOG_WARNING, "%s: %m", request);
		return http_error(conn, 404);
	}

	conn->len = lseek(fd, 0, SEEK_END);

	if(http_build_response(conn, mime)) {
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
		close(fd);
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


static int isdir(char *name)
{
	struct stat sbuf;

	if(stat(name, &sbuf) == -1) return 0;
	return S_ISDIR(sbuf.st_mode);
}

#else

// Stub
int http_get(struct connection *conn)
{
	char error[500];

	strcpy(error, "HTTP/1.1 501 Not Implemented\r\n\r\n"
		   "Sorry!\r\n\r\n"
		   "This is a gopher server, not a web server.\r\n");
	write(SOCKET(conn), error, strlen(error));

	return -1;
}

#endif // USE_HTTP

// These are always safe
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
