/**
 * (C) Copyright 2016-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
/**
 * VOS Container API implementation
 * vos/vos_container.c
 *
 * Author: Vishwanath Venkatesan <vishwanath.venkatesan@intel.com>
 */
#define D_LOGFAC	DD_FAC(vos)

#include <daos_srv/vos.h>
#include <daos_errno.h>
#include <daos/common.h>
#include <daos/mem.h>
#include <gurt/hash.h>
#include <daos/btree.h>
#include <daos_types.h>
#include "vos_obj.h"
#include <daos/checksum.h>

#include "vos_internal.h"

/**
 * Parameters for vos_cont_df btree
 */
struct cont_df_args {
	struct vos_cont_df	*ca_cont_df;
	struct vos_pool		*ca_pool;
};

static int
cont_df_hkey_size(void)
{
	return sizeof(struct d_uuid);
}

static int
cont_df_rec_msize(int alloc_overhead)
{
	return alloc_overhead + sizeof(struct vos_cont_df);
}


static void
cont_df_hkey_gen(struct btr_instance *tins, d_iov_t *key_iov, void *hkey)
{
	D_ASSERT(key_iov->iov_len == sizeof(struct d_uuid));
	memcpy(hkey, key_iov->iov_buf, key_iov->iov_len);
}

static int
cont_df_rec_free(struct btr_instance *tins, struct btr_record *rec, void *args)
{
	struct vos_cont_df	*cont_df;
	struct vos_pool		*vos_pool = (struct vos_pool *)tins->ti_priv;

	if (UMOFF_IS_NULL(rec->rec_off))
		return -DER_NONEXIST;

	cont_df = umem_off2ptr(&tins->ti_umm, rec->rec_off);
	vos_ts_evict(&cont_df->cd_ts_idx, VOS_TS_TYPE_CONT, vos_pool->vp_sysdb);

	return gc_add_item(vos_pool, DAOS_HDL_INVAL, GC_CONT, rec->rec_off, 0);
}

static int
cont_df_rec_alloc(struct btr_instance *tins, d_iov_t *key_iov,
		  d_iov_t *val_iov, struct btr_record *rec, d_iov_t *val_out)
{
	struct vos_pool		*pool;
	struct cont_df_args	*args;
	struct d_uuid		*ukey;
	struct vos_cont_df	*cont_df;
	daos_handle_t		 hdl;
	umem_off_t		 offset;
	int			 rc = 0;

	D_ASSERT(key_iov->iov_len == sizeof(struct d_uuid));
	ukey = (struct d_uuid *)key_iov->iov_buf;
	args = (struct cont_df_args *)val_iov->iov_buf;
	pool = args->ca_pool;

	D_DEBUG(DB_DF, "Allocating container uuid=" DF_UUID "\n",
		DP_UUID(ukey->uuid));
	offset = umem_zalloc(&tins->ti_umm, sizeof(struct vos_cont_df));
	if (UMOFF_IS_NULL(offset))
		return -DER_NOSPACE;

	cont_df = umem_off2ptr(&tins->ti_umm, offset);
	if (args->ca_cont_df != NULL) {
		/* Copy from existing container, only happened during upgrade.
		 * see vos_cont_upgrade().
		 */
		D_DEBUG(DB_DF, "copying from existing container " DF_UUID "\n",
			DP_UUID(ukey->uuid));
		memcpy(cont_df, args->ca_cont_df, sizeof(*cont_df));
		D_ASSERT(cont_df->cd_obj_root.tr_class != 0);
	} else {
		rc = dbtree_create_inplace_ex(VOS_BTR_OBJ_TABLE, 0, VOS_OBJ_ORDER, &pool->vp_uma,
					      &cont_df->cd_obj_root, DAOS_HDL_INVAL, pool, &hdl);
		if (rc) {
			D_ERROR("dbtree create failed\n");
			D_GOTO(failed, rc);
		}
		dbtree_close(hdl);
	}

	uuid_copy(cont_df->cd_id, ukey->uuid);

	gc_init_cont(&tins->ti_umm, cont_df);
	args->ca_cont_df = cont_df;
	rec->rec_off = offset;
	return 0;
failed:
	/* Ignore umem_free failure. */
	umem_free(&tins->ti_umm, offset);
	return rc;
}

static int
cont_df_rec_fetch(struct btr_instance *tins, struct btr_record *rec,
		  d_iov_t *key_iov, d_iov_t *val_iov)
{
	struct vos_cont_df		*cont_df;
	struct cont_df_args		*args = NULL;

	cont_df = umem_off2ptr(&tins->ti_umm, rec->rec_off);
	args = (struct cont_df_args *)val_iov->iov_buf;
	args->ca_cont_df = cont_df;
	val_iov->iov_len = sizeof(struct cont_df_args);

	return 0;
}

