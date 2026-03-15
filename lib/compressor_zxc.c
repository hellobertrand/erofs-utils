// SPDX-License-Identifier: GPL-2.0+ OR Apache-2.0
/*
 * ZXC compressor for erofs-utils
 *
 * Copyright (C) 2026, Bertrand Lebonnois
 */
#include "erofs/internal.h"
#include "erofs/print.h"
#include "erofs/config.h"
#include <zxc_buffer.h>
#include <zxc_constants.h>
#include <zxc_error.h>
#include <stdlib.h>
#include "compressor.h"
#include "erofs/atomic.h"

struct erofs_zxc_context {
	zxc_cctx *cctx;
	u8 *fitblk_buffer;
	unsigned int fitblk_bufsiz;
};

static int zxc_compress_block(const struct erofs_compress *c,
			      const void *src, unsigned int srcsize,
			      void *dst, unsigned int dstcapacity)
{
	struct erofs_zxc_context *ctx = c->private_data;
	zxc_compress_opts_t opts = {
		.level = c->compression_level,
		.checksum_enabled = 0,
	};
	int64_t csize;

	csize = zxc_compress_cctx(ctx->cctx, src, srcsize,
				  dst, dstcapacity, &opts);
	if (csize < 0) {
		if (csize == ZXC_ERROR_DST_TOO_SMALL)
			return -ENOSPC;
		erofs_err("ZXC compress failed: %s", zxc_error_name(csize));
		return -EFAULT;
	}
	return (int)csize;
}

static int zxc_compress_destsize(const struct erofs_compress *c,
				 const void *src, unsigned int *srcsize,
				 void *dst, unsigned int dstsize)
{
	struct erofs_zxc_context *ctx = c->private_data;
	zxc_compress_opts_t opts = {
		.level = c->compression_level,
		.checksum_enabled = 0,
	};
	size_t l = 0;		/* largest input that fits so far */
	int64_t l_csize = 0;
	size_t r = *srcsize + 1; /* smallest input that doesn't fit so far */
	size_t m;

	if (dstsize + 32 > ctx->fitblk_bufsiz) {
		u8 *buf = realloc(ctx->fitblk_buffer, dstsize + 32);

		if (!buf)
			return -ENOMEM;
		ctx->fitblk_bufsiz = dstsize + 32;
		ctx->fitblk_buffer = buf;
	}

	m = dstsize * 4;
	for (;;) {
		int64_t csize;

		m = max(m, l + 1);
		m = min(m, r - 1);

		csize = zxc_compress_cctx(ctx->cctx, src, m,
					  ctx->fitblk_buffer,
					  dstsize + 32, &opts);
		if (csize < 0) {
			if (csize == ZXC_ERROR_DST_TOO_SMALL)
				goto doesnt_fit;
			return -EFAULT;
		}

		if (csize > 0 && (size_t)csize <= dstsize) {
			/* Fits */
			memcpy(dst, ctx->fitblk_buffer, csize);
			l = m;
			l_csize = csize;
			if (r <= l + 1 || csize + 1 >= (int64_t)dstsize)
				break;
			/*
			 * Estimate needed input prefix size based on current
			 * compression ratio.
			 */
			m = (dstsize * m) / csize;
		} else {
doesnt_fit:
			/* Doesn't fit */
			r = m;
			if (r <= l + 1)
				break;
			m = (l + r) / 2;
		}
	}
	*srcsize = l;
	return l_csize;
}

static int compressor_zxc_exit(struct erofs_compress *c)
{
	struct erofs_zxc_context *ctx = c->private_data;

	if (!ctx)
		return -EINVAL;

	free(ctx->fitblk_buffer);
	zxc_free_cctx(ctx->cctx);
	free(ctx);
	return 0;
}

static int erofs_compressor_zxc_setlevel(struct erofs_compress *c,
					 int compression_level)
{
	if (compression_level < 0)
		compression_level = erofs_compressor_zxc.default_level;

	if (compression_level > erofs_compressor_zxc.best_level) {
		erofs_err("invalid ZXC compression level %d",
			  compression_level);
		return -EINVAL;
	}
	c->compression_level = compression_level;
	return 0;
}

static int compressor_zxc_init(struct erofs_compress *c)
{
	struct erofs_zxc_context *ctx = c->private_data;
	static erofs_atomic_bool_t __warnonce;
	zxc_cctx *cctx;
	zxc_compress_opts_t opts = {
		.level = c->compression_level,
		.checksum_enabled = 0,
	};

	if (ctx) {
		zxc_free_cctx(ctx->cctx);
		ctx->cctx = NULL;
		c->private_data = NULL;
	} else {
		ctx = calloc(1, sizeof(*ctx));
		if (!ctx)
			return -ENOMEM;
	}

	cctx = zxc_create_cctx(&opts);
	if (!cctx) {
		free(ctx);
		return -ENOMEM;
	}

	ctx->cctx = cctx;
	c->private_data = ctx;

	if (!erofs_atomic_test_and_set(&__warnonce)) {
		erofs_warn("EXPERIMENTAL ZXC compressor in use. "
			   "The fitblk binary-search approach is used "
			   "for compress_destsize.");
	}
	return 0;
}

const struct erofs_compressor erofs_compressor_zxc = {
	.default_level = ZXC_LEVEL_DEFAULT,
	.best_level = ZXC_LEVEL_COMPACT,
	.init = compressor_zxc_init,
	.exit = compressor_zxc_exit,
	.setlevel = erofs_compressor_zxc_setlevel,
	.compress = zxc_compress_block,
	.compress_destsize = zxc_compress_destsize,
};
