/* See LICENSE for license details */
#ifndef _ISC_H_
#define _ISC_H_

typedef int32_t (*msg_handler)(void *msg, uint32_t len, void *arg);

struct isc_handle {
	void (*close)(struct isc_handle *isc);

	int (*send)(struct isc_handle *isc, void *msg, uint32_t len,
		    int32_t *result);

	int (*add_listener)(struct isc_handle *isc, msg_handler h, void *arg);

	int (*rm_listener)(struct isc_handle *isc, msg_handler h, void *arg);
};

struct isc_attr {
	uint16_t msz; /* size of user message in bytes */
	uint16_t num; /* depth of user message queue, number of messages */
};

int open_isc(uint32_t uid,
	     struct isc_attr *s, /* send direction */
	     struct isc_attr *r, /* recv direction */
	     struct isc_handle **isc);

#endif /* _ISC_H_ */
