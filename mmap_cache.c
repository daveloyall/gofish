/*
 * mmap-cache.c - GoFish mmap caching for performance
 * Copyright (C) 2002 Sean MacLennan <seanm@seanm.ca>
 * $Revision: 1.5 $ $Date: 2002/10/21 00:31:45 $
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

/* All knowledge of mmap is isolated to this file. */

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

#ifdef HAVE_MMAP
#include <sys/mman.h>
#else

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

#ifdef USE_CACHE

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


void mmap_release(struct connection *conn)
{
	struct cache *m;
	int i;

	for(i = 0, m = mmap_cache; i < MMAP_CACHE; ++i, ++m)
		if(m->mapped == conn->buf) {
			m->in_use--;
			return;
		}

	printf("PROBLEMS: buffer not in cache\n");
}

#else

void mmap_init(void) {}


unsigned char *mmap_get(struct connection *conn, int fd)
{
	return mmap(NULL, conn->len, PROT_READ, MAP_SHARED, fd, 0);
}

void mmap_release(struct connection *conn)
{
	munmap(conn->buf, conn->len);
}

#endif // USE_CACHE
