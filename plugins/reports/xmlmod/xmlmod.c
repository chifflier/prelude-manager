/*****
*
* Copyright (C) 2002, 2004 Yoann Vandoorselaere <yoann@mandrakesoft.com>
*
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

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include <libprelude/prelude.h>
#include <libprelude/idmef-util.h>

#include <libxml/parser.h>

#include "libmissing.h"
#include "report.h"


prelude_plugin_generic_t *xmlmod_LTX_prelude_plugin_init(void);


static void process_file(xmlNodePtr parent, idmef_file_t *file);


typedef struct {
        int format;
        int no_buffering;
        char *logfile;
        xmlDtdPtr idmef_dtd;
        xmlOutputBufferPtr fd;
} xmlmod_plugin_t;


static plugin_report_t xmlmod_plugin;
extern prelude_option_t *manager_root_optlist;


PRELUDE_PLUGIN_OPTION_DECLARE_STRING_CB(xmlmod, xmlmod_plugin_t, logfile)



static int file_write(void *context, const char *buf, int len) 
{
        return fwrite(buf, 1, len, context);
}



#define idmef_attr_generic_optional(node, attr, fmt, ptr) \
do { \
        char buf[512]; \
        if ( ptr ) { \
               snprintf(buf, sizeof(buf), fmt, *ptr); \
               xmlSetProp(node, attr, buf); \
        } \
} while (0)


#define idmef_attr_generic(node, attr, fmt, value) \
do { \
        char buf[512]; \
        if ( value ) { \
               snprintf(buf, sizeof(buf), fmt, value); \
               xmlSetProp(node, attr, buf); \
        } \
} while (0) 


#define idmef_content_generic_optional(node, tag, fmt, ptr) \
do { \
        char buf[512]; \
        if ( ptr ) { \
               snprintf(buf, sizeof(buf), fmt, *ptr); \
               xmlNewChild(node, NULL, tag, buf); \
        } \
} while (0) 

 
static void idmef_attr_uint64(xmlNodePtr node, const char *attr, uint64_t ident) 
{
        char buf[64];

        if ( ! ident )
                return;
        
        snprintf(buf, sizeof(buf), "%" PRIu64, ident);
        xmlSetProp(node, attr, buf);
}



static void idmef_content_string(xmlNodePtr node, const char *tag, prelude_string_t *string) 
{
	const char *content;

        if ( ! string )
                return;

	content = prelude_string_get_string(string);

        xmlNewChild(node, NULL, tag, content ? content : "");
}



static void idmef_attr_string(xmlNodePtr node, const char *attr, prelude_string_t *string) 
{
	const char *content;

        if ( ! string )
                return;

	content = prelude_string_get_string(string);

        xmlSetProp(node, attr, content ? content : "");
}



static void _idmef_attr_enum(xmlNodePtr node, const char *attr, int value, const char *(*convert)(int))
{
	xmlSetProp(node, attr, convert(value));
}

#define idmef_attr_enum(node, attr, value, convert) \
	_idmef_attr_enum(node, attr, value, (const char *(*)(int)) convert)



static void _idmef_attr_enum_optional(xmlNodePtr node, const char *attr, int *value, const char *(*convert)(int))
{
	if ( ! value )
		return;

	idmef_attr_enum(node, attr, *value, convert);
}

#define idmef_attr_enum_optional(node, attr, value, convert) \
	_idmef_attr_enum_optional(node, attr, value, (const char *(*)(int)) convert)



static void process_time(xmlNodePtr parent, const char *type, idmef_time_t *time) 
{
        xmlNodePtr new;
        char time_idmef[IDMEF_TIME_MAX_STRING_SIZE];
	char ntpstamp[IDMEF_TIME_MAX_NTPSTAMP_SIZE];

        if ( ! time )
                return;

        idmef_time_to_string(time, time_idmef, sizeof(time_idmef));
        idmef_time_to_ntpstamp(time, ntpstamp, sizeof(ntpstamp));

        new = xmlNewChild(parent, NULL, type, time_idmef);
        if ( ! new )
                return;

        xmlSetProp(new, "ntpstamp", ntpstamp);
}



static void process_address(xmlNodePtr parent, idmef_address_t *address) 
{
        xmlNodePtr new;

        if ( ! address )
                return;
        
        new = xmlNewChild(parent, NULL, "Address", NULL);
        if ( ! new )
                return;

        idmef_attr_uint64(new, "ident", idmef_address_get_ident(address));
        idmef_attr_enum(new, "category", idmef_address_get_category(address), idmef_address_category_to_string);
        idmef_attr_string(new, "vlan-name", idmef_address_get_vlan_name(address));
        
        idmef_attr_generic_optional(new, "vlan-num", "%d", idmef_address_get_vlan_num(address));
        
        idmef_content_string(new, "address", idmef_address_get_address(address));
        idmef_content_string(new, "netmask", idmef_address_get_netmask(address));
}




static void process_node(xmlNodePtr parent, idmef_node_t *node) 
{
        xmlNodePtr new;
        idmef_address_t *address;
        
        if ( ! node )
                return;

        new = xmlNewChild(parent, NULL, "Node", NULL);
        if ( ! new )
                return;
        
        idmef_attr_uint64(new, "ident", idmef_node_get_ident(node));
        idmef_attr_enum(new, "category", idmef_node_get_category(node), idmef_node_category_to_string);
        idmef_content_string(new, "name", idmef_node_get_name(node));
        idmef_content_string(new, "location", idmef_node_get_location(node));

	address = NULL;
	while ( (address = idmef_node_get_next_address(node, address)) )
		process_address(new, address);
}



static void process_user_id(xmlNodePtr parent, idmef_user_id_t *user_id) 
{
        xmlNodePtr new;

        if ( ! user_id )
                return;
        
        new = xmlNewChild(parent, NULL, "UserId", NULL);
        if ( ! new )
                return;

        idmef_attr_uint64(new, "ident", idmef_user_id_get_ident(user_id));
        idmef_attr_enum(new, "type", idmef_user_id_get_type(user_id), idmef_user_id_type_to_string);
        idmef_content_string(new, "name", idmef_user_id_get_name(user_id));
        idmef_content_generic_optional(new, "number", "%" PRIu32, idmef_user_id_get_number(user_id));
}



static void process_user(xmlNodePtr parent, idmef_user_t *user) 
{
        xmlNodePtr new;
        idmef_user_id_t *user_id;

        if ( ! user )
                return;

        new = xmlNewChild(parent, NULL, "User", NULL);
        if ( ! new )
                return;

        idmef_attr_uint64(new, "ident", idmef_user_get_ident(user));
        idmef_attr_enum(new, "category", idmef_user_get_category(user), idmef_user_category_to_string);

	user_id = NULL;
	while ( (user_id = idmef_user_get_next_user_id(user, user_id)) )
		process_user_id(new, user_id);
}




static void process_process(xmlNodePtr parent, idmef_process_t *process)
{
        xmlNodePtr new;
	prelude_string_t *string;

        if ( ! process )
                return;

        new = xmlNewChild(parent, NULL, "Process", NULL);
        if ( ! new )
                return;

        idmef_attr_uint64(new, "ident", idmef_process_get_ident(process));
        idmef_content_string(new, "name", idmef_process_get_name(process));
        idmef_content_generic_optional(new, "pid", "%" PRIu32, idmef_process_get_pid(process));
        idmef_content_string(new, "path", idmef_process_get_path(process));

	string = NULL;
	while ( (string = idmef_process_get_next_arg(process, string)) )
		xmlNewChild(new, NULL, "arg", prelude_string_get_string(string));

	string = NULL;
	while ( (string = idmef_process_get_next_env(process, string)) )
		xmlNewChild(new, NULL, "env", prelude_string_get_string(string));
}




static void process_snmp_service(xmlNodePtr parent, idmef_snmp_service_t *snmp) 
{
        xmlNodePtr new;

        if ( ! snmp )
                return;

        new = xmlNewChild(parent, NULL, "SNMPService", NULL);
        if ( ! new )
                return;

        idmef_content_string(new, "oid", idmef_snmp_service_get_oid(snmp));
        idmef_content_string(new, "community", idmef_snmp_service_get_community(snmp));
        idmef_content_string(new, "security_name", idmef_snmp_service_get_security_name(snmp));
        idmef_content_string(new, "context_name", idmef_snmp_service_get_context_name(snmp));
        idmef_content_string(new, "context_engine_id", idmef_snmp_service_get_context_engine_id(snmp));
        idmef_content_string(new, "command", idmef_snmp_service_get_command(snmp));
}




static void process_web_service(xmlNodePtr parent, idmef_web_service_t *web) 
{
        xmlNodePtr new;
	prelude_string_t *arg;

        if ( ! web )
                return;

        new = xmlNewChild(parent, NULL, "WebService", NULL);

        idmef_content_string(new, "url", idmef_web_service_get_url(web));
        idmef_content_string(new, "cgi", idmef_web_service_get_cgi(web));
        idmef_content_string(new, "http-method", idmef_web_service_get_http_method(web));

	arg = NULL;
	while ( (arg = idmef_web_service_get_next_arg(web, arg)) )
		xmlNewChild(new, NULL, "arg", prelude_string_get_string(arg));
}



static void process_service(xmlNodePtr parent, idmef_service_t *service) 
{
        xmlNodePtr new;

        if ( ! service )
                return;

        new = xmlNewChild(parent, NULL, "Service", NULL);
        if ( ! new )
                return;

        idmef_attr_uint64(new, "ident", idmef_service_get_ident(service));
	idmef_content_generic_optional(new, "ip_version", "%" PRIu8, idmef_service_get_ip_version(service));
        idmef_content_string(new, "name", idmef_service_get_name(service));
        idmef_content_generic_optional(new, "port", "%" PRIu16, idmef_service_get_port(service));
	idmef_content_generic_optional(new, "iana_protocol_number", "%" PRIu8, idmef_service_get_iana_protocol_number(service));
	idmef_content_string(new, "iana_protocol_name", idmef_service_get_iana_protocol_name(service));
        idmef_content_string(new, "portlist", idmef_service_get_portlist(service));
        idmef_content_string(new, "protocol", idmef_service_get_protocol(service));

        switch ( idmef_service_get_type(service) ) {

        case IDMEF_SERVICE_TYPE_SNMP:
                process_snmp_service(new, idmef_service_get_snmp_service(service));
                break;

        case IDMEF_SERVICE_TYPE_WEB:
                process_web_service(new, idmef_service_get_web_service(service));
                break;

        default:
                break;
        }
}



static void process_source(xmlNodePtr parent, idmef_source_t *source)
{
        xmlNodePtr new;

        if ( ! source )
                return;
        
        new = xmlNewChild(parent, NULL, "Source", NULL);
        if ( ! new )
                return;
        
        idmef_attr_uint64(new, "ident", idmef_source_get_ident(source));
        idmef_attr_enum(new, "spoofed", idmef_source_get_spoofed(source), idmef_source_spoofed_to_string);
        idmef_attr_string(new, "interface", idmef_source_get_interface(source));
        
        process_node(new, idmef_source_get_node(source));
        process_user(new, idmef_source_get_user(source));
        process_process(new, idmef_source_get_process(source));
        process_service(new, idmef_source_get_service(source));
}



static void process_file_access(xmlNodePtr parent, idmef_file_access_t *file_access)
{
        xmlNodePtr new;
	prelude_string_t *permission;

        if ( ! file_access )
                return;
        
	new = xmlNewChild(parent, NULL, "FileAccess", NULL);
	if ( ! new )
		return;

	process_user_id(new, idmef_file_access_get_user_id(file_access));

	permission = NULL;
	while ( (permission = idmef_file_access_get_next_permission(file_access, permission)) )
		xmlNewChild(new, NULL, "permission", prelude_string_get_string(permission));
}



static void process_file_linkage(xmlNodePtr parent, idmef_linkage_t *linkage) 
{
        xmlNodePtr new;

        if ( ! linkage )
                return;
        
	new = xmlNewChild(parent, NULL, "Linkage", NULL);
	if ( ! new )
		return;

	idmef_attr_enum(new, "category", idmef_linkage_get_category(linkage), idmef_linkage_category_to_string);
	idmef_content_string(new, "name", idmef_linkage_get_name(linkage));
	idmef_content_string(new, "path", idmef_linkage_get_path(linkage));

	process_file(new, idmef_linkage_get_file(linkage));
}




static void process_inode(xmlNodePtr parent, idmef_inode_t *inode) 
{
        xmlNodePtr new;

        if ( ! inode )
                return;

        new = xmlNewChild(parent, NULL, "Inode", NULL);
        if ( ! new )
                return;

        process_time(new, "change-time", idmef_inode_get_change_time(inode));

        idmef_content_generic_optional(new, "number", "%" PRIu32, idmef_inode_get_number(inode));
        idmef_content_generic_optional(new, "major-device", "%" PRIu32,  idmef_inode_get_major_device(inode));
        idmef_content_generic_optional(new, "minor-device", "%" PRIu32, idmef_inode_get_minor_device(inode));
        idmef_content_generic_optional(new, "c-major-device", "%" PRIu32, idmef_inode_get_c_major_device(inode));
        idmef_content_generic_optional(new, "c-minor-devide", "%" PRIu32, idmef_inode_get_c_minor_device(inode));
}




static void process_file(xmlNodePtr parent, idmef_file_t *file) 
{
        xmlNodePtr new;
	idmef_linkage_t *file_linkage;
	idmef_file_access_t *file_access;

        if ( ! file )
                return;
        
        new = xmlNewChild(parent, NULL, "File", NULL);
        if ( ! new )
                return;

        idmef_attr_uint64(new, "ident", idmef_file_get_ident(file));
        idmef_attr_enum(new, "category", idmef_file_get_category(file), idmef_file_category_to_string);
	idmef_attr_enum_optional(new, "fstype", idmef_file_get_fstype(file), idmef_file_fstype_to_string);
        idmef_content_string(new, "name", idmef_file_get_name(file));
        idmef_content_string(new, "path", idmef_file_get_path(file));
        process_time(new, "create-time", idmef_file_get_create_time(file));
        process_time(new, "modify-time", idmef_file_get_modify_time(file));
        process_time(new, "access-time", idmef_file_get_access_time(file));
        idmef_content_generic_optional(new, "data-size", "%" PRIu64, idmef_file_get_data_size(file));
        idmef_content_generic_optional(new, "disk-size", "%" PRIu64, idmef_file_get_disk_size(file));

	file_access = NULL;
	while ( (file_access = idmef_file_get_next_file_access(file, file_access)) )
		process_file_access(new, file_access);

	file_linkage = NULL;
	while ( (file_linkage = idmef_file_get_next_linkage(file, file_linkage)) )
		process_file_linkage(new, file_linkage);

        process_inode(new, idmef_file_get_inode(file));
}




static void process_target(xmlNodePtr parent, idmef_target_t *target)
{
        xmlNodePtr new;
        idmef_file_t *file;

        if ( ! target )
                return;
        
        new = xmlNewChild(parent, NULL, "Target", NULL);
        if ( ! new )
                return;

        idmef_attr_uint64(new, "ident", idmef_target_get_ident(target));
        idmef_attr_enum(new, "decoy", idmef_target_get_decoy(target), idmef_target_decoy_to_string);
        idmef_attr_string(new, "interface", idmef_target_get_interface(target));

        process_node(new, idmef_target_get_node(target));
        process_user(new, idmef_target_get_user(target));
        process_process(new, idmef_target_get_process(target));
        process_service(new, idmef_target_get_service(target));

	file = NULL;
	while ( (file = idmef_target_get_next_file(target, file)) )
                process_file(new, file);
}



static void process_analyzer(xmlNodePtr parent, idmef_analyzer_t *analyzer) 
{
        xmlNodePtr new;

        if ( ! analyzer )
                return;

        new = xmlNewChild(parent, NULL, "Analyzer", NULL);
        if ( ! new )
                return;
        
        idmef_attr_uint64(new, "analyzerid", idmef_analyzer_get_analyzerid(analyzer));
        idmef_attr_string(new, "name", idmef_analyzer_get_name(analyzer));
        idmef_attr_string(new, "manufacturer", idmef_analyzer_get_manufacturer(analyzer));
        idmef_attr_string(new, "model", idmef_analyzer_get_model(analyzer));
        idmef_attr_string(new, "version", idmef_analyzer_get_version(analyzer));
        idmef_attr_string(new, "class", idmef_analyzer_get_class(analyzer));
        idmef_attr_string(new, "ostype", idmef_analyzer_get_ostype(analyzer));
        idmef_attr_string(new, "osversion", idmef_analyzer_get_osversion(analyzer));

        process_node(new, idmef_analyzer_get_node(analyzer));
        process_process(new, idmef_analyzer_get_process(analyzer));

        process_analyzer(new, idmef_analyzer_get_analyzer(analyzer));
}



static void process_reference(xmlNodePtr parent, idmef_reference_t *reference)
{
        xmlNodePtr new;
        
        new = xmlNewChild(parent, NULL, "Reference", NULL);
        if ( ! new )
                return;

        idmef_attr_enum(new, "origin", idmef_reference_get_origin(reference), idmef_reference_origin_to_string);
        
        idmef_content_string(new, "name", idmef_reference_get_name(reference));

        idmef_content_string(new, "url", idmef_reference_get_url(reference));
}



static void process_classification(xmlNodePtr parent, idmef_classification_t *classification) 
{
        xmlNodePtr new;
        idmef_reference_t *reference;
        
        if ( ! classification )
                return;
        
        new = xmlNewChild(parent, NULL, "Classification", NULL);
        if ( ! new )
                return;

        idmef_attr_generic(new, "ident", "%" PRIu64, idmef_classification_get_ident(classification));
        
        idmef_attr_string(new, "text", idmef_classification_get_text(classification));

        reference = NULL;
        while ( (reference = idmef_classification_get_next_reference(classification, reference)) )
                process_reference(new, reference);
}



static void process_additional_data(xmlNodePtr parent, idmef_additional_data_t *ad) 
{
        size_t dlen;
        xmlNodePtr new;
        const char *tmp;
        char buf[128];

        if ( ! ad )
                return;
        
        dlen = sizeof(buf);
        
	tmp = idmef_additional_data_data_to_string(ad, buf, &dlen);
	if ( ! tmp )
		return;

        new = xmlNewChild(parent, NULL, "AdditionalData", tmp);
        if ( ! new )
                return;

        idmef_attr_enum(new, "type", idmef_additional_data_get_type(ad), idmef_additional_data_type_to_string);
        idmef_attr_string(new, "meaning", idmef_additional_data_get_meaning(ad));
}




static void process_impact(xmlNodePtr parent, idmef_impact_t *impact) 
{
        xmlNodePtr new;
        
        if ( ! impact )
                return;

        new = xmlNewChild(parent, NULL, "Impact",
			  prelude_string_get_string(idmef_impact_get_description(impact)));
        if ( ! new )
                return;

	idmef_attr_enum_optional(new, "severity", idmef_impact_get_severity(impact), idmef_impact_severity_to_string);

	idmef_attr_enum_optional(new, "completion", idmef_impact_get_completion(impact), idmef_impact_completion_to_string);
        
        idmef_attr_enum(new, "type", idmef_impact_get_type(impact), idmef_impact_type_to_string);
}



static void process_confidence(xmlNodePtr parent, idmef_confidence_t *confidence) 
{
        char buf[64];
        xmlNodePtr new;

        if ( ! confidence )
                return;

        if ( idmef_confidence_get_rating(confidence) == IDMEF_CONFIDENCE_RATING_NUMERIC &&
	     idmef_confidence_get_confidence(confidence) ) {
                snprintf(buf, sizeof(buf), "%f", * idmef_confidence_get_confidence(confidence));
                new = xmlNewChild(parent, NULL, "Confidence", buf);
        } else
                new = xmlNewChild(parent, NULL, "Confidence", NULL);

        if ( ! new )
                return;

        idmef_attr_enum(new, "rating", idmef_confidence_get_rating(confidence), idmef_confidence_rating_to_string);
}




static void process_action(xmlNodePtr parent, idmef_action_t *action) 
{
        xmlNodePtr new;

        if ( ! action )
                return;

	if ( idmef_action_get_description(action) )
		new = xmlNewChild(parent, NULL, "Action",
				  prelude_string_get_string(idmef_action_get_description(action)));
	else
		new = xmlNewChild(parent, NULL, "Action", NULL);
	
        if ( ! new )
                return;

        idmef_attr_enum(new, "category", idmef_action_get_category(action), idmef_action_category_to_string);
}




static void process_assessment(xmlNodePtr parent, idmef_assessment_t *assessment) 
{
        xmlNodePtr new;
        idmef_action_t *action;

        if ( ! assessment )
                return;

        new = xmlNewChild(parent, NULL, "Assessment", NULL);
        if ( ! new )
                return;

        process_impact(new, idmef_assessment_get_impact(assessment));

	action = NULL;
	while ( (action = idmef_assessment_get_next_action(assessment, action)) )
                process_action(new, action);

        process_confidence(new, idmef_assessment_get_confidence(assessment));
}





static void process_alert(xmlNodePtr root, idmef_alert_t *alert) 
{
        xmlNodePtr new;
        idmef_source_t *source;
        idmef_target_t *target;
        idmef_additional_data_t *additional_data;

        if ( ! alert )
                return;
        
        new = xmlNewChild(root, NULL, "Alert", NULL);
        if ( ! new )
                return;
        
        idmef_attr_uint64(new, "messageid", idmef_alert_get_messageid(alert));

        process_analyzer(new, idmef_alert_get_analyzer(alert));
        process_time(new, "CreateTime", idmef_alert_get_create_time(alert));
        process_time(new, "DetectTime", idmef_alert_get_detect_time(alert));
        process_time(new, "AnalyzerTime", idmef_alert_get_analyzer_time(alert));

        process_assessment(new, idmef_alert_get_assessment(alert));

	source = NULL;
	while ( (source = idmef_alert_get_next_source(alert, source)) )
                process_source(new, source);

	target = NULL;
	while ( (target = idmef_alert_get_next_target(alert, target)) )
                process_target(new, target);
        
        process_classification(new, idmef_alert_get_classification(alert));

	additional_data = NULL;
	while ( (additional_data = idmef_alert_get_next_additional_data(alert, additional_data)) )
                process_additional_data(new, additional_data);
}





static void process_heartbeat(xmlNodePtr idmefmsg, idmef_heartbeat_t *heartbeat) 
{
        char buf[256];
        xmlNodePtr hb;
        idmef_additional_data_t *additional_data;

        if ( ! heartbeat )
                return;
        
        hb = xmlNewChild(idmefmsg, NULL, "Heartbeat", NULL);
        if ( ! hb )
                return;
        
        snprintf(buf, sizeof(buf), "%" PRIu64, idmef_heartbeat_get_messageid(heartbeat));
        xmlSetProp(hb, "messageid", buf);
        
        process_analyzer(hb, idmef_heartbeat_get_analyzer(heartbeat));
        process_time(hb, "CreateTime", idmef_heartbeat_get_create_time(heartbeat));
        process_time(hb, "AnalyzerTime", idmef_heartbeat_get_analyzer_time(heartbeat));

	additional_data = NULL;
	while ( (additional_data = idmef_heartbeat_get_next_additional_data(heartbeat, additional_data)) )
                process_additional_data(hb, additional_data);
}



static void validate_dtd(xmlmod_plugin_t *plugin, xmlDoc *doc) 
{
        xmlValidCtxt validation_context;
        
        validation_context.doc = doc;
        validation_context.userData = (void *) stderr;
        validation_context.error = (xmlValidityErrorFunc) fprintf;
        validation_context.warning = (xmlValidityWarningFunc) fprintf;
        
        xmlValidateDtd(&validation_context, doc, plugin->idmef_dtd);
}



static void dump_to_buffer(xmlmod_plugin_t *plugin, xmlDoc *doc) 
{
        xmlNodeDumpOutput(plugin->fd, doc, doc->children, 0, plugin->format, NULL);

	xmlOutputBufferWriteString(plugin->fd, "\n");
        
        xmlOutputBufferFlush(plugin->fd);
}



static void dump_document(xmlmod_plugin_t *plugin, xmlDoc *doc) 
{
        dump_to_buffer(plugin, doc);
                
        if ( plugin->idmef_dtd )
                validate_dtd(plugin, doc);
}



static int xmlmod_run(prelude_plugin_instance_t *pi, idmef_message_t *message) 
{
        xmlNodePtr root;
        xmlDoc *document;
        xmlmod_plugin_t *plugin = prelude_plugin_instance_get_data(pi);
        
        document = xmlNewDoc("1.0");
        if ( ! document ) {
                log(LOG_ERR, "error creating XML document.\n");
                return -1;
        }
        
        root = xmlNewDocNode(document, NULL, "IDMEF-Message", NULL);
        if ( ! root ) {
                xmlFreeDoc(document);
                return -1;
        }
        
        xmlDocSetRootElement(document, root);
                 
        switch ( idmef_message_get_type(message) ) {

        case IDMEF_MESSAGE_TYPE_ALERT:
                process_alert(root, idmef_message_get_alert(message));
                break;

        case IDMEF_MESSAGE_TYPE_HEARTBEAT:
                process_heartbeat(root, idmef_message_get_heartbeat(message));
                break;

        default:
                log(LOG_ERR, "unknow message type: %d.\n", idmef_message_get_type(message));
                xmlFreeDoc(document);
                return -1;
        }

        dump_document(plugin, document);
        xmlFreeDoc(document);

        return 0;
}



static int xmlmod_init(prelude_plugin_instance_t *pi)
{
        int ret;
        FILE *fd;
        xmlmod_plugin_t *plugin = prelude_plugin_instance_get_data(pi);
        
        if ( ! plugin->logfile ) {
                log(LOG_ERR, "no logfile specified.\n");
                return -1;
        }

        ret = strcmp(plugin->logfile, "stderr");
        if ( ret == 0 )
                fd = stderr;
        
        else if ( ! (fd = fopen(plugin->logfile, "a+")) ) {
                log(LOG_ERR, "error opening %s for writing.\n", plugin->logfile);
                return -1;
        }
        
        if ( plugin->no_buffering ) {
                ret = setvbuf(fd, NULL, _IONBF, 0);
                if ( ret != 0)
                        log(LOG_ERR, "error opening %s for writing.\n", plugin->logfile);
        }
        
        plugin->fd = xmlAllocOutputBuffer(NULL);
        if ( ! plugin->fd ) {
                log(LOG_ERR, "error creating an XML output buffer.\n");
                fclose(fd);
                return -1;
        }
        
        plugin->fd->context = fd;
        plugin->fd->writecallback = file_write;
        plugin->fd->closecallback = NULL;  /* No close callback */

        return 0;
}



