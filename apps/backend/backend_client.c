/*
 *
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren

  This file is part of CLIXON.

  CLIXON is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 3 of the License, or
  (at your option) any later version.

  CLIXON is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with CLIXON; see the file LICENSE.  If not, see
  <http://www.gnu.org/licenses/>.

 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "backend_commit.h"
#include "backend_plugin.h"
#include "backend_client.h"
#include "backend_handle.h"

/*! Add client notification subscription. Ie send notify to this client when event occurs
 * @param[in] ce      Client entry struct
 * @param[in] stream  Notification stream name
 * @param[in] format  How to display event (see enum format_enum)
 * @param[in] filter  Filter, what to display, eg xpath for format=xml, fnmatch
 *
 * @see backend_notify - where subscription is made and notify call is made
 */
static struct client_subscription *
client_subscription_add(struct client_entry *ce, 
			char                *stream, 
			enum format_enum     format,
			char                *filter)
{
    struct client_subscription *su = NULL;

    clicon_debug(1, "%s stream:%s filter:%s", __FUNCTION__, stream, filter);
    if ((su = malloc(sizeof(*su))) == NULL){
	clicon_err(OE_PLUGIN, errno, "malloc");
	goto done;
    }
    memset(su, 0, sizeof(*su));
    su->su_stream = strdup(stream);
    su->su_format = format;
    su->su_filter = strdup(filter);
    su->su_next   = ce->ce_subscription;
    ce->ce_subscription = su;
  done:
    return su;
}

static struct client_entry *
ce_find_bypid(struct client_entry *ce_list, int pid)
{
    struct client_entry *ce;

    for (ce = ce_list; ce; ce = ce->ce_next)
	if (ce->ce_pid == pid)
	    return ce;
    return NULL;
}

static int
client_subscription_delete(struct client_entry *ce, 
		    struct client_subscription *su0)
{
    struct client_subscription   *su;
    struct client_subscription  **su_prev;

    su_prev = &ce->ce_subscription; /* this points to stack and is not real backpointer */
    for (su = *su_prev; su; su = su->su_next){
	if (su == su0){
	    *su_prev = su->su_next;
	    free(su->su_stream);
	    if (su->su_filter)
		free(su->su_filter);
	    free(su);
	    break;
	}
	su_prev = &su->su_next;
    }
    return 0;
}

static struct client_subscription *
client_subscription_find(struct client_entry *ce, char *stream)
{
    struct client_subscription   *su = NULL;

    for (su = ce->ce_subscription; su; su = su->su_next)
	if (strcmp(su->su_stream, stream) == 0)
	    break;

    return su;
}

/*! Remove client entry state
 * Close down everything wrt clients (eg sockets, subscriptions)
 * Finally actually remove client struct in handle
 * @param[in]  h   Clicon handle
 * @param[in]  ce  Client hadnle
 * @see backend_client_delete for actual deallocation of client entry struct
 */
int
backend_client_rm(clicon_handle        h, 
		  struct client_entry *ce)
{
    struct client_entry   *c;
    struct client_entry   *c0;
    struct client_entry  **ce_prev;
    struct client_subscription   *su;

    c0 = backend_client_list(h);
    ce_prev = &c0; /* this points to stack and is not real backpointer */
    for (c = *ce_prev; c; c = c->ce_next){
	if (c == ce){
	    if (ce->ce_s){
		event_unreg_fd(ce->ce_s, from_client);
		close(ce->ce_s);
		ce->ce_s = 0;
	    }
	    while ((su = ce->ce_subscription) != NULL)
		client_subscription_delete(ce, su);
	    break;
	}
	ce_prev = &c->ce_next;
    }
    return backend_client_delete(h, ce); /* actually purge it */
}

/*! Internal message: Change entry set/delete in database xmldb variant
 * @param[in]   h     Clicon handle
 * @param[in]   s     Socket where request arrived, and where replies are sent
 * @param[in]   pid   Unix process id
 * @param[in]   msg   Message
 * @param[in]   label Memory chunk
 * @retval      0     OK
 * @retval      -1    Error. Send error message back to client.
 */
