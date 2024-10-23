// See LICENSE for license details.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "isc_uapi.h"
#include "list.h"

#include "isc.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(*(x)))
#endif

#define ISC_DEV_NAME "/dev/isc"
#define LOGE(...)    fprintf(stderr, __VA_ARGS__)

enum isc_direct {
    ISC_DIR_SEND = 1,
    ISC_DIR_RECV = 2,
    ISC_DIR_BIDIRECTION = 3,
};

struct isc_listener {
    const struct isc_listener_ops *ops;
    void *arg;
};

struct isc_queue {
    struct list *wp, *rp;
    void *mem;
    uint32_t size;
};

struct isc_device {
    struct isc_handle isc;
    uint32_t direct;
    uint32_t uid;
    int fd, efd;
    bool is_task_started;
    pthread_t task_handle;
    struct isc_queue sendq, recvq;
    uint32_t seq;
    pthread_mutex_t send_lock;
    bool send_ready, recv_ready;
    pthread_mutex_t listener_lock;
    struct list *listener_list;
};

static void isc_handle_user_msg(struct isc_device *idev, struct isc_msg *msg)
{
    struct isc_listener *li;
    struct list *pos;
    int32_t rc = 0;

    if (!msg)
        return;

    pthread_mutex_lock(&idev->listener_lock);
    pos = idev->listener_list;
    if (!pos) {
        pthread_mutex_unlock(&idev->listener_lock);
        msg->rc = -1;
        return;
    }

    do {
        li = (struct isc_listener *)list_get(pos, NULL);
        if (li->ops->got)
            rc |= li->ops->got(msg->d, msg->len, li->arg);
        pos = list_next(pos);
    } while (pos != idev->listener_list);

    pthread_mutex_unlock(&idev->listener_lock);
    msg->rc = rc;
}

static void isc_notify_listener(struct isc_device *idev, bool is_bound)
{
    struct isc_listener *li;
    struct list *pos;

    pthread_mutex_lock(&idev->listener_lock);
    pos = idev->listener_list;
    if (!pos) {
        pthread_mutex_unlock(&idev->listener_lock);
        return;
    }

    do {
        li = (struct isc_listener *)list_get(pos, NULL);
        if (is_bound) {
            if (li->ops->bound)
                li->ops->bound(li->arg);
        } else {
            if (li->ops->unbind)
                li->ops->unbind(li->arg);
        }
        pos = list_next(pos);
    } while (pos != idev->listener_list);

    pthread_mutex_unlock(&idev->listener_lock);
}

static void isc_handle_int_msg(struct isc_device *idev, struct isc_msg *msg)
{
    struct isc_int_msg *imsg;

    if (!msg)
        return;

    imsg = (struct isc_int_msg *)msg->d;

    switch (imsg->id) {
    case ISC_MSG_BOUND:
        if (idev->direct & ISC_DIR_RECV)
            idev->recv_ready = true;
        pthread_mutex_lock(&idev->send_lock);
        if (idev->direct & ISC_DIR_SEND)
            idev->send_ready = true;
        pthread_mutex_unlock(&idev->send_lock);
        isc_notify_listener(idev, true);
        break;
    case ISC_MSG_UNBIND:
        isc_notify_listener(idev, false);
        pthread_mutex_lock(&idev->send_lock);
        if (idev->direct & ISC_DIR_SEND)
            idev->send_ready = false;
        pthread_mutex_unlock(&idev->send_lock);
        if (idev->direct & ISC_DIR_RECV)
            idev->recv_ready = false;
        break;
    default:
        break;
    }
    msg->rc = 0;
}

static inline void isc_handle_msg(struct isc_device *idev, struct isc_msg *msg)
{
    if (msg->flags & ISC_MSG_FLAG_USER)
        isc_handle_user_msg(idev, msg);
    else
        isc_handle_int_msg(idev, msg);
}

