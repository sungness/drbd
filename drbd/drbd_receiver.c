/*
-*- linux-c -*-
   drbd_receiver.c
   Kernel module for 2.4.x/2.6.x Kernels

   This file is part of drbd by Philipp Reisner.

   Copyright (C) 1999-2006, Philipp Reisner <philipp.reisner@linbit.com>.
   Copyright (C) 2002-2006, Lars Ellenberg <lars.ellenberg@linbit.com>.
   Copyright (C) 2001-2006, LINBIT Information Technologies GmbH.

   drbd is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   drbd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with drbd; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 */


#include <linux/config.h>
#include <linux/module.h>

#include <asm/uaccess.h>
#include <net/sock.h>

#include <linux/tcp.h>

#include <linux/version.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/in.h>
#include <linux/mm.h>
#include <linux/drbd_config.h>
#include <linux/mm_inline.h>
#include <linux/slab.h>
#include <linux/smp_lock.h>
#include <linux/pkt_sched.h>
#define __KERNEL_SYSCALLS__
#include <linux/unistd.h>
#include <linux/vmalloc.h>
#include <linux/random.h>
#include <linux/drbd.h>
#include "drbd_int.h"
#include "drbd_req.h"

#if defined(__arch_um__) && !defined(HAVE_UML_TO_VIRT)
static inline void *to_virt(unsigned long phys)
{
	return((void *) uml_physmem + phys);
}
#endif

#ifdef DBG_ASSERTS
void drbd_assert_breakpoint(drbd_dev *mdev, char *exp,
			    char *file, int line)
{
	ERR("ASSERT( %s ) in %s:%d\n", exp, file, line);
}
#endif


#if 0
#define CHECK_LIST_LIMIT 1000
void check_list(drbd_dev *mdev,struct list_head *list,char *t)
{
	struct list_head *le,*la;
	int forward=0,backward=0;

	le=list;
	do {
		la=le;
		le=le->next;
		if( le->prev != la ) {
			printk(KERN_ERR DEVICE_NAME
			       "%d: %s list fucked.\n",
			       mdev_to_minor(mdev),t);
			break;
		}
		if( forward++ > CHECK_LIST_LIMIT ) {
			printk(KERN_ERR DEVICE_NAME
			       "%d: %s forward > 1000\n",
			       mdev_to_minor(mdev),t);
			break;
		}
	} while(le != list);

	le=list;
	do {
		la=le;
		le=le->prev;
		if( le->next != la ) {
			printk(KERN_ERR DEVICE_NAME
			       "%d: %s list fucked.\n",
			       mdev_to_minor(mdev),t);
			break;
		}
		if( backward++ > CHECK_LIST_LIMIT ) {
			printk(KERN_ERR DEVICE_NAME
			       "%d: %s backward > 1000\n",
			       mdev_to_minor(mdev),t);
			break;
		}
	} while(le != list);

	if(forward != backward) {
		printk(KERN_ERR DEVICE_NAME "%d: forward=%d, backward=%d\n",
		       mdev_to_minor(mdev),forward,backward);
	}
}
#endif

#define GFP_TRY	( __GFP_HIGHMEM | __GFP_NOWARN )

/**
 * drbd_bp_alloc: Returns a page. Fails only if a signal comes in.
 */
STATIC struct page * drbd_pp_alloc(drbd_dev *mdev, unsigned int gfp_mask)
{
	unsigned long flags=0;
	struct page *page;
	DEFINE_WAIT(wait);

	/* FIXME Add some usefull watermark again to "kick_lo", if pages get
	 * used up too quickly. The watermark that had been in place here did
	 * not make sense.
	 */

	spin_lock_irqsave(&drbd_pp_lock,flags);
	/* This lock needs to lock out irq because we might call drdb_pp_free()
	   from IRQ context.
	   FIXME but why irq _save_ ?
	   this is only called from drbd_alloc_ee,
	   and that is strictly process context! */
	if ( (page = drbd_pp_pool) ) {
		drbd_pp_pool = (struct page*)page->U_PRIVATE;
		drbd_pp_vacant--;
	}
	spin_unlock_irqrestore(&drbd_pp_lock,flags);
	if (page) goto got_page;

	drbd_kick_lo(mdev);

	for (;;) {
		prepare_to_wait(&drbd_pp_wait, &wait, TASK_INTERRUPTIBLE);

		/* try the pool again, maybe the drbd_kick_lo set some free */
		spin_lock_irqsave(&drbd_pp_lock,flags);
		if ( (page = drbd_pp_pool) ) {
			drbd_pp_pool = (struct page*)page->U_PRIVATE;
			drbd_pp_vacant--;
		}
		spin_unlock_irqrestore(&drbd_pp_lock,flags);

		if (page) break;

		/* hm. pool was empty. try to allocate from kernel.
		 * don't wait, if none is available, though.
		 */
		if ( atomic_read(&mdev->pp_in_use) < mdev->net_conf->max_buffers ) {
			if( (page = alloc_page(GFP_TRY)) )
				break;
		}

		/* doh. still no page.
		 * either used up the configured maximum number,
		 * or we are low on memory.
		 * wait for someone to return a page into the pool.
		 * unless, of course, someone signalled us.
		 */
		if (signal_pending(current)) {
			WARN("drbd_pp_alloc interrupted!\n");
			finish_wait(&drbd_pp_wait, &wait);
			return NULL;
		}
		drbd_kick_lo(mdev);
		schedule();
	}
	finish_wait(&drbd_pp_wait, &wait);

 got_page:
	atomic_inc(&mdev->pp_in_use);
	return page;
}

STATIC void drbd_pp_free(drbd_dev *mdev,struct page *page)
{
	unsigned long flags=0;
	int free_it;

	spin_lock_irqsave(&drbd_pp_lock,flags);
	if (drbd_pp_vacant > (DRBD_MAX_SEGMENT_SIZE/PAGE_SIZE)*minor_count) {
		free_it = 1;
	} else {
		page->U_PRIVATE = (unsigned long)drbd_pp_pool;
		drbd_pp_pool = page;
		drbd_pp_vacant++;
		free_it = 0;
	}
	spin_unlock_irqrestore(&drbd_pp_lock,flags);

	atomic_dec(&mdev->pp_in_use);

	if(free_it) __free_page(page);

	/*
	 * FIXME
	 * typically there are no waiters.
	 * we should try to avoid any unnecessary call to wake_up.
	 */
	wake_up(&drbd_pp_wait);
}

/*
You need to hold the req_lock:
 drbd_free_ee()
 _drbd_wait_ee_list_empty()

You must not have the req_lock:
 drbd_alloc_ee()
 drbd_init_ee()
 drbd_release_ee()
 drbd_ee_fix_bhs()
 drbd_process_done_ee()
 drbd_clear_done_ee()
 drbd_wait_ee_list_empty()
*/

struct Tl_epoch_entry* drbd_alloc_ee(drbd_dev *mdev,
				     u64 id,
				     sector_t sector,
				     unsigned int data_size,
				     unsigned int gfp_mask)
{
	struct Tl_epoch_entry* e;
	struct bio_vec *bvec;
	struct page *page;
	struct bio *bio;
	unsigned int ds;
	int i;

	e = mempool_alloc(drbd_ee_mempool, gfp_mask);
	if (!e) return NULL;

	bio = bio_alloc(GFP_KERNEL, div_ceil(data_size,PAGE_SIZE));
	if (!bio) goto fail1;

	bio->bi_bdev = mdev->bc->backing_bdev;
	bio->bi_sector = sector;

	ds = data_size;
	while(ds) {
		page = drbd_pp_alloc(mdev, gfp_mask);
		if (!page) goto fail2;
		if (!bio_add_page(bio, page, min_t(int, ds, PAGE_SIZE), 0)) {
			drbd_pp_free(mdev,page);
			goto fail2;
			break;
		}
		ds -= min_t(int, ds, PAGE_SIZE);
	}

	D_ASSERT( data_size == bio->bi_size);

	bio->bi_private = e;
	e->mdev = mdev;
	e->sector = sector;
	e->size = bio->bi_size;

	e->private_bio = bio;
	e->block_id = id;
	INIT_HLIST_NODE(&e->colision);
	e->barrier_nr = 0;
	e->barrier_nr2 = 0;

	return e;

 fail2:
	__bio_for_each_segment(bvec, bio, i, 0) {
		drbd_pp_free(mdev,bvec->bv_page);
	}
	bio_put(bio);
 fail1:
	mempool_free(e, drbd_ee_mempool);

	return NULL;
}

void drbd_free_ee(drbd_dev *mdev, struct Tl_epoch_entry* e)
{
	struct bio *bio=e->private_bio;
	struct bio_vec *bvec;
	int i;

	__bio_for_each_segment(bvec, bio, i, 0) {
		drbd_pp_free(mdev,bvec->bv_page);
	}

	bio_put(bio);

	mempool_free(e, drbd_ee_mempool);
}

/* currently on module unload only */
int drbd_release_ee(drbd_dev *mdev,struct list_head* list)
{
	int count=0;
	struct Tl_epoch_entry* e;
	struct list_head *le;

	spin_lock_irq(&mdev->req_lock);
	while(!list_empty(list)) {
		le = list->next;
		e = list_entry(le, struct Tl_epoch_entry, w.list);
		drbd_free_ee(mdev,e);
		count++;
	}
	spin_unlock_irq(&mdev->req_lock);

	return count;
}


STATIC void reclaim_net_ee(drbd_dev *mdev)
{
	struct Tl_epoch_entry *e;
	struct list_head *le,*tle;

	/* The EEs are always appended to the end of the list. Since
	   they are sent in order over the wire, they have to finish
	   in order. As soon as we see the first not finished we can
	   stop to examine the list... */

	list_for_each_safe(le, tle, &mdev->net_ee) {
		e = list_entry(le, struct Tl_epoch_entry, w.list);
		if( drbd_bio_has_active_page(e->private_bio) ) break;
		list_del(le);
		drbd_free_ee(mdev,e);
	}
}


/*
 * This function is called from _asender only_
 *
 * Move entries from net_ee to done_ee, if ready.
 * Grab done_ee, call all callbacks, free the entries.
 * The callbacks typically send out ACKs.
 */
STATIC int drbd_process_done_ee(drbd_dev *mdev)
{
	LIST_HEAD(work_list);
	struct Tl_epoch_entry *e, *t;
	int ok=1;

	spin_lock_irq(&mdev->req_lock);
	reclaim_net_ee(mdev);
	list_splice_init(&mdev->done_ee,&work_list);
	spin_unlock_irq(&mdev->req_lock);

	/* XXX maybe wake_up here already?
	 * or wake_up withing drbd_free_ee just after mempool_free?
	 */

	/* possible callbacks here:
	 * e_end_block, and e_end_resync_block.
	 * both ignore the last argument.
	 */
	list_for_each_entry_safe(e, t, &work_list, w.list) {
		// list_del not necessary, next/prev members not touched
		ok = ok && e->w.cb(mdev,&e->w,0);
		drbd_free_ee(mdev,e);
	}
	wake_up(&mdev->ee_wait);

	return ok;
}



/* clean-up helper for drbd_disconnect */
void _drbd_clear_done_ee(drbd_dev *mdev)
{
	struct list_head *le;
	struct Tl_epoch_entry *e;
	int n = 0;

	MUST_HOLD(&mdev->ee_lock);

	reclaim_net_ee(mdev);

	while(!list_empty(&mdev->done_ee)) {
		le = mdev->done_ee.next;
		list_del(le);
		e = list_entry(le, struct Tl_epoch_entry, w.list);
		if(mdev->net_conf->wire_protocol == DRBD_PROT_C ||
		   is_syncer_block_id(e->block_id)) {
			++n;
		}
		drbd_free_ee(mdev,e);
	}

	sub_unacked(mdev, n);
}

void _drbd_wait_ee_list_empty(drbd_dev *mdev,struct list_head *head)
{
	DEFINE_WAIT(wait);
	MUST_HOLD(&mdev->req_lock);

	/* avoids spin_lock/unlock and calling prepare_to_wait in the fast path */
	while (!list_empty(head)) {
		prepare_to_wait(&mdev->ee_wait,&wait,TASK_UNINTERRUPTIBLE);
		spin_unlock_irq(&mdev->req_lock);
		drbd_kick_lo(mdev);
		schedule();
		finish_wait(&mdev->ee_wait, &wait);
		spin_lock_irq(&mdev->req_lock);
	}
}

void drbd_wait_ee_list_empty(drbd_dev *mdev,struct list_head *head)
{
	spin_lock_irq(&mdev->req_lock);
	_drbd_wait_ee_list_empty(mdev, head);
	spin_unlock_irq(&mdev->req_lock);
}

