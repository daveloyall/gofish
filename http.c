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

#include "gopherd.h"
#include "version.h"

#ifdef USE_HTTP

// Does not always return errors
// Does not proxy external links
// Maybe implement buffering in write_out
// Better binary mime handling
// Better image mime handling

static char *os_str;

static int smart_open(char *name, char type);


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

static int http_build_response(struct connection *conn, int status, char *type)
{
	char str[500], *p;
	int len;

	strcpy(str, "HTTP/1.1 ");
	switch(status) {
	case 200: strcat(str, "200 OK\r\n"); break;
	case 404: strcat(str, "404 Not found\r\n"); break;
	default:
		sprintf(str, "HTTP/1.1 %d Unknown\r\n", status);
		break;
	}
	p = str + strlen(str);
	sprintf(p, "Server: GoFish/%s (%s)\r\n", GOFISH_VERSION, os_str);
	if(type) {
		p += strlen(p);
		sprintf(p, "Content-Type: %s\r\n", type);
	}
	p += strlen(p);
	sprintf(p, "Content-Length: %d\r\n\r\n", conn->len);

	len = strlen(str);
	if((conn->hdr = malloc(sizeof(struct iovec) + len + 1)) == NULL)
		return 1;

	p = (char*)(conn->hdr) + sizeof(struct iovec);
	strcpy(p, str);
	conn->hdr->iov_base = p;
	conn->hdr->iov_len  = len;

	conn->status = status;

	return 0;
}


