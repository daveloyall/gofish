/*
 * gopherd.c - the mainline for the gofish gopher daemon
 * Copyright (C) 2002 Sean MacLennan <seanm@seanm.ca>
 * $Revision: 1.6 $ $Date: 2002/09/01 00:50:53 $
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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <assert.h>
#include <syslog.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <sys/time.h>

#include "gopherd.h"
#include "version.h"

#ifdef HAVE_MMAP
#include <sys/mman.h>
#endif


int verbose = 0;
int max_requests = 0;


// Add an extra connection for error replies
static struct connection conns[MAX_REQUESTS];

#ifdef HAVE_POLL
static struct pollfd ufds[MAX_REQUESTS];
static int npoll;

static void start_polling(int csock);
#else
// SAM For select, could we tie fd to connection?
static fd_set readfds, writefds;
static int nfds;

static void start_selecting(int csock);
#endif


static uid_t real_uid;

// forward references
static void gopherd(void);
static void create_pidfile(char *fname);
static int new_connection(int csock);
static int read_request(struct connection *conn);
static int write_request(struct connection *conn);


static void sighandler(int signum)
{
	switch(signum) {
	case SIGHUP:
	case SIGTERM:
	case SIGINT:
		// Somebody wants us to quit
		syslog(LOG_INFO, "GoFish stopping.");
		log_close();
		exit(0);
	case SIGPIPE:
		// We get a SIGPIPE if the client closes the
		// connection on us.
		break;
	default:
		syslog(LOG_WARNING, "Got an unexpected %d signal\n", signum);
		break;
	}
}


static void cleanup()
{
	struct connection *conn = conns;
	int i;

	http_cleanup();

	close(SOCKET(conn)); // accept socket

	/*
     * This is mainly for valgrind.
     * Close any outstanding connections.
     * Free any cached memory.
     */
	for(++conn, i = 1; i < MAX_REQUESTS; ++i, ++conn) {
		if(SOCKET(conn)) close_request(conn, 500);
		if(conn->cmd) free(conn->cmd);
	}

	free(root_dir);
	free(hostname);

	printf("Max requests %d\n", max_requests);
}


int main(int argc, char *argv[])
{
	char *config = GOPHER_CONFIG;
	pid_t pid;
	int c, daemon = 0;

	while((c = getopt(argc, argv, "c:dv")) != -1)
		switch(c) {
		case 'c': config = optarg; break;
		case 'd': daemon = 1; break;
		case 'v': ++verbose; break;
		default:
			printf("usage: %s [-dv] [-c config]\n", *argv);
			exit(1);
		}

	if(read_config(config)) exit(1);

	http_init();

	if(daemon) {
		if((pid = fork()) == 0) {
			gopherd(); // never returns
			exit(1);	// paranoia
		}
		return pid ==-1 ? 1 : 0; // parent exits
	}
	else
		gopherd(); // never returns
	return 1; // compiler shutup
}


void gopherd(void)
{
	int csock, i;

	openlog("gopherd", LOG_CONS, LOG_DAEMON);
	syslog(LOG_INFO, "GoFish " GOFISH_VERSION " starting.");
	log_open(logfile);

	// Create *before* chroot
	create_pidfile(pidfile);

	if(chdir(root_dir)) {
		perror(root_dir);
		exit(1);
	}

	real_uid = getuid();
	setgid(gid);

	if(chroot(root_dir)) {
#ifdef ALLOW_NON_ROOT
		if(errno == EPERM)
			printf("WARNING: You are not running in a chroot environment!\n");
		else
#endif
		{
			perror("chroot");
			exit(1);
		}
	}

	signal(SIGHUP,  sighandler);
	signal(SIGTERM, sighandler);
	signal(SIGINT,  sighandler);
	signal(SIGPIPE, sighandler);

	// connection socket
	if((csock = listen_socket(port)) < 0) {
		syslog(LOG_ERR, "Unable to create socket\n");
		exit(1);
	}

	memset(conns, 0, sizeof(conns));
	for(i = 0; i < MAX_REQUESTS; ++i) {
		conns[i].status = 200;
		conns[i].conn_n = i;
	}

	// These never return
#ifdef HAVE_POLL
	start_polling(csock);
#else
	start_selecting(csock);
#endif
}


