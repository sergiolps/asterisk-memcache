/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * Memcache client
 * 
 * Copyright (C) 2010, Konstantin Tumalevich (userad@gmail.com) 
 *
 */

#include <asterisk.h>
#include <stdio.h>
#include <asterisk/file.h>
#include <asterisk/logger.h>
#include <asterisk/channel.h>
#include <asterisk/pbx.h>
#include <asterisk/module.h>
#include <asterisk/lock.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <memcache.h>

#define AST_MODULE "memcache"
#define AST_MEMCACHE_ID_DUMMY   0
#define AST_MEMCACHE_ID_CONNID  1
#define AST_MEMCACHE_ID_RESID   2


static char *app = "MEMCACHE";
static char *synopsis = "Simple implementation memcache client for asterisk";
static char *descrip =
"Example: MEMCACHE(connect connid host:port) MEMCACHE(get connid var) MEMCACHE(set connid var)\n";

AST_MUTEX_DEFINE_STATIC(_memcache_mutex);

//BEGIN Store for connid
static void memcache_ds_destroy(void *data);
static void memcache_ds_fixup(void *data, struct ast_channel *oldchan, struct ast_channel *newchan);

static struct ast_datastore_info memcache_ds_info = {
  .type = "APP_ADDON_MEMCACHE",
  .destroy = memcache_ds_destroy,
  .chan_fixup = memcache_ds_fixup,
};

struct ast_MEMCACHE_id {
  struct ast_channel *owner;
  int identifier_type; /* 0=dummy, 1=connid, 2=resultid */
  int identifier;
  void *data;
  AST_LIST_ENTRY(ast_MEMCACHE_id) entries;
} *ast_MEMCACHE_id;

AST_LIST_HEAD(MEMCACHEidshead,ast_MEMCACHE_id) _memcache_ids_head;


static void memcache_ds_destroy(void *data)
{
	/* Destroy any IDs owned by the channel */
	struct MEMCACHEidshead *headp;
	struct ast_MEMCACHE_id *i;
	if (AST_LIST_LOCK(&_memcache_ids_head)) {
		ast_log(LOG_WARNING, "Unable to lock identifiers list\n");
	} else {
		AST_LIST_TRAVERSE_SAFE_BEGIN(&_memcache_ids_head, i, entries) {
			if (i->owner == data) {
				AST_LIST_REMOVE_CURRENT(&_memcache_ids_head, entries);
				if (i->identifier_type == AST_MEMCACHE_ID_CONNID) {
					/* Drop connection */
					mc_server_free(i->data);
				} else if (i->identifier_type == AST_MEMCACHE_ID_RESID) {
					/* Drop result */
					free(i->data);
				}
				ast_free(i);
			}
		}
		AST_LIST_TRAVERSE_SAFE_END
		AST_LIST_UNLOCK(&_memcache_ids_head);
	}
}

static void memcache_ds_fixup(void *data, struct ast_channel *oldchan, struct ast_channel *newchan)
{
	/* Destroy any IDs owned by the channel */
	struct MEMCACHEidshead *headp;
	struct ast_MEMCACHE_id *i;
	if (AST_LIST_LOCK(&_memcache_ids_head)) {
		ast_log(LOG_WARNING, "Unable to lock identifiers list\n");
	} else {
		AST_LIST_TRAVERSE_SAFE_BEGIN(&_memcache_ids_head, i, entries) {
			if (i->owner == data) {
				AST_LIST_REMOVE_CURRENT(&_memcache_ids_head, entries);
				if (i->identifier_type == AST_MEMCACHE_ID_CONNID) {
					/* Drop connection */
					mc_server_free(i->data);
				} else if (i->identifier_type == AST_MEMCACHE_ID_RESID) {
					/* Drop result */
					free(i->data);
				}
				ast_free(i);
			}
		}
		AST_LIST_TRAVERSE_SAFE_END
		AST_LIST_UNLOCK(&_memcache_ids_head);
	}
}

