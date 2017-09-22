/*
 * Copyright (c) 2009, Sun Microsystems, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of Sun Microsystems, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * clnt_tcp.c, Implements a TCP/IP based, client side RPC.
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 *
 * TCP based RPC supports 'batched calls'.
 * A sequence of calls may be batched-up in a send buffer.  The rpc call
 * return immediately to the client even though the call was not necessarily
 * sent.  The batching occurs if the results' xdr routine is NULL (0) AND
 * the rpc timeout value is zero (see clnt.h, rpc).
 *
 * Clients should NOT casually batch calls that in fact return results; that is,
 * the server side should be aware that a call is batched and not produce any
 * return message.  Batched calls that produce many result messages can
 * deadlock (netlock) the client and the server....
 *
 * Now go hang yourself.  [Ouch, that was intemperate.]
 */
#include <config.h>

#include <sys/types.h>
#include <sys/poll.h>
#include <sys/syslog.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <misc/socket.h>
#include <arpa/inet.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <rpc/types.h>
#include <misc/portable.h>
#include <reentrant.h>
#include <rpc/rpc.h>
#include "rpc_com.h"
#include <rpc/svc_rqst.h>
#include <rpc/xdr_ioq.h>
#include "svc_ioq.h"

static struct clnt_ops *clnt_vc_ops(void);

#include "clnt_internal.h"
#include "svc_internal.h"

/*
 *      This machinery implements per-fd locks for MT-safety.  It is not
 *      sufficient to do per-CLIENT handle locks for MT-safety because a
 *      user may create more than one CLIENT handle with the same fd behind
 *      it.  Therfore, we allocate an array of flags (vc_fd_locks), protected
 *      by the clnt_fd_lock mutex, and an array (vc_cv) of condition variables
 *      similarly protected.  Vc_fd_lock[fd] == 1 => a call is active on some
 *      CLIENT handle created for that fd.  (Historical interest only.)
 *
 *      The current implementation holds locks across the entire RPC and reply.
 *      Yes, this is silly, and as soon as this code is proven to work, this
 *      should be the first thing fixed.  One step at a time.  (Fixing this.)
 *
 *      The ONC RPC record marking (RM) standard (RFC 5531, s. 11) does not
 *      provide for mixed call and reply fragment reassembly, so writes to the
 *      bytestream with MUST be record-wise atomic.  It appears that an
 *      implementation may interleave call and reply messages on distinct
 *      conversations.  This is not incompatible with holding a transport
 *      exclusive locked across a full call and reply, bud does require new
 *      control tranfers and delayed decoding support in the transport.  For
 *      duplex channels, full coordination is required between client and
 *      server tranpsorts sharing an underlying bytestream (Matt).
 */

/*
 * Create a client handle for a connection.
 * Default options are set, which the user can change using clnt_control()'s.
 * The rpc/vc package does buffering similar to stdio, so the client
 * must pick send and receive buffer sizes, 0 => use the default.
 * NB: fd is copied into a private area.
 * NB: The rpch->cl_auth is set null authentication. Caller may wish to
 * set this something more useful.
 *
 * fd should be an open socket
 */