#ifdef HAVE_POLL
void start_polling(int csock)
{
	int i;
	int n;

	memset(ufds, 0, sizeof(ufds));

	for(i = 0; i < MAX_REQUESTS; ++i)
		conns[i].ufd = &ufds[i];

	// Now it is safe to install
	atexit(cleanup);

	ufds[0].fd = csock;
	ufds[0].events = POLLIN;
	npoll = 1;

	while(1) {
		if((n = poll(ufds, npoll, -1)) <= 0) {
			syslog(LOG_WARNING, "poll: %m");
			continue;
		}

		if(ufds[0].revents) {
			new_connection(ufds[0].fd);
			--n;
		}

		for(i = 1; n >= 0 && i < npoll; ++i)
			if(ufds[i].revents & POLLIN) {
				read_request(&conns[i]);
				--n;
			}
			else if(ufds[i].revents & POLLOUT) {
				write_request(&conns[i]);
				--n;
			}
			else if(ufds[i].revents) {
				syslog(LOG_DEBUG, "Revents = %d", ufds[i].revents);
				--n;
			}

		if(n > 0) syslog(LOG_DEBUG, "Not all requests processed");
	}
}

#else
static inline struct connection *find_conn(fd)
{
	int i;

	for(i = 0; i < MAX_REQUESTS; ++i)
		if(conns[i].sock == fd)
			return &conns[i];

	return NULL;
}


void start_selecting(int csock)
{
	int n, fd;
	struct connection *conn;
	fd_set cur_reads, cur_writes;

	FD_ZERO(&readfds);
	FD_ZERO(&writefds);

	FD_SET(csock, &readfds);
	nfds = csock + 1;

	atexit(cleanup);

	while(1) {
		memcpy(&cur_reads,  &readfds, sizeof(fd_set));
		memcpy(&cur_writes, &writefds, sizeof(fd_set));

		if((n = select(nfds, &cur_reads, &cur_writes, NULL, NULL)) <= 0) {
			syslog(LOG_WARNING, "select: %m");
			continue;
		}

		if(FD_ISSET(csock, &cur_reads)) {
			--n;
			FD_CLR(csock, &cur_reads);
			new_connection(csock);
		}

		for(fd = 0; n > 0 && fd < nfds; ++fd) {
			if(FD_ISSET(fd, &cur_reads)) {
				--n;
				if((conn = find_conn(fd)))
					read_request(conn);
				else
					syslog(LOG_DEBUG, "No connection found for read fd");
			} else if(FD_ISSET(fd, &cur_writes)) {
				--n;
				if((conn = find_conn(fd)))
					write_request(conn);
					else
						syslog(LOG_DEBUG, "No connection found for write fd");
			}
		}

		if(n > 0) syslog(LOG_DEBUG, "Not all requests processed");
	}
}
#endif

#if !defined(HAVE_MMAP)
#define PROT_READ	0
#define MAP_SHARED	0


// This is an incomplete implementation of mmap just for GoFish
// start, prot, flags, and offset args ignored
void *mmap(void *start,  size_t length, int prot , int flags, int fd, off_t offset)
{
	char *buf;

	if((buf = malloc(length)) == NULL) return NULL;

	lseek(fd, 0, SEEK_SET);
	if(read(fd, buf, length) != length) {
		free(buf);
		return NULL;
	}

	return buf;
}

int munmap(void *start, size_t length)
{
	free(start);
	return 0;
}

#endif

#ifdef ALLOW_NON_ROOT
int checkpath(char *path)
{
#if 0
	// This does not work in a chroot environment
	char full[PATH_MAX + 2];
	char real[PATH_MAX + 2];
	int  len = strlen(root_dir);

	strcpy(full, root_dir);
	strcat(full, "/");
	strncat(full, path, PATH_MAX - len);
	full[PATH_MAX] = '\0';

	if(!realpath(full, real)) {
		return 1;
	}

	if(strncmp(real, root_dir, len)) {
		errno = EACCES;
		return -1;
	}
#else
	// A .. at the end is safe since it will never specify a file,
	// only a directory.
	if(strncmp(path, "../", 3) == 0 || (int)strstr(path, "/../")) {
		errno = EACCES;
		return -1;
	}
#endif

	return 0;
}
#endif


