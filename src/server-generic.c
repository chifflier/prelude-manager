/*****
*
* Copyright (C) 1999, 2000, 2001, 2002, 2003, 2004, 2005 PreludeIDS Technologies. All Rights Reserved.
* Author: Yoann Vandoorselaere <yoann.v@prelude-ids.com>
*
* This file is part of the Prelude-Manager program.
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
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <sys/poll.h>
#include <assert.h>
#include <sys/stat.h>
#include <signal.h>

#include <libprelude/common.h>
#include <libprelude/prelude-log.h>
#include <libprelude/prelude-io.h>
#include <libprelude/prelude-msg.h>
#include <libprelude/prelude-message-id.h>
#include <libprelude/prelude-client.h>
#include <libprelude/prelude-error.h>

#include <gnutls/gnutls.h>

#include "config.h"
#include "libmissing.h"
#include "manager-auth.h"
#include "server-logic.h"
#include "server-generic.h"


struct server_generic {

        int sock;

        size_t slen;
        struct sockaddr *sa;
        
        size_t clientlen;
        struct server_logic *logic;
        server_generic_read_func_t *read;
        server_generic_write_func_t *write;
        server_generic_close_func_t *close;
        server_generic_accept_func_t *accept;
};


struct server_generic_client {
        SERVER_GENERIC_OBJECT;
};



extern prelude_client_t *manager_client;
static volatile sig_atomic_t continue_processing = 1;





static int send_auth_result(server_generic_client_t *client, int result)
{
        int ret;
        uint64_t nident;
        prelude_client_profile_t *cp;
        
        if ( ! client->msg ) {
                ret = prelude_msg_new(&client->msg, 1, sizeof(uint64_t), PRELUDE_MSG_AUTH, 0);
                if ( ret < 0 )
                        return -1;
                
                cp = prelude_client_get_profile(manager_client);
                nident = prelude_hton64(prelude_client_profile_get_analyzerid(cp));
                prelude_msg_set(client->msg, result, sizeof(nident), &nident);
        }
        
        ret = prelude_msg_write(client->msg, client->fd);
        
        if ( ret < 0 ) {
		if ( prelude_error_get_code(ret) == PRELUDE_ERROR_EAGAIN ) {
			server_logic_notify_write_enable((server_logic_client_t *) client);
			return 0;
		}
                
		prelude_msg_destroy(client->msg);
		return -1;
        }

        prelude_msg_destroy(client->msg);

        client->msg = NULL;
                
        return (client->state & SERVER_GENERIC_CLIENT_STATE_AUTHENTICATED) ? 1 : -1;
}




/*
 * Read the message sent by the Prelude Manager client.
 * This message should contain information about the kind of
 * connection wanted, and the authentication data.
 *
 * Once we finish reading the message, we start the authentication process.
 */
static int authenticate_client(server_generic_t *server, server_generic_client_t *client) 
{
        int ret;
        
        if ( ! client->msg && ! (client->state & SERVER_GENERIC_CLIENT_STATE_AUTHENTICATED) ) {

                ret = manager_auth_client(client, client->fd);                
                if ( ret == 0 )
                        return ret; /* EAGAIN happened */
                
                if ( ret < 0 )
                        return send_auth_result(client, PRELUDE_MSG_AUTH_FAILED);
                
                client->state |= SERVER_GENERIC_CLIENT_STATE_AUTHENTICATED;
                
                ret = send_auth_result(client, PRELUDE_MSG_AUTH_SUCCEED);
                if ( ret != 1 )
                        return ret;
        }

        else if ( client->msg ) {
                ret = send_auth_result(client, -1);
                if ( ret != 1 )
                        return ret;
        }
        
        if ( ! client->state & SERVER_GENERIC_CLIENT_STATE_AUTHENTICATED )
                return -1;
        
        if ( server->sa->sa_family == AF_UNIX && ! (client->state & SERVER_GENERIC_CLIENT_STATE_ACCEPTED) ) {
                ret = manager_auth_disable_encryption(client, client->fd);
                if ( ret <= 0 )
                        return ret;

                server_generic_log_client(client, PRELUDE_LOG_INFO, "disabled encryption on local UNIX connection.\n");
        }

        client->state |= SERVER_GENERIC_CLIENT_STATE_ACCEPTED;
        
        return server->accept(client);
}




static int write_connection_cb(void *sdata, server_logic_client_t *ptr)
{
        server_generic_t *server = sdata;
        server_generic_client_t *client = (server_generic_client_t *) ptr;
        
        if ( client->state & SERVER_GENERIC_CLIENT_STATE_ACCEPTED )
                return server->write(client);
        else {
                server_logic_notify_write_disable(ptr);
                return authenticate_client(sdata, (server_generic_client_t *) ptr);
        }
}




