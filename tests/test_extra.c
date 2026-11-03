/*
 * Checks for the storage-class memory manager, the word tree, and the
 * cs238 shell. From the project directory:
 *
 *   make test
 */

#include "scm.h"
#include "avl.h"
#include "shell.h"
#include "term.h"
#include "drive_shell.h"

#define PATH "test_extra.scm"
#define NWORDS 32

static int passed;
static int failed;

static void
expect(int cond, const char *name)
{
	if (cond) {
		printf("  ok   %s\n", name);
		++passed;
		return;
	}
	fprintf(stderr, "  FAIL %s\n", name);
	++failed;
}

static void
expect_u64(uint64_t got, uint64_t want, const char *name)
{
	if (got == want) {
		printf("  ok   %s\n", name);
		++passed;
		return;
	}
	fprintf(stderr, "  FAIL %s (got %lu, want %lu)\n",
		name, (unsigned long)got, (unsigned long)want);
	++failed;
}

static void
expect_sz(size_t got, size_t want, const char *name)
{
	if (got == want) {
		printf("  ok   %s\n", name);
		++passed;
		return;
	}
	fprintf(stderr, "  FAIL %s (got %lu, want %lu)\n",
		name, (unsigned long)got, (unsigned long)want);
	++failed;
}

static void
section(const char *name)
{
	printf("\n%s\n", name);
}

static struct scm *
open_scm(int truncate)
{
	struct scm *scm;

	if (truncate) {
		file_delete(PATH);
	}
	scm = scm_open(PATH, truncate);
	if (!scm) {
		fprintf(stderr, "scm_open(%s) failed\n", PATH);
		exit(1);
	}
	return scm;
}

static struct avl *
open_avl(int truncate)
{
	struct avl *avl;

	if (truncate) {
		file_delete(PATH);
	}
	avl = avl_open(PATH, truncate);
	if (!avl) {
		fprintf(stderr, "avl_open(%s) failed\n", PATH);
		exit(1);
	}
	return avl;
}

static void
test_reuse_and_null(void)
{
	struct scm *scm;
	void *p;
	void *q;
	size_t live;

	section("scm_free: reuse");
	scm = open_scm(1);
	expect_sz(scm_utilized(scm), 0, "fresh region is empty");
	scm_free(scm, NULL);
	expect_sz(scm_utilized(scm), 0, "free(NULL) is a no-op");

	p = scm_malloc(scm, 32);
	expect(p != NULL, "malloc(32) succeeds");
	live = scm_utilized(scm);
	expect(live > 0, "malloc increases utilized");
	scm_free(scm, p);
	expect_sz(scm_utilized(scm), 0, "free drops utilized to zero");
	q = scm_malloc(scm, 32);
	expect(q == p, "next malloc reuses the freed block");
	expect_sz(scm_utilized(scm), live, "reuse does not grow utilized");
	scm_close(scm);
}

static void
test_many_blocks(void)
{
	struct scm *scm;
	void *ptrs[100];
	void *q;
	int i;
	int ok;

	section("scm_free: many blocks");
	scm = open_scm(1);
	ok = 1;
	for (i = 0; i < 100; ++i) {
		ptrs[i] = scm_malloc(scm, (size_t)(16 + (i % 7)));
		if (!ptrs[i]) {
			ok = 0;
		}
	}
	expect(ok, "allocate 100 blocks");
	for (i = 99; 0 <= i; --i) {
		scm_free(scm, ptrs[i]);
	}
	expect_sz(scm_utilized(scm), 0, "freeing every block clears utilized");
	q = scm_malloc(scm, 16);
	expect(ok && (q == ptrs[0]), "coalesced space is reused from the front");
	scm_close(scm);
}

