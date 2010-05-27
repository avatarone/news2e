/*
 * Virtio Block Device
 *
 * Copyright IBM, Corp. 2007
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#include <qemu-common.h>
#include <sysemu.h>
#include "virtio-blk.h"
#include "block_int.h"
#ifdef __linux__
# include <scsi/sg.h>
#endif

typedef struct VirtIOBlock
{
    VirtIODevice vdev;
    BlockDriverState *bs;
    VirtQueue *vq;
    void *rq;
    char serial_str[BLOCK_SERIAL_STRLEN + 1];
    QEMUBH *bh;
    size_t config_size;
} VirtIOBlock;

static VirtIOBlock *to_virtio_blk(VirtIODevice *vdev)
{
    return (VirtIOBlock *)vdev;
}

/* store identify data in little endian format
 */
static inline void put_le16(uint16_t *p, unsigned int v)
{
    *p = cpu_to_le16(v);
}

/* copy to *dst from *src, nul pad dst tail as needed to len bytes
 */
static inline void padstr(char *dst, const char *src, int len)
{
    while (len--)
        *dst++ = *src ? *src++ : '\0';
}

/* setup simulated identify data as appropriate for virtio block device
 *
 * ref: AT Attachment 8 - ATA/ATAPI Command Set (ATA8-ACS)
 */
static inline void virtio_identify_template(struct virtio_blk_config *bc)
{
    uint16_t *p = &bc->identify[0];
    uint64_t lba_sectors = bc->capacity;

    memset(p, 0, sizeof(bc->identify));
    put_le16(p + 0, 0x0);                            /* ATA device */
    padstr((char *)(p + 23), QEMU_VERSION, 8);       /* firmware revision */
    padstr((char *)(p + 27), "QEMU VIRT_BLK", 40);   /* model# */
    put_le16(p + 47, 0x80ff);                        /* max xfer 255 sectors */
    put_le16(p + 49, 0x0b00);                        /* support IORDY/LBA/DMA */
    put_le16(p + 59, 0x1ff);                         /* cur xfer 255 sectors */
    put_le16(p + 80, 0x1f0);                         /* support ATA8/7/6/5/4 */
    put_le16(p + 81, 0x16);
    put_le16(p + 82, 0x400);
    put_le16(p + 83, 0x400);
    put_le16(p + 100, lba_sectors);
    put_le16(p + 101, lba_sectors >> 16);
    put_le16(p + 102, lba_sectors >> 32);
    put_le16(p + 103, lba_sectors >> 48);
}

typedef struct VirtIOBlockReq
{
    VirtIOBlock *dev;
    VirtQueueElement elem;
    struct virtio_blk_inhdr *in;
    struct virtio_blk_outhdr *out;
    struct virtio_scsi_inhdr *scsi;
    QEMUIOVector qiov;
    struct VirtIOBlockReq *next;
} VirtIOBlockReq;

static void virtio_blk_req_complete(VirtIOBlockReq *req, int status)
{
    VirtIOBlock *s = req->dev;

    req->in->status = status;
    virtqueue_push(s->vq, &req->elem, req->qiov.size + sizeof(*req->in));
    virtio_notify(&s->vdev, s->vq);

    qemu_free(req);
}

static int virtio_blk_handle_rw_error(VirtIOBlockReq *req, int error,
    int is_read)
{
    BlockInterfaceErrorAction action =
        drive_get_on_error(req->dev->bs, is_read);
    VirtIOBlock *s = req->dev;

    if (action == BLOCK_ERR_IGNORE)
        return 0;

    if ((error == ENOSPC && action == BLOCK_ERR_STOP_ENOSPC)
            || action == BLOCK_ERR_STOP_ANY) {
        req->next = s->rq;
        s->rq = req;
        vm_stop(0);
    } else {
        virtio_blk_req_complete(req, VIRTIO_BLK_S_IOERR);
    }

    return 1;
}

static void virtio_blk_rw_complete(void *opaque, int ret)
{
    VirtIOBlockReq *req = opaque;

    if (ret) {
        int is_read = !(req->out->type & VIRTIO_BLK_T_OUT);
        if (virtio_blk_handle_rw_error(req, -ret, is_read))
            return;
    }

    virtio_blk_req_complete(req, VIRTIO_BLK_S_OK);
}

static void virtio_blk_flush_complete(void *opaque, int ret)
{
    VirtIOBlockReq *req = opaque;

    virtio_blk_req_complete(req, ret ? VIRTIO_BLK_S_IOERR : VIRTIO_BLK_S_OK);
}

static VirtIOBlockReq *virtio_blk_alloc_request(VirtIOBlock *s)
{
    VirtIOBlockReq *req = qemu_mallocz(sizeof(*req));
    req->dev = s;
    return req;
}