static int
cont_df_rec_update(struct btr_instance *tins, struct btr_record *rec,
		   d_iov_t *key, d_iov_t *val, d_iov_t *val_out)
{
	D_DEBUG(DB_DF, "Record exists already. Nothing to do\n");
	return 0;
}

static btr_ops_t vct_ops = {
	.to_rec_msize	= cont_df_rec_msize,
	.to_hkey_size	= cont_df_hkey_size,
	.to_hkey_gen	= cont_df_hkey_gen,
	.to_rec_alloc	= cont_df_rec_alloc,
	.to_rec_free	= cont_df_rec_free,
	.to_rec_fetch	= cont_df_rec_fetch,
	.to_rec_update  = cont_df_rec_update,
};

static int
cont_df_lookup(struct vos_pool *vpool, struct d_uuid *ukey,
	       struct cont_df_args *args)
{
	d_iov_t	key, value;

	d_iov_set(&key, ukey, sizeof(struct d_uuid));
	d_iov_set(&value, args, sizeof(struct cont_df_args));
	return dbtree_lookup(vpool->vp_cont_th, &key, &value);
}

/**
 * Container cache secondary key
 * comparison
 */
bool
cont_cmp(struct d_ulink *ulink, void *cmp_args)
{
	struct vos_container *cont;
	struct d_uuid	     *pkey;

	pkey = (struct d_uuid *)cmp_args;
	cont = container_of(ulink, struct vos_container, vc_uhlink);
	return !uuid_compare(cont->vc_pool->vp_id, pkey->uuid);
}

static void
cont_free_internal(struct vos_container *cont)
{
	int i;

	D_ASSERT(cont->vc_open_count == 0);

	if (daos_handle_is_valid(cont->vc_dtx_active_hdl))
		dbtree_destroy(cont->vc_dtx_active_hdl, NULL);
	if (daos_handle_is_valid(cont->vc_dtx_committed_hdl))
		dbtree_destroy(cont->vc_dtx_committed_hdl, NULL);

	if (cont->vc_dtx_array)
		lrua_array_free(cont->vc_dtx_array);

	D_ASSERT(d_list_empty(&cont->vc_dtx_act_list));

	dbtree_close(cont->vc_btr_hdl);

	if (!d_list_empty(&cont->vc_gc_link))
		d_list_del(&cont->vc_gc_link);

	for (i = 0; i < VOS_IOS_CNT; i++) {
		if (cont->vc_hint_ctxt[i])
			vea_hint_unload(cont->vc_hint_ctxt[i]);
	}

	cont->vc_pool->vp_dtx_committed_count -= cont->vc_dtx_committed_count;
	d_tm_dec_gauge(vos_tls_get(cont->vc_pool->vp_sysdb)->vtl_committed,
		       cont->vc_dtx_committed_count);

	D_FREE(cont);
}

/**
 * Container cache functions
 */
void
cont_free(struct d_ulink *ulink)
{
	struct vos_container		*cont;

	cont = container_of(ulink, struct vos_container, vc_uhlink);
	cont_free_internal(cont);
}

struct d_ulink_ops   co_hdl_uh_ops = {
	.uop_free       = cont_free,
	.uop_cmp	= cont_cmp,
};

int
cont_insert(struct vos_container *cont, struct d_uuid *key, struct d_uuid *pkey,
	    daos_handle_t *coh)
{
	int	rc	= 0;

	D_ASSERT(cont != NULL && coh != NULL);

	d_uhash_ulink_init(&cont->vc_uhlink, &co_hdl_uh_ops);
	rc = d_uhash_link_insert(vos_cont_hhash_get(cont->vc_pool->vp_sysdb), key,
				 pkey, &cont->vc_uhlink);
	if (rc) {
		D_ERROR("UHASH table container handle insert failed\n");
		D_GOTO(exit, rc);
	}

	*coh = vos_cont2hdl(cont);
exit:
	return rc;
}

static int
cont_lookup(struct d_uuid *key, struct d_uuid *pkey,
	    struct vos_container **cont, bool is_sysdb) {

	struct d_ulink *ulink;

	ulink = d_uhash_link_lookup(vos_cont_hhash_get(is_sysdb), key, pkey);
	if (ulink == NULL)
		return -DER_NONEXIST;

	*cont = container_of(ulink, struct vos_container, vc_uhlink);
	return 0;
}

