/*
 * Copyright (c) 2012 Linux Box Corporation.
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

#ifndef TIRPC_RPC_CTX_H
#define TIRPC_RPC_CTX_H

#include <misc/rbtree_x.h>
#include <misc/wait_queue.h>
#include <rpc/clnt.h>
#include <rpc/svc.h>

#define RPC_CTX_FLAG_NONE     0x0000
#define RPC_CTX_FLAG_ACKSYNC  0x0008

/*
 * RPC context.  Intended to enable efficient multiplexing of calls
 * and replies sharing a common channel.
 */
typedef struct rpc_ctx_s {
	struct opr_rbtree_node node_k;
	struct waitq_entry we;
	struct rpc_err error;
	union {
		struct {
			struct rpc_client *clnt;
			struct timespec timeout;
		} clnt;
		struct {
			/* nothing */
		} svc;
	} ctx_u;
	struct rpc_msg cc_msg;
	struct xdrpair cc_xdr;

	AUTH *cc_auth;
	int refreshes;
	uint32_t xid;
	uint32_t refcount;
	uint16_t flags;
} rpc_ctx_t;
#define CTX_MSG(p) (opr_containerof((p), struct rpc_ctx_s, cc_msg))

int call_xid_cmpf(const struct opr_rbtree_node *lhs,
		  const struct opr_rbtree_node *rhs);

rpc_ctx_t *rpc_ctx_alloc(CLIENT *, struct timeval);
int rpc_ctx_wait_reply(rpc_ctx_t *);
enum xprt_stat rpc_ctx_xfer_replymsg(struct svc_req *);
void rpc_ctx_release(rpc_ctx_t *);

#endif				/* TIRPC_RPC_CTX_H */