static VirtIOBlockReq *virtio_blk_get_request(VirtIOBlock *s)
{
    VirtIOBlockReq *req = virtio_blk_alloc_request(s);

    if (req != NULL) {
        if (!virtqueue_pop(s->vq, &req->elem)) {
            qemu_free(req);
            return NULL;
        }
    }

    return req;
}

#ifdef __linux__
static void virtio_blk_handle_scsi(VirtIOBlockReq *req)
{
    struct sg_io_hdr hdr;
    int ret, size = 0;
    int status;
    int i;

    /*
     * We require at least one output segment each for the virtio_blk_outhdr
     * and the SCSI command block.
     *
     * We also at least require the virtio_blk_inhdr, the virtio_scsi_inhdr
     * and the sense buffer pointer in the input segments.
     */
    if (req->elem.out_num < 2 || req->elem.in_num < 3) {
        virtio_blk_req_complete(req, VIRTIO_BLK_S_IOERR);
        return;
    }

    /*
     * No support for bidirection commands yet.
     */
    if (req->elem.out_num > 2 && req->elem.in_num > 3) {
        virtio_blk_req_complete(req, VIRTIO_BLK_S_UNSUPP);
        return;
    }

    /*
     * The scsi inhdr is placed in the second-to-last input segment, just
     * before the regular inhdr.
     */
    req->scsi = (void *)req->elem.in_sg[req->elem.in_num - 2].iov_base;
    size = sizeof(*req->in) + sizeof(*req->scsi);

    memset(&hdr, 0, sizeof(struct sg_io_hdr));
    hdr.interface_id = 'S';
    hdr.cmd_len = req->elem.out_sg[1].iov_len;
    hdr.cmdp = req->elem.out_sg[1].iov_base;
    hdr.dxfer_len = 0;

    if (req->elem.out_num > 2) {
        /*
         * If there are more than the minimally required 2 output segments
         * there is write payload starting from the third iovec.
         */
        hdr.dxfer_direction = SG_DXFER_TO_DEV;
        hdr.iovec_count = req->elem.out_num - 2;

        for (i = 0; i < hdr.iovec_count; i++)
            hdr.dxfer_len += req->elem.out_sg[i + 2].iov_len;

        hdr.dxferp = req->elem.out_sg + 2;

    } else if (req->elem.in_num > 3) {
        /*
         * If we have more than 3 input segments the guest wants to actually
         * read data.
         */
        hdr.dxfer_direction = SG_DXFER_FROM_DEV;
        hdr.iovec_count = req->elem.in_num - 3;
        for (i = 0; i < hdr.iovec_count; i++)
            hdr.dxfer_len += req->elem.in_sg[i].iov_len;

        hdr.dxferp = req->elem.in_sg;
        size += hdr.dxfer_len;
    } else {
        /*
         * Some SCSI commands don't actually transfer any data.
         */
        hdr.dxfer_direction = SG_DXFER_NONE;
    }

    hdr.sbp = req->elem.in_sg[req->elem.in_num - 3].iov_base;
    hdr.mx_sb_len = req->elem.in_sg[req->elem.in_num - 3].iov_len;
    size += hdr.mx_sb_len;

    ret = bdrv_ioctl(req->dev->bs, SG_IO, &hdr);
    if (ret) {
        status = VIRTIO_BLK_S_UNSUPP;
        hdr.status = ret;
        hdr.resid = hdr.dxfer_len;
    } else if (hdr.status) {
        status = VIRTIO_BLK_S_IOERR;
    } else {
        status = VIRTIO_BLK_S_OK;
    }

    req->scsi->errors = hdr.status;
    req->scsi->residual = hdr.resid;
    req->scsi->sense_len = hdr.sb_len_wr;
    req->scsi->data_len = hdr.dxfer_len;

    virtio_blk_req_complete(req, status);
}
#else
static void virtio_blk_handle_scsi(VirtIOBlockReq *req)
{
    virtio_blk_req_complete(req, VIRTIO_BLK_S_UNSUPP);
}
#endif /* __linux__ */

static void do_multiwrite(BlockDriverState *bs, BlockRequest *blkreq,
    int num_writes)
{
    int i, ret;
    ret = bdrv_aio_multiwrite(bs, blkreq, num_writes);

    if (ret != 0) {
        for (i = 0; i < num_writes; i++) {
            if (blkreq[i].error) {
                virtio_blk_rw_complete(blkreq[i].opaque, -EIO);
            }
        }
    }
}