static void
test_split(void)
{
	struct scm *scm;
	void *big1;
	void *big2;
	void *a;
	void *b;
	char *block;
	char *end;
	size_t big_gap;
	size_t small_gap;
	size_t live;

	section("scm_free: split");
	scm = open_scm(1);
	big1 = scm_malloc(scm, 200);
	big2 = scm_malloc(scm, 200);
	expect((big1 != NULL) && (big2 != NULL), "two large blocks allocate");
	if (!big1 || !big2) {
		scm_close(scm);
		return;
	}
	big_gap = (size_t)((char *)big2 - (char *)big1);
	scm_free(scm, big1);
	live = scm_utilized(scm);
	a = scm_malloc(scm, 24);
	b = scm_malloc(scm, 24);
	expect(a == big1, "small malloc takes the front of a freed block");
	expect((a != NULL) && (b != NULL) && ((char *)b > (char *)a),
	       "second small malloc succeeds");
	if (!a || !b) {
		scm_close(scm);
		return;
	}
	small_gap = (size_t)((char *)b - (char *)a);
	block = (char *)big1 - sizeof (size_t);
	end = block + big_gap;
	expect(small_gap < big_gap, "freed block was split");
	expect(((char *)b - sizeof (size_t)) + small_gap <= end,
	       "both pieces stay inside the original block");
	expect_sz(scm_utilized(scm), live + (2 * small_gap),
		  "only the split pieces are charged");
	scm_close(scm);
}

static void
test_coalesce(void)
{
	struct scm *scm;
	void *a;
	void *b;
	void *c;
	void *d;
	size_t gap;
	size_t need;
	size_t live_one;

	section("scm_free: coalesce");
	scm = open_scm(1);
	a = scm_malloc(scm, 24);
	b = scm_malloc(scm, 24);
	c = scm_malloc(scm, 24);
	expect((a != NULL) && (b != NULL) && (c != NULL) &&
	       ((char *)b > (char *)a) && ((char *)c > (char *)b),
	       "three blocks are ordered");
	if (!a || !b || !c) {
		scm_close(scm);
		return;
	}
	gap = (size_t)((char *)b - (char *)a);
	expect((char *)c == (char *)b + gap, "bump blocks are adjacent");
	scm_free(scm, a);
	scm_free(scm, b);
	live_one = scm_utilized(scm);
	need = (2 * gap) - sizeof (size_t);
	d = scm_malloc(scm, need);
	expect(d == a, "adjacent frees satisfy a two-block request");
	expect_sz(scm_utilized(scm), live_one + (2 * gap),
		  "coalesced allocation is charged once");
	scm_close(scm);
}

static void
test_no_coalesce_across_live(void)
{
	struct scm *scm;
	void *a;
	void *b;
	void *c;
	void *big;
	void *small;
	size_t gap;

	section("scm_free: no coalesce across a live block");
	scm = open_scm(1);
	a = scm_malloc(scm, 24);
	b = scm_malloc(scm, 24);
	c = scm_malloc(scm, 24);
	expect((a != NULL) && (b != NULL) && (c != NULL),
	       "three blocks allocate");
	if (!a || !b || !c) {
		scm_close(scm);
		return;
	}
	gap = (size_t)((char *)b - (char *)a);
	scm_free(scm, a);
	scm_free(scm, c);
	big = scm_malloc(scm, (2 * gap) - sizeof (size_t));
	expect(big && (big != a) && (big != c),
	       "a live neighbor blocks a two-block reuse");
	small = scm_malloc(scm, 24);
	expect(small == a, "first-fit still reuses the earlier hole");
	scm_close(scm);
}

static void
test_free_persists(void)
{
	struct scm *scm;
	void *p;
	void *q;
	size_t live;

	section("scm_free: persistence");
	scm = open_scm(1);
	p = scm_malloc(scm, 48);
	expect(p != NULL, "allocate before close");
	scm_free(scm, p);
	live = scm_utilized(scm);
	scm_close(scm);

	scm = scm_open(PATH, 0);
	expect(scm != NULL, "reopen the region");
	if (!scm) {
		return;
	}
	expect_sz(scm_utilized(scm), live, "utilized survives close");
	q = scm_malloc(scm, 48);
	expect(q == p, "freed block is reused after reopen");
	scm_close(scm);
}