static void
cont_decref(struct vos_container *cont)
{
	d_uhash_link_putref(vos_cont_hhash_get(cont->vc_pool->vp_sysdb), &cont->vc_uhlink);
}

static void
cont_addref(struct vos_container *cont)
{
	d_uhash_link_addref(vos_cont_hhash_get(cont->vc_pool->vp_sysdb), &cont->vc_uhlink);
}

/**
 * Create a container within a VOS pool
 */
int
vos_cont_create(daos_handle_t poh, uuid_t co_uuid)
{

	struct vos_pool		*vpool = NULL;
	struct cont_df_args	 args = {0};
	struct d_uuid		 ukey;
	d_iov_t			 key, value;
	int			 rc = 0;

	vpool = vos_hdl2pool(poh);
	if (vpool == NULL) {
		D_ERROR("Empty pool handle?\n");
		return -DER_INVAL;
	}

	D_DEBUG(DB_TRACE, "looking up co_id in container index\n");
	uuid_copy(ukey.uuid, co_uuid);
	args.ca_pool = vpool;

	rc = cont_df_lookup(vpool, &ukey, &args);
	if (!rc) {
		/* Check if attempt to reuse the same container uuid */
		D_ERROR("Container already exists\n");
		D_GOTO(exit, rc = -DER_EXIST);
	}

	rc = umem_tx_begin(vos_pool2umm(vpool), NULL);
	if (rc != 0)
		goto exit;

	d_iov_set(&key, &ukey, sizeof(ukey));
	d_iov_set(&value, &args, sizeof(args));

	rc = dbtree_update(vpool->vp_cont_th, &key, &value);

	rc = umem_tx_end(vos_pool2umm(vpool), rc);
exit:
	return rc;
}

static const struct lru_callbacks lru_cont_cbs = {
	.lru_on_alloc = vos_lru_alloc_track,
	.lru_on_free = vos_lru_free_track,
};

/**
 * Open a container within a VOSP
 */
