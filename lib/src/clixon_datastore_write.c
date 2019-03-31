/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand and Benny Holmgren

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****
1000 entries
valgrind --tool=callgrind datastore_client -d candidate -b /tmp/text -p ../datastore/text/text.so -y /tmp -m ietf-ip mget 300 /x/y[a=574][b=574] > /dev/null
  xml_copy_marked 87% 200x 
    yang_key_match 81% 600K
      yang_arg2cvec 52% 400K
      cvecfree      23% 400K

10000 entries
valgrind --tool=callgrind datastore_client -d candidate -b /tmp/text -p ../datastore/text/text.so -y /tmp -m ietf-ip mget 10 /x/y[a=574][b=574] > /dev/null

 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <fnmatch.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <assert.h>
#include <syslog.h>       
#include <fcntl.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_err.h"
#include "clixon_string.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_log.h"
#include "clixon_file.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_xml_sort.h"
#include "clixon_options.h"
#include "clixon_data.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_xml_map.h"
#include "clixon_json.h"
#include "clixon_nacm.h"
#include "clixon_netconf_lib.h"
#include "clixon_yang_module.h"

#include "clixon_datastore.h"
#include "clixon_datastore_write.h"
#include "clixon_datastore_read.h"


/*! Modify a base tree x0 with x1 with yang spec y according to operation op
 * @param[in]  th       Datastore text handle
 * @param[in]  x0       Base xml tree (can be NULL in add scenarios)
 * @param[in]  y0       Yang spec corresponding to xml-node x0. NULL if x0 is NULL
 * @param[in]  x0p      Parent of x0
 * @param[in]  x1       XML tree which modifies base
 * @param[in]  op       OP_MERGE, OP_REPLACE, OP_REMOVE, etc 
 * @param[in]  username User name of requestor for nacm
 * @param[in]  xnacm    NACM XML tree (only if !permit)
 * @param[in]  permit   If set, no NACM tests using xnacm required
 * @param[out] cbret    Initialized cligen buffer. Contains return XML if retval is 0.
 * @retval    -1        Error
 * @retval     0        Failed (cbret set)
 * @retval     1        OK
 * Assume x0 and x1 are same on entry and that y is the spec
 * @see text_modify_top
 */