/*
 * callback called by server-logic when data is available for reading.
 * We direct the message either to the authentication process either
 * to the real data handling function.
 *
 * If the authentication function return -1 (error), this will cause
 * server-logic to call the close_connection_cb callback.
 */
static int read_connection_cb(void *sdata, server_logic_client_t *ptr) 
{
        int ret = 0;
        server_generic_t *server = sdata;
        server_generic_client_t *client = (server_generic_client_t *) ptr;
        
        if ( client->state & SERVER_GENERIC_CLIENT_STATE_CLOSING ) {
                /* stop further processing */
                ret = (server_logic_remove_client(ptr) == 0) ? -2 : 0;
        }
        
        else if ( client->state & SERVER_GENERIC_CLIENT_STATE_ACCEPTED )
                ret = server->read(client);
        
        else
                ret = authenticate_client(server, client);
        
        return ret;
}



/*
 * callback called by server-logic when a connection should be closed.
 * if the authentication process succeed for this connection, call
 * the real close() callback function.
 */
static int close_connection_cb(void *sdata, server_logic_client_t *ptr) 
{
        int ret;
        server_generic_t *server = sdata;
        server_generic_client_t *client = (server_generic_client_t *) ptr;

        client->state |= SERVER_GENERIC_CLIENT_STATE_CLOSING;
        
        if ( client->state & SERVER_GENERIC_CLIENT_STATE_ACCEPTED && ! (client->state & SERVER_GENERIC_CLIENT_STATE_CLOSED) ) {
                
                ret = server->close(client);
                if ( ret < 0 )
                        return ret;

                client->state |= SERVER_GENERIC_CLIENT_STATE_CLOSED;
        }
        
        /*
         * layer above server-generic are permited to set fd to NULL so
         * that they can take control over the connection FD.
         */
        if ( client->fd ) {
                
                ret = prelude_io_close(client->fd);
                if ( ret < 0 && prelude_error_get_code(ret) == PRELUDE_ERROR_EAGAIN ) {

                        if ( server->sa->sa_family != AF_UNIX && gnutls_record_get_direction(prelude_io_get_fdptr(client->fd)) == 1 )
                                server_logic_notify_write_enable(ptr);
                        
                        return -1;
                }
                
                prelude_io_destroy(client->fd);
        }
        
        server_generic_log_client(client, PRELUDE_LOG_INFO, "closing connection.\n");

        free(client->permission_string);
        free(client->addr);        
        free(client);
        
        return 0;
}





#ifdef HAVE_TCP_WRAPPERS

#include <tcpd.h>

int allow_severity = LOG_INFO, deny_severity = LOG_NOTICE;


/*
 *
 */
static int tcpd_auth(server_generic_client_t *cdata, int clnt_sock) 
{
        int ret;
        struct request_info request;
        
        request_init(&request, RQ_DAEMON, "prelude-manager", RQ_FILE, clnt_sock, 0);
        
        fromhost(&request);

        ret = hosts_access(&request);
        if ( ! ret ) {
                server_generic_log_client(cdata, PRELUDE_LOG_WARN, "tcp wrapper refused connection.\n", cdata->addr);
                return -1;
        }
        
        return 0;
}

#endif




/*
 * put client socket in non blocking mode and
 * create a prelude_io object for IO abstraction.
 *
 * Tell server-logic to handle event on the newly accepted client.
 */
static int setup_client_socket(server_generic_t *server,
                               server_generic_client_t *cdata, int client) 
{
        int ret;
        
#ifdef HAVE_TCP_WRAPPERS
        if ( server->sa->sa_family != AF_UNIX ) {
                ret = tcpd_auth(cdata, client);
                if ( ret < 0 )
                        return -1;
        }
#endif
        /*
         * set client socket non blocking.
         */
        ret = fcntl(client, F_SETFL, O_NONBLOCK);
        if ( ret < 0 ) {
                prelude_log(PRELUDE_LOG_ERR, "couldn't set non blocking mode for client.\n");
                return -1;
        }

        ret = prelude_io_new(&cdata->fd);
        if ( ret < 0 ) 
                return -1;

        prelude_io_set_sys_io(cdata->fd, client);
               
        cdata->msg = NULL;
        cdata->state = 0;
        
        return 0;
}