CLIENT *
clnt_vc_ncreatef(const int fd,	/* open file descriptor */
		 const struct netbuf *raddr,	/* servers address */
		 const rpcprog_t prog,	/* program number */
		 const rpcvers_t vers,	/* version number */
		 const u_int sendsz,	/* buffer send size */
		 const u_int recvsz,	/* buffer recv size */
		 const uint32_t flags)
{
	CLIENT *clnt = NULL;
	SVCXPRT *xprt = NULL;
	struct cx_data *cx = NULL;
	struct ct_data *cs;
	struct svc_vc_xprt *xd;
	struct rpc_msg call_msg;
	sigset_t mask, newmask;
	struct sockaddr_storage ss;
	XDR ct_xdrs[1];		/* temp XDR stream */
	socklen_t slen;

	sigfillset(&newmask);
	thr_sigsetmask(SIG_SETMASK, &newmask, &mask);

	if (flags & CLNT_CREATE_FLAG_CONNECT) {
		slen = sizeof(ss);
		if (getpeername(fd, (struct sockaddr *)&ss, &slen) < 0) {
			if (errno != ENOTCONN) {
				rpc_createerr.cf_stat = RPC_SYSTEMERROR;
				rpc_createerr.cf_error.re_errno = errno;
				goto err;
			}
			if (connect
			    (fd, (struct sockaddr *)raddr->buf,
			     raddr->len) < 0) {
				rpc_createerr.cf_stat = RPC_SYSTEMERROR;
				rpc_createerr.cf_error.re_errno = errno;
				goto err;
			}
			__warnx(TIRPC_DEBUG_FLAG_CLNT_VC,
				"%s: fd %d connected",
				__func__, fd);
		}
	}

	/* find or create shared fd state; ref+1 */
	xprt = svc_fd_ncreatef(fd, sendsz, recvsz, flags);
	if (!xprt) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d svc_fd_ncreatef failed",
			__func__, fd);
		goto err;
	}
	xd = VC_DR(REC_XPRT(xprt));

	/* buffer sizes should match svc side */
	cx = alloc_cx_data(CX_VC_DATA, xd->sx_dr.sendsz, xd->sx_dr.recvsz);
	cx->cx_rec = &xd->sx_dr;
	cs = CT_DATA(cx);

	if (sizeof(struct sockaddr_storage) < raddr->len) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d called with invalid address length"
			" (max %z < %u len)",
			__func__, fd,
			sizeof(struct sockaddr_storage),
			raddr->len);
		goto err;
	}
	memcpy(&cs->ct_raddr, raddr->buf, raddr->len);
	cs->ct_rlen = raddr->len;

	/*
	 * initialize call message
	 */
	call_msg.rm_xid = 1;
	call_msg.rm_direction = CALL;
	call_msg.rm_call.cb_rpcvers = RPC_MSG_VERSION;
	call_msg.cb_prog = prog;
	call_msg.cb_vers = vers;

	/*
	 * pre-serialize the static part of the call msg and stash it away
	 */
	xdrmem_create(ct_xdrs, cs->ct_u.ct_mcallc, MCALL_MSG_SIZE, XDR_ENCODE);
	if (!xdr_callhdr(ct_xdrs, &call_msg)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d xdr_callhdr failed",
			__func__, fd);
		goto err;
	}
	cs->ct_mpos = XDR_GETPOS(ct_xdrs);
	XDR_DESTROY(ct_xdrs);

	clnt = &cx->cx_c;
	clnt->cl_ops = clnt_vc_ops();

	__warnx(TIRPC_DEBUG_FLAG_CLNT_VC,
		"%s: fd %d completed",
		__func__, fd);
	thr_sigsetmask(SIG_SETMASK, &(mask), NULL);
	return (clnt);

 err:
	if (cx) {
		free_cx_data(cx);
	}

	if (xprt) {
		SVC_RELEASE(xprt, SVC_RELEASE_FLAG_NONE);
	}

	thr_sigsetmask(SIG_SETMASK, &(mask), NULL);
	return (NULL);
}

/*
 * Create an RPC client handle from an active service transport
 * handle, i.e., to issue calls on the channel.
 */
CLIENT *
clnt_vc_ncreate_svc(const SVCXPRT *xprt, const rpcprog_t prog,
		    const rpcvers_t vers, const uint32_t flags)
{
	struct svc_vc_xprt *xd = VC_DR(REC_XPRT(xprt));

	return clnt_vc_ncreatef(xprt->xp_fd, &xprt->xp_remote.nb, prog, vers,
				xd->sx_dr.sendsz, xd->sx_dr.recvsz,
				flags | CLNT_CREATE_FLAG_SVCXPRT);
}

static enum xprt_stat
clnt_vc_process(struct svc_req *req)
{
	SVCXPRT *xprt = req->rq_xprt;

	__warnx(TIRPC_DEBUG_FLAG_WARN,
		"%s: %p fd %d unexpected CALL",
		__func__, xprt, xprt->xp_fd);
	return SVC_STAT(xprt);
}

static enum clnt_stat
clnt_vc_call(CLIENT *clnt, AUTH *auth, rpcproc_t proc,
	     xdrproc_t xdr_args, void *args_ptr,
	     xdrproc_t xdr_results, void *results_ptr,
	     struct timeval timeout)
{
	struct cx_data *cx = CX_DATA(clnt);
	struct ct_data *cs = CT_DATA(cx);
	struct rpc_dplx_rec *rec = cx->cx_rec;
	SVCXPRT *xprt = &rec->xprt;
	struct xdr_ioq *xioq;
	XDR *xdrs;
	struct clnt_req *ctx;
	enum clnt_stat result;
	int code;

