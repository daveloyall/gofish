/*
 * gopherd.h - defines for the gofish gopher daemon
 * Copyright (C) 2002 Sean MacLennan <seanm@seanm.ca>
 * $Revision: 1.7 $ $Date: 2002/09/22 17:54:24 $
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
#ifndef _GOFISH_H_
#define _GOFISH_H_

#include "config.h"

#ifdef HAVE_POLL
#include <poll.h>
#endif
#include <sys/uio.h>

#define MAX_HOSTNAME	65
#define MAX_LINE		1024
#define MAX_REQUESTS	25
#define MIN_REQUESTS	4
#define GOPHER_BACKLOG	5 // helps when backed up

// If you leave GOPHER_HOST unset, it will default to your
// your hostname.
#define GOPHER_ROOT		"/var/lib/gopherd"
#define GOPHER_LOGFILE	"/var/log/gopherd.log"
#define GOPHER_PIDFILE	"/var/run/gopherd.pid"
#define GOPHER_CONFIG	"/etc/gofish.conf"
#define GOPHER_HOST		NULL
#define GOPHER_PORT		70

// Supplied icons are this size
#define ICON_WIDTH		20
#define ICON_HEIGHT		23


/* Set to 1 to not log the local network (192.168.x.x).
 * Set to 0 to log everything. Do not undefine.
 */
#define IGNORE_LOCAL	1

#define GOPHER_UID		13
#define GOPHER_GID		30


struct connection {
	int conn_n;
#ifdef HAVE_POLL
	struct pollfd *ufd;
#else
	int sock;
#endif
	unsigned addr;
	char *cmd;
	off_t offset;
	size_t len;
	unsigned char *buf;
	int   status;
	struct iovec iovs[4];
#if defined(USE_HTTP)
	int http;
	char *http_header;
	char *html_header;
	char *html_trailer;
	char *outname;
#endif
};


// exported from gopherd.c
extern void close_request(struct connection *conn, int status);
int checkpath(char *path);

// exported from log.c
extern int  log_open(char *log_name);
extern void log_hit(struct connection *conn, unsigned status);
extern void log_close(void);
extern void send_error(struct connection *conn, unsigned error);

// exported from socket.c
extern int listen_socket(int port);
extern int accept_socket(int sock, unsigned *addr);
extern char *ntoa(unsigned n); // helper

// exported from config.c
extern char *config;
extern char *root_dir;
extern char *logfile;
extern char *pidfile;
extern char *hostname;
extern int   port;
extern uid_t uid;
extern gid_t gid;
extern int   ignore_local;
extern int   icon_width;
extern int   icon_height;

int read_config(char *fname);
char *must_strdup(char *str);

// exported from http.c
int http_init(void);
void http_cleanup(void);
int http_get(struct connection *conn);
int http_send_response(struct connection *conn);

#if MAX_REQUESTS < 2
#error You must have at least 2 requests!
#endif

#ifdef HAVE_POLL

#define SOCKET(c)	((c)->ufd->fd)

#define set_readable(c, sock) \
	do { \
		(c)->ufd->fd = sock; \
		(c)->ufd->events = POLLIN; \
		if((c)->conn_n + 1 > npoll) npoll = (c)->conn_n + 1; \
	} while(0)

#define set_writeable(c) \
	(c)->ufd->events = POLLOUT

#else

#define SOCKET(c)	((c)->sock)

#define set_readable(c, sock) \
	do { \
		(c)->sock = sock; \
		FD_SET(sock, &readfds); \
		if(sock + 1 > nfds) nfds = sock + 1; \
	} while(0)

#define set_writeable(c) \
	FD_CLR((c)->sock, &readfds); \
	FD_SET((c)->sock, &writefds)

#endif

#endif /* _GOFISH_H_ */
