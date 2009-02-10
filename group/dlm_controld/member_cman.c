#include "dlm_daemon.h"
#include "config.h"
#include <corosync/corotypes.h>
#include <corosync/cfg.h>
#include <corosync/votequorum.h>
#include "libfenced.h"

static corosync_cfg_handle_t	ch;
static votequorum_handle_t	qh;
static votequorum_node_t	old_nodes[MAX_NODES];
static int			old_node_count;
static votequorum_node_t	quorum_nodes[MAX_NODES];
static int			quorum_node_count;

void kick_node_from_cluster(int nodeid)
{
	if (!nodeid) {
		log_error("telling corosync to shut down cluster locally");
		corosync_cfg_try_shutdown(ch,
				COROSYNC_CFG_SHUTDOWN_FLAG_IMMEDIATE);
	} else {
		log_error("telling corosync to remove nodeid %d from cluster",
			  nodeid);
		corosync_cfg_kill_node(ch, nodeid, "dlm_controld");
	}
}

static int is_member(votequorum_node_t *node_list, int count, int nodeid)
{
	int i;

	for (i = 0; i < count; i++) {
		if (node_list[i].nodeid == nodeid)
			return (node_list[i].state == NODESTATE_MEMBER);
	}
	return 0;
}

static int is_old_member(int nodeid)
{
	return is_member(old_nodes, old_node_count, nodeid);
}

int is_cluster_member(int nodeid)
{
	return is_member(quorum_nodes, quorum_node_count, nodeid);
}

/* what's the replacement for this? */
#if 0
static void cman_callback(cman_handle_t h, void *private, int reason, int arg)
{
	case CMAN_REASON_CONFIG_UPDATE:
		setup_logging();
		setup_ccs();
		break;
}
#endif

/* add a configfs dir for cluster members that don't have one,
   del the configfs dir for cluster members that are now gone */

static void quorum_callback(votequorum_handle_t h, uint64_t context,
			    uint32_t quorate, uint32_t node_list_entries,
			    votequorum_node_t node_list[])
{
	corosync_cfg_node_address_t addrs[MAX_NODE_ADDRESSES];
	corosync_cfg_node_address_t *addrptr = addrs;
	cs_error_t err;
	int i, j, num_addrs;

	cluster_quorate = quorate;

	old_node_count = quorum_node_count;
	memcpy(&old_nodes, &quorum_nodes, sizeof(old_nodes));

	quorum_node_count = 0;
	memset(&quorum_nodes, 0, sizeof(quorum_nodes));

	for (i = 0; i < node_list_entries; i++) {
		if (node_list[i].state == NODESTATE_MEMBER) {
			memcpy(&quorum_nodes[quorum_node_count],
			       &node_list[i], sizeof(votequorum_node_t));
			quorum_node_count++;
		}
	}

	for (i = 0; i < old_node_count; i++) {
		if ((old_nodes[i].state == NODESTATE_MEMBER) &&
		    !is_cluster_member(old_nodes[i].nodeid)) {

			log_debug("quorum: node %d removed",
				   old_nodes[i].nodeid);

			del_configfs_node(old_nodes[i].nodeid);
		}
	}

	for (i = 0; i < quorum_node_count; i++) {
		if ((quorum_nodes[i].state == NODESTATE_MEMBER) &&
		    !is_old_member(quorum_nodes[i].nodeid)) {

			log_debug("quorum: node %d added",
				  quorum_nodes[i].nodeid);

			err = corosync_cfg_get_node_addrs(ch,
					quorum_nodes[i].nodeid,
					MAX_NODE_ADDRESSES,
					&num_addrs, addrs);
			if (err != CS_OK) {
				log_error("corosync_cfg_get_node_addrs failed "
					  "nodeid %d", quorum_nodes[i].nodeid);
				continue;
			}

			for (j = 0; j < num_addrs; j++) {
				add_configfs_node(quorum_nodes[i].nodeid,
						  addrptr[j].address,
						  addrptr[j].address_length,
						  (quorum_nodes[i].nodeid ==
						   our_nodeid));
			}
		}
	}
}