int
vos_cont_open(daos_handle_t poh, uuid_t co_uuid, daos_handle_t *coh)
{

	int				rc = 0;
	struct vos_pool			*pool = NULL;
	struct d_uuid			ukey;
	struct d_uuid			pkey;
	struct cont_df_args		 args = {0};
	struct vos_container		*cont = NULL;
	struct umem_attr		uma;

	D_DEBUG(DB_TRACE, "Open container "DF_UUID"\n", DP_UUID(co_uuid));

	pool = vos_hdl2pool(poh);
	if (pool == NULL) {
		D_ERROR("Empty pool handle?\n");
		return -DER_INVAL;
	}
	uuid_copy(pkey.uuid, pool->vp_id);
	uuid_copy(ukey.uuid, co_uuid);

	/**
	 * Check if handle exists
	 * then return the handle immediately
	 */
	rc = cont_lookup(&ukey, &pkey, &cont, pool->vp_sysdb);
	if (rc == 0) {
		cont->vc_open_count++;
		D_DEBUG(DB_TRACE, "Found handle for cont "DF_UUID
			" in DRAM hash table, open count: %d\n",
			DP_UUID(co_uuid), cont->vc_open_count);
		*coh = vos_cont2hdl(cont);
		D_GOTO(exit, rc);
	}

	rc = cont_df_lookup(pool, &ukey, &args);
	if (rc) {
		D_DEBUG(DB_TRACE, DF_UUID" container does not exist\n",
			DP_UUID(co_uuid));
		D_GOTO(exit, rc);
	}

	D_ALLOC_PTR(cont);
	if (!cont) {
		D_GOTO(exit, rc = -DER_NOMEM);
	}

	uuid_copy(cont->vc_id, co_uuid);
	cont->vc_pool	 = pool;
	cont->vc_cont_df = args.ca_cont_df;
	cont->vc_ts_idx = &cont->vc_cont_df->cd_ts_idx;
	cont->vc_dtx_active_hdl = DAOS_HDL_INVAL;
	cont->vc_dtx_committed_hdl = DAOS_HDL_INVAL;
	if (UMOFF_IS_NULL(cont->vc_cont_df->cd_dtx_committed_head))
		cont->vc_cmt_dtx_indexed = 1;
	else
		cont->vc_cmt_dtx_indexed = 0;
	cont->vc_cmt_dtx_reindex_pos = cont->vc_cont_df->cd_dtx_committed_head;
	D_INIT_LIST_HEAD(&cont->vc_dtx_act_list);
	cont->vc_dtx_committed_count = 0;
	cont->vc_solo_dtx_epoch = d_hlc_get();
	gc_check_cont(cont);

	/* Cache this btr object ID in container handle */
	rc = dbtree_open_inplace_ex(&cont->vc_cont_df->cd_obj_root,
				    &pool->vp_uma, vos_cont2hdl(cont),
				    cont->vc_pool, &cont->vc_btr_hdl);
	if (rc) {
		D_ERROR("No Object handle, Tree open failed\n");
		D_GOTO(exit, rc);
	}

	D_DEBUG(DB_TRACE, "root is %p\n", &cont->vc_cont_df->cd_obj_root);
	memset(&uma, 0, sizeof(uma));
	uma.uma_id = UMEM_CLASS_VMEM;

	rc = lrua_array_alloc(&cont->vc_dtx_array, DTX_ARRAY_LEN, DTX_ARRAY_NR,
			      sizeof(struct vos_dtx_act_ent),
			      LRU_FLAG_REUSE_UNIQUE, &lru_cont_cbs,
			      vos_tls_get(cont->vc_pool->vp_sysdb));
	if (rc != 0) {
		D_ERROR("Failed to create DTX active array: rc = "DF_RC"\n",
			DP_RC(rc));
		D_GOTO(exit, rc);
	}

	rc = dbtree_create_inplace_ex(VOS_BTR_DTX_ACT_TABLE, 0,
				      DTX_BTREE_ORDER, &uma,
				      &cont->vc_dtx_active_btr,
				      DAOS_HDL_INVAL, cont,
				      &cont->vc_dtx_active_hdl);
	if (rc != 0) {
		D_ERROR("Failed to create DTX active btree: rc = "DF_RC"\n",
			DP_RC(rc));
		D_GOTO(exit, rc);
	}

	rc = dbtree_create_inplace_ex(VOS_BTR_DTX_CMT_TABLE, 0,
				      DTX_BTREE_ORDER, &uma,
				      &cont->vc_dtx_committed_btr,
				      DAOS_HDL_INVAL, cont,
				      &cont->vc_dtx_committed_hdl);
	if (rc != 0) {
		D_ERROR("Failed to create DTX committed btree: rc = "DF_RC"\n",
			DP_RC(rc));
		D_GOTO(exit, rc);
	}

	if (cont->vc_pool->vp_vea_info != NULL) {
		int	i;

		for (i = 0; i < VOS_IOS_CNT; i++) {
			rc = vea_hint_load(&cont->vc_cont_df->cd_hint_df[i],
					   &cont->vc_hint_ctxt[i]);
			if (rc) {
				D_ERROR("Error loading allocator %d hint "
					DF_UUID": %d\n", i, DP_UUID(co_uuid),
					rc);
				goto exit;
			}
		}
	}

	rc = vos_dtx_act_reindex(cont);
	if (rc != 0) {
		D_ERROR("Fail to reindex active DTX entries: %d\n", rc);
		goto exit;
	}

	rc = cont_insert(cont, &ukey, &pkey, coh);
	if (rc != 0) {
		D_ERROR("Error inserting vos container handle to uuid hash\n");
		goto exit;
	}

	cont->vc_open_count = 1;
	D_DEBUG(DB_TRACE, "Inert cont "DF_UUID" into hash table.\n",
		DP_UUID(cont->vc_id));

exit:
	if (rc != 0 && cont)
		cont_free_internal(cont);

	return rc;
}

int
vos_cont_get_boundary(daos_handle_t coh, uint64_t *epoch)
{
	struct vos_container *cont;
	struct cont_df_args   args = {0};
	struct d_uuid	      ukey;
	int		      rc;

	cont = vos_hdl2cont(coh);
	if (cont == NULL) {
		rc = -DER_NO_HDL;
		DL_ERROR(rc, "Can not find vos container.");
		return rc;
	}

	if (cont->vc_pool->vp_pool_df->pd_version < VOS_POOL_DF_2_6) {
		D_DEBUG(DB_MD, "skip for lower version container " DF_UUID "/%u\n",
			DP_UUID(cont->vc_id), cont->vc_pool->vp_pool_df->pd_version);
		*epoch = 0;
		return 0;
	}

	if (cont->vc_lowest_act_eph != 0) {
		*epoch = cont->vc_lowest_act_eph - 1;
		D_DEBUG(DB_TRACE, "get from act" DF_X64 " with container " DF_UUID "\n",
			*epoch, DP_UUID(cont->vc_id));
		return 0;
	}

	D_ASSERT(cont->vc_cmt_dtx_indexed == 1);
	if (cont->vc_highest_cmt_eph != 0) {
		*epoch = cont->vc_highest_cmt_eph + 1;
		D_DEBUG(DB_TRACE, "get from cmt" DF_X64 " with container " DF_UUID "\n",
			*epoch, DP_UUID(cont->vc_id));
		return 0;
	}

	uuid_copy(ukey.uuid, cont->vc_id);
	rc = cont_df_lookup(cont->vc_pool, &ukey, &args);
	if (rc) {
		D_DEBUG(DB_TRACE, DF_UUID " container does not exist.\n",
			DP_UUID(cont->vc_id));
		return rc;
	}

	D_DEBUG(DB_TRACE, "commit epoch " DF_X64 " with container " DF_UUID "\n",
		args.ca_cont_df->cd_commit_epoch, DP_UUID(cont->vc_id));
	*epoch = args.ca_cont_df->cd_commit_epoch;
	return 0;
}