STATIC struct socket* drbd_accept(drbd_dev *mdev,struct socket* sock)
{
	struct socket *newsock;
	int err = 0;

	err = sock->ops->listen(sock, 5);
	if (err)
		goto out;

	if (sock_create_kern(AF_INET, SOCK_STREAM, IPPROTO_TCP, &newsock))
		goto out;

	newsock->type = sock->type;
	newsock->ops  = sock->ops;

	err = newsock->ops->accept(sock, newsock, 0);
	if (err < 0)
		goto out_release;

	return newsock;

      out_release:
	sock_release(newsock);
      out:
	if(err != -EAGAIN && err != -EINTR)
		ERR("accept failed! %d\n", err);
	return 0;
}

STATIC int drbd_recv_short(drbd_dev *mdev, struct socket *sock,
			   void *buf, size_t size)
{
	mm_segment_t oldfs;
	struct iovec iov;
	struct msghdr msg;
	int rv;

	if (unlikely(drbd_did_panic == DRBD_MAGIC)) {
		drbd_suicide();
	}

	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_iovlen = 1;
	msg.msg_iov = &iov;
	iov.iov_len = size;
	iov.iov_base = buf;
	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_flags = MSG_WAITALL | MSG_NOSIGNAL;

	oldfs = get_fs();
	set_fs(KERNEL_DS);

	rv = sock_recvmsg(sock, &msg, size, msg.msg_flags);

	set_fs(oldfs);

	return rv;
}

int drbd_recv(drbd_dev *mdev,void *buf, size_t size)
{
	mm_segment_t oldfs;
	struct iovec iov;
	struct msghdr msg;
	int rv;

	if (unlikely(drbd_did_panic == DRBD_MAGIC)) {
		drbd_suicide();
	}

	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_iovlen = 1;
	msg.msg_iov = &iov;
	iov.iov_len = size;
	iov.iov_base = buf;
	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_flags = MSG_WAITALL | MSG_NOSIGNAL;

	oldfs = get_fs();
	set_fs(KERNEL_DS);

	for(;;) {
		rv = sock_recvmsg(mdev->data.socket,&msg,size,msg.msg_flags);
		if (rv == size) break;

		/* Note:
		 * ECONNRESET   other side closed the connection
		 * ERESTARTSYS  (on  sock) we got a signal
		 */

		if (rv < 0) {
			if (rv == -ECONNRESET)
				INFO("sock was reset by peer\n");
			else if (rv != -ERESTARTSYS)
				ERR("sock_recvmsg returned %d\n",rv);
			break;
		} else if (rv == 0) {
			INFO("sock was shut down by peer\n");
			break;
		} else  {
			/* signal came in, or peer/link went down,
			 * after we read a partial message
			 */
			// D_ASSERT(signal_pending(current));
			break;
		}
	};

	set_fs(oldfs);

	if(rv != size) {
		drbd_force_state(mdev,NS(conn,BrokenPipe));
		drbd_thread_restart_nowait(&mdev->receiver);
	}

	return rv;
}

STATIC struct socket *drbd_try_connect(drbd_dev *mdev)
{
	int err;
	struct socket *sock;
	struct sockaddr_in src_in;

	err = sock_create_kern(AF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
	if (err) {
		ERR("sock_creat(..)=%d\n", err);
		return NULL;
	}

	if(!inc_net(mdev)) return NULL;

	sock->sk->sk_rcvtimeo =
	sock->sk->sk_sndtimeo =  mdev->net_conf->try_connect_int*HZ;

       /* explicitly bind to the configured IP as source IP 
	   for the outgoing connections.
	   This is needed for multihomed hosts and to be 
	   able to use lo: interfaces for drbd.
          Make sure to use 0 as portnumber, so linux selects
	   a free one dynamically.
	*/
	memcpy (&src_in, &(mdev->net_conf->my_addr), sizeof(struct sockaddr_in));
	src_in.sin_port = 0; 

	err = sock->ops->bind(sock,
			      (struct sockaddr * ) &src_in,
			      sizeof (struct sockaddr_in));
	if (err) {
		ERR("Unable to bind source sock (%d)\n", err);
		sock_release(sock);
		sock = NULL;
		return sock;
	}

	err = sock->ops->connect(sock,
				 (struct sockaddr *)mdev->net_conf->peer_addr,
				 mdev->net_conf->peer_addr_len, 0);

	if (err) {
		sock_release(sock);
		sock = NULL;
	}

	dec_net(mdev);
	return sock;
}

STATIC struct socket *drbd_wait_for_connect(drbd_dev *mdev)
{
	int err;
	struct socket *sock,*sock2;

	err = sock_create_kern(AF_INET, SOCK_STREAM, IPPROTO_TCP, &sock2);
	if (err) {
		ERR("sock_creat(..)=%d\n", err);
		return NULL;
	}

	if(!inc_net(mdev)) return NULL;

	sock2->sk->sk_reuse    = 1; /* SO_REUSEADDR */
	sock2->sk->sk_rcvtimeo =
	sock2->sk->sk_sndtimeo =  mdev->net_conf->try_connect_int*HZ;

	err = sock2->ops->bind(sock2,
			      (struct sockaddr *) mdev->net_conf->my_addr,
			      mdev->net_conf->my_addr_len);
	dec_net(mdev);

	if (err) {
		ERR("Unable to bind sock2 (%d)\n", err);
		sock_release(sock2);
		drbd_force_state(mdev,NS(conn,StandAlone));
		return NULL;
	}

	sock = drbd_accept(mdev,sock2);
	sock_release(sock2);

	return sock;
}

STATIC int drbd_do_handshake(drbd_dev *mdev);
STATIC int drbd_do_auth(drbd_dev *mdev);

STATIC int drbd_send_fp(drbd_dev *mdev,struct socket *sock,Drbd_Packet_Cmd cmd)
{
	Drbd_Header *h = (Drbd_Header *) &mdev->data.sbuf.head;

	return _drbd_send_cmd(mdev,sock,cmd,h,sizeof(*h),0);
}

STATIC Drbd_Packet_Cmd drbd_recv_fp(drbd_dev *mdev,struct socket *sock)
{
	Drbd_Header *h = (Drbd_Header *) &mdev->data.sbuf.head;
	int rr;

	rr = drbd_recv_short(mdev, sock, h, sizeof(*h));

	if( rr==sizeof(*h) && h->magic==BE_DRBD_MAGIC ) {
		return be16_to_cpu(h->command);
	}

	return 0xffff;
}

/*
 * return values:
 *   1 yess, we have a valid connection
 *   0 oops, did not work out, please try again
 *  -1 peer talks different language,
 *     no point in trying again, please go standalone.
 */
int drbd_connect(drbd_dev *mdev)
{
	struct socket *s, *sock,*msock;
	int try,h;

	D_ASSERT(mdev->state.conn > StandAlone);
	D_ASSERT(!mdev->data.socket);

	if(drbd_request_state(mdev,NS(conn,WFConnection)) < SS_Success ) return 0;
	clear_bit(UNIQUE, &mdev->flags);

	sock  = NULL;
	msock = NULL;

	do {
		for(try=0;;) { // 3 tries, this should take less than a second!
			s=drbd_try_connect(mdev);
			if(s || ++try >= 3 ) break;
			// give the other side time to call bind() & listen()
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ / 10);
		}

		if(s) {
			if( !sock ) {
				if( drbd_send_fp(mdev, s, HandShakeS) ) {
					sock = s;
					s = NULL;
				}
			} else if( !msock ) {
				if( drbd_send_fp(mdev, s, HandShakeM) ) {
					msock = s;
					s = NULL;
				}
			} else {
				ERR("Logic error in drbd_connect()\n");
				return -1;
			}
			if(s) {
				ERR("Error during sending initial packet.\n");
				sock_release(s);
			}
		}

		if(sock && msock) break;

		s=drbd_wait_for_connect(mdev);		
		if(s) {
			switch(drbd_recv_fp(mdev,s)) {
			case HandShakeS:
				if(sock) sock_release(sock);
				sock = s;
				break;
			case HandShakeM:
				if(msock) sock_release(msock);
				msock = s;
				set_bit(UNIQUE, &mdev->flags);
				break;
			default:
				WARN("Error receiving initial packet\n");
				sock_release(s);
			}
		}

		if(mdev->state.conn == StandAlone) return -1;
		if(signal_pending(current)) {
			flush_signals(current);
			smp_rmb();
			if (get_t_state(&mdev->receiver) == Exiting) {
				if(sock) sock_release(sock);
				if(msock) sock_release(msock);
				return -1;
			}
		}

	} while( !sock || !msock );

	msock->sk->sk_reuse=1; /* SO_REUSEADDR */
	sock->sk->sk_reuse=1; /* SO_REUSEADDR */

	sock->sk->sk_allocation = GFP_NOIO;
	msock->sk->sk_allocation = GFP_NOIO;

	sock->sk->sk_priority=TC_PRIO_BULK;
	tcp_sk(sock->sk)->nonagle = 0;
	// FIXME fold to limits. should be done in drbd_ioctl
	sock->sk->sk_sndbuf = mdev->net_conf->sndbuf_size;
	sock->sk->sk_rcvbuf = mdev->net_conf->sndbuf_size;
	/* NOT YET ...
	 * sock->sk->sk_sndtimeo = mdev->net_conf->timeout*HZ/20;
	 * sock->sk->sk_rcvtimeo = MAX_SCHEDULE_TIMEOUT;
	 * first set it to the HandShake timeout, wich is hardcoded for now: */
	sock->sk->sk_sndtimeo =
	sock->sk->sk_rcvtimeo = 2*HZ;
	sock->sk->sk_userlocks |= SOCK_SNDBUF_LOCK | SOCK_RCVBUF_LOCK;

	msock->sk->sk_priority=TC_PRIO_INTERACTIVE;
	tcp_sk(sock->sk)->nonagle = 1;
	msock->sk->sk_sndbuf = 2*32767;
	msock->sk->sk_sndtimeo = mdev->net_conf->timeout*HZ/20;
	msock->sk->sk_rcvtimeo = mdev->net_conf->ping_int*HZ;

	mdev->data.socket = sock;
	mdev->meta.socket = msock;
	mdev->last_received = jiffies;

	if(drbd_request_state(mdev,NS(conn,WFReportParams)) < SS_Success) return 0;
	D_ASSERT(mdev->asender.task == NULL);

	h = drbd_do_handshake(mdev);
	if (h <= 0) return h;

	if ( mdev->cram_hmac_tfm ) {
		if (!drbd_do_auth(mdev)) {
			ERR("Authentication of peer failed\n");
			return 0;
		}
	}

	sock->sk->sk_sndtimeo = mdev->net_conf->timeout*HZ/20;
	sock->sk->sk_rcvtimeo = MAX_SCHEDULE_TIMEOUT;

	atomic_set(&mdev->packet_seq,0);
	mdev->peer_seq=0;

	drbd_thread_start(&mdev->asender);

	drbd_send_protocol(mdev);
	drbd_send_sync_param(mdev,&mdev->sync_conf);
	drbd_send_sizes(mdev);
	drbd_send_uuids(mdev);
	drbd_send_state(mdev);
	clear_bit(USE_DEGR_WFC_T,&mdev->flags);

	return 1;
}

STATIC int drbd_recv_header(drbd_dev *mdev, Drbd_Header *h)
{
	int r;

	r = drbd_recv(mdev,h,sizeof(*h));

	if (unlikely( r != sizeof(*h) )) {
		ERR("short read expecting header on sock: r=%d\n",r);
		return FALSE;
	};
	h->command = be16_to_cpu(h->command);
	h->length  = be16_to_cpu(h->length);
	if (unlikely( h->magic != BE_DRBD_MAGIC )) {
		ERR("magic?? on data m: 0x%lx c: %d l: %d\n",
		    (long)be32_to_cpu(h->magic),
		    h->command, h->length);
		return FALSE;
	}
	mdev->last_received = jiffies;

	return TRUE;
}

#if 0
STATIC int receive_Barrier_tcq(drbd_dev *mdev, Drbd_Header* h)
{
	int rv;
	int epoch_size=0;
	Drbd_Barrier_Packet *p = (Drbd_Barrier_Packet*)h;

	ERR_IF(h->length != (sizeof(*p)-sizeof(*h))) return FALSE;

	rv = drbd_recv(mdev, h->payload, h->length);
	ERR_IF(rv != h->length) return FALSE;

	inc_unacked(mdev);

	spin_lock_irq(&mdev->ee_lock);
	if(list_empty(&mdev->active_ee)) {
		epoch_size = mdev->epoch_size;
		mdev->epoch_size = 0;
	} else if (mdev->last_write_w_barrier) {
		mdev->last_write_w_barrier->barrier_nr2 = be32_to_cpu(p->barrier);
	} else {
		mdev->next_barrier_nr = be32_to_cpu(p->barrier);
	}
	spin_unlock_irq(&mdev->ee_lock);

	if(epoch_size) {
		rv = drbd_send_b_ack(mdev, p->barrier, epoch_size);
		dec_unacked(mdev);
	}

	return rv;
}
#endif

