/*
 * gopherd.c - the mainline for the gofish gopher daemon
 * Copyright (C) 2002 Sean MacLennan <seanm@seanm.ca>
 * $Revision: 1.4 $ $Date: 2002/08/26 02:35:42 $
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

#ifdef HAVE_LINUX_SENDFILE
#include <sys/sendfile.h>
#endif

int verbose = 0;

struct connection conns[MAX_REQUESTS];

#ifdef HAVE_POLL
static struct pollfd ufds[MAX_REQUESTS];
static int npoll;

static void start_polling(int csock);
#else
static fd_set readfds, writefds;
static int nfds;

static void start_selecting(int csock);
#endif


uid_t real_uid;

// forward references
static void gopherd(void);
static void create_pidfile(char *fname);
static int new_connection(int csock);
static int read_request(struct connection *conn);
static int write_request(struct connection *conn);


void sighandler(int signum)
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
	int csock;

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
		printf("Unable to create listen socket\n");
		syslog(LOG_ERR, "Unable to create socket\n");
		exit(1);
	}

	memset(conns, 0, sizeof(conns));

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
				close_request(&conns[i]);
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

#if !defined(HAVE_SENDFILE) && !defined(HAVE_MMAP)
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


void close_request(struct connection *conn)
{
	if(verbose) printf("Close request\n");

#ifdef HAVE_SENDFILE
	if(conn->fd) {
		close(conn->fd);
		conn->fd = 0;
	}
#else
	if(conn->buf) {
		munmap(conn->buf, conn->len);
		conn->buf = NULL;
	}
#endif

#ifdef HAVE_POLL
	if(conn->ufd->fd) {
		close(conn->ufd->fd);
		conn->ufd->fd = 0;
	}
	conn->ufd->events = 0;
	conn->ufd->revents = 0;

	while(npoll > 1 && ufds[npoll - 1].fd == 0) --npoll;
#else
	if(conn->sock) {
		close(conn->sock);
		FD_CLR(conn->sock, &readfds);
		FD_CLR(conn->sock, &writefds);
		// SAM decrement nfds?
		conn->sock = 0;
	}
#endif

	if(conn->cmd) {
		free(conn->cmd);
		conn->cmd = NULL;
	}

#ifdef USE_HTTP
	if(conn->hdr) {
		free(conn->hdr);
		conn->hdr = NULL;
	}
	if(conn->outname) {
		if(unlink(conn->outname))
			syslog(LOG_WARNING, "unlink %s: %m", conn->outname);
		free(conn->outname);
		conn->outname = NULL;
	}
#endif

	conn->len = conn->offset = 0;
}


int new_connection(int csock)
{
	int sock;
	unsigned addr;
	int i;
	char *cmd;

	if((sock = accept_socket(csock, &addr)) <= 0) {
		syslog(LOG_WARNING, "Accept connection: %m");
		return -1;
	}

	if(fcntl(sock, F_SETFL, O_NONBLOCK)) {
		syslog(LOG_ERR, "fcntl %m");
		send_errno(sock, "Server error", errno);
		close(sock);
		return -1;
	}

	if((cmd = malloc(MAX_LINE + 1)) == NULL) {
		syslog(LOG_ERR, "Out of memory");
		send_error(sock, "Not enough resources.");
		close(sock);
		return -1;
	}

	// Find a free connection
#ifdef HAVE_POLL
	for(i = 1; i < MAX_REQUESTS; ++i)
		if(ufds[i].fd == 0) {
			ufds[i].fd = sock;
			ufds[i].events = POLLIN;

			conns[i].cmd = cmd;
			conns[i].offset = 0;
			conns[i].len = MAX_LINE;

			conns[i].addr = addr;
			if(i + 1 > npoll) npoll = i + 1;
			if(verbose)
				printf("Accepted connection %d from %s\n", i, ntoa(addr));
			return 0;
		}
#else
	for(i = 1; i < MAX_REQUESTS; ++i)
		if(conns[i].sock == 0) {
			conns[i].sock = sock;
			FD_SET(sock, &readfds);

			conns[i].cmd = cmd;
			conns[i].offset = 0;
			conns[i].len = MAX_LINE;

			conns[i].addr = addr;
			if(sock + 1 > nfds) nfds = sock + 1;
			if(verbose)
				printf("Accepted connection %d from %s\n", i, ntoa(addr));
			return 0;
		}
#endif

	syslog(LOG_WARNING, "Too many requests.");

	close(sock);
	return 1;
}

int read_request(struct connection *conn)
{
	int fd;
	int n;
	char *p;

	n = read(SOCKET(conn), conn->cmd + conn->offset, conn->len - conn->offset);

	if(n <= 0) {
		// If we can't read, we probably can't write ....
		syslog(LOG_ERR, "Read error %m");
		send_error(SOCKET(conn), "Unable to read request");
		log_hit(conn->addr, conn->cmd, 414, 0);
		close_request(conn);
		return 1;
	}

	conn->offset += n;

	// We alloced an extra space for the '\0'
	conn->cmd[conn->offset] = '\0';

	// Look for the first \n in case we got a multiline http GET command
	if((p = strchr(conn->cmd, '\n')) == 0) {
		if(conn->offset >= conn->len) {
			syslog(LOG_ERR, "Line too long.");
			send_error(SOCKET(conn), "Line too long");
			log_hit(conn->addr, conn->cmd, 414, 0);
			close_request(conn);
			return 1;
		}
		return 0; // not an error
	}

	*p-- = '\0';
	if(conn->offset > 1 && *p == '\r') *p = '\0';

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
				send_errno(SOCKET(conn), conn->cmd, errno);
				log_hit(conn->addr, conn->cmd + 4, 404, 0);
				close_request(conn);
				return 1;
			}
		}
		else {
			log_hit(conn->addr, conn->cmd, 404, 0);
			send_errno(SOCKET(conn), conn->cmd, errno);
			close_request(conn);
			return 1;
		}
	}

#ifdef HAVE_SENDFILE
	conn->fd = fd;

	conn->len = lseek(fd, -1, SEEK_END) + 1;
	if(*conn->cmd == '9')
		conn->neednl = 0;
	else {
		char c;
		conn->neednl = read(fd, &c, 1) == 1 && c != '\n';
	}
#else
	conn->len = lseek(fd, 0, SEEK_END);

	conn->buf = mmap(NULL, conn->len, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);

	if(conn->buf == NULL) {
		syslog(LOG_ERR, "mmap: %m");
		send_error(SOCKET(conn), "Out of resources");
		log_hit(conn->addr, conn->cmd, 414, 0);
		close_request(conn);
		return 1;
	}
#endif

	conn->offset = 0;

#ifdef HAVE_POLL
	conn->ufd->events = POLLOUT;
#else
	FD_CLR(conn->sock, &readfds);
	FD_SET(conn->sock, &writefds);
#endif

	return 0;
}


int write_request(struct connection *conn)
{
	int n;
#ifdef HAVE_BSD_SENDFILE
	/* You must pass in an address, even if all 0s */
	struct sf_hdtr hdr;

	memset(&hdr, 0, sizeof(hdr));
	if(conn->hdr) {
		hdr.headers = conn->hdr;
		hdr.hdr_cnt = 1;
	}

	if(sendfile(conn->fd, SOCKET(conn), conn->offset, conn->len - conn->offset,
				&hdr, &n, 0) == 0 && n > 0)
		// success
		conn->offset += n;
	else if(errno == EAGAIN && n > 0) {
		// success - we just need to poll again
		if(conn->hdr) {
			if(n >= conn->hdr->iov_len) {
				conn->offset += n - conn->hdr->iov_len;
				free(conn->hdr);
				conn->hdr = NULL;
			}
			else {
				// This should never happen
				conn->hdr->iov_base += n;
				conn->hdr->iov_len  -= n;
			}
		}
		else
			conn->offset += n;
	} else
		// failure
		n = -1;
