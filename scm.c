/**
 * Tony Givargis
 * Copyright (C), 2023-2026
 * University of California, Irvine
 *
 * CS 238P - Operating Systems
 * scm.c
 */

#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include "scm.h"

/**
 * Needs:
 *   fstat()
 *   S_ISREG()
 *   open()
 *   close()
 *   sbrk()
 *   mmap()
 *   munmap()
 *   msync()
 */


#define SCM_MAGIC ((uint64_t)0x53434d31u) /* "SCM1" */
#define SCM_CAP   ((size_t)64 << 20)      /* virtual heap size */
#define SCM_GAP   ((size_t)256 << 20)     /* hole above the libc heap */
#define ALIGNMENT ((size_t)sizeof (void *))

struct block {
	size_t size;          /* whole block, including this header */
	struct block *next;   /* free list link; overlaps user bytes */
};

struct meta {
	uint64_t magic;
	uint64_t capacity;    /* mapped bytes */
	uint64_t cursor;      /* bump offset from base */
	uint64_t live;        /* bytes currently checked out */
	void *base;
	struct block *free_head;
};

struct scm {
	int fd;
	void *map;
	size_t cap;
	struct meta *meta;
};

static size_t
align_up(size_t n)
{
	size_t r;

	r = n % ALIGNMENT;
	if (!r) {
		return n;
	}
	if (n > ((size_t)-1) - (ALIGNMENT - r)) {
		return 0;
	}
	return n + (ALIGNMENT - r);
}

static size_t
heap_off(void)
{
	return align_up(sizeof (struct meta));
}

static size_t
page_align(size_t n)
{
	size_t p;
	size_t r;

	p = page_size();
	r = n % p;
	if (r) {
		n += p - r;
	}
	return n;
}

static size_t
block_size(size_t n)
{
	size_t bytes;

	if (!n || (n > ((size_t)-1) - sizeof (size_t))) {
		return 0;
	}
	bytes = sizeof (size_t) + n;
	if (bytes < sizeof (struct block)) {
		bytes = sizeof (struct block);
	}
	return align_up(bytes);
}

static void *
payload_of(struct block *blk)
{
	return (char *)blk + sizeof (size_t);
}

static struct block *
block_of(void *p)
{
	return (struct block *)((char *)p - sizeof (size_t));
}

static void *
choose_addr(size_t cap)
{
	void *brk;
	void *addr;
	uintptr_t end;

	brk = sbrk(0);
	if ((void *)-1 == brk) {
		TRACE("sbrk()");
		return NULL;
	}
	if ((uintptr_t)brk > ((uintptr_t)-1) - SCM_GAP) {
		TRACE("sbrk()");
		return NULL;
	}
	addr = (char *)brk + SCM_GAP;
	addr = memory_align(addr, page_size());
	end = (uintptr_t)addr + cap;
	if (end < (uintptr_t)addr) {
		TRACE("software");
		return NULL;
	}
	return addr;
}

static void *
map_at(int fd, void *addr, size_t cap)
{
	void *p;
	int flags;

	flags = MAP_SHARED | MAP_FIXED;
#ifdef MAP_FIXED_NOREPLACE
	flags |= MAP_FIXED_NOREPLACE;
#endif
	p = mmap(addr, cap, PROT_READ | PROT_WRITE, flags, fd, 0);
#ifdef MAP_FIXED_NOREPLACE
	if ((MAP_FAILED == p) && (EINVAL == errno)) {
		p = mmap(addr, cap, PROT_READ | PROT_WRITE,
			 MAP_SHARED | MAP_FIXED, fd, 0);
	}
#endif
	if (MAP_FAILED == p) {
		TRACE("mmap()");
		return NULL;
	}
	if (p != addr) {
		munmap(p, cap);
		TRACE("mmap()");
		return NULL;
	}
	return p;
}