static void *mc_find_identifier(int identifier,int identifier_type) {
	struct MEMCACHEidshead *headp;
	struct ast_MEMCACHE_id *i;
	void *res=NULL;
	int found=0;
	
	headp=&_memcache_ids_head;
	
	if (AST_LIST_LOCK(headp)) {
		ast_log(LOG_WARNING,"Unable to lock identifiers list\n");
	} else {
		AST_LIST_TRAVERSE(headp,i,entries) {
			if ((i->identifier==identifier) && (i->identifier_type==identifier_type)) {
				found=1;
				res=i->data;
				break;
			}
		}
		if (!found) {
			ast_log(LOG_WARNING,"Identifier %d, identifier_type %d not found in identifier list\n",identifier,identifier_type);
		}
		AST_LIST_UNLOCK(headp);
	}
	
	return res;
}

static int mc_add_identifier(struct ast_channel *chan, int identifier_type,void *data) {
	struct ast_MEMCACHE_id *i,*j;
	struct MEMCACHEidshead *headp;
	int maxidentifier=0;
	
	headp=&_memcache_ids_head;
	i=NULL;
	j=NULL;
	
	if (AST_LIST_LOCK(headp)) {
		ast_log(LOG_WARNING,"Unable to lock identifiers list\n");
		return(-1);
	} else {
		i=malloc(sizeof(struct ast_MEMCACHE_id));
		AST_LIST_TRAVERSE(headp,j,entries) {
			if (j->identifier>maxidentifier) {
				maxidentifier=j->identifier;
			}
		}
		i->identifier=maxidentifier+1;
		i->identifier_type=identifier_type;
		i->data=data;
		i->owner = chan;
		AST_LIST_INSERT_HEAD(headp,i,entries);
		AST_LIST_UNLOCK(headp);
	}
	return i->identifier;
}

static int mc_del_identifier(int identifier,int identifier_type) {
	struct ast_MEMCACHE_id *i;
	struct MEMCACHEidshead *headp;
	int found=0;
	
	headp=&_memcache_ids_head;
	
	if (AST_LIST_LOCK(headp)) {
		ast_log(LOG_WARNING,"Unable to lock identifiers list\n");
	} else {
		AST_LIST_TRAVERSE(headp,i,entries) {
			if ((i->identifier==identifier) &&
				(i->identifier_type==identifier_type)) {
				AST_LIST_REMOVE(headp,i,entries);
				free(i);
				found=1;
				break;
			}
		}
		AST_LIST_UNLOCK(headp);
	}
	if (found==0) {
		ast_log(LOG_WARNING,"Could not find identifier %d, identifier_type %d in list to delete\n",identifier,identifier_type);
		return(-1);
	} else {
		return(0);
	}
}

static int mc_set_asterisk_int(struct ast_channel *chan, char *varname, int id) {
	if( id>=0 ) {
		char s[100] = "";
		snprintf(s, sizeof(s)-1, "%d", id);
		pbx_builtin_setvar_helper(chan,varname,s);
	}
	return id;
}

static int mc_add_identifier_and_set_asterisk_int(struct ast_channel *chan, char *varname, int identifier_type, void *data) {
	return mc_set_asterisk_int(chan, varname, mc_add_identifier(chan, identifier_type, data));
}

static int mc_safe_scan_int( char** data, char* delim, int def ) {
	char* end;
	int res = def;
	char* s = strsep(data,delim);
	if( s ) {
		res = strtol(s,&end,10);
		if (*end) res = def;  /* not an integer */
	}
	return res;
}


//END Store for connid



static int memcache_connect(struct ast_channel *chan, char *data)
{
  struct memcache *mc = mc_new();
  char *connid;
  char *server_host;

  strsep(&data," "); // eat the first token, we already know it :P

  connid = strsep(&data," ");
  server_host = strsep(&data," ");

  if (connid && server_host)
  {
    if (!(mc_server_add4(mc, server_host) == 0))
    {
      ast_log(LOG_WARNING, "MEMCACHE error connecting to %s", server_host);
      return -1;
    }
    else
	{
		mc_add_identifier_and_set_asterisk_int(chan,connid,AST_MEMCACHE_ID_CONNID,mc);
	}
  }
  else
  {
    ast_log(LOG_WARNING, "MEMCACHE missing some argument");
  }

  return -1;
}