static votequorum_callbacks_t quorum_callbacks =
{
	.votequorum_notify_fn = quorum_callback,
};

void process_cluster(int ci)
{
	cs_error_t err;

	err = votequorum_dispatch(qh, CS_DISPATCH_ALL);
	if (err != CS_OK)
		cluster_dead(0);
}

/* Force re-read of quorum nodes */
void update_cluster(void)
{
	cs_error_t err;

	err = votequorum_dispatch(qh, CS_DISPATCH_ONE);
	if (err != CS_OK)
		cluster_dead(0);
}

int setup_cluster(void)
{
	struct votequorum_info qinfo;
	cs_error_t err;
	int fd;

	err = votequorum_initialize(&qh, &quorum_callbacks);
	if (err != CS_OK)
		return -1;

	err = votequorum_fd_get(qh, &fd);
	if (err != CS_OK)
		goto fail;

	err = votequorum_getinfo(qh, 0, &qinfo);
	if (err != CS_OK)
		goto fail;
	our_nodeid = qinfo.node_id;

	err = votequorum_trackstart(qh, 0, CS_TRACK_CURRENT);
	if (err != CS_OK)
		goto fail;

	old_node_count = 0;
	memset(&old_nodes, 0, sizeof(old_nodes));
	quorum_node_count = 0;
	memset(&quorum_nodes, 0, sizeof(quorum_nodes));

	return fd;
 fail:
	votequorum_finalize(qh);
	return -1;
}

void close_cluster(void)
{
	votequorum_trackstop(qh);
	votequorum_finalize(qh);
}

static void shutdown_callback(corosync_cfg_handle_t h,
			      corosync_cfg_shutdown_flags_t flags)
{
	if (flags & COROSYNC_CFG_SHUTDOWN_FLAG_REQUEST) {
		if (list_empty(&lockspaces))
			corosync_cfg_replyto_shutdown(ch,
					COROSYNC_CFG_SHUTDOWN_FLAG_YES);
		else {
			log_debug("no to corosync shutdown");
			corosync_cfg_replyto_shutdown(ch,
					COROSYNC_CFG_SHUTDOWN_FLAG_NO);
		}
	}
}

static corosync_cfg_callbacks_t cfg_callbacks =
{
	.corosync_cfg_shutdown_callback = shutdown_callback,
	.corosync_cfg_state_track_callback = NULL,
};

void process_cluster_cfg(int ci)
{
	cs_error_t err;

	err = corosync_cfg_dispatch(ch, CS_DISPATCH_ALL);
	if (err != CS_OK)
		cluster_dead(0);
}

int setup_cluster_cfg(void)
{
	cs_error_t err;
	int fd;

	err = corosync_cfg_initialize(&ch, &cfg_callbacks);
	if (err != CS_OK)
		return -1;

	err = corosync_cfg_fd_get(ch, &fd);
	if (err != CS_OK) {
		corosync_cfg_finalize(ch);
		return -1;
	}

	return fd;
}

void close_cluster_cfg(void)
{
	corosync_cfg_finalize(ch);
}

int fence_node_time(int nodeid, uint64_t *last_fenced_time)
{
	struct fenced_node nodeinfo;
	int rv;

	memset(&nodeinfo, 0, sizeof(nodeinfo));

	rv = fenced_node_info(nodeid, &nodeinfo);
	if (rv < 0)
		return rv;

	*last_fenced_time = nodeinfo.last_fenced_time;
	return 0;
}

int fence_in_progress(int *count)
{
	struct fenced_domain domain;
	int rv;

	memset(&domain, 0, sizeof(domain));

	rv = fenced_domain_info(&domain);
	if (rv < 0)
		return rv;

	*count = domain.victim_count;
	return 0;
}