static void
test_remove_counts(void)
{
	struct avl *avl;
	size_t live;

	section("remove: counts");
	avl = open_avl(1);
	expect(avl_remove(avl, "cat") != 0, "missing word fails");
	expect_u64(avl_items(avl), 0, "failed remove leaves the item count");
	expect_u64(avl_unique(avl), 0, "failed remove leaves the unique count");

	expect(avl_insert(avl, "cat") == 0, "insert cat");
	expect(avl_insert(avl, "cat") == 0, "insert cat again");
	expect(avl_insert(avl, "cat") == 0, "insert cat a third time");
	expect(avl_insert(avl, "dog") == 0, "insert dog");
	live = avl_scm_utilized(avl);

	expect(avl_remove(avl, "cat") == 0, "remove one cat");
	expect_u64(avl_exists(avl, "cat"), 2, "two cats remain");
	expect_u64(avl_items(avl), 3, "item count drops by one");
	expect_u64(avl_unique(avl), 2, "unique count stays while copies remain");
	expect_sz(avl_scm_utilized(avl), live,
		  "dropping a copy does not free the word");

	expect(avl_remove(avl, "missing") != 0, "missing word still fails");
	expect_u64(avl_items(avl), 3, "failed remove keeps the item count");
	expect_sz(avl_scm_utilized(avl), live, "failed remove keeps utilized");

	expect(avl_remove(avl, "cat") == 0, "remove another cat");
	expect_u64(avl_exists(avl, "cat"), 1, "one cat remains");
	expect_sz(avl_scm_utilized(avl), live, "the last copy is still stored");

	expect(avl_remove(avl, "cat") == 0, "remove the last cat");
	expect_u64(avl_exists(avl, "cat"), 0, "cat is gone");
	expect_u64(avl_items(avl), 1, "only dog remains");
	expect_u64(avl_unique(avl), 1, "one unique word remains");
	expect(avl_scm_utilized(avl) < live, "last copy frees its storage");

	expect(avl_remove(avl, "dog") == 0, "remove dog");
	expect_u64(avl_items(avl), 0, "tree is empty");
	expect_u64(avl_unique(avl), 0, "no unique words remain");
	avl_close(avl);
}

struct walk {
	const char **words;
	uint64_t *counts;
	int n;
	int i;
	int bad;
};

static void
on_word(void *arg, const char *word, uint64_t count)
{
	struct walk *walk;

	walk = arg;
	if ((walk->i >= walk->n) ||
	    strcmp(word, walk->words[walk->i]) ||
	    (count != walk->counts[walk->i])) {
		walk->bad = 1;
	}
	++walk->i;
}

static void
expect_words(struct avl *avl, const char **words, uint64_t *counts, int n,
	     const char *name)
{
	struct walk walk;

	memset(&walk, 0, sizeof (walk));
	walk.words = words;
	walk.counts = counts;
	walk.n = n;
	avl_traverse(avl, on_word, &walk);
	expect(!walk.bad && (walk.i == n), name);
}

