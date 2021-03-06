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

/*
 * File:   action_internal_ctxt.h
 * Author: Brian O'Krafka
 *
 * Created on April 9, 2008
 *
 * (c) Copyright 2008, Schooner Information Technology, Inc.
 * http://www.schoonerinfotech.com/
 *
 * $Id: action_internal_ctxt.h 802 2008-03-29 00:44:48Z darpan $
 */

#include "common/sdftypes.h"
#include "sdfmsg/sdf_msg_types.h"
#include "common/sdftypes.h"
#include "agent/agent_helper.h"
#include "fth/fth.h"
#include "protocol/protocol_common.h"
#include "protocol/replication/sdf_vips.h"
#include "protocol/action/simple_replication.h"
#include "shared/sdf_sm_msg.h"
#include "shared/container_meta.h"

#ifndef _ACTION_INTERNAL_CTXT_H
#define _ACTION_INTERNAL_CTXT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t              appreq_counts[N_SDF_APP_REQS];
    uint64_t              msg_out_counts[N_SDF_PROTOCOL_MSGS];
    uint64_t              msg_in_counts[N_SDF_PROTOCOL_MSGS];
    uint64_t              sdf_status_counts[N_SDF_STATUS_STRINGS];
    uint64_t              flash_retcode_counts[FLASH_N_ERR_CODES];
    int64_t               n_only_in_cache;
    int64_t               n_total_in_cache;
    int64_t               bytes_only_in_cache;
    int64_t               bytes_total_in_cache;
    int64_t               all_bytes_ever_created;
    int64_t               all_objects_ever_created;
    int64_t               n_overwrites_s;
    int64_t               n_overwrites_m;
    int64_t               n_in_place_overwrites_s;
    int64_t               n_in_place_overwrites_m;
    int64_t               n_new_entry;
    int64_t               n_writebacks;
    int64_t               n_writethrus;
    int64_t               n_flushes;
    //  added to expose async write/writeback/flush failures:
    int64_t               n_async_drains;
    int64_t               n_async_puts;
    int64_t               n_async_flushes;
    int64_t               n_async_wrbks;
    int64_t               n_async_put_fails;
    int64_t               n_async_flush_fails;
    int64_t               n_async_wrbk_fails;
	int64_t               n_async_commit_fails;
} SDF_cache_ctnr_stats_t;

typedef struct SDF_action_stats_new {
    SDF_cache_ctnr_stats_t   *ctnr_stats; // 1 per container
} SDF_action_stats_new_t;

typedef struct SDF_action_stats {
    uint64_t              appreq_counts[N_SDF_APP_REQS];
    uint64_t              msg_out_counts[N_SDF_PROTOCOL_MSGS];
    uint64_t              msg_in_counts[N_SDF_PROTOCOL_MSGS];
} SDF_action_stats_t;

typedef struct {
    int                   valid;
    int                   n;
    SDF_cguid_t           cguid;
    SDF_container_meta_t  meta;
    struct shard         *pshard;
    uint64_t              n_only_in_cache;
    uint64_t              n_total_in_cache;
    uint64_t              bytes_only_in_cache;
    uint64_t              bytes_total_in_cache;
    uint64_t              all_bytes_ever_created;
    uint64_t              all_objects_ever_created;
    uint64_t              all_objects_at_restart;
    uint32_t              flush_progress_percent;

    /**
     * @brief Key based locks for recovery/new operation interaction
     *
     * At 2010-03-05 only recovery get-by-cursor, recovery put, and
     * new delete operations use this.  With only full recovery 
     * implemented recovery puts can use FLASH_PUT_TEST_NONEXIST to avoid
     * overwriting newer puts.
     */
    struct replicator_key_lock_container *lock_container;
} SDF_cache_ctnr_metadata_t;

struct SDF_action_thrd_state;
struct SDFNewCache;
struct SDF_async_puts_state;

/*
 * This is the structure that sync container worker threads use to
 * pass data back and forth between to get cursors to work on
 */
struct cursor_data {
    SDF_context_t         ctxt;
    SDF_shardid_t         shard;
    vnode_t               dest_node;
    void                 *cursor;
    int                   cursor_len;
    SDF_cguid_t           cguid;
    fthMbox_t             mbox;
    int64_t               clock_skew;
    int                   rc;
    /* Lock container for current container */
    struct replicator_key_lock_container *lock_container;
};

