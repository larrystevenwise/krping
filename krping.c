/*
 * Copyright (c) 2005 Ammasso, Inc. All rights reserved.
 * Copyright (c) 2006 Open Grid Computing, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include <linux/version.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/parser.h>
#include <linux/proc_fs.h>
#include <linux/inet.h>
#include <linux/list.h>
#include <linux/in.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <asm/system.h>

#include <asm/atomic.h>
#include <asm/pci.h>

#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

#include "getopt.h"

#define PFX "krping: "

static int debug = 0;
module_param(debug, int, 0);
MODULE_PARM_DESC(debug, "Debug level (0=none, 1=all)");
#define DEBUG_LOG if (debug) printk

MODULE_AUTHOR("Steve Wise");
MODULE_DESCRIPTION("RDMA ping server");
MODULE_LICENSE("Dual BSD/GPL");

enum mem_type {
	DMA = 1,
	FASTREG = 2,
	WINDOW = 3,
	MR = 4
};

static const struct krping_option krping_opts[] = {
	{"count", OPT_INT, 'C'},
	{"size", OPT_INT, 'S'},
	{"addr", OPT_STRING, 'a'},
	{"port", OPT_INT, 'p'},
	{"verbose", OPT_NOPARAM, 'v'},
	{"validate", OPT_NOPARAM, 'V'},
	{"server", OPT_NOPARAM, 's'},
	{"client", OPT_NOPARAM, 'c'},
	{"mem_mode", OPT_STRING, 'm'},
 	{"wlat", OPT_NOPARAM, 'l'},
 	{"rlat", OPT_NOPARAM, 'L'},
 	{"bw", OPT_NOPARAM, 'B'},
 	{"duplex", OPT_NOPARAM, 'd'},
 	{"txdepth", OPT_INT, 'T'},
 	{"poll", OPT_NOPARAM, 'P'},
	{NULL, 0, 0}
};

struct krping_stats {
	unsigned long long send_bytes;
	unsigned long long send_msgs;
	unsigned long long recv_bytes;
	unsigned long long recv_msgs;
	unsigned long long write_bytes;
	unsigned long long write_msgs;
	unsigned long long read_bytes;
	unsigned long long read_msgs;
};

#define htonll(x) cpu_to_be64((x))
#define ntohll(x) cpu_to_be64((x))

static DECLARE_MUTEX(krping_mutex);

/*
 * List of running krping threads.
 */
static LIST_HEAD(krping_cbs);

static struct proc_dir_entry *krping_proc;

/*
 * Invoke like this, one on each side, using the server's address on
 * the RDMA device (iw%d):
 *
 * /bin/echo server,port=9999,addr=192.168.69.142,validate > /proc/krping  
 * /bin/echo client,port=9999,addr=192.168.69.142,validate > /proc/krping  
 *
 * krping "ping/pong" loop:
 * 	client sends source rkey/addr/len
 *	server receives source rkey/add/len
 *	server rdma reads "ping" data from source
 * 	server sends "go ahead" on rdma read completion
 *	client sends sink rkey/addr/len
 * 	server receives sink rkey/addr/len
 * 	server rdma writes "pong" data to sink
 * 	server sends "go ahead" on rdma write completion
 * 	<repeat loop>
 */

/*
 * These states are used to signal events between the completion handler
 * and the main client or server thread.
 *
 * Once CONNECTED, they cycle through RDMA_READ_ADV, RDMA_WRITE_ADV, 
 * and RDMA_WRITE_COMPLETE for each ping.
 */
enum test_state {
	IDLE = 1,
	CONNECT_REQUEST,
	ADDR_RESOLVED,
	ROUTE_RESOLVED,
	CONNECTED,
	RDMA_READ_ADV,
	RDMA_READ_COMPLETE,
	RDMA_WRITE_ADV,
	RDMA_WRITE_COMPLETE,
	ERROR
};

struct krping_rdma_info {
	uint64_t buf;
	uint32_t rkey;
	uint32_t size;
};

/*
 * Default max buffer size for IO...
 */
#define RPING_BUFSIZE 128*1024
#define RPING_SQ_DEPTH 64

/*
 * Control block struct.
 */
struct krping_cb {
	int server;			/* 0 iff client */
	struct ib_cq *cq;
	struct ib_pd *pd;
	struct ib_qp *qp;

	enum mem_type mem;
	struct ib_mr *dma_mr;
	struct ib_fast_reg_page_list *page_list;
	int page_list_len;
	struct ib_send_wr fastreg_wr;
	struct ib_send_wr invalidate_wr;

	struct ib_recv_wr rq_wr;	/* recv work request record */
	struct ib_sge recv_sgl;		/* recv single SGE */
	struct krping_rdma_info recv_buf;/* malloc'd buffer */
	u64 recv_dma_addr;
	DECLARE_PCI_UNMAP_ADDR(recv_mapping)
	struct ib_mr *recv_mr;

	struct ib_send_wr sq_wr;	/* send work requrest record */
	struct ib_sge send_sgl;
	struct krping_rdma_info send_buf;/* single send buf */
	u64 send_dma_addr;
	DECLARE_PCI_UNMAP_ADDR(send_mapping)
	struct ib_mr *send_mr;

	struct ib_send_wr rdma_sq_wr;	/* rdma work request record */
	struct ib_sge rdma_sgl;		/* rdma single SGE */
	char *rdma_buf;			/* used as rdma sink */
	u64  rdma_dma_addr;
	DECLARE_PCI_UNMAP_ADDR(rdma_mapping)
	struct ib_mr *rdma_mr;

	uint32_t remote_rkey;		/* remote guys RKEY */
	uint64_t remote_addr;		/* remote guys TO */
	uint32_t remote_len;		/* remote guys LEN */

	char *start_buf;		/* rdma read src */
	u64  start_dma_addr;
	DECLARE_PCI_UNMAP_ADDR(start_mapping)
	struct ib_mr *start_mr;

	enum test_state state;		/* used for cond/signalling */
	wait_queue_head_t sem;
	struct krping_stats stats;

	uint16_t port;			/* dst port in NBO */
	uint32_t addr;			/* dst addr in NBO */
	char *addr_str;			/* dst addr string */
	int verbose;			/* verbose logging */
	int count;			/* ping count */
	int size;			/* ping data size */
	int validate;			/* validate ping data */
	int wlat;			/* run wlat test */
	int rlat;			/* run rlat test */
	int bw;				/* run bw test */
	int duplex;			/* run bw full duplex test */
	int poll;			/* poll or block for rlat test */
	int txdepth;			/* SQ depth */

	/* CM stuff */
	struct rdma_cm_id *cm_id;	/* connection on client side,*/
					/* listener on service side. */
	struct rdma_cm_id *child_cm_id;	/* connection on server side */
	struct list_head list;	
};

static int krping_cma_event_handler(struct rdma_cm_id *cma_id,
				   struct rdma_cm_event *event)
{
	int ret;
	struct krping_cb *cb = cma_id->context;

	DEBUG_LOG("cma_event type %d cma_id %p (%s)\n", event->event, cma_id,
		  (cma_id == cb->cm_id) ? "parent" : "child");

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		cb->state = ADDR_RESOLVED;
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			printk(KERN_ERR PFX "rdma_resolve_route error %d\n", 
			       ret);
			wake_up_interruptible(&cb->sem);
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		cb->state = ROUTE_RESOLVED;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		cb->state = CONNECT_REQUEST;
		cb->child_cm_id = cma_id;
		DEBUG_LOG("child cma %p\n", cb->child_cm_id);
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		DEBUG_LOG("ESTABLISHED\n");
		if (!cb->server) {
			cb->state = CONNECTED;
		}
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		printk(KERN_ERR PFX "cma event %d, error %d\n", event->event,
		       event->status);
		cb->state = ERROR;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		printk(KERN_ERR PFX "DISCONNECT EVENT...\n");
		cb->state = ERROR;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		printk(KERN_ERR PFX "cma detected device removal!!!!\n");
		break;

