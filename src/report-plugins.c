/*****
*
* Copyright (C) 1998-2004 Yoann Vandoorselaere <yoann@prelude-ids.org>
* All Rights Reserved
*
* This file is part of the Prelude program.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by 
* the Free Software Foundation; either version 2, or (at your option)
* any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; see the file COPYING.  If not, write to
* the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
*
*****/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>

#include <libprelude/prelude-inttypes.h>
#include <libprelude/prelude-message-buffered.h>
#include <libprelude/idmef.h>
#include <libprelude/idmef-message-write.h>
#include <libprelude/prelude-linked-object.h>
#include <libprelude/prelude-log.h>
#include <libprelude/timer.h>
#include <libprelude/common.h>
#include <libprelude/prelude-failover.h>

#include "plugin-report.h"
#include "plugin-filter.h"
#include "pmsg-to-idmef.h"


#define FAILOVER_RETRY_TIMEOUT 10 * 60


static prelude_msgbuf_t *msgbuf;
static PRELUDE_LIST_HEAD(report_plugins_instance);


typedef struct {
        int failover_enabled;
        prelude_timer_t timer;
        prelude_failover_t *failover;
} plugin_failover_t;



static void get_failover_filename(prelude_plugin_instance_t *pi, char *buf, size_t size)
{
        prelude_plugin_generic_t *plugin = prelude_plugin_instance_get_plugin(pi);
        
        snprintf(buf, size, MANAGER_FIFO_DIR "/%s[%s]",
                 plugin->name, prelude_plugin_instance_get_name(pi));
}



static int recover_from_failover(prelude_plugin_instance_t *pi, plugin_failover_t *pf, size_t *totsize)
{
        ssize_t size;
        int ret, count = 0;
        idmef_message_t *idmef;
        prelude_msg_t *msg = NULL;

        *totsize = 0;
        
        do {
                size = prelude_failover_get_saved_msg(pf->failover, &msg);
                if ( size <= 0 )
                        break;
                
                *totsize += size;

                idmef = pmsg_to_idmef(msg);
                if ( ! idmef )
                        break;
                 
                ret = prelude_plugin_run(pi, plugin_report_t, run, pi, idmef);
                if ( ret < 0 && pf ) 
                        break;
 
                prelude_msg_destroy(msg);

                count++;
                
        } while ( 1 );

        return count;
}




static int try_recovering_from_failover(prelude_plugin_instance_t *pi, plugin_failover_t *pf)
{
        int ret;
        size_t totsize;
        const char *text;
        prelude_plugin_generic_t *plugin;
        unsigned int available, count = 0;
        
        ret = prelude_plugin_instance_call_commit_func(pi);
        if ( ret < 0 )
                return -1;
        
        available = prelude_failover_get_available_msg_count(pf->failover);
        if ( ! available )
                return 0;
        
        plugin = prelude_plugin_instance_get_plugin(pi);
        
        log(LOG_INFO, "- Plugin %s[%s]: flushing %u message (%u erased due to quota)...\n",
            plugin->name,prelude_plugin_instance_get_name(pi),
            available, prelude_failover_get_deleted_msg_count(pf->failover));
         
        count = recover_from_failover(pi, pf, &totsize);

        if ( count != available ) 
                text = "failed recovering";
        else {
                text = "recovered";
                pf->failover_enabled = 0;
        }

        log(LOG_INFO, "- Plugin %s[%s]: %s from failover: %u/%u message flushed (%u bytes).\n",
            plugin->name, prelude_plugin_instance_get_name(pi), text, count, available, totsize);

        return (count == available) ? 0 : -1;
}




static void failover_timer_expire_cb(void *data)
{
        int ret;
        plugin_failover_t *pf;
        prelude_plugin_instance_t *pi = data;
        
        pf = prelude_plugin_instance_get_private_data(pi);
        
        ret = try_recovering_from_failover(pi, pf);
        if ( ret < 0 )
                timer_reset(&pf->timer);
        else
                timer_destroy(&pf->timer);
}



static int setup_plugin_failover(prelude_plugin_instance_t *pi)
{
        char filename[256];
        plugin_failover_t *pf;
        prelude_plugin_generic_t *plugin = prelude_plugin_instance_get_plugin(pi);

        get_failover_filename(pi, filename, sizeof(filename));
        
        if ( ! prelude_plugin_instance_has_commit_func(pi) ) {
                log(LOG_ERR, "plugin %s doesn't support failover.\n", plugin->name);
                return -1;
        }

        pf = calloc(1, sizeof(*pf));
        if ( ! pf ) {
                log(LOG_ERR, "memory exhausted.\n");
                return -1;
        }
        
        pf->failover = prelude_failover_new(filename);
        if ( ! pf->failover ) {
                free(pf);
                return -1;
        }
        
        prelude_plugin_instance_set_private_data(pi, pf);
        
        try_recovering_from_failover(pi, pf);
        if ( pf->failover_enabled ) {
                prelude_failover_destroy(pf->failover);
                free(pf);
                return -1;
        }
        
        return 0;
}



/*
 *
 */
static int subscribe(prelude_plugin_instance_t *pi) 
{
        prelude_plugin_generic_t *plugin = prelude_plugin_instance_get_plugin(pi);
        
        log(LOG_INFO, "- Subscribing %s[%s] to active reporting plugins.\n",
            plugin->name, prelude_plugin_instance_get_name(pi));

        prelude_plugin_add(pi, &report_plugins_instance, NULL);

        return 0;
}