static int memcache_disconnect(struct ast_channel *chan, char *data)
{
	int id;
	struct memcache *mc;
	strsep(&data," "); // eat the first token, we already know it :P
	id = mc_safe_scan_int(&data," \n",-1);
	
	if ((mc=mc_find_identifier(id,AST_MEMCACHE_ID_CONNID))==NULL) {
		ast_log(LOG_WARNING,"Invalid connection identifier %d passed in memcache_disconnect\n",id);
	} else {
		mc_server_disconnect_all(mc);
		mc_del_identifier(id,AST_MEMCACHE_ID_CONNID);
	}
	
	return 0;
}


static int memcache_set(struct ast_channel *chan, char *data)
{
	int connid;
	struct memcache *mc;
	char *variable;
	char *value;
	
	strsep(&data," "); // eat the first token, we already know it :P
	
	connid       = mc_safe_scan_int(&data," ",-1);
	variable  = strsep(&data," ");
	value = strsep(&data, "\n");
	
	if (variable && (connid>=0) && value) {
		if ((mc=mc_find_identifier(connid,AST_MEMCACHE_ID_CONNID))!=NULL) {
			mc_add(mc, variable, strlen(variable), value, strlen(value), 0, 0);
		}
		else
		{
			ast_log(LOG_WARNING, "memcache_set not valid conn_id\n");
		}
	}
	else{
		ast_log(LOG_WARNING, "memcache_set requires an arguments (see manual)\n");
	}
	
	return 0;
	
}

static int memcache_replace(struct ast_channel *chan, char *data)
{
	int connid;
	struct memcache *mc;
	char *variable;
	char *value;
	
	strsep(&data," "); // eat the first token, we already know it :P
	
	connid       = mc_safe_scan_int(&data," ",-1);
	variable  = strsep(&data," ");
	value = strsep(&data, "\n");
	
	if (variable && (connid>=0) && value) {
		if ((mc=mc_find_identifier(connid,AST_MEMCACHE_ID_CONNID))!=NULL) {
			mc_replace(mc, variable, strlen(variable), value, strlen(value), 0, 0);
		}
		else
		{
			ast_log(LOG_WARNING, "memcache_replace not valid conn_id\n");
		}
	}
	else{
		ast_log(LOG_WARNING, "memcache_replace requires an arguments (see manual)\n");
	}
	
	return 0;
}




static int memcache_del(struct ast_channel *chan, char *data)
{
	int connid;
	struct memcache *mc;
	char *variable;
	
	strsep(&data," "); // eat the first token, we already know it :P
	
	connid       = mc_safe_scan_int(&data," ",-1);
	variable  = strsep(&data," ");
	
	if (variable && (connid>=0)) {
		if ((mc=mc_find_identifier(connid,AST_MEMCACHE_ID_CONNID))!=NULL) {
			mc_delete(mc, variable, strlen(variable), 0);
		}
		else
		{
			ast_log(LOG_WARNING, "memcache_delete not valid conn_id\n");
		}
	}
	else{
		ast_log(LOG_WARNING, "memcache_delete requires an arguments (see manual)\n");
	}
	
	return 0;
	
}

static int memcache_inc(struct ast_channel *chan, char *data)
{
	int connid;
	struct memcache *mc;
	char *variable;
	int value;
	
	strsep(&data," "); // eat the first token, we already know it :P
	
	connid       = mc_safe_scan_int(&data," ",-1);
	variable  = strsep(&data," ");
	value = mc_safe_scan_int(&data, "\n", -1);
	
	if (variable && (connid>=0) && (value >=0)) {
		if ((mc=mc_find_identifier(connid,AST_MEMCACHE_ID_CONNID))!=NULL) {
			mc_incr(mc, variable, strlen(variable), value);
		}
		else
		{
			ast_log(LOG_WARNING, "memcache_inc not valid conn_id\n");
		}
	}
	else{
		ast_log(LOG_WARNING, "memcache_inc requires an arguments (see manual)\n");
	}
	return 0;	
}

