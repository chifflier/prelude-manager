/*****
*
* Copyright (C) 2001 Yoann Vandoorselaere <yoann@mandrakesoft.com>
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
#include <sys/time.h>
#include <inttypes.h>

#include <libprelude/common.h>
#include <libprelude/plugin-common.h>
#include <libprelude/plugin-common-prv.h>
#include <libprelude/alert.h>

#include "plugin-decode.h"


static LIST_HEAD(decode_plugins_list);



/*
 *
 */
static int decode_plugin_register(plugin_container_t *pc) 
{        
        log(LOG_INFO, "\tInitialized %s.\n", pc->plugin->name);

        return plugin_register_for_use(pc, &decode_plugins_list, NULL);
}



/*
 *
 */
int decode_plugins_run(int sock, alert_t *alert) 
{
        int ret = -1;
        plugin_decode_t *p;
        struct list_head *tmp;
        plugin_container_t *pc;

        list_for_each(tmp, &decode_plugins_list) {
            
                pc = list_entry(tmp, plugin_container_t, ext_list);
                p = (plugin_decode_t *) pc->plugin;

                if ( p->decode_id != alert->sensor_data_id )
                        continue;
                
                plugin_run_with_return_value(pc, &ret, plugin_decode_t, sock);
                if ( ret < 0 ) {
                        log(LOG_ERR, "%s couldn't decode sensor data.\n", p->name);
                        return -1;
                }

                if ( ret != alert->sensor_data_len ) {
                        log(LOG_ERR, "decoded len (%d) doesn't match sensor data len (%d).\n",
                            ret, alert->sensor_data_len);
                        return -1;
                }
                

                return ret;
        }
        
        log(LOG_ERR, "No decode plugin for handling sensor id %d.\n", alert->sensor_data_id);
        
        return -1;
}



/*
 *
 */
void decode_plugins_init(const char *dirname) 
{
        int ret;

        ret = plugin_load_from_dir(dirname, decode_plugin_register);
        if ( ret < 0 ) 
                log(LOG_ERR, "couldn't load plugin subsystem.\n");

        if ( list_empty(&decode_plugins_list) )
                log(LOG_ERR, "No decode plugin loaded."
                    "You won't be able to get sensor private data.\n");
}


