/*
 * QEMU Block driver for PureFlash (https://github.com/cocalele/PureFlash)
 *
 * Copyright (C) 2021 LiuLele <liu_lele@126.com>
 * Copyright (C) 2010-2011 Christian Brunner <chb@muc.de>,
 *                         Josh Durgin <josh.durgin@dreamhost.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include <time.h>
#include <pfbd/pf_client_api.h>
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "qemu/typedefs.h"
#include "block/block-io.h"
#include "block/block_int.h"
#include "block/qdict.h"
#include "crypto/secret.h"
#include "qemu/cutils.h"
#include "sysemu/replay.h"
#include "qapi/qmp/qstring.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qlist.h"
#include "qapi/qobject-input-visitor.h"
#include "qapi/qapi-visit-block-core.h"
#include "block/aio.h"

#define USE_COROUTINE
struct PfbdCoData;
struct BDRVPfbdState;

typedef struct PfbdCoData {
    Coroutine* co;
    //struct BDRVPfbdState* bs;
    BlockDriverState* bds;
    bool complete;
    volatile int ret;
    uint64_t submit_time;
    //AioContext* ctx;
} PfbdCoData;


#if 0
typedef struct PfbdCompleteQueue
{
    int tail;			///< tail pointer
    int head;			///< head pointer
    int queue_depth;	///< queue depth, max number of element can be put to this queue plus 1
    struct PfbdCoData** data;		///< memory used by this queue, it's size of queue_depth * ele_size
    pthread_spinlock_t lock;
    //struct CoMutex mutex;
    //char name[44];
}PfbdCompleteQueue;
#define QUEUE_SPACE(size, head, tail) 	 ( (head) <=  (tail) ?   (size - tail + head - 1) : (head - tail - 1) )
#define QUEUE_COUNT(size, head, tail) ( (head) <= (tail) ? (tail-head) : (size + tail - head) )

/**
 * Initialize a queue.
 *
 * The queue depth conforms to Depth = 2 ^ queue_depth_order - 1.
 * For example, with queue_depth_order=10, this queue can contains 1023 elements.
 * because this queue is implemented with an array which size is of 2 ^ queue_depth_order
 * and the queue can contain array_size - 1 elements.
 *
 * @param[in,out]	queue				queue to initialize
 * @param[in]		queue_depth_order	queue depth order value
 * @return 0 on success, -ENOMEM on fail
 */
static int cq_init(PfbdCompleteQueue* pthis, int _queue_depth)
{
    pthis->head = pthis->tail = 0;
    pthis->queue_depth = _queue_depth + 1;
    pthis->data = (PfbdCoData**)calloc((size_t)_queue_depth, sizeof(PfbdCoData*));
    if (pthis->data == NULL)
        return -ENOMEM;
    //qemu_co_mutex_init(&pthis->mutex);
    pthread_spin_init(&pthis->lock, 0);
    return 0;
}

/**
 * Destroy an unused queue.
 */
static void cq_destroy(PfbdCompleteQueue* pthis )
{
    pthis->queue_depth = pthis->tail = pthis->head = 0;
    free(pthis->data);
    pthis->data = NULL;
    //pthread_spin_destroy(&lock);
}
/**
 * Returns whether the queue is empty(i.e. whether its size is 0).
 *
 * @param[in]	queue	queue depth
 *
 * @retval		1		queue is empty
 * @retval		0		queue is not empty
 */
static inline bool cq_is_empty(PfbdCompleteQueue* pthis ) { return pthis->tail == pthis->head; }
/**
 * Returns whether the queue container is full(i.e. whether its remaining space is 0).
 *
 * @param[in]	queue	queue depth
 *
 * @retval		1		queue is full
 * @retval		0		queue is not full
 */
static inline bool cq_is_full(PfbdCompleteQueue* pthis ) { return QUEUE_SPACE(pthis->queue_depth, pthis->head, pthis->tail) == 0; }

/**
 * Get the available space in queue.
 *
 * @param[in]	queue	queue to compute
 *
 * @return available space in queue.
 */
static inline int cq_space(PfbdCompleteQueue* pthis ) { return QUEUE_SPACE(pthis->queue_depth, pthis->head, pthis->tail); }

/**
 * get the valid entries count in queue
 *
 * @param[in]	queue	queue to compute
 *
 * @return valid element number in queue.
 */
