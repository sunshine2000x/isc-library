// See LICENSE for license details.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "isc.h"
#include "sample_uapi.h"

#define LOGI(...) fprintf(stdout, __VA_ARGS__)
#define LOGE(...) fprintf(stderr, __VA_ARGS__)

struct sample_data {
    uint32_t id;
    struct isc_handle *isc;
};

static int32_t sample_msg_handler(void *msg, uint32_t len, void *arg)
{
    struct sample_msg *m = (struct sample_msg *)msg;

    if (!msg || len < sizeof(struct sample_msg))
        return -1;

    if (m->id == SAMPLE_MSG_IRQ_STAT)
        LOGI("got irq:%d, stat:0x%x\n", m->irq.id, m->irq.stat);
    return 0;
}

static void sample_bound(void *arg)
{
}

static void sample_unbind(void *arg)
{
}

static const struct isc_listener_ops sample_listener_ops = {
    .bound = sample_bound,
    .unbind = sample_unbind,
    .got = sample_msg_handler,
};

static int sample_open(uint32_t id, struct sample_data **ppdata)
{
    struct sample_data *pdata;
    struct isc_attr s = {sizeof(struct sample_msg), 64};
    int rc;

    if (!ppdata)
        return -1;

    pdata = (struct sample_data *)calloc(1, sizeof(*pdata));
    if (!pdata)
        return -1;

    pdata->id = id;

    rc = open_isc(SAMPLE_UID(id), &s, &s, &pdata->isc);
    if (rc < 0) {
        free(pdata);
        return rc;
    }

    *ppdata = pdata;
    return 0;
}

static void sample_close(struct sample_data *pdata)
{
    if (!pdata)
        return;

    pdata->isc->close(pdata->isc);
    pdata->isc = NULL;

    free(pdata);
}

static int sample_reg_read(struct sample_data *pdata, uint32_t offset,
                           uint32_t *value)
{
    struct sample_msg msg;
    int32_t result;
    int rc;

    if (!pdata || !value)
        return -1;

    msg.id = SAMPLE_MSG_READ_REG;
    msg.reg.offset = offset;
    msg.reg.value = 0;
    rc = pdata->isc->send(pdata->isc, &msg, sizeof(msg), &result);
    if (rc < 0 || result < 0)
        return -1;

    *value = msg.reg.value;
    return 0;
}

static int sample_reg_write(struct sample_data *pdata, uint32_t offset,
                            uint32_t value)
{
    struct sample_msg msg;
    int32_t result;
    int rc;

    if (!pdata)
        return -1;

    msg.id = SAMPLE_MSG_WRITE_REG;
    msg.reg.offset = offset;
    msg.reg.value = value;
    rc = pdata->isc->send(pdata->isc, &msg, sizeof(msg), &result);
    if (rc < 0 || result < 0)
        return -1;

    return 0;
}

static int sample_add_listener(struct sample_data *pdata)
{
    if (!pdata)
        return -1;

    return pdata->isc->add_listener(pdata->isc, &sample_listener_ops, pdata);
}

static int sample_rm_listener(struct sample_data *pdata)
{
    if (!pdata)
        return -1;

    return pdata->isc->rm_listener(pdata->isc, &sample_listener_ops, pdata);
}

int main(int argc, char *argv[])
{
    struct sample_data *pdata;
    int rc;
    int i = 0, count = 32 /*how many registers will be checked*/;

    srand(time(NULL));

    rc = sample_open(0, &pdata);
    if (rc < 0) {
        LOGE("failed to call sample_open (rc=%d)\n", rc);
        return -1;
    }

    rc = sample_add_listener(pdata);
    if (rc < 0) {
        LOGE("failed to call sample_add_listener (rc=%d)\n", rc);
        sample_close(pdata);
        return rc;
    }

    while (i < count) {
        uint32_t offset = i * 4, value_w = rand(), value_r;

        rc = sample_reg_write(pdata, offset, value_w);
        if (rc < 0) {
            LOGE("failed to call sample_reg_write (rc=%d)\n", rc);
            sample_close(pdata);
            return rc;
        }

        rc = sample_reg_read(pdata, offset, &value_r);
        if (rc < 0) {
            LOGE("failed to call sample_reg_read (rc=%d)\n", rc);
            sample_close(pdata);
            return rc;
        }

        if (value_r != value_w)
            LOGE("write and read of reg (0x%08x) not consistent!\n", offset);
        else
            LOGI("read hw reg (0x%08x)=(0x%08x)\n", offset, value_r);

        usleep(rand() % 1000000);
        i++;
    }

    rc = sample_rm_listener(pdata);
    if (rc < 0) {
        LOGE("failed to call sample_rm_listener (rc=%d)\n", rc);
        sample_close(pdata);
        return rc;
    }

    sample_close(pdata);
    return 0;
}
