#include <syslog.h>

#include "config.h"
#include "dlm_daemon.h"

#include <glib.h>
#include <bzlib.h>
#include <heartbeat/ha_msg.h>

#include <pacemaker/crm_config.h>

#include <pacemaker/crm/crm.h>
#include <pacemaker/crm/ais.h>
#include <pacemaker/crm/attrd.h>
/* heartbeat support is irrelevant here */
#undef SUPPORT_HEARTBEAT 
#define SUPPORT_HEARTBEAT 0
#include <pacemaker/crm/common/cluster.h>
#include <pacemaker/crm/common/stack.h>
#include <pacemaker/crm/common/ipc.h>
#include <pacemaker/crm/msg_xml.h>
#include <pacemaker/crm/cib.h>

#define COMMS_DIR     "/sys/kernel/config/dlm/cluster/comms"

int setup_ccs(void)
{
    /* To avoid creating an additional place for the dlm to be configured,
     * only allow configuration from the command-line until CoroSync is stable
     * enough to be used with Pacemaker
     */
    return 0;
}

void close_ccs(void) { return; }
int get_weight(int nodeid, char *lockspace) { return 1; }

/* TODO: Make this configurable
 * Can't use logging.c as-is as whitetank exposes a different logging API
 */
void init_logging(void) {
    openlog("cluster-dlm", LOG_PERROR|LOG_PID|LOG_CONS|LOG_NDELAY, LOG_DAEMON);
    /* cl_log_enable_stderr(TRUE); */
}

void setup_logging(void) { return; }
void close_logging(void) {
    closelog();
}

int setup_cluster_cfg(void) { return 0; }
void process_cluster_cfg(int ci) {}
void close_cluster_cfg(void) {}

extern int ais_fd_async;

int local_node_id = 0;
char *local_node_uname = NULL;
void dlm_process_node(gpointer key, gpointer value, gpointer user_data);

int setup_cluster(void)
{
    ais_fd_async = -1;
    crm_log_init("cluster-dlm", LOG_INFO, FALSE, TRUE, 0, NULL);
    
    if(init_ais_connection(NULL, NULL, NULL, &local_node_uname, &our_nodeid) == FALSE) {
	log_error("Connection to our AIS plugin (%d) failed", CRM_SERVICE);
	return -1;
    }

    /* Sign up for membership updates */
    send_ais_text(crm_class_notify, "true", TRUE, NULL, crm_msg_ais);
    
    /* Requesting the current list of known nodes */
    send_ais_text(crm_class_members, __FUNCTION__, TRUE, NULL, crm_msg_ais);

    return ais_fd_async;
}

void update_cluster(void)
{
    static uint64_t last_membership = 0;
    cluster_quorate = crm_have_quorum;
    if(last_membership < crm_peer_seq) {
	log_debug("Processing membership %llu", crm_peer_seq);
	g_hash_table_foreach(crm_peer_cache, dlm_process_node, &last_membership);
	last_membership = crm_peer_seq;
    }
}

void process_cluster(int ci)
{
    ais_dispatch(ais_fd_async, NULL);
    update_cluster();
}

void close_cluster(void) {
    terminate_ais_connection();
}

#include <arpa/inet.h>
#include <corosync/totem/totemip.h>