static void
test_remove_tree(void)
{
	struct avl *avl;
	const char *all[] = {
		"apple", "banana", "date", "fig", "guava",
		"kiwi", "lime", "mango", "pear", "plum"
	};
	const char *left[] = {
		"apple", "banana", "guava", "kiwi", "lime", "plum"
	};
	uint64_t all_counts[10];
	uint64_t left_counts[6];
	char word[8];
	char keep[NWORDS][8];
	const char *kept[NWORDS];
	uint64_t keep_counts[NWORDS];
	int i;
	int nkeep;

	section("remove: tree order");
	avl = open_avl(1);
	for (i = 0; i < 10; ++i) {
		all_counts[i] = 1;
		expect(avl_insert(avl, all[i]) == 0, all[i]);
	}
	expect(avl_insert(avl, "kiwi") == 0, "second kiwi");
	all_counts[5] = 2;
	expect_words(avl, all, all_counts, 10, "in-order traversal after insert");

	expect(avl_remove(avl, "pear") == 0, "remove pear");
	expect(avl_remove(avl, "date") == 0, "remove date");
	expect(avl_remove(avl, "mango") == 0, "remove mango");
	expect(avl_remove(avl, "fig") == 0, "remove fig");
	expect(avl_remove(avl, "kiwi") == 0, "remove one kiwi");
	for (i = 0; i < 6; ++i) {
		left_counts[i] = 1;
	}
	expect_words(avl, left, left_counts, 6, "remaining words stay sorted");
	expect_u64(avl_items(avl), 6, "six words remain");
	expect_u64(avl_unique(avl), 6, "six unique words remain");
	avl_close(avl);

	avl = open_avl(1);
	for (i = 0; i < NWORDS; ++i) {
		safe_sprintf(word, sizeof (word), "%02d", i);
		if (avl_insert(avl, word)) {
			fprintf(stderr, "  FAIL insert %s\n", word);
			++failed;
			avl_close(avl);
			return;
		}
	}
	nkeep = 0;
	for (i = 0; i < NWORDS; ++i) {
		safe_sprintf(word, sizeof (word), "%02d", i);
		if (i % 2) {
			if (avl_remove(avl, word)) {
				fprintf(stderr, "  FAIL remove %s\n", word);
				++failed;
				avl_close(avl);
				return;
			}
		}
		else {
			safe_sprintf(keep[nkeep], sizeof (keep[nkeep]), "%s", word);
			kept[nkeep] = keep[nkeep];
			keep_counts[nkeep] = 1;
			++nkeep;
		}
	}
	expect(1, "insert 00..31 and remove the odd keys");
	expect_words(avl, kept, keep_counts, nkeep,
		     "even keys remain sorted after deletions");
	expect_u64(avl_items(avl), (uint64_t)nkeep, "half of the keys remain");
	for (i = 0; i < nkeep; ++i) {
		if (avl_remove(avl, kept[i])) {
			fprintf(stderr, "  FAIL remove %s\n", kept[i]);
			++failed;
			avl_close(avl);
			return;
		}
	}
	expect_u64(avl_items(avl), 0, "sequential tree is empty");
	expect_u64(avl_unique(avl), 0, "sequential tree has no unique keys");
	avl_close(avl);
}

static void
fill_words(struct avl *avl, int n)
{
	char word[16];
	int i;

	for (i = 0; i < n; ++i) {
		safe_sprintf(word, sizeof (word), "w%02d", i);
		if (avl_insert(avl, word)) {
			fprintf(stderr, "insert %s failed\n", word);
			exit(1);
		}
	}
}

static void
drop_words(struct avl *avl, int n)
{
	char word[16];
	int i;

	for (i = 0; i < n; ++i) {
		safe_sprintf(word, sizeof (word), "w%02d", i);
		if (avl_remove(avl, word)) {
			fprintf(stderr, "remove %s failed\n", word);
			exit(1);
		}
	}
}

static void
test_remove_reuses_memory(void)
{
	struct avl *avl;
	size_t base;
	size_t peak;

	section("remove: storage is reused");
	avl = open_avl(1);
	base = avl_scm_utilized(avl);
	fill_words(avl, 40);
	peak = avl_scm_utilized(avl);
	expect(peak > base, "inserts consume storage");
	drop_words(avl, 40);
	expect_u64(avl_items(avl), 0, "all words removed");
	expect_sz(avl_scm_utilized(avl), base,
		  "removing every word releases their storage");
	fill_words(avl, 40);
	expect_sz(avl_scm_utilized(avl), peak,
		  "reinserting the same words reuses freed blocks");
	avl_close(avl);
}

static void
test_remove_persists(void)
{
	struct avl *avl;
	size_t live;

	section("remove: persistence");
	avl = open_avl(1);
	expect(avl_insert(avl, "cat") == 0, "insert cat");
	expect(avl_insert(avl, "dog") == 0, "insert dog");
	expect(avl_insert(avl, "dog") == 0, "insert dog again");
	expect(avl_remove(avl, "cat") == 0, "remove cat");
	live = avl_scm_utilized(avl);
	avl_close(avl);

	avl = avl_open(PATH, 0);
	expect(avl != NULL, "reopen the word tree");
	if (!avl) {
		return;
	}
	expect_u64(avl_exists(avl, "cat"), 0, "removed word stays gone");
	expect_u64(avl_exists(avl, "dog"), 2, "remaining copies stay");
	expect_u64(avl_items(avl), 2, "item count survives reopen");
	expect_u64(avl_unique(avl), 1, "unique count survives reopen");
	expect_sz(avl_scm_utilized(avl), live, "utilized survives reopen");
	expect(avl_remove(avl, "dog") == 0, "remove one dog after reopen");
	expect_u64(avl_exists(avl, "dog"), 1, "one dog remains after reopen");
	avl_close(avl);
}