static int isc_send_ack(int fd, uint32_t seq)
{
    struct isc_recv recv;
    int rc;

    memset(&recv, 0, sizeof(recv));
    recv.num = 1;
    recv.seq = seq;
    rc = ioctl(fd, ISC_IOCTL_RECV, &recv);
    if (rc < 0)
        LOGE("failed to ioctl ISC_IOCTL_RECV (rc=%s)\n", strerror(errno));
    return rc;
}

static void *isc_task_handler(void *arg)
{
    struct isc_device *idev = (struct isc_device *)arg;
    struct pollfd fds[2];
    struct isc_msg *m;
    int rc;

    if (!idev)
        return NULL;

    memset(fds, 0, sizeof(fds));
    fds[0].fd = idev->fd;
    fds[1].fd = idev->efd;
    fds[0].events = POLLIN;
    fds[1].events = POLLIN;

    while (idev->is_task_started) {
        fds[0].revents = 0;
        rc = poll(fds, ARRAY_SIZE(fds), -1);
        if (rc <= 0)
            continue;
        if (!(fds[0].revents & POLLIN))
            continue;
        m = (struct isc_msg *)list_get(idev->recvq.rp, NULL);
        isc_handle_msg(idev, m);
        rc = isc_send_ack(idev->fd, m->seq);
        if (rc < 0) {
            LOGE("failed to call isc_send_ack (rc=%d)\n", rc);
            continue;
        }
        idev->recvq.rp = list_next(idev->recvq.rp);
    }
    return NULL;
}

static int isc_create_task(struct isc_device *idev)
{
    int rc;
    int fd;

    fd = eventfd(0, 0);
    if (fd < 0)
        return fd;

    idev->efd = fd;
    idev->is_task_started = true;
    rc = pthread_create(&idev->task_handle, NULL, isc_task_handler, idev);
    if (rc < 0) {
        idev->is_task_started = false;
        close(fd);
        return rc;
    }
    return 0;
}

static void isc_destroy_task(struct isc_device *idev)
{
    uint64_t u = 1;
    ssize_t rn;

    idev->is_task_started = false;

    rn = write(idev->efd, &u, sizeof(u));
    (void)rn;

    pthread_join(idev->task_handle, NULL);
    close(idev->efd);
}

static void isc_destroy_queue(struct isc_queue *q)
{
    struct list *pos, *next;

    next = q->wp;
    pos = q->wp;
    while (next) {
        next = list_del(pos);
        free(pos);
        pos = next;
    }
    q->wp = NULL;
    q->rp = NULL;
}

static int isc_create_queue(struct isc_queue *q, uint16_t msz, uint16_t num,
                            uint8_t *mem)
{
    uint32_t node_sz = msz + sizeof(struct isc_msg);
    struct isc_msg *m;
    struct list *pos;
    uint16_t i = 0;

    while (i < num) {
        pos = (struct list *)calloc(1, sizeof(struct list));
        if (!pos) {
            isc_destroy_queue(q);
            return -1;
        }
        list_init(pos);

        m = (struct isc_msg *)(mem + i * node_sz);
        list_set(pos, m, msz);
        if (i > 0) {
            list_add(pos, q->wp);
        } else {
            q->wp = pos;
            q->rp = pos;
        }
        i++;
    }
    return 0;
}

static void isc_close(struct isc_handle *isc)
{
    struct isc_device *idev = (struct isc_device *)isc;
    int rc, noarg = 0;

    if (!idev)
        return;

    isc_destroy_task(idev);

    if (idev->sendq.mem) {
        isc_destroy_queue(&idev->sendq);
        munmap(idev->sendq.mem, idev->sendq.size);
    }
    if (idev->recvq.mem) {
        isc_destroy_queue(&idev->recvq);
        munmap(idev->recvq.mem, idev->recvq.size);
    }

    rc = ioctl(idev->fd, ISC_IOCTL_CLOSE, &noarg);
    if (rc < 0)
        LOGE("failed to ioctl ISC_IOCTL_CLOSE (rc=%s)\n", strerror(errno));

    pthread_mutex_destroy(&idev->send_lock);
    close(idev->fd);
    free(idev);
}

