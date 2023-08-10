/* See LICENSE for license details */
#ifndef _LIST_H_
#define _LIST_H_

struct list {
	struct list *next;
	unsigned int size;
	void *data;
};

static void list_init(struct list *h)
{
	h->next = h;
	h->data = NULL;
}

static void list_set(struct list *n, void *data, unsigned int sz)
{
	n->data = data;
	n->size = sz;
}

static void *list_get(struct list *n, unsigned int *sz)
{
	if (sz)
		*sz = n->size;
	return n->data;
}

static struct list *list_next(struct list *n)
{
	return n->next;
}

static void list_add(struct list *n, struct list *h)
{
	if (n == h)
		return;

	n->next = h;

	while (h->next != n->next)
		h = h->next;

	h->next = n;
}

static struct list *list_del(struct list *n)
{
	struct list *p = n->next;

	if (p == n) {
		n->next = NULL;
		return NULL;
	}

	while (p->next != n)
		p = p->next;

	p->next = n->next;
	n->next = NULL;
	return p->next;
}

#endif /* _LIST_H_ */
