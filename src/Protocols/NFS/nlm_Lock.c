/*
 * Copyright IBM Corporation, 2010
 *  Contributor: Aneesh Kumar K.v  <aneesh.kumar@linux.vnet.ibm.com>
 *
 * --------------------------
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * 
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef _SOLARIS
#include "solaris_port.h"
#endif

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/file.h>           /* for having FNDELAY */
#include "HashData.h"
#include "HashTable.h"
#include "rpc.h"
#include "log_macros.h"
#include "stuff_alloc.h"
#include "nfs23.h"
#include "nfs4.h"
#include "nfs_core.h"
#include "cache_inode.h"
#include "cache_content.h"
#include "nfs_exports.h"
#include "nfs_creds.h"
#include "nfs_tools.h"
#include "mount.h"
#include "nfs_proto_functions.h"
#include "nlm_util.h"
#include "nlm_async.h"

/**
 * nlm4_Lock: Set a range lock
 *
 *  @param parg        [IN]
 *  @param pexportlist [IN]
 *  @param pcontextp   [IN]
 *  @param pclient     [INOUT]
 *  @param ht          [INOUT]
 *  @param preq        [IN]
 *  @param pres        [OUT]
 *
 */

int nlm4_Lock(nfs_arg_t * parg /* IN     */ ,
              exportlist_t * pexport /* IN     */ ,
              fsal_op_context_t * pcontext /* IN     */ ,
              cache_inode_client_t * pclient /* INOUT  */ ,
              hash_table_t * ht /* INOUT  */ ,
              struct svc_req *preq /* IN     */ ,
              nfs_res_t * pres /* OUT    */ )
{
  nlm4_lockargs *arg = &parg->arg_nlm4_lock;
  cache_entry_t *pentry;
  fsal_attrib_list_t attr;
  nlm_lock_entry_t *nlm_entry = NULL;
  cache_inode_status_t cache_status;
  cache_inode_fsal_data_t fsal_data;
  char buffer[1024];

  netobj_to_string(&arg->cookie, buffer, 1024);
  LogDebug(COMPONENT_NLM,
           "REQUEST PROCESSING: Calling nlm4_Lock svid=%d off=%llx len=%llx cookie=%s",
           (int) arg->alock.svid,
           (unsigned long long) arg->alock.l_offset,
           (unsigned long long) arg->alock.l_len, buffer);

  copy_netobj(&pres->res_nlm4test.cookie, &arg->cookie);

  /* Convert file handle into a cache entry */
  if(!nfs3_FhandleToFSAL((nfs_fh3 *) & (arg->alock.fh), &fsal_data.handle, pcontext))
    {
      /* handle is not valid */
      pres->res_nlm4.stat.stat = NLM4_STALE_FH;
      /*
       * Should we do a REQ_OK so that the client get
       * a response ? FIXME!!
       */
      LogDebug(COMPONENT_NLM, "REQUEST RESULT: nlm4_Lock %s",
               lock_result_str(pres->res_nlm4.stat.stat));
      return NFS_REQ_DROP;
    }
  /* Now get the cached inode attributes */
  fsal_data.cookie = DIR_START;
  if((pentry = cache_inode_get(&fsal_data, &attr, ht,
                               pclient, pcontext, &cache_status)) == NULL)
    {
      /* handle is not valid */
      pres->res_nlm4.stat.stat = NLM4_STALE_FH;
      LogDebug(COMPONENT_NLM, "REQUEST RESULT: nlm4_Lock %s",
               lock_result_str(pres->res_nlm4.stat.stat));
      return NFS_REQ_OK;
    }

  /* allow only reclaim lock request during recovery */
  if(in_nlm_grace_period() && !arg->reclaim)
    {
      pres->res_nlm4.stat.stat = NLM4_DENIED_GRACE_PERIOD;
      LogDebug(COMPONENT_NLM, "REQUEST RESULT: nlm4_Lock %s",
               lock_result_str(pres->res_nlm4.stat.stat));
      return NFS_REQ_OK;
    }
  if(!in_nlm_grace_period() && arg->reclaim)
    {
      pres->res_nlm4.stat.stat = NLM4_DENIED_GRACE_PERIOD;
      LogDebug(COMPONENT_NLM, "REQUEST RESULT: nlm4_Lock %s",
               lock_result_str(pres->res_nlm4.stat.stat));
      return NFS_REQ_OK;
    }

  /* add the host to monitor list */
  if(nlm_monitor_host(arg->alock.caller_name))
    {
      /*
       * ok failed to register monitor for the host
       */
      LogDebug(COMPONENT_NLM, "Failed to register a monitor for the host %s",
               arg->alock.caller_name);
      pres->res_nlm4.stat.stat = NLM4_DENIED_NOLOCKS;
      LogDebug(COMPONENT_NLM, "REQUEST RESULT: nlm4_Lock %s",
               lock_result_str(pres->res_nlm4.stat.stat));
      return NFS_REQ_OK;

    }
  /*
   * Add lock details to the lock list. This check for conflicting
   * locks and add entry with correct state value.
   */
  nlm_entry = nlm_add_to_locklist(arg, pentry, pclient, pcontext);
  if(!nlm_entry)
    {
      /* We failed to create a lock entry and add to the list */
      nlm_unmonitor_host(arg->alock.caller_name);
      pres->res_nlm4.stat.stat = NLM4_DENIED_NOLOCKS;
      LogDebug(COMPONENT_NLM, "REQUEST RESULT: nlm4_Lock %s",
               lock_result_str(pres->res_nlm4.stat.stat));
      return NFS_REQ_OK;
    }
  pres->res_nlm4.stat.stat = nlm_entry->state;
  nlm_entry->ht = ht;
  nlm_lock_entry_dec_ref(nlm_entry);
  LogDebug(COMPONENT_NLM, "REQUEST RESULT: nlm4_Lock %s",
           lock_result_str(pres->res_nlm4.stat.stat));
  return NFS_REQ_OK;
}

