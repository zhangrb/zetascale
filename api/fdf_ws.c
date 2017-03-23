//----------------------------------------------------------------------------
// ZetaScale
// Copyright (c) 2016, SanDisk Corp. and/or all its affiliates.
//
// This program is free software; you can redistribute it and/or modify it under
// the terms of the GNU Lesser General Public License version 2.1 as published by the Free
// Software Foundation;
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License v2.1 for more details.
//
// A copy of the GNU Lesser General Public License v2.1 is provided with this package and
// can also be found at: http://opensource.org/licenses/LGPL-2.1
// You should have received a copy of the GNU Lesser General Public License along with
// this program; if not, write to the Free Software Foundation, Inc., 59 Temple
// Place, Suite 330, Boston, MA 02111-1307 USA.
//----------------------------------------------------------------------------

/**********************************************************************
 *
 *  fdf_ws.c   8/30/16   Brian O'Krafka   
 *
 *  Code to initialize Write serializer module for ZetaScale b-tree.
 *
 * (c) Copyright 2016  Western Digital Corporation
 *
 *  Notes:
 *
 **********************************************************************/

#include "btree/btree_raw_internal.h"
#include "ws/ws.h"
#include "api/zs.h"
#include "fth/fthLock.h"
#include "ssd/fifo/mcd_osd.h"
#include "ssd/fifo/hash.h"
#include "api/fdf_ws_internal.h"
#include "protocol/action/action_internal_ctxt.h"
#include "protocol/action/action_new.h"
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <aio.h>
#include <fcntl.h>

#define LOG_ID PLAT_LOG_ID_INITIAL
#define LOG_CAT PLAT_LOG_CAT_SDF_NAMING
#define LOG_TRACE PLAT_LOG_LEVEL_TRACE
#define LOG_DBG PLAT_LOG_LEVEL_DEBUG
#define LOG_DIAG PLAT_LOG_LEVEL_DIAGNOSTIC
#define LOG_INFO PLAT_LOG_LEVEL_INFO
#define LOG_ERR PLAT_LOG_LEVEL_ERROR

// xxxzzz imported from ssd/fifo/mcd_osd.c
extern struct ws_state *pWriteSerializer;
extern uint64_t Mcd_osd_blk_size;

// xxxzzz imported from api/enumerate.c
ZS_status_t cguid_to_shard(SDF_action_init_t *pai, ZS_cguid_t cguid, shard_t **shard_ptr, int delete_ok);

// xxxzzz imported from fdf_wrapper.c
extern btree_t *bt_get_btree_from_cguid(ZS_cguid_t cguid, int *index, ZS_status_t *error, bool write);
extern void bt_rel_entry(int i, bool write);

// xxxzzz imported from btree/fdf_wrapper.c
extern uint32_t get_btree_node_size();

static __thread dataptrs_t              dataptrs         = {0};
static __thread dataptrs_t              dataptrs2        = {0};
static __thread struct ZS_thread_state *gc_thd_state     = NULL;
static __thread struct bv              *accounted        = NULL;
static __thread mcd_osd_shard_t        *shard_vdc        = NULL;
static __thread struct aiocb          **stripe_iocb      = NULL;
static int                              do_new_gc        = 0;
static int                              fd_serialize     = 0;
static uint32_t                         btree_node_size  = 0;
static uint32_t                         sectors_per_node = 0;
static uint32_t                         blks_per_node    = 0;

static int ws_client_ops_cb(struct ZS_thread_state *pzs, void *state, uint64_t *n_ops);
static int ws_gc_cb(void *state, void *pbuf, uint64_t *p_out, uint32_t stripe_bytes, uint32_t sector_bytes, uint64_t stripe_addr, uint64_t *compacted_bytes, uint64_t device_offset);
static int ws_gc_new_phase1_cb(void *state, void *pbuf, uint64_t *p_out, uint32_t stripe_bytes, uint32_t sector_bytes, uint64_t stripe_addr, uint64_t *compacted_bytes, uint64_t device_offset);
static int ws_gc_new_phase2_cb(void *state, void *pbuf, uint64_t *p_out, uint32_t stripe_bytes, uint32_t sector_bytes, uint64_t stripe_addr, uint64_t *compacted_bytes, uint64_t device_offset);
static int btree_traversal_cb(void *pdata, void *pzst, node_check_cb_t *check_cb);
static int ws_per_thread_state_cb(struct ZS_state *pzs, struct ZS_thread_state **pzst);
static int ws_set_metadata_cb(struct ZS_thread_state *pzs, void *state, void *key, uint32_t key_size, void *data, uint32_t data_size);
static int ws_get_metadata_cb(struct ZS_thread_state *pzs, void *state, void *key, uint32_t key_size, void *data, uint32_t max_size, uint32_t *md_size);
static int decode_btree_node_data_in_leaves(struct ZS_thread_state *pzs, bv_t *accounted, uint32_t sector_bytes, uint64_t stripe_addr, void *pbuf, void *p_from, void **pp_to, void *pmax_from);
static void *advance_p(bv_t *accounted, void *pbuf, void *p, void *pmax, uint32_t sector_bytes, uint32_t max_sectors);
static void traverse_node(struct ZS_thread_state *pzs, btree_raw_t *bt, btree_raw_node_t *n, struct shard *shard_in, node_check_cb_t *check_cb, void *pdata);