int
vos_cont_update_boundary(daos_handle_t coh, uint64_t epoch)
{
	struct vos_container *cont;
	struct umem_instance *umm;
	struct vos_cont_df   *cont_df;
	int		      rc = 0;

	cont = vos_hdl2cont(coh);
	if (cont == NULL) {
		D_ERROR("Can not find vos container\n");
		return -DER_NO_HDL;
	}

	cont_df = cont->vc_cont_df;
	if (cont->vc_pool->vp_pool_df->pd_version < VOS_POOL_DF_2_6) {
		D_DEBUG(DB_MD, "skip for lower version container " DF_UUID "/%u\n",
			DP_UUID(cont->vc_id), cont->vc_pool->vp_pool_df->pd_version);
		return 0;
	}

	if (epoch <= cont_df->cd_commit_epoch) {
		D_DEBUG(DB_MD, DF_UUID " epoch " DF_X64 " < " DF_X64 "\n",
			DP_UUID(cont->vc_id), epoch, cont_df->cd_commit_epoch);
		return 0;
	}

	umm = vos_cont2umm(cont);
	rc = umem_tx_begin(umm, NULL);
	if (rc != 0) {
		DL_ERROR(rc, "Failed to TX begin.");
		return rc;
	}

	rc = umem_tx_add_ptr(umm, &cont_df->cd_commit_epoch, sizeof(cont_df->cd_commit_epoch));
	if (rc != 0) {
		DL_ERROR(rc, DF_UUID " Failed to refresh epoch for commit.", DP_UUID(cont->vc_id));
		D_GOTO(out_tx, rc);
	}

	D_DEBUG(DB_MD, DF_UUID " epoch " DF_X64 " -> " DF_X64 "\n",
		DP_UUID(cont->vc_id), cont_df->cd_commit_epoch, epoch);

	cont_df->cd_commit_epoch = epoch;

out_tx:
	rc = umem_tx_end(umm, rc);

	return rc;
}

/**
 * Release container open handle
 */
int
vos_cont_close(daos_handle_t coh)
{
	struct vos_container	*cont;

	cont = vos_hdl2cont(coh);
	if (cont == NULL) {
		D_ERROR("Cannot close a NULL handle\n");
		return -DER_NO_HDL;
	}

	D_ASSERTF(cont->vc_open_count > 0,
		  "Invalid close "DF_UUID", open count %d\n",
		  DP_UUID(cont->vc_id), cont->vc_open_count);

	cont->vc_open_count--;
	if (cont->vc_open_count == 0)
		vos_obj_cache_evict(vos_obj_cache_current(cont->vc_pool->vp_sysdb), cont);

	D_DEBUG(DB_TRACE, "Close cont "DF_UUID", open count: %d\n",
		DP_UUID(cont->vc_id), cont->vc_open_count);

	cont_decref(cont);

	return 0;
}

/**
 * Query container information
 */
int
vos_cont_query(daos_handle_t coh, vos_cont_info_t *cont_info)
{
	struct vos_container	*cont;

	cont = vos_hdl2cont(coh);
	if (cont == NULL) {
		D_ERROR("Empty container handle for querying?\n");
		return -DER_INVAL;
	}

	cont_info->ci_nobjs = cont->vc_cont_df->cd_nobjs;
	cont_info->ci_used = cont->vc_cont_df->cd_used;
	cont_info->ci_hae = cont->vc_cont_df->cd_hae;

	return 0;
}

/**
 * Set container state
 */
int
vos_cont_ctl(daos_handle_t coh, enum vos_cont_opc opc)
{
	struct vos_container	*cont;

	cont = vos_hdl2cont(coh);
	if (cont == NULL) {
		D_ERROR("Empty container handle for ctl\n");
		return -DER_NO_HDL;
	}

	switch (opc) {
	default:
		return -DER_NOSYS;
	}

	return 0;
}