static int accept_connection(server_generic_t *server, server_generic_client_t *cdata) 
{
        int sock;
        socklen_t addrlen;

#ifndef HAVE_IPV6
        struct sockaddr_in addr;
#else
        struct sockaddr_in6 addr;
#endif
        
        addrlen = sizeof(addr);
        
        sock = accept(server->sock, (struct sockaddr *) &addr, &addrlen);
        if ( sock < 0 ) {
                prelude_log(PRELUDE_LOG_ERR, "accept returned an error.\n");
                return -1;
        }

        if ( server->sa->sa_family == AF_UNIX )
                cdata->addr = strdup(((struct sockaddr_un *) server->sa)->sun_path);
        else {
                void *in_addr;
                char out[128];
                const char *str;
                struct sockaddr *sa = (struct sockaddr *) &addr;

#ifdef HAVE_IPV6
                cdata->port = ntohs(addr.sin6_port);
#else
		cdata->port = ntohs(addr.sin_port);
#endif
                in_addr = prelude_sockaddr_get_inaddr(sa);
                if ( ! in_addr ) {
                        close(sock);
                        return -1;
                }
                
                str = inet_ntop(sa->sa_family, in_addr, out, sizeof(out));
                if ( str ) {
                        snprintf(out + strlen(out), sizeof(out) - strlen(out), ":%d", cdata->port);
                        cdata->addr = strdup(out);
                }
        }

        if ( ! cdata->addr ) {
                close(sock);
                return -1;
        }

        return sock;
}






static int handle_connection(server_generic_t *server) 
{
        int ret, client;
        server_generic_client_t *cdata;
        
        cdata = calloc(1, server->clientlen);
        if ( ! cdata ) {
                prelude_log(PRELUDE_LOG_ERR, "memory exhausted.\n");
                return -1;
        }

        client = accept_connection(server, cdata);                
        if ( client < 0 ) {
                prelude_log(PRELUDE_LOG_ERR, "couldn't accept connection.\n");
                free(cdata);
                return -1;
        }
                
        ret = setup_client_socket(server, cdata, client);
        if ( ret < 0 ) {
                free(cdata);
                close(client);
                return -1;
        }
        
        ret = server_logic_process_requests(server->logic, (server_logic_client_t *) cdata);
        if ( ret < 0 ) {
                prelude_log(PRELUDE_LOG_ERR, "queueing client FD for server logic processing failed.\n");
                prelude_io_close(cdata->fd);
                prelude_io_destroy(cdata->fd);
                free(cdata->addr);
                free(cdata);
                return -1;
        }

        return 0;
}






/*
 * Wait for client to connect on the Prelude Manager.
 */
static int wait_connection(server_generic_t **server, size_t nserver)
{
        size_t i;
        int active_fd;
        struct pollfd pfd[nserver];
        
        for ( i = 0; i < nserver; i++ ) {                
                pfd[i].events = POLLIN;
                pfd[i].fd = server[i]->sock;
        } 
        
        while ( continue_processing ) {

                active_fd = poll(pfd, nserver, -1);                
                if ( active_fd < 0 )
                        continue;

                for ( i = 0; i < nserver && active_fd > 0; i++ ) {
                        if ( pfd[i].revents & POLLIN ) {
                                active_fd--;
                                handle_connection(server[i]);
                        }
                }
        }

        return 0;
}



/*
 *
 */
static int generic_server(int sock, struct sockaddr *addr, size_t alen) 
{
        int ret;
        
        ret = bind(sock, addr, alen);
        if ( ret < 0 ) {
                prelude_log(PRELUDE_LOG_ERR, "could not bind socket: %s.\n", strerror(errno));
                return -1;
        }
        
        ret = listen(sock, 10);
        if ( ret < 0 ) {
                prelude_log(PRELUDE_LOG_ERR, "could not listen on socket: %s.\n", strerror(errno));
                return -1;
        }
        
        return 0;
}




/*
 * If the UNIX socket already exist, check if it is in use.
 * if it is not, delete it.
 *
 * FIXME: Using connect for this is dirty.
 *
 * return 1 if the socket is already in use.
 * return 0 if the socket is unused.
 * retuir -1 on error.
 */
static int is_unix_socket_already_used(int sock, struct sockaddr_un *sa, int addrlen) 
{
        int ret;
        
        ret = access(sa->sun_path, F_OK);
        if ( ret < 0 )
                return 0;
        
        ret = connect(sock, (struct sockaddr *) sa, addrlen);
        if ( ret == 0 ) {
                prelude_log(PRELUDE_LOG_WARN, "Prelude Manager UNIX socket is already used. Exiting.\n");
                return 1;
        }
        
        /*
         * The unix socket exist on the file system,
         * but no one use it... Delete it.
         */
        ret = unlink(sa->sun_path);
        if ( ret < 0 ) {
                prelude_log(PRELUDE_LOG_ERR, "couldn't delete UNIX socket.\n");
                return -1;
        }
        
        return 0;
}



