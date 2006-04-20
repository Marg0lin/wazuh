/*   $OSSEC, client-config.c, v0.1, 2005/04/01, Daniel B. Cid$   */

/* Copyright (C) 2003-2006 Daniel B. Cid <dcid@ossec.net>
 * All right reserved.
 *
 * This program is a free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation
 */



#include "shared.h"
#include "client-config.h"


int Read_Client(XML_NODE node, void *d1, void *d2) 
{
    int i = 0;
    
    /* XML definitions */
    char *xml_client_ip = "server-ip";
    char *xml_client_port = "port";
    char *xml_ar_disabled = "disable-active-response";

    agent *logr;

    logr = (agent *)d1;

    logr->execdq = 0;

    while(node[i])
    {
        if(!node[i]->element)
        {
            merror(XML_ELEMNULL, ARGV0);
            return(OS_INVALID);
        }
        else if(!node[i]->content)
        {
            merror(XML_VALUENULL, ARGV0, node[i]->element);
            return(OS_INVALID);
        }

        /* Getting server */
        else if(strcmp(node[i]->element,xml_client_ip) == 0)
        {
            os_strdup(node[i]->content, logr->rip);
            if(!OS_IsValidIP(logr->rip))
            {
                merror(INVALID_IP, ARGV0, logr->rip);
                return(OS_INVALID);
            }
        }
        else if(strcmp(node[i]->element,xml_client_port) == 0)
        {
            if(!OS_StrIsNum(node[i]->content))
            {
                merror(XML_VALUEERR,ARGV0,node[i]->element,node[i]->content);
                return(OS_INVALID);
            }
            logr->port = atoi(node[i]->content);
        }
        else if(strcmp(node[i]->element,xml_ar_disabled) == 0)
        {
            if(strcmp(node[i]->content, "yes") == 0)
                logr->execdq = -1;
            else if(strcmp(node[i]->content, "no") == 0)
                logr->execdq = 0;
            else
            {
                merror(XML_VALUEERR,ARGV0,node[i]->element,node[i]->content);
                return(OS_INVALID);
            }
        }
        else
        {
            merror(XML_INVELEM, ARGV0, node[i]->element);
            return(OS_INVALID);
        }
        i++;
    }

    return(0);
}


/* EOF */
