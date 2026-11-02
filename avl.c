/**
 * Tony Givargis
 * Copyright (C), 2023-2026
 * University of California, Irvine
 *
 * CS 238P - Operating Systems
 * avl.c
 */

#include "scm.h"
#include "avl.h"

struct avl {
	struct state {
		uint64_t items;
		uint64_t unique;
		struct node {
			int depth;
			uint64_t count;
			const char *item;
			struct node *left;
			struct node *right;
		} *root;
	} *state; /* SCM */
	struct scm *scm;
};

static int
delta(const struct node *node)
{
	return node ? node->depth : -1;
}

static int
balance(const struct node *node)
{
	return delta(node->left) - delta(node->right);
}

static int
depth(const struct node *a, const struct node *b)
{
	return (delta(a) > delta(b)) ? (delta(a) + 1) : (delta(b) + 1);
}

static struct node *
rotate_right(struct node *node)
{
	struct node *root;

	root = node->left;
	node->left = root->right;
	root->right = node;
	node->depth = depth(node->left, node->right);
	root->depth = depth(root->left, node);
	return root;
}

static struct node *
rotate_left(struct node *node)
{
	struct node *root;

	root = node->right;
	node->right = root->left;
	root->left = node;
	node->depth = depth(node->left, node->right);
	root->depth = depth(root->right, node);
	return root;
}

static struct node *
rotate_left_right(struct node *node)
{
	node->left = rotate_left(node->left);
	return rotate_right(node);
}

static struct node *
rotate_right_left(struct node *node)
{
	node->right = rotate_right(node->right);
	return rotate_left(node);
}

static struct node *
update(struct avl *avl, struct node *root, const char *item)
{
	int d;

	if (!root) {
		if (!(root = scm_malloc(avl->scm, sizeof (struct node)))) {
			TRACE(0);
			return NULL;
		}
		memset(root, 0, sizeof (struct node));
		if (!(root->item = scm_strdup(avl->scm, item))) {
			scm_free(avl->scm, root);
			TRACE(0);
			return NULL;
		}
		++root->count;
		++avl->state->items;
		++avl->state->unique;
		return root;
	}
	if (!(d = strcmp(item, root->item))) {
		++root->count;
		++avl->state->items;
	}
	else if (0 > d) {
		root->left = update(avl, root->left, item);
		if (1 < abs(balance(root))) {
			if (0 > strcmp(item, root->left->item)) {
				root = rotate_right(root);
			}
			else {
				root = rotate_left_right(root);
			}
		}
	}
	else if (0 < d) {
		root->right = update(avl, root->right, item);
		if (1 < abs(balance(root))) {
			if (0 < strcmp(item, root->right->item)) {
				root = rotate_left(root);
			}
			else {
				root = rotate_right_left(root);
			}
		}
	}
	root->depth = depth(root->left, root->right);
	return root;
}

static void
traverse(struct node *node, avl_fnc_t fnc, void *arg)
{
	if (node) {
		traverse(node->left, fnc, arg);
		fnc(arg, node->item, node->count);
		traverse(node->right, fnc, arg);
	}
}

static struct node *
rebalance(struct node *root)
{
	int b;

	if (!root) {
		return NULL;
	}
	root->depth = depth(root->left, root->right);
	b = balance(root);
	if (1 < b) {
		if (0 > balance(root->left)) {
			root = rotate_left_right(root);
		}
		else {
			root = rotate_right(root);
		}
	}
	else if (-1 > b) {
		if (0 < balance(root->right)) {
			root = rotate_right_left(root);
		}
		else {
			root = rotate_left(root);
		}
	}
	return root;
}

/* Unlink the minimum node and return the rebalanced remainder. */
static struct node *
detach_min(struct node *root, struct node **min)
{
	assert( root );

	if (!root->left) {
		*min = root;
		return root->right;
	}
	root->left = detach_min(root->left, min);
	return rebalance(root);
}

static struct node *
remove_rec(struct avl *avl, struct node *root, const char *item, int *found)
{
	int d;
	struct node *child;
	struct node *succ;
	const char *old;

	if (!root) {
		return NULL;
	}
	d = strcmp(item, root->item);
	if (0 > d) {
		root->left = remove_rec(avl, root->left, item, found);
	}
	else if (0 < d) {
		root->right = remove_rec(avl, root->right, item, found);
	}
	else {
		*found = 1;
		if (1 < root->count) {
			--root->count;
			--avl->state->items;
			return root;
		}
		--avl->state->items;
		--avl->state->unique;
		if (!root->left || !root->right) {
			child = root->left ? root->left : root->right;
			scm_free(avl->scm, (void *)root->item);
			scm_free(avl->scm, root);
			return child;
		}
		root->right = detach_min(root->right, &succ);
		old = root->item;
		root->item = succ->item;
		root->count = succ->count;
		scm_free(avl->scm, (void *)old);
		scm_free(avl->scm, succ);
	}
	return rebalance(root);
}

struct avl *
avl_open(const char *pathname, int truncate)
{
	struct avl *avl;

	assert( pathname );

	if (!(avl = malloc(sizeof (struct avl)))) {
		TRACE("out of memory");
		return NULL;
	}
	memset(avl, 0, sizeof (struct avl));
	if (!(avl->scm = scm_open(pathname, truncate))) {
		avl_close(avl);
		TRACE(0);
		return NULL;
	}
	if (scm_utilized(avl->scm)) {
		avl->state = scm_mbase(avl->scm);
	}
	else {
		if (!(avl->state = scm_malloc(avl->scm,
					      sizeof (struct state)))) {
			avl_close(avl);
			TRACE(0);
			return NULL;
		}
		memset(avl->state, 0, sizeof (struct state));
		assert( avl->state == scm_mbase(avl->scm) );
	}
	return avl;
}

void
avl_close(struct avl *avl)
{
	if (avl) {
		scm_close(avl->scm);
		memset(avl, 0, sizeof (struct avl));
	}
	FREE(avl);
}

int
avl_insert(struct avl *avl, const char *item)
{
	struct node *root;

	assert( avl );
	assert( safe_strlen(item) );

	if (!(root = update(avl, avl->state->root, item))) {
		TRACE(0);
		return -1;
	}
	avl->state->root = root;
	return 0;
}

int
avl_remove(struct avl *avl, const char *item)
{
	int found;

	assert( avl );
	assert( safe_strlen(item) );

	found = 0;
	avl->state->root = remove_rec(avl, avl->state->root, item, &found);
	return found ? 0 : -1;
}

uint64_t
avl_exists(const struct avl *avl, const char *item)
{
	const struct node *node;
	int d;

	assert( avl );
	assert( safe_strlen(item) );

	node = avl->state->root;
	while (node) {
		if (!(d = strcmp(item, node->item))) {
			return node->count;
		}
		node = (0 > d) ? node->left : node->right;
	}
	return 0;
}

void
avl_traverse(const struct avl *avl, avl_fnc_t fnc, void *arg)
{
	assert( avl );
	assert( fnc );

	traverse(avl->state->root, fnc, arg);
}

uint64_t
avl_items(const struct avl *avl)
{
	assert( avl );

	return avl->state->items;
}

uint64_t
avl_unique(const struct avl *avl)
{
	assert( avl );

	return avl->state->unique;
}

size_t
avl_scm_utilized(const struct avl *avl)
{
	assert( avl );

	return scm_utilized(avl->scm);
}

size_t
avl_scm_capacity(const struct avl *avl)
{
	assert( avl );

	return scm_capacity(avl->scm);
}