static inline int cq_count(PfbdCompleteQueue* pthis ) { return QUEUE_COUNT(pthis->queue_depth, pthis->head, pthis->tail); }

static inline int cq_enqueue_nolock(PfbdCompleteQueue* pthis, /*in*/PfbdCoData* element)
{ //this function should be called from PfClient event thread only, and only one thread access this

    pthis->data[pthis->tail] = element;
    smp_wmb();
    pthis->tail = (pthis->tail + 1) % pthis->queue_depth;
    return 0;
}
/**
 * Dequeue an element from the head of the queue.
 *
 * User cannot long term retain element dequeued, and should not free it also.
 *
 * @param[in]	queue	the queue to operate on
 * @return a T
 * @throws bad_logic exception if queue is empty.
 *
 * @mark, dequeue must return a value of T, not T* or T&. T* or T& is a pointer to data[i],
 * after dequeue return, data[i] may has changed its value by other call to enqueue before
 * caller consume dequeue's return value
 */
static inline PfbdCoData* cq_dequeue_nolock(PfbdCompleteQueue* pthis)
{ //this function should be called from qemu coroutine only
    smp_rmb();
    PfbdCoData* t = pthis->data[pthis->head];
    pthis->head = (pthis->head + 1) % pthis->queue_depth;
    return t;
}
//inline T dequeue(PfbdCompleteQueue* pthis, )
//{
//    AutoSpinLock l(&lock);
//    return dequeue_nolock();
//}


#endif


//typedef struct {
//    int plugged;
//    unsigned int in_queue;
//    unsigned int in_flight;
//    bool blocked;
//    QSIMPLEQ_HEAD(, PfbdCoData) pending;
//} PfbdIoQueue; // copy from LaioQueue;
//

typedef struct BDRVPfbdState {
    char volume_name[128];
    int64_t image_size;

    struct PfClientVolume* vol;
   // AioContext* aio_context;

    //EventNotifier e;

    /* io queue for submit at batch.  Protected by AioContext lock. */
    //PfbdIoQueue io_q;

    //PfbdIoQueue io_cq; //completion queue
    //PfbdCompleteQueue io_cq;
    /* I/O completion processing.  Only runs in I/O thread.  */
    //QEMUBH* completion_bh;
    //int event_idx;
    //int event_max;
    //bool plugged;
    char conf_path[1024];
} BDRVPfbdState;



static char *qemu_pfbd_next_tok(char *src, char delim, char **p)
{
	char *end;

	*p = NULL;

	for (end = src; *end; ++end) {
		if (*end == delim) {
			break;
		}
		if (*end == '\\' && end[1] != '\0') {
			end++;
		}
	}
	if (*end == delim) {
		*p = end + 1;
		*end = '\0';
	}
	return src;
}

static void qemu_pfbd_unescape(char *src)
{
	char *p;

	for (p = src; *src; ++src, ++p) {
		if (*src == '\\' && src[1] != '\0') {
			src++;
		}
		*p = *src;
	}
	*p = '\0';
}

static void qemu_pfbd_parse_filename(const char *filename, QDict *options,
                                    Error **errp)
{
	const char *start;
	char *p, *buf;
	QList *keypairs = NULL;
	char *found_str, *image_name;

	if (!strstart(filename, "pfbd:", &start)) {
		error_setg(errp, "File name must start with 'pfbd:'");
		return;
	}

	buf = g_strdup(start);
	p = buf;

	if (strchr(p, '@')) {
		image_name = qemu_pfbd_next_tok(p, '@', &p);

		found_str = qemu_pfbd_next_tok(p, ':', &p);
		qemu_pfbd_unescape(found_str);
		qdict_put_str(options, "snapshot", found_str);
	} else {
		image_name = qemu_pfbd_next_tok(p, ':', &p);
	}

	qemu_pfbd_unescape(image_name);
	qdict_put_str(options, "volume", image_name);
	if (!p) {
		goto done;
	}

	/* The following are essentially all key/value pairs, and we treat
	 * 'id' and 'conf' a bit special.  Key/value pairs may be in any order. */
	while (p) {
		char *name, *value;
		name = qemu_pfbd_next_tok(p, '=', &p);
		if (!p) {
			error_setg(errp, "conf option %s has no value", name);
			break;
		}

		qemu_pfbd_unescape(name);

		value = qemu_pfbd_next_tok(p, ':', &p);
		qemu_pfbd_unescape(value);

		if (!strcmp(name, "conf")) {
			qdict_put_str(options, "conf", value);
		} else if (!strcmp(name, "id")) {
			qdict_put_str(options, "user", value);
		} else {
			/*
			 * We pass these internally to qemu_pfbd_set_keypairs(), so
			 * we can get away with the simpler list of [ "key1",
			 * "value1", "key2", "value2" ] rather than a raw dict
			 * { "key1": "value1", "key2": "value2" } where we can't
			 * guarantee order, or even a more correct but complex
			 * [ { "key1": "value1" }, { "key2": "value2" } ]
			 */
			if (!keypairs) {
				keypairs = qlist_new();
			}
			qlist_append_str(keypairs, name);
			qlist_append_str(keypairs, value);
		}
	}

	if (keypairs) {
		qdict_put(options, "=keyvalue-pairs",
		          qstring_from_gstring(qobject_to_json(QOBJECT(keypairs))));
	}

done:
	g_free(buf);
	qobject_unref(keypairs);
	return;
}