/*   bit vector utility
 */

static struct bv *bv_init(uint32_t n);
static void bv_clear(struct bv *bv);
static void bv_set(struct bv *bv, uint32_t n);
static void bv_set_bits(struct bv *bv, uint32_t n, uint32_t cnt);
// static void bv_unset(struct bv *bv, uint32_t n);
static int bv_bit(struct bv *bv, uint32_t n);

int init_write_serializer_subsystem(struct ZS_thread_state *pzst, struct ZS_state *zs_state, ZS_cguid_t md_cguid, const char *sdev, const char *sbatchdev, int mb, int format_flag, uint32_t sectors_per_node, uint32_t sector_size, int use_new_gc)
{
    ws_config_t       cfg;
    struct ws_state  *pws;
    char             *v;
    gc_ws_state_t    *pcb;

    WSLoadDefaultConfig(&cfg);

    pcb = (gc_ws_state_t *) malloc(sizeof(gc_ws_state_t));
    if (!pcb) {
        plat_log_msg(160315, LOG_CAT, LOG_ERR, "Could not allocate callback state");
        return(1);
    }
    pcb->pzs            = zs_state;
    pcb->md_cguid       = md_cguid;

    cfg.n_devices = 1;
    cfg.fd_devices[0] = open(sdev, O_RDWR|O_DIRECT);
    if (cfg.fd_devices[0] == -1) {
        plat_log_msg(160316, LOG_CAT, LOG_ERR, "Could not open write serialization device '%s' (errno = '%s')", sdev, strerror(errno));
    }
    fd_serialize = cfg.fd_devices[0];
    if (sbatchdev != NULL) {
	cfg.batch_fd = open(sbatchdev, O_RDWR|O_DIRECT);
	if (cfg.batch_fd == -1) {
	    plat_log_msg(160317, LOG_CAT, LOG_ERR, "Could not open write serialization batch device '%s' (errno = '%s')", sbatchdev, strerror(errno));
	}
    }
    cfg.sector_size         = sector_size;
    cfg.sectors_per_node    = sectors_per_node;
    cfg.device_mbytes       = mb;
    do_new_gc               = use_new_gc;
    cfg.gc_phase1_cb        = ws_gc_new_phase1_cb;
    cfg.gc_phase2_cb        = ws_gc_new_phase2_cb;
    cfg.btree_traversal_cb  = btree_traversal_cb;
    cfg.per_thread_cb       = ws_per_thread_state_cb;
    cfg.get_md_cb           = ws_get_metadata_cb;
    cfg.set_md_cb           = ws_set_metadata_cb;
    cfg.get_client_ops_cb   = ws_client_ops_cb;
    cfg.cb_state            = pcb;

    pws = WSStart(zs_state, pzst, &cfg, format_flag);
    if (pws == NULL) {
        return(1);
    }
    WSDumpConfig(stderr, pws);
    pcb->ps = pws;

    pWriteSerializer = pws; // this is for mcd_osd.c

    return(0);
}

static char **alloc_keybufs(gc_ws_state_t *pgc, int n, uint32_t max_key_size, dataptrs_t *dp)
{
    int    i;
    char **keybufs;
    char  *s;

    dp->n           = n;
    dp->n_in_stripe = 0;

    dp->copyflags = (int *) malloc(n*sizeof(int));
    if (dp->copyflags == NULL) {
        return(NULL);
    }
    dp->keyptrs = (uint64_t *) malloc(n*sizeof(uint64_t));
    if (dp->keyptrs == NULL) {
        free(dp->copyflags);
        return(NULL);
    }
    dp->keylens = (uint32_t *) malloc(n*sizeof(uint32_t));
    if (dp->keylens == NULL) {
        free(dp->copyflags);
        free(dp->keyptrs);
        return(NULL);
    }
    dp->keydatasizes = (uint32_t *) malloc(n*sizeof(uint32_t));
    if (dp->keydatasizes == NULL) {
        free(dp->copyflags);
        free(dp->keyptrs);
        free(dp->keylens);
        return(NULL);
    }
    dp->keybufs = (char **) malloc(n*sizeof(char *));
    if (dp->keybufs == NULL) {
        free(dp->copyflags);
        free(dp->keyptrs);
        free(dp->keylens);
        free(dp->keydatasizes);
        return(NULL);
    }
    s = (char *) malloc(n*max_key_size*sizeof(char));
    if (s == NULL) {
        free(dp->copyflags);
        free(dp->keyptrs);
        free(dp->keylens);
        free(dp->keydatasizes);
        free(dp->keybufs);
        return(NULL);
    }

    for (i=0; i<n; i++) {
        dp->keybufs[i] = s;
	s += max_key_size;
    }
    return(dp->keybufs);
}