static int isc_send_msg(struct isc_handle *isc, void *msg, uint32_t len,
                        int32_t *result)
{
    struct isc_device *idev = (struct isc_device *)isc;
    struct isc_send send;
    struct isc_msg *m;
    uint32_t sz;
    bool ready;
    int rc;

    if (!idev || !msg || !len)
        return -1;

    if (!(idev->direct & ISC_DIR_SEND))
        return -1;

    pthread_mutex_lock(&idev->send_lock);
    ready = idev->send_ready;
    pthread_mutex_unlock(&idev->send_lock);

    if (!ready)
        return -1;

    m = (struct isc_msg *)list_get(idev->sendq.wp, &sz);
    if (sz < len)
        return -1;

    m->seq = idev->seq;
    m->len = len;
    m->flags &= ~ISC_MSG_FLAG_USER;
    m->flags |= ISC_MSG_FLAG_USER;
    memcpy(m->d, msg, len);

    memset(&send, 0, sizeof(send));
    send.num = 1;
    send.seq = idev->seq;
    rc = ioctl(idev->fd, ISC_IOCTL_SEND, &send);
    if (rc < 0) {
        LOGE("failed to ioctl ISC_IOCTL_SEND (rc=%s)\n", strerror(errno));
        return rc;
    }

    *result = m->rc;
    if (!m->rc)
        memcpy(msg, m->d, len);
    idev->sendq.wp = list_next(idev->sendq.wp);
    idev->sendq.rp = list_next(idev->sendq.rp);

    idev->seq++;
    return rc;
}

static int isc_try_bind(struct isc_device *idev, uint32_t msz, uint32_t num,
                        bool is_send)
{
    struct isc_bind bind;
    struct isc_queue *q;
    int rc;

    memset(&bind, 0, sizeof(bind));
    bind.uid = idev->uid;
    bind.msz = msz;
    bind.num = num;
    if (is_send) {
        bind.dir = ISC_BIND_U_2_K;
        q = &idev->sendq;
    } else {
        bind.dir = ISC_BIND_K_2_U;
        q = &idev->recvq;
    }

    rc = ioctl(idev->fd, ISC_IOCTL_BIND, &bind);
    if (rc < 0) {
        LOGE("failed to ioctl ISC_IOCTL_BIND (rc=%s)\n", strerror(errno));
        return rc;
    }

    if (bind.size < (msz + sizeof(struct isc_msg)) * num)
        return -1;

    q->mem = mmap(0, bind.size, PROT_READ | PROT_WRITE, MAP_SHARED, idev->fd,
                  bind.mem);
    if (q->mem == MAP_FAILED)
        return -1;

    if (bind.stat == 1) {
        if (is_send) {
            pthread_mutex_lock(&idev->send_lock);
            idev->send_ready = true;
            pthread_mutex_unlock(&idev->send_lock);
        } else {
            idev->recv_ready = true;
        }
    }

    return isc_create_queue(q, msz, num, q->mem);
}

static struct list *isc_create_new_listener(const struct isc_listener_ops *ops,
                                            void *arg)
{
    struct list *pos;
    struct isc_listener *li;

    pos = (struct list *)calloc(1, sizeof(*pos));
    if (!pos)
        return NULL;

    list_init(pos);

    li = (struct isc_listener *)calloc(1, sizeof(*li));
    if (!li) {
        free(pos);
        return NULL;
    }

    li->ops = ops;
    li->arg = arg;
    list_set(pos, li, sizeof(*li));
    return pos;
}

static void isc_destroy_listener(struct list *pos)
{
    free(list_get(pos, NULL));
    free(pos);
}

