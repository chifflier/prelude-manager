/*****
*
* Copyright (C) 2000, 2002 Yoann Vandoorselaere <yoann@mandrakesoft.com>
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

#ifndef _MANAGER_PCONFIG_H
#define _MANAGER_PCONFIG_H

int pconfig_init(int argc, char **argv);

struct report_config {
	char *addr;
        unsigned int port;

	char *cm_comm_server_addr;
	unsigned int cm_comm_server_port;

        const char *pidfile;
        int use_ssl;
}; 


/*
 * FIXME: this has nothing to do here.
 */
void manager_relay_msg_if_needed(prelude_msg_t *msg);

#endif /* _MANAGER_PCONFIG_H */