static void virtio_blk_handle_flush(BlockRequest *blkreq, int *num_writes,
    VirtIOBlockReq *req, BlockDriverState **old_bs)
{
    BlockDriverAIOCB *acb;

    /*
     * Make sure all outstanding writes are posted to the backing device.
     */
    if (*old_bs != NULL) {
        do_multiwrite(*old_bs, blkreq, *num_writes);
    }
    *num_writes = 0;
    *old_bs = req->dev->bs;

    acb = bdrv_aio_flush(req->dev->bs, virtio_blk_flush_complete, req);
    if (!acb) {
        virtio_blk_req_complete(req, VIRTIO_BLK_S_IOERR);
    }
}

static void virtio_blk_handle_write(BlockRequest *blkreq, int *num_writes,
    VirtIOBlockReq *req, BlockDriverState **old_bs)
{
    if (req->dev->bs != *old_bs || *num_writes == 32) {
        if (*old_bs != NULL) {
            do_multiwrite(*old_bs, blkreq, *num_writes);
        }
        *num_writes = 0;
        *old_bs = req->dev->bs;
    }

    blkreq[*num_writes].sector = req->out->sector;
    blkreq[*num_writes].nb_sectors = req->qiov.size / 512;
    blkreq[*num_writes].qiov = &req->qiov;
    blkreq[*num_writes].cb = virtio_blk_rw_complete;
    blkreq[*num_writes].opaque = req;
    blkreq[*num_writes].error = 0;

    (*num_writes)++;
}

static void virtio_blk_handle_read(VirtIOBlockReq *req)
{
    BlockDriverAIOCB *acb;

    acb = bdrv_aio_readv(req->dev->bs, req->out->sector, &req->qiov,
                         req->qiov.size / 512, virtio_blk_rw_complete, req);
    if (!acb) {
        virtio_blk_rw_complete(req, -EIO);
    }
}

typedef struct MultiReqBuffer {
    BlockRequest        blkreq[32];
    int                 num_writes;
    BlockDriverState    *old_bs;
} MultiReqBuffer;

static void virtio_blk_handle_request(VirtIOBlockReq *req,
    MultiReqBuffer *mrb)
{
    if (req->elem.out_num < 1 || req->elem.in_num < 1) {
        fprintf(stderr, "virtio-blk missing headers\n");
        exit(1);
    }

    if (req->elem.out_sg[0].iov_len < sizeof(*req->out) ||
        req->elem.in_sg[req->elem.in_num - 1].iov_len < sizeof(*req->in)) {
        fprintf(stderr, "virtio-blk header not in correct element\n");
        exit(1);
    }

    req->out = (void *)req->elem.out_sg[0].iov_base;
    req->in = (void *)req->elem.in_sg[req->elem.in_num - 1].iov_base;

    if (req->out->type & VIRTIO_BLK_T_FLUSH) {
        virtio_blk_handle_flush(mrb->blkreq, &mrb->num_writes,
            req, &mrb->old_bs);
    } else if (req->out->type & VIRTIO_BLK_T_SCSI_CMD) {
        virtio_blk_handle_scsi(req);
    } else if (req->out->type & VIRTIO_BLK_T_OUT) {
        qemu_iovec_init_external(&req->qiov, &req->elem.out_sg[1],
                                 req->elem.out_num - 1);
        virtio_blk_handle_write(mrb->blkreq, &mrb->num_writes,
            req, &mrb->old_bs);
    } else {
        qemu_iovec_init_external(&req->qiov, &req->elem.in_sg[0],
                                 req->elem.in_num - 1);
        virtio_blk_handle_read(req);
    }
}

static void virtio_blk_handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOBlock *s = to_virtio_blk(vdev);
    VirtIOBlockReq *req;
    MultiReqBuffer mrb = {
        .num_writes = 0,
        .old_bs = NULL,
    };

    while ((req = virtio_blk_get_request(s))) {
        virtio_blk_handle_request(req, &mrb);
    }

    if (mrb.num_writes > 0) {
        do_multiwrite(mrb.old_bs, mrb.blkreq, mrb.num_writes);
    }

    /*
     * FIXME: Want to check for completions before returning to guest mode,
     * so cached reads and writes are reported as quickly as possible. But
     * that should be done in the generic block layer.
     */
}

static void virtio_blk_dma_restart_bh(void *opaque)
{
    VirtIOBlock *s = opaque;
    VirtIOBlockReq *req = s->rq;
    MultiReqBuffer mrb = {
        .num_writes = 0,
        .old_bs = NULL,
    };

    qemu_bh_delete(s->bh);
    s->bh = NULL;

    s->rq = NULL;

    while (req) {
        virtio_blk_handle_request(req, &mrb);
        req = req->next;
    }

    if (mrb.num_writes > 0) {
        do_multiwrite(mrb.old_bs, mrb.blkreq, mrb.num_writes);
    }
}

static void virtio_blk_dma_restart_cb(void *opaque, int running, int reason)
{
    VirtIOBlock *s = opaque;

    if (!running)
        return;

    if (!s->bh) {
        s->bh = qemu_bh_new(virtio_blk_dma_restart_bh, s);
        qemu_bh_schedule(s->bh);
    }
}