	default:
		printk(KERN_ERR PFX "oof bad type!\n");
		wake_up_interruptible(&cb->sem);
		break;
	}
	return 0;
}

static int server_recv(struct krping_cb *cb, struct ib_wc *wc)
{
	if (wc->byte_len != sizeof(cb->recv_buf)) {
		printk(KERN_ERR PFX "Received bogus data, size %d\n", 
		       wc->byte_len);
		return -1;
	}

	cb->remote_rkey = ntohl(cb->recv_buf.rkey);
	cb->remote_addr = ntohll(cb->recv_buf.buf);
	cb->remote_len  = ntohl(cb->recv_buf.size);
	DEBUG_LOG("Received rkey %x addr %llx len %d from peer\n",
		  cb->remote_rkey, (unsigned long long)cb->remote_addr, 
		  cb->remote_len);

	if (cb->state <= CONNECTED || cb->state == RDMA_WRITE_COMPLETE)
		cb->state = RDMA_READ_ADV;
	else
		cb->state = RDMA_WRITE_ADV;

	return 0;
}

static int client_recv(struct krping_cb *cb, struct ib_wc *wc)
{
	if (wc->byte_len != sizeof(cb->recv_buf)) {
		printk(KERN_ERR PFX "Received bogus data, size %d\n", 
		       wc->byte_len);
		return -1;
	}

	if (cb->state == RDMA_READ_ADV)
		cb->state = RDMA_WRITE_ADV;
	else
		cb->state = RDMA_WRITE_COMPLETE;

	return 0;
}

static void krping_cq_event_handler(struct ib_cq *cq, void *ctx)
{
	struct krping_cb *cb = ctx;
	struct ib_wc wc;
	struct ib_recv_wr *bad_wr;
	int ret;

	BUG_ON(cb->cq != cq);
	if (cb->state == ERROR) {
		printk(KERN_ERR PFX "cq completion in ERROR state\n");
		return;
	}
	if (!cb->wlat && !cb->rlat && !cb->bw)
		ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
	while ((ret = ib_poll_cq(cb->cq, 1, &wc)) == 1) {
		if (wc.status) {
			if (wc.status == IB_WC_WR_FLUSH_ERR) {
				DEBUG_LOG("cq flushed\n");
				continue;
			} else {
				printk(KERN_ERR PFX "cq completion failed status %d\n",
					wc.status);
				goto error;
			}
		}

		switch (wc.opcode) {
		case IB_WC_SEND:
			DEBUG_LOG("send completion\n");
			cb->stats.send_bytes += cb->send_sgl.length;
			cb->stats.send_msgs++;
			break;

		case IB_WC_RDMA_WRITE:
			DEBUG_LOG("rdma write completion\n");
			cb->stats.write_bytes += cb->rdma_sq_wr.sg_list->length;
			cb->stats.write_msgs++;
			cb->state = RDMA_WRITE_COMPLETE;
			wake_up_interruptible(&cb->sem);
			break;

		case IB_WC_RDMA_READ:
			DEBUG_LOG("rdma read completion\n");
			cb->stats.read_bytes += cb->rdma_sq_wr.sg_list->length;
			cb->stats.read_msgs++;
			cb->state = RDMA_READ_COMPLETE;
			wake_up_interruptible(&cb->sem);
			break;

		case IB_WC_RECV:
			DEBUG_LOG("recv completion\n");
			cb->stats.recv_bytes += sizeof(cb->recv_buf);
			cb->stats.recv_msgs++;
			if (cb->wlat || cb->rlat || cb->bw)
				ret = server_recv(cb, &wc);
			else
				ret = cb->server ? server_recv(cb, &wc) :
						   client_recv(cb, &wc);
			if (ret) {
				printk(KERN_ERR PFX "recv wc error: %d\n", ret);
				goto error;
			}

			ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
			if (ret) {
				printk(KERN_ERR PFX "post recv error: %d\n", 
				       ret);
				goto error;
			}
			wake_up_interruptible(&cb->sem);
			break;

		default:
			DEBUG_LOG("unknown!!!!! completion\n");
			goto error;
		}
	}
	if (ret) {
		printk(KERN_ERR PFX "poll error %d\n", ret);
		goto error;
	}
	return;
error:
	cb->state = ERROR;
	wake_up_interruptible(&cb->sem);
}

static int krping_accept(struct krping_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	DEBUG_LOG("accepting client connection request\n");

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;

	ret = rdma_accept(cb->child_cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR PFX "rdma_accept error: %d\n", ret);
		return ret;
	}

	if (!cb->wlat && !cb->rlat && !cb->bw) {
		wait_event_interruptible(cb->sem, cb->state >= CONNECTED);
		if (cb->state == ERROR) {
			printk(KERN_ERR PFX "wait for CONNECTED state %d\n", cb->state);
			return -1;
		}
	}
	return 0;
}

static void krping_setup_wr(struct krping_cb *cb)
{
	cb->recv_sgl.addr = cb->recv_dma_addr;
	cb->recv_sgl.length = sizeof cb->recv_buf;
	if (cb->mem == DMA)
		cb->recv_sgl.lkey = cb->dma_mr->lkey;
	else
		cb->recv_sgl.lkey = cb->recv_mr->lkey;
	cb->rq_wr.sg_list = &cb->recv_sgl;
	cb->rq_wr.num_sge = 1;

	cb->send_sgl.addr = cb->send_dma_addr;
	cb->send_sgl.length = sizeof cb->send_buf;
	if (cb->mem == DMA)
		cb->send_sgl.lkey = cb->dma_mr->lkey;
	else
		cb->send_sgl.lkey = cb->send_mr->lkey;

	cb->sq_wr.opcode = IB_WR_SEND;
	cb->sq_wr.send_flags = IB_SEND_SIGNALED;
	cb->sq_wr.sg_list = &cb->send_sgl;
	cb->sq_wr.num_sge = 1;

	cb->rdma_sgl.addr = cb->rdma_dma_addr;
	if (cb->mem == DMA)
		cb->rdma_sgl.lkey = cb->dma_mr->lkey;
	else
		cb->rdma_sgl.lkey = cb->rdma_mr->lkey;
	cb->rdma_sq_wr.send_flags = IB_SEND_SIGNALED;
	cb->rdma_sq_wr.sg_list = &cb->rdma_sgl;
	cb->rdma_sq_wr.num_sge = 1;

	if (cb->mem == FASTREG) {
		u64 p;
		int i;

		cb->fastreg_wr.wr.fast_reg.page_shift = PAGE_SHIFT;
		cb->fastreg_wr.wr.fast_reg.length = cb->size;
		cb->fastreg_wr.wr.fast_reg.iova_start = (u64)cb->rdma_dma_addr;
		cb->fastreg_wr.wr.fast_reg.first_byte_offset = cb->rdma_dma_addr & ~PAGE_MASK;
		cb->fastreg_wr.wr.fast_reg.page_list = cb->page_list;
		cb->fastreg_wr.wr.fast_reg.page_list_len = cb->page_list_len;
		p = (u64)((u64)cb->rdma_dma_addr & PAGE_MASK);
		printk(KERN_INFO "%s shift %u len %u iova_start %llx fbo %u page_list_len %u\n",
			__func__, 
			cb->fastreg_wr.wr.fast_reg.page_shift,
			cb->fastreg_wr.wr.fast_reg.length,
			cb->fastreg_wr.wr.fast_reg.iova_start,
			cb->fastreg_wr.wr.fast_reg.first_byte_offset,
			cb->fastreg_wr.wr.fast_reg.page_list_len);
		for (i=0; i < cb->fastreg_wr.wr.fast_reg.page_list_len; i++, p += PAGE_SIZE) {
			cb->page_list->page_list[i] = p;
			printk(KERN_INFO "page_list[%d] 0x%llx\n", i, p);
		}
	}

}

