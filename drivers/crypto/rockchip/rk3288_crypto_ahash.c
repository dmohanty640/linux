// SPDX-License-Identifier: GPL-2.0-only
/*
 * Crypto acceleration support for Rockchip RK3288
 *
 * Copyright (c) 2015, Fuzhou Rockchip Electronics Co., Ltd
 *
 * Author: Zain Wang <zain.wang@rock-chips.com>
 *
 * Some ideas are from marvell/cesa.c and s5p-sss.c driver.
 */

#include <linux/unaligned.h>
#include <crypto/internal/hash.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include "rk3288_crypto.h"

/*
 * IC can not process zero message hash,
 * so we put the fixed hash out when met zero message.
 */

static bool rk_ahash_need_fallback(struct ahash_request *req)
{
	struct scatterlist *sg;

	sg = req->src;
	while (sg) {
		if (!IS_ALIGNED(sg->offset, sizeof(u32))) {
			return true;
		}
		if (sg->length % 4) {
			return true;
		}
		sg = sg_next(sg);
	}
	return false;
}

static int rk_ahash_digest_fb(struct ahash_request *areq)
{
	struct rk_ahash_rctx *rctx = ahash_request_ctx(areq);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(areq);
	struct rk_ahash_ctx *tfmctx = crypto_ahash_ctx(tfm);
	struct ahash_alg *alg = crypto_ahash_alg(tfm);
	struct rk_crypto_tmp *algt = container_of(alg, struct rk_crypto_tmp, alg.hash.base);

	algt->stat_fb++;

	ahash_request_set_tfm(&rctx->fallback_req, tfmctx->fallback_tfm);
	ahash_request_set_callback(&rctx->fallback_req,
				   areq->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP,
				   areq->base.complete, areq->base.data);
	ahash_request_set_crypt(&rctx->fallback_req, areq->src, areq->result,
				areq->nbytes);

	return crypto_ahash_digest(&rctx->fallback_req);
}