static int
from_client_change(clicon_handle      h,
		   int                s, 
		   int                pid, 
		   struct clicon_msg *msg, 
		   const char        *label)
{
    int         retval = -1;
    uint32_t    len;
    char       *xk;
    char       *db;
    enum operation_type op;
    char       *str = NULL;
    char       *val=NULL;
    int         piddb;

    if (clicon_msg_change_decode(msg, 
				 &db, 
				 &op,
				 &xk, 
				 &val, 
				 &len, 
				 label) < 0){
	send_msg_err(s, clicon_errno, clicon_suberrno,
		     clicon_err_reason);
	goto done;
    }
    /* candidate is locked by other client */
    if (strcmp(db, "candidate") == 0){
	piddb = xmldb_islocked(h, db);
	if (piddb && pid != piddb){
	    send_msg_err(s, OE_DB, 0,
			 "lock failed: locked by %d", piddb);
	    goto done;
	}
    }
    /* Update database */
    if (xmldb_put_xkey(h, db, xk, val, op) < 0){
	send_msg_err(s, clicon_errno, clicon_suberrno,
		     clicon_err_reason);
	goto done;
    }
    if (send_msg_ok(s) < 0)
	goto done;
    retval = 0;
  done:
    if (str)
	free(str);
    return retval;
}

/*! Internal message: Change entries as XML 
 * @param[in]   h     Clicon handle
 * @param[in]   s     Socket where request arrived, and where replies are sent
 * @param[in]   pid   Unix process id
 * @param[in]   msg   Message
 * @param[in]   label Memory chunk
 * @retval      0     OK
 * @retval      -1    Error. Send error message back to client.
 */
static int
from_client_xmlput(clicon_handle      h,
		   int                s, 
		   int                pid, 
		   struct clicon_msg *msg, 
		   const char        *label)
{
    int         retval = -1;
    char       *db;
    enum operation_type op;
    cvec       *cvv = NULL;
    char       *str = NULL;
    char       *xml = NULL;
    cxobj      *xt = NULL;
    int         piddb;

    if (clicon_msg_xmlput_decode(msg, 
				 &db, 
				 &op,
				 &xml, 
				 label) < 0){
	send_msg_err(s, clicon_errno, clicon_suberrno,
		     clicon_err_reason);
	goto done;
    }
    /* candidate is locked by other client */
    if (strcmp(db, "candidate") == 0){
	piddb = xmldb_islocked(h, db);
	if (piddb && pid != piddb){
	    send_msg_err(s, OE_DB, 0,
			 "lock failed: locked by %d", piddb);
	    goto done;
	}
    }
    if (clicon_xml_parse_string(&xml, &xt) < 0){
	send_msg_err(s, clicon_errno, clicon_suberrno,
		     clicon_err_reason);
	goto done;
    }
    if (xmldb_put(h, db, xt, op) < 0){
	send_msg_err(s, clicon_errno, clicon_suberrno,
		     clicon_err_reason);
	goto done;
    }
    if (send_msg_ok(s) < 0)
	goto done;
    retval = 0;
  done:
    if (str)
	free(str);
    if (cvv)
	cvec_free (cvv);
    if (xt)
	xml_free(xt);
    return retval;
}

/* Nr of snapshots. Can be made into a dynamic option */
#define SNAPSHOTS_NR 30
/*! dump old running_db to snapshot file #0. move all other checkpoints
 * one step up
 */
