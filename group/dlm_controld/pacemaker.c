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

extern int ais_fd_async;

int local_node_id = 0;
char *local_node_uname = NULL;
void dlm_process_node(gpointer key, gpointer value, gpointer user_data);

int setup_cluster(void)
{
    ais_fd_async = -1;
    crm_log_init("cluster-dlm", LOG_INFO, FALSE, TRUE, 0, NULL);
    
    if(init_ais_connection(NULL, NULL, NULL, &local_node_uname, &our_nodeid) == FALSE) {
	log_printf(LOG_ERR, "Connection to our AIS plugin (%d) failed", CRM_SERVICE);
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
	log_printf(LOG_INFO, "Processing membership %llu", crm_peer_seq);
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
		    log_printf(LOG_ERR, "Could not extract IPv6 address from '%s'", addr);
		    continue;			
		}
		addr_family = AF_INET6;
		    
	    } else {
		rc = sscanf(addr, "ip(%[0-9.]) ", ipaddr);
		if(rc != 1) {
		    log_printf(LOG_ERR, "Could not extract IPv4 address from '%s'", addr);
		    continue;			
		}
	    }
		
	    rc = inet_pton(addr_family, ipaddr, &totem_addr);
	    if(rc != 1) {
		log_printf(LOG_ERR, "Could not parse '%s' as in IPv%c address", ipaddr, (addr_family==AF_INET)?'4':'6');
		continue;
	    }

	    rc = totemip_parse(&totem_addr, ipaddr, addr_family);
	    if(rc != 0) {
		log_printf(LOG_ERR, "Could not convert '%s' into a totem address", ipaddr);
		continue;
	    }

	    rc = totemip_totemip_to_sockaddr_convert(&totem_addr, 0, &cna_addr, &cna_len);
	    if(rc != 0) {
		log_printf(LOG_ERR, "Could not convert totem address for '%s' into sockaddr", ipaddr);
		continue;
	    }

	    log_debug("Adding address %s to configfs for node %u/%s ", addr, node->id, node->uname);
	    add_configfs_node(node->id, ((char*)&cna_addr), cna_len, (node->id == local_node_id));

	} while(addr != NULL);
	free(addr_top);
    }

    log_printf(LOG_INFO, "%s %sctive node %u '%s': born-on=%llu, last-seen=%llu, this-event=%llu, last-event=%llu",
	       action, crm_is_member_active(value)?"a":"ina",
	       node->id, node->uname, node->born, node->last_seen,
	       crm_peer_seq, (unsigned long long)*last);
}

int is_cluster_member(int nodeid)
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

static IPC_Channel *attrd = NULL;

static void attrd_deadfn(int ci) 
{
    log_printf(LOG_ERR, "Lost connection to attrd");
    attrd = NULL;
    return;
}

void kick_node_from_cluster(int nodeid)
{
    gboolean rc = FALSE;
    xmlNode *update = NULL;
    time_t now = time(NULL);
    crm_node_t *node = crm_get_peer(nodeid, NULL);

    if(node == NULL || node->uname == NULL) {
	log_printf(LOG_ERR, "%s: Don't know how to kick node %d/%p", __FUNCTION__, nodeid, node);
	return;
    }

    if(attrd == NULL) {
	log_printf(LOG_INFO, "Connecting to attrd...");
	attrd = init_client_ipc_comms_nodispatch(T_ATTRD);
	if(attrd) {
	    client_add(attrd->ops->get_recv_select_fd(attrd), NULL, attrd_deadfn);
	}
    }
    
    if(attrd != NULL) {
	update = create_xml_node(NULL, __FUNCTION__);
	crm_xml_add(update, F_TYPE, T_ATTRD);
	crm_xml_add(update, F_ORIG, crm_system_name);
    
	crm_xml_add(update, F_ATTRD_TASK, "update");
	crm_xml_add(update, F_ATTRD_SECTION, XML_CIB_TAG_STATUS);
	crm_xml_add(update, F_ATTRD_ATTRIBUTE, "terminate");
	crm_xml_add_int(update, F_ATTRD_VALUE, now);
	crm_xml_add(update, F_ATTRD_HOST, node->uname);
	
	rc = send_ipc_message(attrd, update);
	free_xml(update);
    }

    if(rc) {
	log_printf(LOG_INFO, "Requested that node %d/%s be kicked from the cluster", nodeid, node->uname);
    } else {
	log_printf(LOG_ERR, "Could not kick node %d/%s from the cluster", nodeid, node->uname);
    }
    
    return;
}

cib_t *cib = NULL;

static void cib_deadfn(int ci) 
{
    log_printf(LOG_ERR, "Lost connection to the cib");
    cib = NULL; /* TODO: memory leak in unlikely error path */
    return;
}

static cib_t *cib_connect(void) 
{
    int rc = 0;
    int cib_fd = 0;
    if(cib) {
	return cib;
    }
    
    cib = cib_new();
    rc = cib->cmds->signon_raw(cib, crm_system_name, cib_command, &cib_fd, NULL);
    if(rc != cib_ok) {
	log_printf(LOG_ERR, "Signon to cib failed: %s", cib_error2string(rc));
	cib = NULL; /* TODO: memory leak in unlikely error path */

    } else {
	client_add(cib_fd, NULL, cib_deadfn);
    }
    return cib;
}


int fence_in_progress(int *unused)
{
    int rc = 0;
    xmlNode *xpath_data;

    cib_connect();    
    if(cib == NULL) {
	return -1;
    }

    /* TODO: Not definitive - but a good approximation */
    rc = cib->cmds->query(cib, "//nvpar[@name='terminate']", &xpath_data,
			  cib_xpath|cib_scope_local|cib_sync_call);

    log_printf(LOG_INFO, "Fencing in progress: %s", xpath_data?"true":"false");	

    if(xpath_data == NULL) {
	return 0;
    }

    free_xml(xpath_data);
    return 1;
}

#define XPATH_MAX  1024

int fence_node_time(int nodeid, uint64_t *last_fenced_time)
{
    int rc = 0;
    xmlNode *xpath_data;
    char xpath_query[XPATH_MAX];
    crm_node_t *node = crm_get_peer(nodeid, NULL);

    if(last_fenced_time) {
	*last_fenced_time = 0;
    }

    if(node == NULL || node->uname == NULL) {
	log_printf(LOG_ERR, "Nothing known about node %d", nodeid);	
	return 0;
    }

    cib_connect();
    if(cib == NULL) {
	return -1;
    }

    snprintf(xpath_query, XPATH_MAX, "//lrm[@id='%s']", node->uname);
    rc = cib->cmds->query(
	cib, xpath_query, &xpath_data, cib_xpath|cib_scope_local|cib_sync_call);

    if(xpath_data == NULL) {
	/* the node has been shot - return 'now' */
	log_printf(LOG_INFO, "Node %d/%s was last shot 'now'", nodeid, node->uname);	
	*last_fenced_time = time(NULL);
    }

    free_xml(xpath_data);
    log_printf(LOG_INFO, "It does not appear node %d/%s has been shot", nodeid, node->uname);	
    return 0;
}
