#include "dlm_daemon.h"
#include "config.h"

#include <linux/dlm_plock.h>

/* FIXME: remove this once everyone is using the version of
 * dlm_plock.h which defines it */

#ifndef DLM_PLOCK_FL_CLOSE
#warning DLM_PLOCK_FL_CLOSE undefined. Enabling build workaround.
#define DLM_PLOCK_FL_CLOSE 1
#define DLM_PLOCK_BUILD_WORKAROUND 1
#endif

static uint32_t plock_read_count;
static uint32_t plock_recv_count;
static uint32_t plock_rate_delays;
static struct timeval plock_read_time;
static struct timeval plock_recv_time;
static struct timeval plock_rate_last;

static int plock_device_fd = -1;
static int need_fsid_translation = 0;

extern int message_flow_control_on;

#define RD_CONTINUE 0x00000001

struct resource_data {
	uint64_t number;
	int      owner;
	uint32_t lock_count;
	uint32_t flags;
	uint32_t pad;
};

struct plock_data {
	uint64_t start;
	uint64_t end;
	uint64_t owner;
	uint32_t pid;
	uint32_t nodeid;
	uint8_t ex;
	uint8_t waiter;
	uint16_t pad1;
	uint32_t pad;
};

#define R_GOT_UNOWN   0x00000001 /* have received owner=0 message */
#define R_SEND_UNOWN  0x00000002 /* have sent owner=0 message */
#define R_SEND_OWN    0x00000004 /* have sent owner=our_nodeid message */
#define R_PURGE_UNOWN 0x00000008 /* set owner=0 in purge */
#define R_SEND_DROP   0x00000010

struct resource {
	struct list_head	list;	   /* list of resources */
	uint64_t		number;
	int                     owner;     /* nodeid or 0 for unowned */
	uint32_t		flags;
	struct timeval          last_access;
	struct list_head	locks;	   /* one lock for each range */
	struct list_head	waiters;
	struct list_head        pending;   /* discovering r owner */
	struct rb_node		rb_node;
};

#define P_SYNCING 0x00000001 /* plock has been sent as part of sync but not
				yet received */

struct posix_lock {
	struct list_head	list;	   /* resource locks or waiters list */
	uint32_t		pid;
	uint64_t		owner;
	uint64_t		start;
	uint64_t		end;
	int			ex;
	int			nodeid;
	uint32_t		flags;
};

struct lock_waiter {
	struct list_head	list;
	uint32_t		flags;
	struct dlm_plock_info	info;
};

struct save_msg {
	struct list_head list;
	int nodeid;
	int len;
	int type;
	char buf[0];
};


static void send_own(struct lockspace *ls, struct resource *r, int owner);
static void save_pending_plock(struct lockspace *ls, struct resource *r,
			       struct dlm_plock_info *in);


static int got_unown(struct resource *r)
{
	return !!(r->flags & R_GOT_UNOWN);
}

static void info_bswap_out(struct dlm_plock_info *i)
{
	i->version[0]	= cpu_to_le32(i->version[0]);
	i->version[1]	= cpu_to_le32(i->version[1]);
	i->version[2]	= cpu_to_le32(i->version[2]);
	i->pid		= cpu_to_le32(i->pid);
	i->nodeid	= cpu_to_le32(i->nodeid);
	i->rv		= cpu_to_le32(i->rv);
	i->fsid		= cpu_to_le32(i->fsid);
	i->number	= cpu_to_le64(i->number);
	i->start	= cpu_to_le64(i->start);
	i->end		= cpu_to_le64(i->end);
	i->owner	= cpu_to_le64(i->owner);
}

static void info_bswap_in(struct dlm_plock_info *i)
{
	i->version[0]	= le32_to_cpu(i->version[0]);
	i->version[1]	= le32_to_cpu(i->version[1]);
	i->version[2]	= le32_to_cpu(i->version[2]);
	i->pid		= le32_to_cpu(i->pid);
	i->nodeid	= le32_to_cpu(i->nodeid);
	i->rv		= le32_to_cpu(i->rv);
	i->fsid		= le32_to_cpu(i->fsid);
	i->number	= le64_to_cpu(i->number);
	i->start	= le64_to_cpu(i->start);
	i->end		= le64_to_cpu(i->end);
	i->owner	= le64_to_cpu(i->owner);
}

static const char *op_str(int optype)
{
	switch (optype) {
	case DLM_PLOCK_OP_LOCK:
		return "LK";
	case DLM_PLOCK_OP_UNLOCK:
		return "UN";
	case DLM_PLOCK_OP_GET:
		return "GET";
	default:
		return "??";
	}
}

static const char *ex_str(int optype, int ex)
{
	if (optype == DLM_PLOCK_OP_UNLOCK || optype == DLM_PLOCK_OP_GET)
		return "-";
	if (ex)
		return "WR";
	else
		return "RD";
}

/*
 * In kernels before 2.6.26, plocks came from gfs2's lock_dlm module.
 * Reading plocks from there as well should allow us to use cluster3
 * on old (RHEL5) kernels.  In this case, the fsid we read in plock_info
 * structs is the mountgroup id, which we need to translate to the ls id.
 */

int setup_plocks(void)
{
	plock_read_count = 0;
	plock_recv_count = 0;
	plock_rate_delays = 0;
	gettimeofday(&plock_read_time, NULL);
	gettimeofday(&plock_recv_time, NULL);
	gettimeofday(&plock_rate_last, NULL);

	if (plock_minor) {
		plock_device_fd = open("/dev/misc/dlm_plock", O_RDWR);
	} else if (old_plock_minor) {
		log_debug("setup_plocks using old lock_dlm interface");
		need_fsid_translation = 1;
		plock_device_fd = open("/dev/misc/lock_dlm_plock", O_RDWR);
	}

	if (plock_device_fd < 0) {
		log_error("Failure to open plock device: %s", strerror(errno));
		return -1;
	}

	log_debug("plocks %d", plock_device_fd);
	log_debug("plock cpg message size: %u bytes",
		  (unsigned int) (sizeof(struct dlm_header) +
		                  sizeof(struct dlm_plock_info)));

	return plock_device_fd;
}

void close_plocks(void)
{
	if (plock_device_fd > 0)
		close(plock_device_fd);
}

static uint32_t mg_to_ls_id(uint32_t fsid)
{
	struct lockspace *ls;
	int do_set = 1;

 retry:
	list_for_each_entry(ls, &lockspaces, list) {
		if (ls->associated_mg_id == fsid)
			return ls->global_id;
	}

	if (do_set) {
		do_set = 0;
		set_associated_id(fsid);
		goto retry;
	}

	return fsid;
}

/* FIXME: unify these two */

static unsigned long time_diff_ms(struct timeval *begin, struct timeval *end)
{
	struct timeval result;
	timersub(end, begin, &result);
	return (result.tv_sec * 1000) + (result.tv_usec / 1000);
}

static uint64_t dt_usec(struct timeval *start, struct timeval *stop)
{
	uint64_t dt;

	dt = stop->tv_sec - start->tv_sec;
	dt *= 1000000;
	dt += stop->tv_usec - start->tv_usec;
	return dt;
}

static struct resource * rb_search_plock_resource(struct lockspace *ls, uint64_t number)
{
	struct rb_node *n = ls->plock_resources_root.rb_node;
	struct resource *r;

	while (n) {
		r = rb_entry(n, struct resource, rb_node);
		if (number < r->number)
			n = n->rb_left;
		else if (number > r->number)
			n = n->rb_right;
		else
			return r;
	}
	return NULL;
}

static void rb_insert_plock_resource(struct lockspace *ls, struct resource *r)
{
	struct resource *entry;
	struct rb_node **p;
	struct rb_node *parent = NULL;
	
	p = &ls->plock_resources_root.rb_node;
	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct resource, rb_node);
		if (r->number < entry->number)
			p = &parent->rb_left;
		else if (r->number > entry->number)
			p = &parent->rb_right;
		else
			return; 
	}
	rb_link_node(&r->rb_node, parent, p);
	rb_insert_color(&r->rb_node, &ls->plock_resources_root);
}