static int
text_modify(clicon_handle       h,
	    cxobj              *x0,
	    yang_node          *y0,
	    cxobj              *x0p,
	    cxobj              *x1,
	    enum operation_type op,
	    char               *username,
	    cxobj              *xnacm,
	    int                 permit,
	    cbuf               *cbret)
{
    int        retval = -1;
    char      *opstr;
    char      *x1name;
    char      *x1cname; /* child name */
    cxobj     *x0a; /* attribute */
    cxobj     *x1a; /* attribute */
    cxobj     *x0c; /* base child */
    cxobj     *x0b; /* base body */
    cxobj     *x1c; /* mod child */
    char      *xns;  /* namespace */
    char      *x0bstr; /* mod body string */
    char      *x1bstr; /* mod body string */
    yang_stmt *yc;  /* yang child */
    cxobj    **x0vec = NULL;
    int        i;
    int        ret;

    assert(x1 && xml_type(x1) == CX_ELMNT);
    assert(y0);
    /* Check for operations embedded in tree according to netconf */
    if ((opstr = xml_find_value(x1, "operation")) != NULL)
	if (xml_operation(opstr, &op) < 0)
	    goto done;
    x1name = xml_name(x1);
    if (y0->yn_keyword == Y_LEAF_LIST || y0->yn_keyword == Y_LEAF){
	x1bstr = xml_body(x1);
	switch(op){ 
	case OP_CREATE:
	    if (x0){
		if (netconf_data_exists(cbret, "Data already exists; cannot create new resource") < 0)
		    goto done;
		goto fail;
	    }
	case OP_NONE: /* fall thru */
	case OP_MERGE:
	case OP_REPLACE:
	    if (x0==NULL){
		if ((op != OP_NONE) && !permit && xnacm){
		    if ((ret = nacm_datanode_write(NULL, x1, NACM_CREATE, username, xnacm, cbret)) < 0) 
			goto done;
		    if (ret == 0)
			goto fail;
		    permit = 1;
		}
		//		int iamkey=0;
		if ((x0 = xml_new(x1name, x0p, (yang_stmt*)y0)) == NULL)
		    goto done;

		/* Copy xmlns attributes  */
		x1a = NULL;
		while ((x1a = xml_child_each(x1, x1a, CX_ATTR)) != NULL) 
		    if (strcmp(xml_name(x1a),"xmlns")==0 ||
			((xns = xml_prefix(x1a)) && strcmp(xns, "xmlns")==0)){
			if ((x0a = xml_dup(x1a)) == NULL)
			    goto done;
			if (xml_addsub(x0, x0a) < 0)
			    goto done;
		    }

#if 0
		/* If it is key I dont want to mark it */
		if ((iamkey=yang_key_match(y0->yn_parent, x1name)) < 0)
		    goto done;
		if (!iamkey && op==OP_NONE)
#else
		if (op==OP_NONE)
#endif
		    xml_flag_set(x0, XML_FLAG_NONE); /* Mark for potential deletion */
		if (x1bstr){ /* empty type does not have body */
		    if ((x0b = xml_new("body", x0, NULL)) == NULL)
			goto done; 
		    xml_type_set(x0b, CX_BODY);
		}
	    }
	    if (x1bstr){
		if ((x0b = xml_body_get(x0)) != NULL){
		    x0bstr = xml_value(x0b);
		    if (x0bstr==NULL || strcmp(x0bstr, x1bstr)){
			if ((op != OP_NONE) && !permit && xnacm){
			    if ((ret = nacm_datanode_write(NULL, x1,
							   x0bstr==NULL?NACM_CREATE:NACM_UPDATE,
							   username, xnacm, cbret)) < 0)
				goto done;
			    if (ret == 0)
				goto fail;
			}
			if (xml_value_set(x0b, x1bstr) < 0)
			    goto done;
		    }
		}
	    }
	    break;
	case OP_DELETE:
	    if (x0==NULL){
		if (netconf_data_missing(cbret, "Data does not exist; cannot delete resource") < 0)
		    goto done;
		goto fail;
	    }
	case OP_REMOVE: /* fall thru */
	    if (x0){
		if ((op != OP_NONE) && !permit && xnacm){
		    if ((ret = nacm_datanode_write(NULL, x0, NACM_DELETE, username, xnacm, cbret)) < 0)
			goto done;
		    if (ret == 0)
			goto fail;
		}
		if (xml_purge(x0) < 0)
		    goto done;
	    }
	    break;
	default:
	    break;
	} /* switch op */
    } /* if LEAF|LEAF_LIST */
    else { /* eg Y_CONTAINER, Y_LIST, Y_ANYXML  */
	switch(op){ 
	case OP_CREATE:
	    if (x0){
		if (netconf_data_exists(cbret, "Data already exists; cannot create new resource") < 0)
		    goto done;
		goto fail;
	    }
	case OP_REPLACE: /* fall thru */
	    if (!permit && xnacm){
		if ((ret = nacm_datanode_write(NULL, x1, x0?NACM_UPDATE:NACM_CREATE, username, xnacm, cbret)) < 0) 
		    goto done;
		if (ret == 0)
		    goto fail;
		permit = 1;
	    }
	    if (x0){
		xml_purge(x0);
		x0 = NULL;
	    }
	case OP_MERGE:  /* fall thru */
	case OP_NONE: 
	    /* Special case: anyxml, just replace tree, 
	       See rfc6020 7.10.3:n
	       An anyxml node is treated as an opaque chunk of data.  This data
	       can be modified in its entirety only.
	       Any "operation" attributes present on subelements of an anyxml 
	       node are ignored by the NETCONF server.*/
	    if (y0->yn_keyword == Y_ANYXML || y0->yn_keyword == Y_ANYDATA){
		if (op == OP_NONE)
		    break;
		if (op==OP_MERGE && !permit && xnacm){
		    if ((ret = nacm_datanode_write(NULL, x0, x0?NACM_UPDATE:NACM_CREATE, username, xnacm, cbret)) < 0) 
			goto done;
		    if (ret == 0)
			goto fail;
		    permit = 1;
		}
		if (x0){
		    xml_purge(x0);
		}
		if ((x0 = xml_new(x1name, x0p, (yang_stmt*)y0)) == NULL)
		    goto done;
		if (xml_copy(x1, x0) < 0)
		    goto done;
		break;
	    }
	    if (x0==NULL){
		if (op==OP_MERGE && !permit && xnacm){
		    if ((ret = nacm_datanode_write(NULL, x0, x0?NACM_UPDATE:NACM_CREATE, username, xnacm, cbret)) < 0) 
			goto done;
		    if (ret == 0)
			goto fail;
		    permit = 1;
		}
		if ((x0 = xml_new(x1name, x0p, (yang_stmt*)y0)) == NULL)
		    goto done;
		/* Copy xmlns attributes  */
		x1a = NULL;
		while ((x1a = xml_child_each(x1, x1a, CX_ATTR)) != NULL) 
		    if (strcmp(xml_name(x1a),"xmlns")==0 ||
			((xns = xml_prefix(x1a)) && strcmp(xns, "xmlns")==0)){
			if ((x0a = xml_dup(x1a)) == NULL)
			    goto done;
			if (xml_addsub(x0, x0a) < 0)
			    goto done;
		    }
		if (op==OP_NONE)
		    xml_flag_set(x0, XML_FLAG_NONE); /* Mark for potential deletion */
	    }
	    /* First pass: Loop through children of the x1 modification tree 
	     * collect matching nodes from x0 in x0vec (no changes to x0 children)
	     */
	    if ((x0vec = calloc(xml_child_nr(x1), sizeof(x1))) == NULL){
		clicon_err(OE_UNIX, errno, "calloc");
		goto done;
	    }
	    x1c = NULL; 
	    i = 0;
	    while ((x1c = xml_child_each(x1, x1c, CX_ELMNT)) != NULL) {
		x1cname = xml_name(x1c);
		/* Get yang spec of the child */
		if ((yc = yang_find_datanode(y0, x1cname)) == NULL){
		    clicon_err(OE_YANG, errno, "No yang node found: %s", x1cname);
		    goto done;
		}
		/* See if there is a corresponding node in the base tree */
		x0c = NULL;
		if (match_base_child(x0, x1c, yc, &x0c) < 0)
		    goto done;
#if 1
		if (x0c && (yc != xml_spec(x0c))){
		    /* There is a match but is should be replaced (choice)*/
		    if (xml_purge(x0c) < 0)
			goto done;
		    x0c = NULL;
		}
#endif
		x0vec[i++] = x0c; /* != NULL if x0c is matching x1c */
	    }
	    /* Second pass: Loop through children of the x1 modification tree again
	     * Now potentially modify x0:s children 
	     * Here x0vec contains one-to-one matching nodes of x1:s children.
	     */
	    x1c = NULL;
	    i = 0;
	    while ((x1c = xml_child_each(x1, x1c, CX_ELMNT)) != NULL) {
		x1cname = xml_name(x1c);
		x0c = x0vec[i++];
		yc = yang_find_datanode(y0, x1cname);
		if ((ret = text_modify(h, x0c, (yang_node*)yc, x0, x1c, op,
				       username, xnacm, permit, cbret)) < 0)
		    goto done;
		/* If xml return - ie netconf error xml tree, then stop and return OK */
		if (ret == 0)
		    goto fail;
	    }
	    break;
	case OP_DELETE:
	    if (x0==NULL){
		if (netconf_data_missing(cbret, "Data does not exist; cannot delete resource") < 0)
		    goto done;
		goto fail;
	    }
	case OP_REMOVE: /* fall thru */
	    if (x0){
		if (!permit && xnacm){
		    if ((ret = nacm_datanode_write(NULL, x0, NACM_DELETE, username, xnacm, cbret)) < 0) 
			goto done;
		    if (ret == 0)
			goto fail;
		}
		if (xml_purge(x0) < 0)
		    goto done;
	    }
	    break;
	default:
	    break;
	} /* CONTAINER switch op */
    } /* else Y_CONTAINER  */
    xml_sort(x0p, NULL);
    retval = 1;
 done:
    if (x0vec)
	free(x0vec);
    return retval;
 fail: /* cbret set */
    retval = 0;
    goto done;
} /* text_modify */