static int krping_setup_buffers(struct krping_cb *cb)
{
	int ret;
	struct ib_phys_buf buf;
	u64 iovbase;

	DEBUG_LOG(PFX "krping_setup_buffers called on cb %p\n", cb);

	cb->recv_dma_addr = dma_map_single(cb->pd->device->dma_device, 
				   &cb->recv_buf, 
				   sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, recv_mapping, cb->recv_dma_addr);
	cb->send_dma_addr = dma_map_single(cb->pd->device->dma_device, 
					   &cb->send_buf, sizeof(cb->send_buf),
					   DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, send_mapping, cb->send_dma_addr);

	if (cb->mem == DMA) {
		cb->dma_mr = ib_get_dma_mr(cb->pd, IB_ACCESS_LOCAL_WRITE|
					   IB_ACCESS_REMOTE_READ|
				           IB_ACCESS_REMOTE_WRITE);
		if (IS_ERR(cb->dma_mr)) {
			DEBUG_LOG(PFX "reg_dmamr failed\n");
			return PTR_ERR(cb->dma_mr);
		}
	} else {

		buf.addr = cb->recv_dma_addr;
		buf.size = sizeof cb->recv_buf;
		DEBUG_LOG(PFX "recv buf dma_addr %llx size %d\n", buf.addr, (int)buf.size);
		iovbase = cb->recv_dma_addr;
		cb->recv_mr = ib_reg_phys_mr(cb->pd, &buf, 1, 
					     IB_ACCESS_LOCAL_WRITE, 
					     &iovbase);

		if (IS_ERR(cb->recv_mr)) {
			DEBUG_LOG(PFX "recv_buf reg_mr failed\n");
			return PTR_ERR(cb->recv_mr);
		}

		buf.addr = cb->send_dma_addr;
		buf.size = sizeof cb->send_buf;
		DEBUG_LOG(PFX "send buf dma_addr %llx size %d\n", buf.addr, (int)buf.size);
		iovbase = cb->send_dma_addr;
		cb->send_mr = ib_reg_phys_mr(cb->pd, &buf, 1, 
					     0, &iovbase);

		if (IS_ERR(cb->send_mr)) {
			DEBUG_LOG(PFX "send_buf reg_mr failed\n");
			ib_dereg_mr(cb->recv_mr);
			return PTR_ERR(cb->send_mr);
		}
	}

	cb->rdma_buf = kmalloc(cb->size, GFP_KERNEL);
	if (!cb->rdma_buf) {
		DEBUG_LOG(PFX "rdma_buf malloc failed\n");
		ret = -ENOMEM;
		goto err1;
	}

	cb->rdma_dma_addr = dma_map_single(cb->pd->device->dma_device, 
			       cb->rdma_buf, cb->size, 
			       DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, rdma_mapping, cb->rdma_dma_addr);
	if (cb->mem != DMA) {
		if (cb->mem == FASTREG) {
			cb->page_list_len = (((cb->size - 1) & PAGE_MASK) + PAGE_SIZE) >> PAGE_SHIFT;
			cb->page_list = ib_alloc_fast_reg_page_list(cb->pd->device, cb->page_list_len);
			if (IS_ERR(cb->page_list)) {
				DEBUG_LOG(PFX "recv_buf reg_mr failed\n");
				ret = PTR_ERR(cb->recv_mr);
				goto err2;
			}
			printk("%s page_list %p page_list_len %u\n", __func__, cb->page_list, 
				cb->page_list_len);
#if 0
			cb->rdma_mr = ib_alloc_fast_reg_mr(cb->pd, cb->page_list->max_page_list_len);
#else
			buf.addr = cb->rdma_dma_addr;
			buf.size = cb->size;
			DEBUG_LOG(PFX "rdma buf dma_addr %llx size %d\n", buf.addr, (int)buf.size);
			iovbase = cb->rdma_dma_addr;
			cb->rdma_mr = ib_reg_phys_mr(cb->pd, &buf, 1, 
					     IB_ACCESS_REMOTE_READ| 
					     IB_ACCESS_REMOTE_WRITE, 
					     &iovbase);
#endif
			
		} else {
			buf.addr = cb->rdma_dma_addr;
			buf.size = cb->size;
			DEBUG_LOG(PFX "rdma buf dma_addr %llx size %d\n", buf.addr, (int)buf.size);
			iovbase = cb->rdma_dma_addr;
			cb->rdma_mr = ib_reg_phys_mr(cb->pd, &buf, 1, 
					     IB_ACCESS_REMOTE_READ| 
					     IB_ACCESS_REMOTE_WRITE, 
					     &iovbase);
		}

		if (IS_ERR(cb->rdma_mr)) {
			DEBUG_LOG(PFX "rdma_buf reg_mr failed\n");
			ret = PTR_ERR(cb->rdma_mr);
			goto err3;
		}
	}

	if (!cb->server || cb->wlat || cb->rlat || cb->bw) {

		cb->start_buf = kmalloc(cb->size, GFP_KERNEL);
		if (!cb->start_buf) {
			DEBUG_LOG(PFX "start_buf malloc failed\n");
			ret = -ENOMEM;
			goto err3;
		}

		cb->start_dma_addr = dma_map_single(cb->pd->device->dma_device, 
						   cb->start_buf, cb->size, 
						   DMA_BIDIRECTIONAL);
		pci_unmap_addr_set(cb, start_mapping, cb->start_dma_addr);

		if (cb->mem != DMA) {
			unsigned flags = IB_ACCESS_REMOTE_READ;

			if (cb->wlat || cb->rlat || cb->bw)
				flags |= IB_ACCESS_REMOTE_WRITE;

			buf.addr = cb->start_dma_addr;
			buf.size = cb->size;
			DEBUG_LOG(PFX "start buf dma_addr %llx size %d\n", buf.addr, (int)buf.size);
			iovbase = cb->start_dma_addr;
			cb->start_mr = ib_reg_phys_mr(cb->pd, &buf, 1, 
					     flags,
					     &iovbase);

			if (IS_ERR(cb->start_mr)) {
				DEBUG_LOG(PFX "start_buf reg_mr failed\n");
				ret = PTR_ERR(cb->start_mr);
				goto err4;
			}
		}
	}

	krping_setup_wr(cb);
	DEBUG_LOG(PFX "allocated & registered buffers...\n");
	return 0;
err4:
	kfree(cb->start_buf);

	if (cb->mem != DMA)
		ib_dereg_mr(cb->rdma_mr);
err3:
	ib_free_fast_reg_page_list(cb->page_list);
err2:
	kfree(cb->rdma_buf);
err1:
	if (cb->mem == DMA)
		ib_dereg_mr(cb->dma_mr);
	else {
		ib_dereg_mr(cb->recv_mr);
		ib_dereg_mr(cb->send_mr);
	}
	return ret;
}

static void krping_free_buffers(struct krping_cb *cb)
{
	DEBUG_LOG("krping_free_buffers called on cb %p\n", cb);
	
	if (cb->mem == DMA)
		ib_dereg_mr(cb->dma_mr);
	else {
		ib_dereg_mr(cb->send_mr);
		ib_dereg_mr(cb->recv_mr);
		ib_dereg_mr(cb->rdma_mr);
		if (!cb->server)
			ib_dereg_mr(cb->start_mr);
	}

	dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, recv_mapping),
			 sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);
	dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, send_mapping),
			 sizeof(cb->send_buf), DMA_BIDIRECTIONAL);
	dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, rdma_mapping),
			 cb->size, DMA_BIDIRECTIONAL);
	kfree(cb->rdma_buf);
	if (!cb->server || cb->wlat || cb->rlat || cb->bw) {
		dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, start_mapping),
			 cb->size, DMA_BIDIRECTIONAL);
		kfree(cb->start_buf);
	}
}