STATIC int receive_Barrier_no_tcq(drbd_dev *mdev, Drbd_Header* h)
{
	int rv;
	int epoch_size;
	Drbd_Barrier_Packet *p = (Drbd_Barrier_Packet*)h;

	ERR_IF(h->length != (sizeof(*p)-sizeof(*h))) return FALSE;

	rv = drbd_recv(mdev, h->payload, h->length);
	ERR_IF(rv != h->length) return FALSE;

	inc_unacked(mdev);

	if (mdev->net_conf->wire_protocol != DRBD_PROT_C)
		drbd_kick_lo(mdev);

	spin_lock_irq(&mdev->req_lock);
	_drbd_wait_ee_list_empty(mdev,&mdev->active_ee);
	epoch_size = mdev->epoch_size;
	mdev->epoch_size = 0;
	spin_unlock_irq(&mdev->req_lock);

	rv = drbd_send_b_ack(mdev, p->barrier, epoch_size);
	dec_unacked(mdev);

	return rv;
}

/* used from receive_RSDataReply (recv_resync_read)
 * and from receive_Data */
STATIC struct Tl_epoch_entry *
read_in_block(drbd_dev *mdev, u64 id, sector_t sector, int data_size)
{
	struct Tl_epoch_entry *e;
	struct bio_vec *bvec;
	struct page *page;
	struct bio *bio;
	int ds,i,rr;

	e = drbd_alloc_ee(mdev,id,sector,data_size,GFP_KERNEL);
	if(!e) return 0;
	bio = e->private_bio;
	ds = data_size;
	bio_for_each_segment(bvec, bio, i) {
		page = bvec->bv_page;
		rr = drbd_recv(mdev,kmap(page),min_t(int,ds,PAGE_SIZE));
		kunmap(page);
		if( rr != min_t(int,ds,PAGE_SIZE) ) {
			drbd_free_ee(mdev,e);
			WARN("short read receiving data: read %d expected %d\n",
			     rr, min_t(int,ds,PAGE_SIZE));
			return 0;
		}
		ds -= rr;
	}

	mdev->recv_cnt+=data_size>>9;
	return e;
}

/* kick lower level device, if we have more than (arbitrary number)
 * reference counts on it, which typically are locally submitted io
 * requests.  don't use unacked_cnt, so we speed up proto A and B, too. */
static void maybe_kick_lo(drbd_dev *mdev)
{
	if (atomic_read(&mdev->local_cnt) >= mdev->net_conf->unplug_watermark ) {
		/* FIXME hysteresis ?? */
		drbd_kick_lo(mdev);
	}
}

STATIC int recv_dless_read(drbd_dev *mdev, drbd_request_t *req,
			   sector_t sector, int data_size)
{
	struct bio_vec *bvec;
	struct bio *bio;
	int rr,i,expect;

	bio = req->master_bio;
	D_ASSERT( sector == bio->bi_sector );

	bio_for_each_segment(bvec, bio, i) {
		expect = min_t(int,data_size,bvec->bv_len);
		rr=drbd_recv(mdev,
			     kmap(bvec->bv_page)+bvec->bv_offset,
			     expect);
		kunmap(bvec->bv_page);
		if (rr != expect) {
			WARN("short read receiving data reply: read %d expected %d\n",
			     rr, expect);
			return 0;
		}
		data_size -= rr;
	}

	D_ASSERT(data_size == 0);
	/* FIXME recv_cnt accounting ?? */
	return 1;
}

/* e_end_resync_block() is called via
 * drbd_process_done_ee() or _drbd_clear_done_ee().
 * only runs in the asender thread
 */
STATIC int e_end_resync_block(drbd_dev *mdev, struct drbd_work *w, int unused)
{
	struct Tl_epoch_entry *e = (struct Tl_epoch_entry*)w;
	sector_t sector = e->sector;
	int ok;

	D_ASSERT(hlist_unhashed(&e->colision));

	/* before set_in_sync()
	 * FIXME because ... */
	drbd_rs_complete_io(mdev,sector);
	if (likely( drbd_bio_uptodate(e->private_bio) )) {
		/* "optimization" only...  state could still change anytime
		 * while we are calling drbd_set_in_sync */
		ok = mdev->state.disk >= Inconsistent &&
			mdev->state.pdsk >= Inconsistent;
		if (likely( ok )) {
			drbd_set_in_sync(mdev, sector, e->size);
			ok = drbd_send_ack(mdev,WriteAck,e);
		} else {
			/* FIXME think:
			 * send a WriteAck anyways?
			 * send a NegAck?
			 * just ignore it?  (ignoring it is valid, peer has no
			 * structs referencing this) */
		}
		/* FIXME what exactly do we need this flag for, again??
		 * and why do we set it only in the "up-to-date" branch? */
		set_bit(SYNC_STARTED,&mdev->flags);
	} else {
		ok = drbd_send_ack(mdev,NegAck,e);
		ok&= drbd_io_error(mdev);
	}
	dec_unacked(mdev);

	return ok;
}

STATIC int recv_resync_read(drbd_dev *mdev,sector_t sector, int data_size)
{
	struct Tl_epoch_entry *e;

	e = read_in_block(mdev,ID_SYNCER,sector,data_size);
	if(!e) return FALSE;

	dec_rs_pending(mdev);

	drbd_ee_prepare_write(mdev,e);
	e->w.cb     = e_end_resync_block;

	inc_unacked(mdev);
	/* corresponding dec_unacked() in e_end_resync_block()
	 * respective _drbd_clear_done_ee */

	spin_lock_irq(&mdev->req_lock);
	list_add(&e->w.list,&mdev->sync_ee);
	spin_unlock_irq(&mdev->req_lock);

	drbd_generic_make_request(WRITE,e->private_bio);
	/* accounting done in endio */

	maybe_kick_lo(mdev);
	return TRUE;
}

STATIC int receive_DataReply(drbd_dev *mdev,Drbd_Header* h)
{
	drbd_request_t *req;
	sector_t sector;
	unsigned int header_size,data_size;
	int ok;
	Drbd_Data_Packet *p = (Drbd_Data_Packet*)h;

	header_size = sizeof(*p) - sizeof(*h);
	data_size   = h->length  - header_size;

	/* I expect a block to be a multiple of 512 byte,
	 * and no more than DRBD_MAX_SEGMENT_SIZE.
	 * is this too restrictive?  */
	ERR_IF(data_size == 0) return FALSE;
	ERR_IF(data_size &  0x1ff) return FALSE;
	ERR_IF(data_size >  DRBD_MAX_SEGMENT_SIZE) return FALSE;

	if (drbd_recv(mdev, h->payload, header_size) != header_size)
		return FALSE;

	sector = be64_to_cpu(p->sector);

	spin_lock_irq(&mdev->req_lock);
	req = _ar_id_to_req(mdev,p->block_id, sector);
	spin_unlock_irq(&mdev->req_lock);
	if (unlikely(!req)) {
		ERR("Got a corrupt block_id/sector pair(1).\n");
		return FALSE;
	}

	/* hlist_del(&req->colision) is done in _req_may_be_done, to avoid
	 * special casing it there for the various failure cases.
	 * still no race with drbd_fail_pending_reads */
	ok = recv_dless_read(mdev,req,sector,data_size);

	if (ok) req_mod(req, data_received);
	/* else: nothing. handled from drbd_disconnect...
	 * I don't think we may complete this just yet
	 * in case we are "on-disconnect: freeze" */

	return ok;
}

STATIC int receive_RSDataReply(drbd_dev *mdev,Drbd_Header* h)
{
	sector_t sector;
	unsigned int header_size,data_size;
	int ok;
	Drbd_Data_Packet *p = (Drbd_Data_Packet*)h;

	header_size = sizeof(*p) - sizeof(*h);
	data_size   = h->length  - header_size;

	/* I expect a block to be a multiple of 512 byte,
	 * and no more than DRBD_MAX_SEGMENT_SIZE.
	 * is this too restrictive?  */
	ERR_IF(data_size == 0) return FALSE;
	ERR_IF(data_size &  0x1ff) return FALSE;
	ERR_IF(data_size >  DRBD_MAX_SEGMENT_SIZE) return FALSE;

	if (drbd_recv(mdev, h->payload, header_size) != header_size)
		return FALSE;

	sector = be64_to_cpu(p->sector);
	D_ASSERT(p->block_id == ID_SYNCER);

	if(inc_local(mdev)) {
		/* data is submitted to disk within recv_resync_read.
		 * corresponding dec_local done below on error,
		 * or in drbd_endio_write_sec. */
		/* FIXME paranoia:
		 * verify that the corresponding bit is set.
		 * in case we are Primary SyncTarget,
		 * verify there are no pending write request to that area.
		 */
		ok = recv_resync_read(mdev,sector,data_size);
		if (!ok) dec_local(mdev);
	} else {
		if (DRBD_ratelimit(5*HZ,5))
			ERR("Can not write resync data to local disk.\n");
		drbd_send_ack_dp(mdev,NegAck,p);
		/* FIXME:
		 * we need to drain the data.  only then can we keep the
		 * connection open.
		 * without draining, we'd see an invalid packet header next,
		 * and drop the connection there. */
		/* ok = 1; not yet: keep connection open */
		ok = 0;
	}

	return ok;
}

/* e_end_block() is called via drbd_process_done_ee().
 * this means this function only runs in the asender thread
 *
 * for a broken example implementation of the TCQ barrier version of
 * e_end_block see older revisions...
 */
STATIC int e_end_block(drbd_dev *mdev, struct drbd_work *w, int unused)
{
	struct Tl_epoch_entry *e = (struct Tl_epoch_entry*)w;
	sector_t sector = e->sector;
	// unsigned int epoch_size;
	int ok=1;

	if(mdev->net_conf->wire_protocol == DRBD_PROT_C) {
		if(likely(drbd_bio_uptodate(e->private_bio))) {
			ok &= drbd_send_ack(mdev,WriteAck,e);
			if (test_bit(SYNC_STARTED,&mdev->flags))
				drbd_set_in_sync(mdev,sector,e->size);
		} else {
			/* FIXME I think we should send a NegAck regardless of
			 * which protocol is in effect.
			 * In which case we would need to make sure that any
			 * NegAck is sent. basically that means that drbd_process_done_ee
			 * may not list_del() the ee before this callback did run...
			 * maybe even move the list_del(e) in here... */
			ok = drbd_send_ack(mdev,NegAck,e);
			ok&= drbd_io_error(mdev);
			/* we expect it to be marked out of sync anyways...
			 * maybe assert this?  */
		}
		dec_unacked(mdev);
		return ok;
	} else if(unlikely(!drbd_bio_uptodate(e->private_bio))) {
		ok = drbd_io_error(mdev);
	}

// warning LGE "FIXME code missing"
#if 0
	/* we delete from the conflict detection hash _after_ we sent out the
	 * WriteAck / NegAck, to get the sequence number right.  */
	D_ASSERT(!hlist_unhashed(&e->colision));
	/* FIXME "wake" any conflicting requests
	 * that have been waiting for this one to finish */
	hlist_del_init(&e->colision);
#endif

	return ok;
}

/* FIXME implementation wrong.
 * For the algorithm to be correct, we need to send and store the
 * sector and size, not the block id. We have to check for overlap.
 * We may _only_ remove the info when its sequence number is less than
 * the current sequence number.
 *
 * I think the "discard info" are the wrong way, anyways.
 * Instead of silently discarding such writes, we should send a DiscardAck,
 * and we should retard sending of the data until we get that Discard Ack
 * and thus the conflicting request is done.
 */
STATIC int drbd_chk_discard(drbd_dev *mdev,struct Tl_epoch_entry *e)
{
	struct drbd_discard_note *dn;
	struct list_head *le,*tmp;

	MUST_HOLD(&mdev->peer_seq_lock);
	list_for_each_safe(le,tmp,&mdev->discard) {
		dn = list_entry(le, struct drbd_discard_note, list);
		if( dn->seq_num == mdev->peer_seq ) {
			D_ASSERT( dn->block_id == e->block_id );
			list_del(le);
			kfree(dn);
			return 1;
		}
		if( dn->seq_num < mdev->peer_seq ) {
			list_del(le);
			kfree(dn);
		}
	}
	return 0;
}