int
config_snapshot(clicon_handle h,
		char *db, 
		char *dir)
{
    int         retval = -1;
    char        filename0[MAXPATHLEN];
    char        filename1[MAXPATHLEN];
    struct stat st;
    int         i;
    FILE       *f = NULL;
    cxobj      *xn;

    if (stat(dir, &st) < 0){
	clicon_err(OE_CFG, errno, "%s: stat(%s): %s\n", 
		__FUNCTION__, dir, strerror(errno));
	return -1;
    }
    if (!S_ISDIR(st.st_mode)){
	clicon_err(OE_CFG, 0, "%s: %s: not directory\n", 
		__FUNCTION__, dir);
	return -1;
    }
    for (i=SNAPSHOTS_NR-1; i>0; i--){
	snprintf(filename0, MAXPATHLEN, "%s/%d", 
		 dir,
		 i-1);
	snprintf(filename1, MAXPATHLEN, "%s/%d", 
		 dir,
		 i);
	if (stat(filename0, &st) == 0)
	    if (rename(filename0, filename1) < 0){
		clicon_err(OE_CFG, errno, "%s: rename(%s, %s): %s\n", 
			__FUNCTION__, filename0, filename1, strerror(errno));
		return -1;
	    }
    }
    /* Make the most current snapshot */
    snprintf(filename0, MAXPATHLEN, "%s/0", dir);
    if ((f = fopen(filename0, "wb")) == NULL){
	clicon_err(OE_CFG, errno, "Creating file %s", filename0);
	return -1;
    } 
    if (xmldb_get(h, db, "/", 0, &xn, NULL, NULL) < 0)
	goto done;
    if (xml_print(f, xn) < 0)
	goto done;
    retval = 0;
 done:
    if (f != NULL)
	fclose(f);
    if (xn)
	xml_free(xn);
    return retval;
}


/*! Internal message: Dump/print database to file
 * @param[in]   h     Clicon handle
 * @param[in]   s     Socket where request arrived, and where replies are sent
 * @param[in]   msg   Message
 * @param[in]   label Memory chunk
 * @retval      0     OK
 * @retval      -1    Error. Send error message back to client.
 */
static int
from_client_save(clicon_handle      h,
		 int                s, 
		 struct clicon_msg *msg, 
		 const char        *label)
{
    int      retval = -1;
    char    *filename;
    char    *archive_dir;
    char    *db;
    uint32_t snapshot;
    FILE    *f = NULL;
    cxobj   *xn = NULL;

    if (clicon_msg_save_decode(msg, 
			      &db, 
			      &snapshot,
			      &filename,
			      label) < 0){
	send_msg_err(s, clicon_errno, clicon_suberrno,
		     clicon_err_reason);
	goto done;
    }
    if (strcmp(db, "running") != 0 && strcmp(db, "candidate") != 0){
	clicon_err(OE_XML, 0, "Expected running or candidate, got %s", db);
	goto done;
    }
    if (snapshot){
	if ((archive_dir = clicon_archive_dir(h)) == NULL){
	    clicon_err(OE_PLUGIN, 0, "snapshot set and clicon_archive_dir not defined");
	    goto done;
	}
	if (config_snapshot(h, db, archive_dir) < 0){
	    send_msg_err(s, clicon_errno, clicon_suberrno,
		     clicon_err_reason);

	    goto done;
	}
    }
    else{
	if ((f = fopen(filename, "wb")) == NULL){
	    clicon_err(OE_CFG, errno, "Creating file %s", filename);
	    return -1;
	} 
	if (xmldb_get(h, db, "/", 0, &xn, NULL, NULL) < 0)
	    goto done;
	if (xml_print(f, xn) < 0)
	    goto done; 
    }
    if (send_msg_ok(s) < 0)
	goto done;
    retval = 0;
  done:
    if (f != NULL)
	fclose(f);
    if (xn)
	xml_free(xn);
    return retval;
}

/*! Internal message: Load file into database
 * @param[in]   h     Clicon handle
 * @param[in]   s     Socket where request arrived, and where replies are sent
 * @param[in]   pid   Unix process id
 * @param[in]   msg   Message
 * @param[in]   label Memory chunk
 * @retval      0     OK
 * @retval      -1    Error. Send error message back to client.
 */
static int
from_client_load(clicon_handle      h,
		 int                s, 
		 int                pid, 
		 struct clicon_msg *msg,
		 const char        *label)