static void xmlmod_destroy(prelude_plugin_instance_t *pi)
{
        xmlmod_plugin_t *plugin = prelude_plugin_instance_get_data(pi);

        if ( plugin->fd )
                xmlOutputBufferClose(plugin->fd);
        
        if ( plugin->logfile )
                free(plugin->logfile);

        free(plugin);
}




static int xmlmod_activate(void *context, prelude_option_t *opt, const char *arg) 
{
        xmlmod_plugin_t *new;
        
        new = calloc(1, sizeof(*new));
        if ( ! new ) {
                log(LOG_ERR, "memory exhausted.\n");
                return prelude_error_from_errno(errno);
        }

        prelude_plugin_instance_set_data(context, new);
        
        return 0;
}



static int set_dtd_check(void *context, prelude_option_t *option, const char *arg)
{
        xmlmod_plugin_t *plugin = prelude_plugin_instance_get_data(context);
        
        if ( ! arg )
                arg = IDMEF_DTD;
                
        plugin->idmef_dtd = xmlParseDTD(NULL, arg);
        if ( ! plugin->idmef_dtd ) {
                log(LOG_ERR, "error loading IDMEF DTD %s.\n", arg);
                return prelude_error_from_errno(errno);
        }

        return 0;
}



static int enable_formatting(void *context, prelude_option_t *option, const char *arg)
{
        xmlmod_plugin_t *plugin = prelude_plugin_instance_get_data(context);
        
        plugin->format = ! plugin->format;
        
        return 0;
}