static int gc_init(void *state, uint32_t stripe_bytes, uint32_t sector_bytes)
{
    gc_ws_state_t             *pgc = (gc_ws_state_t *) state;
    uint32_t                   max_key_size;
    uint32_t                   max_key_bufs;
    ZS_status_t                status;
    struct aiocb              *tmpiocb;
    int                        i, n_io;

    if (dataptrs.n == 0) {
        btree_node_size = get_btree_node_size();
	if (btree_node_size % sector_bytes) {
	    btree_node_size = (btree_node_size/sector_bytes + 1)*sector_bytes;
	}
	plat_assert((btree_node_size % Mcd_osd_blk_size) == 0);
	plat_assert((btree_node_size % sector_bytes) == 0);

	blks_per_node    = btree_node_size/Mcd_osd_blk_size;
	sectors_per_node = btree_node_size/sector_bytes;

        max_key_size    = btree_node_size/2;  // xxxzzz check this
	max_key_bufs    = btree_node_size/16; // xxxzzz check this

        n_io = stripe_bytes/btree_node_size;
        tmpiocb = (struct aiocb *) malloc(n_io*sizeof(struct aiocb));
	if (tmpiocb == NULL) {
	    plat_log_msg(160318, LOG_CAT, LOG_ERR, "Could not allocate 'tmpiocb' array in gc_init");
	    return(1); // error!
	}
        stripe_iocb = (struct aiocb **) malloc(n_io*sizeof(struct aiocb *));
	if (stripe_iocb == NULL) {
	    plat_log_msg(160319, LOG_CAT, LOG_ERR, "Could not allocate 'stripe_iocb' array in gc_init");
	    return(1); // error!
	}
        for (i=0; i<n_io; i++) {
	    stripe_iocb[i] = &(tmpiocb[i]);
	}

        accounted = bv_init(stripe_bytes/sector_bytes);
	if (accounted == NULL) {
	    plat_log_msg(160320, LOG_CAT, LOG_ERR, "Could not allocate 'accounted' bit vector in gc_init");
	    return(1); // error!
	}
        if (alloc_keybufs(pgc, max_key_bufs, max_key_size, &dataptrs) == NULL) {
	    plat_log_msg(160321, LOG_CAT, LOG_ERR, "Could not allocate dataptrs in gc_init");
	    return(1); // error!
	}
        if (alloc_keybufs(pgc, max_key_bufs, max_key_size, &dataptrs2) == NULL) {
	    plat_log_msg(160322, LOG_CAT, LOG_ERR, "Could not allocate dataptrs2 in gc_init");
	    return(1); // error!
	}

	// Initialize per-thread ZS state for this garbage collector thread.
	if ((status = ZSInitPerThreadState(pgc->pzs, &gc_thd_state)) != ZS_SUCCESS) {
	    plat_log_msg(160323, LOG_CAT, LOG_ERR, "ZSInitPerThreadState failed with error %s\n", ZSStrError(status));
	    return(1);
	}
	if ((status = cguid_to_shard((SDF_action_init_t *) gc_thd_state, VDC_CGUID, (struct shard **) &shard_vdc, 0)) != ZS_SUCCESS) {
	    plat_log_msg(160324, LOG_CAT, LOG_ERR, "cguid_to_shard failed with error %s\n", ZSStrError(status));
	    return(1);
	}
	if (shard_vdc->hash_handle->key_cache == NULL) {
	    plat_log_msg(160325, LOG_CAT, LOG_ERR, "FDC shard has no key cache!\n");
	    return(1);
	}
    }
    return(0);
}