static void virtio_blk_reset(VirtIODevice *vdev)
{
    /*
     * This should cancel pending requests, but can't do nicely until there
     * are per-device request lists.
     */
    qemu_aio_flush();
}

/* coalesce internal state, copy to pci i/o region 0
 */
static void virtio_blk_update_config(VirtIODevice *vdev, uint8_t *config)
{
    VirtIOBlock *s = to_virtio_blk(vdev);
    struct virtio_blk_config blkcfg;
    uint64_t capacity;
    int cylinders, heads, secs;

    bdrv_get_geometry(s->bs, &capacity);
    bdrv_get_geometry_hint(s->bs, &cylinders, &heads, &secs);
    memset(&blkcfg, 0, sizeof(blkcfg));
    stq_raw(&blkcfg.capacity, capacity);
    stl_raw(&blkcfg.seg_max, 128 - 2);
    stw_raw(&blkcfg.cylinders, cylinders);
    blkcfg.heads = heads;
    blkcfg.sectors = secs;
    blkcfg.size_max = 0;
    virtio_identify_template(&blkcfg);
    memcpy(&blkcfg.identify[VIRTIO_BLK_ID_SN], s->serial_str,
        VIRTIO_BLK_ID_SN_BYTES);
    memcpy(config, &blkcfg, s->config_size);
}

static uint32_t virtio_blk_get_features(VirtIODevice *vdev)
{
    VirtIOBlock *s = to_virtio_blk(vdev);
    uint32_t features = 0;

    features |= (1 << VIRTIO_BLK_F_SEG_MAX);
    features |= (1 << VIRTIO_BLK_F_GEOMETRY);

    if (bdrv_enable_write_cache(s->bs))
        features |= (1 << VIRTIO_BLK_F_WCACHE);
#ifdef __linux__
    features |= (1 << VIRTIO_BLK_F_SCSI);
#endif
    if (strcmp(s->serial_str, "0"))
        features |= 1 << VIRTIO_BLK_F_IDENTIFY;
    
    if (bdrv_is_read_only(s->bs))
        features |= 1 << VIRTIO_BLK_F_RO;

    return features;
}

static void virtio_blk_save(QEMUFile *f, void *opaque)
{
    VirtIOBlock *s = opaque;
    VirtIOBlockReq *req = s->rq;

    virtio_save(&s->vdev, f);
    
    while (req) {
        qemu_put_sbyte(f, 1);
        qemu_put_buffer(f, (unsigned char*)&req->elem, sizeof(req->elem));
        req = req->next;
    }
    qemu_put_sbyte(f, 0);
}

static int virtio_blk_load(QEMUFile *f, void *opaque, int version_id)
{
    VirtIOBlock *s = opaque;

    if (version_id != 2)
        return -EINVAL;

    virtio_load(&s->vdev, f);
    while (qemu_get_sbyte(f)) {
        VirtIOBlockReq *req = virtio_blk_alloc_request(s);
        qemu_get_buffer(f, (unsigned char*)&req->elem, sizeof(req->elem));
        req->next = s->rq;
        s->rq = req->next;
    }

    return 0;
}

VirtIODevice *virtio_blk_init(DeviceState *dev, DriveInfo *dinfo)
{
    VirtIOBlock *s;
    int cylinders, heads, secs;
    static int virtio_blk_id;
    char *ps = (char *)drive_get_serial(dinfo->bdrv);
    size_t size = strlen(ps) ? sizeof(struct virtio_blk_config) :
	    offsetof(struct virtio_blk_config, _blk_size);

    s = (VirtIOBlock *)virtio_common_init("virtio-blk", VIRTIO_ID_BLOCK,
                                          size,
                                          sizeof(VirtIOBlock));

    s->config_size = size;
    s->vdev.get_config = virtio_blk_update_config;
    s->vdev.get_features = virtio_blk_get_features;
    s->vdev.reset = virtio_blk_reset;
    s->bs = dinfo->bdrv;
    s->rq = NULL;
    if (strlen(ps))
        strncpy(s->serial_str, ps, sizeof(s->serial_str));
    else
        snprintf(s->serial_str, sizeof(s->serial_str), "0");
    bdrv_guess_geometry(s->bs, &cylinders, &heads, &secs);
    bdrv_set_geometry_hint(s->bs, cylinders, heads, secs);

    s->vq = virtio_add_queue(&s->vdev, 128, virtio_blk_handle_output);

    qemu_add_vm_change_state_handler(virtio_blk_dma_restart_cb, s);
    register_savevm("virtio-blk", virtio_blk_id++, 2,
                    virtio_blk_save, virtio_blk_load, s);

    return &s->vdev;
}
