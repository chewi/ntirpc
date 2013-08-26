/*
 * Copyright (c) 2013 Linux Box Corporation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR `AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/poll.h>

#include <sys/un.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <misc/timespec.h>

#include <rpc/types.h>
#include <misc/portable.h>
#include <rpc/rpc.h>
#include <rpc/svc.h>
#include <rpc/svc_auth.h>

#include <intrinsic.h>
#include "rpc_com.h"
#include "clnt_internal.h"
#include "svc_internal.h"
#include "svc_xprt.h"
#include "rpc_dplx_internal.h"
#include "rpc_ctx.h"
#include <rpc/svc_rqst.h>
#include <rpc/xdr_inrec.h>
#include <rpc/xdr_ioq.h>
#include <getpeereid.h>
#include <misc/thrdpool.h>
#include "svc_ioq.h"

static struct thrdpool pool;

void svc_ioq_init() 
{
    struct thrdpool_params params = {
        .thrd_max = 200,
        .thrd_min = 2
    };

    (void) thrdpool_init(&pool, "svc_ioq", &params);
}

static inline void
cfconn_set_dead(SVCXPRT *xprt, struct x_vc_data *xd)
{
    mutex_lock(&xprt->xp_lock);
    xd->sx.strm_stat = XPRT_DIED;
    mutex_unlock(&xprt->xp_lock);
}

#define VREC_NIOVS 12
#define LAST_FRAG ((u_int32_t)(1 << 31))

static inline void
ioq_flushv(SVCXPRT *xprt, struct x_vc_data *xd, struct xdr_ioq *xioq)
{
    struct iovec *iov, *tiov;
    ssize_t nbytes = 0, resid = xioq->ioq.frag_len + sizeof(u_int32_t);
    struct v_rec *vrec = NULL;
    u_int32_t frag_header;
    int iovcnt, ix;

    frag_header =
        htonl((u_int32_t)(xioq->ioq.frag_len | LAST_FRAG));

    iov = alloca(xioq->ioq.size * sizeof(struct iovec));
    iov[0].iov_base = &(frag_header);
    iov[0].iov_len = sizeof(u_int32_t);

    ix = 1;
    TAILQ_FOREACH(vrec, &(xioq->ioq.q), ioq) {
        tiov = iov+ix;
        tiov->iov_base = vrec->base;
        tiov->iov_len = vrec->len;
        ix++;
    }
    
    iovcnt = ix;
    while (resid > 0) {
        /* advance iov */
        for (ix = 0, tiov = iov; ((nbytes > 0) && (ix < iovcnt)); ++ix) {
            tiov = iov+ix;
            if (tiov->iov_len > nbytes) {
                tiov->iov_base += nbytes;
                tiov->iov_len -= nbytes;
            } else {
                nbytes -= tiov->iov_len;
                iovcnt--;
                continue;
            }
        }

        /* blocking write */
        nbytes = writev(xprt->xp_fd, iov, iovcnt);
        if (unlikely(nbytes < 0)) {
            __warnx(TIRPC_DEBUG_FLAG_SVC_VC, "writev failed %d\n",
                    __func__, errno);
            cfconn_set_dead(xprt, xd);
            return;
        }
        resid -= nbytes;
    }
}

void svc_ioq(struct thrd_context *thr_ctx)
{
    struct svc_ioq_args *arg = (struct svc_ioq_args *) thr_ctx->arg;
    SVCXPRT *xprt = arg->xprt;
    struct x_vc_data *xd = arg->xd;
    struct xdr_ioq *xioq = NULL;

    for (;;) {
        mutex_lock(&xprt->xp_lock);
        if (! xd->shared.ioq.size) {
            xd->shared.ioq.active = false;
            SVC_RELEASE(xprt, SVC_RELEASE_FLAG_LOCKED);
            goto out;
        }
        xioq = TAILQ_FIRST(&xd->shared.ioq.q);
        TAILQ_REMOVE(&xd->shared.ioq.q, xioq, ioq_s);
        (xd->shared.ioq.size)--;
        /* do i/o unlocked */
        mutex_unlock(&xprt->xp_lock);
        ioq_flushv(xprt, xd, xioq);
        XDR_DESTROY(xioq->xdrs);
    }

out:
    return;
}

void
svc_ioq_append(SVCXPRT *xprt, struct x_vc_data *xd, XDR *xdrs)
{
    struct xdr_ioq *xioq = xdrs->x_private;

    mutex_lock(&xprt->xp_lock);
    TAILQ_INSERT_TAIL(&xd->shared.ioq.q, xioq, ioq_s);
    (xd->shared.ioq.size)++;
    if (! xd->shared.ioq.active) {
        struct svc_ioq_args *arg =
            mem_alloc(sizeof(struct svc_ioq_args));
        arg->xprt = xprt;
        arg->xd = xd;
        xd->shared.ioq.active = true;
        thrdpool_submit_work(&pool, svc_ioq, arg);
        SVC_REF(xprt, SVC_REF_FLAG_LOCKED);
    }
    else
        mutex_unlock(&xprt->xp_lock);
}

void
svc_ioq_shutdown()
{
    /* XXX */
}