static int ws_gc_cb(void *state, void *pbuf, uint64_t *p_out, uint32_t stripe_bytes, uint32_t sector_bytes, uint64_t stripe_addr, uint64_t *compacted_bytes, uint64_t device_offset)
{
    gc_ws_state_t             *pgc = (gc_ws_state_t *) state;
    uint32_t                   sectors_per_stripe;
    uint32_t                   nodesize;
    void                      *p_from, *pma_from, *p_to, *p_from_new;
    void                      *pmax_from;
    int                        rc;
    ZS_status_t                status;
    uint32_t                   max_key_size;
    uint32_t                   max_key_bufs;

    /*******************************************************
     * Algorithm:
     *
     *    - First node-sized region must be a b-tree node.
     *    - Lock hashtable for node id.
     *
     *  Cases:
     *
     *  - If leaf node and data in b-tree leaves:
     *
     *      - node id in use and still points to this node:
     *          - copy node
     *      - otherwise:
     *          - do not copy node
     *
     *  - If leaf node and data outside of b-tree:
     *
     *      - node id in use and still points to this node:
     *          - find data pointers landing in stripe
     *          - copy data that is still valid
     *          - copy node
     *
     *      - otherwise:
     *          - find data pointers landing in stripe
     *          - use keys to query b-tree to see which pointers are
     *            still valid 
     *              - do a range query, since keys should be clustered 
     *                in a small number of leaf nodes)
     *              - in most cases, there should only be one key to look up!
     *          - copy data that is still valid
     *          - do not copy node
     *
     *      - xxxzzz: what about extent handling?
     *
     *    - If non-leaf node (due to b-tree splits/joins):
     *        - If node is still current, copy. 
     *        - Otherwise: skip.
     *
     *    - For copied data, update physical pointers in hashtable 
     *      and copied leaf-nodes.
     *	  - Mark bit vector for accounted blocks.
     *    - Unlock hashtable for node id.
     *	  - Go to next unaccounted region and decode the
     *	    next b-tree node and repeat.
     *
     *   Must identify non-leaf b-tree nodes caused by b-tree restructuring.
     *   These also require changes logical-to-physical map in ZS hashtable.
     *
     *******************************************************/

    bv_clear(accounted);  //  bit vector is at sector granularity

    sectors_per_stripe = stripe_bytes/sector_bytes;
    p_from             = pbuf;
    pmax_from          = p_from + stripe_bytes;
    p_to               = pbuf;

    while (1) {
        if ( 1 /* data_in_leaves */) { // xxxzzz hardcoded for now!
	    rc = decode_btree_node_data_in_leaves(gc_thd_state, accounted, sector_bytes, stripe_addr, pbuf, p_from, &p_to, pmax_from);
	} else {
	    // xxxzzz rc = decode_btree_node_data_not_in_leaves(pgc->pzs, accounted, sector_bytes, stripe_addr, pbuf, p_from, &p_to, pmax_from)
	}
	if (rc != 0) {
	    //  oh-oh!
	    return(1); // panic!
	}
	p_from_new = advance_p(accounted, pbuf, p_from, pmax_from, sector_bytes, sectors_per_stripe);
	if (p_from_new >= pmax_from) {
	    break;
	}
	p_from = p_from_new;
    }
    *p_out = (p_to - pbuf);
    *compacted_bytes = p_to - pbuf;
    return(0);
}

static int do_stripe_read(int fd, uint32_t stripe_bytes, uint64_t addr, void *buf)
{
    ssize_t    sze;

    sze = pread(fd, buf, stripe_bytes, addr);
    // xxxzzz improve error handling
    // xxxzzz what if fewer bytes were read?
    if (sze != stripe_bytes) {
        return(1);
    }

    return(0);
}

static int ws_gc_new_phase1_cb(void *state, void *pbuf, uint64_t *p_out, uint32_t stripe_bytes, uint32_t sector_bytes, uint64_t stripe_addr, uint64_t *compacted_bytes, uint64_t device_offset)
{
    uint32_t       i;
    uint32_t       nodes_per_stripe;
    uint32_t       valid_bytes;
    uint64_t       blkaddress;
    uint64_t       tmpkey;
    ssize_t        ssize;
    int            n_io;
    int            rc;
    int            flags;
    struct aiocb  *pio;

    rc = gc_init(state, stripe_bytes, sector_bytes);
    if (rc != 0) {
        return(rc);
    }

    /*******************************************************
     *
     * Phase 1: read in stripe
     *     Old GC: read entire stripe in one I/O
     *     New GC: read only blocks that are still valid
     *             (much more efficient when stripes are mostly empty)
     *
     *******************************************************/

    if (!do_new_gc) {
	//  old GC: read entire stripe, all the time
	if (do_stripe_read(fd_serialize, stripe_bytes, stripe_addr + device_offset, pbuf)) {
	    return(1);
	}
	return(0);
    }

    //   zero out the buffer: zeroed nodes will be skipped for GC
    (void) memset(pbuf, 0, stripe_bytes);

    //  read in the still-valid nodes
    n_io             = 0;
    valid_bytes      = 0;
    flags            = FLASH_GET_SERIALIZED;
    nodes_per_stripe = stripe_bytes/btree_node_size;
    for (i=0; i<nodes_per_stripe; i++) {

        blkaddress = (stripe_addr + i*btree_node_size)/Mcd_osd_blk_size;
	tmpkey = keycache_get(shard_vdc->hash_handle, blkaddress, flags);

	if (tmpkey != 0) {
	    //  this physical block is still in use
	    pio = stripe_iocb[n_io];
	    pio->aio_lio_opcode = LIO_READ;
	    pio->aio_fildes     = fd_serialize;
	    pio->aio_offset     = (blkaddress*Mcd_osd_blk_size + device_offset);
	    pio->aio_nbytes     = (size_t) btree_node_size;
	    pio->aio_buf        = pbuf + i*btree_node_size;
	    pio->aio_reqprio    = 0;
	    pio->aio_sigevent.sigev_notify = SIGEV_NONE;
	    n_io++;

	    valid_bytes += btree_node_size;
	}
    }

    if (valid_bytes > 0) {
        //  read in valid blocks
	if (n_io > (nodes_per_stripe/2)) {
	     // just read in the whole stripe
             if (do_stripe_read(fd_serialize, stripe_bytes, stripe_addr + device_offset, pbuf) != 0) {
		return(1);
	     }
	} else {
            
            rc = lio_listio(LIO_WAIT, stripe_iocb, n_io, NULL);
	    if (rc != 0) {
		plat_log_msg(160326, LOG_CAT, LOG_ERR, "lio_listio failed with error %d ('%s')", errno, strerror(errno));
		return(1);
	    }
	}
    }

    *compacted_bytes = valid_bytes;
    return(0);
}