#define DEFAULT_NUM_SYNC_CONTAINER_THREADS 32
#define DEFAULT_NUM_SYNC_CONTAINER_CURSORS 1000

typedef struct SDF_action_state {
    SDF_vnode_t              mynode;
    SDF_context_t            next_ctxt;   /* xxxzzz fix this */
    /*
     * Arrays of pointers to queue pairs are sized by nnode and indexed
     * by the destination node.
     */
    struct sdf_queue_pair  **q_pair_consistency;
    struct sdf_queue_pair  **q_pair_responses;
    uint32_t                 nthrds;
    fthLock_t                nthrds_lock;
    uint32_t                 nnodes;
    uint64_t                 nbuckets;
    uint64_t                 nslabs;
    uint64_t                 cachesize;
    fthMbox_t                mbox_request; /* for "messages" to self-node */
    fthMbox_t                mbox_response; /* for "messages" from self-node */
    uint64_t                 n_context;
    uint64_t                 contextcount;
    fthLock_t                context_lock;
    fthLock_t                flush_all_lock;
    fthLock_t                flush_ctnr_lock;
    fthLock_t                sync_ctnr_lock;
    fthLock_t                ctnr_preload_lock;
    fthLock_t                stats_lock;
    SDF_action_stats_t       stats;
    struct SDF_action_thrd_state  *threadstates;
    /** @brief Disable fast path between action and home code */
    int                      disable_fast_path;
    /* for action_new.c */
    SDF_boolean_t                     state_machine_mode;
    struct SDFNewCache               *new_actiondir;
    int                               n_containers;

    SDF_cache_ctnr_metadata_t        *ctnr_meta;
    #ifdef MULTIPLE_FLASH_DEV_ENABLED
	struct flashDev             **flash_dev;
    #else
	struct flashDev              *flash_dev;
    #endif
    uint32_t                          flash_dev_count;
    SDF_boolean_t                     trace_on;
    SDF_boolean_t                     new_allocator;
    SDF_action_stats_new_t            *stats_new_per_sched;
    SDF_action_stats_new_t            *stats_new;
    SDF_action_stats_new_t            *stats_per_ctnr;
    SDF_boolean_t                     strict_wrbk;
    SDF_boolean_t                     always_miss;
    SDF_boolean_t                     enable_replication;
    fthLock_t                         container_serialization_lock;

    // for simple replication
    SDF_boolean_t                 simple_replication;
    qrep_state_t                  qrep_state;
    SDF_boolean_t                 failback;
    fthLock_t                     sync_remote_ctnr_lock;
    fthMbox_t                     git_mbx;
    fthMbox_t                    *gbc_mbx;
    fthMbox_t                    *cursor_mbx_todo;
    fthMbox_t                    *cursor_mbx_done;
    struct cursor_data           *cursor_datas;

    // for asynchronous puts
    struct SDF_async_puts_state  *async_puts_state;
    uint32_t                      max_obj_size;
    uint32_t                      max_key_size;

    // for prefix-based delete
    char                          prefix_delete_delimiter;

    //  Count of writes in flight, used to ensure there is
    //  space for asynchronous writes.
    uint64_t                      writes_in_flight;

} SDF_action_state_t;

typedef struct SDF_action_init {
   SDF_action_state_t  *pcs;
   uint32_t             nthreads;
   uint32_t             nnode;
   uint32_t             nnodes;
   SDF_context_t        ctxt;
   void                *pts;
   void                *phs;
   struct ssdaio_ctxt  *paio_ctxt;

   /**
    * @brief Disable fast path between action and home node code
    *
    * This is mostly for debugging replication in a single node
    * environment.  Long term we might want to have a non-coherent
    * replicated case where fthHomeFunction has a fast path to
    * local replication.
    */
   int                  disable_fast_path;

    /*  mbox_idx stores the mailbox index (of mbox_shmem array in the queue) that
        this thread will wait on. */
   int                 mbox_idx;

   /* for action_new.c */

#ifdef MULTIPLE_FLASH_DEV_ENABLED
    struct flashDev        **flash_dev;
#else
    struct flashDev        *flash_dev;
#endif
    uint32_t                flash_dev_count;

} SDF_action_init_t;

SDF_action_state_t *allocate_action_state();
void deallocate_action_state(SDF_action_state_t *pas);
#ifdef	__cplusplus
}
#endif

#endif /* _ACTION_INTERNAL_CTXT_H */