	/* Create a call context.  A lot of TI-RPC decisions need to be
	 * looked at, including:
	 *
	 * 1. the client has a serialized call.  This looks harmless, so long
	 * as the xid is adjusted.
	 *
	 * 2. the last xid used is now saved in handle shared private
	 * data.  There's no more reason to use the old time-dependent
	 * xid logic.  It should be preferable to count atomically from 1.
	 *
	 * 3. the server has the XDR structure.  There is only one
	 * physical byte stream.  The main issue that will arise is the
	 * need to transition the stream between calls.  We'll keep the
	 * call parameters, control transfer machinery, etc, in clnt_req.
	 */
	ctx = clnt_req_alloc(clnt, timeout);
	if (!ctx)
		return (RPC_TLIERROR);

	ctx->cc_auth = auth;
	ctx->cc_xdr.proc = xdr_results;
	ctx->cc_xdr.where = results_ptr;

 call_again:
	/* XXX Until gss_get_mic and gss_wrap can be replaced with
	 * iov equivalents, replies with RPCSEC_GSS security must be
	 * encoded in a contiguous buffer.
	 *
	 * Nb, we should probably use getpagesize() on Unix.  Need
	 * an equivalent for Windows.
	 */
	xioq = xdr_ioq_create(RPC_MAXDATA_DEFAULT,
			      __svc_params->ioq.send_max + RPC_MAXDATA_DEFAULT,
			      (auth->ah_cred.oa_flavor == RPCSEC_GSS)
			      ? UIO_FLAG_REALLOC | UIO_FLAG_FREE
			      : UIO_FLAG_FREE);

	xdrs = xioq->xdrs;
	ctx->error.re_status = RPC_SUCCESS;

	/* Need lock for ct_mcallc.
	 */
	mutex_lock(&clnt->cl_lock);
	cs->ct_u.ct_mcalli = ntohl(ctx->xid);

	if ((!XDR_PUTBYTES(xdrs, cs->ct_u.ct_mcallc, cs->ct_mpos))
	    || (!XDR_PUTINT32(xdrs, (int32_t *) &proc))
	    || (!AUTH_MARSHALL(auth, xdrs))
	    || (!AUTH_WRAP(auth, xdrs, xdr_args, args_ptr))) {
		/* error case */
		mutex_unlock(&clnt->cl_lock);
		__warnx(TIRPC_DEBUG_FLAG_CLNT_VC,
			"%s: fd %d failed @ %s:%d",
			__func__, xprt->xp_fd, __func__, __LINE__);
		clnt_req_release(ctx);
		return (RPC_CANTENCODEARGS);
	}
	mutex_unlock(&clnt->cl_lock);

	xdrs->x_lib[1] = (void *)xprt;
	svc_ioq_write_submit(xprt, xioq);

	/* reply */
	if (!rec->ev_p) {
		xprt->xp_dispatch.process_cb = clnt_vc_process;
		svc_rqst_evchan_reg(__svc_params->ev_u.evchan.id, xprt,
				    SVC_RQST_FLAG_CHAN_AFFINITY);
	}
	code = clnt_req_wait_reply(ctx);

	if (ctx->refreshes > 0) {
		ctx->flags = CLNT_REQ_FLAG_NONE;
		goto call_again;
	}
	if (code == ETIMEDOUT) {
		/* UL can retry, we dont.  This CAN indicate xprt
		 * destroyed (error status already set). */
		__warnx(TIRPC_DEBUG_FLAG_CLNT_VC,
			"%s: fd %d ETIMEDOUT",
			__func__, xprt->xp_fd);
	}

	result = ctx->error.re_status;
	clnt_req_release(ctx);
	__warnx(TIRPC_DEBUG_FLAG_CLNT_VC,
		"%s: fd %d result=%d",
		__func__, xprt->xp_fd, result);
	return (result);
}