// mirrored write
STATIC int receive_Data(drbd_dev *mdev,Drbd_Header* h)
{
	sector_t sector;
	struct Tl_epoch_entry *e;
	/* FIXME currently some unused variable intended
	 * for the now not-implemented conflict detection */
	drbd_request_t * req;
	Drbd_Data_Packet *p = (Drbd_Data_Packet*)h;
	int header_size, data_size, packet_seq, discard, rv;
	unsigned int barrier_nr = 0;
	unsigned int epoch_size = 0;

	// FIXME merge this code dups into some helper function
	header_size = sizeof(*p) - sizeof(*h);
	data_size   = h->length  - header_size;

	ERR_IF(data_size == 0) return FALSE;
	ERR_IF(data_size &  0x1ff) return FALSE;
	ERR_IF(data_size >  DRBD_MAX_SEGMENT_SIZE) return FALSE;

	if (drbd_recv(mdev, h->payload, header_size) != header_size)
		return FALSE;

	if(!inc_local(mdev)) {
		/* data is submitted to disk at the end of this function.
		 * corresponding dec_local done either below (on error),
		 * or in drbd_endio_write_sec. */
		if (DRBD_ratelimit(5*HZ,5))
			ERR("Can not write mirrored data block to local disk.\n");
		drbd_send_ack_dp(mdev,NegAck,p);
		return TRUE;
	}

	sector = be64_to_cpu(p->sector);
	e = read_in_block(mdev,p->block_id,sector,data_size);
	if (!e) {
		dec_local(mdev);
		return FALSE;
	}

	drbd_ee_prepare_write(mdev, e);
	e->w.cb     = e_end_block;

	/* FIXME drbd_al_begin_io in case we have two primaries... */

// warning LGE "FIXME code missing"
#if 0 
/* sorry.
 * to get this patch in a shape where it can be committed,
 * I need to disable the broken conflict detection code for now.
 * will implement the correct as soon as possible...
 * it is done in my head already, "only have to write it down",
 * which will take an other couple of days, probably.
 */

	/* This wait_event is here so even when a DATA packet traveling via
	 * sock overtook an ACK packet traveling on msock, they are still
	 * processed in the order they have been sent.
	 * FIXME TODO: Wrap around of seq_num !!!
	 */
	if (mdev->net_conf->two_primaries) {
		packet_seq = be32_to_cpu(p->seq_num);
		if( wait_event_interruptible(mdev->cstate_wait,
					     packet_seq <= peer_seq(mdev)+1)) {
			rv = FALSE;
			goto out2;
		}

		/* FIXME current discard implementation is wrong */
		spin_lock(&mdev->peer_seq_lock);
		mdev->peer_seq = max(mdev->peer_seq, packet_seq);
		/* is update_peer_seq(mdev,packet_seq); */
		discard = drbd_chk_discard(mdev,e);
		spin_unlock(&mdev->peer_seq_lock);

		if(discard) {
			WARN("Concurrent write! [DISCARD BY LIST] sec=%lu\n",
			     (unsigned long)sector);
			rv = TRUE;
			goto out2;
		}

		req = req_have_write(mdev, e);

		if(req) {
			/* FIXME RACE
			 * rq_status may be changing while we are looking.
			 * in rare cases it could even disappear right now.
			 * e.g. when it has already been ACK'ed, and the local
			 * storage has been way too slow, and only now
			 * completes the thing.
			 */
			if( req->rq_status & RQ_DRBD_SENT ) {
				/* Conflicting write, got ACK */
				/* write afterwards ...*/
				WARN("Concurrent write! [W AFTERWARDS1] "
				     "sec=%lu\n",(unsigned long)sector);
				if( wait_event_interruptible(mdev->cstate_wait,
					       !req_have_write(mdev,e))) {
					rv = FALSE;
					goto out2;
				}
			} else {
				/* Conflicting write, no ACK by now*/
				if (test_bit(UNIQUE,&mdev->flags)) {
					WARN("Concurrent write! [DISCARD BY FLAG] sec=%lu\n",
					     (unsigned long)sector);
					rv = TRUE;
					goto out2;
				} else {
					/* write afterwards do not exp ACK */
					WARN("Concurrent write! [W AFTERWARDS2] sec=%lu\n",
					     (unsigned long)sector);
					drbd_send_discard(mdev,req);
					drbd_end_req(req, RQ_DRBD_SENT, 1, sector);
					dec_ap_pending(mdev);
					if( wait_event_interruptible(mdev->cstate_wait,
								     !req_have_write(mdev,e))) {
						rv = FALSE;
						goto out2;
					}
				}
			}
		}
	}
#endif

	if ( be32_to_cpu(p->dp_flags) & DP_HARDBARRIER ) {
		e->private_bio->bi_rw |= BIO_RW_BARRIER;
	}

	/* when using TCQ:
	 * note that, when using tagged command queuing, we may
	 * have more than one reorder domain "active" at a time.
	 *
	 * THINK:
	 * do we have any guarantees that we get the completion
	 * events of the different reorder domains in order?
	 * or does the api only "guarantee" that the events
	 * _happened_ in order, but eventually the completion
	 * callbacks are shuffeled again?
	 *
	 * note that I wonder about the order in which the
	 * callbacks are run, I am reasonable confident that the
	 * actual completion happens in order.
	 *
	 * - can it happen that the tagged write completion is
	 *   called even though not all of the writes before it
	 *   have run their completion callback?
	 * - can it happen that some completion callback of some
	 *   write after the tagged one is run, even though the
	 *   callback of the tagged one itself is still pending?
	 *
	 * if this can happen, we either need to drop our "debug
	 * assertion" about the epoch size and just trust our code
	 * and the layers below us (nah, won't do that).
	 *
	 * or we need to replace the "active_ee" list by some sort
	 * of "transfer log" on the receiving side, too, which
	 * uses epoch counters per reorder domain.
	 */

	/* when using tcq:
	 * if we got a barrier packet before, but at that time the active_ee
	 * was not yet empty, we just "remembered" this barrier request.
	 *
	 * if this is the first data packet since that barrier, maybe meanwhile
	 * all previously active writes have been completed?
	 * if so, send the b_ack right now
	 * (though, maybe rather move it into the e_end_block callback,
	 * where it would be sent as soon as possible).
	 *
	 * otherwise, tag the write with the barrier number, so it
	 * will trigger the b_ack before its own ack.
	 */

	spin_lock_irq(&mdev->req_lock);
	if (mdev->next_barrier_nr) {
		/* only when using TCQ */
		if (list_empty(&mdev->active_ee)) {
			barrier_nr = mdev->next_barrier_nr;
			epoch_size = mdev->epoch_size;
			mdev->epoch_size = 0;
		} else {
			e->barrier_nr = mdev->next_barrier_nr;
		}
		e->private_bio->bi_rw |= BIO_RW_BARRIER;
		mdev->next_barrier_nr = 0;
	}
	list_add(&e->w.list,&mdev->active_ee);
	spin_unlock_irq(&mdev->req_lock);

	if (barrier_nr) {
		/* only when using TCQ
		 * maybe rather move it into the e_end_block callback,
		 * where it would be sent as soon as possible).
		 */
		(void)drbd_send_b_ack(mdev, cpu_to_be32(barrier_nr), epoch_size);
	}

	switch(mdev->net_conf->wire_protocol) {
	case DRBD_PROT_C:
		inc_unacked(mdev);
		/* corresponding dec_unacked() in e_end_block()
		 * respective _drbd_clear_done_ee */
		break;
	case DRBD_PROT_B:
		/* I really don't like it that the receiver thread
		 * sends on the msock, but anyways */
		drbd_send_ack(mdev, RecvAck, e);
		break;
	case DRBD_PROT_A:
		// nothing to do
		break;
	}

	/* FIXME drbd_al_begin_io in case we have two primaries... */
	drbd_generic_make_request(WRITE,e->private_bio);
	/* accounting done in endio */

	maybe_kick_lo(mdev);
	return TRUE;

 out2:
	/* yes, the epoch_size now is imbalanced.
	 * but we drop the connection anyways, so we don't have a chance to
	 * receive a barrier... atomic_inc(&mdev->epoch_size); */
	dec_local(mdev);
	drbd_free_ee(mdev,e);
	return rv;
}

STATIC int receive_DataRequest(drbd_dev *mdev,Drbd_Header *h)
{
	sector_t sector;
	const sector_t capacity = drbd_get_capacity(mdev->this_bdev);
	struct Tl_epoch_entry *e;
	int size;
	Drbd_BlockRequest_Packet *p = (Drbd_BlockRequest_Packet*)h;

	ERR_IF(h->length != (sizeof(*p)-sizeof(*h))) return FALSE;

	if (drbd_recv(mdev, h->payload, h->length) != h->length)
		return FALSE;

	sector = be64_to_cpu(p->sector);
	size   = be32_to_cpu(p->blksize);

	if (size <= 0 || (size & 0x1ff) != 0 || size > DRBD_MAX_SEGMENT_SIZE) {
		ERR("%s:%d: sector: %lu, size: %d\n", __FILE__, __LINE__,
				(unsigned long)sector,size);
		return FALSE;
	}
	if ( sector + (size>>9) > capacity) {
		ERR("%s:%d: sector: %lu, size: %d\n", __FILE__, __LINE__,
				(unsigned long)sector,size);
		return FALSE;
	}

	if(!inc_local_if_state(mdev, UpToDate)) {
		if (DRBD_ratelimit(5*HZ,5))
			ERR("Can not satisfy peer's read request, no local data.\n");
		drbd_send_ack_rp(mdev,h->command == DataRequest ? NegDReply :
				 NegRSDReply ,p);
		return TRUE;
	}

	e = drbd_alloc_ee(mdev,p->block_id,sector,size,GFP_KERNEL);
	if (!e) {
		dec_local(mdev);
		return FALSE;
	}

	spin_lock_irq(&mdev->req_lock);
	list_add(&e->w.list,&mdev->read_ee);
	spin_unlock_irq(&mdev->req_lock);

	drbd_ee_prepare_read(mdev,e);

	switch (h->command) {
	case DataRequest:
		e->w.cb = w_e_end_data_req;
		break;
	case RSDataRequest:
		e->w.cb = w_e_end_rsdata_req;
		/* Eventually this should become asynchrously. Currently it
		 * blocks the whole receiver just to delay the reading of a
		 * resync data block.
		 * the drbd_work_queue mechanism is made for this...
		 */
		if (!drbd_rs_begin_io(mdev,sector)) {
			/* we have been interrupted,
			 * probably connection lost! */
			D_ASSERT(signal_pending(current));
			dec_local(mdev);
			drbd_free_ee(mdev,e);
			return 0;
		}
		break;
	default:; /* avoid compiler warning */
	}

	inc_unacked(mdev);
	/* FIXME actually, it could be a READA originating from the peer ... */
	drbd_generic_make_request(READ,e->private_bio);
	maybe_kick_lo(mdev);

	return TRUE;
}

STATIC int drbd_asb_recover_0p(drbd_dev *mdev)
{
	int self, peer, rv=-100;
	unsigned long ch_self, ch_peer;

	self = mdev->bc->md.uuid[Bitmap] & 1;
	peer = mdev->p_uuid[Bitmap] & 1;

	switch ( mdev->net_conf->after_sb_0p ) {
	case Consensus:
	case DiscardSecondary:
	case PanicPrimary:
		ERR("Configuration error.\n");
		break;
	case Disconnect:
		break;
	case DiscardYoungerPri:
		if (self == 0 && peer == 1) rv = -1;
		if (self == 1 && peer == 0) rv =  1;
		D_ASSERT(self != peer);
		break;
	case DiscardOlderPri:
		if (self == 0 && peer == 1) rv =  1;
		if (self == 1 && peer == 0) rv = -1;
		D_ASSERT(self != peer);
		break;
	case DiscardLeastChg:
		ch_peer = mdev->p_uuid[UUID_SIZE];
		ch_self = drbd_bm_total_weight(mdev);
		if      ( ch_self < ch_peer ) rv = -1;
		else if ( ch_self > ch_peer ) rv =  1;
		else /* ( ch_self == ch_peer ) */ {
			// Well, then use the order of the IP addresses...
			ch_self = (unsigned long)
				(((struct sockaddr_in *)mdev->net_conf->my_addr)
				 ->sin_addr.s_addr);
			ch_peer = (unsigned long)
				(((struct sockaddr_in *)mdev->net_conf->peer_addr)
				 ->sin_addr.s_addr);
			if      ( ch_self < ch_peer ) rv = -1;
			else if ( ch_self > ch_peer ) rv =  1;
			else ERR("Everything equal!?!\n");
		}
		break;
	case DiscardLocal:
		rv = -1;
		break;
	case DiscardRemote:
		rv =  1;
	}

	return rv;
}

STATIC int drbd_asb_recover_1p(drbd_dev *mdev)
{
	int self, peer, hg, rv=-100;

	self = mdev->bc->md.uuid[Bitmap] & 1;
	peer = mdev->p_uuid[Bitmap] & 1;

	switch ( mdev->net_conf->after_sb_1p ) {
	case DiscardYoungerPri:
	case DiscardOlderPri:
	case DiscardLeastChg:
	case DiscardLocal:
	case DiscardRemote:
		ERR("Configuration error.\n");
		break;
	case Disconnect:
		break;
	case Consensus:
		hg = drbd_asb_recover_0p(mdev);
		if( hg == -1 && mdev->state.role==Secondary) rv=hg;
		if( hg == 1  && mdev->state.role==Primary)   rv=hg;
		break;
	case DiscardSecondary:
		return mdev->state.role==Primary ? 1 : -1;
	case PanicPrimary:
		hg = drbd_asb_recover_0p(mdev);
		if( hg == -1 && mdev->state.role==Primary) {
			int got_mutex=!down_interruptible(&mdev->device_mutex);
			if (got_mutex) self = drbd_set_role(mdev,Secondary,0);
			if (self != SS_Success || !got_mutex) {
				drbd_khelper(mdev,"pri-lost-after-sb");
				drbd_panic("Panic by after-sb-1pri handler\n");
			} else {
				WARN("Sucessfully gave up primary role.\n");
				rv = hg;
			}
			if (got_mutex) up(&mdev->device_mutex);
		} else rv = hg;
	}
	return rv;
}