#else // !USE_BSD_SENDFILE
#ifdef USE_HTTP
	if(conn->hdr)
		return http_send_response(conn);
#endif

#ifdef HAVE_LINUX_SENDFILE
	n = sendfile(SOCKET(conn), conn->fd, &conn->offset, conn->len - conn->offset);
#else
	n = write(SOCKET(conn),
			  conn->buf + conn->offset,
			  conn->len - conn->offset);
	conn->offset += n;
#endif
#endif // USE_BSD_SENDFILE

	if(n <= 0) {
		send_errno(SOCKET(conn), conn->cmd, errno);
		log_hit(conn->addr, conn->cmd, 500, 0);
		close_request(conn);
		return 1;
	}

	if(conn->offset >= conn->len) {
		if(*conn->cmd != '9') {
			int neednl;

#ifdef HAVE_SENDFILE
			neednl = conn->neednl;
#else
			neednl = conn->buf[conn->len - 1] != '\n';
#endif
			if(neednl)
				write(SOCKET(conn), "\n.\r\n", 4);
			else
				write(SOCKET(conn), ".\r\n", 3);
		}

		log_hit(conn->addr, conn->cmd, 200, conn->len);
		close_request(conn);
	}

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


#ifndef USE_HTTP
int http_init() { return 0; }

int http_get(struct connection *conn)
{
	log_hit(conn->addr, conn->cmd + 4, 501, 0);
	send_http_error(SOCKET(conn));
	return -1;
}
#endif