static int krping_create_qp(struct krping_cb *cb)
{
	struct ib_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = cb->txdepth;
	init_attr.cap.max_recv_wr = 2;
	init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_send_sge = 1;
	init_attr.qp_type = IB_QPT_RC;
	init_attr.send_cq = cb->cq;
	init_attr.recv_cq = cb->cq;

	if (cb->server) {
		ret = rdma_create_qp(cb->child_cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->child_cm_id->qp;
	} else {
		ret = rdma_create_qp(cb->cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->cm_id->qp;
	}

	return ret;
}

static void krping_free_qp(struct krping_cb *cb)
{
	ib_destroy_qp(cb->qp);
	ib_destroy_cq(cb->cq);
	ib_dealloc_pd(cb->pd);
}

static int krping_setup_qp(struct krping_cb *cb, struct rdma_cm_id *cm_id)
{
	int ret;
	cb->pd = ib_alloc_pd(cm_id->device);
	if (IS_ERR(cb->pd)) {
		printk(KERN_ERR PFX "ib_alloc_pd failed\n");
		return PTR_ERR(cb->pd);
	}
	DEBUG_LOG("created pd %p\n", cb->pd);

	cb->cq = ib_create_cq(cm_id->device, krping_cq_event_handler, NULL,
			      cb, cb->txdepth * 2, 0);
	if (IS_ERR(cb->cq)) {
		printk(KERN_ERR PFX "ib_create_cq failed\n");
		ret = PTR_ERR(cb->cq);
		goto err1;
	}
	DEBUG_LOG("created cq %p\n", cb->cq);

	if (!cb->wlat && !cb->rlat && !cb->bw) {
		ret = ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
		if (ret) {
			printk(KERN_ERR PFX "ib_create_cq failed\n");
			goto err2;
		}
	}

	ret = krping_create_qp(cb);
	if (ret) {
		printk(KERN_ERR PFX "krping_create_qp failed: %d\n", ret);
		goto err2;
	}
	DEBUG_LOG("created qp %p\n", cb->qp);
	return 0;
err2:
	ib_destroy_cq(cb->cq);
err1:
	ib_dealloc_pd(cb->pd);
	return ret;
}

static void krping_format_send(struct krping_cb *cb, u64 buf, 
			       struct ib_mr *mr)
{
	struct krping_rdma_info *info = &cb->send_buf;

	info->buf = htonll(buf);
	info->rkey = htonl(mr->rkey);
	info->size = htonl(cb->size);

	DEBUG_LOG("RDMA addr %llx rkey %x len %d\n",
		  (unsigned long long)buf, mr->rkey, cb->size);
}

static void krping_test_server(struct krping_cb *cb)
{
	struct ib_send_wr *bad_wr;
	int ret;

	while (1) {
		/* Wait for client's Start STAG/TO/Len */
		wait_event_interruptible(cb->sem, cb->state >= RDMA_READ_ADV);
		if (cb->state != RDMA_READ_ADV) {
			printk(KERN_ERR PFX "wait for RDMA_READ_ADV state %d\n",
				cb->state);
			break;
		}

		DEBUG_LOG("server received sink adv\n");

		/* Issue RDMA Read. */
		cb->rdma_sq_wr.opcode = IB_WR_RDMA_READ;
		cb->rdma_sq_wr.wr.rdma.rkey = cb->remote_rkey;
		cb->rdma_sq_wr.wr.rdma.remote_addr = cb->remote_addr;
		cb->rdma_sq_wr.sg_list->length = cb->remote_len;

		ret = ib_post_send(cb->qp, &cb->rdma_sq_wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}
		DEBUG_LOG("server posted rdma read req \n");

		/* Wait for read completion */
		wait_event_interruptible(cb->sem, 
					 cb->state >= RDMA_READ_COMPLETE);
		if (cb->state != RDMA_READ_COMPLETE) {
			printk(KERN_ERR PFX 
			       "wait for RDMA_READ_COMPLETE state %d\n",
			       cb->state);
			break;
		}
		DEBUG_LOG("server received read complete\n");

		/* Display data in recv buf */
		if (cb->verbose)
			printk(KERN_INFO PFX "server ping data: %s\n", cb->rdma_buf);

		/* Tell client to continue */
		ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}
		DEBUG_LOG("server posted go ahead\n");

		/* Wait for client's RDMA STAG/TO/Len */
		wait_event_interruptible(cb->sem, cb->state >= RDMA_WRITE_ADV);
		if (cb->state != RDMA_WRITE_ADV) {
			printk(KERN_ERR PFX 
			       "wait for RDMA_WRITE_ADV state %d\n",
			       cb->state);
			break;
		}
		DEBUG_LOG("server received sink adv\n");

		/* RDMA Write echo data */
		cb->rdma_sq_wr.opcode = IB_WR_RDMA_WRITE;
		cb->rdma_sq_wr.wr.rdma.rkey = cb->remote_rkey;
		cb->rdma_sq_wr.wr.rdma.remote_addr = cb->remote_addr;
		cb->rdma_sq_wr.sg_list->length = strlen(cb->rdma_buf) + 1;
		DEBUG_LOG("rdma write from lkey %x laddr %llx len %d\n",
			  cb->rdma_sq_wr.sg_list->lkey,
			  (unsigned long long)cb->rdma_sq_wr.sg_list->addr,
			  cb->rdma_sq_wr.sg_list->length);

		ret = ib_post_send(cb->qp, &cb->rdma_sq_wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}

		/* Wait for completion */
		ret = wait_event_interruptible(cb->sem, cb->state >= 
							 RDMA_WRITE_COMPLETE);
		if (cb->state != RDMA_WRITE_COMPLETE) {
			printk(KERN_ERR PFX 
			       "wait for RDMA_WRITE_COMPLETE state %d\n",
			       cb->state);
			break;
		}
		DEBUG_LOG("server rdma write complete \n");

		cb->state = CONNECTED;

		/* Tell client to begin again */
		ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}
		DEBUG_LOG("server posted go ahead\n");
	}
}

static void rlat_test(struct krping_cb *cb)
{
	int scnt;
	int iters = cb->count;
	struct timeval start_tv, stop_tv;
	int ret;
	struct ib_wc wc;
	struct ib_send_wr *bad_wr;
	int ne;

	scnt = 0;
	cb->rdma_sq_wr.opcode = IB_WR_RDMA_READ;
	cb->rdma_sq_wr.wr.rdma.rkey = cb->remote_rkey;
	cb->rdma_sq_wr.wr.rdma.remote_addr = cb->remote_addr;
	cb->rdma_sq_wr.sg_list->length = cb->size;

	do_gettimeofday(&start_tv);
	if (!cb->poll) {
		cb->state = RDMA_READ_ADV;
		ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
	}
	while (scnt < iters) {

		cb->state = RDMA_READ_ADV;
		ret = ib_post_send(cb->qp, &cb->rdma_sq_wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX  
				"Couldn't post send: ret=%d scnt %d\n",
				ret, scnt);
			return;
		}

		do {
			if (!cb->poll) {
				wait_event_interruptible(cb->sem, cb->state != RDMA_READ_ADV);
				if (cb->state == RDMA_READ_COMPLETE) {
					ne = 1;
					ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
				} else {
					ne = -1;
				}
			} else
				ne = ib_poll_cq(cb->cq, 1, &wc);
			if (cb->state == ERROR) {
				printk(KERN_ERR PFX 
				       "state == ERROR...bailing scnt %d\n", scnt);
				return;
			}
		} while (ne == 0);

		if (ne < 0) {
			printk(KERN_ERR PFX "poll CQ failed %d\n", ne);
			return;
		}
		if (cb->poll && wc.status != IB_WC_SUCCESS) {
			printk(KERN_ERR PFX "Completion wth error at %s:\n",
				cb->server ? "server" : "client");
			printk(KERN_ERR PFX "Failed status %d: wr_id %d\n",
				wc.status, (int) wc.wr_id);
			return;
		}
		++scnt;
	}
	do_gettimeofday(&stop_tv);

        if (stop_tv.tv_usec < start_tv.tv_usec) {
                stop_tv.tv_usec += 1000000;
                stop_tv.tv_sec  -= 1;
        }

	printk(KERN_ERR PFX "delta sec %lu delta usec %lu iter %d size %d\n",
		stop_tv.tv_sec - start_tv.tv_sec, 
		stop_tv.tv_usec - start_tv.tv_usec,
		scnt, cb->size);
}