static int zero_message_process(struct ahash_request *req)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	int rk_digest_size = crypto_ahash_digestsize(tfm);

	switch (rk_digest_size) {
	case SHA1_DIGEST_SIZE:
		memcpy(req->result, sha1_zero_message_hash, rk_digest_size);
		break;
	case SHA256_DIGEST_SIZE:
		memcpy(req->result, sha256_zero_message_hash, rk_digest_size);
		break;
	case MD5_DIGEST_SIZE:
		memcpy(req->result, md5_zero_message_hash, rk_digest_size);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void rk_ahash_reg_init(struct ahash_request *req,
			      struct rk_crypto_info *dev)
{
	struct rk_ahash_rctx *rctx = ahash_request_ctx(req);
	int reg_status;

	reg_status = CRYPTO_READ(dev, RK_CRYPTO_CTRL) |
		     RK_CRYPTO_HASH_FLUSH | _SBF(0xffff, 16);
	CRYPTO_WRITE(dev, RK_CRYPTO_CTRL, reg_status);

	reg_status = CRYPTO_READ(dev, RK_CRYPTO_CTRL);
	reg_status &= (~RK_CRYPTO_HASH_FLUSH);
	reg_status |= _SBF(0xffff, 16);
	CRYPTO_WRITE(dev, RK_CRYPTO_CTRL, reg_status);

	memset_io(dev->reg + RK_CRYPTO_HASH_DOUT_0, 0, 32);

	CRYPTO_WRITE(dev, RK_CRYPTO_INTENA, RK_CRYPTO_HRDMA_ERR_ENA |
					    RK_CRYPTO_HRDMA_DONE_ENA);

	CRYPTO_WRITE(dev, RK_CRYPTO_INTSTS, RK_CRYPTO_HRDMA_ERR_INT |
					    RK_CRYPTO_HRDMA_DONE_INT);

	CRYPTO_WRITE(dev, RK_CRYPTO_HASH_CTRL, rctx->mode |
					       RK_CRYPTO_HASH_SWAP_DO);

	CRYPTO_WRITE(dev, RK_CRYPTO_CONF, RK_CRYPTO_BYTESWAP_HRFIFO |
					  RK_CRYPTO_BYTESWAP_BRFIFO |
					  RK_CRYPTO_BYTESWAP_BTFIFO);

	CRYPTO_WRITE(dev, RK_CRYPTO_HASH_MSG_LEN, req->nbytes);
}

static int rk_ahash_init(struct ahash_request *req)
{
	struct rk_ahash_rctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct rk_ahash_ctx *ctx = crypto_ahash_ctx(tfm);

	ahash_request_set_tfm(&rctx->fallback_req, ctx->fallback_tfm);
	ahash_request_set_callback(&rctx->fallback_req,
				   req->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP,
				   req->base.complete, req->base.data);

	return crypto_ahash_init(&rctx->fallback_req);
}

static int rk_ahash_update(struct ahash_request *req)
{
	struct rk_ahash_rctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct rk_ahash_ctx *ctx = crypto_ahash_ctx(tfm);

	ahash_request_set_tfm(&rctx->fallback_req, ctx->fallback_tfm);
	ahash_request_set_callback(&rctx->fallback_req,
				   req->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP,
				   req->base.complete, req->base.data);
	ahash_request_set_crypt(&rctx->fallback_req, req->src, NULL, req->nbytes);

	return crypto_ahash_update(&rctx->fallback_req);
}

static int rk_ahash_final(struct ahash_request *req)
{
	struct rk_ahash_rctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct rk_ahash_ctx *ctx = crypto_ahash_ctx(tfm);

	ahash_request_set_tfm(&rctx->fallback_req, ctx->fallback_tfm);
	ahash_request_set_callback(&rctx->fallback_req,
				   req->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP,
				   req->base.complete, req->base.data);
	ahash_request_set_crypt(&rctx->fallback_req, NULL, req->result, 0);

	return crypto_ahash_final(&rctx->fallback_req);
}

static int rk_ahash_finup(struct ahash_request *req)
{
	struct rk_ahash_rctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct rk_ahash_ctx *ctx = crypto_ahash_ctx(tfm);

	ahash_request_set_tfm(&rctx->fallback_req, ctx->fallback_tfm);
	ahash_request_set_callback(&rctx->fallback_req,
				   req->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP,
				   req->base.complete, req->base.data);
	ahash_request_set_crypt(&rctx->fallback_req, req->src, req->result,
				req->nbytes);

	return crypto_ahash_finup(&rctx->fallback_req);
}

static int rk_ahash_import(struct ahash_request *req, const void *in)
{
	struct rk_ahash_rctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct rk_ahash_ctx *ctx = crypto_ahash_ctx(tfm);

	ahash_request_set_tfm(&rctx->fallback_req, ctx->fallback_tfm);
	ahash_request_set_callback(&rctx->fallback_req,
				   req->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP,
				   req->base.complete, req->base.data);

	return crypto_ahash_import(&rctx->fallback_req, in);
}

static int rk_ahash_export(struct ahash_request *req, void *out)
{
	struct rk_ahash_rctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct rk_ahash_ctx *ctx = crypto_ahash_ctx(tfm);

	ahash_request_set_tfm(&rctx->fallback_req, ctx->fallback_tfm);
	ahash_request_set_callback(&rctx->fallback_req,
				   req->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP,
				   req->base.complete, req->base.data);

	return crypto_ahash_export(&rctx->fallback_req, out);
}

static int rk_ahash_digest(struct ahash_request *req)
{
	struct rk_ahash_rctx *rctx = ahash_request_ctx(req);
	struct rk_crypto_info *dev;
	struct crypto_engine *engine;

	if (rk_ahash_need_fallback(req))
		return rk_ahash_digest_fb(req);

	if (!req->nbytes)
		return zero_message_process(req);

	dev = get_rk_crypto();

	rctx->dev = dev;
	engine = dev->engine;

	return crypto_transfer_hash_request_to_engine(engine, req);
}

static void crypto_ahash_dma_start(struct rk_crypto_info *dev, struct scatterlist *sg)
{
	CRYPTO_WRITE(dev, RK_CRYPTO_HRDMAS, sg_dma_address(sg));
	CRYPTO_WRITE(dev, RK_CRYPTO_HRDMAL, sg_dma_len(sg) / 4);
	CRYPTO_WRITE(dev, RK_CRYPTO_CTRL, RK_CRYPTO_HASH_START |
					  (RK_CRYPTO_HASH_START << 16));
}

static int rk_hash_prepare(struct crypto_engine *engine, void *breq)
{
	struct ahash_request *areq = container_of(breq, struct ahash_request, base);
	struct rk_ahash_rctx *rctx = ahash_request_ctx(areq);
	struct rk_crypto_info *rkc = rctx->dev;
	int ret;

	ret = dma_map_sg(rkc->dev, areq->src, sg_nents(areq->src), DMA_TO_DEVICE);
	if (ret <= 0)
		return -EINVAL;

	rctx->nrsg = ret;

	return 0;
}

static void rk_hash_unprepare(struct crypto_engine *engine, void *breq)
{
	struct ahash_request *areq = container_of(breq, struct ahash_request, base);
	struct rk_ahash_rctx *rctx = ahash_request_ctx(areq);
	struct rk_crypto_info *rkc = rctx->dev;

	dma_unmap_sg(rkc->dev, areq->src, rctx->nrsg, DMA_TO_DEVICE);
}

static int rk_hash_run(struct crypto_engine *engine, void *breq)
{
	struct ahash_request *areq = container_of(breq, struct ahash_request, base);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(areq);
	struct rk_ahash_rctx *rctx = ahash_request_ctx(areq);
	struct ahash_alg *alg = crypto_ahash_alg(tfm);
	struct rk_crypto_tmp *algt = container_of(alg, struct rk_crypto_tmp, alg.hash.base);
	struct scatterlist *sg = areq->src;
	struct rk_crypto_info *rkc = rctx->dev;
	int err;
	int i;
	u32 v;

	err = pm_runtime_resume_and_get(rkc->dev);
	if (err)
		return err;

	err = rk_hash_prepare(engine, breq);
	if (err)
		goto theend;

	rctx->mode = 0;

	algt->stat_req++;
	rkc->nreq++;

	switch (crypto_ahash_digestsize(tfm)) {
	case SHA1_DIGEST_SIZE:
		rctx->mode = RK_CRYPTO_HASH_SHA1;
		break;
	case SHA256_DIGEST_SIZE:
		rctx->mode = RK_CRYPTO_HASH_SHA256;
		break;
	case MD5_DIGEST_SIZE:
		rctx->mode = RK_CRYPTO_HASH_MD5;
		break;
	default:
		err =  -EINVAL;
		goto theend;
	}

	rk_ahash_reg_init(areq, rkc);

	while (sg) {
		reinit_completion(&rkc->complete);
		rkc->status = 0;
		crypto_ahash_dma_start(rkc, sg);
		wait_for_completion_interruptible_timeout(&rkc->complete,
							  msecs_to_jiffies(2000));
		if (!rkc->status) {
			dev_err(rkc->dev, "DMA timeout\n");
			err = -EFAULT;
			goto theend;
		}
		sg = sg_next(sg);
	}

	/*
	 * it will take some time to process date after last dma
	 * transmission.
	 *
	 * waiting time is relative with the last date len,
	 * so cannot set a fixed time here.
	 * 10us makes system not call here frequently wasting
	 * efficiency, and make it response quickly when dma
	 * complete.
	 */
	readl_poll_timeout(rkc->reg + RK_CRYPTO_HASH_STS, v, v == 0, 10, 1000);

	for (i = 0; i < crypto_ahash_digestsize(tfm) / 4; i++) {
		v = readl(rkc->reg + RK_CRYPTO_HASH_DOUT_0 + i * 4);
		put_unaligned_le32(v, areq->result + i * 4);
	}

theend:
	pm_runtime_put_autosuspend(rkc->dev);

	rk_hash_unprepare(engine, breq);

	local_bh_disable();
	crypto_finalize_hash_request(engine, breq, err);
	local_bh_enable();

	return 0;
}

static int rk_hash_init_tfm(struct crypto_ahash *tfm)
{
	struct rk_ahash_ctx *tctx = crypto_ahash_ctx(tfm);
	const char *alg_name = crypto_ahash_alg_name(tfm);
	struct ahash_alg *alg = crypto_ahash_alg(tfm);
	struct rk_crypto_tmp *algt = container_of(alg, struct rk_crypto_tmp, alg.hash.base);

	/* for fallback */
	tctx->fallback_tfm = crypto_alloc_ahash(alg_name, 0,
						CRYPTO_ALG_NEED_FALLBACK);
	if (IS_ERR(tctx->fallback_tfm)) {
		dev_err(algt->dev->dev, "Could not load fallback driver.\n");
		return PTR_ERR(tctx->fallback_tfm);
	}

	crypto_ahash_set_reqsize(tfm,
				 sizeof(struct rk_ahash_rctx) +
				 crypto_ahash_reqsize(tctx->fallback_tfm));

	return 0;
}

static void rk_hash_exit_tfm(struct crypto_ahash *tfm)
{
	struct rk_ahash_ctx *tctx = crypto_ahash_ctx(tfm);

	crypto_free_ahash(tctx->fallback_tfm);
}

struct rk_crypto_tmp rk_ahash_sha1 = {
	.type = CRYPTO_ALG_TYPE_AHASH,
	.alg.hash.base = {
		.init = rk_ahash_init,
		.update = rk_ahash_update,
		.final = rk_ahash_final,
		.finup = rk_ahash_finup,
		.export = rk_ahash_export,
		.import = rk_ahash_import,
		.digest = rk_ahash_digest,
		.init_tfm = rk_hash_init_tfm,
		.exit_tfm = rk_hash_exit_tfm,
		.halg = {
			 .digestsize = SHA1_DIGEST_SIZE,
			 .statesize = sizeof(struct sha1_state),
			 .base = {
				  .cra_name = "sha1",
				  .cra_driver_name = "rk-sha1",
				  .cra_priority = 300,
				  .cra_flags = CRYPTO_ALG_ASYNC |
					       CRYPTO_ALG_NEED_FALLBACK,
				  .cra_blocksize = SHA1_BLOCK_SIZE,
				  .cra_ctxsize = sizeof(struct rk_ahash_ctx),
				  .cra_module = THIS_MODULE,
			}
		}
	},
	.alg.hash.op = {
		.do_one_request = rk_hash_run,
	},
};

struct rk_crypto_tmp rk_ahash_sha256 = {
	.type = CRYPTO_ALG_TYPE_AHASH,
	.alg.hash.base = {
		.init = rk_ahash_init,
		.update = rk_ahash_update,
		.final = rk_ahash_final,
		.finup = rk_ahash_finup,
		.export = rk_ahash_export,
		.import = rk_ahash_import,
		.digest = rk_ahash_digest,
		.init_tfm = rk_hash_init_tfm,
		.exit_tfm = rk_hash_exit_tfm,
		.halg = {
			 .digestsize = SHA256_DIGEST_SIZE,
			 .statesize = sizeof(struct sha256_state),
			 .base = {
				  .cra_name = "sha256",
				  .cra_driver_name = "rk-sha256",
				  .cra_priority = 300,
				  .cra_flags = CRYPTO_ALG_ASYNC |
					       CRYPTO_ALG_NEED_FALLBACK,
				  .cra_blocksize = SHA256_BLOCK_SIZE,
				  .cra_ctxsize = sizeof(struct rk_ahash_ctx),
				  .cra_module = THIS_MODULE,
			}
		}
	},
	.alg.hash.op = {
		.do_one_request = rk_hash_run,
	},
};

struct rk_crypto_tmp rk_ahash_md5 = {
	.type = CRYPTO_ALG_TYPE_AHASH,
	.alg.hash.base = {
		.init = rk_ahash_init,
		.update = rk_ahash_update,
		.final = rk_ahash_final,
		.finup = rk_ahash_finup,
		.export = rk_ahash_export,
		.import = rk_ahash_import,
		.digest = rk_ahash_digest,
		.init_tfm = rk_hash_init_tfm,
		.exit_tfm = rk_hash_exit_tfm,
		.halg = {
			 .digestsize = MD5_DIGEST_SIZE,
			 .statesize = sizeof(struct md5_state),
			 .base = {
				  .cra_name = "md5",
				  .cra_driver_name = "rk-md5",
				  .cra_priority = 300,
				  .cra_flags = CRYPTO_ALG_ASYNC |
					       CRYPTO_ALG_NEED_FALLBACK,
				  .cra_blocksize = SHA1_BLOCK_SIZE,
				  .cra_ctxsize = sizeof(struct rk_ahash_ctx),
				  .cra_module = THIS_MODULE,
			}
		}
	},
	.alg.hash.op = {
		.do_one_request = rk_hash_run,
	},
};