/*! Modify a top-level base tree x0 with modification tree x1
 * @param[in]  th       Datastore text handle
 * @param[in]  x0       Base xml tree (can be NULL in add scenarios)
 * @param[in]  x1       XML tree which modifies base
 * @param[in]  yspec    Top-level yang spec (if y is NULL)
 * @param[in]  op       OP_MERGE, OP_REPLACE, OP_REMOVE, etc 
 * @param[in]  username User name of requestor for nacm
 * @param[in]  xnacm    NACM XML tree (only if !permit)
 * @param[in]  permit   If set, no NACM tests using xnacm required
 * @param[out] cbret    Initialized cligen buffer. Contains return XML if retval is 0.
 * @retval    -1        Error
 * @retval     0        Failed (cbret set)
 * @retval     1        OK
 * @see text_modify
 */
static int
text_modify_top(clicon_handle       h,
		cxobj              *x0,
		cxobj              *x1,
		yang_spec          *yspec,
		enum operation_type op,
		char               *username,
		cxobj              *xnacm,
		int                 permit,
		cbuf               *cbret)
{
    int        retval = -1;
    char      *x1cname; /* child name */
    cxobj     *x0c; /* base child */
    cxobj     *x1c; /* mod child */
    yang_stmt *yc;  /* yang child */
    yang_stmt *ymod;/* yang module */
    char      *opstr;
    int        ret;

    /* Assure top-levels are 'config' */
    assert(x0 && strcmp(xml_name(x0),"config")==0);
    assert(x1 && strcmp(xml_name(x1),"config")==0);

    /* Check for operations embedded in tree according to netconf */
    if ((opstr = xml_find_value(x1, "operation")) != NULL)
	if (xml_operation(opstr, &op) < 0)
	    goto done;
    /* Special case if x1 is empty, top-level only <config/> */
    if (xml_child_nr_type(x1, CX_ELMNT) == 0){ 
	if (xml_child_nr_type(x0, CX_ELMNT)){ /* base tree not empty */
	    switch(op){ 
	    case OP_DELETE:
	    case OP_REMOVE:
	    case OP_REPLACE:
		if (!permit && xnacm){
		    if ((ret = nacm_datanode_write(NULL, x0, NACM_DELETE, username, xnacm, cbret)) < 0)
			goto done;
		    if (ret == 0)
			goto fail;
		    permit = 1;
		}
		while ((x0c = xml_child_i(x0, 0)) != 0)
		    if (xml_purge(x0c) < 0)
			goto done;
		break;
	    default:
		break;
	    }
	}
	else /* base tree empty */
	    switch(op){ 
#if 0 /* According to RFC6020 7.5.8 you cant delete a non-existing object.
	 On the other hand, the top-level cannot be removed anyway.
	 Additionally, I think this is irritating so I disable it.
	 I.e., curl -u andy:bar -sS -X DELETE http://localhost/restconf/data
      */
	    case OP_DELETE:
		if (netconf_data_missing(cbret, "Data does not exist; cannot delete resource") < 0)
		    goto done;
		goto fail;
		break;
#endif
	    default:
		break;
	    }
    }
    /* Special case top-level replace */
    else if (op == OP_REPLACE || op == OP_DELETE){
	if (!permit && xnacm){
	    if ((ret = nacm_datanode_write(NULL, x1, NACM_UPDATE, username, xnacm, cbret)) < 0) 
		goto done;
	    if (ret == 0)
		goto fail;
	    permit = 1;
	}
	while ((x0c = xml_child_i(x0, 0)) != 0)
	    if (xml_purge(x0c) < 0)
		goto done;
    }
    /* Loop through children of the modification tree */
    x1c = NULL;
    while ((x1c = xml_child_each(x1, x1c, CX_ELMNT)) != NULL) {
	x1cname = xml_name(x1c);
	/* Get yang spec of the child */
	yc = NULL;
	if (ys_module_by_xml(yspec, x1c, &ymod) <0)
	    goto done;
	if (ymod != NULL)
	    yc = yang_find_datanode((yang_node*)ymod, x1cname);
	if (yc == NULL){
	    if (netconf_unknown_element(cbret, "application", x1cname, "Unassigned yang spec") < 0)
		goto done;
	    goto fail;
	}
	/* See if there is a corresponding node in the base tree */
	if (match_base_child(x0, x1c, yc, &x0c) < 0)
	    goto done;
#if 1
	if (x0c && (yc != xml_spec(x0c))){
	    /* There is a match but is should be replaced (choice)*/
	    if (xml_purge(x0c) < 0)
		goto done;
	    x0c = NULL;
	}
#endif
	if ((ret = text_modify(h, x0c, (yang_node*)yc, x0, x1c, op,
			       username, xnacm, permit, cbret)) < 0)
	    goto done;
	/* If xml return - ie netconf error xml tree, then stop and return OK */
	if (ret == 0)
	    goto fail;
    }
    // ok:
    retval = 1;
 done:
    return retval;
 fail: /* cbret set */
    retval = 0;
    goto done;
} /* text_modify_top */