static void wlat_test(struct krping_cb *cb)
{
	int ccnt, scnt, rcnt;
	int iters=cb->count;
	volatile char *poll_buf = (char *) cb->start_buf;
	char *buf = (char *)cb->rdma_buf;
	struct timeval start_tv, stop_tv;
	cycles_t *post_cycles_start, *post_cycles_stop;
	cycles_t *poll_cycles_start, *poll_cycles_stop;
	cycles_t *last_poll_cycles_start;
	cycles_t sum_poll = 0, sum_post = 0, sum_last_poll = 0;
	int i;
	int cycle_iters = 1000;

	ccnt = 0;
	scnt = 0;
	rcnt = 0;

	post_cycles_start = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!post_cycles_start) {
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		return;
	}
	post_cycles_stop = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!post_cycles_stop) {
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		return;
	}
	poll_cycles_start = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!poll_cycles_start) {
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		return;
	}
	poll_cycles_stop = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!poll_cycles_stop) {
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		return;
	}
	last_poll_cycles_start = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!last_poll_cycles_start) {
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		return;
	}
	cb->rdma_sq_wr.opcode = IB_WR_RDMA_WRITE;
	cb->rdma_sq_wr.wr.rdma.rkey = cb->remote_rkey;
	cb->rdma_sq_wr.wr.rdma.remote_addr = cb->remote_addr;
	cb->rdma_sq_wr.sg_list->length = cb->size;

	if (cycle_iters > iters)
		cycle_iters = iters;
	do_gettimeofday(&start_tv);
	while (scnt < iters || ccnt < iters || rcnt < iters) {

		/* Wait till buffer changes. */
		if (rcnt < iters && !(scnt < 1 && !cb->server)) {
			++rcnt;
			while (*poll_buf != (char)rcnt) {
				if (cb->state == ERROR) {
					printk(KERN_ERR PFX "state = ERROR, bailing\n");
					return;
				}
			}
		}

		if (scnt < iters) {
			struct ib_send_wr *bad_wr;

			*buf = (char)scnt+1;
			if (scnt < cycle_iters)
				post_cycles_start[scnt] = get_cycles();
			if (ib_post_send(cb->qp, &cb->rdma_sq_wr, &bad_wr)) {
				printk(KERN_ERR PFX  "Couldn't post send: scnt=%d\n",
					scnt);
				return;
			}
			if (scnt < cycle_iters)
				post_cycles_stop[scnt] = get_cycles();
			scnt++;
		}

		if (ccnt < iters) {
			struct ib_wc wc;
			int ne;

			if (ccnt < cycle_iters)
				poll_cycles_start[ccnt] = get_cycles();
			do {
				if (ccnt < cycle_iters)
					last_poll_cycles_start[ccnt] = get_cycles();
				ne = ib_poll_cq(cb->cq, 1, &wc);
			} while (ne == 0);
			if (ccnt < cycle_iters)
				poll_cycles_stop[ccnt] = get_cycles();
			++ccnt;

			if (ne < 0) {
				printk(KERN_ERR PFX "poll CQ failed %d\n", ne);
				return;
			}
			if (wc.status != IB_WC_SUCCESS) {
				printk(KERN_ERR PFX "Completion wth error at %s:\n",
					cb->server ? "server" : "client");
				printk(KERN_ERR PFX "Failed status %d: wr_id %d\n",
					wc.status, (int) wc.wr_id);
				printk(KERN_ERR PFX "scnt=%d, rcnt=%d, ccnt=%d\n",
					scnt, rcnt, ccnt);
				return;
			}
		}
	}
	do_gettimeofday(&stop_tv);

        if (stop_tv.tv_usec < start_tv.tv_usec) {
                stop_tv.tv_usec += 1000000;
                stop_tv.tv_sec  -= 1;
        }

	for (i=0; i < cycle_iters; i++) {
		sum_post += post_cycles_stop[i] - post_cycles_start[i];
		sum_poll += poll_cycles_stop[i] - poll_cycles_start[i];
		sum_last_poll += poll_cycles_stop[i] - last_poll_cycles_start[i];
	}
	printk(KERN_ERR PFX "delta sec %lu delta usec %lu iter %d size %d cycle_iters %d sum_post %llu sum_poll %llu sum_last_poll %llu\n",
		stop_tv.tv_sec - start_tv.tv_sec, 
		stop_tv.tv_usec - start_tv.tv_usec,
		scnt, cb->size, cycle_iters, 
		(unsigned long long)sum_post, (unsigned long long)sum_poll, 
		(unsigned long long)sum_last_poll);
	kfree(post_cycles_start);
	kfree(post_cycles_stop);
	kfree(poll_cycles_start);
	kfree(poll_cycles_stop);
	kfree(last_poll_cycles_start);
}