static int ws_gc_new_phase2_cb(void *state, void *pbuf, uint64_t *p_out, uint32_t stripe_bytes, uint32_t sector_bytes, uint64_t stripe_addr, uint64_t *compacted_bytes, uint64_t device_offset)
{
    int rc;

    rc = ws_gc_cb(state, pbuf, p_out, stripe_bytes, sector_bytes, stripe_addr, compacted_bytes, device_offset);
    return(rc);
}

static int ht_lock_and_fetch_data_ptr(struct ZS_thread_state *pzs, ZS_cguid_t cguid, mcd_osd_shard_t *shard, uint64_t logical_id, uint64_t *ht_addr, ht_handle_t *ph)
{
    fthLock_t           *lk;
    fthWaitEl_t         *lk_wait;
    hash_entry_t        *hash_entry;
    SDF_action_init_t   *pac = (SDF_action_init_t *) pzs;
    uint64_t             syndrome;
    int                  flags = FLASH_GET_SERIALIZED;

    syndrome = hashck((unsigned char *) &logical_id, 8, 0, cguid);

    lk       = hash_table_find_lock(shard->hash_handle, syndrome, SYN);
    lk_wait  = fthLock(lk, 1, NULL );

    hash_entry = hash_table_get( pac->paio_ctxt, shard->hash_handle, (char *) &logical_id, 8, cguid, flags);
    if (hash_entry == NULL) {
	fthUnlock(lk_wait);
        return(1);
    }

    *ht_addr = hash_entry->blkaddress;

    ph->lk_wait    = lk_wait;
    ph->hash_entry = hash_entry;
    ph->syndrome   = syndrome;
    ph->cguid      = cguid;
    ph->shard      = shard;
    ph->nodesize   = btree_node_size;
    ph->logical_id = logical_id;
    ph->pzs        = pzs;

    return(0);
}

static void ht_change_ptr_and_unlock(ht_handle_t *ph, uint64_t new_addr)
{
    mcd_logrec_object_t   log_rec;
    uint64_t              old_addr;
    uint64_t              target_seqno;
    uint64_t              seqno;
    int                   flags;

    seqno = atomic_inc_get(ph->shard->sequence);
    target_seqno = 0;  //  For b-tree with key_cache enabled, 
                       //  otherwise old value read from flash metadata.

    /*
     * update the hash table entry
     */

    // old_addr                   = ph->hash_entry->blkaddress;
    ph->hash_entry->blkaddress = new_addr;

    flags = FLASH_PUT_SERIALIZED;
    //  we should always use a key_cache when using the write serializer
      // delete old key mapping
    keycache_set(ph->shard->hash_handle, ph->hash_entry->blkaddress, 0, flags);
      // create new key mapping
    keycache_set(ph->shard->hash_handle, new_addr, ph->logical_id, flags);

    // create a log record for crash safety

    log_rec.syndrome   = ph->hash_entry->hesyndrome;
    log_rec.deleted    = ph->hash_entry->deleted;
    log_rec.reserved   = 0;
    log_rec.blocks     = ph->hash_entry->blocks;
    if (!ph->shard->hash_handle->addr_table) {
	log_rec.rbucket = (ph->syndrome % ph->shard->hash_handle->hash_size) / Mcd_osd_bucket_size;
    } else {
	log_rec.rbucket = (ph->syndrome % ph->shard->hash_handle->hash_size);
    }
    log_rec.mlo_blk_offset = ph->hash_entry->blkaddress;
    log_rec.cntr_id        = ph->cguid;
    log_rec.seqno          = seqno;
    log_rec.target_seqno   = target_seqno;
    log_rec.raw            = FALSE;
    log_rec.mlo_dl         = SDF_FULL_DURABILITY;

    /* Overwrite case in store mode:
     *    Must set this to zero to suppress slab deallocation in log_write_post_process!
     *    Old code: log_rec.mlo_old_offset = ~(old_addr) & 0x0000ffffffffffffull;
     */
    log_rec.mlo_old_offset = 0;

    log_write_trx(ph->shard, &log_rec, ph->syndrome, ph->hash_entry);

    if (ph->shard->hash_handle->addr_table) {
	ph->shard->hash_handle->addr_table[new_addr] = (ph->syndrome % ph->shard->hash_handle->hash_size);
    }

    fthUnlock(ph->lk_wait);
}