static void
test_system(void)
{
	char buf[32];
	char raw[64];
	void *aligned;
	FILE *fp;
	size_t pages;

	section("system");
	expect_sz(safe_strlen(NULL), 0, "safe_strlen(NULL) is zero");
	expect_sz(safe_strlen("abcd"), 4, "safe_strlen counts characters");
	memset(buf, 0x5a, sizeof (buf));
	safe_sprintf(buf, sizeof (buf), "%s %d", "ok", 7);
	expect(!strcmp(buf, "ok 7"), "safe_sprintf formats into the buffer");
	pages = page_size();
	expect(pages && !(pages & (pages - 1)), "page_size is a power of two");
	aligned = memory_align(raw, 16);
	expect(!((uintptr_t)aligned % 16), "memory_align lands on the boundary");
	expect((char *)aligned >= raw && (char *)aligned < raw + 16,
	       "memory_align moves at most one step");
	fp = fopen("test_delete_me.txt", "w");
	expect(fp != NULL, "create a file to delete");
	if (fp) {
		fputs("gone\n", fp);
		fclose(fp);
	}
	file_delete("test_delete_me.txt");
	fp = fopen("test_delete_me.txt", "r");
	expect(!fp, "file_delete removes the file");
	if (fp) {
		fclose(fp);
	}
	file_delete(NULL);
	file_delete("");
	us_sleep(1000);
	expect(1, "us_sleep returns");
}

static void
paint_color(void)
{
	term_init(0);
	term_bold();
	term_color(TERM_COLOR_RED);
	printf("RED");
	term_reset();
}

static void
paint_plain(void)
{
	term_init(1);
	term_bold();
	term_color(TERM_COLOR_GREEN);
	printf("PLAIN");
	term_reset();
}

static void
test_term(void)
{
	char buf[128];

	section("term");
	capture_stdout(paint_color, buf, sizeof (buf));
	expect(strstr(buf, "\033[1m") != NULL, "bold emits an escape");
	expect(strstr(buf, "\033[31m") != NULL, "red emits an escape");
	expect(strstr(buf, "RED") != NULL, "colored text is printed");
	expect(strstr(buf, "\033[0m") != NULL, "reset emits an escape");
	capture_stdout(paint_plain, buf, sizeof (buf));
	expect(!strcmp(buf, "PLAIN"), "nocolor prints the text only");
}

static void
expect_trim(const char *in, const char *want, const char *name)
{
	char buf[64];

	safe_sprintf(buf, sizeof (buf), "%s", in);
	shell_strtrim(buf);
	expect(!strcmp(buf, want), name);
}

static void
test_strtrim(void)
{
	section("shell: trim");
	expect_trim("  hi  ", "hi", "trim drops both ends");
	expect_trim("\t\n hello \t", "hello", "trim drops tabs and newlines");
	expect_trim("a b", "a b", "trim keeps an interior space");
	expect_trim("   ", "", "trim of blank text is empty");
	expect_trim("", "", "trim of an empty string stays empty");
	expect_trim("x", "x", "trim leaves a bare word");
}