static int isc_add_listener(struct isc_handle *isc,
                            const struct isc_listener_ops *ops, void *arg)
{
    struct isc_device *idev = (struct isc_device *)isc;
    struct isc_listener *li;
    struct list *pos;
    int rc = -1;

    if (!idev || !ops)
        return -1;

    if (!ops->bound && !ops->unbind && !ops->got)
        return -1;

    pthread_mutex_lock(&idev->listener_lock);
    pos = idev->listener_list;
    if (!pos)
        goto _null_pos;

    do {
        li = (struct isc_listener *)list_get(pos, NULL);
        if (li->ops == ops && li->arg == arg)
            goto _exit;
        pos = list_next(pos);
    } while (pos != idev->listener_list);

_null_pos:
    pos = isc_create_new_listener(ops, arg);
    if (!pos)
        goto _exit;

    if (idev->listener_list)
        list_add(pos, idev->listener_list);
    else
        idev->listener_list = pos;
    rc = 0;

_exit:
    pthread_mutex_unlock(&idev->listener_lock);

    if (idev->recv_ready && ops->bound)
        ops->bound(arg);
    return rc;
}

static int isc_rm_listener(struct isc_handle *isc,
                           const struct isc_listener_ops *ops, void *arg)
{
    struct isc_device *idev = (struct isc_device *)isc;
    struct isc_listener *li;
    struct list *pos;
    int rc = 0;

    if (!idev || !ops)
        return -1;

    pthread_mutex_lock(&idev->listener_lock);
    pos = idev->listener_list;
    if (!pos)
        goto _exit;

    do {
        li = (struct isc_listener *)list_get(pos, NULL);
        if (li->ops == ops && li->arg == arg) {
            if (pos == idev->listener_list)
                idev->listener_list = pos->next;
            list_del(pos);
            isc_destroy_listener(pos);
            goto _exit;
        }
        pos = list_next(pos);
    } while (pos != idev->listener_list);
    rc = -1;

_exit:
    pthread_mutex_unlock(&idev->listener_lock);
    return rc;
}

int open_isc(uint32_t uid, struct isc_attr *s, struct isc_attr *r,
             struct isc_handle **isc)
{
    struct isc_device *idev;
    int fd;
    int rc;
    uint32_t direct = 0;
    struct isc_attr recv;

    if (!isc)
        return -1;

    if (s)
        direct |= ISC_DIR_SEND;
    if (r)
        direct |= ISC_DIR_RECV;

    fd = open(ISC_DEV_NAME, O_RDWR);
    if (fd < 0)
        return -1;

    idev = (struct isc_device *)calloc(1, sizeof(*idev));
    if (!idev) {
        close(fd);
        return -1;
    }

    idev->fd = fd;
    idev->uid = uid;
    idev->direct = direct;

    pthread_mutex_init(&idev->send_lock, NULL);

    rc = isc_create_task(idev);
    if (rc < 0) {
        free(idev);
        close(fd);
        return rc;
    }

    if (direct & ISC_DIR_RECV) {
        if (r->msz < sizeof(struct isc_int_msg))
            recv.msz = sizeof(struct isc_int_msg);
        else
            recv.msz = r->msz;
        recv.num = r->num;
    } else {
        recv.msz = sizeof(struct isc_int_msg);
        recv.num = 8;
    }

    rc = isc_try_bind(idev, recv.msz, recv.num, false);
    if (rc < 0) {
        isc_destroy_task(idev);
        pthread_mutex_destroy(&idev->send_lock);
        free(idev);
        close(fd);
        return rc;
    }

    if (direct & ISC_DIR_SEND) {
        rc = isc_try_bind(idev, s->msz, s->num, true);
        if (rc < 0) {
            isc_destroy_task(idev);
            pthread_mutex_destroy(&idev->send_lock);
            free(idev);
            close(fd);
            return rc;
        }
    }

    idev->isc.close = isc_close;
    idev->isc.send = isc_send_msg;
    idev->isc.add_listener = isc_add_listener;
    idev->isc.rm_listener = isc_rm_listener;

    *isc = &idev->isc;
    return 0;
}