static void ht_unlock(ht_handle_t *ph)
{
    fthUnlock(ph->lk_wait);
}

static int decode_btree_node_data_in_leaves(struct ZS_thread_state *pzs, bv_t *accounted, uint32_t sector_bytes, uint64_t stripe_addr, void *pbuf, void *p_from, void **pp_to, void *pmax_from)
{
    uint64_t                   node_addr, ht_addr, new_addr;
    btree_raw_node_t          *n;
    void                      *p_to;
    ht_handle_t                handle;
    ZS_status_t                ret;
    ZS_cguid_t                 cguid;
    mcd_osd_meta_t            *osd_meta;
    char                      *p;

    p = p_from;
    osd_meta = (mcd_osd_meta_t *) p_from;

    if (do_new_gc) {
	if (osd_meta->magic == 0) {
	    //  skip this block
	    //  account for the space for the node we just decoded
	    bv_set_bits(accounted, (p_from - pbuf)/sector_bytes, sectors_per_node);
	    return(0);
	}
    }

    p += sizeof(mcd_osd_meta_t);
    p += osd_meta->key_len;

    n = (btree_raw_node_t *) p;
    p_to = *pp_to;

    cguid = osd_meta->cguid;

    node_addr = (stripe_addr + (p_from - pbuf))/Mcd_osd_blk_size;

    if (ht_lock_and_fetch_data_ptr(pzs, cguid, shard_vdc, n->logical_id, &ht_addr, &handle) == 0) {
        if (node_addr == ht_addr) {
            plat_assert(blks_per_node == handle.hash_entry->blocks);
	    memcpy(p_to, p_from, blks_per_node*Mcd_osd_blk_size);
	    new_addr = (stripe_addr + (p_to - pbuf))/Mcd_osd_blk_size;
	    ht_change_ptr_and_unlock(&handle, new_addr);
            //  only advance p_to if we copy something
	    p_to += sector_bytes*sectors_per_node;
	} else {
	    ht_unlock(&handle);
	}
    }
    // account for the space for the node we just decoded
    bv_set_bits(accounted, (p_from - pbuf)/sector_bytes, sectors_per_node);

    *pp_to   = p_to;
    return(0);
}

static void *advance_p(bv_t *accounted, void *pbuf, void *p, void *pmax, uint32_t sector_bytes, uint32_t max_sectors)
{
    uint64_t   i;

    for (i=(p-pbuf)/sector_bytes; i<max_sectors; i++) {
        if (!bv_bit(accounted, i)) {
	    break;
	}
	p += sector_bytes;
    }
    return(p);
}

static void dump_stripe(FILE *f, void *pbuf, uint32_t nodebytes, uint32_t stripebytes) __attribute__((unused));

static void dump_stripe(FILE *f, void *pbuf, uint32_t nodebytes, uint32_t stripebytes)
{
    int                        n_node;
    int                        nodes_per_stripe;
    btree_raw_node_t          *n;
    mcd_osd_meta_t            *osd_meta;
    char                      *p;

    nodes_per_stripe = stripebytes/nodebytes;
    
    for (n_node=0; n_node<nodes_per_stripe; n_node++) {
        p = pbuf + n_node*nodebytes;
	osd_meta = (mcd_osd_meta_t *) p;
	p += sizeof(mcd_osd_meta_t);
	p += osd_meta->key_len;
	n = (btree_raw_node_t *) p;
        
	fprintf(f, "Node %d: logical_id=%"PRIu64", flags=%d, cguid=%"PRIu64", seqno=%"PRIu64", next=%"PRIu64", rightmost=%"PRIu64"\n", n_node, n->logical_id, n->flags, osd_meta->cguid, osd_meta->seqno, n->next, n->rightmost);
    }
}

//=============================================
//   Metadata Get/Set Callbacks
//=============================================

//  Callback for allocating per-thread ZS state
  /*  returns:
   *    WS_OK
   *    WS_ERROR
   */
static int ws_per_thread_state_cb(struct ZS_state *pzs, struct ZS_thread_state **pzst)
{
    ZS_status_t       status;

    //Initialize per-thread ZS state for this thread.
    if ((status = ZSInitPerThreadState(pzs, pzst)) != ZS_SUCCESS) {
	plat_log_msg(160327, LOG_CAT, LOG_ERR, "In ws_per_thread_state_cb, ZSInitPerThreadState failed with error %s\n", ZSStrError(status));
	return(WS_ERROR);
    }
    return(WS_OK);
}

