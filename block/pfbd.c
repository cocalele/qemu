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

	r = 0;
	goto out;


out:
	g_free(conf_file);
	g_free(volume_name);
	return r;
}



static int qemu_pfbd_reopen_prepare(BDRVReopenState *state,
                                   BlockReopenQueue *queue, Error **errp)
{
	int ret = 0;
	return ret;
}

static void qemu_pfbd_close(BlockDriverState *bs)
{
	BDRVPfbdState *s = bs->opaque;

	pf_close_volume(s->vol);
}


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
    
    r = pf_iov_submit(s->vol, qiov->iov, qiov->niov, bytes, offset, pfbd_rw_cb, &data, 0);
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