{
    char      *filename = NULL;
    int        retval = -1;
    char      *db = NULL;
    int        replace = 0;
    int        fd = -1;
    cxobj     *xt = NULL;
    cxobj     *xn;
    int        piddb;

    if (clicon_msg_load_decode(msg, 
			       &replace,
			       &db, 
			       &filename,
			       label) < 0){
	send_msg_err(s, clicon_errno, clicon_suberrno,
		     clicon_err_reason);
	goto done;
    }
    if (strcmp(db, "running") != 0 && strcmp(db, "candidate") != 0){
	clicon_err(OE_XML, 0, "Expected running or candidate, got %s", db);
	goto done;
    }
    /* candidate is locked by other client */
    if (strcmp(db, "candidate") == 0){
	piddb = xmldb_islocked(h, db);
	if (piddb && pid != piddb){
	    send_msg_err(s, OE_DB, 0,
			 "lock failed: locked by %d", piddb);
	    goto done;
	}
    }
    if (replace){
	if (xmldb_delete(h, db) < 0){
	    send_msg_err(s, OE_UNIX, 0, "rm %s %s", filename, strerror(errno));
	    goto done;
	}
	if (xmldb_init(h, db) < 0) 
	    goto done;
    }

    if ((fd = open(filename, O_RDONLY)) < 0){
	clicon_err(OE_UNIX, errno, "%s: open(%s)", __FUNCTION__, filename);
	send_msg_err(s, clicon_errno, clicon_suberrno,
		     clicon_err_reason);
	goto done;
    }
    if (clicon_xml_parse_file(fd, &xt, "</clicon>") < 0){
	send_msg_err(s, clicon_errno, clicon_suberrno,
		     clicon_err_reason);
	goto done;
    }
    if ((xn = xml_child_i(xt, 0)) != NULL){
	if (xmldb_put(h, db, xn, replace?OP_REPLACE:OP_MERGE) < 0){
	    send_msg_err(s, clicon_errno, clicon_suberrno,
			 clicon_err_reason);
	    goto done;
	}
    }
    if (send_msg_ok(s) < 0)
	goto done;
    retval = 0;
  done:
    if (fd != -1)
	close(fd);
    if (xt)
	xml_free(xt);
    return retval;
}

/*! Internal message: Copy file from file1 to file2
 * @param[in]   h     Clicon handle
 * @param[in]   s     Socket where request arrived, and where replies are sent
 * @param[in]   pid   Unix process id
 * @param[in]   msg   Message
 * @param[in]   label Memory chunk
 * @retval      0     OK
 * @retval      -1    Error. Send error message back to client.
 */
static int
from_client_copy(clicon_handle      h,
		 int                s, 
		 int                pid, 
		 struct clicon_msg *msg, 
		 const char        *label)
{
    char *db1;
    char *db2;
    int   retval = -1;

    if (clicon_msg_copy_decode(msg, 
			      &db1,
			      &db2,
			      label) < 0){
	send_msg_err(s, clicon_errno, clicon_suberrno,
		     clicon_err_reason);
	goto done;
    }
    if (xmldb_copy(h, db1, db2) < 0)
	goto done;
    if (send_msg_ok(s) < 0)
	goto done;
    retval = 0;
  done:
    return retval;
}


/*! Internal message:  Kill session (Kill the process)
 * @param[in]   h     Clicon handle
 * @param[in]   s     Client socket where request arrived, and where replies are sent
 * @param[in]   msg   Message
 * @param[in]   label Memory chunk
 * @retval      0     OK
 * @retval      -1    Error. Send error message back to client.
 */