static void bw_test(struct krping_cb *cb)
{
	int ccnt, scnt, rcnt;
	int iters=cb->count;
	struct timeval start_tv, stop_tv;
	cycles_t *post_cycles_start, *post_cycles_stop;
	cycles_t *poll_cycles_start, *poll_cycles_stop;
	cycles_t *last_poll_cycles_start;
	cycles_t sum_poll = 0, sum_post = 0, sum_last_poll = 0;
	int i;
	int cycle_iters = 1000;

	ccnt = 0;
	scnt = 0;
	rcnt = 0;

	post_cycles_start = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!post_cycles_start) {
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		return;
	}
	post_cycles_stop = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!post_cycles_stop) {
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		return;
	}
	poll_cycles_start = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!poll_cycles_start) {
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		return;
	}
	poll_cycles_stop = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!poll_cycles_stop) {
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		return;
	}
	last_poll_cycles_start = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!last_poll_cycles_start) {
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		return;
	}
	cb->rdma_sq_wr.opcode = IB_WR_RDMA_WRITE;
	cb->rdma_sq_wr.wr.rdma.rkey = cb->remote_rkey;
	cb->rdma_sq_wr.wr.rdma.remote_addr = cb->remote_addr;
	cb->rdma_sq_wr.sg_list->length = cb->size;

	if (cycle_iters > iters)
		cycle_iters = iters;
	do_gettimeofday(&start_tv);
	while (scnt < iters || ccnt < iters) {

		while (scnt < iters && scnt - ccnt < cb->txdepth) {
			struct ib_send_wr *bad_wr;

			if (scnt < cycle_iters)
				post_cycles_start[scnt] = get_cycles();
			if (ib_post_send(cb->qp, &cb->rdma_sq_wr, &bad_wr)) {
				printk(KERN_ERR PFX  "Couldn't post send: scnt=%d\n",
					scnt);
				return;
			}
			if (scnt < cycle_iters)
				post_cycles_stop[scnt] = get_cycles();
			++scnt;
		}

		if (ccnt < iters) {
			int ne;
			struct ib_wc wc;

			if (ccnt < cycle_iters)
				poll_cycles_start[ccnt] = get_cycles();
			do {
				if (ccnt < cycle_iters)
					last_poll_cycles_start[ccnt] = get_cycles();
				ne = ib_poll_cq(cb->cq, 1, &wc);
			} while (ne == 0);
			if (ccnt < cycle_iters)
				poll_cycles_stop[ccnt] = get_cycles();
			ccnt += 1;

			if (ne < 0) {
				printk(KERN_ERR PFX "poll CQ failed %d\n", ne);
				return;
			}
			if (wc.status != IB_WC_SUCCESS) {
				printk(KERN_ERR PFX "Completion wth error at %s:\n",
					cb->server ? "server" : "client");
				printk(KERN_ERR PFX "Failed status %d: wr_id %d\n",
					wc.status, (int) wc.wr_id);
				return;
			}
		}
	}
	do_gettimeofday(&stop_tv);

        if (stop_tv.tv_usec < start_tv.tv_usec) {
                stop_tv.tv_usec += 1000000;
                stop_tv.tv_sec  -= 1;
        }

	for (i=0; i < cycle_iters; i++) {
		sum_post += post_cycles_stop[i] - post_cycles_start[i];
		sum_poll += poll_cycles_stop[i] - poll_cycles_start[i];
		sum_last_poll += poll_cycles_stop[i] - last_poll_cycles_start[i];
	}
	printk(KERN_ERR PFX "delta sec %lu delta usec %lu iter %d size %d cycle_iters %d sum_post %llu sum_poll %llu sum_last_poll %llu\n",
		stop_tv.tv_sec - start_tv.tv_sec, 
		stop_tv.tv_usec - start_tv.tv_usec,
		scnt, cb->size, cycle_iters, 
		(unsigned long long)sum_post, (unsigned long long)sum_poll, 
		(unsigned long long)sum_last_poll);
	kfree(post_cycles_start);
	kfree(post_cycles_stop);
	kfree(poll_cycles_start);
	kfree(poll_cycles_stop);
	kfree(last_poll_cycles_start);
}

static void krping_rlat_test_server(struct krping_cb *cb)
{
	struct ib_send_wr *bad_wr;
	struct ib_wc wc;
	int ret;

	/* Spin waiting for client's Start STAG/TO/Len */
	while (cb->state < RDMA_READ_ADV) {
		krping_cq_event_handler(cb->cq, cb);
	}

	/* Send STAG/TO/Len to client */
	if (cb->mem == DMA)
		krping_format_send(cb, cb->start_dma_addr, cb->dma_mr);
	else
		krping_format_send(cb, cb->start_dma_addr, cb->start_mr);
	ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "post send error %d\n", ret);
		return;
	}

	/* Spin waiting for send completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc) == 0));
	if (ret < 0) {
		printk(KERN_ERR PFX "poll error %d\n", ret);
		return;
	}
	if (wc.status) {
		printk(KERN_ERR PFX "send completiong error %d\n", wc.status);
		return;
	}

	wait_event_interruptible(cb->sem, cb->state == ERROR);
}

static void krping_wlat_test_server(struct krping_cb *cb)
{
	struct ib_send_wr *bad_wr;
	struct ib_wc wc;
	int ret;

	/* Spin waiting for client's Start STAG/TO/Len */
	while (cb->state < RDMA_READ_ADV) {
		krping_cq_event_handler(cb->cq, cb);
	}

	/* Send STAG/TO/Len to client */
	if (cb->mem == DMA)
		krping_format_send(cb, cb->start_dma_addr, cb->dma_mr);
	else
		krping_format_send(cb, cb->start_dma_addr, cb->start_mr);
	ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "post send error %d\n", ret);
		return;
	}

	/* Spin waiting for send completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc) == 0));
	if (ret < 0) {
		printk(KERN_ERR PFX "poll error %d\n", ret);
		return;
	}
	if (wc.status) {
		printk(KERN_ERR PFX "send completiong error %d\n", wc.status);
		return;
	}

	wlat_test(cb);

}

static void krping_bw_test_server(struct krping_cb *cb)
{
	struct ib_send_wr *bad_wr;
	struct ib_wc wc;
	int ret;

	/* Spin waiting for client's Start STAG/TO/Len */
	while (cb->state < RDMA_READ_ADV) {
		krping_cq_event_handler(cb->cq, cb);
	}

	/* Send STAG/TO/Len to client */
	if (cb->mem == DMA)
		krping_format_send(cb, cb->start_dma_addr, cb->dma_mr);
	else
		krping_format_send(cb, cb->start_dma_addr, cb->start_mr);
	ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "post send error %d\n", ret);
		return;
	}

	/* Spin waiting for send completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc) == 0));
	if (ret < 0) {
		printk(KERN_ERR PFX "poll error %d\n", ret);
		return;
	}
	if (wc.status) {
		printk(KERN_ERR PFX "send completiong error %d\n", wc.status);
		return;
	}

	if (cb->duplex)
		bw_test(cb);
	wait_event_interruptible(cb->sem, cb->state == ERROR);
}

static int krping_bind_server(struct krping_cb *cb)
{
	struct sockaddr_in sin;
	int ret;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = cb->addr;
	sin.sin_port = cb->port;

	ret = rdma_bind_addr(cb->cm_id, (struct sockaddr *) &sin);
	if (ret) {
		printk(KERN_ERR PFX "rdma_bind_addr error %d\n", ret);
		return ret;
	}
	DEBUG_LOG("rdma_bind_addr successful\n");

	DEBUG_LOG("rdma_listen\n");
	ret = rdma_listen(cb->cm_id, 3);
	if (ret) {
		printk(KERN_ERR PFX "rdma_listen failed: %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= CONNECT_REQUEST);
	if (cb->state != CONNECT_REQUEST) {
		printk(KERN_ERR PFX "wait for CONNECT_REQUEST state %d\n",
			cb->state);
		return -1;
	}

	return 0;
}

static void krping_run_server(struct krping_cb *cb)
{
	struct ib_recv_wr *bad_wr;
	int ret;

	ret = krping_bind_server(cb);
	if (ret)
		return;

	ret = krping_setup_qp(cb, cb->child_cm_id);
	if (ret) {
		printk(KERN_ERR PFX "setup_qp failed: %d\n", ret);
		return;
	}

	ret = krping_setup_buffers(cb);
	if (ret) {
		printk(KERN_ERR PFX "krping_setup_buffers failed: %d\n", ret);
		goto err1;
	}

	ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "ib_post_recv failed: %d\n", ret);
		goto err2;
	}

	ret = krping_accept(cb);
	if (ret) {
		printk(KERN_ERR PFX "connect error %d\n", ret);
		goto err2;
	}

	if (cb->wlat)
		krping_wlat_test_server(cb);
	else if (cb->rlat)
		krping_rlat_test_server(cb);
	else if (cb->bw)
		krping_bw_test_server(cb);
	else
		krping_test_server(cb);
	rdma_disconnect(cb->child_cm_id);
	rdma_destroy_id(cb->child_cm_id);
err2:
	krping_free_buffers(cb);
err1:
	krping_free_qp(cb);
}

static void krping_test_client(struct krping_cb *cb)
{
	int ping, start, cc, i, ret;
	struct ib_send_wr *bad_wr;
	unsigned char c;

	start = 65;
	for (ping = 0; !cb->count || ping < cb->count; ping++) {
		cb->state = RDMA_READ_ADV;

		/* Put some ascii text in the buffer. */
		cc = sprintf(cb->start_buf, "rdma-ping-%d: ", ping);
		for (i = cc, c = start; i < cb->size; i++) {
			cb->start_buf[i] = c;
			c++;
			if (c > 122)
				c = 65;
		}
		start++;
		if (start > 122)
			start = 65;
		cb->start_buf[cb->size - 1] = 0;

		if (cb->mem == DMA)
			krping_format_send(cb, cb->start_dma_addr, cb->dma_mr);
		else
			krping_format_send(cb, cb->start_dma_addr, cb->start_mr);
		ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}

		/* Wait for server to ACK */
		wait_event_interruptible(cb->sem, cb->state >= RDMA_WRITE_ADV);
		if (cb->state != RDMA_WRITE_ADV) {
			printk(KERN_ERR PFX 
			       "wait for RDMA_WRITE_ADV state %d\n",
			       cb->state);
			break;
		}

		if (cb->mem == DMA)
			krping_format_send(cb, cb->rdma_dma_addr, cb->dma_mr);
		else
			krping_format_send(cb, cb->rdma_dma_addr, cb->rdma_mr);
		ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}

		/* Wait for the server to say the RDMA Write is complete. */
		wait_event_interruptible(cb->sem, 
					 cb->state >= RDMA_WRITE_COMPLETE);
		if (cb->state != RDMA_WRITE_COMPLETE) {
			printk(KERN_ERR PFX 
			       "wait for RDMA_WRITE_COMPLETE state %d\n",
			       cb->state);
			break;
		}

		if (cb->validate)
			if (memcmp(cb->start_buf, cb->rdma_buf, cb->size)) {
				printk(KERN_ERR PFX "data mismatch!\n");
				break;
			}

		if (cb->verbose)
			printk(KERN_INFO PFX "ping data: %s\n", cb->rdma_buf);
	}
}