//  Callback for setting metadata (transactionally!)
  /*  returns:
   *    WS_OK
   *    WS_ERROR
   */
static int ws_set_metadata_cb(struct ZS_thread_state *pzs, void *state, void *key, uint32_t key_size, void *data, uint32_t data_size)
{
    ZS_status_t       status;
    gc_ws_state_t    *pgc = (gc_ws_state_t *) state;

    status = ZSWriteObject(pzs, pgc->md_cguid, key, key_size, data, data_size, 0); // xxxzzz check flags!
    if (status == ZS_SUCCESS) {
        return(WS_OK);
    } else {
	plat_log_msg(160328, LOG_CAT, LOG_ERR, "In ws_set_metadata_cb, ZSWriteObject failed with error %s\n", ZSStrError(status));
        return(WS_ERROR);
    }
}

//  Callback for getting metadata
  /*  returns:
   *    WS_OK
   *    WS_ERROR
   */
static int ws_get_metadata_cb(struct ZS_thread_state *pzs, void *state, void *key, uint32_t key_size, void *data, uint32_t max_size, uint32_t *md_size)
{
    ZS_status_t       status;
    gc_ws_state_t    *pgc = (gc_ws_state_t *) state;
    char             *data_zs;
    uint64_t          datalen_zs;

    status = ZSReadObject(pzs, pgc->md_cguid, key, key_size, &data_zs, &datalen_zs);
    if (status != ZS_SUCCESS) {
	plat_log_msg(160329, LOG_CAT, LOG_ERR, "In ws_get_metadata_cb, ZSWriteObject failed with error %s\n", ZSStrError(status));
        return(WS_ERROR);
    }
    if (max_size < datalen_zs) {
	plat_log_msg(160330, LOG_CAT, LOG_ERR, "In ws_get_metadata_cb, ZSWriteObject failed with max_size (%d) less than datalen_zs (%"PRIu64")\n", max_size, datalen_zs);
        return(WS_ERROR);
    }
    (void) memcpy(data, (void *) data_zs, datalen_zs);
    *md_size = datalen_zs;

    return(WS_OK);
}

//  Callback for getting number of client ops
  /*  returns:
   *    WS_OK
   *    WS_ERROR
   */
static int ws_client_ops_cb(struct ZS_thread_state *pzs, void *state, uint64_t *n_ops)
{
    ZS_stats_t    stats;
    ZS_status_t   status;
    uint64_t      ops;
    int           i;
    ZS_cguid_t    cguids[64*1024];
    uint32_t      n_cguids;

    status = ZSGetContainers(pzs, cguids, &n_cguids);
    if (status != ZS_SUCCESS) {
	plat_log_msg(160331, LOG_CAT, LOG_ERR, "ws_client_ops_cb failed in call to ZSGetContainers");
	*n_ops = 0;
	return(WS_ERROR);
    }

    ops = 0;
    for (i=0; i<n_cguids; i++) {
	status = ZSGetContainerStats(pzs, cguids[i], &stats);
	if (status != ZS_SUCCESS) {
	    plat_log_msg(160332, LOG_CAT, LOG_ERR, "ws_client_ops_cb failed in call to ZSGetContainerStats");
	    *n_ops = 0;
	    return(WS_ERROR);
	}

	ops += stats.n_accesses[ZS_ACCESS_TYPES_READ]
	    +  stats.n_accesses[ZS_ACCESS_TYPES_WRITE]
	    +  stats.n_accesses[ZS_ACCESS_TYPES_DELETE];

	#ifdef notdef
	ops += ( stats.btree_stats[ZS_BTREE_GET]
	       + stats.btree_stats[ZS_BTREE_CREATE]
	       + stats.btree_stats[ZS_BTREE_UPDATE]
	       + stats.btree_stats[ZS_BTREE_SET]
	       + stats.btree_stats[ZS_BTREE_DELETE]);
	#endif
    }

    *n_ops = ops;
    return(WS_OK);
}


//=============================================
//   B-tree Traversal Callback (for WSCheck)
//=============================================