void dlm_process_node(gpointer key, gpointer value, gpointer user_data)
{
    int rc = 0;
    struct stat tmp;
    char path[PATH_MAX];
    crm_node_t *node = value;
    uint64_t *last = user_data;
    const char *action = "Skipped";

    gboolean do_add = FALSE;
    gboolean do_remove = FALSE;
    gboolean is_active = FALSE;

    memset(path, 0, PATH_MAX);
    snprintf(path, PATH_MAX, "%s/%d", COMMS_DIR, node->id);

    rc = stat(path, &tmp);
    is_active = crm_is_member_active(node);
    
    if(rc == 0 && is_active) {
	/* nothing to do?
	 * maybe the node left and came back...
	 */
    } else if(rc == 0) {
	do_remove = TRUE;

    } else if(is_active) {
	do_add = TRUE;
    }

    if(do_remove) {
	action = "Removed";
	del_configfs_node(node->id);
    }

    if(do_add) {
	char *addr_copy = strdup(node->addr);
	char *addr_top = addr_copy;
	char *addr = NULL;
	
	if(do_remove) {
	    action = "Re-added";
	} else {
	    action = "Added";
	}
	
	if(local_node_id == 0) {
	    crm_node_t *local_node = g_hash_table_lookup(
		crm_peer_cache, local_node_uname);
	    local_node_id = local_node->id;
	}
	
	do {
	    char ipaddr[1024];
	    int addr_family = AF_INET;
	    int cna_len = 0, rc = 0;
	    struct sockaddr_storage cna_addr;
	    struct totem_ip_address totem_addr;
	    
	    addr = strsep(&addr_copy, " ");
	    if(addr == NULL) {
		break;
	    }
	    
	    /* do_cmd_get_node_addrs */
	    if(strstr(addr, "ip(") == NULL) {
		continue;
		
	    } else if(strchr(addr, ':')) {
		rc = sscanf(addr, "ip(%[0-9A-Fa-f:])", ipaddr);
		if(rc != 1) {
		    log_error("Could not extract IPv6 address from '%s'", addr);
		    continue;			
		}
		addr_family = AF_INET6;
		    
	    } else {
		rc = sscanf(addr, "ip(%[0-9.]) ", ipaddr);
		if(rc != 1) {
		    log_error("Could not extract IPv4 address from '%s'", addr);
		    continue;			
		}
	    }
		
	    rc = inet_pton(addr_family, ipaddr, &totem_addr);
	    if(rc != 1) {
		log_error("Could not parse '%s' as in IPv%c address", ipaddr, (addr_family==AF_INET)?'4':'6');
		continue;
	    }

	    rc = totemip_parse(&totem_addr, ipaddr, addr_family);
	    if(rc != 0) {
		log_error("Could not convert '%s' into a totem address", ipaddr);
		continue;
	    }

	    rc = totemip_totemip_to_sockaddr_convert(&totem_addr, 0, &cna_addr, &cna_len);
	    if(rc != 0) {
		log_error("Could not convert totem address for '%s' into sockaddr", ipaddr);
		continue;
	    }

	    log_debug("Adding address %s to configfs for node %u/%s ", addr, node->id, node->uname);
	    add_configfs_node(node->id, ((char*)&cna_addr), cna_len, (node->id == local_node_id));

	} while(addr != NULL);
	free(addr_top);
    }

    log_debug("%s %sctive node %u '%s': born-on=%llu, last-seen=%llu, this-event=%llu, last-event=%llu",
	       action, crm_is_member_active(value)?"a":"ina",
	       node->id, node->uname, node->born, node->last_seen,
	       crm_peer_seq, (unsigned long long)*last);
}

int is_cluster_member(uint32_t nodeid)
{
    crm_node_t *node = crm_get_peer(nodeid, NULL);
    return crm_is_member_active(node);
}

char *nodeid2name(int nodeid) {
    crm_node_t *node = crm_get_peer(nodeid, NULL);
    if(node->uname == NULL) {
	return NULL;
    }
    return strdup(node->uname);
}

void kick_node_from_cluster(int nodeid)
{
    int rc = crm_terminate_member_no_mainloop(nodeid, NULL, NULL);   
    switch(rc) {
	case 0:
	    log_debug("Requested that node %d be kicked from the cluster", nodeid);
	    break;
	case -1:
	    log_error("Don't know how to kick node %d from the cluster", nodeid);
	    break;
	case 1:
	    log_error("Could not kick node %d from the cluster", nodeid);
	    break;
	default:
	    log_error("Unknown result when kicking node %d from the cluster", nodeid);
	    break;
    }
    return;
}

int fence_in_progress(int *in_progress)
{
    *in_progress = 0;
    return 0;
}

int fence_node_time(int nodeid, uint64_t *last_fenced_time)
{
    int rc = 0;
    const char *uname = NULL;
    crm_node_t *node = crm_get_peer(nodeid, uname);
    stonith_history_t *history, *hp = NULL;

    if(last_fenced_time) {
	*last_fenced_time = 0;
    }

    if (node && node->uname) {
        uname = node->uname;
        st = stonith_api_new();

    } else {
        crm_err("Nothing known about node id=%d", nodeid);
        return 0;
    }
    
    if(st) {
	rc = st->cmds->connect(st, crm_system_name, NULL);
    }

    if(rc == stonith_ok) {
        st->cmds->history(st, st_opt_sync_call, uname, &history, 120);
        for(hp = history; hp; hp = hp->next) {
            if(hp->state == st_done) {
                *last_fenced_time = hp->completed;
            }
        }
    }
    
    if(*last_fenced_time != 0) {
        log_debug("Node %d/%s was last shot at: %s", nodeid, ctime(*last_fenced_time));	
    } else {
        log_debug("It does not appear node %d/%s has been shot", nodeid, uname);
    }
    
    if(st) {
        st->cmds->disconnect(st);
        stonith_api_delete(st);
    }
        
    return 0;
}
