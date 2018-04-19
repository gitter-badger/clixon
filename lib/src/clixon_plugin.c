/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2018 Olof Hagsand and Benny Holmgren

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
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include <dirent.h>

#include <sys/stat.h>
#include <sys/param.h>

/* cligen */
#include <cligen/cligen.h>

#include "clixon_err.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_log.h"
#include "clixon_file.h"
#include "clixon_handle.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_plugin.h"

/* List of plugins XXX 
 * 1. Place in clixon handle not global variables
 * 2. Use qelem circular lists
 */
static clixon_plugin *_clixon_plugins = NULL;  /* List of plugins (of client) */
static int            _clixon_nplugins = 0;  /* Number of plugins */

/*! Iterator over clixon plugins
 *
 * @note Never manipulate the plugin during operation or using the
 * same object recursively
 *
 * @param[in]  h       Clicon handle
 * @param[in] plugin   previous plugin, or NULL on init
 * @code
 *   clicon_plugin *cp = NULL;
 *   while ((cp = clixon_plugin_each(h, cp)) != NULL) {
 *     ...
 *   }
 * @endcode
 * @note Not optimized, alwasy iterates from the start of the list
 */
clixon_plugin *
clixon_plugin_each(clicon_handle  h,
		   clixon_plugin *cpprev)
{
    int            i;
    clixon_plugin *cp;
    clixon_plugin *cpnext = NULL; 

    if (cpprev == NULL)
	cpnext = _clixon_plugins;
    else{
	for (i = 0; i < _clixon_nplugins; i++) {
	    cp = &_clixon_plugins[i];
	    if (cp == cpprev)
		break;
	    cp = NULL;
	}
	if (cp && i < _clixon_nplugins-1)
	    cpnext = &_clixon_plugins[i+1];
    }
    return cpnext;
}

/*! Reverse iterator over clixon plugins, iterater from nr to 0
 *
 * @note Never manipulate the plugin during operation or using the
 * same object recursively
 *
 * @param[in]  h       Clicon handle
 * @param[in] plugin   previous plugin, or NULL on init
 * @code
 *   clicon_plugin *cp = NULL;
 *   while ((cp = clixon_plugin_each_revert(h, cp, nr)) != NULL) {
 *     ...
 *   }
 * @endcode
 * @note Not optimized, alwasy iterates from the start of the list
 */
clixon_plugin *
clixon_plugin_each_revert(clicon_handle  h,
			  clixon_plugin *cpprev,
			  int            nr)
{
    int            i;
    clixon_plugin *cp;
    clixon_plugin *cpnext = NULL; 

    if (cpprev == NULL)
	cpnext = &_clixon_plugins[nr-1];
    else{
	for (i = nr-1; i >= 0; i--) {
	    cp = &_clixon_plugins[i];
	    if (cp == cpprev)
		break;
	    cp = NULL;
	}
	if (cp && i > 0)
	    cpnext = &_clixon_plugins[i-1];
    }
    return cpnext;
}

/*! Find plugin by name
 * @param[in]  h    Clicon handle
 * @param[in]  name Plugin name
 * @retval     p    Plugin if found
 * @retval     NULL Not found
 */
clixon_plugin *
clixon_plugin_find(clicon_handle h,
		   char         *name)
{
    int            i;
    clixon_plugin *cp = NULL;

    for (i = 0; i < _clixon_nplugins; i++) {
	cp = &_clixon_plugins[i];
	if (strcmp(cp->cp_name, name) == 0)
	    return cp;
    }
    return NULL;
}

/*! Load a dynamic plugin object and call its init-function
 * @param[in]  h       Clicon handle
 * @param[in]  file    Which plugin to load
 * @param[in]  function Which function symbol to load and call
 * @param[in]  dlflags See man(3) dlopen
 * @retval     cp      Clixon plugin structure
 * @retval     NULL    Error
 * @see clixon_plugins_load  Load all plugins
 */
