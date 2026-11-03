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

/*
 * File overview
 *   1. Constants and data structures
 *   2. Size/alignment helpers
 *   3. Mapping helpers (fixed-address mmap)
 *   4. Allocator internals (free list, bump, release/coalesce)
 *   5. Public API: open/close, malloc/free/strdup, stats
 */

/* ===== 1. Constants and data structures ===== */

#define SCM_MAGIC ((uint64_t)0x53434d31u) /* "SCM1" */
#define SCM_CAP   ((size_t)64 << 20)      /* size of the region, 64 MB */
#define SCM_GAP   ((size_t)256 << 20)     /* distance between the normal heap and where the file gets mapped, 256 MB */
#define ALIGNMENT ((size_t)sizeof (void *))


struct block {
	size_t size;          /* whole block, including this header */
	struct block *next;   /* free list link; overlaps user bytes */
};

/* Persistent metadata stored at the start of the file (survives restarts). */
struct meta {
	uint64_t magic;
	uint64_t capacity;    /* mapped bytes */
	uint64_t cursor;      /* bump offset from base */
	uint64_t live;        /* bytes currently checked out */
	void *base;           /* address the file is mapped at */
	struct block *free_head;
};

/* In-memory handle for open file. */
struct scm {
	int fd;				/* open file descriptor */
	void *map;			/* where file is mapped in memory*/
	size_t cap;			/* how many bytes are mapped*/
	struct meta *meta;	/* pointer to the header inside mapping */
};

/* ===== 2. Size/alignment helpers ===== */

/* Round block up to a multiple of pointer size; returns 0 on overflow. */
static size_t
align_up(size_t n)
{
	size_t r;

	r = n % ALIGNMENT;
	if (!r) {
		return n;
	}
	if (n > ((size_t)-1) - (ALIGNMENT - r)) {
		return 0; /* overflow */
	}
	return n + (ALIGNMENT - r);
}

/* Returns where the heap starts in the file (after meta) */
static size_t
heap_off(void)
{
	return align_up(sizeof (struct meta));
}

/* Round mapped file up to a whole number of pages (mmap needs page-sized lengths). */
static size_t
page_align(size_t n)
{
	size_t p;
	size_t r;

	p = page_size();
	r = n % p;
	return r ? n + (p - r) : n;
}

/* Total block size for n user bytes: size header + payload, aligned, min block. */
static size_t
block_size(size_t n)
{
	size_t bytes;

	if (!n || (n > ((size_t)-1) - sizeof (size_t))) {
		return 0; /* zero or overflowing request */
	}
	bytes = sizeof (size_t) + n;
	if (bytes < sizeof (struct block)) {
		bytes = sizeof (struct block);
	}
	return align_up(bytes);
}

/* Block -> payload pointer: skip the size header. */
static void *
payload_of(struct block *blk)
{
	return (char *)blk + sizeof (size_t);
}

/* User pointer -> block: step back to the size header. */
static struct block *
block_of(void *p)
{
	return (struct block *)((char *)p - sizeof (size_t));
}

/* ===== 3. Mapping helpers (persistence relies on a fixed address) ===== */

/* Pick a fixed virtual address well above the heap for the mapping (for new file). */
static void *
choose_addr(void)
{
	void *brk;

	if ((void *)-1 == (brk = sbrk(0))) {	/* Get where normal heap currently ends */
		TRACE("sbrk()");
		return NULL;
	}
	return memory_align((char *)brk + SCM_GAP, page_size());	/* Add gap so lands clear of it*/
}

/*
 * Map the file at exactly `addr` (MAP_SHARED so writes reach the file).
 * Same address every run keeps stored pointers valid.
 */
static void *
map_at(int fd, void *addr, size_t cap)
{
	void *p;

	p = mmap(addr, cap, PROT_READ | PROT_WRITE,		/* Make this file appear as memory, starting at this address. */
		 MAP_SHARED | MAP_FIXED, fd, 0);
	if (MAP_FAILED == p) {
		TRACE("mmap()");
		return NULL;
	}
	return p;
}

