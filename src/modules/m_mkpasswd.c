/*
 *   IRC - Internet Relay Chat, src/modules/m_mkpasswd.c
 *   (C) 2001 The UnrealIRCd Team
 *
 *   mkpasswd command
 *
 *   See file AUTHORS in IRC package for additional names of
 *   the programmers.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 1, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include "config.h"
#include "struct.h"
#include "common.h"
#include "sys.h"
#include "numeric.h"
#include "msg.h"
#include "channel.h"
#include <time.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#endif
#include <fcntl.h>
#include "h.h"
#include "proto.h"
#ifdef STRIPBADWORDS
#include "badwords.h"
#endif
#ifdef _WIN32
#include "version.h"
#endif

DLLFUNC int m_mkpasswd(aClient *cptr, aClient *sptr, int parc, char *parv[]);

#define MSG_MKPASSWD 	"MKPASSWD"	
#define TOK_MKPASSWD 	"y"	


#ifndef DYNAMIC_LINKING
ModuleHeader m_mkpasswd_Header
#else
#define m_mkpasswd_Header Mod_Header
ModuleHeader Mod_Header
#endif
  = {
	"m_mkpasswd",
	"$Id$",
	"command /mkpasswd", 
	"3.2-b8-1",
	NULL 
    };

#ifdef DYNAMIC_LINKING
DLLFUNC int	Mod_Init(ModuleInfo *modinfo)
#else
int    m_mkpasswd_Init(ModuleInfo *modinfo)
#endif
{
	add_Command(MSG_MKPASSWD, TOK_MKPASSWD, m_mkpasswd, MAXPARA);
	return MOD_SUCCESS;
	
}

#ifdef DYNAMIC_LINKING
DLLFUNC int	Mod_Load(int module_load)
#else
int    m_mkpasswd_Load(int module_load)
#endif
{
	return MOD_SUCCESS;
}

#ifdef DYNAMIC_LINKING
DLLFUNC int	Mod_Unload(int module_unload)
#else
int	m_mkpasswd_Unload(int module_unload)
#endif
{
	if (del_Command(MSG_MKPASSWD, TOK_MKPASSWD, m_mkpasswd) < 0)
	{
		sendto_realops("Failed to delete commands when unloading %s",
				m_mkpasswd_Header.name);
	}
	return MOD_SUCCESS;
	
}
/*
** m_mkpasswd
**      parv[0] = sender prefix
**      parv[1] = password to encrypt
*/
int  m_mkpasswd(aClient *cptr, aClient *sptr, int parc, char *parv[])
{
	short	type;
	char	*result = NULL;
        if (!IsAnOper(sptr))
                return -1;
	if (parc < 3)
	{
		sendto_one(sptr, ":%s NOTICE %s :*** Syntax: /mkpasswd <authmethod> :parameter",
			me.name, sptr->name);
		return 0;
	}        
        
        if (strlen(parv[1]) < 1)
        {
                sendto_one(sptr,
                    ":%s %s %s :*** Parameters MUST be atleast 1 character in length",
                    me.name, IsWebTV(sptr) ? "PRIVMSG" : "NOTICE", parv[0]);
                return 0;
        }
#ifndef _WIN32
        srandom(TStime());
#endif
	if ((type = Auth_FindType(parv[1])) == -1)
	{
		sendto_one(sptr, 
			":%s NOTICE %s :*** %s is not an enabled authentication method",
				me.name, sptr->name, parv[1]);
		return 0;
	}
	
	if (!(result = Auth_Make(type, parv[2])))
	{
		sendto_one(sptr, 
			":%s NOTICE %s :*** Authentication method %s failed",
				me.name, sptr->name, parv[1]);
		return 0;
	}
        sendto_one(sptr, ":%s %s %s :*** Authentication phrase (method=%s, para=%s) is: %s",
            me.name, IsWebTV(sptr) ? "PRIVMSG" : "NOTICE", parv[0], parv[1], parv[2], result);
        return 0;
}