static void unsubscribe(prelude_plugin_instance_t *pi) 
{
        prelude_plugin_generic_t *plugin = prelude_plugin_instance_get_plugin(pi);
        
        log(LOG_INFO, "- Un-subscribing %s[%s] from active reporting plugins.\n",
            plugin->name, prelude_plugin_instance_get_name(pi));

        prelude_plugin_del(pi);
}



static void failover_init(prelude_plugin_generic_t *pg, prelude_plugin_instance_t *pi, plugin_failover_t *pf)
{
        pf->failover_enabled = 1;
                        
        log(LOG_INFO, "- Plugin %s[%s]: failure. Enabling failover.\n", pg->name, prelude_plugin_instance_get_name(pi));

        timer_set_data(&pf->timer, pi);
        timer_set_expire(&pf->timer, FAILOVER_RETRY_TIMEOUT);
        timer_set_callback(&pf->timer, failover_timer_expire_cb);

        timer_init(&pf->timer);
}




static prelude_msg_t *save_msgbuf(prelude_msgbuf_t *msgbuf)
{
        prelude_msg_t *msg = prelude_msgbuf_get_msg(msgbuf);
        plugin_failover_t *pf = prelude_msgbuf_get_data(msgbuf);
        
        prelude_failover_save_msg(pf->failover, msg);
        prelude_msg_recycle(msg);

        return msg;
}





static void save_idmef_message(plugin_failover_t *pf, idmef_message_t *msg)
{
        prelude_msg_t *pmsg;

        pmsg = idmef_message_get_pmsg(msg);
        if ( pmsg ) {
                prelude_failover_save_msg(pf->failover, pmsg);
                return;
        }

        /*
         * this a message we generated ourself...
         */
        prelude_msgbuf_set_data(msgbuf, pf);
        idmef_write_message(msgbuf, msg);
        prelude_msgbuf_mark_end(msgbuf);
}




/*
 * Start all plugins of kind 'list'.
 */
void report_plugins_run(idmef_message_t *idmef)
{
        int ret;
        prelude_list_t *tmp;
        plugin_failover_t *pf;
        prelude_plugin_generic_t *pg;
        prelude_plugin_instance_t *pi;
        
        ret = filter_plugins_run_by_category(idmef, FILTER_CATEGORY_REPORTING);
        if ( ret < 0 ) 
                return;
        
        prelude_list_for_each(tmp, &report_plugins_instance) {

                pi = prelude_linked_object_get_object(tmp, prelude_plugin_instance_t);
                pg = prelude_plugin_instance_get_plugin(pi);
                pf = prelude_plugin_instance_get_private_data(pi);
                
                ret = filter_plugins_run_by_plugin(idmef, pi);
                if ( ret < 0 ) 
                        continue;

                if ( pf && pf->failover_enabled ) {
                        save_idmef_message(pf, idmef);
                        continue;
                }
                                        
                ret = prelude_plugin_run(pi, plugin_report_t, run, pi, idmef);
                if ( ret < 0 && pf ) {
                        failover_init(pg, pi, pf);
                        save_idmef_message(pf, idmef);
                }
        }
}




/*
 * Close all report plugins.
 */
void report_plugins_close(void)
{
        prelude_list_t *tmp;
        plugin_report_t *plugin;
        prelude_plugin_instance_t *pi;
                
        prelude_list_for_each(tmp, &report_plugins_instance) {
                pi = prelude_linked_object_get_object(tmp, prelude_plugin_instance_t);
                plugin = (plugin_report_t *) prelude_plugin_instance_get_plugin(pi);
                
                if ( plugin->close )
                        plugin->close(pi);
        }
}



/*
 * Open the plugin directory (dirname),
 * and try to load all plugins located in it.
 */
int report_plugins_init(const char *dirname, int argc, char **argv)
{
        int ret;
        
	ret = access(dirname, F_OK);
	if ( ret < 0 ) {
		if ( errno == ENOENT )
			return 0;
		log(LOG_ERR, "can't access %s.\n", dirname);
		return -1;
	}

        ret = prelude_plugin_load_from_dir(dirname, subscribe, unsubscribe);

        /*
         * don't return an error if the report directory doesn't exist.
         * this could happen as it's normal to not use report plugins on
         * certain system.
         */
        if ( ret < 0 && errno != ENOENT ) {
                log(LOG_ERR, "couldn't load plugin subsystem.\n");
                return -1;
        }

        msgbuf = prelude_msgbuf_new(NULL);
        if ( ! msgbuf )
                return -1;

        prelude_msgbuf_set_callback(msgbuf, save_msgbuf);
                
        return ret;
}




/**
 * report_plugins_available:
 *
 * Returns: 0 if there is active REPORT plugins, -1 otherwise.
 */
int report_plugins_available(void) 
{
        return prelude_list_empty(&report_plugins_instance) ? -1 : 0;
}



int report_plugin_activate_failover(const char *plugin)
{
        int ret;
        plugin_failover_t *pf;
        char pname[256], iname[256];
        prelude_plugin_instance_t *pi;
        
        ret = sscanf(plugin, "%255[^[][%255[^]]", pname, iname);

        pi = prelude_plugin_search_instance_by_name(pname, (ret == 2) ? iname : NULL);
        if ( ! pi ) {
                log(LOG_ERR, "couldn't find plugin %s.\n", plugin);
                return -1;
        }

        pf = calloc(1, sizeof(*pf));
        if ( ! pf ) {
                log(LOG_ERR, "memory exhausted.\n");
                return -1;
        }

        return setup_plugin_failover(pi);
}