// This handles parsing the name and opening the file
// We allow the following /?[019]/?<path> || nothing
static int smart_open(char *name)
{
	char type;

	if(*name == '/') ++name;
	type = *name++;
	if(type && *name == '/') ++name;

#ifdef ALLOW_NON_ROOT
	if(checkpath(name)) return -1;
#endif

	switch(type) {
	case '\0':
		return open(".cache", O_RDONLY);
	case '0':
	case '9':
		return open(name, O_RDONLY);
	case '1':
	{
		char dirname[MAX_LINE + 10], *p;

		strcpy(dirname, name);
		p = dirname + strlen(dirname);
		if(*(p - 1) != '/') *p++ = '/';
		strcpy(p, ".cache");
		return open(dirname, O_RDONLY);
	}
	default:
		errno = EINVAL;
		return -1;
	}
}


void close_request(struct connection *conn, int status)
{
	if(verbose) printf("Close request\n");

	// Log hits in one place
	log_hit(conn, status);

	// Send errors in one place also
	if(status != 200) send_error(conn, status);

	// Note: conn[0] never has memory allocated
	if(conn->conn_n > MIN_REQUESTS && conn->cmd) {
		free(conn->cmd);
		conn->cmd = NULL;
	}

	conn->len = conn->offset = 0;

	if(conn->buf) {
		munmap(conn->buf, conn->len);
		conn->buf = NULL;
	}

#ifdef HAVE_POLL
	if(conn->ufd->fd) close(conn->ufd->fd);
	memset(conn->ufd, 0, sizeof(struct pollfd));
	while(npoll > 1 && ufds[npoll - 1].fd == 0) --npoll;
#else
	if(conn->sock) {
		close(conn->sock);
		FD_CLR(conn->sock, &readfds);
		FD_CLR(conn->sock, &writefds);
		if(conn->sock >= nfds - 1)
			for(nfds = conn->sock - 1; nfds > 0; --nfds)
				if(ISSET(nfds, &readfds) || ISSET(nfds, &writefds)) {
					nfds++;
					break;
				}
		conn->sock = 0;
	}
#endif

#ifdef USE_HTTP
	if(conn->http_header) {
		free(conn->http_header);
		conn->http_header = NULL;
	}
	if(conn->outname) {
		if(unlink(conn->outname))
			syslog(LOG_WARNING, "unlink %s: %m", conn->outname);
		free(conn->outname);
		conn->outname = NULL;
	}
	conn->html_header  = NULL;
	conn->html_trailer = NULL;
#endif

	conn->status = 200;
}


int new_connection(int csock)
{
	int sock;
	unsigned addr;
	int i;
	struct connection *conn;


	if((sock = accept_socket(csock, &addr)) <= 0) {
		syslog(LOG_WARNING, "Accept connection: %m");
		return -1;
	}

	// Find a free connection
	// We have one extra sentinel at the end for error replies
	conn = &conns[1];
	for(i = 1; i < MAX_REQUESTS; ++i, ++conn)
		if(SOCKET(conn) == 0) {
			set_readable(conn, sock);
			break;
		}

	if(i > max_requests) max_requests = i;

	conn->addr   = addr;
	conn->offset = 0;
	conn->len    = 0;

	if(i == MAX_REQUESTS - 1) {
		syslog(LOG_WARNING, "Too many requests.");
		close_request(conn, 503);
		return -1;
	}

	if(fcntl(sock, F_SETFL, O_NONBLOCK)) {
		close_request(conn, 500);
		return -1;
	}

	if(!conn->cmd && !(conn->cmd = malloc(MAX_LINE + 1))) {
		close_request(conn, 503);
		return -1;
	}

	return 0;
}