static void nlm4_lock_message_resp(void *arg)
{
  nlm_async_res_t *pres = arg;

  if(isFullDebug(COMPONENT_NLM))
    {
      char buffer[1024];
      netobj_to_string(&pres->pres.res_nlm4test.cookie, buffer, 1024);
      LogFullDebug(COMPONENT_NLM,
                   "nlm4_unlock_message_resp calling nlm_send_async cookie=%s status=%s",
                   buffer, lock_result_str(pres->pres.res_nlm4.stat.stat));
    }
  nlm_send_async(NLMPROC4_LOCK_RES, pres->caller_name, &(pres->pres), NULL);
  nlm4_Lock_Free(&pres->pres);
  Mem_Free(pres->caller_name);
  Mem_Free(pres);
}

/**
 * nlm4_Lock_Message: Lock Message
 *
 *  @param parg        [IN]
 *  @param pexportlist [IN]
 *  @param pcontextp   [IN]
 *  @param pclient     [INOUT]
 *  @param ht          [INOUT]
 *  @param preq        [IN]
 *  @param pres        [OUT]
 *
 */
int nlm4_Lock_Message(nfs_arg_t * parg /* IN     */ ,
                      exportlist_t * pexport /* IN     */ ,
                      fsal_op_context_t * pcontext /* IN     */ ,
                      cache_inode_client_t * pclient /* INOUT  */ ,
                      hash_table_t * ht /* INOUT  */ ,
                      struct svc_req *preq /* IN     */ ,
                      nfs_res_t * pres /* OUT    */ )
{
  nlm_async_res_t *arg;
  LogDebug(COMPONENT_NLM, "REQUEST PROCESSING: Calling nlm_Lock_Message");

  nlm4_Lock(parg, pexport, pcontext, pclient, ht, preq, pres);

  arg = nlm_build_async_res_nlm4(parg->arg_nlm4_lock.alock.caller_name, pres);
  if(arg != NULL)
    nlm_async_callback(nlm4_lock_message_resp, arg);

  return NFS_REQ_DROP;

}

/**
 * nlm4_Lock_Free: Frees the result structure allocated for nlm4_Lock
 *
 * Frees the result structure allocated for nlm4_Lock. Does Nothing in fact.
 *
 * @param pres        [INOUT]   Pointer to the result structure.
 *
 */
void nlm4_Lock_Free(nfs_res_t * pres)
{
  netobj_free(&pres->res_nlm4test.cookie);
  return;
}