static void rb_del_plock_resource(struct lockspace *ls, struct resource *r)
{
	if (!RB_EMPTY_NODE(&r->rb_node)) {
		rb_erase(&r->rb_node, &ls->plock_resources_root);
		RB_CLEAR_NODE(&r->rb_node);
	}
}

static struct resource *search_resource(struct lockspace *ls, uint64_t number)
{
	struct resource *r;

	list_for_each_entry(r, &ls->plock_resources, list) {
		if (r->number == number)
			return r;
	}
	return NULL;
}

static int find_resource(struct lockspace *ls, uint64_t number, int create,
			 struct resource **r_out)
{
	struct resource *r = NULL;
	int rv = 0;

	r = rb_search_plock_resource(ls, number);
	if (r)
		goto out;

	if (create == 0) {
		rv = -ENOENT;
		goto out;
	}

	r = malloc(sizeof(struct resource));
	if (!r) {
		log_elock(ls, "find_resource no memory %d", errno);
		rv = -ENOMEM;
		goto out;
	}

	memset(r, 0, sizeof(struct resource));
	r->number = number;
	INIT_LIST_HEAD(&r->locks);
	INIT_LIST_HEAD(&r->waiters);
	INIT_LIST_HEAD(&r->pending);

	if (cfgd_plock_ownership)
		r->owner = -1;
	else
		r->owner = 0;

	list_add_tail(&r->list, &ls->plock_resources);
	rb_insert_plock_resource(ls, r);
 out:
	if (r)
		gettimeofday(&r->last_access, NULL);
	*r_out = r;
	return rv;
}

static void put_resource(struct lockspace *ls, struct resource *r)
{
	/* with ownership, resources are only freed via drop messages */
	if (cfgd_plock_ownership)
		return;

	if (list_empty(&r->locks) && list_empty(&r->waiters)) {
		rb_del_plock_resource(ls, r);
		list_del(&r->list);
		free(r);
	}
}

static inline int ranges_overlap(uint64_t start1, uint64_t end1,
				 uint64_t start2, uint64_t end2)
{
	if (end1 < start2 || start1 > end2)
		return 0;
	return 1;
}

/**
 * overlap_type - returns a value based on the type of overlap
 * @s1 - start of new lock range
 * @e1 - end of new lock range
 * @s2 - start of existing lock range
 * @e2 - end of existing lock range
 *
 */

static int overlap_type(uint64_t s1, uint64_t e1, uint64_t s2, uint64_t e2)
{
	int ret;

	/*
	 * ---r1---
	 * ---r2---
	 */

	if (s1 == s2 && e1 == e2)
		ret = 0;

	/*
	 * --r1--
	 * ---r2---
	 */

	else if (s1 == s2 && e1 < e2)
		ret = 1;

	/*
	 *   --r1--
	 * ---r2---
	 */

	else if (s1 > s2 && e1 == e2)
		ret = 1;

	/*
	 *  --r1--
	 * ---r2---
	 */

	else if (s1 > s2 && e1 < e2)
		ret = 2;

	/*
	 * ---r1---  or  ---r1---  or  ---r1---
	 * --r2--	  --r2--       --r2--
	 */

	else if (s1 <= s2 && e1 >= e2)
		ret = 3;

	/*
	 *   ---r1---
	 * ---r2---
	 */

	else if (s1 > s2 && e1 > e2)
		ret = 4;

	/*
	 * ---r1---
	 *   ---r2---
	 */

	else if (s1 < s2 && e1 < e2)
		ret = 4;

	else
		ret = -1;

	return ret;
}

/* shrink the range start2:end2 by the partially overlapping start:end */

static int shrink_range2(uint64_t *start2, uint64_t *end2,
			 uint64_t start, uint64_t end)
{
	int error = 0;

	if (*start2 < start)
		*end2 = start - 1;
	else if (*end2 > end)
		*start2 =  end + 1;
	else
		error = -1;
	return error;
}

static int shrink_range(struct posix_lock *po, uint64_t start, uint64_t end)
{
	return shrink_range2(&po->start, &po->end, start, end);
}

static int is_conflict(struct resource *r, struct dlm_plock_info *in, int get)
{
	struct posix_lock *po;

	list_for_each_entry(po, &r->locks, list) {
		if (po->nodeid == in->nodeid && po->owner == in->owner)
			continue;
		if (!ranges_overlap(po->start, po->end, in->start, in->end))
			continue;

		if (in->ex || po->ex) {
			if (get) {
				in->ex = po->ex;
				in->pid = po->pid;
				in->start = po->start;
				in->end = po->end;
			}
			return 1;
		}
	}
	return 0;
}

static int add_lock(struct resource *r, uint32_t nodeid, uint64_t owner,
		    uint32_t pid, int ex, uint64_t start, uint64_t end)
{
	struct posix_lock *po;

	po = malloc(sizeof(struct posix_lock));
	if (!po)
		return -ENOMEM;
	memset(po, 0, sizeof(struct posix_lock));

	po->start = start;
	po->end = end;
	po->nodeid = nodeid;
	po->owner = owner;
	po->pid = pid;
	po->ex = ex;
	list_add_tail(&po->list, &r->locks);

	return 0;
}

/* RN within RE (and starts or ends on RE boundary)
   1. add new lock for non-overlap area of RE, orig mode
   2. convert RE to RN range and mode */

static int lock_case1(struct posix_lock *po, struct resource *r,
		      struct dlm_plock_info *in)
{
	uint64_t start2, end2;
	int rv;

	/* non-overlapping area start2:end2 */
	start2 = po->start;
	end2 = po->end;
	rv = shrink_range2(&start2, &end2, in->start, in->end);
	if (rv)
		goto out;

	po->start = in->start;
	po->end = in->end;
	po->ex = in->ex;

	rv = add_lock(r, in->nodeid, in->owner, in->pid, !in->ex, start2, end2);
 out:
	return rv;
}

/* RN within RE (RE overlaps RN on both sides)
   1. add new lock for front fragment, orig mode
   2. add new lock for back fragment, orig mode
   3. convert RE to RN range and mode */
			 
static int lock_case2(struct posix_lock *po, struct resource *r,
		      struct dlm_plock_info *in)

{
	int rv;

	rv = add_lock(r, in->nodeid, in->owner, in->pid,
		      !in->ex, po->start, in->start - 1);
	if (rv)
		goto out;

	rv = add_lock(r, in->nodeid, in->owner, in->pid,
		      !in->ex, in->end + 1, po->end);
	if (rv)
		goto out;

	po->start = in->start;
	po->end = in->end;
	po->ex = in->ex;
 out:
	return rv;
}

static int lock_internal(struct lockspace *ls, struct resource *r,
			 struct dlm_plock_info *in)
{
	struct posix_lock *po, *safe;
	int rv = 0;

	list_for_each_entry_safe(po, safe, &r->locks, list) {
		if (po->nodeid != in->nodeid || po->owner != in->owner)
			continue;
		if (!ranges_overlap(po->start, po->end, in->start, in->end))
			continue;

		/* existing range (RE) overlaps new range (RN) */

		switch(overlap_type(in->start, in->end, po->start, po->end)) {

		case 0:
			if (po->ex == in->ex)
				goto out;

			/* ranges the same - just update the existing lock */
			po->ex = in->ex;
			goto out;

		case 1:
			if (po->ex == in->ex)
				goto out;

			rv = lock_case1(po, r, in);
			goto out;

		case 2:
			if (po->ex == in->ex)
				goto out;

			rv = lock_case2(po, r, in);
			goto out;

		case 3:
			list_del(&po->list);
			free(po);
			break;

		case 4:
			if (po->start < in->start)
				po->end = in->start - 1;
			else
				po->start = in->end + 1;
			break;

		default:
			rv = -1;
			goto out;
		}
	}

