/*
 * gopherd.h - defines for the gofish gopher daemon
 * Copyright (C) 2002 Sean MacLennan <seanm@seanm.ca>
 * $Revision: 1.2 $ $Date: 2002/08/24 05:04:31 $
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

#ifdef HAVE_POLL
#include <poll.h>
#endif


#define MAX_HOSTNAME	65
#define MAX_LINE		1024
#define MAX_REQUESTS	256
#define GOPHER_BACKLOG	10 // helps latency

// If you leave GOPHER_HOST unset, it will default to your
// your hostname.
#define GOPHER_ROOT		"/var/lib/gopherd"
#define GOPHER_LOGFILE	"/var/log/gopherd.log"
#define GOPHER_PIDFILE	"/var/run/gopherd.pid"
#define GOPHER_CONFIG	"/etc/gofish.conf"
#define GOPHER_HOST		""
#define GOPHER_PORT		70


/* Set to 1 to not log the local network (192.168.x.x).
 * Set to 0 to log everything. Do not undefine.
 */
#define IGNORE_LOCAL	1

#define GOPHER_UID		13
#define GOPHER_GID		30


struct connection {
#ifdef HAVE_POLL
	struct pollfd *ufd;
#define SOCKET(c)	((c)->ufd->fd)
#else
	int sock;
#define SOCKET(c)	((c)->sock)
#endif
	unsigned addr;
	char *cmd;
#ifdef USE_SENDFILE
	int fd;
	int neednl;
#else
	unsigned char *buf;
#endif
	off_t offset;
	size_t len;
#ifdef USE_HTTP
	char *hdr_str;
	int   hdr_offset;
	int   hdr_len;
	char *outname;
	int   status;
#endif
};


// exported from gopherd.c
extern void close_request(struct connection *conn);

// exported from log.c
extern int  log_open(char *log_name);
extern void log_hit(unsigned ip, char *name, unsigned status, unsigned bytes);
extern void log_close(void);
extern void send_errno(int sock, char *name, int errnum);
extern void send_error(int sock, char *errstr);
extern void send_http_error(int sock);

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

int read_config(char *fname);
char *must_strdup(char *str);

// exported from http.c
int http_init(void);
int http_get(struct connection *conn);
int http_send_response(struct connection *conn);

#endif /* _GOFISH_H_ */
