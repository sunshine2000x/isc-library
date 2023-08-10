/* See LICENSE for license details */
#ifndef _ISC_H_
#define _ISC_H_

#ifdef __cplusplus
extern "C" {
#endif

struct isc_handle;

struct isc_listener_ops {
	void (*bound)(void *arg);
	void (*unbind)(void *arg);
	int32_t (*got)(void *msg, uint32_t len, void *arg);
};

struct isc_handle {
	void (*close)(struct isc_handle *isc);

	int (*send)(struct isc_handle *isc, void *msg, uint32_t len,
		    int32_t *result);

	int (*add_listener)(struct isc_handle *isc,
			     const struct isc_listener_ops *ops,
			     void *arg);

	int (*rm_listener)(struct isc_handle *isc,
			    const struct isc_listener_ops *ops,
			    void *arg);
};

struct isc_attr {
	uint16_t msz; /* size of user message in bytes */
	uint16_t num; /* depth of user message queue, number of messages */
};

int open_isc(uint32_t uid,
	     struct isc_attr *s, /* send direction */
	     struct isc_attr *r, /* recv direction */
	     struct isc_handle **isc);

#ifdef __cplusplus
}
#endif

#endif /* _ISC_H_ */