/* ===== 4. Allocator internals ===== */

/* First-fit search of the free list; splits the block if the remainder is usable. */
static void *
from_free(struct meta *meta, size_t need)
{
	struct block *prev;
	struct block *cur;
	struct block *rest;

	/* Walk the free list; take the first block that is big enough. */
	prev = NULL;
	cur = meta->free_head;
	while (cur) {
		if (cur->size >= need) {
			if ((cur->size - need) >= sizeof (struct block)) {
				/* split: remainder stays on the free list */
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
				/* leftover too small to be a block: unlink and use it all */
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

/* Carve a fresh block from the untouched end of the heap (advance cursor). */
static void *
from_bump(struct meta *meta, size_t need)
{
	struct block *blk;

	if ((uint64_t)need > (meta->capacity - meta->cursor)) {
		return NULL; /* out of space */
	}
	/* New block goes at the cursor; then move the cursor past it. */
	blk = (struct block *)((char *)meta->base + (size_t)meta->cursor);
	blk->size = need;
	meta->cursor += need;
	meta->live += need;
	return payload_of(blk);
}

/*
 * Insert blk into the address-ordered free list, then coalesce with the next and previous neighbors if adjacent.
 */
static void
release(struct meta *meta, struct block *blk)
{
	struct block *prev;
	struct block *cur;

	/* Find insertion point: free list is sorted by address. */
	prev = NULL;
	cur = meta->free_head;
	while (cur && ((uintptr_t)cur < (uintptr_t)blk)) {
		prev = cur;
		cur = cur->next;
	}
	meta->live -= blk->size;
	/* Link blk between prev and cur. */
	blk->next = cur;
	if (prev) {
		prev->next = blk;
	}
	else {
		meta->free_head = blk;
	}
	/* Merge with the next block if it starts where blk ends. */
	if (cur && ((char *)blk + blk->size == (char *)cur)) {
		blk->size += cur->size;
		blk->next = cur->next;
	}
	/* Merge with the previous block if it ends where blk starts. */
	if (prev && ((char *)prev + prev->size == (char *)blk)) {
		prev->size += blk->size;
		prev->next = blk->next;
	}
}

/* ===== 5a. Public API: open / close (persistence) ===== */

/* Flush the mapping to disk (msync), unmap, close file, free handle. */
void
scm_close(struct scm *scm)
{
	if (!scm) {
		return;
	}
	if (scm->map && (MAP_FAILED != scm->map) && scm->cap) {
		/* msync forces the data to disk; this is what persists it */
		if (msync(scm->map, scm->cap, MS_SYNC)) {
			TRACE("msync()");
		}
		/* munmap: remove the file from memory */
		if (munmap(scm->map, scm->cap)) {
			TRACE("munmap()");
		}
	}
	if (0 <= scm->fd) {
		if (close(scm->fd)) {	/* close the file */
			TRACE("close()");
		}
	}
	memset(scm, 0, sizeof (struct scm));
	FREE(scm);		/* free the handle's RAM */
}

/*
 * Open (or create) the backing file and map it.
 * New/empty file: size it, map at a fresh address, init metadata.
 * Existing file: read the saved base address, validate, and remap at
 * that same address so previously stored pointers remain valid.
 */
struct scm *
scm_open(const char *pathname, int truncate)
{
	struct scm *scm;
	struct stat st;
	struct meta hint;
	size_t cap;
	void *addr;
	void *map;
	int flags;

	assert( safe_strlen(pathname) );
	/* allocate the handle in normal RAM */
	if (!(scm = malloc(sizeof (struct scm)))) {
		TRACE("out of memory");
		return NULL;
	}
	memset(scm, 0, sizeof (struct scm));
	scm->fd = -1;

	/* Open the backing file; truncate wipes old contents (fresh start). */
	flags = O_RDWR | O_CREAT;
	if (truncate) {
		flags |= O_TRUNC;
	}
	if (0 > (scm->fd = open(pathname, flags, 0644))) {	/* open the file */
		TRACE("open()");
		scm_close(scm);
		return NULL;
	}
	if (fstat(scm->fd, &st)) {		/* get file info, we need its size */
		TRACE("fstat()");
		scm_close(scm);
		return NULL;
	}

	/* Fresh file: create and initialize the heap. */
	if (!st.st_size) {
		cap = page_align(SCM_CAP);	/* region size: 64 MB, whole pages */
		if (!(addr = choose_addr()) ||	/* pick where to map it */
		    ftruncate(scm->fd, (off_t)cap) ||	/* make the file 64 MB long */
		    !(map = map_at(scm->fd, addr, cap))) {	/* map file at that address */
			scm_close(scm);	/* any failure: clean up */
			return NULL;
		}
		scm->map = map;	/* remember where it's mapped */
		scm->cap = cap;	/* remember how big */
		scm->meta = map;	/* header sits at the start of the file */
		memset(scm->meta, 0, sizeof (struct meta));	/* clear the header */
		scm->meta->magic = SCM_MAGIC;	/* mark as an SCM file */
		scm->meta->capacity = cap;	/* save total size */
		scm->meta->cursor = heap_off();	/* first block goes right after header */
		scm->meta->base = map;	/* save address so reopen maps same spot */
		return scm;
	}

	/* Existing file: read metadata to learn the original base address. */
	memset(&hint, 0, sizeof (hint));
	if ((ssize_t)sizeof (hint) != read(scm->fd, &hint, sizeof (hint)) ||	/* read header */
	    (SCM_MAGIC != hint.magic)) {
		TRACE("corrupt");
		scm_close(scm);
		return NULL;
	}
	/* Remap at the saved address; fail if the OS gave us a different one. */
	cap = (size_t)hint.capacity;
	if (!(map = map_at(scm->fd, hint.base, cap)) || (map != hint.base)) {
		scm_close(scm);
		return NULL;
	}
	scm->map = map;	/* remember where it's mapped */
	scm->cap = cap;	/* remember how big */
	scm->meta = map;	/* header is at the start of the mapping */
	return scm;
}

/* ===== 5b. Public API: malloc / strdup / free ===== */

/* Allocate: reuse a free block if possible, else bump-allocate. */
void *
scm_malloc(struct scm *scm, size_t n)
{
	size_t need;
	void *p;

	assert( scm );

	/* Convert request to full block size (header + alignment). */
	if (!(need = block_size(n))) {
		return NULL;
	}
	/* Prefer recycling a freed block before growing the heap. */
	if ((p = from_free(scm->meta, need))) {
		return p;
	}
	return from_bump(scm->meta, need);
}

/* Duplicate a string into SCM memory. */
char *
scm_strdup(struct scm *scm, const char *s)
{
	size_t n;
	char *d;

	assert( scm );
	assert( s );

	n = safe_strlen(s) + 1; /* +1 for the terminating '\0' */
	if (!(d = scm_malloc(scm, n))) {	/* get space in the file */
		TRACE(0);
		return NULL;
	}
	memcpy(d, s, n);
	return d;
}

/* Free: return the block to the free list. */
void
scm_free(struct scm *scm, void *p)
{
	assert( scm );

	if (p) {
		release(scm->meta, block_of(p));
	}
}

/* ===== 5c. Public API: stats and base address ===== */

/* Bytes currently allocated (including block headers). */
size_t
scm_utilized(const struct scm *scm)
{
	assert( scm );

	return (size_t)scm->meta->live;
}

/* Total usable heap bytes (excludes metadata). */
size_t
scm_capacity(const struct scm *scm)
{
	assert( scm );

	return (size_t)(scm->meta->capacity - heap_off());
}

/* Address of the first allocation's payload; stable across runs (lets callers find their root object). */
void *
scm_mbase(struct scm *scm)
{
	assert( scm );

	return (char *)scm->meta->base + heap_off() + sizeof (size_t);
}