/*! For containers without presence and no children(except attrs), remove
 * @param[in]   x       XML tree node
 * See section 7.5.1 in rfc6020bis-02.txt:
 * No presence:
 * those that exist only for organizing the hierarchy of data nodes:
 * the container has no meaning of its own, existing
 * only to contain child nodes.  This is the default style.
 * (Remove these if no children)
 * Presence:
 * the presence of the container itself is
 * configuration data, representing a single bit of configuration data.
 * The container acts as both a configuration knob and a means of
 * organizing related configuration.  These containers are explicitly
 * created and deleted.
 * (Dont touch these)
 */
static int
xml_container_presence(cxobj  *x, 
		       void   *arg)
{
    int        retval = -1;
    yang_stmt *y;  /* yang node */

    if ((y = (yang_stmt*)xml_spec(x)) == NULL){
	retval = 0;
	goto done;
    }
    /* Mark node that is: container, have no children, dont have presence */
    if (y->ys_keyword == Y_CONTAINER && 
	xml_child_nr_notype(x, CX_ATTR)==0 &&
	yang_find((yang_node*)y, Y_PRESENCE, NULL) == NULL)
	xml_flag_set(x, XML_FLAG_MARK); /* Mark, remove later */
    retval = 0;
 done:
    return retval;
}

