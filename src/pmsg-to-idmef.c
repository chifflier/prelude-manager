#include <stdio.h>
#include <sys/types.h>
#include <sys/time.h>
#include <netinet/in.h>

#include <libprelude/idmef.h>
#include <libprelude/prelude-log.h>
#include <libprelude/extract.h>
#include <libprelude/idmef-message-id.h>
#include <libprelude/idmef-message-read.h>
#include <libprelude/prelude-ident.h>
#include <libprelude/prelude-client.h>

#include "plugin-decode.h"
#include "pmsg-to-idmef.h"
#include "config.h"


#define MANAGER_MODEL "Prelude Manager"
#define MANAGER_CLASS "Manager"
#define MANAGER_MANUFACTURER "The Prelude Team http://www.prelude-ids.org"



extern prelude_client_t *manager_client;




static int fill_local_analyzer_infos(idmef_analyzer_t *analyzer)
{
        idmef_analyzer_t *next, *local;
        
        if ( ! analyzer )
                return -1;
        
        do {
                next = idmef_analyzer_get_analyzer(analyzer);
                if ( ! next ) {
                        local = idmef_analyzer_ref(prelude_client_get_analyzer(manager_client));
                        idmef_analyzer_set_analyzer(analyzer, local);
                }
                
                analyzer = next;

        } while ( next );

        return 0;
}




static idmef_time_t *get_msg_time(prelude_msg_t *msg)
{
        struct timeval tv;
        idmef_time_t *time;

        prelude_msg_get_time(msg, &tv);
        
        time = idmef_time_new();
        if ( ! time ) {
                log(LOG_ERR, "memory exhausted.\n");
                return NULL;
        }

        idmef_time_set_sec(time, tv.tv_sec);
        idmef_time_set_usec(time, tv.tv_usec);
        
        return time;
}




static int handle_heartbeat_msg(prelude_msg_t *msg, idmef_message_t *idmef)
{
        idmef_heartbeat_t *heartbeat;
        
        heartbeat = idmef_message_new_heartbeat(idmef);
        if ( ! heartbeat )
                return -1;

        if ( ! idmef_heartbeat_read(heartbeat, msg) )
                return -1;

        if ( ! idmef_heartbeat_get_analyzer_time(heartbeat) )
                idmef_heartbeat_set_analyzer_time(heartbeat, get_msg_time(msg));
        
        return fill_local_analyzer_infos(idmef_heartbeat_get_analyzer(heartbeat));
}




static int handle_alert_msg(prelude_msg_t *msg, idmef_message_t *idmef)
{
        idmef_alert_t *alert;
                        
        alert = idmef_message_new_alert(idmef);
        if ( ! alert )
                return -1;
        
        if ( ! idmef_alert_read(alert, msg) )
                return -1;
        
        if ( ! idmef_alert_get_analyzer_time(alert) )
                idmef_alert_set_analyzer_time(alert, get_msg_time(msg));
        
        return fill_local_analyzer_infos(idmef_alert_get_analyzer(alert));
}




static int handle_proprietary_msg(prelude_msg_t *msg, idmef_message_t *idmef, void *buf, uint32_t len)
{
        int ret;
        uint8_t tag;
        
        ret = extract_uint8_safe(&tag, buf, len);
        if ( ret < 0 )
                return -1;
                        
        ret = decode_plugins_run(tag, msg, idmef);
        if ( ret < 0 )
                return -1;

        return 0;
}




idmef_message_t *pmsg_to_idmef(prelude_msg_t *msg) 
{
	int ret;
	void *buf;
	uint8_t tag;
	uint32_t len;
        idmef_message_t *idmef;
        
	idmef = idmef_message_new();
	if ( ! idmef ) {
		log(LOG_ERR, "memory exhausted.\n");
		return NULL;
	}

        while ( (ret = prelude_msg_get(msg, &tag, &len, &buf)) > 0 ) {
                
                if ( tag == MSG_ALERT_TAG ) 
			ret = handle_alert_msg(msg, idmef);

                else if ( tag == MSG_HEARTBEAT_TAG ) 
                        ret = handle_heartbeat_msg(msg, idmef);

                else if ( tag == MSG_OWN_FORMAT )
                        ret = handle_proprietary_msg(msg, idmef, buf, len);

                else log(LOG_ERR, "unknow tag: %d.\n", tag);
                
                if ( ret < 0 )
                        break;
        }
        
        if ( ret == 0 )
                return idmef;
        
        log(LOG_ERR, "error reading IDMEF message.\n");
        idmef_message_destroy(idmef);
                
        return NULL;
}