int read_request(struct connection *conn)
{
	int fd;
	int n;
	char *p;

	n = read(SOCKET(conn), conn->cmd + conn->offset, MAX_LINE - conn->offset);

	if(n <= 0) {
		// If we can't read, we probably can't write ....
		syslog(LOG_ERR, "Read error %m");
		close_request(conn, 408);
		return 1;
	}

	conn->offset += n;

	// We alloced an extra space for the '\0'
	conn->cmd[conn->offset] = '\0';

	if(conn->cmd[conn->offset - 1] != '\n') {
		if(conn->offset >= MAX_LINE) {
			close_request(conn, 414);
			return 1;
		}
		return 0; // not an error
	}

	conn->cmd[conn->offset - 1] = '\0';
	if(conn->offset > 1 && conn->cmd[conn->offset - 2] == '\r')
		conn->cmd[conn->offset - 2] = '\0';

	if(verbose > 2) printf("Request '%s'\n", conn->cmd);

	// For gopher+ clients - ingore $ command
	// Got this idea from floodgap.com
	if((p = strchr(conn->cmd, '\t'))) {
		strcpy(conn->cmd, "0/.gopher+");
	}

	seteuid(uid);
	fd = smart_open(conn->cmd);
	seteuid(real_uid);

	if(fd < 0) {
		if(strncmp(conn->cmd, "GET ", 4) == 0 ||
		   strncmp(conn->cmd, "HEAD ", 5) == 0) {
			// Someone did an http://host:70/
			if((fd = http_get(conn)) < 0) {
				close_request(conn, 404);
				return 1;
			}
		}
		else {
			close_request(conn, 404);
			return 1;
		}
	}

	conn->len = lseek(fd, 0, SEEK_END);

	conn->buf = mmap(NULL, conn->len, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);

	if(conn->buf == NULL) {
		syslog(LOG_ERR, "mmap: %m");
		close_request(conn, 408);
		return 1;
	}

	memset(conn->iovs, 0, sizeof(conn->iovs));

	if(conn->http_header) {
		conn->iovs[0].iov_base = conn->http_header;
		conn->iovs[0].iov_len  = strlen(conn->http_header);
	}

	if(conn->html_header) {
		conn->iovs[1].iov_base = conn->html_header;
		conn->iovs[1].iov_len  = strlen(conn->html_header);
	}

	conn->iovs[2].iov_base = conn->buf;
	conn->iovs[2].iov_len  = conn->len;

	if(conn->html_trailer) {
		conn->iovs[3].iov_base = conn->html_trailer;
		conn->iovs[3].iov_len  = strlen(conn->html_trailer);
	}
	else if(*conn->cmd != '9') {
		// SAM does not handle neednl
		conn->iovs[3].iov_base = ".\r\n";
		conn->iovs[3].iov_len  = 3;
	}

	// already is iovs[2].iov_len
	conn->len +=
		conn->iovs[0].iov_len +
		conn->iovs[1].iov_len +
		conn->iovs[3].iov_len;

	set_writeable(conn);

	return 0;
}


int write_request(struct connection *conn)
{
	int n, i;
	struct iovec *iov;

	n = writev(SOCKET(conn), conn->iovs, 4);

	if(n <= 0) {
		syslog(LOG_ERR, "writev: %m");
		close_request(conn, 408);
		return 1;
	}

	for(iov = conn->iovs, i = 0; i < 4; ++i, ++iov)
		if(n >= iov->iov_len) {
			n -= iov->iov_len;
			iov->iov_len = 0;
		}
		else {
			iov->iov_len -= n;
			return 0;
		}

	close_request(conn, conn->status);

	return 0;
}


void create_pidfile(char *fname)
{
	FILE *fp;
	int n;
	int pid;

	if((fp = fopen(fname, "r"))) {
		n = fscanf(fp, "%d\n", &pid);
		fclose(fp);

		if(n == 1) {
			if(kill(pid, 0) == 0) {
				syslog(LOG_ERR, "gopherd already running (pid = %d)", pid);
				exit(1);
			}
		} else {
			syslog(LOG_ERR, "Unable to read %s", fname);
			exit(1);
		}
	} else if(errno != ENOENT) {
		syslog(LOG_ERR, "Open %s: %m", fname);
		exit(1);
	}

	if((fp = fopen(fname, "w")) == NULL) {
		syslog(LOG_ERR, "Create %s: %m", fname);
		exit(1);
	}

	pid = getpid();
	fprintf(fp, "%d\n", pid);

	fclose(fp);
}