static void qemu_pfbd_refresh_limits(BlockDriverState* bs, Error** errp)
{
    //BDRVPfbdState* s = bs->opaque;

    bs->bl.opt_mem_alignment = 4096;
    bs->bl.request_alignment = 4096; //this will prevent guest send non 4k aligned IO
    bs->bl.min_mem_alignment = 4096;
    bs->bl.max_transfer = PF_MAX_IO_SIZE;
}

#if 0
static void process_pfbd_io_completions(BDRVPfbdState* s)
{
    PfbdCoData* iocb;
    //qemu_co_mutex_lock(&s->io_cq.mutex);
    //pthread_spin_lock(&s->io_cq.lock);
    while (!cq_is_empty(&s->io_cq)) {
        iocb = cq_dequeue_nolock(&s->io_cq);
#if 1
        if (!qemu_coroutine_entered(iocb->co)) {
            if(iocb->co){
                aio_co_wake(iocb->co);
                
            }
        }
#else
                
            if (!iocb->co) {
                return;
            }
            replay_bh_schedule_oneshot_event(iocb->ctx, pfbd_rw_cb_bh, iocb);
#endif
    }
    //qemu_co_mutex_unlock(&s->io_cq.mutex); //can't used here, not in coroutine
    //pthread_spin_unlock(&s->io_cq.lock);
}

static void pfbd_process_completion_bh(void* opaque)
{
    BDRVPfbdState* s = opaque;
    
    qemu_bh_schedule(s->completion_bh);
    process_pfbd_io_completions(s);
    qemu_bh_cancel(s->completion_bh);

}

static void qemu_pfbd_completion_cb(EventNotifier* e)
{
    BDRVPfbdState* s = container_of(e, BDRVPfbdState, e);

    if (event_notifier_test_and_clear(&s->e)) {
        process_pfbd_io_completions(s);
    }
}

static bool qemu_pfbd_poll_cb(void* opaque)
{
    EventNotifier* e = opaque;
    BDRVPfbdState* s = container_of(e, BDRVPfbdState, e);


    if (cq_is_empty(&s->io_cq)) {
        return false;
    }

    process_pfbd_io_completions(s);
    return true;
}
#endif