static clixon_plugin *
plugin_load_one(clicon_handle   h, 
		char           *file,
		char           *function,
		int             dlflags)
{
    char          *error;
    void          *handle = NULL;
    plginit2_t    *initfn;
    clixon_plugin_api *api = NULL;
    clixon_plugin *cp = NULL;
    char          *name;
    char          *p;

    clicon_debug(1, "%s", __FUNCTION__);
    dlerror();    /* Clear any existing error */
    if ((handle = dlopen(file, dlflags)) == NULL) {
        error = (char*)dlerror();
	clicon_err(OE_PLUGIN, errno, "dlopen: %s\n", error ? error : "Unknown error");
	goto done;
    }
    /* call plugin_init() if defined, eg CLIXON_PLUGIN_INIT or CLIXON_BACKEND_INIT */
    if ((initfn = dlsym(handle, function)) == NULL){
	clicon_err(OE_PLUGIN, errno, "Failed to find %s when loading clixon plugin %s", CLIXON_PLUGIN_INIT, file);
	goto err;
    }
    if ((error = (char*)dlerror()) != NULL) {
	clicon_err(OE_UNIX, 0, "dlsym: %s: %s", file, error);
	goto done;
    }
    if ((api = initfn(h)) == NULL) {
	clicon_err(OE_PLUGIN, errno, "Failed to initiate %s", strrchr(file,'/')?strchr(file, '/'):file);
	if (!clicon_errno) 	/* sanity: log if clicon_err() is not called ! */
	    clicon_err(OE_DB, 0, "Unknown error: %s: plugin_init does not make clicon_err call on error",
		       file);
	goto err;
    }
    /* Note: sizeof clixon_plugin_api which is largest of clixon_plugin_api:s */
    if ((cp = (clixon_plugin *)malloc(sizeof(struct clixon_plugin))) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    memset(cp, 0, sizeof(struct clixon_plugin));
    cp->cp_handle = handle;
    /* Extract string after last '/' in filename, if any */
    name = strrchr(file, '/') ? strrchr(file, '/')+1 : file;
    /* strip extension, eg .so from name */
    if ((p=strrchr(name, '.')) != NULL)
	*p = '\0';
    /* Copy name to struct */
    memcpy(cp->cp_name, name, strlen(name)+1);

    snprintf(cp->cp_name, sizeof(cp->cp_name), "%*s",
	     (int)strlen(name), name);
    cp->cp_api = *api;
    clicon_debug(1, "%s", __FUNCTION__);
 done:
    return cp;
 err:
    if (handle)
	dlclose(handle);
    return NULL;
}

/*! Load a set of plugin objects from a directory and and call their init-function
 * @param[in]  h     Clicon handle
 * @param[in]  function Which function symbol to load and call (eg CLIXON_PLUGIN_INIT)
 * @param[in]  dir   Directory. .so files in this dir will be loaded.
 * @param[in]  regexp Regexp for matching files in plugin directory. Default *.so.
 * @retval     0     OK
 * @retval     -1    Error
 */
int
clixon_plugins_load(clicon_handle h,
    		    char         *function,
		    char         *dir,
    		    char         *regexp)
{
    int            retval = -1;
    int            ndp;
    struct dirent *dp = NULL;
    int            i;
    char           filename[MAXPATHLEN];
    clixon_plugin *cp;

    clicon_debug(1, "%s", __FUNCTION__); 
    /* Get plugin objects names from plugin directory */
    if((ndp = clicon_file_dirent(dir, &dp,
				 regexp?regexp:"(.so)$", S_IFREG))<0)
	goto done;
    /* Load all plugins */
    for (i = 0; i < ndp; i++) {
	snprintf(filename, MAXPATHLEN-1, "%s/%s", dir, dp[i].d_name);
	clicon_debug(1, "DEBUG: Loading plugin '%.*s' ...", 
		     (int)strlen(filename), filename);
	if ((cp = plugin_load_one(h, filename, function, RTLD_NOW)) == NULL)
	    goto done;
	_clixon_nplugins++;
	if ((_clixon_plugins = realloc(_clixon_plugins, _clixon_nplugins*sizeof(clixon_plugin))) == NULL) {
	    clicon_err(OE_UNIX, errno, "realloc");
	    goto done;
	}
	_clixon_plugins[_clixon_nplugins-1] = *cp;
	free(cp);
    }
    retval = 0;
done:
    if (dp)
	free(dp);
    return retval;
}

/*! Call plugin_start in all plugins
 * @param[in]  h       Clicon handle
 */
int
clixon_plugin_start(clicon_handle h, 
		    int           argc, 
		    char        **argv)
{
    clixon_plugin *cp;
    int            i;
    plgstart_t    *startfn;          /* Plugin start */
    
    for (i = 0; i < _clixon_nplugins; i++) {
	cp = &_clixon_plugins[i];
	if ((startfn = cp->cp_api.ca_start) == NULL)
	    continue;
	//	optind = 0;
	if (startfn(h, argc, argv) < 0) {
	    clicon_debug(1, "plugin_start() failed\n");
	    return -1;
	}
    }
    return 0;
}

/*! Unload all plugins: call exit function and close shared handle
 * @param[in]  h       Clicon handle
 */
int
clixon_plugin_exit(clicon_handle h)
{
    clixon_plugin *cp;
    plgexit_t     *exitfn;
    int            i;
    char          *error;
    
    for (i = 0; i < _clixon_nplugins; i++) {
	cp = &_clixon_plugins[i];
	if ((exitfn = cp->cp_api.ca_exit) == NULL)
	    continue;
	if (exitfn(h) < 0) {
	    clicon_debug(1, "plugin_exit() failed\n");
	    return -1;
	}
	if (dlclose(cp->cp_handle) != 0) {
	    error = (char*)dlerror();
	    clicon_err(OE_PLUGIN, errno, "dlclose: %s\n", error ? error : "Unknown error");
	}
    }
    if (_clixon_plugins){
	free(_clixon_plugins);
	_clixon_plugins = NULL;
    }
    _clixon_nplugins = 0;
    return 0;
}


/*! Run the restconf user-defined credentials callback if present
 * Find first authentication callback and call that, then return.
 * The callback is to set the authenticated user
 * @param[in]  h    Clicon handle
 * @param[in]  arg  Argument, such as fastcgi handler for restconf
 * @retval    -1    Error
 * @retval     0    Not authenticated
 * @retval     1    Authenticated 
 * @note If authenticated either a callback was called and clicon_username_set() 
 *       Or no callback was found.
 */
int
clixon_plugin_auth(clicon_handle h, 
		   void         *arg)
{
    clixon_plugin *cp;
    int            i;
    plgauth_t     *authfn;          /* Plugin auth */
    int            retval = 1;
    
    for (i = 0; i < _clixon_nplugins; i++) {
	cp = &_clixon_plugins[i];
	if ((authfn = cp->cp_api.ca_auth) == NULL)
	    continue;
	if ((retval = authfn(h, arg)) < 0) {
	    clicon_debug(1, "plugin_start() failed\n");
	    return -1;
	}
	break;
    }
    return retval;
}

/*--------------------------------------------------------------------
 * RPC callbacks for both client/frontend and backend plugins.
 * RPC callbacks are explicitly registered in the plugin_init() function
 * with a tag and a function
 * WHen the the tag is encountered, the callback is called.
 * Primarily backend, but also netconf and restconf frontend plugins.
 * CLI frontend so far have direct callbacks, ie functions in the cligen
 * specification are directly dlsym:ed to the CLI plugin.
 * It would be possible to use this rpc registering API for CLI plugins as well.
 * 
 * @note may have a problem if callbacks are of different types
 */
typedef struct {
    qelem_t 	  rc_qelem;	/* List header */
    clicon_rpc_cb rc_callback;  /* RPC Callback */
    void	  *rc_arg;	/* Application specific argument to cb */
    char          *rc_tag;	/* Xml/json tag when matched, callback called */
} rpc_callback_t;

/* List of rpc callback entries */
static rpc_callback_t *rpc_cb_list = NULL;

/*! Register a RPC callback
 * Called from plugin to register a callback for a specific netconf XML tag.
 *
 * @param[in]  h       clicon handle
 * @param[in]  cb,     Callback called 
 * @param[in]  arg,    Domain-specific argument to send to callback 
 * @param[in]  tag     Xml tag when callback is made 
 * @see rpc_callback_call
 */
int
rpc_callback_register(clicon_handle  h,
		      clicon_rpc_cb  cb,
		      void          *arg,       
		      char          *tag)
{
    rpc_callback_t *rc;

    if ((rc = malloc(sizeof(rpc_callback_t))) == NULL) {
	clicon_err(OE_DB, errno, "malloc: %s", strerror(errno));
	goto done;
    }
    memset (rc, 0, sizeof (*rc));
    rc->rc_callback = cb;
    rc->rc_arg  = arg;
    rc->rc_tag  = strdup(tag); /* XXX strdup memleak */
    INSQ(rc, rpc_cb_list);
    return 0;
 done:
    if (rc){
	if (rc->rc_tag)
	    free(rc->rc_tag);
	free(rc);
    }
    return -1;
}

/*! Delete all RPC callbacks
 */
int
rpc_callback_delete_all(void)
{
    rpc_callback_t *rc;

    while((rc = rpc_cb_list) != NULL) {
	DELQ(rc, rpc_cb_list, rpc_callback_t *);
	if (rc->rc_tag)
	    free(rc->rc_tag);
	free(rc);
    }
    return 0;
}

/*! Search RPC callbacks and invoke if XML match with tag
 *
 * @param[in]  h       clicon handle
 * @param[in]  xn      Sub-tree (under xorig) at child of rpc: <rpc><xn></rpc>.
 * @param[out] xret    Return XML, error or OK
 * @param[in]  arg   Domain-speific arg (eg client_entry)
 *
 * @retval -1   Error
 * @retval  0   OK, not found handler.
 * @retval  1   OK, handler called
 */
int
rpc_callback_call(clicon_handle h,
		  cxobj        *xe, 
		  cbuf         *cbret,
		  void         *arg)
{
    int            retval = -1;
    rpc_callback_t *rc;

    if (rpc_cb_list == NULL)
	return 0;
    rc = rpc_cb_list;
    do {
	if (strcmp(rc->rc_tag, xml_name(xe)) == 0){
	    if ((retval = rc->rc_callback(h, xe, cbret, arg, rc->rc_arg)) < 0){
		clicon_debug(1, "%s Error in: %s", __FUNCTION__, rc->rc_tag);
		goto done;
	    }
	    else{
		retval = 1; /* handled */
		goto done;
	    }
	}
	rc = NEXTQ(rpc_callback_t *, rc);
    } while (rc != rpc_cb_list);
    retval = 0;
 done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    return retval;
}