/**
 * Destroy a container
 */
int
vos_cont_destroy(daos_handle_t poh, uuid_t co_uuid)
{

	struct vos_pool		*pool;
	struct vos_container	*cont;
	struct cont_df_args	 args;
	struct d_uuid		 pkey;
	struct d_uuid		 key;
	d_iov_t			 iov;
	int			 rc;

	uuid_copy(key.uuid, co_uuid);
	D_DEBUG(DB_TRACE, "Destroying CO ID in container index "DF_UUID"\n",
		DP_UUID(key.uuid));

	pool = vos_hdl2pool(poh);
	if (pool == NULL) {
		D_ERROR("Empty pool handle for destroying container?\n");
		return -DER_INVAL;
	}
	uuid_copy(pkey.uuid, pool->vp_id);

	vos_dedup_invalidate(pool);

	rc = cont_lookup(&key, &pkey, &cont, pool->vp_sysdb);
	if (rc != -DER_NONEXIST) {
		D_ASSERT(rc == 0);

		if (cont->vc_open_count == 0) {
			d_uhash_link_delete(vos_cont_hhash_get(pool->vp_sysdb),
					    &cont->vc_uhlink);
			cont_decref(cont);
		} else {
			D_ERROR("Open reference exists for cont "DF_UUID
				", cannot destroy, open count: %d\n",
				DP_UUID(co_uuid), cont->vc_open_count);
			cont_decref(cont);
			D_GOTO(exit, rc = -DER_BUSY);
		}
	}


	rc = cont_df_lookup(pool, &key, &args);
	if (rc) {
		D_DEBUG(DB_TRACE, DF_UUID" container does not exist\n",
			DP_UUID(co_uuid));
		D_GOTO(exit, rc);
	}

	rc = vos_flush_wal_header(pool);
	if (rc) {
		D_ERROR("Failed to flush WAL header. "DF_RC"\n", DP_RC(rc));
		D_GOTO(exit, rc);
	}

	rc = umem_tx_begin(vos_pool2umm(pool), NULL);
	if (rc) {
		D_ERROR("Failed to start pmdk transaction: "DF_RC"\n",
			DP_RC(rc));
		D_GOTO(exit, rc);
	}

	d_iov_set(&iov, &key, sizeof(struct d_uuid));
	rc = dbtree_delete(pool->vp_cont_th, BTR_PROBE_EQ, &iov, NULL);

	rc = umem_tx_end(vos_pool2umm(pool), rc);
	if (rc) {
		D_ERROR("Failed to end pmdk transaction: "DF_RC"\n", DP_RC(rc));
		D_GOTO(exit, rc);
	}
	gc_wait();

	return 0;
exit:
	return rc;
}

static void
old_cont_df_2_new_cont_df(struct vos_cont_df *new_df, struct vos_cont_df *old_df)
{
	/* XXX might need more complex conversion if the entry is added/deleted
	 * in the middle of structure.
	 */
	memcpy(new_df, old_df, sizeof(*old_df) - sizeof(new_df->cd_commit_epoch));
}