/*! Modify database given an xml tree and an operation
 *
 * @param[in]  h      CLICON handle
 * @param[in]  db     running or candidate
 * @param[in]  op     Top-level operation, can be superceded by other op in tree
 * @param[in]  xt     xml-tree. Top-level symbol is dummy
 * @param[in]  username User name for nacm
 * @param[out] cbret  Initialized cligen buffer. On exit contains XML if retval == 0
 * @retval     1      OK
 * @retval     0      Failed, cbret contains error xml message
 * @retval     -1     Error
 * The xml may contain the "operation" attribute which defines the operation.
 * @code
 *   cxobj     *xt;
 *   cxobj     *xret = NULL;
 *   if (xml_parse_string("<a>17</a>", yspec, &xt) < 0)
 *     err;
 *   if ((ret = xmldb_put(h, "running", OP_MERGE, xt, username, cbret)) < 0)
 *     err;
 *   if (ret==0)
 *     cbret contains netconf error message
 * @endcode
 * @note that you can add both config data and state data. In comparison,
 *  xmldb_get has a parameter to get config data only.
 * @note if xret is non-null, it may contain error message
 */
int
xmldb_put(clicon_handle       h,
	 const char         *db, 
	 enum operation_type op,
	 cxobj              *x1,
	 char               *username,
    	 cbuf               *cbret)
{
    int                 retval = -1;
    char               *dbfile = NULL;
    FILE               *f = NULL;
    cbuf               *cb = NULL;
    yang_spec          *yspec;
    cxobj              *x0 = NULL;
    db_elmnt           *de = NULL;
    int                 ret;
    cxobj              *xnacm = NULL; 
    char               *mode;
    cxobj              *xnacm0 = NULL;
    cxobj              *xmodst = NULL;
    cxobj              *x;
    int                 permit = 0; /* nacm permit all */
    char               *format;

    if (cbret == NULL){
	clicon_err(OE_XML, EINVAL, "cbret is NULL");
	goto done;
    }
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_YANG, ENOENT, "No yang spec");
	goto done;
    }
    if (x1 && strcmp(xml_name(x1),"config")!=0){
	clicon_err(OE_XML, 0, "Top-level symbol of modification tree is %s, expected \"config\"",
		   xml_name(x1));
	goto done;
    }
    if (clicon_option_bool(h, "CLICON_XMLDB_CACHE")){
	if ((de = clicon_db_elmnt_get(h, db)) != NULL)
	    x0 = de->de_xml; 
    }
    /* If there is no xml x0 tree (in cache), then read it from file */
    if (x0 == NULL){
	if (xmldb_readfile(h, db, yspec, &x0, NULL) < 0)
	    goto done;
    }
    if (strcmp(xml_name(x0), "config")!=0){
	clicon_err(OE_XML, 0, "Top-level symbol is %s, expected \"config\"",
		   xml_name(x0));
	goto done;
    }
    /* Here x0 looks like: <config>...</config> */