STATIC int drbd_asb_recover_2p(drbd_dev *mdev)
{
	int self, peer, hg, rv=-100;

	self = mdev->bc->md.uuid[Bitmap] & 1;
	peer = mdev->p_uuid[Bitmap] & 1;

	switch ( mdev->net_conf->after_sb_2p ) {
	case DiscardYoungerPri:
	case DiscardOlderPri:
	case DiscardLeastChg:
	case DiscardLocal:
	case DiscardRemote:
	case Consensus:
	case DiscardSecondary:
		ERR("Configuration error.\n");
		break;
	case Disconnect:
		break;
	case PanicPrimary:
		hg = drbd_asb_recover_0p(mdev);
		if( hg == -1 ) {
			int got_mutex=!down_interruptible(&mdev->device_mutex);
			if (got_mutex) self = drbd_set_role(mdev,Secondary,0);
			if (self != SS_Success || !got_mutex) {
				drbd_khelper(mdev,"pri-lost-after-sb");
				drbd_panic("Panic by after-sb-2pri handler\n");
			} else {
				WARN("Sucessfully gave up primary role.\n");
				rv = hg;
			}
			if (got_mutex) up(&mdev->device_mutex);
		} else rv = hg;
	}
	return rv;
}

STATIC void drbd_uuid_dump(drbd_dev *mdev,char* text,u64* uuid)
{
	WARN("%s %016llX:%016llX:%016llX:%016llX\n",
	     text,
	     uuid[Current],
	     uuid[Bitmap],
	     uuid[History_start],
	     uuid[History_end]);
}

/*
  100   after split brain try auto recover
    2   SyncSource set BitMap
    1   SyncSource use BitMap
    0   no Sync
   -1   SyncTarget use BitMap
   -2   SyncTarget set BitMap
 -100   after split brain, disconnect
-1000   unrelated data
 */
STATIC int drbd_uuid_compare(drbd_dev *mdev)
{
	u64 self, peer;
	int i,j;

	self = mdev->bc->md.uuid[Current] & ~((u64)1);
	peer = mdev->p_uuid[Current] & ~((u64)1);

	if (self == UUID_JUST_CREATED &&
	    peer == UUID_JUST_CREATED) return 0;

	if (self == UUID_JUST_CREATED &&
	    peer != UUID_JUST_CREATED) return -2;

	if (self != UUID_JUST_CREATED &&
	    peer == UUID_JUST_CREATED) return 2;

	if (self == peer) return 0;

	peer = mdev->p_uuid[Bitmap] & ~((u64)1);
	if (self == peer) return -1;

	for ( i=History_start ; i<=History_end ; i++ ) {
		peer = mdev->p_uuid[i] & ~((u64)1);
		if (self == peer) return -2;
	}

	self = mdev->bc->md.uuid[Bitmap] & ~((u64)1);
	peer = mdev->p_uuid[Current] & ~((u64)1);

	if (self == peer) return 1;

	for ( i=History_start ; i<=History_end ; i++ ) {
		self = mdev->bc->md.uuid[i] & ~((u64)1);
		if (self == peer) return 2;
	}

	self = mdev->bc->md.uuid[Bitmap] & ~((u64)1);
	peer = mdev->p_uuid[Bitmap] & ~((u64)1);

	if (self == peer) return 100;

	for ( i=History_start ; i<=History_end ; i++ ) {
		self = mdev->p_uuid[i] & ~((u64)1);
		for ( j=History_start ; j<=History_end ; j++ ) {
			peer = mdev->p_uuid[j] & ~((u64)1);
			if (self == peer) return -100;
		}
	}

	return -1000;
}

/* drbd_sync_handshake() returns the new conn state on success, or
   conn_mask (-1) on failure.
 */
STATIC drbd_conns_t drbd_sync_handshake(drbd_dev *mdev, drbd_role_t peer_role,
					drbd_disks_t peer_disk)
{
	int hg;
	drbd_conns_t rv = conn_mask;


	//drbd_uuid_dump(mdev,"self",mdev->bc->md.uuid);
	//drbd_uuid_dump(mdev,"peer",mdev->p_uuid);

	hg = drbd_uuid_compare(mdev);

	//WARN("uuid_compare()=%d\n",hg);

	if (hg == 100) {
		if ( mdev->state.role==Secondary && peer_role==Secondary ) {
			hg = drbd_asb_recover_0p(mdev);
		} else if (mdev->state.role==Primary && peer_role==Primary) {
			hg = drbd_asb_recover_2p(mdev);
		} else {
			hg = drbd_asb_recover_1p(mdev);
		}
		if ( abs(hg) < 100 ) {
			WARN("Split-Brain detected, automatically solved.\n");
		}
	}

	if ( hg == -100 ) {
		if(mdev->net_conf->want_lose && !mdev->p_uuid[UUID_FLAGS]) {
			hg = -1;
		}
		if(!mdev->net_conf->want_lose && mdev->p_uuid[UUID_FLAGS]) {
			hg = 1;
		}

		if ( abs(hg) < 100 ) {
			WARN("Split-Brain detected, manually solved.\n");
		}
	}
	
	if (hg == 0) {
		// This is needed in case someone does an invalidate on an
		// disconnected node.
		if(mdev->state.disk==Inconsistent && peer_disk>Inconsistent) hg=-1;
		if(mdev->state.disk>Inconsistent && peer_disk==Inconsistent) hg= 1;
	}

	if (hg == -1000) {
		ALERT("Unrelated data, dropping connection!\n");
		drbd_force_state(mdev,NS(conn,StandAlone));
		drbd_thread_stop_nowait(&mdev->receiver);
		return conn_mask;
	}

	if (hg == -100) {
		ALERT("Split-Brain detected, dropping connection!\n");
		drbd_force_state(mdev,NS(conn,StandAlone));
		drbd_thread_stop_nowait(&mdev->receiver);
		return conn_mask;
	}

	if (hg > 0 && mdev->state.disk <= Inconsistent ) {
		ERR("I shall become SyncSource, but I am inconsistent!\n");
		drbd_force_state(mdev,NS(conn,StandAlone));
		drbd_thread_stop_nowait(&mdev->receiver);
		return conn_mask;
	}
	if (hg < 0 && 
	    mdev->state.role == Primary && mdev->state.disk != Attaching ) {
		ERR("I shall become SyncTarget, but I am primary!\n");
		drbd_force_state(mdev,NS(conn,StandAlone));
		drbd_thread_stop_nowait(&mdev->receiver);
		return conn_mask;
	}

	if (abs(hg) >= 2) {
		drbd_md_set_flag(mdev,MDF_FullSync);
		drbd_md_sync(mdev);

		drbd_bm_set_all(mdev);
		drbd_bm_write(mdev);

		drbd_md_clear_flag(mdev,MDF_FullSync);
		drbd_md_sync(mdev);
	}

	if (hg > 0) { // become sync source.
		rv = WFBitMapS;
	} else if (hg < 0) { // become sync target
		drbd_uuid_set(mdev,Current,mdev->p_uuid[Bitmap]);
		rv = WFBitMapT;
	} else {
		rv = Connected;
		drbd_bm_lock(mdev);   // {
		if(drbd_bm_total_weight(mdev)) {
			INFO("No resync -> clearing bit map.\n");
			drbd_bm_clear_all(mdev);
			drbd_bm_write(mdev);
		}
		drbd_bm_unlock(mdev); // }
	}

	return rv;
}

STATIC int receive_protocol(drbd_dev *mdev, Drbd_Header *h)
{
	Drbd_Protocol_Packet *p = (Drbd_Protocol_Packet*)h;

	ERR_IF(h->length != (sizeof(*p)-sizeof(*h))) return FALSE;
	if (drbd_recv(mdev, h->payload, h->length) != h->length)
		return FALSE;

	if(be32_to_cpu(p->protocol)!=mdev->net_conf->wire_protocol) {
		int peer_proto = be32_to_cpu(p->protocol);
		if (DRBD_PROT_A <= peer_proto && peer_proto <= DRBD_PROT_C) {
			ERR("incompatible communication protocols: "
			    "me %c, peer %c\n",
				'A'-1+mdev->net_conf->wire_protocol,
				'A'-1+peer_proto);
		} else {
			ERR("incompatible communication protocols: "
			    "me %c, peer [%d]\n",
				'A'-1+mdev->net_conf->wire_protocol,
				peer_proto);
		}
		drbd_force_state(mdev,NS(conn,StandAlone));
		drbd_thread_stop_nowait(&mdev->receiver);
		return FALSE;
	}

	return TRUE;
}

STATIC int receive_SyncParam(drbd_dev *mdev,Drbd_Header *h)
{
	int ok = TRUE;
	Drbd_SyncParam_Packet *p = (Drbd_SyncParam_Packet*)h;

	ERR_IF(h->length != (sizeof(*p)-sizeof(*h))) return FALSE;
	if (drbd_recv(mdev, h->payload, h->length) != h->length)
		return FALSE;

	// XXX harmless race with ioctl ...
	mdev->sync_conf.rate      = be32_to_cpu(p->rate);
	drbd_alter_sa(mdev, be32_to_cpu(p->after));

	return ok;
}

STATIC void drbd_setup_order_type(drbd_dev *mdev, int peer)
{
#if 0
	int self = drbd_queue_order_type(mdev);
	int type;

	static char *order_txt[] = {
		[QUEUE_ORDERED_NONE]  = "none - oldIDE",
		[QUEUE_ORDERED_FLUSH] = "flush - IDE",
		[QUEUE_ORDERED_TAG]   = "tag - TCQ",
	};

	if(self == QUEUE_ORDERED_NONE ||
	   peer == QUEUE_ORDERED_NONE) {
		type = QUEUE_ORDERED_NONE;
	} else if (self == QUEUE_ORDERED_FLUSH ||
		   peer == QUEUE_ORDERED_FLUSH) {
		type = QUEUE_ORDERED_FLUSH;
	} else if(self == QUEUE_ORDERED_TAG ||
		  peer == QUEUE_ORDERED_TAG) {
		type = QUEUE_ORDERED_TAG;
	} else {
		D_ASSERT(0);
		type = QUEUE_ORDERED_NONE;
	}

	if (type != self ) {
		INFO("Exposing an order type of '%s' to the kernel\n",
		     order_txt[type]);
		blk_queue_ordered(mdev->rq_queue,type);
	}
#endif 
}

/* warn if the arguments differ by more than 12.5% */
static void warn_if_differ_considerably(drbd_dev *mdev, const char *s, sector_t a, sector_t b)
{
	sector_t d;
	if (a == 0 || b == 0) return;
	d = (a > b) ? (a - b) : (b - a);
	if ( d > (a>>3) || d > (b>>3)) {
		WARN("Considerable difference in %s: %llu vs. %llu\n", s,
		     (unsigned long long)a, (unsigned long long)b);
	}
}

