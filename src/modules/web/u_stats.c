/*
   UnrealIRCd internal webserver - /stats
   Copyright (c) 2001, The UnrealIRCd Team
   All rights reserved.

   Redistribution and use in source and binary forms, with or without modification, are permitted
   provided that the following conditions are met:
   
     * Redistributions of source code must retain the above copyright notice, this list of conditions
       and the following disclaimer.
     * Redistributions in binary form must reproduce the above copyright notice, this list of conditions
       and the following disclaimer in the documentation and/or other materials provided with the
       distribution.
     * Neither the name of the The UnrealIRCd Team nor the names of its contributors may be used
       to endorse or promote products derived from this software without specific prior written permission.
     * The source code may not be redistributed for a fee or in closed source
       programs, without expressed oral consent by the UnrealIRCd Team, however
       for operating systems where binary distribution is required, if URL
       is passed with the package to get the full source
     * No warranty is given unless stated so by the The UnrealIRCd Team

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS `AS IS'' AND ANY EXPRESS OR
   IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
   FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
   LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
   BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR  
   BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
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
#else
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#endif
#include <fcntl.h>
#include "h.h"
#ifdef STRIPBADWORDS
#include "badwords.h"
#endif
#ifdef _WIN32
#include "version.h"
#endif
#include "threads.h"
#include "modules/web/httpd.h"

extern ircstats IRCstats;

int h_u_stats(HTTPd_Request *r);

#define soprintf sockprintf
DLLFUNC int h_u_stats(HTTPd_Request *r)
{
	HTTPd_Header *p;
	if (!match("/stats", r->url))
	{
		httpd_standard_header(r, "text/html");
		soprintf(r, "<title>Current user stats on %s</title>", me.name);
		soprintf(r, "Clients: %li  Invisible: %li  Servers: %li<br>",
			IRCstats.clients, IRCstats.invisible, IRCstats.servers);
		soprintf(r, "Unknown: %li  My clients: %li  My Servers: %li<br>",
			IRCstats.unknown, IRCstats.me_clients, IRCstats.me_servers);
		soprintf(r, "My Peak: %li    Global Peak: %li<br>",
			IRCstats.me_max, IRCstats.global_max);
		for (p = r->headers; p; p = p->next)
		{
			soprintf(r, "%s = %s<br>",
				p->name, p->value);	
		}
		return 1;
	}
	return 0;
}