static int qemu_pfbd_open(BlockDriverState *bs, QDict *options, int flags,
                         Error **errp)
{
	BDRVPfbdState *s = bs->opaque;
	const QDictEntry *e;
	//Error *local_err = NULL;
	char *volume_name, *conf_file;
	int r=0;

	volume_name = g_strdup(qdict_get_try_str(options, "volume"));
	conf_file = g_strdup(qdict_get_try_str(options, "conf"));
    strncpy(s->volume_name, volume_name, sizeof(s->volume_name)-1);
    strncpy(s->conf_path, conf_file ? conf_file : "/etc/pureflash/pf.conf", sizeof(s->conf_path)-1);
	s->vol = pf_open_volume(volume_name, s->conf_path, NULL, S5_LIB_VER);

	if (s->vol == NULL) {
		goto out;
	}


	s->image_size = pf_get_volume_size(s->vol);

    /* If we are using an rbd snapshot, we must be r/o, otherwise
        * leave as-is */
    if (qdict_get_try_str(options, "snapshot") != NULL) {
        r = bdrv_apply_auto_read_only(bs, "snapshots are read-only", errp);
        if (r < 0) {
            pf_close_volume(s->vol);
            goto out;
        }
    }


    while ((e = qdict_first(options))) {
        qdict_del(options, e->key);
    }
	/* When extending regular files, we get zeros from the OS */
	bs->supported_truncate_flags = BDRV_REQ_ZERO_WRITE;
    bs->bl.max_iov = 1;
    bs->bl.max_transfer = PF_MAX_IO_SIZE;
    bs->bl.opt_mem_alignment = 4096;
    bs->bl.request_alignment = 4096;
    bs->bl.min_mem_alignment = 4096;

#if 0
    cq_init(&s->io_cq, 2048);
    s->aio_context = bdrv_get_aio_context(bs);


    int rc = event_notifier_init(&s->e, false);
    if (rc < 0) {
        error_setg_errno(errp, -rc, "failed to to initialize event notifier");
        goto out;
    }

    s->completion_bh = aio_bh_new(s->aio_context, pfbd_process_completion_bh, s);
    aio_set_event_notifier(s->aio_context, &s->e, false,
        qemu_pfbd_completion_cb,
        qemu_pfbd_poll_cb);

#endif


	r = 0;
	goto out;


out:
	g_free(conf_file);
	g_free(volume_name);
	return r;
}


/* Since RBD is currently always opened R/W via the API,
 * we just need to check if we are using a snapshot or not, in
 * order to determine if we will allow it to be R/W */
static int qemu_pfbd_reopen_prepare(BDRVReopenState *state,
                                   BlockReopenQueue *queue, Error **errp)
{
	//BDRVPfbdState *s = state->bs->opaque;
	int ret = 0;

	return ret;
}

static void qemu_pfbd_close(BlockDriverState *bs)
{
	BDRVPfbdState *s = bs->opaque;

	pf_close_volume(s->vol);
#if 0
    cq_destroy(&s->io_cq);
    g_free(s->completion_bh);//allocaed by g_new
#endif
}
//static uint64_t now_time_usec(void)
//{
//    struct timespec tp;
//    clock_gettime(CLOCK_MONOTONIC_RAW, &tp);
//    return tp.tv_sec * 1000000LL + tp.tv_nsec / 1000;
//}


//static const AIOCBInfo pfbd_aiocb_info = {
//		.aiocb_size = sizeof(PFBDAIOCB),
//};
#ifndef USE_COROUTINE
static BlockAIOCB read_aiocb, write_aiocb;
static BlockAIOCB* qemu_pfbd_aio_preadv(BlockDriverState* bs,
    uint64_t offset, uint64_t bytes,
    QEMUIOVector* qiov, int flags,
    BlockCompletionFunc* cb,
    void* opaque)
{
    BDRVPfbdState* s = bs->opaque;
    int r;
    r = pf_iov_submit(s->vol, qiov->iov, qiov->niov, bytes, offset, cb, opaque, 0);
    if (r != 0) {
        error_report("Failed pf_io_submit_read, rc:%d", r);
        return NULL;
    }
    return &read_aiocb;
}

static BlockAIOCB *qemu_pfbd_aio_pwritev(BlockDriverState *bs,
                                        uint64_t offset, uint64_t bytes,
                                        QEMUIOVector *qiov, int flags,
                                        BlockCompletionFunc *cb,
                                        void *opaque)
{
    BDRVPfbdState* s = bs->opaque;
    int r;
    //char* buf;
    r = pf_iov_submit(s->vol, qiov->iov, qiov->niov, bytes, offset, cb, opaque, 1);
    if (r != 0) {
        error_report("Failed pf_io_submit_write, rc:%d", r);
        return NULL;
    }
    return &write_aiocb;
}
#else

static void pfbd_rw_cb_bh(void* opaque)
{
    PfbdCoData* data = opaque;
    data->complete=true;
    qemu_coroutine_enter(data->co);
}