/* Upgrade vos_container to the 2.6 format */
int
vos_cont_upgrade(daos_handle_t poh, uuid_t co_uuid)
{
	struct cont_df_args   args = { 0 };
	struct d_uuid	      pkey;
	struct d_uuid	      ukey;
	struct vos_container *cont;
	struct vos_pool      *pool;
	struct umem_instance *umm;
	struct vos_cont_df    cont_df;
	d_iov_t		      key;
	d_iov_t		      iov;
	int		      rc;

	pool = vos_hdl2pool(poh);
	if (pool == NULL) {
		D_ERROR("Empty pool handle?\n");
		return -DER_INVAL;
	}
	uuid_copy(pkey.uuid, pool->vp_id);
	uuid_copy(ukey.uuid, co_uuid);

	/**
	 * Check if handle exists
	 * then return the handle immediately
	 */
	rc = cont_lookup(&ukey, &pkey, &cont, pool->vp_sysdb);
	if (rc) {
		D_DEBUG(DB_TRACE, DF_UUID" container does not exist\n", DP_UUID(co_uuid));
		return 0;
	}

	if (!DAOS_FAIL_CHECK(DAOS_FORCE_OBJ_UPGRADE)) {
	       if (cont->vc_pool->vp_pool_df->pd_version >= POOL_DF_VERSION) {
			D_DEBUG(DB_MD, "skip for lower version container "DF_UUID"/%u\n",
				DP_UUID(cont->vc_id), cont->vc_pool->vp_pool_df->pd_version);
			D_GOTO(put, rc = 0);
	       }

	       if (cont->vc_pool->vp_pool_df->pd_version < VOS_POOL_DF_2_4) {
			rc = -DER_NO_PERM;
			DL_ERROR(rc,
				 "Direct upgrade from pre-2.4 pool not supported:"DF_UUID"/%u",
				 DP_UUID(cont->vc_id), cont->vc_pool->vp_pool_df->pd_version);
			D_GOTO(put, rc);
		}
	}

	uuid_copy(ukey.uuid, cont->vc_id);
	rc = cont_df_lookup(cont->vc_pool, &ukey, &args);
	if (rc) {
		D_DEBUG(DB_TRACE, DF_UUID" container does not exist\n",
			DP_UUID(cont->vc_id));
		D_GOTO(put, rc = 0);
	}

	old_cont_df_2_new_cont_df(&cont_df, args.ca_cont_df);
	umm = vos_cont2umm(cont);
	rc  = umem_tx_begin(umm, NULL);
	if (rc) {
		DL_ERROR(rc, "Failed to start transaction.");
		D_GOTO(put, rc);
	}

	/* reset the root to empty the tree, since these tree will be moved to another container */
	memset(&args.ca_cont_df->cd_obj_root, 0, sizeof(args.ca_cont_df->cd_obj_root));
	rc = umem_tx_add_ptr(umm, &args.ca_cont_df->cd_obj_root,
			     sizeof(&args.ca_cont_df->cd_obj_root));
	if (rc != 0)
		D_GOTO(tx_end, rc);

	/* Delete the old container */
	d_iov_set(&iov, &ukey, sizeof(struct d_uuid));
	rc = dbtree_delete(cont->vc_pool->vp_cont_th, BTR_PROBE_EQ, &iov, NULL);
	if (rc) {
		DL_ERROR(rc, "Failed to delete the container "DF_UUID,
			 DP_UUID(cont->vc_id));
		D_GOTO(tx_end, rc);
	}

	/* Update the new container */
	args.ca_cont_df = &cont_df;
	args.ca_pool    = cont->vc_pool;
	d_iov_set(&key, &ukey, sizeof(ukey));
	d_iov_set(&iov, &args, sizeof(args));
	rc = dbtree_update(cont->vc_pool->vp_cont_th, &key, &iov);
	if (rc) {
		DL_ERROR(rc, "Failed to update the container "DF_UUID"\n",
			 DP_UUID(cont->vc_id));
		D_GOTO(tx_end, rc);
	}
	dbtree_handle_reset_root(cont->vc_btr_hdl, &args.ca_cont_df->cd_obj_root);
	cont->vc_cont_df = args.ca_cont_df;
	D_DEBUG(DB_MD, DF_UUID " upgrade to current version %u contdf %p root %p\n",
		DP_UUID(cont->vc_id), POOL_DF_VERSION, cont->vc_cont_df,
		&cont->vc_cont_df->cd_obj_root);
tx_end:
	rc = umem_tx_end(umm, rc);
	if (rc)
		DL_ERROR(rc, "Failed to end transaction.");
put:
	cont_decref(cont);
	return rc;
}

void
vos_cont_addref(struct vos_container *cont)
{
	cont_addref(cont);
}

void
vos_cont_decref(struct vos_container *cont)
{
	cont_decref(cont);
}

/**
 * Internal Usage API
 * For use from container APIs and int APIs
 */

int
vos_cont_tab_register()
{
	int	rc;

	D_DEBUG(DB_DF, "Registering Container table class: %d\n",
		VOS_BTR_CONT_TABLE);

	rc = dbtree_class_register(VOS_BTR_CONT_TABLE, 0, &vct_ops);
	if (rc)
		D_ERROR("dbtree create failed\n");
	return rc;
}

/** iterator for co_uuid */
struct cont_iterator {
	struct vos_iterator		 cot_iter;
	/* Handle of iterator */
	daos_handle_t			 cot_hdl;
	/* Pool handle */
	struct vos_pool			*cot_pool;
};

static struct cont_iterator *
vos_iter2co_iter(struct vos_iterator *iter)
{
	return container_of(iter, struct cont_iterator, cot_iter);
}