STATIC int receive_sizes(drbd_dev *mdev, Drbd_Header *h)
{
	Drbd_Sizes_Packet *p = (Drbd_Sizes_Packet*)h;
	unsigned int max_seg_s;
	sector_t p_size, p_usize, my_usize;
	drbd_conns_t nconn;

	ERR_IF(h->length != (sizeof(*p)-sizeof(*h))) return FALSE;
	if (drbd_recv(mdev, h->payload, h->length) != h->length)
		return FALSE;

	p_size=be64_to_cpu(p->d_size);
	p_usize=be64_to_cpu(p->u_size);

	if(p_size == 0 && mdev->state.disk == Diskless ) {
		ERR("some backing storage is needed\n");
		drbd_force_state(mdev,NS(conn,StandAlone));
		drbd_thread_stop_nowait(&mdev->receiver);
		return FALSE;
	}

#define min_not_zero(l, r) (l == 0) ? r : ((r == 0) ? l : min(l, r))
	if(inc_local(mdev)) {
		warn_if_differ_considerably(mdev, "lower level device sizes",
			   p_size, drbd_get_capacity(mdev->bc->backing_bdev));
		warn_if_differ_considerably(mdev, "user requested size",
					    p_usize, mdev->bc->dc.disk_size);

		if (mdev->state.conn == WFReportParams) {
			/* this is first connect, or an otherwise expected 
			   param exchange.  choose the minimum */
			p_usize = min_not_zero(mdev->bc->dc.disk_size, p_usize);
		}

		my_usize = mdev->bc->dc.disk_size;

		if( mdev->bc->dc.disk_size != p_usize ) {
			mdev->bc->dc.disk_size = p_usize;
			INFO("Peer sets u_size to %lu KB\n",
			     (unsigned long)mdev->bc->dc.disk_size);
		}

		// Never shrink a device with usable data.
		if(drbd_new_dev_size(mdev,mdev->bc) < 
		   drbd_get_capacity(mdev->this_bdev) &&
		   mdev->state.disk >= Outdated ) {
			dec_local(mdev);
			ERR("The peer's disk size is too small!\n");
			drbd_force_state(mdev,NS(conn,StandAlone));
			drbd_thread_stop_nowait(&mdev->receiver);
			mdev->bc->dc.disk_size = my_usize;
			return FALSE;
		}
		dec_local(mdev);
	}
#undef min_not_zero

	mdev->p_size=p_size;
	if(inc_local(mdev)) {
		drbd_bm_lock(mdev); // {
		/*
		 * you may get a flip-flop connection established/connection loss, 
		 * in case both really have different usize uppon first connect!
		 * try to solve it thus:
		 ***/

		drbd_determin_dev_size(mdev);
		drbd_bm_unlock(mdev); // }
		dec_local(mdev);
	} else {
		// I am diskless, need to accept the peer's size.
		drbd_set_my_capacity(mdev,p_size);
	}

	if (mdev->p_uuid && mdev->state.conn <= Connected && inc_local(mdev)) {
		nconn=drbd_sync_handshake(mdev,mdev->state.peer,mdev->state.pdsk);
		dec_local(mdev);

		if(nconn == conn_mask) return FALSE;

		if(drbd_request_state(mdev,NS(conn,nconn)) < SS_Success) {
			drbd_force_state(mdev,NS(conn,StandAlone));
			drbd_thread_stop_nowait(&mdev->receiver);
			return FALSE;
		}
	}

	if(inc_local(mdev)) {
		max_seg_s = be32_to_cpu(p->max_segment_size);
		if( max_seg_s != mdev->rq_queue->max_segment_size ) {
			drbd_setup_queue_param(mdev, max_seg_s);
		}

		drbd_setup_order_type(mdev,be32_to_cpu(p->queue_order_type));
		dec_local(mdev);
	}

	if (mdev->state.conn > WFReportParams ) {
		if( be64_to_cpu(p->c_size) !=
		    drbd_get_capacity(mdev->this_bdev) ) {
			// we have different sizes, probabely peer
			// needs to know my new size...
			drbd_send_sizes(mdev);
		}
	}

	return TRUE;
}

STATIC int receive_uuids(drbd_dev *mdev, Drbd_Header *h)
{
	Drbd_GenCnt_Packet *p = (Drbd_GenCnt_Packet*)h;
	u64 *p_uuid;
	int i;

	ERR_IF(h->length != (sizeof(*p)-sizeof(*h))) return FALSE;
	if (drbd_recv(mdev, h->payload, h->length) != h->length)
		return FALSE;

	p_uuid = kmalloc(sizeof(u64)*EXT_UUID_SIZE, GFP_KERNEL);

	for (i = Current; i < EXT_UUID_SIZE; i++) {
		p_uuid[i] = be64_to_cpu(p->uuid[i]);
	}

	if ( mdev->p_uuid ) kfree(mdev->p_uuid);
	mdev->p_uuid = p_uuid;

	return TRUE;
}

/** 
 * convert_state:
 * Switches the view of the state.
 */ 
STATIC drbd_state_t convert_state(drbd_state_t ps)
{
	drbd_state_t ms;

	static drbd_conns_t c_tab[] = {
		[Connected] = Connected,

		[StartingSyncS] = StartingSyncT,
		[StartingSyncT] = StartingSyncS,

		[conn_mask]   = conn_mask,
	};

	ms.i = ps.i;

	ms.conn = c_tab[ps.conn];
	ms.peer = ps.role;
	ms.role = ps.peer;
	ms.pdsk = ps.disk;
	ms.disk = ps.pdsk;
	ms.peer_isp = ( ps.aftr_isp | ps.user_isp );

	return ms;
}

STATIC int receive_req_state(drbd_dev *mdev, Drbd_Header *h)
{
	Drbd_Req_State_Packet *p = (Drbd_Req_State_Packet*)h;
	drbd_state_t mask,val;
	int rv;

	ERR_IF(h->length != (sizeof(*p)-sizeof(*h))) return FALSE;
	if (drbd_recv(mdev, h->payload, h->length) != h->length)
		return FALSE;

	mask.i = be32_to_cpu(p->mask);
	val.i = be32_to_cpu(p->val);

	if (test_bit(UNIQUE,&mdev->flags)) drbd_state_lock(mdev);

	mask = convert_state(mask);
	val = convert_state(val);

	rv = drbd_change_state(mdev,ChgStateVerbose,mask,val);

	if (test_bit(UNIQUE,&mdev->flags)) drbd_state_unlock(mdev);

	drbd_send_sr_reply(mdev,rv);
	drbd_md_sync(mdev);

	return TRUE;
}

STATIC int receive_state(drbd_dev *mdev, Drbd_Header *h)
{
	Drbd_State_Packet *p = (Drbd_State_Packet*)h;
	drbd_conns_t nconn;
	drbd_state_t os,ns,peer_state;
	int rv;

	ERR_IF(h->length != (sizeof(*p)-sizeof(*h))) return FALSE;
	if (drbd_recv(mdev, h->payload, h->length) != h->length)
		return FALSE;

	nconn = mdev->state.conn;
	if (nconn == WFReportParams ) nconn = Connected;

	peer_state.i = be32_to_cpu(p->state);

	if (mdev->p_uuid && mdev->state.conn <= Connected && 
	    inc_local_if_state(mdev,Negotiating) ) {
		nconn=drbd_sync_handshake(mdev,peer_state.role,peer_state.disk);
		dec_local(mdev);

		if(nconn == conn_mask) return FALSE;
	}

	if (mdev->state.conn > WFReportParams ) {
		if( nconn > Connected && peer_state.conn == Connected) {
			// we want resync, peer has not yet decided to sync...
			drbd_send_uuids(mdev);
			drbd_send_state(mdev);
		}
	}

	/*
        if ( peer_state.disk == Negotiating && nconn == Connected ) {
		// Peer should promote from Negotiating to UpToDate.
		drbd_send_state(mdev);
		peer_state.disk = UpToDate;
	}
	*/

	spin_lock_irq(&mdev->req_lock);
	os = mdev->state;
	ns.i = mdev->state.i;
	ns.conn = nconn;
	ns.peer = peer_state.role;
	ns.pdsk = peer_state.disk;
	ns.peer_isp = ( peer_state.aftr_isp | peer_state.user_isp );
	if(nconn == Connected && ns.disk == Negotiating ) ns.disk = UpToDate;
	rv = _drbd_set_state(mdev,ns,ChgStateVerbose | ChgStateHard);
	spin_unlock_irq(&mdev->req_lock);
	if (rv==SS_Success) {
		after_state_ch(mdev,os,ns,ChgStateVerbose | ChgStateHard);
	}

	if(rv < SS_Success) {
		drbd_force_state(mdev,NS(conn,StandAlone));
		drbd_thread_stop_nowait(&mdev->receiver);
		return FALSE;
	}

	mdev->net_conf->want_lose = 0;

	/* FIXME assertion for (gencounts do not diverge) */
	drbd_md_sync(mdev); // update connected indicator, la_size, ...

	return TRUE;
}

STATIC int receive_sync_uuid(drbd_dev *mdev, Drbd_Header *h)
{
	Drbd_SyncUUID_Packet *p = (Drbd_SyncUUID_Packet*)h;

	wait_event( mdev->cstate_wait, 
		    mdev->state.conn < Connected || mdev->state.conn == WFSyncUUID);

	// D_ASSERT( mdev->state.conn == WFSyncUUID );

	ERR_IF(h->length != (sizeof(*p)-sizeof(*h))) return FALSE;
	if (drbd_recv(mdev, h->payload, h->length) != h->length)
		return FALSE;
	
	_drbd_uuid_set(mdev,Current,be64_to_cpu(p->uuid));
	_drbd_uuid_set(mdev,Bitmap,0UL);

	drbd_start_resync(mdev,SyncTarget);

	return TRUE;
}

/* Since we are processing the bitfild from lower addresses to higher,
   it does not matter if the process it in 32 bit chunks or 64 bit
   chunks as long as it is little endian. (Understand it as byte stream,
   beginning with the lowest byte...) If we would use big endian
   we would need to process it from the highest address to the lowest,
   in order to be agnostic to the 32 vs 64 bits issue.

   returns 0 on failure, 1 if we suceessfully received it. */
STATIC int receive_bitmap(drbd_dev *mdev, Drbd_Header *h)
{
	size_t bm_words, bm_i, want, num_words;
	unsigned long *buffer;
	int ok=FALSE;

	drbd_bm_lock(mdev);  // {

	bm_words = drbd_bm_words(mdev);
	bm_i     = 0;
	buffer   = vmalloc(BM_PACKET_WORDS*sizeof(long));

	while (1) {
		num_words = min_t(size_t, BM_PACKET_WORDS, bm_words-bm_i );
		want = num_words * sizeof(long);
		ERR_IF(want != h->length) goto out;
		if (want==0) break;
		if (drbd_recv(mdev, buffer, want) != want)
			goto out;

		drbd_bm_merge_lel(mdev, bm_i, num_words, buffer);
		bm_i += num_words;

		if (!drbd_recv_header(mdev,h))
			goto out;
		D_ASSERT(h->command == ReportBitMap);
	}

	clear_bit(CRASHED_PRIMARY, &mdev->flags); // md_write() is in drbd_start_resync.
	if (mdev->state.conn == WFBitMapS) {
		drbd_start_resync(mdev,SyncSource);
	} else if (mdev->state.conn == WFBitMapT) {
		ok = drbd_send_bitmap(mdev);
		if (!ok) goto out;
		ok = drbd_request_state(mdev,NS(conn,WFSyncUUID));
		D_ASSERT( ok == SS_Success );
	} else {
		ERR("unexpected cstate (%s) in receive_bitmap\n",
		    conns_to_name(mdev->state.conn));
	}

	// We just started resync. Now we can be sure that local disk IO is okay.

	/* no, actually we can't. failures happen asynchronously, anytime.
	 * we can never be sure. disk may have failed while we where busy shaking hands...
	 */

	ok=TRUE;
 out:
	drbd_bm_unlock(mdev); // }
	vfree(buffer);
	return ok;
}

STATIC int receive_skip(drbd_dev *mdev,Drbd_Header *h)
{
	// TODO zero copy sink :)
	static char sink[128];
	int size,want,r;

	WARN("skipping unknown optional packet type %d, l: %d!\n",
	     h->command, h->length );

	size = h->length;
	while (size > 0) {
		want = min_t(int,size,sizeof(sink));
		r = drbd_recv(mdev,sink,want);
		ERR_IF(r < 0) break;
		size -= r;
	}
	return (size == 0);
}

STATIC int receive_pause_resync(drbd_dev *mdev, Drbd_Header *h)
{
	drbd_resync_pause(mdev, PeerImposed);
	return TRUE;

}

STATIC int receive_resume_resync(drbd_dev *mdev, Drbd_Header *h)
{
	drbd_resync_resume(mdev, PeerImposed);
	return TRUE;
}


STATIC int receive_UnplugRemote(drbd_dev *mdev, Drbd_Header *h)
{
	if (mdev->state.disk >= Inconsistent) drbd_kick_lo(mdev);
	return TRUE; // cannot fail.
}

STATIC int receive_outdate(drbd_dev *mdev, Drbd_Header *h)
{
	drbd_state_t os,ns;
	int r;

	spin_lock_irq(&mdev->req_lock);
	os = mdev->state;
	if( os.disk < Outdated ) {
		r=-999;
	} else {
		r = _drbd_set_state(mdev, _NS2(disk,Outdated,conn,TearDown),
				    ChgStateVerbose);
	}
	ns = mdev->state;
	spin_unlock_irq(&mdev->req_lock);
	if( r == SS_Success) after_state_ch(mdev,os,ns,ChgStateVerbose);

	if( r >= SS_Success) {
		drbd_md_sync(mdev);
		drbd_send_short_cmd(mdev, OutdatedReply);
		return TRUE;
	}

	return FALSE;
}

STATIC int receive_outdated(drbd_dev *mdev, Drbd_Header *h)
{
	int r;

	r = drbd_request_state(mdev,NS2(pdsk,Outdated,conn,TearDown));
	WARN("r=%d\n",r);
	D_ASSERT(r >= SS_Success);
	drbd_md_sync(mdev); // because drbd_request_state created a new UUID.

	return TRUE;
}

typedef int (*drbd_cmd_handler_f)(drbd_dev*,Drbd_Header*);