static void
test_scm_core(void)
{
	struct scm *scm;
	char *p;
	void *base;
	size_t cap;
	size_t live;
	size_t sizes[6];
	int i;
	int ok;

	section("scm: allocation");
	scm = open_scm(1);
	cap = scm_capacity(scm);
	expect(cap > ((size_t)32 << 20), "capacity covers the region");
	expect_sz(scm_utilized(scm), 0, "a new region is empty");
	base = scm_mbase(scm);
	p = scm_malloc(scm, 24);
	expect(p == base, "the first malloc returns scm_mbase");
	expect(p && !((uintptr_t)p % sizeof (void *)), "the payload is aligned");
	live = scm_utilized(scm);
	expect(scm_malloc(scm, 0) == NULL, "malloc(0) fails");
	expect(scm_malloc(scm, (size_t)-1) == NULL, "an overflowing size fails");
	expect(scm_malloc(scm, (size_t)80 << 20) == NULL,
	       "a request larger than the region fails");
	expect_sz(scm_utilized(scm), live, "a failed malloc allocates nothing");
	expect_sz(scm_capacity(scm), cap, "capacity does not change");
	ok = 1;
	sizes[0] = 1;
	sizes[1] = 2;
	sizes[2] = 3;
	sizes[3] = 8;
	sizes[4] = 15;
	sizes[5] = 64;
	for (i = 0; i < 6; ++i) {
		void *q;

		q = scm_malloc(scm, sizes[i]);
		if (!q || ((uintptr_t)q % sizeof (void *))) {
			ok = 0;
		}
	}
	expect(ok, "several sizes come back aligned");
	if (p) {
		for (i = 0; i < 24; ++i) {
			p[i] = (char)('A' + (i % 26));
		}
	}
	scm_close(scm);

	scm = scm_open(PATH, 0);
	expect(scm != NULL, "reopen keeps the region");
	if (!scm) {
		return;
	}
	expect(scm_mbase(scm) == base, "the base address is stable");
	expect_sz(scm_capacity(scm), cap, "capacity survives reopen");
	ok = 1;
	for (i = 0; i < 24; ++i) {
		if (((char *)base)[i] != (char)('A' + (i % 26))) {
			ok = 0;
		}
	}
	expect(ok, "payload bytes survive close and reopen");
	scm_close(scm);

	scm = open_scm(1);
	expect_sz(scm_utilized(scm), 0, "truncate clears utilized bytes");
	expect(scm_malloc(scm, 8) == scm_mbase(scm),
	       "truncate starts allocation over");
	scm_close(scm);
}

static void
test_strdup_and_isolation(void)
{
	struct scm *scm;
	char src[] = "persimmon";
	char *copy;
	char *empty;
	unsigned char *a;
	unsigned char *b;

	section("scm: strdup");
	scm = open_scm(1);
	copy = scm_strdup(scm, src);
	expect(copy && !strcmp(copy, "persimmon"), "strdup copies the characters");
	src[0] = 'X';
	expect(copy && ('p' == copy[0]), "the copy does not follow the source");
	empty = scm_strdup(scm, "");
	expect(empty && !empty[0], "strdup of an empty string succeeds");
	a = scm_malloc(scm, 32);
	b = scm_malloc(scm, 32);
	expect(a && b, "two neighboring buffers allocate");
	if (a && b) {
		memset(a, 0x11, 32);
		memset(b, 0x22, 32);
		expect((0x11 == a[31]) && (0x22 == b[0]),
		       "neighboring buffers do not overlap");
	}
	scm_free(scm, copy);
	scm_close(scm);
}

static void
test_scm_rejects(void)
{
	struct scm *scm;
	FILE *fp;

	section("scm: rejected files");
	printf("  (open/read/corrupt lines on stderr are expected)\n");
	fflush(stdout);
	file_delete(PATH);
	fp = fopen(PATH, "w");
	expect(fp != NULL, "create a short backing file");
	if (fp) {
		fputs("not-a-region", fp);
		fclose(fp);
	}
	scm = scm_open(PATH, 0);
	expect(!scm, "a short file is rejected");
	scm_close(scm);
	scm = scm_open(".", 0);
	expect(!scm, "a directory is rejected");
	scm_close(scm);
	scm = scm_open("/no/such/cs238/region", 0);
	expect(!scm, "a missing directory is rejected");
	scm_close(scm);
	expect(!avl_open(".", 0), "avl_open rejects a directory");
	scm_close(NULL);
	avl_close(NULL);
	expect(1, "close(NULL) is safe");
}

static void
count_visit(void *arg, const char *word, uint64_t count)
{
	int *n;

	UNUSED(word);
	UNUSED(count);
	n = arg;
	++*n;
}

#define BIG 128

struct bag {
	char item[BIG][8];
	int n;
	int bad;
};