static void krping_rlat_test_client(struct krping_cb *cb)
{
	struct ib_send_wr *bad_wr;
	struct ib_wc wc;
	int ret;

	cb->state = RDMA_READ_ADV;

	/* Send STAG/TO/Len to client */
	if (cb->mem == DMA)
		krping_format_send(cb, cb->start_dma_addr, cb->dma_mr);
	else
		krping_format_send(cb, cb->start_dma_addr, cb->start_mr);
	ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "post send error %d\n", ret);
		return;
	}

	/* Spin waiting for send completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc) == 0));
	if (ret < 0) {
		printk(KERN_ERR PFX "poll error %d\n", ret);
		return;
	}
	if (wc.status) {
		printk(KERN_ERR PFX "send completion error %d\n", wc.status);
		return;
	}

	/* Spin waiting for server's Start STAG/TO/Len */
	while (cb->state < RDMA_WRITE_ADV) {
		krping_cq_event_handler(cb->cq, cb);
	}

#if 0
{
	int i;
	struct timeval start, stop;
	time_t sec;
	suseconds_t usec;
	unsigned long long elapsed;
	struct ib_wc wc;
	struct ib_send_wr *bad_wr;
	int ne;
	
	cb->rdma_sq_wr.opcode = IB_WR_RDMA_WRITE;
	cb->rdma_sq_wr.wr.rdma.rkey = cb->remote_rkey;
	cb->rdma_sq_wr.wr.rdma.remote_addr = cb->remote_addr;
	cb->rdma_sq_wr.sg_list->length = 0;
	cb->rdma_sq_wr.num_sge = 0;

	do_gettimeofday(&start);
	for (i=0; i < 100000; i++) {
		if (ib_post_send(cb->qp, &cb->rdma_sq_wr, &bad_wr)) {
			printk(KERN_ERR PFX  "Couldn't post send\n");
			return;
		}
		do {
			ne = ib_poll_cq(cb->cq, 1, &wc);
		} while (ne == 0);
		if (ne < 0) {
			printk(KERN_ERR PFX "poll CQ failed %d\n", ne);
			return;
		}
		if (wc.status != IB_WC_SUCCESS) {
			printk(KERN_ERR PFX "Completion wth error at %s:\n",
				cb->server ? "server" : "client");
			printk(KERN_ERR PFX "Failed status %d: wr_id %d\n",
				wc.status, (int) wc.wr_id);
			return;
		}
	}
	do_gettimeofday(&stop);
	
	if (stop.tv_usec < start.tv_usec) {
		stop.tv_usec += 1000000;
		stop.tv_sec  -= 1;
	}
	sec     = stop.tv_sec - start.tv_sec;
	usec    = stop.tv_usec - start.tv_usec;
	elapsed = sec * 1000000 + usec;
	printk(KERN_ERR PFX "0B-write-lat iters 100000 usec %llu\n", elapsed);
}
#endif

	rlat_test(cb);
}

static void krping_wlat_test_client(struct krping_cb *cb)
{
	struct ib_send_wr *bad_wr;
	struct ib_wc wc;
	int ret;

	cb->state = RDMA_READ_ADV;

	/* Send STAG/TO/Len to client */
	if (cb->mem == DMA)
		krping_format_send(cb, cb->start_dma_addr, cb->dma_mr);
	else
		krping_format_send(cb, cb->start_dma_addr, cb->start_mr);
	ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "post send error %d\n", ret);
		return;
	}

	/* Spin waiting for send completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc) == 0));
	if (ret < 0) {
		printk(KERN_ERR PFX "poll error %d\n", ret);
		return;
	}
	if (wc.status) {
		printk(KERN_ERR PFX "send completion error %d\n", wc.status);
		return;
	}

	/* Spin waiting for server's Start STAG/TO/Len */
	while (cb->state < RDMA_WRITE_ADV) {
		krping_cq_event_handler(cb->cq, cb);
	}

	wlat_test(cb);
}

static void krping_bw_test_client(struct krping_cb *cb)
{
	struct ib_send_wr *bad_wr;
	struct ib_wc wc;
	int ret;

	cb->state = RDMA_READ_ADV;

	/* Send STAG/TO/Len to client */
	if (cb->mem == DMA)
		krping_format_send(cb, cb->start_dma_addr, cb->dma_mr);
	else
		krping_format_send(cb, cb->start_dma_addr, cb->start_mr);
	ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "post send error %d\n", ret);
		return;
	}

	/* Spin waiting for send completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc) == 0));
	if (ret < 0) {
		printk(KERN_ERR PFX "poll error %d\n", ret);
		return;
	}
	if (wc.status) {
		printk(KERN_ERR PFX "send completion error %d\n", wc.status);
		return;
	}

	/* Spin waiting for server's Start STAG/TO/Len */
	while (cb->state < RDMA_WRITE_ADV) {
		krping_cq_event_handler(cb->cq, cb);
	}

	bw_test(cb);
}

static int krping_connect_client(struct krping_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 10;

	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR PFX "rdma_connect error %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= CONNECTED);
	if (cb->state == ERROR) {
		printk(KERN_ERR PFX "wait for CONNECTED state %d\n", cb->state);
		return -1;
	}

	DEBUG_LOG("rdma_connect successful\n");
	return 0;
}

static int krping_bind_client(struct krping_cb *cb)
{
	struct sockaddr_in sin;
	int ret;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = cb->addr;
	sin.sin_port = cb->port;

	ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr *) &sin,
				2000);
	if (ret) {
		printk(KERN_ERR PFX "rdma_resolve_addr error %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= ROUTE_RESOLVED);
	if (cb->state != ROUTE_RESOLVED) {
		printk(KERN_ERR PFX 
		       "addr/route resolution did not resolve: state %d\n",
		       cb->state);
		return -EINTR;
	}

	DEBUG_LOG("rdma_resolve_addr - rdma_resolve_route successful\n");
	return 0;
}

static void krping_run_client(struct krping_cb *cb)
{
	struct ib_recv_wr *bad_wr;
	int ret;

	ret = krping_bind_client(cb);
	if (ret)
		return;

	ret = krping_setup_qp(cb, cb->cm_id);
	if (ret) {
		printk(KERN_ERR PFX "setup_qp failed: %d\n", ret);
		return;
	}

	ret = krping_setup_buffers(cb);
	if (ret) {
		printk(KERN_ERR PFX "krping_setup_buffers failed: %d\n", ret);
		goto err1;
	}

	ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "ib_post_recv failed: %d\n", ret);
		goto err2;
	}

	ret = krping_connect_client(cb);
	if (ret) {
		printk(KERN_ERR PFX "connect error %d\n", ret);
		goto err2;
	}

	if (cb->wlat)
		krping_wlat_test_client(cb);
	else if (cb->rlat)
		krping_rlat_test_client(cb);
	else if (cb->bw)
		krping_bw_test_client(cb);
	else
		krping_test_client(cb);
	rdma_disconnect(cb->cm_id);