static drbd_cmd_handler_f drbd_default_handler[] = {
	[Data]             = receive_Data,
	[DataReply]        = receive_DataReply,
	[RSDataReply]      = receive_RSDataReply,
	[RecvAck]          = NULL, // via msock: got_RecvAck,
	[WriteAck]         = NULL, // via msock: got_WriteAck,
	[Barrier]          = receive_Barrier_no_tcq,
	[BarrierAck]       = NULL, // via msock: got_BarrierAck,
	[ReportBitMap]     = receive_bitmap,
	[Ping]             = NULL, // via msock: got_Ping,
	[PingAck]          = NULL, // via msock: got_PingAck,
	[UnplugRemote]     = receive_UnplugRemote,
	[DataRequest]      = receive_DataRequest,
	[RSDataRequest]    = receive_DataRequest, //receive_RSDataRequest,
	[SyncParam]        = receive_SyncParam,
	[ReportProtocol]   = receive_protocol,
	[ReportUUIDs]      = receive_uuids,
	[ReportSizes]      = receive_sizes,
	[ReportState]      = receive_state,
	[StateChgRequest]  = receive_req_state,
	[ReportSyncUUID]   = receive_sync_uuid,
	[PauseResync]      = receive_pause_resync,
	[ResumeResync]     = receive_resume_resync,
	[OutdateRequest]   = receive_outdate,
	[OutdatedReply]    = receive_outdated,
};

static drbd_cmd_handler_f *drbd_cmd_handler = drbd_default_handler;
static drbd_cmd_handler_f *drbd_opt_cmd_handler = NULL;

#if 0
	/* FIXME lge thinks the implementation of barrier handling via
	 * tcq is currently broken */
void drbd_set_recv_tcq(drbd_dev * mdev, int tcq_enabled)
{
// warning LGE "FIXME make drbd_cmd_handler a member of mdev"
	if(tcq_enabled &&
	   drbd_default_handler[Barrier] != receive_Barrier_tcq) {
		INFO("Enabling TCQ for barrier processing on backend.\n");
		drbd_default_handler[Barrier] = receive_Barrier_tcq;
	}

	if(!tcq_enabled &&
	   drbd_default_handler[Barrier] != receive_Barrier_usual) {
		INFO("Using conventional (non TCQ) barrier processing"
		     " on backend.\n");
		drbd_default_handler[Barrier] = receive_Barrier_usual;
	}
}
#endif

STATIC void drbdd(drbd_dev *mdev)
{
	drbd_cmd_handler_f handler;
	Drbd_Header *header = &mdev->data.rbuf.head;

	for (;;) {
		if (!drbd_recv_header(mdev,header))
			break;

		if (header->command < MAX_CMD)
			handler = drbd_cmd_handler[header->command];
		else if (MayIgnore < header->command && header->command < MAX_OPT_CMD)
			handler = drbd_opt_cmd_handler[header->command-MayIgnore];
		else if (header->command > MAX_OPT_CMD)
			handler = receive_skip;
		else
			handler = NULL;

		dump_packet(mdev,mdev->data.socket,2,&mdev->data.rbuf, __FILE__, __LINE__);

		if (unlikely(!handler)) {
			ERR("unknown packet type %d, l: %d!\n",
			    header->command, header->length);
			break;
		}
		if (unlikely(!handler(mdev,header))) {
			ERR("error receiving %s, l: %d!\n",
			    cmdname(header->command), header->length);
			break;
		}
	}
}

STATIC void drbd_disconnect(drbd_dev *mdev)
{
	enum fencing_policy fp;

	struct drbd_work *disconnect_work;

	D_ASSERT(mdev->state.conn < Connected);

	drbd_thread_stop(&mdev->asender);

	fp = DontCare;
	if(inc_local(mdev)) {
		fp = mdev->bc->dc.fencing;
		dec_local(mdev);
	}

	// Receiving side (may be primary, in case we had two primaries)
	spin_lock_irq(&mdev->req_lock);
	_drbd_wait_ee_list_empty(mdev,&mdev->read_ee);
	_drbd_wait_ee_list_empty(mdev,&mdev->active_ee);
	_drbd_wait_ee_list_empty(mdev,&mdev->sync_ee);
	_drbd_clear_done_ee(mdev);
	mdev->epoch_size = 0;
	spin_unlock_irq(&mdev->req_lock);
	// Needs to happen before we schedule the disconnect work callback,
	// Since they might have something for the worker's queue as well.

	disconnect_work = kmalloc(sizeof(struct drbd_work),GFP_KERNEL);
	if(disconnect_work) {
		disconnect_work->cb = w_disconnect;
		drbd_queue_work(&mdev->data.work,disconnect_work);
	} else {
		WARN("kmalloc failed, taking messy shortcut.\n");
		w_disconnect(mdev,NULL,1);
	}

	drbd_md_sync(mdev);

	if ( mdev->state.role == Primary ) {		
		if( fp >= Resource &&
		    mdev->state.pdsk >= DUnknown ) {
			drbd_disks_t nps = drbd_try_outdate_peer(mdev);
			drbd_request_state(mdev,NS(pdsk,nps));
		}
	}

	spin_lock_irq(&mdev->req_lock);
	if ( mdev->state.conn > Unconnected ) {
		// Do not restart in case we are StandAlone
		_drbd_set_state(mdev, _NS(conn,Unconnected), ScheduleAfter);
	}
	spin_unlock_irq(&mdev->req_lock);

	drbd_md_sync(mdev);
}

/*
 * we hereby assure that we always support the drbd dialects
 * PRO_VERSION and (PRO_VERSION -1), allowing for rolling upgrades
 *
 * feature flags and the reserved array should be enough room for future
 * enhancements of the handshake protocol, and possible plugins...
 *
 * for now, they are expected to be zero, but ignored.
 */
int drbd_send_handshake(drbd_dev *mdev)
{
	// ASSERT current == mdev->receiver ...
	Drbd_HandShake_Packet *p = &mdev->data.sbuf.HandShake;
	int ok;

	if (down_interruptible(&mdev->data.mutex)) {
		ERR("interrupted during initial handshake\n");
		return 0; /* interrupted. not ok. */
	}
	/* FIXME do we need to verify this here? */
	if (mdev->data.socket == NULL) {
		up(&mdev->data.mutex);
		return 0;
	}

	memset(p,0,sizeof(*p));
	p->protocol_version = cpu_to_be32(PRO_VERSION);
	ok = _drbd_send_cmd( mdev, mdev->data.socket, HandShake,
	                     (Drbd_Header *)p, sizeof(*p), 0 );
	up(&mdev->data.mutex);
	return ok;
}

/*
 * return values:
 *   1 yess, we have a valid connection
 *   0 oops, did not work out, please try again
 *  -1 peer talks different language,
 *     no point in trying again, please go standalone.
 */
STATIC int drbd_do_handshake(drbd_dev *mdev)
{
	// ASSERT current == mdev->receiver ...
	Drbd_HandShake_Packet *p = &mdev->data.rbuf.HandShake;
	const int expect = sizeof(Drbd_HandShake_Packet)-sizeof(Drbd_Header);
	int rv;

	rv = drbd_send_handshake(mdev);
	if (!rv) goto break_c_loop;

	rv = drbd_recv_header(mdev,&p->head);
	if (!rv) goto break_c_loop;

	if (p->head.command != HandShake) {
		ERR( "expected HandShake packet, received: %s (0x%04x)\n",
		     cmdname(p->head.command), p->head.command );
		return -1;
	}

	if (p->head.length != expect) {
		ERR( "expected HandShake length: %u, received: %u\n",
		     expect, p->head.length );
		return -1;
	}

	rv = drbd_recv(mdev, &p->head.payload, expect);

	if (rv != expect) {
		ERR("short read receiving handshake packet: l=%u\n", rv);
		return 0;
	}

	dump_packet(mdev,mdev->data.socket,2,&mdev->data.rbuf, __FILE__, __LINE__);

	p->protocol_version = be32_to_cpu(p->protocol_version);

	if ( p->protocol_version == PRO_VERSION ||
	     p->protocol_version == (PRO_VERSION+1) ) {
		if (p->protocol_version == (PRO_VERSION+1)) {
			WARN( "You should upgrade me! "
			      "Peer wants protocol version: %u\n",
			      p->protocol_version );
		}
		INFO( "Handshake successful: DRBD Network Protocol version %u\n",
		      PRO_VERSION );
	} /* else if ( p->protocol_version == (PRO_VERSION-1) ) {
		// not yet; but next time :)
		INFO( "Handshake successful: DRBD Protocol version %u\n",
		      (PRO_VERSION-1) );
		... do some remapping of defaults and jump tables here ...
	} */ else {
		ERR( "incompatible DRBD dialects: "
		     "I support %u, peer wants %u\n",
		     PRO_VERSION, p->protocol_version );
		return -1;
	}

	return 1;

 break_c_loop:
	WARN( "My msock connect got accepted onto peer's sock!\n");
	/* In case a tcp connection set-up takes longer than 
	   connect-int, we might get into the situation that this
	   node's msock gets connected to the peer's sock!
	   
	   To break out of this endless loop behaviour, we need to 
	   wait unti the peer's msock connect tries are over. (1 Second)

	   Additionally we wait connect-int/2 to hit with our next 
	   connect try exactly in the peer's window of expectation. */

	set_current_state(TASK_INTERRUPTIBLE);
	schedule_timeout(HZ + (mdev->net_conf->try_connect_int*HZ)/2);
	
	return 0;
}

#ifndef CONFIG_CRYPTO_HMAC
STATIC int drbd_do_auth(drbd_dev *mdev)
{
	ERR( "This kernel was build without CONFIG_CRYPTO_HMAC.\n");
	ERR( "You need to disable 'cram-hmac-alg' in drbd.conf.\n");
	return 0;
}
#else
#define CHALLENGE_LEN 64
STATIC int drbd_do_auth(drbd_dev *mdev)
{
	char my_challenge[CHALLENGE_LEN];  /* 64 Bytes... */
	struct scatterlist sg;
	char *response = NULL;
	char *right_response = NULL;
	char *peers_ch = NULL;
	Drbd_Header p;
	unsigned int key_len = strlen(mdev->net_conf->shared_secret);
	unsigned int resp_size;
	int rv;

	get_random_bytes(my_challenge, CHALLENGE_LEN);

	rv = drbd_send_cmd2(mdev,AuthChallenge,my_challenge,CHALLENGE_LEN);
	if (!rv) goto fail;

	rv = drbd_recv_header(mdev,&p);
	if (!rv) goto fail;

	if (p.command != AuthChallenge) {
		ERR( "expected AuthChallenge packet, received: %s (0x%04x)\n",
		     cmdname(p.command), p.command );
		rv = 0;
		goto fail;
	}

	if (p.length > CHALLENGE_LEN*2 ) {
		ERR( "expected AuthChallenge payload too big.\n");
		rv = 0;
		goto fail;
	}

	peers_ch = kmalloc(p.length,GFP_KERNEL);
	if(peers_ch == NULL) {
		ERR("kmalloc of peers_ch failed\n");
		rv = 0;
		goto fail;
	}

	rv = drbd_recv(mdev, peers_ch, p.length);

	if (rv != p.length) {
		ERR("short read AuthChallenge: l=%u\n", rv);
		rv = 0;
		goto fail;
	}

	resp_size = crypto_tfm_alg_digestsize(mdev->cram_hmac_tfm);
	response = kmalloc(resp_size,GFP_KERNEL);
	if(response == NULL) {
		ERR("kmalloc of response failed\n");
		rv = 0;
		goto fail;
	}

	sg.page   = virt_to_page(peers_ch);
	sg.offset = offset_in_page(peers_ch);
	sg.length = p.length;
	crypto_hmac(mdev->cram_hmac_tfm, (u8*)mdev->net_conf->shared_secret,
		    &key_len, &sg, 1, response);

	rv = drbd_send_cmd2(mdev,AuthResponse,response,resp_size);
	if (!rv) goto fail;

	rv = drbd_recv_header(mdev,&p);
	if (!rv) goto fail;

	if (p.command != AuthResponse) {
		ERR( "expected AuthResponse packet, received: %s (0x%04x)\n",
		     cmdname(p.command), p.command );
		rv = 0;
		goto fail;
	}

	if (p.length != resp_size ) {
		ERR( "expected AuthResponse payload of wrong size\n" );
		rv = 0;
		goto fail;
	}

	rv = drbd_recv(mdev, response , resp_size);

	if (rv != resp_size) {
		ERR("short read receiving AuthResponse: l=%u\n", rv);
		rv = 0;
		goto fail;
	}

	right_response = kmalloc(resp_size,GFP_KERNEL);
	if(response == NULL) {
		ERR("kmalloc of right_response failed\n");
		rv = 0;
		goto fail;
	}

	sg.page   = virt_to_page(my_challenge);
	sg.offset = offset_in_page(my_challenge);
	sg.length = CHALLENGE_LEN;
	crypto_hmac(mdev->cram_hmac_tfm, (u8*)mdev->net_conf->shared_secret,
		    &key_len, &sg, 1, right_response);

	rv = ! memcmp(response,right_response,resp_size);

	if(rv) {
		INFO("Peer authenticated usind %d bytes of '%s' HMAC\n",
		     resp_size,mdev->net_conf->cram_hmac_alg);
	}

 fail:
	if(peers_ch) kfree(peers_ch);
	if(response) kfree(response);
	if(right_response) kfree(right_response);

	return rv;
}
#endif