	rv = add_lock(r, in->nodeid, in->owner, in->pid,
		      in->ex, in->start, in->end);
 out:
	return rv;

}

static int unlock_internal(struct lockspace *ls, struct resource *r,
			   struct dlm_plock_info *in)
{
	struct posix_lock *po, *safe;
	int rv = 0;

	list_for_each_entry_safe(po, safe, &r->locks, list) {
		if (po->nodeid != in->nodeid || po->owner != in->owner)
			continue;
		if (!ranges_overlap(po->start, po->end, in->start, in->end))
			continue;

		/* existing range (RE) overlaps new range (RN) */

		switch (overlap_type(in->start, in->end, po->start, po->end)) {

		case 0:
			/* ranges the same - just remove the existing lock */

			list_del(&po->list);
			free(po);
			goto out;

		case 1:
			/* RN within RE and starts or ends on RE boundary -
			 * shrink and update RE */

			rv = shrink_range(po, in->start, in->end);
			goto out;

		case 2:
			/* RN within RE - shrink and update RE to be front
			 * fragment, and add a new lock for back fragment */

			rv = add_lock(r, in->nodeid, in->owner, in->pid,
				      po->ex, in->end + 1, po->end);
			po->end = in->start - 1;
			goto out;

		case 3:
			/* RE within RN - remove RE, then continue checking
			 * because RN could cover other locks */

			list_del(&po->list);
			free(po);
			continue;

		case 4:
			/* front of RE in RN, or end of RE in RN - shrink and
			 * update RE, then continue because RN could cover
			 * other locks */

			rv = shrink_range(po, in->start, in->end);
			continue;

		default:
			rv = -1;
			goto out;
		}
	}
 out:
	return rv;
}

static void clear_waiters(struct lockspace *ls, struct resource *r,
			  struct dlm_plock_info *in)
{
	struct lock_waiter *w, *safe;

	list_for_each_entry_safe(w, safe, &r->waiters, list) {
		if (w->info.nodeid != in->nodeid || w->info.owner != in->owner)
			continue;

		list_del(&w->list);

		log_elock(ls, "clear waiter %llx %llx-%llx %d/%u/%llx",
			  (unsigned long long)in->number,
			  (unsigned long long)in->start,
			  (unsigned long long)in->end,
			  in->nodeid, in->pid,
			  (unsigned long long)in->owner);
		free(w);
	}
}

static int add_waiter(struct lockspace *ls, struct resource *r,
		      struct dlm_plock_info *in)

{
	struct lock_waiter *w;

	w = malloc(sizeof(struct lock_waiter));
	if (!w)
		return -ENOMEM;
	memcpy(&w->info, in, sizeof(struct dlm_plock_info));
	list_add_tail(&w->list, &r->waiters);
	return 0;
}

static void write_result(struct lockspace *ls, struct dlm_plock_info *in,
			 int rv)
{
	if (need_fsid_translation)
		in->fsid = ls->associated_mg_id;

	in->rv = rv;
	write(plock_device_fd, in, sizeof(struct dlm_plock_info));
}

static void do_waiters(struct lockspace *ls, struct resource *r)
{
	struct lock_waiter *w, *safe;
	struct dlm_plock_info *in;
	int rv;

	list_for_each_entry_safe(w, safe, &r->waiters, list) {
		in = &w->info;

		if (is_conflict(r, in, 0))
			continue;

		list_del(&w->list);

		/*
		log_group(ls, "take waiter %llx %llx-%llx %d/%u/%llx",
			  in->number, in->start, in->end,
			  in->nodeid, in->pid, in->owner);
		*/

		rv = lock_internal(ls, r, in);

		if (in->nodeid == our_nodeid)
			write_result(ls, in, rv);

		free(w);
	}
}

static void do_lock(struct lockspace *ls, struct dlm_plock_info *in,
		    struct resource *r)
{
	int rv;

	if (is_conflict(r, in, 0)) {
		if (!in->wait)
			rv = -EAGAIN;
		else {
			rv = add_waiter(ls, r, in);
			if (rv)
				goto out;
			rv = -EINPROGRESS;
		}
	} else
		rv = lock_internal(ls, r, in);

 out:
	if (in->nodeid == our_nodeid && rv != -EINPROGRESS)
		write_result(ls, in, rv);

	do_waiters(ls, r);
	put_resource(ls, r);
}

static void do_unlock(struct lockspace *ls, struct dlm_plock_info *in,
		      struct resource *r)
{
	int rv;

	rv = unlock_internal(ls, r, in);

#ifdef DLM_PLOCK_BUILD_WORKAROUND
	if (in->pad & DLM_PLOCK_FL_CLOSE) {
#else
	if (in->flags & DLM_PLOCK_FL_CLOSE) {
#endif
		clear_waiters(ls, r, in);
		/* no replies for unlock-close ops */
		goto skip_result;
	}

	if (in->nodeid == our_nodeid)
		write_result(ls, in, rv);

 skip_result:
	do_waiters(ls, r);
	put_resource(ls, r);
}

/* we don't even get to this function if the getlk isn't from us */

static void do_get(struct lockspace *ls, struct dlm_plock_info *in,
		   struct resource *r)
{
	int rv;

	if (is_conflict(r, in, 1))
		rv = 1;
	else
		rv = 0;