static int
meta_ok(const struct meta *m)
{
	uintptr_t base;
	size_t pagesz;

	pagesz = page_size();
	base = (uintptr_t)m->base;
	if (SCM_MAGIC != m->magic) {
		return 0;
	}
	if (!base || (base % pagesz)) {
		return 0;
	}
	if (!m->capacity || (m->capacity % pagesz)) {
		return 0;
	}
	if (m->capacity < (uint64_t)heap_off() + sizeof (struct block)) {
		return 0;
	}
	if (m->capacity > ((uint64_t)4 << 30)) {
		return 0;
	}
	if ((m->cursor < heap_off()) || (m->cursor > m->capacity)) {
		return 0;
	}
	if (m->cursor % ALIGNMENT) {
		return 0;
	}
	if (m->live > m->capacity) {
		return 0;
	}
	return 1;
}

static int
free_head_ok(const struct meta *m)
{
	uintptr_t p;
	uintptr_t lo;
	uintptr_t hi;

	if (!m->free_head) {
		return 1;
	}
	p = (uintptr_t)m->free_head;
	lo = (uintptr_t)m->base + heap_off();
	hi = (uintptr_t)m->base + (uintptr_t)m->capacity;
	if ((p < lo) || (p >= hi) || (p % ALIGNMENT)) {
		return 0;
	}
	return 1;
}

static void *
from_free(struct meta *meta, size_t need)
{
	struct block *prev;
	struct block *cur;
	struct block *rest;

	prev = NULL;
	cur = meta->free_head;
	while (cur) {
		if (cur->size >= need) {
			if ((cur->size - need) >= sizeof (struct block)) {
				rest = (struct block *)((char *)cur + need);
				rest->size = cur->size - need;
				rest->next = cur->next;
				cur->size = need;
				if (prev) {
					prev->next = rest;
				}
				else {
					meta->free_head = rest;
				}
			}
			else {
				if (prev) {
					prev->next = cur->next;
				}
				else {
					meta->free_head = cur->next;
				}
			}
			meta->live += cur->size;
			return payload_of(cur);
		}
		prev = cur;
		cur = cur->next;
	}
	return NULL;
}

static void *
from_bump(struct meta *meta, size_t need)
{
	struct block *blk;

	if ((meta->cursor > meta->capacity) ||
	    ((uint64_t)need > (meta->capacity - meta->cursor))) {
		return NULL;
	}
	blk = (struct block *)((char *)meta->base + (size_t)meta->cursor);
	blk->size = need;
	meta->cursor += need;
	meta->live += need;
	return payload_of(blk);
}

static int
block_ok(const struct meta *meta, struct block *blk)
{
	char *lo;
	char *hi;
	char *p;

	lo = (char *)meta->base + heap_off();
	hi = (char *)meta->base + (size_t)meta->cursor;
	p = (char *)blk;
	if ((p < lo) || (p >= hi)) {
		return 0;
	}
	if ((blk->size < sizeof (struct block)) || (blk->size % ALIGNMENT)) {
		return 0;
	}
	if ((p + blk->size) > hi) {
		return 0;
	}
	return 1;
}

static void
release(struct meta *meta, struct block *blk)
{
	struct block *prev;
	struct block *cur;
	size_t sz;

	prev = NULL;
	cur = meta->free_head;
	while (cur && ((uintptr_t)cur < (uintptr_t)blk)) {
		prev = cur;
		cur = cur->next;
	}
	if (cur == blk) {
		TRACE("double free");
		return;
	}
	sz = blk->size;
	if (meta->live < sz) {
		TRACE("software");
		return;
	}
	meta->live -= sz;
	blk->next = cur;
	if (prev) {
		prev->next = blk;
	}
	else {
		meta->free_head = blk;
	}
	if (cur && ((char *)blk + blk->size == (char *)cur)) {
		blk->size += cur->size;
		blk->next = cur->next;
	}
	if (prev && ((char *)prev + prev->size == (char *)blk)) {
		prev->size += blk->size;
		prev->next = blk->next;
	}
}

void
scm_close(struct scm *scm)
{
	if (!scm) {
		return;
	}
	if (scm->map && (MAP_FAILED != scm->map) && scm->cap) {
		if (msync(scm->map, scm->cap, MS_SYNC)) {
			TRACE("msync()");
		}
		if (munmap(scm->map, scm->cap)) {
			TRACE("munmap()");
		}
	}
	if (0 <= scm->fd) {
		if (close(scm->fd)) {
			TRACE("close()");
		}
	}
	memset(scm, 0, sizeof (struct scm));
	FREE(scm);
}