err2:
	krping_free_buffers(cb);
err1:
	krping_free_qp(cb);
}

int krping_doit(char *cmd)
{
	struct krping_cb *cb;
	int op;
	int ret = 0;
	char *optarg;
	unsigned long optint;

	cb = kzalloc(sizeof(*cb), GFP_KERNEL);
	if (!cb)
		return -ENOMEM;

	down(&krping_mutex);
	list_add_tail(&cb->list, &krping_cbs);
	up(&krping_mutex);

	cb->server = -1;
	cb->state = IDLE;
	cb->size = 64;
	cb->txdepth = RPING_SQ_DEPTH;
	init_waitqueue_head(&cb->sem);

	while ((op = krping_getopt("krping", &cmd, krping_opts, NULL, &optarg,
			      &optint)) != 0) {
		switch (op) {
		case 'a':
			cb->addr_str = optarg;
			cb->addr = in_aton(optarg);
			DEBUG_LOG("ipaddr (%s)\n", optarg);
			break;
		case 'p':
			cb->port = htons(optint);
			DEBUG_LOG("port %d\n", (int)optint);
			break;
		case 'P':
			cb->poll = 1;
			DEBUG_LOG("server\n");
			break;
		case 's':
			cb->server = 1;
			DEBUG_LOG("server\n");
			break;
		case 'c':
			cb->server = 0;
			DEBUG_LOG("client\n");
			break;
		case 'S':
			cb->size = optint;
			if ((cb->size < 1) ||
			    (cb->size > RPING_BUFSIZE)) {
				printk(KERN_ERR PFX "Invalid size %d "
				       "(valid range is 1 to %d)\n",
				       cb->size, RPING_BUFSIZE);
				ret = EINVAL;
			} else
				DEBUG_LOG("size %d\n", (int)optint);
			break;
		case 'C':
			cb->count = optint;
			if (cb->count < 0) {
				printk(KERN_ERR PFX "Invalid count %d\n",
					cb->count);
				ret = EINVAL;
			} else
				DEBUG_LOG("count %d\n", (int) cb->count);
			break;
		case 'v':
			cb->verbose++;
			DEBUG_LOG("verbose\n");
			break;
		case 'V':
			cb->validate++;
			DEBUG_LOG("validate data\n");
			break;
		case 'l':
			cb->wlat++;
			break;
		case 'L':
			cb->rlat++;
			break;
		case 'B':
			cb->bw++;
			break;
		case 'd':
			cb->duplex++;
			break;
		case 'm':
			if (!strncmp(optarg, "dma", 3))
				cb->mem = DMA;
			else if (!strncmp(optarg, "fastreg", 7))
				cb->mem = FASTREG;
			else if (!strncmp(optarg, "window", 6))
				cb->mem = WINDOW;
			else if (!strncmp(optarg, "mr", 2))
				cb->mem = MR;
			else {
				printk(KERN_ERR PFX "unknown mem mode %s.  "
					"Must be dma, fastreg, window, or mr\n",
					optarg);
				ret = -EINVAL;
				break;
			}
			break;
		case 'T':
			cb->txdepth = optint;
			DEBUG_LOG("txdepth %d\n", (int) cb->txdepth);
			break;
		default:
			printk(KERN_ERR PFX "unknown opt %s\n", optarg);
			ret = -EINVAL;
			break;
		}
	}
	if (ret)
		goto out;

	if (cb->server == -1) {
		printk(KERN_ERR PFX "must be either client or server\n");
		ret = -EINVAL;
		goto out;
	}

	if ((cb->bw + cb->rlat + cb->wlat) > 1) {
		printk(KERN_ERR PFX "Pick only one test: bw, rlat, wlat\n");
		ret = -EINVAL;
		goto out;
	}

	cb->cm_id = rdma_create_id(krping_cma_event_handler, cb, RDMA_PS_TCP);
	if (IS_ERR(cb->cm_id)) {
		ret = PTR_ERR(cb->cm_id);
		printk(KERN_ERR PFX "rdma_create_id error %d\n", ret);
		goto out;
	}
	DEBUG_LOG("created cm_id %p\n", cb->cm_id);

	if (cb->server)
		krping_run_server(cb);
	else
		krping_run_client(cb);

	DEBUG_LOG("destroy cm_id %p\n", cb->cm_id);
	rdma_destroy_id(cb->cm_id);
out:
	down(&krping_mutex);
	list_del(&cb->list);
	up(&krping_mutex);
	kfree(cb);
	return ret;
}

/*
 * Read proc returns stats for each device.
 */
static int krping_read_proc(char *page, char **start, off_t off, int count,
			    int *eof, void *data)
{
	struct krping_cb *cb;
	int cc = 0;
	char *cp = page;
	int num = 1;

	DEBUG_LOG(KERN_INFO PFX "proc read called...\n");
	*cp = 0;
	down(&krping_mutex);
	list_for_each_entry(cb, &krping_cbs, list) {
		if (cb->pd) {
			cc = sprintf(cp,
			     "%d-%s %lld %lld %lld %lld %lld %lld %lld %lld\n",
			     num++, cb->pd->device->name, cb->stats.send_bytes,
			     cb->stats.send_msgs, cb->stats.recv_bytes,
			     cb->stats.recv_msgs, cb->stats.write_bytes,
			     cb->stats.write_msgs,
			     cb->stats.read_bytes,
			     cb->stats.read_msgs);
		} else {
			cc = sprintf(cp, "%d listen\n", num++);
		}
		cp += cc;
	}
	up(&krping_mutex);
	*eof = 1;
	return strlen(page);
}

/*
 * Write proc is used to start a ping client or server.
 */
static int krping_write_proc(struct file *file, const char *buffer,
			     unsigned long count, void *data)
{
	char *cmd;
	int rc;

	cmd = kmalloc(count, GFP_KERNEL);
	if (cmd == NULL) {
		printk(KERN_ERR PFX "kmalloc failure\n");
		return -ENOMEM;
	}
	if (copy_from_user(cmd, buffer, count)) {
		return -EFAULT;
	}

	/*
	 * remove the \n.
	 */
	cmd[count - 1] = 0;
	DEBUG_LOG(KERN_INFO PFX "proc write |%s|\n", cmd);
	rc = krping_doit(cmd);
	kfree(cmd);
	if (rc)
		return rc;
	else
		return (int) count;
}

static int __init krping_init(void)
{
	DEBUG_LOG("krping_init\n");
	krping_proc = create_proc_entry("krping", 0666, NULL);
	if (krping_proc == NULL) {
		printk(KERN_ERR PFX "cannot create /proc/krping\n");
		return -ENOMEM;
	}
	krping_proc->owner = THIS_MODULE;
	krping_proc->read_proc = krping_read_proc;
	krping_proc->write_proc = krping_write_proc;
	return 0;
}

static void __exit krping_exit(void)
{
	DEBUG_LOG("krping_exit\n");
	remove_proc_entry("krping", NULL);
}

module_init(krping_init);
module_exit(krping_exit);