	write_result(ls, in, rv);
	put_resource(ls, r);
}

static void save_message(struct lockspace *ls, struct dlm_header *hd, int len,
			 int from, int type)
{
	struct save_msg *sm;

	sm = malloc(sizeof(struct save_msg) + len);
	if (!sm)
		return;
	memset(sm, 0, sizeof(struct save_msg) + len);

	memcpy(&sm->buf, hd, len);
	sm->type = type;
	sm->len = len;
	sm->nodeid = from;

	log_plock(ls, "save %s from %d len %d", msg_name(type), from, len);

	list_add_tail(&sm->list, &ls->saved_messages);
}

static void __receive_plock(struct lockspace *ls, struct dlm_plock_info *in,
			    int from, struct resource *r)
{
	switch (in->optype) {
	case DLM_PLOCK_OP_LOCK:
		ls->last_plock_time = time(NULL);
		do_lock(ls, in, r);
		break;
	case DLM_PLOCK_OP_UNLOCK:
		ls->last_plock_time = time(NULL);
		do_unlock(ls, in, r);
		break;
	case DLM_PLOCK_OP_GET:
		do_get(ls, in, r);
		break;
	default:
		log_elock(ls, "receive_plock error from %d optype %d",
			  from, in->optype);
		if (from == our_nodeid)
			write_result(ls, in, -EINVAL);
	}
}

/* When ls members receive our options message (for our mount), one of them
   saves all plock state received to that point in a checkpoint and then sends
   us our journals message.  We know to retrieve the plock state from the
   checkpoint when we receive our journals message.  Any plocks messages that
   arrive between seeing our options message and our journals message needs to
   be saved and processed after we synchronize our plock state from the
   checkpoint.  Any plock message received while we're mounting but before we
   set save_plocks (when we see our options message) can be ignored because it
   should be reflected in the checkpointed state. */

static void _receive_plock(struct lockspace *ls, struct dlm_header *hd, int len)
{
	struct dlm_plock_info info;
	struct resource *r = NULL;
	struct timeval now;
	uint64_t usec;
	int from = hd->nodeid;
	int rv, create;

	memcpy(&info, (char *)hd + sizeof(struct dlm_header), sizeof(info));
	info_bswap_in(&info);

	log_plock(ls, "receive plock %llx %s %s %llx-%llx %d/%u/%llx w %d",
		  (unsigned long long)info.number,
		  op_str(info.optype),
		  ex_str(info.optype, info.ex),
		  (unsigned long long)info.start, (unsigned long long)info.end,
		  info.nodeid, info.pid, (unsigned long long)info.owner,
		  info.wait);

	plock_recv_count++;
	if (!(plock_recv_count % 1000)) {
		gettimeofday(&now, NULL);
		usec = dt_usec(&plock_recv_time, &now);
		log_plock(ls, "plock_recv_count %u time %.3f s",
			  plock_recv_count, usec * 1.e-6);
		plock_recv_time = now;
	}

	if (info.optype == DLM_PLOCK_OP_GET && from != our_nodeid)
		return;

	if (from != hd->nodeid || from != info.nodeid) {
		log_elock(ls, "receive_plock error from %d header %d info %d",
			  from, hd->nodeid, info.nodeid);
		return;
	}

	create = !cfgd_plock_ownership;

	rv = find_resource(ls, info.number, create, &r);

	if (rv && cfgd_plock_ownership) {
		/* There must have been a race with a drop, so we need to
		   ignore this plock op which will be resent.  If we're the one
		   who sent the plock, we need to send_own() and put it on the
		   pending list to resend once the owner is established. */

		log_plock(ls, "receive_plock from %d no r %llx", from,
			  (unsigned long long)info.number);

		if (from != our_nodeid)
			return;

		rv = find_resource(ls, info.number, 1, &r);
		if (rv)
			return;
		send_own(ls, r, our_nodeid);
		save_pending_plock(ls, r, &info);
		return;
	}
	if (rv) {
		/* r not found, rv is -ENOENT, this shouldn't happen because
		   process_plocks() creates a resource for every op */

		log_elock(ls, "receive_plock error from %d no r %llx %d",
			  from, (unsigned long long)info.number, rv);
		return;
	}

	/* The owner should almost always be 0 here, but other owners may
	   be possible given odd combinations of races with drop.  Odd races to
	   worry about (some seem pretty improbable):

	   - A sends drop, B sends plock, receive drop, receive plock.
	   This is addressed above.

	   - A sends drop, B sends plock, receive drop, B reads plock
	   and sends own, receive plock, on B we find owner of -1.

	   - A sends drop, B sends two plocks, receive drop, receive plocks.
	   Receiving the first plock is the previous case, receiving the
	   second plock will find r with owner of -1.

	   - A sends drop, B sends two plocks, receive drop, C sends own,
	   receive plock, B sends own, receive own (C), receive plock,
	   receive own (B).

	   Haven't tried to cook up a scenario that would lead to the
	   last case below; receiving a plock from ourself and finding
	   we're the owner of r. */

	if (!r->owner) {
		__receive_plock(ls, &info, from, r);

	} else if (r->owner == -1) {
		log_plock(ls, "receive_plock from %d r %llx owner %d", from,
			  (unsigned long long)info.number, r->owner);

		if (from == our_nodeid)
			save_pending_plock(ls, r, &info);

	} else if (r->owner != our_nodeid) {
		log_plock(ls, "receive_plock from %d r %llx owner %d", from,
			  (unsigned long long)info.number, r->owner);

		if (from == our_nodeid)
			save_pending_plock(ls, r, &info);

	} else if (r->owner == our_nodeid) {
		log_plock(ls, "receive_plock from %d r %llx owner %d", from,
			  (unsigned long long)info.number, r->owner);

		if (from == our_nodeid)
			__receive_plock(ls, &info, from, r);
	}
}

void receive_plock(struct lockspace *ls, struct dlm_header *hd, int len)
{
	if (ls->save_plocks) {
		save_message(ls, hd, len, hd->nodeid, DLM_MSG_PLOCK);
		return;
	}

	_receive_plock(ls, hd, len);
}

static int send_struct_info(struct lockspace *ls, struct dlm_plock_info *in,
			    int msg_type)
{
	struct dlm_header *hd;
	int rv = 0, len;
	char *buf;

	len = sizeof(struct dlm_header) + sizeof(struct dlm_plock_info);
	buf = malloc(len);
	if (!buf) {
		rv = -ENOMEM;
		goto out;
	}
	memset(buf, 0, len);

	info_bswap_out(in);

	hd = (struct dlm_header *)buf;
	hd->type = msg_type;

	memcpy(buf + sizeof(struct dlm_header), in, sizeof(*in));

	dlm_send_message(ls, buf, len);

	free(buf);
 out:
	if (rv)
		log_elock(ls, "send_struct_info error %d", rv);
	return rv;
}

static void send_plock(struct lockspace *ls, struct resource *r,
		       struct dlm_plock_info *in)
{
	send_struct_info(ls, in, DLM_MSG_PLOCK);
}

static void send_own(struct lockspace *ls, struct resource *r, int owner)
{
	struct dlm_plock_info info;

	/* if we've already sent an own message for this resource,
	   (pending list is not empty), then we shouldn't send another */

	if (!list_empty(&r->pending)) {
		log_plock(ls, "send_own %llx already pending",
			  (unsigned long long)r->number);
		return;
	}

	if (!owner)
		r->flags |= R_SEND_UNOWN;
	else
		r->flags |= R_SEND_OWN;

	memset(&info, 0, sizeof(info));
	info.number = r->number;
	info.nodeid = owner;

	send_struct_info(ls, &info, DLM_MSG_PLOCK_OWN);
}

static void send_syncs(struct lockspace *ls, struct resource *r)
{
	struct dlm_plock_info info;
	struct posix_lock *po;
	struct lock_waiter *w;
	int rv;

	list_for_each_entry(po, &r->locks, list) {
		memset(&info, 0, sizeof(info));
		info.number    = r->number;
		info.start     = po->start;
		info.end       = po->end;
		info.nodeid    = po->nodeid;
		info.owner     = po->owner;
		info.pid       = po->pid;
		info.ex        = po->ex;

		rv = send_struct_info(ls, &info, DLM_MSG_PLOCK_SYNC_LOCK);
		if (rv)
			goto out;

		po->flags |= P_SYNCING;
	}

	list_for_each_entry(w, &r->waiters, list) {
		memcpy(&info, &w->info, sizeof(info));

		rv = send_struct_info(ls, &info, DLM_MSG_PLOCK_SYNC_WAITER);
		if (rv)
			goto out;

		w->flags |= P_SYNCING;
	}
 out:
	return;
}

static void send_drop(struct lockspace *ls, struct resource *r)
{
	struct dlm_plock_info info;

	memset(&info, 0, sizeof(info));
	info.number = r->number;
	r->flags |= R_SEND_DROP;

	send_struct_info(ls, &info, DLM_MSG_PLOCK_DROP);
}

/* plock op can't be handled until we know the owner value of the resource,
   so the op is saved on the pending list until the r owner is established */

static void save_pending_plock(struct lockspace *ls, struct resource *r,
			       struct dlm_plock_info *in)
{
	struct lock_waiter *w;