static void
clnt_vc_geterr(CLIENT *clnt, struct rpc_err *errp)
{
	struct cx_data *cx = CX_DATA(clnt);
	XDR *xdrs = cx->cx_rec->ioq.xdrs;

	if (xdrs->x_lib[0]) {
		struct clnt_req *ctx = (struct clnt_req *) xdrs->x_lib[0];
		*errp = ctx->error;
	} else {
		/* always zero */
		*errp = CX_DATA(clnt)->cx_error;
	}
}

static bool
clnt_vc_freeres(CLIENT *clnt, xdrproc_t xdr_res, void *res_ptr)
{
	return (xdr_free(xdr_res, res_ptr));
}

 /*ARGSUSED*/
static void
clnt_vc_abort(CLIENT *clnt)
{
}

static bool
clnt_vc_control(CLIENT *clnt, u_int request, void *info)
{
	struct cx_data *cx = CX_DATA(clnt);
	struct ct_data *cs = CT_DATA(cx);
	struct rpc_dplx_rec *rec = cx->cx_rec;
	struct netbuf *addr;
	bool rslt = true;

	/* always take recv lock first if taking together */
	rpc_dplx_rli(rec);
	mutex_lock(&clnt->cl_lock);

	/*
	 * By default, SVC_XPRT_FLAG_CLOSE is always false. It is user
	 * responsibility to do a close on the fd, or set by cl_control.
	 */
	switch (request) {
	case CLSET_FD_CLOSE:
		(void)atomic_set_uint16_t_bits(&rec->xprt.xp_flags,
						SVC_XPRT_FLAG_CLOSE);
		rslt = true;
		goto unlock;
	case CLSET_FD_NCLOSE:
		(void)atomic_clear_uint16_t_bits(&rec->xprt.xp_flags,
						SVC_XPRT_FLAG_CLOSE);
		rslt = true;
		goto unlock;
	default:
		break;
	}

	/* for other requests which use info */
	if (info == NULL) {
		rslt = false;
		goto unlock;
	}
	switch (request) {
	case CLGET_SERVER_ADDR:
		/* Now obsolete. Only for backward compatibility */
		(void)memcpy(info, &cs->ct_raddr, (size_t) cs->ct_rlen);
		break;
	case CLGET_FD:
		*(int *)info = rec->xprt.xp_fd;
		break;
	case CLGET_SVC_ADDR:
		/* The caller should not free this memory area */
		addr = (struct netbuf *)info;
		addr->buf = &cs->ct_raddr;
		addr->len = cs->ct_rlen;
		addr->maxlen = sizeof(cs->ct_raddr);
		break;
	case CLSET_SVC_ADDR:	/* set to new address */
		rslt = false;
		goto unlock;
	case CLGET_XID:
		/*
		 * use the knowledge that xid is the
		 * first element in the call structure
		 * This will get the xid of the PREVIOUS call
		 */
		*(u_int32_t *) info =
		    ntohl(*(u_int32_t *) (void *)&cs->ct_u.ct_mcalli);
		break;
	case CLSET_XID:
		/* This will set the xid of the NEXT call */
		*(u_int32_t *) (void *)&cs->ct_u.ct_mcalli =
		    htonl(*((u_int32_t *) info) + 1);
		/* increment by 1 as clnt_vc_call() decrements once */
		break;
	case CLGET_VERS:
		/*
		 * This RELIES on the information that, in the call body,
		 * the version number field is the fifth field from the
		 * begining of the RPC header. MUST be changed if the
		 * call_struct is changed
		 */
		{
			u_int32_t *tmp =
			    (u_int32_t *) (cs->ct_u.ct_mcallc +
					   4 * BYTES_PER_XDR_UNIT);
			*(u_int32_t *) info = ntohl(*tmp);
		}
		break;

	case CLSET_VERS:
		{
			u_int32_t tmp = htonl(*(u_int32_t *) info);
			*(cs->ct_u.ct_mcallc + 4 * BYTES_PER_XDR_UNIT) = tmp;
		}
		break;

	case CLGET_PROG:
		/*
		 * This RELIES on the information that, in the call body,
		 * the program number field is the fourth field from the
		 * begining of the RPC header. MUST be changed if the
		 * call_struct is changed
		 */
		{
			u_int32_t *tmp =
			    (u_int32_t *) (cs->ct_u.ct_mcallc +
					   3 * BYTES_PER_XDR_UNIT);
			*(u_int32_t *) info = ntohl(*tmp);
		}
		break;

	case CLSET_PROG:
		{
			u_int32_t tmp = htonl(*(u_int32_t *) info);
			*(cs->ct_u.ct_mcallc + 3 * BYTES_PER_XDR_UNIT) = tmp;
		}
		break;

	default:
		rslt = false;
		goto unlock;
		break;
	}

 unlock:
	rpc_dplx_rui(rec);
	mutex_unlock(&clnt->cl_lock);

	return (rslt);
}