//static void pfbd_rw_cb(void* opaque, int ret)
//{
//    PfbdCoData* data = opaque;
//    data->ret = ret;
//    if (!data->co) {
//        /* The rw coroutine hasn't yielded, don't try to enter. */
//        return;
//    }
//    replay_bh_schedule_oneshot_event(data->ctx, pfbd_rw_cb_bh, data);
//    //qemu_coroutine_enter(data->co); //may: Co-routine re-entered recursively
//}
static void pfbd_rw_cb(void* opaque, int ret)
{
    PfbdCoData* data = opaque;
    data->ret = ret;

    aio_bh_schedule_oneshot(bdrv_get_aio_context(data->bds),
        pfbd_rw_cb_bh, data);

#if 0
    BDRVPfbdState* s = data->bs;
    while (cq_is_full(&s->io_cq)) {
        warn_report("Pfbd complete queue full");
        usleep(100);
    }
    cq_enqueue_nolock(&s->io_cq, data);
    static int64_t v=1;

    
    int rc = write(event_notifier_get_fd(&s->e), &v, sizeof(v));
    if(rc != sizeof(v)){
        warn_report("Failed to write event fd, rc:%d", errno);
    }
#endif
}

static void pfbd_read_cb(void* opaque, int ret){
    //PfbdCoData* data = opaque;
    //uint64_t now_t = now_time_usec();
    //if(now_t < data->submit_time + 500)
    //    usleep(500);
    pfbd_rw_cb(opaque, ret);
}

static coroutine_fn int pfbd_co_preadv(BlockDriverState* bs,
    int64_t offset, int64_t bytes,
    QEMUIOVector* qiov, BdrvRequestFlags flags)
{
    int r;
    BDRVPfbdState* s = bs->opaque;
   
    PfbdCoData data = {
        //.ctx = bdrv_get_aio_context(bs),
        //.ctx = s->aio_context,
        .ret = -EINPROGRESS,
        .co= qemu_coroutine_self() ,
        .complete = false,
        .bds = bs,
        //.submit_time = now_time_usec()
    };
    
    r = pf_iov_submit(s->vol, qiov->iov, qiov->niov, bytes, offset, pfbd_read_cb, &data, 0);
    if(r){
        error_report("submit IO error, rc:%d", r);
        return r;
    }

    while (!data.complete) {
        qemu_coroutine_yield();
    }
    return data.ret;
}
static coroutine_fn int pfbd_co_pwritev(BlockDriverState* bs,
    int64_t offset, int64_t bytes,
    QEMUIOVector* qiov, BdrvRequestFlags flags)
{
    int r;
    BDRVPfbdState* s = bs->opaque;

    PfbdCoData data = {
        //.ctx = bdrv_get_aio_context(bs),
        //.ctx = s->aio_context,
        .ret = -EINPROGRESS,
        .co = qemu_coroutine_self() ,
        .complete=false,
        .bds = bs
    };


    //data.co = qemu_coroutine_self();
    r = pf_iov_submit(s->vol, qiov->iov, qiov->niov, bytes, offset, pfbd_rw_cb, &data, 1);
    if (r) {
        error_report("submit IO error, rc:%d", r);
        return r;
    }


    while (!data.complete) {
        qemu_coroutine_yield();
    }
    return data.ret;
}
#endif

//static int qemu_pfbd_getinfo(BlockDriverState *bs, BlockDriverInfo *bdi)
//{
//	BDRVPfbdState *s = bs->opaque;
//	struct PfClientVolumeInfo info;
//	int r;
//	r = pf_query_volume_info(s->volume_name, s->conf_path, NULL, S5_LIB_VER, &info);
//	if (r < 0) {
//		return r;
//	}
//
//	bdi->cluster_size = info.volume_size;
//	return 0;
//}
static int qemu_pfbd_co_getinfo(BlockDriverState* bs, BlockDriverInfo* bdi)
{
    //BDRVPfbdState* s = bs->opaque;
    bdi->cluster_size = 128<<10;
    return 0;
}
static int64_t qemu_pfbd_co_getlength(BlockDriverState *bs)
{
    BDRVPfbdState* s = bs->opaque;
    //struct PfClientVolumeInfo info;
    //int r;

    //r = pf_query_volume_info(s->volume_name, s->conf_path, NULL, S5_LIB_VER, &info);
    //if (r < 0) {
    //    return r;
    //}
    if(s->image_size==0){
        error_report("volume size is 0");
    }
	return s->image_size;
}

