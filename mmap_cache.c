/*
 * mmap-cache.c - GoFish mmap caching for performance
 * Copyright (C) 2002 Sean MacLennan <seanm@seanm.ca>
 * $Revision: 1.3 $ $Date: 2002/10/18 04:21:18 $
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
#include <sys/stat.h>

#include "gopherd.h"
#include "version.h"

#ifdef USE_CACHE
#ifdef HAVE_MMAP
#include <sys/mman.h>
#endif

#define MMAP_CACHE	150 	// SAM must be >= MAX_REQUESTS

struct cache {
//	char *request; // SAM HACK
	unsigned char *mapped;
	int len;
	time_t time;
	time_t mtime;
	ino_t ino;
	int in_use;
};

static struct cache mmap_cache[MMAP_CACHE];


void mmap_init()
{
	int i;

	memset(mmap_cache, 0, sizeof(mmap_cache));

	// this is hacky but it gets the lrus in order
	for(i = 0; i < MMAP_CACHE; ++i)
		mmap_cache[i].time = i;
}


unsigned char *mmap_get(struct connection *conn, int fd)
{
	int i;
	struct cache *lru, *m;
	time_t t = 0x7fffffff; // SAM
	struct stat sbuf;

	if(fstat(fd, &sbuf)) {
		perror("fstat");
		return NULL;
	}

	lru = NULL;
	for(i = 0, m = mmap_cache; i < MMAP_CACHE; ++i, ++m)
		if(sbuf.st_ino == m->ino) { // can we have a zero ino?
			// match
			if(sbuf.st_mtime != m->mtime) continue; // SAM cleanup?
			m->in_use++;
			time(&m->time);
			return m->mapped;
		} else if(m->in_use == 0) {
			if(m->time < t) {
				t = m->time;
				lru = m;
			}
		}

	// no matches

	if(lru == NULL) {
		printf("REAL PROBLEMS: no lru!!!\n");
		return NULL;
	}

	if(lru->mapped)
		munmap(lru->mapped, lru->len);


	lru->mapped = mmap(NULL, conn->len, PROT_READ, MAP_SHARED, fd, 0);
	if(lru->mapped == NULL) {
		perror("REAL PROBLEMS: mmap failed!!");
		return NULL;
	}

	// SAM lru->request = strdup(conn->cmd); // SAM
	lru->ino = sbuf.st_ino;
	lru->len = conn->len;
	time(&lru->time);
	lru->in_use = 1;
	lru->mtime = sbuf.st_mtime;

	return lru->mapped;
}


void mmap_release(unsigned char *buf)
{
	struct cache *m;
	int i;

	for(i = 0, m = mmap_cache; i < MMAP_CACHE; ++i, ++m)
		if(m->mapped == buf) {
			m->in_use--;
			return;
		}

	printf("PROBLEMS: buffer not in cache\n");
}

#endif // USE_CACHE