	w = malloc(sizeof(struct lock_waiter));
	if (!w) {
		log_elock(ls, "save_pending_plock no mem");
		return;
	}
	memcpy(&w->info, in, sizeof(struct dlm_plock_info));
	list_add_tail(&w->list, &r->pending);
}

/* plock ops are on pending list waiting for ownership to be established.
   owner has now become us, so add these plocks to r */

static void add_pending_plocks(struct lockspace *ls, struct resource *r)
{
	struct lock_waiter *w, *safe;

	list_for_each_entry_safe(w, safe, &r->pending, list) {
		__receive_plock(ls, &w->info, our_nodeid, r);
		list_del(&w->list);
		free(w);
	}
}

/* plock ops are on pending list waiting for ownership to be established.
   owner has now become 0, so send these plocks to everyone */

static void send_pending_plocks(struct lockspace *ls, struct resource *r)
{
	struct lock_waiter *w, *safe;

	list_for_each_entry_safe(w, safe, &r->pending, list) {
		send_plock(ls, r, &w->info);
		list_del(&w->list);
		free(w);
	}
}

static void _receive_own(struct lockspace *ls, struct dlm_header *hd, int len)
{
	struct dlm_plock_info info;
	struct resource *r;
	int should_not_happen = 0;
	int from = hd->nodeid;
	int rv;

	ls->last_plock_time = time(NULL);

	memcpy(&info, (char *)hd + sizeof(struct dlm_header), sizeof(info));
	info_bswap_in(&info);

	log_plock(ls, "receive_own %llx from %u owner %u",
		  (unsigned long long)info.number, hd->nodeid, info.nodeid);

	rv = find_resource(ls, info.number, 1, &r);
	if (rv)
		return;

	if (from == our_nodeid) {
		/*
		 * received our own own message
		 */

		if (info.nodeid == 0) {
			/* we are setting owner to 0 */

			if (r->owner == our_nodeid) {
				/* we set owner to 0 when we relinquish
				   ownership */
				should_not_happen = 1;
			} else if (r->owner == 0) {
				/* this happens when we relinquish ownership */
				r->flags |= R_GOT_UNOWN;
			} else {
				should_not_happen = 1;
			}

		} else if (info.nodeid == our_nodeid) {
			/* we are setting owner to ourself */

			if (r->owner == -1) {
				/* we have gained ownership */
				r->owner = our_nodeid;
				add_pending_plocks(ls, r);
			} else if (r->owner == our_nodeid) {
				should_not_happen = 1;
			} else if (r->owner == 0) {
				send_pending_plocks(ls, r);
			} else {
				/* resource is owned by other node;
				   they should set owner to 0 shortly */
			}

		} else {
			/* we should only ever set owner to 0 or ourself */
			should_not_happen = 1;
		}
	} else {
		/*
		 * received own message from another node
		 */

		if (info.nodeid == 0) {
			/* other node is setting owner to 0 */

			if (r->owner == -1) {
				/* we should have a record of the owner before
				   it relinquishes */
				should_not_happen = 1;
			} else if (r->owner == our_nodeid) {
				/* only the owner should relinquish */
				should_not_happen = 1;
			} else if (r->owner == 0) {
				should_not_happen = 1;
			} else {
				r->owner = 0;
				r->flags |= R_GOT_UNOWN;
				send_pending_plocks(ls, r);
			}

		} else if (info.nodeid == from) {
			/* other node is setting owner to itself */

			if (r->owner == -1) {
				/* normal path for a node becoming owner */
				r->owner = from;
			} else if (r->owner == our_nodeid) {
				/* we relinquish our ownership: sync our local
				   plocks to everyone, then set owner to 0 */
				send_syncs(ls, r);
				send_own(ls, r, 0);
				/* we need to set owner to 0 here because
				   local ops may arrive before we receive
				   our send_own message and can't be added
				   locally */
				r->owner = 0;
			} else if (r->owner == 0) {
				/* can happen because we set owner to 0 before
				   we receive our send_own sent just above */
			} else {
				/* do nothing, current owner should be
				   relinquishing its ownership */
			}

		} else if (info.nodeid == our_nodeid) {
			/* no one else should try to set the owner to us */
			should_not_happen = 1;
		} else {
			/* a node should only ever set owner to 0 or itself */
			should_not_happen = 1;
		}
	}

	if (should_not_happen) {
		log_elock(ls, "receive_own error from %u %llx "
			  "info nodeid %d r owner %d",
			  from, (unsigned long long)r->number,
			  info.nodeid, r->owner);
	}
}

void receive_own(struct lockspace *ls, struct dlm_header *hd, int len)
{
	if (ls->save_plocks) {
		save_message(ls, hd, len, hd->nodeid, DLM_MSG_PLOCK_OWN);
		return;
	}

	_receive_own(ls, hd, len);
}

static void clear_syncing_flag(struct lockspace *ls, struct resource *r,
			       struct dlm_plock_info *in)
{
	struct posix_lock *po;
	struct lock_waiter *w;

	list_for_each_entry(po, &r->locks, list) {
		if ((po->flags & P_SYNCING) &&
		    in->start  == po->start &&
		    in->end    == po->end &&
		    in->nodeid == po->nodeid &&
		    in->owner  == po->owner &&
		    in->pid    == po->pid &&
		    in->ex     == po->ex) {
			po->flags &= ~P_SYNCING;
			return;
		}
	}

	list_for_each_entry(w, &r->waiters, list) {
		if ((w->flags & P_SYNCING) &&
		    in->start  == w->info.start &&
		    in->end    == w->info.end &&
		    in->nodeid == w->info.nodeid &&
		    in->owner  == w->info.owner &&
		    in->pid    == w->info.pid &&
		    in->ex     == w->info.ex) {
			w->flags &= ~P_SYNCING;
			return;
		}
	}