static int
cont_iter_fini(struct vos_iterator *iter)
{
	int			rc = 0;
	struct cont_iterator	*co_iter;

	D_ASSERT(iter->it_type == VOS_ITER_COUUID);

	co_iter = vos_iter2co_iter(iter);

	if (daos_handle_is_valid(co_iter->cot_hdl)) {
		rc = dbtree_iter_finish(co_iter->cot_hdl);
		if (rc)
			D_ERROR("co_iter_fini failed: "DF_RC"\n", DP_RC(rc));
	}

	if (co_iter->cot_pool != NULL)
		vos_pool_decref(co_iter->cot_pool);

	D_FREE(co_iter);
	return rc;
}

int
cont_iter_prep(vos_iter_type_t type, vos_iter_param_t *param,
	       struct vos_iterator **iter_pp, struct vos_ts_set *ts_set)
{
	struct cont_iterator	*co_iter = NULL;
	struct vos_pool		*vpool = NULL;
	int			rc = 0;

	if (type != VOS_ITER_COUUID) {
		D_ERROR("Expected Type: %d, got %d\n",
			VOS_ITER_COUUID, type);
		return -DER_INVAL;
	}

	vpool = vos_hdl2pool(param->ip_hdl);
	if (vpool == NULL)
		return -DER_INVAL;

	D_ALLOC_PTR(co_iter);
	if (co_iter == NULL)
		return -DER_NOMEM;

	vos_pool_addref(vpool);
	co_iter->cot_pool = vpool;
	co_iter->cot_iter.it_type = type;

	rc = dbtree_iter_prepare(vpool->vp_cont_th, 0, &co_iter->cot_hdl);
	if (rc)
		D_GOTO(exit, rc);

	*iter_pp = &co_iter->cot_iter;
	return 0;
exit:
	cont_iter_fini(&co_iter->cot_iter);
	return rc;
}

static int
cont_iter_fetch(struct vos_iterator *iter, vos_iter_entry_t *it_entry,
		  daos_anchor_t *anchor)
{
	struct cont_iterator	*co_iter = vos_iter2co_iter(iter);
	d_iov_t		key, value;
	struct d_uuid		ukey;
	struct cont_df_args	args;
	int			rc;

	D_ASSERT(iter->it_type == VOS_ITER_COUUID);

	d_iov_set(&key, &ukey, sizeof(struct d_uuid));
	d_iov_set(&value, &args, sizeof(struct cont_df_args));
	uuid_clear(it_entry->ie_couuid);

	rc = dbtree_iter_fetch(co_iter->cot_hdl, &key, &value, anchor);
	if (rc != 0) {
		D_ERROR("Error while fetching co info: "DF_RC"\n", DP_RC(rc));
		return rc;
	}
	D_ASSERT(value.iov_len == sizeof(struct cont_df_args));
	uuid_copy(it_entry->ie_couuid, args.ca_cont_df->cd_id);
	it_entry->ie_child_type = VOS_ITER_OBJ;

	return rc;
}

static int
cont_iter_next(struct vos_iterator *iter, daos_anchor_t *anchor)
{
	struct cont_iterator	*co_iter = vos_iter2co_iter(iter);

	D_ASSERT(iter->it_type == VOS_ITER_COUUID);
	return dbtree_iter_next(co_iter->cot_hdl);
}

static int
cont_iter_probe(struct vos_iterator *iter, daos_anchor_t *anchor, uint32_t flags)
{
	struct cont_iterator	*co_iter = vos_iter2co_iter(iter);
	dbtree_probe_opc_t	next_opc;
	dbtree_probe_opc_t	opc;

	D_ASSERT(iter->it_type == VOS_ITER_COUUID);

	next_opc = (flags & VOS_ITER_PROBE_NEXT) ? BTR_PROBE_GT : BTR_PROBE_GE;
	opc = vos_anchor_is_zero(anchor) ? BTR_PROBE_FIRST : next_opc;
	/* The container tree will not be affected by the iterator intent,
	 * just set it as DAOS_INTENT_DEFAULT.
	 */
	return dbtree_iter_probe(co_iter->cot_hdl, opc, DAOS_INTENT_DEFAULT,
				 NULL, anchor);
}

static int
cont_iter_process(struct vos_iterator *iter, vos_iter_proc_op_t op, void *args)
{
	D_ASSERT(iter->it_type == VOS_ITER_COUUID);

	return -DER_NO_PERM;
}

struct vos_iter_ops vos_cont_iter_ops = {
	.iop_prepare = cont_iter_prep,
	.iop_finish  = cont_iter_fini,
	.iop_probe   = cont_iter_probe,
	.iop_next    = cont_iter_next,
	.iop_fetch   = cont_iter_fetch,
	.iop_process  = cont_iter_process,
};