struct scm *
scm_open(const char *pathname, int truncate)
{
	struct scm *scm;
	struct stat st;
	struct meta hint;
	ssize_t got;
	size_t cap;
	void *addr;
	void *map;
	int flags;

	assert( safe_strlen(pathname) );

	if (!(scm = malloc(sizeof (struct scm)))) {
		TRACE("out of memory");
		return NULL;
	}
	memset(scm, 0, sizeof (struct scm));
	scm->fd = -1;

	flags = O_RDWR | O_CREAT;
	if (truncate) {
		flags |= O_TRUNC;
	}
	if (0 > (scm->fd = open(pathname, flags, 0644))) {
		TRACE("open()");
		scm_close(scm);
		return NULL;
	}
	if (fstat(scm->fd, &st)) {
		TRACE("fstat()");
		scm_close(scm);
		return NULL;
	}
	if (!S_ISREG(st.st_mode)) {
		TRACE("S_ISREG()");
		scm_close(scm);
		return NULL;
	}

	if (!st.st_size) {
		cap = page_align(SCM_CAP);
		if (!(addr = choose_addr(cap))) {
			scm_close(scm);
			return NULL;
		}
		if (ftruncate(scm->fd, (off_t)cap)) {
			TRACE("ftruncate()");
			scm_close(scm);
			return NULL;
		}
		if (!(map = map_at(scm->fd, addr, cap))) {
			scm_close(scm);
			return NULL;
		}
		scm->map = map;
		scm->cap = cap;
		scm->meta = map;
		memset(scm->meta, 0, sizeof (struct meta));
		scm->meta->magic = SCM_MAGIC;
		scm->meta->capacity = cap;
		scm->meta->cursor = heap_off();
		scm->meta->live = 0;
		scm->meta->base = map;
		scm->meta->free_head = NULL;
		return scm;
	}

	memset(&hint, 0, sizeof (hint));
	got = read(scm->fd, &hint, sizeof (hint));
	if ((ssize_t)sizeof (hint) != got) {
		TRACE("read()");
		scm_close(scm);
		return NULL;
	}
	if (!meta_ok(&hint) || ((uint64_t)st.st_size < hint.capacity)) {
		TRACE("corrupt");
		scm_close(scm);
		return NULL;
	}
	cap = (size_t)hint.capacity;
	if (!(map = map_at(scm->fd, hint.base, cap))) {
		scm_close(scm);
		return NULL;
	}
	scm->map = map;
	scm->cap = cap;
	scm->meta = map;
	if ((SCM_MAGIC != scm->meta->magic) ||
	    (scm->meta->base != map) ||
	    !free_head_ok(scm->meta)) {
		TRACE("corrupt");
		scm_close(scm);
		return NULL;
	}
	return scm;
}

void *
scm_malloc(struct scm *scm, size_t n)
{
	size_t need;
	void *p;

	assert( scm );

	need = block_size(n);
	if (!need) {
		return NULL;
	}
	if ((p = from_free(scm->meta, need))) {
		return p;
	}
	return from_bump(scm->meta, need);
}

char *
scm_strdup(struct scm *scm, const char *s)
{
	size_t n;
	char *d;

	assert( scm );
	assert( s );

	n = safe_strlen(s) + 1;
	if (!(d = scm_malloc(scm, n))) {
		TRACE(0);
		return NULL;
	}
	memcpy(d, s, n);
	return d;
}

void
scm_free(struct scm *scm, void *p)
{
	struct block *blk;

	assert( scm );

	if (!p) {
		return;
	}
	if ((uintptr_t)p % ALIGNMENT) {
		TRACE("scm_free()");
		return;
	}
	blk = block_of(p);
	if (!block_ok(scm->meta, blk)) {
		TRACE("scm_free()");
		return;
	}
	release(scm->meta, blk);
}

size_t
scm_utilized(const struct scm *scm)
{
	assert( scm );

	return (size_t)scm->meta->live;
}

size_t
scm_capacity(const struct scm *scm)
{
	assert( scm );

	return (size_t)(scm->meta->capacity - heap_off());
}

void *
scm_mbase(struct scm *scm)
{
	assert( scm );

	return (char *)scm->meta->base + heap_off() + sizeof (size_t);
}