int drbdd_init(struct Drbd_thread *thi)
{
	drbd_dev *mdev = thi->mdev;
	int minor = mdev_to_minor(mdev);
	int h;

	sprintf(current->comm, "drbd%d_receiver", minor);

	/* printk(KERN_INFO DEVICE_NAME ": receiver living/m=%d\n", minor); */

	do {
		h = drbd_connect(mdev);
		if (h == 0) {
			drbd_disconnect(mdev);
			schedule_timeout(HZ);
		}
		if( h < 0 ) {
			WARN("Discarding network configuration.\n");
			drbd_force_state(mdev,NS(conn,StandAlone));
		}
	} while ( h == 0 );

	if( h > 0 ) {
		if(inc_net(mdev)) {
			drbdd(mdev);
			dec_net(mdev);
		}
	}

	drbd_disconnect(mdev);

	INFO("receiver terminated\n");
	return 0;
}

/* ********* acknowledge sender ******** */

STATIC int got_RqSReply(drbd_dev *mdev, Drbd_Header* h)
{
	Drbd_RqS_Reply_Packet *p = (Drbd_RqS_Reply_Packet*)h;

	int retcode = be32_to_cpu(p->retcode);

	if(retcode >= SS_Success) {
		set_bit(CL_ST_CHG_SUCCESS,&mdev->flags);
	} else {
		set_bit(CL_ST_CHG_FAIL,&mdev->flags);
		ERR("Requested state change failed by peer: %s\n",
		    set_st_err_name(retcode));
	}
	wake_up(&mdev->cstate_wait);

	return TRUE;
}

STATIC int got_Ping(drbd_dev *mdev, Drbd_Header* h)
{
	return drbd_send_ping_ack(mdev);

}

STATIC int got_PingAck(drbd_dev *mdev, Drbd_Header* h)
{
	// restore idle timeout
	mdev->meta.socket->sk->sk_rcvtimeo = mdev->net_conf->ping_int*HZ;

	return TRUE;
}

STATIC int got_BlockAck(drbd_dev *mdev, Drbd_Header* h)
{
	drbd_request_t *req;
	Drbd_BlockAck_Packet *p = (Drbd_BlockAck_Packet*)h;
	sector_t sector = be64_to_cpu(p->sector);
	int blksize = be32_to_cpu(p->blksize);

	update_peer_seq(mdev,be32_to_cpu(p->seq_num));

	smp_rmb();
	/* FIXME smp_rmb() is probably not good enough.
	 * we have to make sure that, no matter what,
	 * we do not set something "in sync" when
	 * the peer has no disk (anymore)
	 * I think this has to be looked at under the req_lock.
	 * since we need to grab that anyways, lets do that.
	 */
	if(likely(mdev->state.pdsk >= Inconsistent )) {
		/*
		 * If one of a few IO requests on the peer failed (got_NegAck),
		 * but some subsequent requests completed sucessfull
		 * afterwards, verification of the block_id below would fail,
		 * since we killed everything out of the transferlog when we
		 * got the news hat IO is broken on the peer.
		 *
		 * FIXME
		 * could this be handled better?
		 * do we need to look over this again for freeze-io?
		 */

		if( is_syncer_block_id(p->block_id)) {
			drbd_set_in_sync(mdev,sector,blksize);
			set_bit(SYNC_STARTED,&mdev->flags);
		} else {
			spin_lock_irq(&mdev->req_lock);
			req = _ack_id_to_req(mdev, p->block_id, sector);

			if (unlikely(!req)) {
				spin_unlock_irq(&mdev->req_lock);
				ERR("Got a corrupt block_id/sector pair(2).\n");
				return FALSE;
			}

			_req_mod(req,
				 h->command == WriteAck
				 ? write_acked_by_peer
				 : recv_acked_by_peer);
			spin_unlock_irq(&mdev->req_lock);

			if (test_bit(SYNC_STARTED,&mdev->flags) &&
			    mdev->net_conf->wire_protocol == DRBD_PROT_C)
				drbd_set_in_sync(mdev,sector,blksize);
		}
	}

	if(is_syncer_block_id(p->block_id)) {
		dec_rs_pending(mdev);
	}
	/* dec_ap_pending is handled within _req_mod */

	return TRUE;
}

STATIC int got_NegAck(drbd_dev *mdev, Drbd_Header* h)
{
	Drbd_BlockAck_Packet *p = (Drbd_BlockAck_Packet*)h;

	update_peer_seq(mdev,be32_to_cpu(p->seq_num));

	/* do nothing here.
	 * we expect to get a "report param" on the data socket soon,
	 * and will do the cleanup then and there.
	 */
	if(is_syncer_block_id(p->block_id)) {
		dec_rs_pending(mdev);
	}
	if (DRBD_ratelimit(5*HZ,5))
		WARN("Got NegAck packet. Peer is in troubles?\n");

	return TRUE;
}

STATIC int got_NegDReply(drbd_dev *mdev, Drbd_Header* h)
{
	drbd_request_t *req;
	Drbd_BlockAck_Packet *p = (Drbd_BlockAck_Packet*)h;
	sector_t sector = be64_to_cpu(p->sector);

	spin_lock_irq(&mdev->req_lock);
	req = _ar_id_to_req(mdev,p->block_id, sector);
	if (unlikely(!req)) {
		spin_unlock_irq(&mdev->req_lock);
		ERR("Got a corrupt block_id/sector pair(3).\n");
		return FALSE;
	}

	/* FIXME what for ?? list_del(&req->w.list); */
	_req_mod(req, neg_acked);
	spin_unlock_irq(&mdev->req_lock);

// warning LGE "ugly and wrong"
	drbd_khelper(mdev,"pri-on-incon-degr");
	drbd_panic("Got NegDReply. WE ARE LOST. We lost our up-to-date disk.\n");

	// THINK do we have other options, but panic?
	//       what about bio_endio, in case we don't panic ??

	return TRUE;
}

STATIC int got_NegRSDReply(drbd_dev *mdev, Drbd_Header* h)
{
	sector_t sector;
	Drbd_BlockAck_Packet *p = (Drbd_BlockAck_Packet*)h;

	sector = be64_to_cpu(p->sector);
	D_ASSERT(p->block_id == ID_SYNCER);

	drbd_rs_complete_io(mdev,sector);


	// In case we are not primary, we could simply live on...

// warning LGE "ugly and wrong"
	drbd_panic("Got NegRSDReply. WE ARE LOST. We lost our up-to-date disk.\n");

	// THINK do we have other options, but panic?
	//       what about bio_endio, in case we don't panic ??

	return TRUE;
}

STATIC int got_BarrierAck(drbd_dev *mdev, Drbd_Header* h)
{
	Drbd_BarrierAck_Packet *p = (Drbd_BarrierAck_Packet*)h;

	smp_rmb();
	if(unlikely(mdev->state.pdsk <= Diskless)) return TRUE;

	tl_release(mdev,p->barrier,be32_to_cpu(p->set_size));
	dec_ap_pending(mdev);

	return TRUE;
}

/* FIXME implementation wrong.
 * For the algorithm to be correct, we need to send and store the
 * sector and size too.
 */
STATIC int got_Discard(drbd_dev *mdev, Drbd_Header* h)
{
	Drbd_Discard_Packet *p = (Drbd_Discard_Packet*)h;
	struct drbd_discard_note *dn;

	dn = kmalloc(sizeof(struct drbd_discard_note),GFP_KERNEL);
	if(!dn) {
		ERR("kmalloc(drbd_discard_note) failed.");
		return FALSE;
	}

	dn->block_id = p->block_id;
	dn->seq_num = be32_to_cpu(p->seq_num);

	spin_lock(&mdev->peer_seq_lock);
	list_add(&dn->list,&mdev->discard);
	spin_unlock(&mdev->peer_seq_lock);

	return TRUE;
}

struct asender_cmd {
	size_t pkt_size;
	int (*process)(drbd_dev *mdev, Drbd_Header* h);
};

int drbd_asender(struct Drbd_thread *thi)
{
	drbd_dev *mdev = thi->mdev;
	Drbd_Header *h = &mdev->meta.rbuf.head;

	int rv,len;
	void *buf    = h;
	int received = 0;
	int expect   = sizeof(Drbd_Header);
	int cmd      = -1;

	static struct asender_cmd asender_tbl[] = {
		[Ping]      ={ sizeof(Drbd_Header),           got_Ping },
		[PingAck]   ={ sizeof(Drbd_Header),           got_PingAck },
		[RecvAck]   ={ sizeof(Drbd_BlockAck_Packet),  got_BlockAck },
		[WriteAck]  ={ sizeof(Drbd_BlockAck_Packet),  got_BlockAck },
		[NegAck]    ={ sizeof(Drbd_BlockAck_Packet),  got_NegAck },
		[NegDReply] ={ sizeof(Drbd_BlockAck_Packet),  got_NegDReply },
		[NegRSDReply]={sizeof(Drbd_BlockAck_Packet),  got_NegRSDReply},
		[BarrierAck]={ sizeof(Drbd_BarrierAck_Packet),got_BarrierAck },
		[DiscardNote]={sizeof(Drbd_Discard_Packet),   got_Discard },
		[StateChgReply]={sizeof(Drbd_RqS_Reply_Packet),got_RqSReply },
	};

	sprintf(current->comm, "drbd%d_asender", mdev_to_minor(mdev));

	current->policy = SCHED_RR;  /* Make this a realtime task! */
	current->rt_priority = 2;    /* more important than all other tasks */

	while (get_t_state(thi) == Running) {
		if (test_and_clear_bit(SEND_PING, &mdev->flags)) {
			ERR_IF(!drbd_send_ping(mdev)) goto err;
			// half ack timeout only,
			// since sendmsg waited the other half already
			mdev->meta.socket->sk->sk_rcvtimeo =
				mdev->net_conf->timeout*HZ/20;
		}

		if (!drbd_process_done_ee(mdev)) goto err;
		set_bit(SIGNAL_ASENDER, &mdev->flags);

		rv = drbd_recv_short(mdev, mdev->meta.socket,
				     buf,expect-received);
		clear_bit(SIGNAL_ASENDER, &mdev->flags);

		flush_signals(current);

		/* Note:
		 * -EINTR        (on meta) we got a signal
		 * -EAGAIN       (on meta) rcvtimeo expired
		 * -ECONNRESET   other side closed the connection
		 * -ERESTARTSYS  (on data) we got a signal
		 * rv <  0       other than above: unexpected error!
		 * rv == expected: full header or command
		 * rv <  expected: "woken" by signal during receive
		 * rv == 0       : "connection shut down by peer"
		 */
		if (likely(rv > 0)) {
			received += rv;
			buf      += rv;
		} else if (rv == 0) {
			ERR("meta connection shut down by peer.\n");
			goto err;
		} else if (rv == -EAGAIN) {
			if( mdev->meta.socket->sk->sk_rcvtimeo ==
			    mdev->net_conf->timeout*HZ/20) {
				ERR("PingAck did not arrive in time.\n");
				goto err;
			}
			set_bit(SEND_PING,&mdev->flags);
			continue;
		} else if (rv == -EINTR) {
			continue;
		} else {
			ERR("sock_recvmsg returned %d\n", rv);
			goto err;
		}

		if (received == expect && cmd == -1 ) {
			cmd = be16_to_cpu(h->command);
			len = be16_to_cpu(h->length);
			if (unlikely( h->magic != BE_DRBD_MAGIC )) {
				ERR("magic?? on meta m: 0x%lx c: %d l: %d\n",
				    (long)be32_to_cpu(h->magic),
				    h->command, h->length);
				goto err;
			}
			expect = asender_tbl[cmd].pkt_size;
			ERR_IF(len != expect-sizeof(Drbd_Header)) {
				dump_packet(mdev,mdev->meta.socket,1,(void*)h, __FILE__, __LINE__);
				DUMPI(expect);
			}
		}
		if(received == expect) {
			D_ASSERT(cmd != -1);
			dump_packet(mdev,mdev->meta.socket,1,(void*)h, __FILE__, __LINE__);
			if(!asender_tbl[cmd].process(mdev,h)) goto err;

			buf      = h;
			received = 0;
			expect   = sizeof(Drbd_Header);
			cmd      = -1;
		}
	} //while

	if(0) {
	err:
		clear_bit(SIGNAL_ASENDER, &mdev->flags);
		if (mdev->state.conn >= Connected)
			drbd_force_state(mdev,NS(conn,NetworkFailure));
		drbd_thread_restart_nowait(&mdev->receiver);
	}

	INFO("asender terminated\n");

	return 0;
}