static int btree_traversal_cb(void *pdata, void *pzst, node_check_cb_t *check_cb)
{
    btree_raw_t               *bt;
    struct ZS_thread_state    *pzs = (struct ZS_thread_state *) pzst;
    btree_status_t             ret = BTREE_SUCCESS;
    btree_raw_mem_node_t      *n;
    uint32_t                   n_cguids;
    ZS_cguid_t                 cguids[64*1024];
    SDF_cache_ctnr_metadata_t *meta;
    ZS_status_t                status;
    struct btree              *bt_notraw;
    int                        failed;
    int                        i;
    int                        index;

    failed = 0;
    status = ZSGetContainers(pzs, cguids, &n_cguids);
    if (status != ZS_SUCCESS) {
	plat_log_msg(160333, LOG_CAT, LOG_ERR, "ZSGetContainers failed with status=%d (%s)!", status, ZSStrError(status));
	return(1);
    }
    for (i=0; i<n_cguids; i++) {

	bt_notraw = bt_get_btree_from_cguid(cguids[i], &index, &status, true); // xxxzzz is true correct?
	if (bt_notraw == NULL) {
	    plat_log_msg(160334, LOG_CAT, LOG_ERR, "bt_get_btree_from_cguid failed with error '%s'", ZSStrError(status));
	    return (1);
	}
	plat_assert(bt_notraw->n_partitions == 1);
	bt = bt_notraw->partitions[0];

	n = get_existing_node(&ret, bt, bt->rootid, 0, LOCKTYPE_NOLOCK); 
	meta = get_container_metadata((SDF_action_init_t *) pzs, cguids[i]);
	if (meta == NULL) {
	    plat_log_msg(160335, LOG_CAT, LOG_ERR, "get_container_metadata failed");
	    failed  = 1;
	} else {
	    traverse_node(pzs, bt, n->pnode, meta->pshard, check_cb, pdata);
	}
	deref_l1cache_node(bt, n);
	if (failed) {
	    return(1);
	}
    }
    return(0);
}

static void traverse_node(struct ZS_thread_state *pzs, btree_raw_t *bt, btree_raw_node_t *n, struct shard *shard_in, node_check_cb_t *check_cb, void *pdata)
{
    int                     i;
    char                   *sflags;
    int                     nkey_bytes;
    node_fkey_t            *pfk;
    node_vkey_t            *pvk;
    node_vlkey_t           *pvlk;
    key_stuff_t             ks;
    btree_raw_mem_node_t   *n_child = NULL;
    int                     is_leaf;
    ht_handle_t             ht_handle;
    uint64_t                ht_addr;
    uint64_t                logical_id;

    if (ht_lock_and_fetch_data_ptr(pzs, bt->cguid, (mcd_osd_shard_t *) shard_in, n->logical_id, &ht_addr, &ht_handle) == 0) {
	(*check_cb)(pdata, ht_addr);
    } else {
	plat_log_msg(160336, LOG_CAT, LOG_ERR, "traverse_node: hash table entry not found for an existing B-tree node id=%"PRIu64" (ptr=%p)!", n->logical_id, n);
    }

    if (!(n->flags & LEAF_NODE)) {
	btree_status_t ret = BTREE_SUCCESS;

	// non-leaf
	for (i=0; i<n->nkeys; i++) {
	    get_key_stuff(bt, n, i, &ks);
	    n_child = get_existing_node(&ret, bt, ks.ptr, 0, LOCKTYPE_NOLOCK); 
	    traverse_node(pzs, bt, n_child->pnode, shard_in, check_cb, pdata);
	}
	deref_l1cache_node(bt, n_child);
    }
    ht_unlock(&ht_handle);
}

/**********************************************************************
 *
 *  simple bit vector data structure
 *
 **********************************************************************/

static struct bv *bv_init(uint32_t n)
{
    int     i;
    bv_t   *bv;

    bv = (bv_t *) malloc(sizeof(bv_t));
    plat_assert(bv);
    bv->n_ints = (n+8*sizeof(uint32_t)-1)/(8*sizeof(uint32_t));
    bv->ints = (uint32_t *) malloc(bv->n_ints*sizeof(uint32_t));
    plat_assert(bv->ints);
    for (i=0; i<bv->n_ints; i++) {
        bv->ints[i] = 0;
    }
    return(bv);
}

static void bv_clear(struct bv *bv)
{
    int   i;

    for (i=0; i<bv->n_ints; i++) {
        bv->ints[i] = 0;
    }
}

static void bv_set(struct bv *bv, uint32_t n)
{
    uint32_t  ni, no;
    
    ni = n/(8*sizeof(uint32_t));
    no = n % (8*sizeof(uint32_t));

    bv->ints[ni] |= (1<<no);
}

static void bv_set_bits(struct bv *bv, uint32_t n, uint32_t cnt)
{
    uint32_t  i, j;
    
    j = n;
    for (i=0; i<cnt; i++) {
        bv_set(bv, j);
        j++;
    }
}

#ifdef notdef
static void bv_unset(struct bv *bv, uint32_t n)
{
    uint32_t  ni, no;
    
    ni = n/(8*sizeof(uint32_t));
    no = n % (8*sizeof(uint32_t));

    bv->ints[ni] &= (~(1<<no));
}
#endif

static int bv_bit(struct bv *bv, uint32_t n)
{
    uint32_t  ni, no;
    
    ni = n/(8*sizeof(uint32_t));
    no = n % (8*sizeof(uint32_t));

    if (bv->ints[ni] & (1<<no)) {
        return(1);
    } else {
        return(0);
    }
}

/**********************************************************************
 *
 *  end of code for simple bit vector data structure
 *
 **********************************************************************/