static int memcache_dec(struct ast_channel *chan, char *data)
{
	int connid;
	struct memcache *mc;
	char *variable;
	int value;
	
	strsep(&data," "); // eat the first token, we already know it :P
	
	connid       = mc_safe_scan_int(&data," ",-1);
	variable  = strsep(&data," ");
	value = mc_safe_scan_int(&data, "\n", -1);
	
	if (variable && (connid>=0) && (value >=0)) {
		if ((mc=mc_find_identifier(connid,AST_MEMCACHE_ID_CONNID))!=NULL) {
			mc_decr(mc, variable, strlen(variable), value);
		}
		else
		{
			ast_log(LOG_WARNING, "memcache_dec not valid conn_id\n");
		}
	}
	else{
		ast_log(LOG_WARNING, "memcache_dec requires an arguments (see manual)\n");
	}
	return 0;	
}

static int memcache_get(struct ast_channel *chan, char *data)
{
	int connid;
	struct memcache *mc;
	char *variable;
	char *value_store;
	char *ret;
	
	
	strsep(&data," "); // eat the first token, we already know it :P
	
	connid    = mc_safe_scan_int(&data," ",-1);
	variable  = strsep(&data," ");
	value_store = strsep(&data, "\n");
		
	if (variable && (connid>=0) && value_store) {
		if ((mc=mc_find_identifier(connid,AST_MEMCACHE_ID_CONNID))!=NULL) {
			ret = mc_aget(mc, variable, strlen(variable));
			pbx_builtin_setvar_helper(chan, value_store, ret);
			free(ret);
			free(mc);
		}
		else
		{
			ast_log(LOG_WARNING, "memcache_get not valid conn_id\n");
		}
	}
	else{
		ast_log(LOG_WARNING, "memcache_get requires an arguments (see manual)\n");
	}
	
	return 0;
	
}


static int memcache_exec(struct ast_channel *chan, void *data)
{
  //I don't know about this
  struct ast_module_user *u;
  u = ast_module_user_add(chan);

  int result=0;

  if (!data)
  {
    ast_log(LOG_WARNING, "APP_MEMCACHED requires an argument (see manual)\n");
  }

  ast_mutex_lock(&_memcache_mutex);

  if (strncasecmp("connect",data,strlen("connect"))==0) {
    result = memcache_connect(chan,ast_strdupa(data));
  } else if (strncasecmp("disconnect",data,strlen("disconnect"))==0) {
	result = memcache_disconnect(chan,ast_strdupa(data));
  } else if (strncasecmp("get",data,strlen("get"))==0) {
	result = memcache_get(chan,ast_strdupa(data));
  } else if (strncasecmp("set",data,strlen("set"))==0) {
	result = memcache_set(chan,ast_strdupa(data));
  } else if (strncasecmp("inc",data,strlen("inc"))==0) {
	result = memcache_inc(chan,ast_strdupa(data));
  } else if (strncasecmp("dec",data,strlen("dec"))==0) {
	result = memcache_dec(chan,ast_strdupa(data));
  } else if (strncasecmp("replace",data,strlen("replace"))==0) {
	result = memcache_replace(chan,ast_strdupa(data));
  } else if (strncasecmp("del",data,strlen("del"))==0) {
	result = memcache_del(chan,ast_strdupa(data));
  }else {ast_log(LOG_WARNING, "APP_MEMCACHED not recognizing command\n");}

  ast_mutex_unlock(&_memcache_mutex);

  return 0;
}

static int unload_module(void)
{
	ast_module_user_hangup_all(); 
	return ast_unregister_application(app);
}

static int load_module(void)
{
	return ast_register_application(app, memcache_exec, synopsis, descrip);
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "memcache client");
