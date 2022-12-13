/**
 * (C) Copyright 2016-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include "dfuse_common.h"
#include "dfuse.h"

static void
dfuse_cb_write_complete(struct dfuse_event *ev)
{
	if (ev->de_ev.ev_error == 0)
		DFUSE_REPLY_WRITE(ev, ev->de_req, ev->de_len);
	else
		DFUSE_REPLY_ERR_RAW(ev, ev->de_req, ev->de_ev.ev_error);
	d_slab_release(ev->de_handle->dpi_write_slab, ev);
}

void
dfuse_cb_write(fuse_req_t req, fuse_ino_t ino, struct fuse_bufvec *bufv, off_t position,
	       struct fuse_file_info *fi)
{
	struct dfuse_obj_hdl         *oh        = (struct dfuse_obj_hdl *)fi->fh;
	struct dfuse_projection_info *fs_handle = fuse_req_userdata(req);
	const struct fuse_ctx        *fc        = fuse_req_ctx(req);
	int                           rc;
	struct dfuse_event           *ev;
	size_t                        len  = fuse_buf_size(bufv);
	struct fuse_bufvec            ibuf = FUSE_BUFVEC_INIT(len);

	DFUSE_TRA_DEBUG(oh, "%#zx-%#zx requested flags %#x pid=%d", position, position + len - 1,
			bufv->buf[0].flags, fc->pid);

	if (atomic_fetch_add_relaxed(&oh->doh_write_count, 1) == 0) {
		if (atomic_fetch_add_relaxed(&oh->doh_ie->ie_open_write_count, 1) == 0) {
			dfuse_cache_evict(oh->doh_ie);
		}
	}

	ev = d_slab_acquire(fs_handle->dpi_write_slab);
	if (ev == NULL)
		D_GOTO(err, rc = ENOMEM);

	DFUSE_TRA_UP(ev, oh, "write");

	/* Declare a bufvec on the stack and have fuse copy into it.
	 * For page size and above this will read directly into the
	 * buffer, avoiding any copying of the data.
	 */
	ibuf.buf[0].mem = ev->de_iov.iov_buf;

	rc = fuse_buf_copy(&ibuf, bufv, 0);
	if (rc != len)
		D_GOTO(err, rc = EIO);

	ev->de_iov.iov_len = len;
	ev->de_req         = req;
	ev->de_len         = len;
	ev->de_complete_cb = dfuse_cb_write_complete;

	/* Check for potentially using readahead on this file, ie_truncated
	 * will only be set if caching is enabled so only check for the one
	 * flag rather than two here
	 */
	if (oh->doh_ie->ie_truncated) {
		if (oh->doh_ie->ie_start_off == 0 && oh->doh_ie->ie_end_off == 0) {
			oh->doh_ie->ie_start_off = position;
			oh->doh_ie->ie_end_off   = position + len;
		} else {
			if (oh->doh_ie->ie_start_off > position)
				oh->doh_ie->ie_start_off = position;
			if (oh->doh_ie->ie_end_off < position + len)
				oh->doh_ie->ie_end_off = position + len;
		}
	}

	if (len + position > oh->doh_ie->ie_stat.st_size)
		oh->doh_ie->ie_stat.st_size = len + position;

	rc = dfs_write(oh->doh_dfs, oh->doh_obj, &ev->de_sgl, position, &ev->de_ev);
	if (rc != 0)
		D_GOTO(err, rc);

	/* Send a message to the async thread to wake it up and poll for events
	 */
	sem_post(&fs_handle->dpi_sem);

	/* Now ensure there are more descriptors for the next request */
	d_slab_restock(fs_handle->dpi_write_slab);

	return;

err:
	DFUSE_REPLY_ERR_RAW(oh, req, rc);
	if (ev)
		d_slab_release(fs_handle->dpi_write_slab, ev);
}