#if 0 /* debug */
    if (xml_apply0(x1, -1, xml_sort_verify, NULL) < 0)
	clicon_log(LOG_NOTICE, "%s: verify failed #1", __FUNCTION__);
#endif
    mode = clicon_option_str(h, "CLICON_NACM_MODE");
    if (mode){
	if (strcmp(mode, "external")==0)
	    xnacm0 = clicon_nacm_ext(h);
	else if (strcmp(mode, "internal")==0)
	    xnacm0 = x0;
    }
    if (xnacm0 != NULL &&
	(xnacm = xpath_first(xnacm0, "nacm")) != NULL){
	/* Pre-NACM access step, if permit, then dont do any nacm checks in 
	 * text_modify_* below */
	if ((permit = nacm_access(mode, xnacm, username)) < 0)
	    goto done;
    }
    /* Here assume if xnacm is set and !permit do NACM */
    /* 
     * Modify base tree x with modification x1. This is where the
     * new tree is made.
     */
    if ((ret = text_modify_top(h, x0, x1, yspec, op, username, xnacm, permit, cbret)) < 0)
	goto done;
    /* If xml return - ie netconf error xml tree, then stop and return OK */
    if (ret == 0)
	goto fail;

    /* Remove NONE nodes if all subs recursively are also NONE */
    if (xml_tree_prune_flagged_sub(x0, XML_FLAG_NONE, 0, NULL) <0)
	goto done;
    if (xml_apply(x0, CX_ELMNT, (xml_applyfn_t*)xml_flag_reset, 
		  (void*)XML_FLAG_NONE) < 0)
	goto done;
    /* Mark non-presence containers that do not have children */
    if (xml_apply(x0, CX_ELMNT, (xml_applyfn_t*)xml_container_presence, NULL) < 0)
	goto done;
    /* Remove (prune) nodes that are marked (non-presence containers w/o children) */
    if (xml_tree_prune_flagged(x0, XML_FLAG_MARK, 1) < 0)
	goto done;
#if 0 /* debug */
    if (xml_apply0(x0, -1, xml_sort_verify, NULL) < 0)
	clicon_log(LOG_NOTICE, "%s: verify failed #3", __FUNCTION__);
#endif
    /* Write back to datastore cache if first time */
    if (clicon_option_bool(h, "CLICON_XMLDB_CACHE")){
	db_elmnt de0 = {0,};
	if (de != NULL)
	    de0 = *de;
	if (de0.de_xml == NULL){
	    de0.de_xml = x0;
	    clicon_db_elmnt_set(h, db, &de0);
	}
    }
    if (xmldb_db2file(h, db, &dbfile) < 0)
	goto done;
    if (dbfile==NULL){
	clicon_err(OE_XML, 0, "dbfile NULL");
	goto done;
    }
    /* Add module revision info before writing to file)
     * Only if CLICON_XMLDB_MODSTATE is set
     */
    if ((x = clicon_modst_cache_get(h, 1)) != NULL){
	if ((xmodst = xml_dup(x)) == NULL)
	    goto done;
	if (xml_addsub(x0, xmodst) < 0)
	    goto done;
    }
    if ((f = fopen(dbfile, "w")) == NULL){
	clicon_err(OE_CFG, errno, "Creating file %s", dbfile);
	goto done;
    } 
    format = clicon_option_str(h, "CLICON_XMLDB_FORMAT");
    if (format && strcmp(format,"json")==0){
	if (xml2json(f, x0, clicon_option_bool(h, "CLICON_XMLDB_PRETTY")) < 0)
	    goto done;
    }
    else if (clicon_xml2file(f, x0, 0, clicon_option_bool(h, "CLICON_XMLDB_PRETTY")) < 0)
	goto done;
    /* Remove modules state after writing to file
     */
    if (xmodst && xml_purge(xmodst) < 0)
	goto done;
    retval = 1;
 done:
    if (f != NULL)
	fclose(f);
    if (dbfile)
	free(dbfile);
    if (cb)
	cbuf_free(cb);
    if (!clicon_option_bool(h, "CLICON_XMLDB_CACHE") && x0)
	xml_free(x0);
    return retval;
 fail:
    retval = 0;
    goto done;
}