static int
from_client_kill(clicon_handle      h,
		 int                s, 
		 struct clicon_msg *msg, 
		 const char        *label)
{
    int                  retval = -1;
    uint32_t             pid; /* other pid */
    struct client_entry *ce;
    char                *db = "running"; /* XXX */

    if (clicon_msg_kill_decode(msg, 
			      &pid,
			      label) < 0){
	send_msg_err(s, clicon_errno, clicon_suberrno,
		     clicon_err_reason);
	goto done;
    }
    /* may or may not be in active client list, probably not */
    if ((ce = ce_find_bypid(backend_client_list(h), pid)) != NULL)
	backend_client_rm(h, ce);
    if (kill (pid, 0) != 0 && errno == ESRCH) /* Nothing there */
	;
    else{
	killpg(pid, SIGTERM);
	kill(pid, SIGTERM);
#if 0 /* Hate sleeps we assume it died, see also 0 in next if.. */
	sleep(1);
#endif
    }
    if (1 || (kill (pid, 0) != 0 && errno == ESRCH)){ /* Nothing there */
	/* clear from locks */
	if (xmldb_islocked(h, db) == pid)
	    xmldb_unlock(h, db, pid);
    }
    else{ /* failed to kill client */
	send_msg_err(s, OE_DB, 0, "failed to kill %d", pid);
	goto done;
    }
    if (send_msg_ok(s) < 0)
	goto done;
    retval = 0;
  done:
    return retval;
}

/*! Internal message: Set debug level. This is global, not just for the session.
 * @param[in]   h     Clicon handle
 * @param[in]   s     Client socket where request arrived, and where replies are sent
 * @param[in]   msg   Message
 * @param[in]   label Memory chunk
 * @retval      0     OK
 * @retval      -1    Error. Send error message back to client.
 */
static int
from_client_debug(clicon_handle      h,
		  int                s, 
		  struct clicon_msg *msg, 
		  const char        *label)
{
    int retval = -1;
    uint32_t level;

    if (clicon_msg_debug_decode(msg, 
				&level,
				label) < 0){
	send_msg_err(s, clicon_errno, clicon_suberrno,
		     clicon_err_reason);
	goto done;
    }
    clicon_debug_init(level, NULL); /* 0: dont debug, 1:debug */

    if (send_msg_ok(s) < 0)
	goto done;
    retval = 0;
  done:
    return retval;
}

/*! Internal message: downcall backend plugin
 * @param[in]   h     Clicon handle
 * @param[in]   s     Client socket where request arrived, and where replies are sent
 * @param[in]   msg   Message
 * @param[in]   label Memory chunk
 * @retval      0    OK
 * @retval      -1   Error. Send error message back to client.
 */
static int
from_client_call(clicon_handle      h,
		 int                s, 
		 struct clicon_msg *msg, 
		 const char        *label)
{
    int retval = -1;
    void *reply_data = NULL;
    uint16_t reply_data_len = 0;
    struct clicon_msg_call_req *req;

    if (clicon_msg_call_decode(msg, &req, label) < 0) {
	send_msg_err(s, clicon_errno, clicon_suberrno,
		     clicon_err_reason);
	goto done;
    }
#ifdef notyet
    if (!strlen(req->cr_plugin)) /* internal */
	internal_function(req, &reply_data_len, &reply_data);
    else
#endif
	if (plugin_downcall(h, req, &reply_data_len, &reply_data) < 0)  {
	    send_msg_err(s, clicon_errno, clicon_suberrno,
		     clicon_err_reason);
	    goto done;
	}
    
    retval = send_msg_reply(s,CLICON_MSG_OK, (char *)reply_data, reply_data_len);
    free(reply_data);

 done:
    return retval;
}

/*! Internal message: Create subscription for notifications
 * @param[in]   h     Clicon handle
 * @param[in]   ce    Client entry (from).
 * @param[in]   msg   Message
 * @param[in]   label Memory chunk
 * @retval      0    OK
 * @retval      -1   Error. Send error message back to client.
 */