/*
 *
 */
static int unix_server_start(server_generic_t *server) 
{
        int ret;
        struct sockaddr_un *sa = (struct sockaddr_un *) server->sa;
        
        server->sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if ( server->sock < 0 ) {
                prelude_log(PRELUDE_LOG_ERR, "error creating UNIX socket: %s.\n", strerror(errno));
		return -1;
	}
        
        ret = is_unix_socket_already_used(server->sock, sa, server->slen);
        if ( ret == 1 || ret < 0  ) {
                close(server->sock);
                return -1;
        }

        ret = generic_server(server->sock, server->sa, server->slen);
        if ( ret < 0 ) {
                close(server->sock);
                return -1;
        }

        /*
         * Everyone should be able to access the filesystem object
         * representing our socket.
         */
        ret = chmod(sa->sun_path, S_IRWXU|S_IRWXG|S_IRWXO);
        if ( ret < 0 ) {
                prelude_log(PRELUDE_LOG_ERR, "could not set permission on UNIX socket: %s.\n", strerror(errno));
                return -1;
        }
        
        return 0;
}




/*
 *
 */
static int inet_server_start(server_generic_t *server, struct sockaddr *addr, socklen_t addrlen) 
{
        int ret, on = 1;
        
        server->sock = socket(server->sa->sa_family, SOCK_STREAM, IPPROTO_TCP);
        if ( server->sock < 0 ) {
                prelude_log(PRELUDE_LOG_ERR, "could not create socket: %s.\n", strerror(errno));
                return -1;
        }
        
        ret = setsockopt(server->sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(int));
        if ( ret < 0 ) {
                prelude_log(PRELUDE_LOG_ERR, "could not set SO_REUSEADDR socket option: %s.\n", strerror(errno));
                goto err;
        }

        ret = setsockopt(server->sock, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(int));
        if ( ret < 0 ) {
                prelude_log(PRELUDE_LOG_ERR, "could not set SO_KEEPALIVE socket option: %s.\n", strerror(errno));
                goto err;
        }
        
        ret = generic_server(server->sock, addr, addrlen);
        if ( ret < 0 )
                goto err;

        return 0;

 err:
        close(server->sock);
        return -1;
}



static prelude_bool_t is_unix_addr(const char **out, const char *addr)
{
        int ret;
        const char *ptr;

        if ( ! addr )
                return FALSE;
        
        ret = strncmp(addr, "unix", 4);
        if ( ret != 0 )
                return FALSE;
        
        ptr = strchr(addr, ':');        
        *out = (ptr && *(ptr + 1)) ? ptr + 1 : prelude_connection_get_default_socket_filename();
        
        return TRUE;
}



static int do_getaddrinfo(struct addrinfo **ai, const char *addr, unsigned int port)
{
        int ret;
        struct addrinfo hints;
        char service[sizeof("00000")];

        memset(&hints, 0, sizeof(hints));
        snprintf(service, sizeof(service), "%u", port);
        
        hints.ai_family = PF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        ret = getaddrinfo(addr, service, &hints, ai);
        if ( ret != 0 ) {
                prelude_log(PRELUDE_LOG_WARN, "could not resolve %s: %s.\n",
                            addr, (ret == EAI_SYSTEM) ? strerror(errno) : gai_strerror(ret));
                return -1;
        }

        return 0;
}



static int resolve_addr(server_generic_t *server, const char *addr, unsigned int port) 
{
        struct addrinfo *ai;
        const char *unixpath = NULL;
        int ret, ai_family, ai_addrlen;
        
        if ( is_unix_addr(&unixpath, addr) ) {
                ai_family = AF_UNIX;
                ai_addrlen = sizeof(struct sockaddr_un);
        }

        else {
                ret = do_getaddrinfo(&ai, addr, port);
                if ( ret < 0 )
                        return -1;

                ai_family = ai->ai_family;
                ai_addrlen = ai->ai_addrlen;
        }

        server->sa = malloc(ai_addrlen);
        if ( ! server->sa ) {
                prelude_log(PRELUDE_LOG_ERR, "memory exhausted.\n");
                freeaddrinfo(ai);
                return -1;
        }

        server->slen = ai_addrlen;
        server->sa->sa_family = ai_family;
                
        if ( ai_family != AF_UNIX ) {
                memcpy(server->sa, ai->ai_addr, ai->ai_addrlen);
                freeaddrinfo(ai);
        } else {
                struct sockaddr_un *un = (struct sockaddr_un *) server->sa;
                strncpy(un->sun_path, unixpath, sizeof(un->sun_path));
        }

        return 0;
}




