/*
 * socket.c - socket utilities for the gofish gopher daemon
 * Copyright (C) 2000,2002  Sean MacLennan <seanm@seanm.ca>
 * $Revision: 1.9 $ $Date: 2002/10/05 02:06:36 $
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this project; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */
/*
 * All knowledge of sockets should be isolated to this file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "gopherd.h"


int listen_socket(int port)
{
 	struct sockaddr_in sock_name;
	int s, optval;

 	sock_name.sin_family = AF_INET;
	sock_name.sin_addr.s_addr = htonl(INADDR_ANY);
	sock_name.sin_port = htons(port);
	optval = 1;

	if((s = socket (AF_INET, SOCK_STREAM, 0)) == -1)
		return -1;

	if(setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
				  (char *)&optval, sizeof (optval)) == -1 ||
	   bind (s, (struct sockaddr *)&sock_name, sizeof(sock_name)) == -1 ||
	   listen(s, GOPHER_BACKLOG) == -1) {
		close(s);
		return -1;
	}

	return s;
}

int accept_socket(int sock, unsigned *addr)
{
	struct sockaddr_in sock_name;
	int addrlen = sizeof(sock_name);
	int new;

	if((new = accept (sock, (struct sockaddr *)&sock_name, &addrlen)) >= 0)
		if(addr)
			*addr = htonl(sock_name.sin_addr.s_addr);

	return new;
}


// network byte order
char *ntoa(unsigned n)
{
	static char a[16];

	sprintf(a, "%d.%d.%d.%d",
			(n >> 24) & 0xff,
			(n >> 16) & 0xff,
			(n >>  8) & 0xff,
			n & 0xff);

	return a;
}