static int
from_client_subscription(clicon_handle        h,
			 struct client_entry *ce,
			 struct clicon_msg   *msg, 
			 const char          *label)
{
    int                  status;
    enum format_enum     format;
    char                *stream;
    char                *filter;
    int                  retval = -1;
    struct client_subscription *su;
    clicon_log_notify_t *old;

    if (clicon_msg_subscription_decode(msg, 
				       &status,
				       &stream,
				       &format,
				       &filter,
				       label) < 0){
	send_msg_err(ce->ce_s, clicon_errno, clicon_suberrno,
		     clicon_err_reason);
	goto done;
    }

    if (status){
	if ((su = client_subscription_add(ce, stream, format, filter)) == NULL){
	    send_msg_err(ce->ce_s, clicon_errno, clicon_suberrno,
			 clicon_err_reason);
	    goto done;
	}
    }
    else{
	if ((su = client_subscription_find(ce, stream)) != NULL)
	    client_subscription_delete(ce, su);
    }
    /* Avoid recursion when sending logs */
    old = clicon_log_register_callback(NULL, NULL);
    if (send_msg_ok(ce->ce_s) < 0)
	goto done;
    clicon_log_register_callback(old, h); /* XXX: old h */
    retval = 0;
  done:
    return retval;
}

/*! An internal clicon message has arrived from a client. Receive and dispatch.
 * @param[in]   s    Socket where message arrived. read from this.
 * @param[in]   arg  Client entry (from).
 * @retval      0    OK
 * @retval      -1   Error Terminates backend and is never called). Instead errors are
 *                   propagated back to client.
 */
int
from_client(int s, void* arg)
{
    struct client_entry *ce = (struct client_entry *)arg;
    clicon_handle        h = ce->ce_handle;
    struct clicon_msg   *msg;
    enum clicon_msg_type type;
    int                  eof;

    assert(s == ce->ce_s);
    if (clicon_msg_rcv(ce->ce_s, &msg, &eof, __FUNCTION__) < 0)
	goto done;
    if (eof){ 
	backend_client_rm(h, ce); 
	goto done;
    }
    type = ntohs(msg->op_type);
    switch (type){
    case CLICON_MSG_COMMIT:
	if (from_client_commit(h, ce->ce_s, msg, __FUNCTION__) < 0)
	    goto done;
	break;
    case CLICON_MSG_VALIDATE:
	if (from_client_validate(h, ce->ce_s, msg, __FUNCTION__) < 0)
	    goto done;
	break;
    case CLICON_MSG_CHANGE:
	if (from_client_change(h, ce->ce_s, ce->ce_pid, msg, 
				     (char *)__FUNCTION__) < 0)
	    goto done;
	break;
    case CLICON_MSG_XMLPUT:
	if (from_client_xmlput(h, ce->ce_s, ce->ce_pid, msg, 
			    (char *)__FUNCTION__) < 0)
	    goto done;
	break;
    case CLICON_MSG_SAVE:
	if (from_client_save(h, ce->ce_s, msg, __FUNCTION__) < 0)
	    goto done;
	break;
    case CLICON_MSG_LOAD:
	if (from_client_load(h, ce->ce_s, ce->ce_pid, msg, __FUNCTION__) < 0)
	    goto done;
	break;
    case CLICON_MSG_COPY:
	if (from_client_copy(h, ce->ce_s, ce->ce_pid, msg, __FUNCTION__) < 0)
	    goto done;
	break;
    case CLICON_MSG_KILL:
	if (from_client_kill(h, ce->ce_s, msg, __FUNCTION__) < 0)
	    goto done;
	break;
    case CLICON_MSG_DEBUG:
	if (from_client_debug(h, ce->ce_s, msg, __FUNCTION__) < 0)
	    goto done;
	break;
    case CLICON_MSG_CALL:
	if (from_client_call(h, ce->ce_s, msg, __FUNCTION__) < 0)
	    goto done;
	break;
    case CLICON_MSG_SUBSCRIPTION:
	if (from_client_subscription(h, ce, msg, __FUNCTION__) < 0)
	    goto done;
	break;
    default:
	send_msg_err(s, OE_PROTO, 0, "Unexpected message: %d", type);
	goto done;
    }
//    retval = 0;
  done:
    unchunk_group(__FUNCTION__);
//    return retval;
    return 0; // -1 here terminates
}