	log_elock(ls, "clear_syncing error %llx no match %s %llx-%llx %d/%u/%llx",
		  (unsigned long long)r->number,
		  in->ex ? "WR" : "RD", 
		  (unsigned long long)in->start,
		  (unsigned long long)in->end,
		  in->nodeid, in->pid,
		  (unsigned long long)in->owner);
}

static void _receive_sync(struct lockspace *ls, struct dlm_header *hd, int len)
{
	struct dlm_plock_info info;
	struct resource *r;
	int from = hd->nodeid;
	int rv;

	ls->last_plock_time = time(NULL);

	memcpy(&info, (char *)hd + sizeof(struct dlm_header), sizeof(info));
	info_bswap_in(&info);

	log_plock(ls, "receive sync %llx from %u %s %llx-%llx %d/%u/%llx",
		  (unsigned long long)info.number, from, info.ex ? "WR" : "RD",
		  (unsigned long long)info.start, (unsigned long long)info.end,
		  info.nodeid, info.pid, (unsigned long long)info.owner);

	rv = find_resource(ls, info.number, 0, &r);
	if (rv) {
		log_elock(ls, "receive_sync error no r %llx from %d",
			  info.number, from);
		return;
	}

	if (from == our_nodeid) {
		/* this plock now in sync on all nodes */
		clear_syncing_flag(ls, r, &info);
		return;
	}

	if (hd->type == DLM_MSG_PLOCK_SYNC_LOCK)
		add_lock(r, info.nodeid, info.owner, info.pid, info.ex, 
			 info.start, info.end);
	else if (hd->type == DLM_MSG_PLOCK_SYNC_WAITER)
		add_waiter(ls, r, &info);
}

void receive_sync(struct lockspace *ls, struct dlm_header *hd, int len)
{
	if (ls->save_plocks) {
		save_message(ls, hd, len, hd->nodeid, hd->type);
		return;
	}

	_receive_sync(ls, hd, len);
}

static void _receive_drop(struct lockspace *ls, struct dlm_header *hd, int len)
{
	struct dlm_plock_info info;
	struct resource *r;
	int from = hd->nodeid;
	int rv;

	ls->last_plock_time = time(NULL);

	memcpy(&info, (char *)hd + sizeof(struct dlm_header), sizeof(info));
	info_bswap_in(&info);

	log_plock(ls, "receive_drop %llx from %u",
		  (unsigned long long)info.number, from);

	rv = find_resource(ls, info.number, 0, &r);
	if (rv) {
		/* we'll find no r if two nodes sent drop at once */
		log_plock(ls, "receive_drop from %d no r %llx", from,
			  (unsigned long long)info.number);
		return;
	}

	if (r->owner != 0) {
		/* - A sent drop, B sent drop, receive drop A, C sent own,
		     receive drop B (this warning on C, owner -1)
	   	   - A sent drop, B sent drop, receive drop A, A sent own,
		     receive own A, receive drop B (this warning on all,
		     owner A) */
		log_plock(ls, "receive_drop from %d r %llx owner %d", from,
			  (unsigned long long)r->number, r->owner);
		return;
	}

	if (!list_empty(&r->pending)) {
		/* shouldn't happen */
		log_elock(ls, "receive_drop error from %d r %llx pending op",
			  from, (unsigned long long)r->number);
		return;
	}

	/* the decision to drop or not must be based on things that are
	   guaranteed to be the same on all nodes */

	if (list_empty(&r->locks) && list_empty(&r->waiters)) {
		rb_del_plock_resource(ls, r);
		list_del(&r->list);
		free(r);
	} else {
		/* A sent drop, B sent a plock, receive plock, receive drop */
		log_plock(ls, "receive_drop from %d r %llx in use", from,
			  (unsigned long long)r->number);
	}
}

void receive_drop(struct lockspace *ls, struct dlm_header *hd, int len)
{
	if (ls->save_plocks) {
		save_message(ls, hd, len, hd->nodeid, DLM_MSG_PLOCK_DROP);
		return;
	}

	_receive_drop(ls, hd, len);
}

/* We only drop resources from the unowned state to simplify things.
   If we want to drop a resource we own, we unown/relinquish it first. */

/* FIXME: in the transition from owner = us, to owner = 0, to drop;
   we want the second period to be shorter than the first */

static int drop_resources(struct lockspace *ls)
{
	struct resource *r;
	struct timeval now;
	int count = 0;

	if (!cfgd_plock_ownership)
		return 0;

	if (list_empty(&ls->plock_resources))
		return 0;

	gettimeofday(&now, NULL);

	if (time_diff_ms(&ls->drop_resources_last, &now) <
			 cfgd_drop_resources_time)
		return 1;

	ls->drop_resources_last = now;

	/* try to drop the oldest, unused resources */

	list_for_each_entry_reverse(r, &ls->plock_resources, list) {
		if (count >= cfgd_drop_resources_count)
			break;
		if (r->owner && r->owner != our_nodeid)
			continue;
		if (time_diff_ms(&r->last_access, &now) <
		    cfgd_drop_resources_age)
			continue;

		if (list_empty(&r->locks) && list_empty(&r->waiters)) {
			if (r->owner == our_nodeid) {
				send_own(ls, r, 0);
				r->owner = 0;
			} else if (r->owner == 0 && got_unown(r)) {
				send_drop(ls, r);
			}

			count++;
		}
	}

	return 1;
}

void drop_resources_all(void)
{
	struct lockspace *ls;
	int rv = 0;

	poll_drop_plock = 0;

	list_for_each_entry(ls, &lockspaces, list) {
		rv = drop_resources(ls);
		if (rv)
			poll_drop_plock = 1;
	}
}

int limit_plocks(void)
{
	struct timeval now;

	/* Don't send more messages while the cpg message queue is backed up */

	if (message_flow_control_on) {
		update_flow_control_status();
		if (message_flow_control_on)
			return 1;
	}

	if (!cfgd_plock_rate_limit || !plock_read_count)
		return 0;

	gettimeofday(&now, NULL);

	/* Every time a plock op is read from the kernel, we increment
	   plock_read_count.  After every cfgd_plock_rate_limit (N) reads,
	   we check the time it's taken to do those N; if the time is less than
	   a second, then we delay reading any more until a second is up.
	   This way we read a max of N ops from the kernel every second. */

	if (!(plock_read_count % cfgd_plock_rate_limit)) {
		if (time_diff_ms(&plock_rate_last, &now) < 1000) {
			plock_rate_delays++;
			return 2;
		}
		plock_rate_last = now;
		plock_read_count++;
	}
	return 0;
}

void process_plocks(int ci)
{
	struct lockspace *ls;
	struct resource *r;
	struct dlm_plock_info info;
	struct timeval now;
	uint64_t usec;
	int create, rv;

	if (limit_plocks()) {
		poll_ignore_plock = 1;
		client_ignore(plock_ci, plock_fd);
		return;
	}

	gettimeofday(&now, NULL);

	memset(&info, 0, sizeof(info));

	rv = do_read(plock_device_fd, &info, sizeof(info));
	if (rv < 0) {
		log_debug("process_plocks: read error %d fd %d\n",
			  errno, plock_device_fd);
		return;
	}

	/* kernel doesn't set the nodeid field */
	info.nodeid = our_nodeid;

	if (!cfgd_enable_plock) {
		rv = -ENOSYS;
		goto fail;
	}

	if (need_fsid_translation)
		info.fsid = mg_to_ls_id(info.fsid);

	ls = find_ls_id(info.fsid);
	if (!ls) {
		log_plock(ls, "process_plocks: no ls id %x", info.fsid);
		rv = -EEXIST;
		goto fail;
	}

	if (ls->disable_plock) {
		rv = -ENOSYS;
		goto fail;
	}

	log_plock(ls, "read plock %llx %s %s %llx-%llx %d/%u/%llx w %d",
		  (unsigned long long)info.number,
		  op_str(info.optype),
		  ex_str(info.optype, info.ex),
		  (unsigned long long)info.start, (unsigned long long)info.end,
		  info.nodeid, info.pid, (unsigned long long)info.owner,
		  info.wait);

	/* report plock rate and any delays since the last report */
	plock_read_count++;
	if (!(plock_read_count % 1000)) {
		usec = dt_usec(&plock_read_time, &now) ;
		log_plock(ls, "plock_read_count %u time %.3f s delays %u",
			  plock_read_count, usec * 1.e-6, plock_rate_delays);
		plock_read_time = now;
		plock_rate_delays = 0;
	}

	create = (info.optype == DLM_PLOCK_OP_UNLOCK) ? 0 : 1;

	rv = find_resource(ls, info.number, create, &r);
	if (rv)
		goto fail;

	if (r->owner == 0) {
		/* plock state replicated on all nodes */
		send_plock(ls, r, &info);

	} else if (r->owner == our_nodeid) {
		/* we are the owner of r, so our plocks are local */
		__receive_plock(ls, &info, our_nodeid, r);

	} else {
		/* r owner is -1: r is new, try to become the owner;
		   r owner > 0: tell other owner to give up ownership;
		   both done with a message trying to set owner to ourself */
		send_own(ls, r, our_nodeid);
		save_pending_plock(ls, r, &info);
	}

	if (cfgd_plock_ownership && !list_empty(&ls->plock_resources))
		poll_drop_plock = 1;
	return;

 fail:
#ifdef DLM_PLOCK_BUILD_WORKAROUND
	if (!(info.pad & DLM_PLOCK_FL_CLOSE)) {
#else
	if (!(info.flags & DLM_PLOCK_FL_CLOSE)) {
#endif
		info.rv = rv;
		rv = write(plock_device_fd, &info, sizeof(info));
	}
}

void process_saved_plocks(struct lockspace *ls)
{
	struct save_msg *sm, *sm2;
	struct dlm_header *hd;
	int count = 0;

	log_dlock(ls, "process_saved_plocks begin");

	if (list_empty(&ls->saved_messages))
		goto out;

	list_for_each_entry_safe(sm, sm2, &ls->saved_messages, list) {
		hd = (struct dlm_header *)sm->buf;

		switch (sm->type) {
		case DLM_MSG_PLOCK:
			_receive_plock(ls, hd, sm->len);
			break;
		case DLM_MSG_PLOCK_OWN:
			_receive_own(ls, hd, sm->len);
			break;
		case DLM_MSG_PLOCK_DROP:
			_receive_drop(ls, hd, sm->len);
			break;
		case DLM_MSG_PLOCK_SYNC_LOCK:
		case DLM_MSG_PLOCK_SYNC_WAITER:
			_receive_sync(ls, hd, sm->len);
			break;
		default:
			continue;
		}

		list_del(&sm->list);
		free(sm);
		count++;
	}
 out:
	log_dlock(ls, "process_saved_plocks %d done", count);
}

/* locks still marked SYNCING should not go into the ckpt; the new node
   will get those locks by receiving PLOCK_SYNC messages */

#define MAX_SEND_SIZE 1024 /* 1024 holds 24 plock_data */

static char send_buf[MAX_SEND_SIZE];

static int pack_send_buf(struct lockspace *ls, struct resource *r, int owner,
			 int full, int *count_out, void **last)
{
	struct resource_data *rd;
	struct plock_data *pp;
	struct posix_lock *po;
	struct lock_waiter *w;
	int count = 0;
	int find = 0;
	int len;

	/* N.B. owner not always equal to r->owner */
	rd = (struct resource_data *)(send_buf + sizeof(struct dlm_header));
	rd->number = cpu_to_le64(r->number);
	rd->owner = cpu_to_le32(owner);

	if (full) {
		rd->flags = RD_CONTINUE;
		find = 1;
	}

	/* plocks not replicated for owned resources */
	if (cfgd_plock_ownership && (owner == our_nodeid))
		goto done;

	len = sizeof(struct dlm_header) + sizeof(struct resource_data);

	pp = (struct plock_data *)(send_buf + sizeof(struct dlm_header) + sizeof(struct resource_data));

	list_for_each_entry(po, &r->locks, list) {
		if (find && *last != po)
			continue;
		find = 0;

		if (po->flags & P_SYNCING)
			continue;

		if (len + sizeof(struct plock_data) > sizeof(send_buf)) {
			*last = po;
			goto full;
		}
		len += sizeof(struct plock_data);

		pp->start	= cpu_to_le64(po->start);
		pp->end		= cpu_to_le64(po->end);
		pp->owner	= cpu_to_le64(po->owner);
		pp->pid		= cpu_to_le32(po->pid);
		pp->nodeid	= cpu_to_le32(po->nodeid);
		pp->ex		= po->ex;
		pp->waiter	= 0;
		pp++;
		count++;
	}

	list_for_each_entry(w, &r->waiters, list) {
		if (find && *last != w)
			continue;
		find = 0;

		if (w->flags & P_SYNCING)
			continue;

		if (len + sizeof(struct plock_data) > sizeof(send_buf)) {
			*last = w;
			goto full;
		}
		len += sizeof(struct plock_data);

		pp->start	= cpu_to_le64(w->info.start);
		pp->end		= cpu_to_le64(w->info.end);
		pp->owner	= cpu_to_le64(w->info.owner);
		pp->pid		= cpu_to_le32(w->info.pid);
		pp->nodeid	= cpu_to_le32(w->info.nodeid);
		pp->ex		= w->info.ex;
		pp->waiter	= 1;
		pp++;
		count++;
	}
 done:
	rd->lock_count = cpu_to_le32(count);
	*count_out = count;
	*last = NULL;
	return 0;

 full:
	rd->lock_count = cpu_to_le32(count);
	*count_out = count;
	return 1;
}

/* Copy all plock state into a checkpoint so new node can retrieve it.  The
   node creating the ckpt for the mounter needs to be the same node that's
   sending the mounter its journals message (i.e. the low nodeid).  The new
   mounter knows the ckpt is ready to read only after it gets its journals
   message.
 
   If the mounter is becoming the new low nodeid in the group, the node doing
   the store closes the ckpt and the new node unlinks the ckpt after reading
   it.  The ckpt should then disappear and the new node can create a new ckpt
   for the next mounter. */

static int send_plocks_data(struct lockspace *ls, uint32_t seq, char *buf, int len)
{
	struct dlm_header *hd;

	hd = (struct dlm_header *)buf;
	hd->type = DLM_MSG_PLOCKS_DATA;
	hd->msgdata = seq;

	dlm_send_message(ls, buf, len);

	return 0;
}

void send_all_plocks_data(struct lockspace *ls, uint32_t seq, uint32_t *plocks_data)
{
	struct resource *r;
	void *last;
	int owner, count, len, full;
	uint32_t send_count = 0;

	if (!cfgd_enable_plock || ls->disable_plock)
		return;

	log_dlock(ls, "send_all_plocks_data %d:%u", our_nodeid, seq);

	/* - If r owner is -1, ckpt nothing.
	   - If r owner is us, ckpt owner of us and no plocks.
	   - If r owner is other, ckpt that owner and any plocks we have on r
	     (they've just been synced but owner=0 msg not recved yet).
	   - If r owner is 0 and !got_unown, then we've just unowned r;
	     ckpt owner of us and any plocks that don't have SYNCING set
	     (plocks with SYNCING will be handled by our sync messages).
	   - If r owner is 0 and got_unown, then ckpt owner 0 and all plocks;
	     (there should be no SYNCING plocks) */

	list_for_each_entry(r, &ls->plock_resources, list) {
		if (!cfgd_plock_ownership)
			owner = 0;
		else if (r->owner == -1)
			continue;
		else if (r->owner == our_nodeid)
			owner = our_nodeid;
		else if (r->owner)
			owner = r->owner;
		else if (!r->owner && !got_unown(r))
			owner = our_nodeid;
		else if (!r->owner)
			owner = 0;
		else {
			log_elock(ls, "send_all_plocks_data error owner %d r %llx",
				  r->owner, (unsigned long long)r->number);
			continue;
		}

		memset(&send_buf, 0, sizeof(send_buf));
		count = 0;
		full = 0;
		last = NULL;

		do {
			full = pack_send_buf(ls, r, owner, full, &count, &last);

			len = sizeof(struct dlm_header) +
			      sizeof(struct resource_data) +
			      sizeof(struct plock_data) * count;

			log_plock(ls, "send_plocks_data %d:%u n %llu o %d locks %d len %d",
				  our_nodeid, seq, (unsigned long long)r->number, r->owner,
				  count, len);

			send_plocks_data(ls, seq, send_buf, len);

			send_count++;

		} while (full);
	}

	*plocks_data = send_count;

	log_dlock(ls, "send_all_plocks_data %d:%u %u done",
		  our_nodeid, seq, send_count);
}

static void free_r_lists(struct resource *r)
{
	struct posix_lock *po, *po2;
	struct lock_waiter *w, *w2;

	list_for_each_entry_safe(po, po2, &r->locks, list) {
		list_del(&po->list);
		free(po);
	}

	list_for_each_entry_safe(w, w2, &r->waiters, list) {
		list_del(&w->list);
		free(w);
	}
}

void receive_plocks_data(struct lockspace *ls, struct dlm_header *hd, int len)
{
	struct resource_data *rd;
	struct plock_data *pp;
	struct posix_lock *po;
	struct lock_waiter *w;
	struct resource *r;
	uint64_t num;
	uint32_t count;
	uint32_t flags;
	int owner;
	int i;

	if (!cfgd_enable_plock || ls->disable_plock)
		return;

	if (!ls->need_plocks)
		return;

	if (!ls->save_plocks)
		return;

	ls->recv_plocks_data_count++;

	if (len < sizeof(struct dlm_header) + sizeof(struct resource_data)) {
		log_elock(ls, "recv_plocks_data %d:%u bad len %d",
			  hd->nodeid, hd->msgdata, len);
		return;
	}

	rd = (struct resource_data *)((char *)hd + sizeof(struct dlm_header));
	num = le64_to_cpu(rd->number);
	owner = le32_to_cpu(rd->owner);
	count = le32_to_cpu(rd->lock_count);
	flags = le32_to_cpu(rd->flags);

	if (flags & RD_CONTINUE) {
		r = search_resource(ls, num);
		if (!r) {
			log_elock(ls, "recv_plocks_data %d:%u n %llu not found",
				  hd->nodeid, hd->msgdata, (unsigned long long)num);
			return;
		}
		log_plock(ls, "recv_plocks_data %d:%u n %llu continue",
			  hd->nodeid, hd->msgdata, (unsigned long long)num);
		goto unpack;
	}

	r = malloc(sizeof(struct resource));
	if (!r) {
		log_elock(ls, "recv_plocks_data %d:%u n %llu no mem",
			  hd->nodeid, hd->msgdata, (unsigned long long)num);
		return;
	}
	memset(r, 0, sizeof(struct resource));
	INIT_LIST_HEAD(&r->locks);
	INIT_LIST_HEAD(&r->waiters);
	INIT_LIST_HEAD(&r->pending);

	if (!cfgd_plock_ownership) {
		if (owner) {
			log_elock(ls, "recv_plocks_data %d:%u n %llu bad owner %d",
				  hd->nodeid, hd->msgdata, (unsigned long long)num,
				  owner);
			goto fail_free;
		}
	} else {
		if (!owner)
			r->flags |= R_GOT_UNOWN;

		/* no locks should be included for owned resources */

		if (owner && count) {
			log_elock(ls, "recv_plocks_data %d:%u n %llu o %d bad count %u",
				  (unsigned long long)num, owner, count);
			goto fail_free;
		}
	}

	r->number = num;
	r->owner = owner;

 unpack:
	if (len < sizeof(struct dlm_header) +
		  sizeof(struct resource_data) +
		  sizeof(struct plock_data) * count) {
		log_elock(ls, "recv_plocks_data %d:%u count %u bad len %d",
			  hd->nodeid, hd->msgdata, count, len);
		goto fail_free;
	}

	pp = (struct plock_data *)((char *)rd + sizeof(struct resource_data));

	for (i = 0; i < count; i++) {
		if (!pp->waiter) {
			po = malloc(sizeof(struct posix_lock));
			if (!po)
				goto fail_free;
			po->start	= le64_to_cpu(pp->start);
			po->end		= le64_to_cpu(pp->end);
			po->owner	= le64_to_cpu(pp->owner);
			po->pid		= le32_to_cpu(pp->pid);
			po->nodeid	= le32_to_cpu(pp->nodeid);
			po->ex		= pp->ex;
			list_add_tail(&po->list, &r->locks);
		} else {
			w = malloc(sizeof(struct lock_waiter));
			if (!w)
				goto fail_free;
			w->info.start	= le64_to_cpu(pp->start);
			w->info.end	= le64_to_cpu(pp->end);
			w->info.owner	= le64_to_cpu(pp->owner);
			w->info.pid	= le32_to_cpu(pp->pid);
			w->info.nodeid	= le32_to_cpu(pp->nodeid);
			w->info.ex	= pp->ex;
			list_add_tail(&w->list, &r->waiters);
		}
		pp++;
	}

	log_plock(ls, "recv_plocks_data %d:%u n %llu o %d locks %d len %d",
		  hd->nodeid, hd->msgdata, (unsigned long long)r->number,
		  r->owner, count, len);

	if (!(flags & RD_CONTINUE)) {
		list_add_tail(&r->list, &ls->plock_resources);
		rb_insert_plock_resource(ls, r);
	}
	return;

 fail_free:
	if (!(flags & RD_CONTINUE)) {
		free_r_lists(r);
		free(r);
	}
	return;
}

void clear_plocks_data(struct lockspace *ls)
{
	struct resource *r, *r2;
	uint32_t count = 0;

	if (!cfgd_enable_plock || ls->disable_plock)
		return;

	list_for_each_entry_safe(r, r2, &ls->plock_resources, list) {
		free_r_lists(r);
		rb_del_plock_resource(ls, r);
		list_del(&r->list);
		free(r);
		count++;
	}

	log_dlock(ls, "clear_plocks_data done %u recv_plocks_data_count %u",
		  count, ls->recv_plocks_data_count);

	ls->recv_plocks_data_count = 0;
}

/* Called when a node has failed, or we're unmounting.  For a node failure, we
   need to call this when the cpg confchg arrives so that we're guaranteed all
   nodes do this in the same sequence wrt other messages. */

void purge_plocks(struct lockspace *ls, int nodeid, int unmount)
{
	struct posix_lock *po, *po2;
	struct lock_waiter *w, *w2;
	struct resource *r, *r2;
	int purged = 0;

	if (!cfgd_enable_plock || ls->disable_plock)
		return;

	list_for_each_entry_safe(r, r2, &ls->plock_resources, list) {
		list_for_each_entry_safe(po, po2, &r->locks, list) {
			if (po->nodeid == nodeid || unmount) {
				list_del(&po->list);
				free(po);
				purged++;
			}
		}

		list_for_each_entry_safe(w, w2, &r->waiters, list) {
			if (w->info.nodeid == nodeid || unmount) {
				list_del(&w->list);
				free(w);
				purged++;
			}
		}

		/* TODO: haven't thought carefully about how this transition
		   to owner 0 might interact with other owner messages in
		   progress. */

		if (r->owner == nodeid) {
			r->owner = 0;
			r->flags |= R_GOT_UNOWN;
			r->flags |= R_PURGE_UNOWN;
			send_pending_plocks(ls, r);
		}
		
		if (!list_empty(&r->waiters))
			do_waiters(ls, r);

		if (!cfgd_plock_ownership &&
		    list_empty(&r->locks) && list_empty(&r->waiters)) {
			rb_del_plock_resource(ls, r);
			list_del(&r->list);
			free(r);
		}
	}
	
	if (purged)
		ls->last_plock_time = time(NULL);

	log_dlock(ls, "purged %d plocks for %d", purged, nodeid);
}

int copy_plock_state(struct lockspace *ls, char *buf, int *len_out)
{
	struct posix_lock *po;
	struct lock_waiter *w;
	struct resource *r;
	struct timeval now;
	int rv = 0;
	int len = DLMC_DUMP_SIZE, pos = 0, ret;

	gettimeofday(&now, NULL);

	list_for_each_entry(r, &ls->plock_resources, list) {

		if (list_empty(&r->locks) &&
		    list_empty(&r->waiters) &&
		    list_empty(&r->pending)) {
			ret = snprintf(buf + pos, len - pos,
			      "%llu rown %d unused_ms %llu\n",
			      (unsigned long long)r->number, r->owner,
			      (unsigned long long)time_diff_ms(&r->last_access,
				      			       &now));
			if (ret >= len - pos) {
				rv = -ENOSPC;
				goto out;
			}
			pos += ret;
			continue;
		}

		list_for_each_entry(po, &r->locks, list) {
			ret = snprintf(buf + pos, len - pos,
			      "%llu %s %llu-%llu nodeid %d pid %u owner %llx rown %d\n",
			      (unsigned long long)r->number,
			      po->ex ? "WR" : "RD",
			      (unsigned long long)po->start,
			      (unsigned long long)po->end,
			      po->nodeid, po->pid,
			      (unsigned long long)po->owner, r->owner);

			if (ret >= len - pos) {
				rv = -ENOSPC;
				goto out;
			}
			pos += ret;
		}

		list_for_each_entry(w, &r->waiters, list) {
			ret = snprintf(buf + pos, len - pos,
			      "%llu %s %llu-%llu nodeid %d pid %u owner %llx rown %d WAITING\n",
			      (unsigned long long)r->number,
			      w->info.ex ? "WR" : "RD",
			      (unsigned long long)w->info.start,
			      (unsigned long long)w->info.end,
			      w->info.nodeid, w->info.pid,
			      (unsigned long long)w->info.owner, r->owner);

			if (ret >= len - pos) {
				rv = -ENOSPC;
				goto out;
			}
			pos += ret;
		}

		list_for_each_entry(w, &r->pending, list) {
			ret = snprintf(buf + pos, len - pos,
			      "%llu %s %llu-%llu nodeid %d pid %u owner %llx rown %d PENDING\n",
			      (unsigned long long)r->number,
			      w->info.ex ? "WR" : "RD",
			      (unsigned long long)w->info.start,
			      (unsigned long long)w->info.end,
			      w->info.nodeid, w->info.pid,
			      (unsigned long long)w->info.owner, r->owner);

			if (ret >= len - pos) {
				rv = -ENOSPC;
				goto out;
			}
			pos += ret;
		}
	}
 out:
	*len_out = pos;
	return rv;
}