static bool
clnt_vc_ref(CLIENT *clnt, u_int flags)
{
	uint32_t refcnt;

	if (!(flags & CLNT_REF_FLAG_LOCKED))
		mutex_lock(&clnt->cl_lock);

	if (clnt->cl_flags & CLNT_FLAG_DESTROYED) {
		mutex_unlock(&clnt->cl_lock);
		return (false);
	}
	refcnt = ++(clnt->cl_refcnt);
	mutex_unlock(&clnt->cl_lock);

	__warnx(TIRPC_DEBUG_FLAG_REFCNT, "%s: postref %p %u", __func__, clnt,
		refcnt);

	return (true);
}

static void
clnt_vc_release(CLIENT *clnt, u_int flags)
{
	uint32_t cl_refcnt;

	if (!(flags & CLNT_RELEASE_FLAG_LOCKED))
		mutex_lock(&clnt->cl_lock);

	cl_refcnt = --(clnt->cl_refcnt);

	__warnx(TIRPC_DEBUG_FLAG_REFCNT, "%s: postunref %p cl_refcnt %u",
		__func__, clnt, cl_refcnt);

	/* conditional destroy */
	if ((clnt->cl_flags & CLNT_FLAG_DESTROYED) && (cl_refcnt == 0)) {
		struct cx_data *cx = CX_DATA(clnt);

		mutex_unlock(&clnt->cl_lock);

		/* client handles are now freed directly */
		SVC_RELEASE(&cx->cx_rec->xprt, SVC_RELEASE_FLAG_NONE);
		free_cx_data(cx);
	} else
		mutex_unlock(&clnt->cl_lock);
}

static void
clnt_vc_destroy(CLIENT *clnt)
{
	uint32_t cl_refcnt;

	mutex_lock(&clnt->cl_lock);
	if (clnt->cl_flags & CLNT_FLAG_DESTROYED) {
		mutex_unlock(&clnt->cl_lock);
		return;
	}

	clnt->cl_flags |= CLNT_FLAG_DESTROYED;
	cl_refcnt = --(clnt->cl_refcnt);
	mutex_unlock(&clnt->cl_lock);

	__warnx(TIRPC_DEBUG_FLAG_REFCNT, "%s: cl_destroy %p cl_refcnt %u",
		__func__, clnt, cl_refcnt);

	/* conditional destroy */
	if (cl_refcnt == 0) {
		struct cx_data *cx = CX_DATA(clnt);

		/* client handles are now freed directly */
		SVC_RELEASE(&cx->cx_rec->xprt, SVC_RELEASE_FLAG_NONE);
		free_cx_data(cx);
	}
}

static struct clnt_ops *
clnt_vc_ops(void)
{
	static struct clnt_ops ops;
	extern mutex_t ops_lock;
	sigset_t mask, newmask;

	/* VARIABLES PROTECTED BY ops_lock: ops */

	sigfillset(&newmask);
	thr_sigsetmask(SIG_SETMASK, &newmask, &mask);
	mutex_lock(&ops_lock);
	if (ops.cl_call == NULL) {
		ops.cl_call = clnt_vc_call;
		ops.cl_abort = clnt_vc_abort;
		ops.cl_geterr = clnt_vc_geterr;
		ops.cl_freeres = clnt_vc_freeres;
		ops.cl_ref = clnt_vc_ref;
		ops.cl_release = clnt_vc_release;
		ops.cl_destroy = clnt_vc_destroy;
		ops.cl_control = clnt_vc_control;
	}
	mutex_unlock(&ops_lock);
	thr_sigsetmask(SIG_SETMASK, &(mask), NULL);
	return (&ops);
}