static void
on_bag(void *arg, const char *word, uint64_t count)
{
	struct bag *bag;

	bag = arg;
	if ((BIG <= bag->n) || (1 != count)) {
		bag->bad = 1;
		return;
	}
	if (bag->n && (0 <= strcmp(bag->item[bag->n - 1], word))) {
		bag->bad = 1;
	}
	safe_sprintf(bag->item[bag->n], sizeof (bag->item[0]), "%s", word);
	++bag->n;
}

static int
fill_pattern(struct avl *avl, int pattern)
{
	char word[8];
	int i;
	int k;

	for (i = 0; i < BIG; ++i) {
		if (!pattern) {
			k = i;
		}
		else if (1 == pattern) {
			k = BIG - 1 - i;
		}
		else {
			k = (i * 37) % BIG;
		}
		safe_sprintf(word, sizeof (word), "k%03d", k);
		if (avl_insert(avl, word)) {
			return -1;
		}
	}
	return 0;
}

static void
test_avl_core(void)
{
	struct avl *avl;
	struct bag bag;
	char src[] = "mutable";
	const char *names[3];
	size_t cap;
	int visits;
	int pattern;

	section("avl: insert, lookup, traverse");
	avl = open_avl(1);
	visits = 0;
	avl_traverse(avl, count_visit, &visits);
	expect(!visits, "an empty tree visits no words");
	expect(!avl_insert(avl, src), "insert mutable");
	src[0] = 'M';
	expect_u64(avl_exists(avl, "mutable"), 1, "the stored word is a copy");
	expect_u64(avl_exists(avl, "Mutable"), 0, "editing the source does not rename it");
	expect(!avl_insert(avl, "mutable"), "insert another copy");
	expect_u64(avl_exists(avl, "mutable"), 2, "exists reports the copy count");
	expect_u64(avl_items(avl), 2, "items counts every copy");
	expect_u64(avl_unique(avl), 1, "unique counts the key once");
	expect_u64(avl_exists(avl, "nope"), 0, "a missing word has count zero");
	cap = avl_scm_capacity(avl);
	expect(cap > ((size_t)32 << 20), "the tree reports region capacity");
	expect(avl_scm_utilized(avl) < cap, "utilized storage fits in the region");
	avl_close(avl);

	avl = open_avl(1);
	expect_u64(avl_exists(avl, "mutable"), 0, "truncate drops the saved words");
	avl_close(avl);

	section("avl: ordering");
	names[0] = "sorted inserts";
	names[1] = "reverse inserts";
	names[2] = "scrambled inserts";
	for (pattern = 0; pattern < 3; ++pattern) {
		avl = open_avl(1);
		expect(!fill_pattern(avl, pattern), names[pattern]);
		memset(&bag, 0, sizeof (bag));
		avl_traverse(avl, on_bag, &bag);
		expect(!bag.bad && (BIG == bag.n) &&
		       !strcmp(bag.item[0], "k000") &&
		       !strcmp(bag.item[BIG - 1], "k127"),
		       "traversal is sorted and complete");
		expect_u64(avl_items(avl), BIG, "item count matches the inserts");
		expect_u64(avl_unique(avl), BIG, "every inserted key is unique");
		expect_sz(avl_scm_capacity(avl), cap, "capacity stays fixed");
		avl_close(avl);
	}
}

int
main(void)
{
	passed = 0;
	failed = 0;
	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	test_system();
	test_term();
	test_strtrim();
	test_scm_core();
	test_strdup_and_isolation();
	test_reuse_and_null();
	test_many_blocks();
	test_split();
	test_coalesce();
	test_no_coalesce_across_live();
	test_free_persists();
	test_scm_rejects();
	test_avl_core();
	test_remove_counts();
	test_remove_tree();
	test_remove_reuses_memory();
	test_remove_persists();
	drive_shell_tests(&passed, &failed);

	file_delete(PATH);
	file_delete("test_shell.scm");
	file_delete("test_flags.scm");
	file_delete("test_color.scm");
	file_delete("test_words.txt");
	file_delete("test_delete_me.txt");
	printf("\n%d passed, %d failed\n", passed, failed);
	return failed ? 1 : 0;
}
