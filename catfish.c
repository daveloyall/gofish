/*
 * catfish.c - a cgi server for the gofish web server
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
#include <stdlib.h>
#include <sys/types.h>
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
#include <pwd.h>
#include <grp.h>

#include "gofish.h"
#include "version.h"

int verbose = 0;

// Stats
unsigned max_requests = 0;
unsigned max_length = 0;
unsigned n_requests = 0;
int      n_connections = 0; // yes signed, I want to know if it goes -ve


// Add an extra connection for error replies
static struct connection conns[MAX_REQUESTS];

// From cgi.c
int cgi_get(struct connection *conn);

#ifdef HAVE_POLL
static struct pollfd ufds[MAX_REQUESTS];
static int npoll;

static void start_polling(int csock);
#else
static fd_set readfds, writefds;
static int nfds;

static void start_selecting(int csock);

void set_writeable(struct connection *conn)
{
	FD_CLR(conn->sock, &readfds);
	FD_SET(conn->sock, &writefds);
}
#endif


static uid_t root_uid;

// forward references
static void gofish(char *name);
static void create_pidfile(char *fname);
static int new_connection(int csock);
static int read_request(struct connection *conn);
static int write_request(struct connection *conn);
static int gofish_stats(struct connection *conn);
static void check_old_connections(void);


// SIGUSR1 is handled in log.c
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
		if(SOCKET(conn) != -1) close_connection(conn, 500);
		if(conn->cmd) free(conn->cmd);
	}

	free(root_dir);
	free(hostname);
	free(logfile);
	free(pidfile);

	mime_cleanup();
}


int main(int argc, char *argv[])
{
	char *config = GOPHER_CONFIG;
	pid_t pid;
	int c, daemon = 0;
	char *prog;

	if((prog = strrchr(argv[0], '/')))
		++prog;
	else
		prog = argv[0];

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

	mime_init();

	http_init();

	if(daemon) {
		if((pid = fork()) == 0) {
			gofish(prog); // never returns
			exit(1);	// paranoia
		}
		return pid ==-1 ? 1 : 0; // parent exits
	}
	else
		gofish(prog); // never returns
	return 1; // compiler shutup
}

static void
setup_privs (void)
{
	struct passwd *pwd = NULL;

	root_uid = getuid();

	if(uid == (uid_t)-1 || gid == (uid_t)-1) {
		pwd = getpwnam(strdup(user));
		if(!pwd) {
			syslog(LOG_ERR, "No such user: `%s'.", user);
			exit(1);
		}
		if(uid == (uid_t)-1)
			uid = pwd->pw_uid;
		if(gid == (uid_t)-1)
			gid = pwd->pw_gid;
	}
	if(pwd)
		initgroups (pwd->pw_name, pwd->pw_gid);
	setgid(gid);
}

void gofish(char *name)
{
	int csock, i;

	openlog(name, LOG_CONS, LOG_DAEMON);
	syslog(LOG_INFO, "GoFish " GOFISH_VERSION " (%s) starting.", name);
	log_open(logfile);

	// Create *before* chroot
	create_pidfile(pidfile);

	if(chdir(root_dir)) {
		perror(root_dir);
		exit(1);
	}

	setup_privs();

#if 0
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
#endif

	signal(SIGHUP,  sighandler);
	signal(SIGTERM, sighandler);
	signal(SIGINT,  sighandler);
	signal(SIGPIPE, sighandler);

	// connection socket
	if((csock = listen_socket(port)) < 0) {
		syslog(LOG_ERR, "Unable to create socket: %m");
		exit(1);
	}

	seteuid(uid);

	memset(conns, 0, sizeof(conns));
	for(i = 0; i < MAX_REQUESTS; ++i) {
		conns[i].status = 200;
		conns[i].conn_n = i;
	}

	mmap_init();

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
	int i, n;
	int timeout;

	memset(ufds, 0, sizeof(ufds));

	for(i = 0; i < MAX_REQUESTS; ++i) {
		ufds[i].fd = -1;
		conns[i].ufd = &ufds[i];
	}

	// Now it is safe to install
	atexit(cleanup);

	ufds[0].fd = csock;
	ufds[0].events = POLLIN;
	npoll = 1;

	while(1) {
		timeout = n_connections ? (POLL_TIMEOUT * 1000) : -1;
		if((n = poll(ufds, npoll, timeout)) < 0) {
			syslog(LOG_WARNING, "poll: %m");
			continue;
		}

		/* Simplistic timeout to start with.
		 * Only check for old connections on a timeout.
		 * Low overhead, but under high load may leave connections
		 * around longer.
		 */
		if(n == 0) {
			check_old_connections();
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
			} else if(ufds[i].revents & POLLOUT) {
				write_request(&conns[i]);
				--n;
			}
			else if(ufds[i].revents) {
				// Error
				int status;

				if(ufds[i].revents & POLLHUP) {
					syslog(LOG_DEBUG, "Connection hung up");
					status = 504;
				} else if(ufds[i].revents & POLLNVAL) {
					syslog(LOG_DEBUG, "Connection invalid");
					status = 410;
				} else {
					syslog(LOG_DEBUG, "Revents = 0x%x", ufds[i].revents);
					status = 501;
				}

				close_connection(&conns[i], status);
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
	struct timeval *timeout, timeoutval;

	for(n = 0; n < MAX_REQUESTS; ++n) conns[n].sock = -1;

	FD_ZERO(&readfds);
	FD_ZERO(&writefds);

	FD_SET(csock, &readfds);
	nfds = csock + 1;

	atexit(cleanup);

	timeoutval.tv_sec  = POLL_TIMEOUT;
	timeoutval.tv_usec = 0;

	while(1) {
		memcpy(&cur_reads,  &readfds, sizeof(fd_set));
		memcpy(&cur_writes, &writefds, sizeof(fd_set));

		timeout = n_connections ? &timeoutval : NULL;
		if((n = select(nfds, &cur_reads, &cur_writes, NULL, timeout)) < 0) {
			syslog(LOG_WARNING, "select: %m");
			continue;
		}

		/* Simplistic timeout to start with.
		 * Only check for old connections on a timeout.
		 * Low overhead, but under high load may leave connections
		 * around longer.
		 */
		if(n == 0) {
			check_old_connections();
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
// We allow the following /?<selector>/<path> || nothing
// Returns the selector type in `selector'
int smart_open(char *name, char *selector)
{
	char type;

	if(*name == '/') ++name;

	// This is worth opimizing
	if(*name == '\0') {
		*selector = '1';
		return open(".cache", O_RDONLY);
	}

	type = *name++;
	if(*name == '/')
		++name;
	else {
		errno = EINVAL;
		return -1;
	}

#ifdef ALLOW_NON_ROOT
	if(checkpath(name)) return -1;
#endif

	*selector = type;

	switch(type) {
	case '0':
	case '4':
	case '5':
	case '6':
	case '9':
	case 'g':
	case 'h':
	case 'I':
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


void close_connection(struct connection *conn, int status)
{
	if(verbose > 2) printf("Close request\n");

	--n_connections;

	if(status != 200 && conn->cmd) {
		// Make sure errors have a clean cmd
		char *p;

		for(p = conn->cmd; *p && *p != '\r' && *p != '\n'; ++p) ;
		*p = '\0';

		if(strncmp(conn->cmd, "GET ", 4) == 0 ||
		   strncmp(conn->cmd, "HEAD ", 5) == 0)
			conn->http = 1;
	}

	// Log hits in one place. Do not log stat requests.
	if(status != 1000) {
		log_hit(conn, status);

		// Send gopher errors in one place also
		if(status != 200 && status != 504 && !conn->http) {
			syslog(LOG_WARNING, "Gopher error %d http %d\n",
				   status, conn->http);
			send_error(conn, status);
		}
	}

	// Note: conn[0] never has memory allocated
	if(conn->conn_n > MIN_REQUESTS && conn->cmd) {
		free(conn->cmd);
		conn->cmd = NULL;
	}

	conn->len = conn->offset = 0;

	if(conn->buf) {
		mmap_release(conn);
		conn->buf = NULL;
	}

	if(SOCKET(conn) >= 0) {
		close(SOCKET(conn));
#ifdef HAVE_POLL
		conn->ufd->fd = -1;
		conn->ufd->revents = 0;
		while(npoll > 1 && ufds[npoll - 1].fd == -1) --npoll;
#else
		FD_CLR(conn->sock, &readfds);
		FD_CLR(conn->sock, &writefds);
		if(conn->sock >= nfds - 1)
			for(nfds = conn->sock - 1; nfds > 0; --nfds)
				if(FD_ISSET(nfds, &readfds) || FD_ISSET(nfds, &writefds)) {
					nfds++;
					break;
				}
		conn->sock = -1;
#endif
	}

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

	conn->http = 0;

	conn->status = 200;

	memset(conn->iovs, 0, sizeof(conn->iovs));
}


int new_connection(int csock)
{
	int sock;
	unsigned addr;
	int i;
	struct connection *conn;

	seteuid(root_uid);

	while(1) {
		if((sock = accept_socket(csock, &addr)) < 0) {
			seteuid(uid);

			if(errno == EWOULDBLOCK)
				return 0;

			syslog(LOG_WARNING, "Accept connection: %m");
			return -1;
		}

		// Find a free connection
		// We have one extra sentinel at the end for error replies
		for(conn = &conns[1], i = 1; i < MAX_REQUESTS; ++i, ++conn)
			if(SOCKET(conn) == -1) {
				set_readable(conn, sock);
				break;
			}

		// Set *before* any closes
		++n_connections;
		++n_requests;
		if(i > max_requests) max_requests = i;

		conn->addr   = addr;
		conn->offset = 0;
		conn->len    = 0;
		time(&conn->access);

		if(i == MAX_REQUESTS - 1) {
			syslog(LOG_WARNING, "Too many requests.");
			close_connection(conn, 403);
		}
		else if(!conn->cmd && !(conn->cmd = malloc(MAX_LINE + 1))) {
			syslog(LOG_WARNING, "Out of memory.");
			close_connection(conn, 503);
		}
	}
}


/* SAM We are going to have to split the http/gopher streams earlier.
 * I think as soon as we see the GET/HEAD we should split and do more
 * intelligent reading on the http side of things.
 */
int read_request(struct connection *conn)
{
// SAM	int fd;
	int n;
// SAM	char *p, selector;

	n = read(SOCKET(conn), conn->cmd + conn->offset, MAX_LINE - conn->offset);

	if(n <= 0) {
		// SAM	This is evil for gopher anyway....
// SAM		if(errno == EAGAIN) {
// SAM			printf("EAGAIN\n");
// SAM			return 0;
// SAM		}
		if(n == 0)
			syslog(LOG_WARNING, "Read: unexpected EOF");
		else
			syslog(LOG_WARNING, "Read error (%d): %m", errno);
		close_connection(conn, 408);
		return 1;
	}

	conn->offset += n;
	time(&conn->access);

	// We alloced an extra space for the '\0'
	conn->cmd[conn->offset] = '\0';

	if(conn->cmd[conn->offset - 1] != '\n') {
		if(conn->offset >= MAX_LINE) {
			syslog(LOG_WARNING, "Line overflow");
			if(strncmp(conn->cmd, "GET ",  4) == 0 ||
			   strncmp(conn->cmd, "HEAD ", 5) == 0)
				return http_error(conn, 414);
			else {
				close_connection(conn, 414);
				return 1;
			}
		}
		return 0; // not an error
	}

	if(conn->offset > max_length) max_length = conn->offset;

	if(strcmp(conn->cmd, "STATS\r\n") == 0)
		return gofish_stats(conn);

	// We must look for \r\n\r\n
	// This is mainly for telnet sessions
	if(strstr(conn->cmd, "\r\n\r\n")) {
		if(verbose > 2) printf("Http: %s\n", conn->cmd);
		return cgi_get(conn);
	}
	conn->http = 1;
	return 0;
}


int write_request(struct connection *conn)
{
	int n, i;
	struct iovec *iov;

	n = writev(SOCKET(conn), conn->iovs, conn->n_iovs);

	if(n <= 0) {
		syslog(LOG_ERR, "writev: %m");
		close_connection(conn, 408);
		return 1;
	}

	for(iov = conn->iovs, i = 0; i < conn->n_iovs; ++i, ++iov)
		if(n >= iov->iov_len) {
			n -= iov->iov_len;
			iov->iov_len = 0;
		}
		else {
			iov->iov_len -= n;
			iov->iov_base += n;
			time(&conn->access);
			return 0;
		}

	close_connection(conn, conn->status);

	return 0;
}


void check_old_connections(void)
{
	struct connection *c;
	int i;
	time_t checkpoint;

	checkpoint = time(NULL) - MAX_IDLE_TIME;

	// Do not close the listen socket
	for(c = &conns[1], i = 1; i < MAX_REQUESTS; ++i, ++c)
		if(SOCKET(c) >= 0 && c->access < checkpoint) {
			// SAM What about http connections?
			syslog(LOG_DEBUG, "%s: Killing idle connection.", ntoa(c->addr));
			close_connection(c, 408);
		}
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


static int gofish_stats(struct connection *conn)
{
	extern unsigned bad_munmaps;
	char buf[200];

	sprintf(buf,
			"GoFish " GOFISH_VERSION "\r\n"
			"Requests:     %10u\r\n"
			"Max parallel: %10u\r\n"
			"Max length:   %10u\r\n"
			"Connections:  %10d\r\n",
			n_requests, max_requests, max_length,
			// we are an outstanding connection
			n_connections - 1);

	if(bad_munmaps) {
		char *p = buf + strlen(buf);
		sprintf(p, "BAD UNMAPS:   %10u\r\n", bad_munmaps);
	}

	// SAM safe? It's < 150 bytes...
	write(SOCKET(conn), buf, strlen(buf));

	close_connection(conn, 1000);

	return 0;
}