//
//static void pfbd_attach_aio_context(BlockDriverState* bs,
//    AioContext* new_context)
//{
//    BDRVPfbdState* s = bs->opaque;
//
//
//    int rc = event_notifier_init(&s->e, false);
//    if (rc < 0) {
//        error_report("failed to to initialize event notifier");
//        return;
//    }
//    s->aio_context = new_context;
//    s->completion_bh =
//        aio_bh_new(new_context, pfbd_process_completion_bh, s);
//    aio_set_event_notifier(new_context, &s->e,
//        false, qemu_pfbd_completion_cb,
//        qemu_pfbd_poll_cb);
//
//
//}
//static void pfbd_detach_aio_context(BlockDriverState* bs)
//{
//    BDRVPfbdState* s = bs->opaque;
//
//    qemu_bh_delete(s->completion_bh);
//    s->completion_bh = NULL;
//
//
//    aio_set_event_notifier(bdrv_get_aio_context(bs),
//        &s->e,
//        false, NULL, NULL);
//}


static QemuOptsList qemu_pfbd_create_opts = {
		.name = "pfbd-create-opts",
		.head = QTAILQ_HEAD_INITIALIZER(qemu_pfbd_create_opts.head),
		.desc = {
				{
						.name = BLOCK_OPT_SIZE,
						.type = QEMU_OPT_SIZE,
						.help = "Virtual disk size"
				},
				{
						.name = BLOCK_OPT_CLUSTER_SIZE,
						.type = QEMU_OPT_SIZE,
						.help = "RBD object size"
				},
				{
						.name = "password-secret",
						.type = QEMU_OPT_STRING,
						.help = "ID of secret providing the password",
				},
				{ /* end of list */ }
		}
};

static const char *const qemu_pfbd_strong_runtime_opts[] = {
		"volume",
		"conf",
		"snapshot",

		NULL
};
//static void pfbd_aio_plug(BlockDriverState* bs)
//{
//    BDRVPfbdState* s = bs->opaque;
//    assert(!s->plugged);
//    s->plugged = true;
//}
//
//static void pfbd_aio_unplug(BlockDriverState* bs)
//{
//    BDRVPfbdState* s = bs->opaque;
//    assert(s->plugged);
//    s->plugged = false;
//    
//}
static BlockDriver bdrv_pfbd = {
		.format_name            = "pfbd",
		.instance_size          = sizeof(BDRVPfbdState),
		.bdrv_parse_filename    = qemu_pfbd_parse_filename,
		.bdrv_refresh_limits    = qemu_pfbd_refresh_limits,
		.bdrv_file_open         = qemu_pfbd_open,
		.bdrv_close             = qemu_pfbd_close,
		.bdrv_reopen_prepare    = qemu_pfbd_reopen_prepare,
		.bdrv_has_zero_init     = bdrv_has_zero_init_1,
		//.bdrv_get_info          = qemu_pfbd_getinfo,
        .bdrv_co_get_info       = qemu_pfbd_co_getinfo,
        //.bdrv_co_create_opts    = qemu_pfbd_co_create_opts,
		.create_opts            = &qemu_pfbd_create_opts,
		.bdrv_co_getlength         = qemu_pfbd_co_getlength,
		.protocol_name          = "pfbd",
#ifndef USE_COROUTINE
		.bdrv_aio_preadv        = qemu_pfbd_aio_preadv,
		.bdrv_aio_pwritev       = qemu_pfbd_aio_pwritev,
#else
        .bdrv_co_preadv = pfbd_co_preadv,
        .bdrv_co_pwritev = pfbd_co_pwritev,
        //.bdrv_detach_aio_context = pfbd_detach_aio_context,
        //.bdrv_attach_aio_context = pfbd_attach_aio_context,
        //.bdrv_io_plug = pfbd_aio_plug,
        //.bdrv_io_unplug = pfbd_aio_unplug,

#endif
        .bdrv_refresh_limits    = qemu_pfbd_refresh_limits,

//#ifdef LIBRBD_SUPPORTS_AIO_FLUSH
//		.bdrv_aio_flush         = qemu_pfbd_aio_flush,
//#else
//		.bdrv_co_flush_to_disk  = qemu_pfbd_co_flush,
//#endif


//		.bdrv_aio_pdiscard      = qemu_pfbd_aio_pdiscard,



//#ifdef LIBRBD_SUPPORTS_INVALIDATE
//		.bdrv_co_invalidate_cache = qemu_pfbd_co_invalidate_cache,
//#endif

		.strong_runtime_opts    = qemu_pfbd_strong_runtime_opts,
};

static void bdrv_pfbd_init(void)
{
	bdrv_register(&bdrv_pfbd);
}

block_init(bdrv_pfbd_init);