/* Exported to send the response before the file */
int http_send_response(struct connection *conn)
{
	int n;

	n = writev(SOCKET(conn), conn->hdr, 1);

	if(n <= 0) {
		close_request(conn, 408);
		return 1;
	}

	if(n >= conn->hdr->iov_len) {
		free(conn->hdr);
		conn->hdr = NULL;

		if(conn->len == 0)
			// This is an error status - no data to send
			close_request(conn, conn->status);

		return 0;
	}

	conn->hdr->iov_base += n;
	conn->hdr->iov_len  -= n;

	return 1;
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

	write_str(out, "<tr>\n");

	if(*line == 'i') {
		write_str(out, " <td>&nbsp;<td>");
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
				" <td width=%d>"
				"<img src=\"/g/icons/gopher_%s.gif\" "
				"width=%d height=%d alt=\"[%s]\">\n <td>",
				icon_width + 4, icon, icon_width, icon_height, icon);
		write_str(out, buf);

		if(strcmp(host, hostname) && strcmp(host, "localhost")) {
			write_str(out, desc);
			write_str(out, " <small>[gopher://");
			write_str(out, host);
			if(strcmp(port, "70")) {
				write_str(out, ":");
				write_str(out, port);
			}
			write_str(out, "/");
			if(*line == '1' && (strcmp(url + 1, "/") == 0 || !*(url + 1))) {
			} else {
				write_out(out, line, 1);
				write_str(out, url + 1);
			}
			write_str(out, "]</small>\n");
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
	sprintf(buffer,
			"<!DOCTYPE HTML PUBLIC "
			"\"-//W3C//DTD HTML 4.01 Transitional//EN\">\n"
			"<html>\n<head><title>%.80s</title></head>\n"
			"<body bgcolor=white>\n\n"
			"<center>\n<table border=0 width=\"90%%\">\n" // table
			"<tr><td colspan=2><h1>%.80s</h1>\n"
			"<tr><td colspan=2><hr>\n",
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

	write_str(out, "<tr><td colspan=2><hr>\n"
		  "<tr><td colspan=2><small>"
		  "<a href=\"http://gofish.sourceforge.net/\">"
		  "GoFish " GOFISH_VERSION
		  "</a> gopher to http gateway.</small>\n"
		  "</table>\n</center>\n");

	write_str(out, "\n</body>\n</html>\n");

	close(fd);

	return out;
}


static int valid_type(char type)
{
	switch(type) {
	case '0':
	case '1':
	case '9':
	case 'g':
	case 'h':
	case 'I':
		return 1;
	case 'i':
	default:
		return 0;
	}
}


struct mime {
	char *ext;
	char *mime;
};

static struct mime image_mimes[] = {
	{ "gif",  "image/gif"  },
	{ "jpg",  "image/jpeg" },
	{ "png",  "image/png"  },
	{ "jpeg", "image/jpeg" },
};
#define N_IMAGE_MIMES	(sizeof(image_mimes) / sizeof(struct mime))


static char *mime_image(char *file)
{
	char *p;
	int i;

	if((p = strrchr(file, '.')) == NULL) return NULL;
	++p;

	for(i = 0; i < N_IMAGE_MIMES; ++i)
		if(strcasecmp(p, image_mimes[i].ext) == 0)
			return image_mimes[i].mime;

	return NULL;
}


/*
 * This routine is exported to GoFish to process http GET requests.
 *
 * This routine is responsonsible for sending errors but does not log
 * hits, even for errors.
 *
 * We expect *at minimum* "GET / HTTP/x.x"
 * Normal: "GET /?[type][type]?/<path> HTTP/x.x"
 */
int http_get(struct connection *conn)
{
	static char *mime_html = "text/html; charset=iso-8859-1";
	char *e;
	int fd;
	char *mime, type1;
	char *request = conn->cmd;

	request += 4;
	while(isspace(*request)) ++request;

	if(!(e = strstr(request, "HTTP/"))) {
		// Lynx does not add HTTP!
		if((e = strchr(request, '\n')) == NULL)
			e = request + strlen(request); // paranoia
		if(*(e - 1) == '\r') --e;
	}

	while(*(e - 1) == ' ') --e;
	*e = '\0';

	if(*request == '/') ++request;

	if(*request) {
		type1 = *request++;
		if(*request && *request != '/') request++;
		if(*request != '/') {
			syslog(LOG_WARNING, "Bad request '%s'\n", request);
			return -1;
		}

		if(!valid_type(type1)) {
			syslog(LOG_WARNING, "Invalid type in '%s'\n", request);
			return -1;
		}
	}
	else
		type1 = '1';

	unquote(request);

	if((fd = smart_open(request, type1)) > 0) {
		switch(type1) {
		case '1':
			fd = http_directory(conn, fd, request);
			if(fd < 0) return -1;
			mime = mime_html;
			break;
		case '0': mime = "text/plain"; break;
		case '9': mime = "application/octet-stream"; break;
		case 'g': mime = "image/gif"; break;
		case 'h': mime = mime_html; break;
		case 'I': mime = mime_image(request); break;
		default:
			syslog(LOG_WARNING, "Bad file type %c", type1);
			return -1;
		}

		conn->len = lseek(fd, 0, SEEK_END);
		if(http_build_response(conn, 200, mime)) {
			syslog(LOG_WARNING, "Out of memory");
			return -1;
		}

		/*
		 * The generic gopher code looks at this for the file
		 * type. All files are "binary" so we do not send the trailing
		 * dot.
		 */
		*conn->cmd = '9';

		return fd;
	} else {
		http_build_response(conn, 404, "text/html");
		syslog(LOG_WARNING, "%s: %m", request);
		return -1;
	}
}


/*
 * This handles parsing the name and opening the file.
 *
 * The difference between this and the gopherd.c version is that we
 * have already stripped the type.
 */
static int smart_open(char *name, char type)
{
	if(*name == '/') ++name;

	if(*name == '\0') return open(".cache", O_RDONLY);

#ifdef ALLOW_NON_ROOT
	if(checkpath(name)) return -1;
#endif

	if(type == '1') {
		char dirname[MAX_LINE + 10], *p;

		strcpy(dirname, name);
		p = dirname + strlen(dirname);
		if(*(p - 1) != '/') *p++ = '/';
		strcpy(p, ".cache");
		return open(dirname, O_RDONLY);
	}

	return open(name, O_RDONLY);
}


int http_init()
{
	struct utsname uts;

	uname(&uts);

	if(!(os_str = strdup(uts.sysname))) {
		os_str = "Unknown";
		return 1;
	}

	return 0;
}


void http_cleanup()
{
	free(os_str);
}


#else

/* Stub out the exported functions */

int http_init() { return 0; }

void http_cleanup() {}

int http_get(struct connection *conn)
{
	char error[500];

	strcpy(error, "HTTP/1.1 501 Not Implemented\r\n\r\n"
		   "Sorry!\r\n\r\n"
		   "This is a gopher server, not a web server.\r\n");
	write(sock, error, strlen(error));

	return -1;
}

#endif /* USE_HTTP */