static int get_formatting(void *context, prelude_option_t *opt, char *buf, size_t size)
{
        xmlmod_plugin_t *plugin = prelude_plugin_instance_get_data(context);
        
        snprintf(buf, size, "%s", plugin->format ? "enabled" : "disabled");

        return 0;
}



static int disable_buffering(void *context, prelude_option_t *option, const char *arg)
{
        xmlmod_plugin_t *plugin = prelude_plugin_instance_get_data(context);
        
        plugin->no_buffering = ! plugin->no_buffering;
        
        return 0;
}



prelude_plugin_generic_t *xmlmod_LTX_prelude_plugin_init(void)
{
	prelude_option_t *opt;
        int hook = PRELUDE_OPTION_TYPE_CLI|PRELUDE_OPTION_TYPE_CFG|PRELUDE_OPTION_TYPE_WIDE;
        
        xmlInitParser();
        
        opt = prelude_option_add(manager_root_optlist, hook, 0, "xmlmod", "Option for the xmlmod plugin",
                                 PRELUDE_OPTION_ARGUMENT_OPTIONAL, xmlmod_activate, NULL);

        prelude_plugin_set_activation_option((void *) &xmlmod_plugin, opt, xmlmod_init);
        
        prelude_option_add(opt, hook, 'l', "logfile", "Specify output file to use",
                           PRELUDE_OPTION_ARGUMENT_REQUIRED, xmlmod_set_logfile, xmlmod_get_logfile);
        
        prelude_option_add(opt, hook, 'v', "validate", "Validate IDMEF XML output against DTD",
                           PRELUDE_OPTION_ARGUMENT_OPTIONAL, set_dtd_check, NULL);

        prelude_option_add(opt, hook, 'f', "format", "Format XML output so that it is readable",
                           PRELUDE_OPTION_ARGUMENT_NONE, enable_formatting, get_formatting);

        prelude_option_add(opt, hook, 'd', "disable-buffering",
                           "Disable output file buffering to prevent truncated tags",
                           PRELUDE_OPTION_ARGUMENT_NONE, disable_buffering, NULL);
       
        prelude_plugin_set_name(&xmlmod_plugin, "XmlMod");
        prelude_plugin_set_author(&xmlmod_plugin, "Yoann Vandoorselaere");
        prelude_plugin_set_contact(&xmlmod_plugin, "yoann@prelude-ids.org");
        prelude_plugin_set_desc(&xmlmod_plugin, "Convert from Prelude internal format to IDMEF-XML format");
        prelude_plugin_set_destroy_func(&xmlmod_plugin, xmlmod_destroy);

        report_plugin_set_running_func(&xmlmod_plugin, xmlmod_run);
        
	return (void *) &xmlmod_plugin;
}