/*
 *
 */
server_generic_t *server_generic_new(size_t clientlen, server_generic_accept_func_t *acceptf,
                                     server_generic_read_func_t *readf,
                                     server_generic_write_func_t *writef,
                                     server_generic_close_func_t *closef)
{
        server_generic_t *server;
                
        server = malloc(sizeof(*server));
        if ( ! server ) {
                prelude_log(PRELUDE_LOG_ERR, "memory exhausted.\n");
                return NULL;
        }

        server->read = readf;
        server->write = writef;
        server->accept = acceptf;
        server->close = closef;
        server->clientlen = clientlen;
        
        server->logic = server_logic_new(server, read_connection_cb, write_connection_cb, close_connection_cb);
        if ( ! server->logic ) {
                prelude_log(PRELUDE_LOG_WARN, "couldn't initialize server pool.\n");
                free(server);
                return NULL;
        }
        
        return server;
}



int server_generic_bind(server_generic_t *server, const char *saddr, unsigned int port)
{
        int ret;
        char out[128];
        void *in_addr;
        
        ret = resolve_addr(server, saddr, port);
        if ( ret < 0 )
                return ret;
        
        if ( server->sa->sa_family == AF_UNIX )
                ret = unix_server_start(server);
        else 
                ret = inet_server_start(server, server->sa, server->slen);
        
        if ( ret < 0 ) {
                server_logic_stop(server->logic);
                free(server->sa);
                free(server);
                return -1;
        }

        if ( server->sa->sa_family == AF_UNIX )
                prelude_log(PRELUDE_LOG_INFO, "- server started (listening on %s).\n",
                            ((struct sockaddr_un *) server->sa)->sun_path);
        else {
                in_addr = prelude_sockaddr_get_inaddr(server->sa);
                assert(in_addr);
                
                inet_ntop(server->sa->sa_family, in_addr, out, sizeof(out));
                prelude_log(PRELUDE_LOG_INFO, "- server started (listening on %s port %u).\n", out, port);
        }
                

        return 0;
}


void server_generic_start(server_generic_t **server, size_t nserver) 
{
        wait_connection(server, nserver);
}




void server_generic_stop(server_generic_t *server)
{
        continue_processing = 0;
}



void server_generic_close(server_generic_t *server) 
{
        close(server->sock);
        
        if ( server->sa->sa_family == AF_UNIX )                 
                unlink(((struct sockaddr_un *)server->sa)->sun_path);
        
        server_logic_stop(server->logic);
}



void server_generic_process_requests(server_generic_t *server, server_generic_client_t *client)
{
        server_logic_process_requests(server->logic, (server_logic_client_t *) client);
}



void server_generic_log_client(server_generic_client_t *cnx, prelude_log_t priority, const char *fmt, ...)
{
        va_list ap;
        int ret = 0;
        char buf[1024];
        
        if ( cnx->ident && cnx->permission_string ) {
                ret = snprintf(buf, sizeof(buf), " 0x%" PRELUDE_PRIx64 " %s]: ", cnx->ident, cnx->permission_string);
                if ( ret < 0 || ret >= sizeof(buf) )
                        return;
        } else {
                ret = snprintf(buf, sizeof(buf), "]: ");
                if ( ret < 0 || ret >= sizeof(buf) )
                        return;
        }
                
        va_start(ap, fmt);
        vsnprintf(buf + ret, sizeof(buf) - ret, fmt, ap);
        va_end(ap);
        
        prelude_log(priority, "[%s%s", cnx->addr, buf);
}



void server_generic_client_set_analyzerid(server_generic_client_t *client, uint64_t analyzerid)
{
        client->ident = analyzerid;
}


void server_generic_client_set_state(server_generic_client_t *client, int state)
{
        client->state = state;
}


int server_generic_client_get_state(server_generic_client_t *client)
{
        return client->state;
}



int server_generic_client_set_permission(server_generic_client_t *client, prelude_connection_permission_t permission)
{
        int ret;
        prelude_string_t *out;

        ret = prelude_string_new(&out);
        if ( ret < 0 )
                return ret;

        ret = prelude_connection_permission_to_string(permission, out);
        if ( ret < 0 ) {
                prelude_string_destroy(out);
                return ret;
        }

        ret = prelude_string_get_string_released(out, &client->permission_string);
        prelude_string_destroy(out);
        
        if ( ret < 0 )
                return ret;

        client->permission = permission;

        return 0;
}
