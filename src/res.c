/************************************************************************
 *   IRC - Internet Relay Chat, ircd/res.c
 *   Copyright (C) 1992 Darren Reed
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

#include "struct.h"
#include "common.h"
#include "sys.h"
#include "res.h"
#include "numeric.h"
#include "h.h"

#include <signal.h>
#ifndef _WIN32
#include <sys/time.h>
#include <sys/socket.h>
#endif
#include "nameser.h"
#include "resolv.h"
#include <string.h>

#ifndef lint
static char rcsid[] = "@(#)$Id$";
#endif

/* #undef	DEBUG	/* because there is a lot of debug code in here :-) */
#ifdef _WIN32
#define HE(x) (x)->he
#else
#define HE(x) (&((x)->he))
#endif
static char hostbuf[HOSTLEN + 1 + 100];	/* +100 for INET6 */
static char dot[] = ".";
static int incache = 0;
static CacheTable hashtable[ARES_CACSIZE];
static aCache *cachetop = NULL;
static ResRQ *last, *first;

static void rem_cache __P((aCache *));
static void rem_request __P((ResRQ *));
static int do_query_name __P((Link *, char *, ResRQ *));
static int do_query_number __P((Link *, struct IN_ADDR *, ResRQ *));
static void resend_query __P((ResRQ *));
static int proc_answer __P((ResRQ *, HEADER *, char *, char *));
static int query_name __P((char *, int, int, ResRQ *));
static aCache *make_cache __P((ResRQ *)), *rem_list __P((aCache *));
static aCache *find_cache_name __P((char *));
static aCache *find_cache_number __P((ResRQ *, char *));
static int add_request __P((ResRQ *));
static ResRQ *make_request __P((Link *));
static int send_res_msg __P((char *, int, int));
static ResRQ *find_id __P((int));
static int hash_number __P((unsigned char *));
static void update_list __P((ResRQ *, aCache *));
static int hash_name __P((char *));
static int bad_hostname __P((char *, int));
#ifdef _WIN32
static	void	async_dns(void *parm);
#endif

static struct cacheinfo {
	int  ca_adds;
	int  ca_dels;
	int  ca_expires;
	int  ca_lookups;
	int  ca_na_hits;
	int  ca_nu_hits;
	int  ca_updates;
} cainfo;

static struct resinfo {
	int  re_errors;
	int  re_nu_look;
	int  re_na_look;
	int  re_replies;
	int  re_requests;
	int  re_resends;
	int  re_sent;
	int  re_timeouts;
	int  re_shortttl;
	int  re_unkrep;
} reinfo;

int init_resolver(op)
	int op;
{
	int  ret = 0;

#ifdef	LRAND48
	srand48(time(NULL));
#endif
	if (op & RES_INITLIST)
	{
		bzero((char *)&reinfo, sizeof(reinfo));
		first = last = NULL;
	}
	if (op & RES_CALLINIT)
	{
		ret = ircd_res_init();
		if (!ircd_res.nscount)
		{
			ircd_res.nscount = 1;
#ifdef INET6
			/* still IPv4 */
			ircd_res.nsaddr_list[0].sin_addr.s_addr =
			    inet_pton(AF_INET, NAME_SERVER,
			    &ircd_res.nsaddr_list[0].sin_addr.s_addr);
#else
			ircd_res.nsaddr_list[0].sin_addr.s_addr =
			    inet_addr(NAME_SERVER);
#endif
		}
	}

	if (op & RES_INITSOCK)
	{
		int  on = 0;

#ifdef INET6
		/* still IPv4 */
		ret = resfd = socket(AF_INET, SOCK_DGRAM, 0);
#else
		ret = resfd = socket(AF_INET, SOCK_DGRAM, 0);
#endif
		(void)setsockopt(ret, SOL_SOCKET, SO_BROADCAST, &on, on);
	}
#ifdef DEBUG
	if (op & RES_INITDEBG);
	ircd_res.options |= RES_DEBUG;
#endif
	if (op & RES_INITCACH)
	{
		bzero((char *)&cainfo, sizeof(cainfo));
		bzero((char *)hashtable, sizeof(hashtable));
	}
	if (op == 0)
		ret = resfd;
	return ret;
}

static int add_request(new)
	ResRQ *new;
{
	if (!new)
		return -1;
	if (!first)
		first = last = new;
	else
	{
		last->next = new;
		last = new;
	}
	new->next = NULL;
	reinfo.re_requests++;
	return 0;
}

/*
 * remove a request from the list. This must also free any memory that has
 * been allocated for temporary storage of DNS results.
 */
static void rem_request(old)
	ResRQ *old;
{
	ResRQ **rptr, *r2ptr = NULL;
	int  i;
	char *s;

	if (!old)
		return;
#ifdef _WIN32
         while (old->locked)
                 Sleep(0);
 #endif
	for (rptr = &first; *rptr; r2ptr = *rptr, rptr = &(*rptr)->next)
		if (*rptr == old)
		{
			*rptr = old->next;
			if (last == old)
				last = r2ptr;
			break;
		}
#ifdef	DEBUG
	Debug((DEBUG_INFO, "rem_request:Remove %#x at %#x %#x",
	    old, *rptr, r2ptr));
#endif
	r2ptr = old;
#ifndef _WIN32
	if (r2ptr->he.h_name)
		MyFree((char *)r2ptr->he.h_name);
	for (i = 0; i < MAXALIASES; i++)
		if ((s = r2ptr->he.h_aliases[i]))
			MyFree(s);
#else
       if (r2ptr->he)
                 MyFree(r2ptr->he);
#endif
	if (r2ptr->name)
		MyFree(r2ptr->name);
	MyFree((char *)r2ptr);

	return;
}

/*
 * Create a DNS request record for the server.
 */
static ResRQ *make_request(lp)
	Link *lp;
{
	ResRQ *nreq;

	nreq = (ResRQ *)MyMalloc(sizeof(ResRQ));
	bzero((char *)nreq, sizeof(ResRQ));
	nreq->next = NULL;	/* where NULL is non-zero ;) */
	nreq->sentat = TStime();
	nreq->retries = HOST_RETRIES;
	nreq->resend = 1;
	nreq->srch = -1;
	if (lp)
		bcopy((char *)lp, (char *)&nreq->cinfo, sizeof(Link));
	else
		bzero((char *)&nreq->cinfo, sizeof(Link));
	nreq->timeout = HOST_TIMEOUT;	/* start at 4 and exponential inc. */
#ifndef _WIN32
	nreq->he.h_addrtype = AFINET;
	nreq->he.h_name = NULL;
	nreq->he.h_aliases[0] = NULL;
#else
        nreq->he = (struct hostent *)MyMalloc(MAXGETHOSTSTRUCT);
        bzero((char *)nreq->he, MAXGETHOSTSTRUCT);
        nreq->he->h_addrtype = AFINET;
        nreq->he->h_name = NULL;
#endif
	(void)add_request(nreq);
	return nreq;
}

/*
 * Remove queries from the list which have been there too long without
 * being resolved.
 */
time_t timeout_query_list(now)
	time_t now;
{
	ResRQ *rptr, *r2ptr;
	time_t next = 0, tout;
	aClient *cptr;

	Debug((DEBUG_DNS, "timeout_query_list at %s", myctime(now)));
	for (rptr = first; rptr; rptr = r2ptr)
	{
		r2ptr = rptr->next;
		tout = rptr->sentat + rptr->timeout;
#ifndef _WIN32
		if (now >= tout)
#else
		if (now >= tout && !rptr->locked)
#endif
			if (--rptr->retries <= 0)
			{
#ifdef DEBUG
				Debug((DEBUG_ERROR, "timeout %x now %d cptr %x",
				    rptr, now, rptr->cinfo.value.cptr));
#endif
				reinfo.re_timeouts++;
				cptr = rptr->cinfo.value.cptr;
				switch (rptr->cinfo.flags)
				{
				  case ASYNC_CLIENT:
#ifdef SHOWCONNECTINFO
					  sendto_one(cptr, REPORT_FAIL_DNS);
#endif
					  ClearDNS(cptr);
                                        if (!DoingAuth(cptr))
                                                 SetAccess(cptr);
					  break;
				  case ASYNC_CONNECT:
					  sendto_ops("Host %s unknown",
					      rptr->name);
					  break;
				}
				rem_request(rptr);
				continue;
			}
			else
			{
				rptr->sentat = now;
				rptr->timeout += rptr->timeout;
				resend_query(rptr);
				tout = now + rptr->timeout;
#ifdef DEBUG
				Debug((DEBUG_INFO, "r %x now %d retry %d c %x",
				    rptr, now, rptr->retries,
				    rptr->cinfo.value.cptr));
#endif
			}
		if (!next || tout < next)
			next = tout;
	}
	return (next > now) ? next : (now + AR_TTL);
}

/*
 * del_queries - called by the server to cleanup outstanding queries for
 * which there no longer exist clients or conf lines.
 */
void del_queries(cp)
	char *cp;
{
	ResRQ *rptr, *r2ptr;

	for (rptr = first; rptr; rptr = r2ptr)
	{
		r2ptr = rptr->next;
		if (cp == rptr->cinfo.value.cp)
			rem_request(rptr);
	}
}

/*
 * sends msg to all nameservers found in the "ircd_res" structure.
 * This should reflect /etc/resolv.conf. We will get responses
 * which arent needed but is easier than checking to see if nameserver
 * isnt present. Returns number of messages successfully sent to 
 * nameservers or -1 if no successful sends.
 */
#ifndef _WIN32
static int send_res_msg(msg, len, rcount)
	char *msg;
	int len, rcount;
{
	int  i;
	int  sent = 0, max;

	if (!msg)
		return -1;

	max = MIN(ircd_res.nscount, rcount);
	if (ircd_res.options & RES_PRIMARY)
		max = 1;
	if (!max)
		max = 1;

	for (i = 0; i < max; i++)
	{
#ifdef INET6
		/* still IPv4 */
		ircd_res.nsaddr_list[i].sin_family = AF_INET;
#else
		ircd_res.nsaddr_list[i].sin_family = AF_INET;
#endif
#ifdef INET6
		if (sendto(resfd, msg, len, 0,
		    (struct sockaddr *)&(ircd_res.nsaddr_list[i]),
		    sizeof(struct sockaddr)) == len)
#else
		if (sendto(resfd, msg, len, 0,
		    (struct sockaddr *)&(ircd_res.nsaddr_list[i]),
		    sizeof(struct sockaddr)) == len)
#endif

		{
			reinfo.re_sent++;
			sent++;
		}
		else
			Debug((DEBUG_ERROR, "s_r_m:sendto: %d on %d",
			    errno, resfd));
	}

	return (sent) ? sent : -1;
}
#endif

/*
 * find a dns request id (id is determined by dn_mkquery)
 */
static ResRQ *find_id(id)
	int id;
{
	ResRQ *rptr;

	for (rptr = first; rptr; rptr = rptr->next)
		if (rptr->id == id)
			return rptr;
	return NULL;
}

struct hostent *gethost_byname(name, lp)
	char *name;
	Link *lp;
{
	aCache *cp;

	reinfo.re_na_look++;
	if ((cp = find_cache_name(name)))
#ifndef _WIN32
		return (struct hostent *)&(cp->he);
#else
		 return (struct hostent *)cp->he;
#endif
	if (!lp)
		return NULL;
	(void)do_query_name(lp, name, NULL);
	return NULL;
}

struct hostent *gethost_byaddr(addr, lp)
	char *addr;
	Link *lp;
{
	aCache *cp;

	reinfo.re_nu_look++;
	if ((cp = find_cache_number(NULL, addr)))
#ifndef _WIN32
		return (struct hostent *)&(cp->he);
#else
		return (struct hostent *)cp->he;
#endif
	if (!lp)
		return NULL;
	(void)do_query_number(lp, (struct IN_ADDR *)addr, NULL);
	return NULL;
}

static int do_query_name(lp, name, rptr)
	Link *lp;
	char *name;
	ResRQ *rptr;
{
#ifndef _WIN32
	char hname[HOSTLEN + 1];
	int  len;

	strncpyzt(hname, name, sizeof(hname));
	len = strlen(hname);

	if (rptr && !index(hname, '.') && ircd_res.options & RES_DEFNAMES)
	{
		(void)strncat(hname, dot, sizeof(hname) - len - 1);
		len++;
		(void)strncat(hname, ircd_res.defdname,
		    sizeof(hname) - len - 1);
	}

	/*
	 * Store the name passed as the one to lookup and generate other host
	 * names to pass onto the nameserver(s) for lookups.
	 */
	if (!rptr)
	{
		rptr = make_request(lp);
#ifdef INET6
		rptr->type = T_AAAA;
#else
		rptr->type = T_A;
#endif
		rptr->name = (char *)MyMalloc(strlen(name) + 1);
		(void)strcpy(rptr->name, name);
	}
	Debug((DEBUG_DNS, "do_query_name(): %s ", hname));
#ifdef INET6
	return (query_name(hname, C_IN, T_AAAA, rptr));
#else
	return (query_name(hname, C_IN, T_A, rptr));
#endif
#else
	 rptr->id = _beginthread(async_dns, 0, (void *)rptr);
         rptr->sends++;
         return 0;
#endif
}

/*
 * Use this to do reverse IP# lookups.
 */
static int do_query_number(lp, numb, rptr)
	Link *lp;
	struct IN_ADDR *numb;
	ResRQ *rptr;
{
	char ipbuf[128];
	u_char *cp;

#ifdef INET6
	cp = (u_char *)numb->s6_addr;
	if (cp[0] == 0 && cp[1] == 0 && cp[2] == 0 && cp[3] == 0 && cp[4] == 0
	    && cp[5] == 0 && cp[6] == 0 && cp[7] == 0 && cp[8] == 0
	    && cp[9] == 0 && ((cp[10] == 0 && cp[11] == 0) || (cp[10] == 0xff
	    && cp[11] == 0xff)))
	{
		(void)ircsprintf(ipbuf, "%u.%u.%u.%u.in-addr.arpa.",
		    (u_int)(cp[15]), (u_int)(cp[14]),
		    (u_int)(cp[13]), (u_int)(cp[12]));
	}
	else
	{
		(void)ircsprintf(ipbuf,
		    "%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.%x.ip6.int.",
		    (u_int)(cp[15] & 0xf), (u_int)(cp[15] >> 4),
		    (u_int)(cp[14] & 0xf), (u_int)(cp[14] >> 4),
		    (u_int)(cp[13] & 0xf), (u_int)(cp[13] >> 4),
		    (u_int)(cp[12] & 0xf), (u_int)(cp[12] >> 4),
		    (u_int)(cp[11] & 0xf), (u_int)(cp[11] >> 4),
		    (u_int)(cp[10] & 0xf), (u_int)(cp[10] >> 4),
		    (u_int)(cp[9] & 0xf), (u_int)(cp[9] >> 4),
		    (u_int)(cp[8] & 0xf), (u_int)(cp[8] >> 4),
		    (u_int)(cp[7] & 0xf), (u_int)(cp[7] >> 4),
		    (u_int)(cp[6] & 0xf), (u_int)(cp[6] >> 4),
		    (u_int)(cp[5] & 0xf), (u_int)(cp[5] >> 4),
		    (u_int)(cp[4] & 0xf), (u_int)(cp[4] >> 4),
		    (u_int)(cp[3] & 0xf), (u_int)(cp[3] >> 4),
		    (u_int)(cp[2] & 0xf), (u_int)(cp[2] >> 4),
		    (u_int)(cp[1] & 0xf), (u_int)(cp[1] >> 4),
		    (u_int)(cp[0] & 0xf), (u_int)(cp[0] >> 4));
	}
#else
	cp = (u_char *)&numb->s_addr;
	(void)ircsprintf(ipbuf, "%u.%u.%u.%u.in-addr.arpa.",
	    (u_int)(cp[3]), (u_int)(cp[2]), (u_int)(cp[1]), (u_int)(cp[0]));
#endif

	if (!rptr)
	{
		rptr = make_request(lp);
		rptr->type = T_PTR;
#ifndef _WIN32
#ifdef INET6
		bcopy(numb->s6_addr, rptr->addr.s6_addr, IN6ADDRSZ);
		bcopy((char *)numb->s6_addr,
		    (char *)&rptr->he.h_addr, sizeof(struct in6_addr));
#else
		rptr->addr.s_addr = numb->s_addr;
		bcopy((char *)&numb->s_addr,
		    (char *)&rptr->he.h_addr, sizeof(struct in_addr));
#endif
		rptr->he.h_length = sizeof(struct IN_ADDR);
#else
		rptr->addr.s_addr = numb->s_addr;
		bcopy((char *)&numb->s_addr,
		    (char *)&rptr->he->h_addr, sizeof(struct in_addr));
		rptr->he->h_length = sizeof(struct IN_ADDR);

#endif
	}
#ifndef _WIN32
	return (query_name(ipbuf, C_IN, T_PTR, rptr));
#else
         rptr->id = _beginthread(async_dns, 0, (void *)rptr);
         rptr->sends++;
         return 0;
#endif
}

#ifndef _WIN32
/*
 * generate a query based on class, type and name.
 */
static int query_name(name, class, type, rptr)
	char *name;
	int class, type;
	ResRQ *rptr;
{
	struct timeval tv;
	char buf[MAXPACKET];
	int  r, s, k = 0;
	HEADER *hptr;

	bzero(buf, sizeof(buf));
	r = ircd_res_mkquery(QUERY, name, class, type, NULL, 0, NULL,
	    (u_char *)buf, sizeof(buf));
	if (r <= 0)
	{
		h_errno = NO_RECOVERY;
		return r;
	}
	hptr = (HEADER *) buf;
#ifdef LRAND48
	do
	{
		hptr->id = htons(ntohs(hptr->id) + k + lrand48() & 0xffff);
#else
	(void)gettimeofday(&tv, NULL);
	do
	{
		/* htons/ntohs can be assembler macros, which cannot
		   be nested. Thus two lines.   -Vesa               */
		u_short nstmp = ntohs(hptr->id) + k +
		    (u_short)(tv.tv_usec & 0xffff);
		hptr->id = htons(nstmp);
#endif /* LRAND48 */
		k++;
	}
	while (find_id(ntohs(hptr->id)));
	rptr->id = ntohs(hptr->id);
	rptr->sends++;
	s = send_res_msg(buf, r, rptr->sends);
	if (s == -1)
	{
		h_errno = TRY_AGAIN;
		return -1;
	}
	else
		rptr->sent += s;
	return 0;
}

static void resend_query(rptr)
	ResRQ *rptr;
{
	if (rptr->resend == 0)
		return;
	reinfo.re_resends++;
	switch (rptr->type)
	{
	  case T_PTR:
		  (void)do_query_number(NULL, &rptr->addr, rptr);
		  break;
#ifdef INET6
	  case T_AAAA:
#endif
	  case T_A:
		  (void)do_query_name(NULL, rptr->name, rptr);
		  break;
	  default:
		  break;
	}
	return;
}

/*
 * process name server reply.
 */
static int proc_answer(rptr, hptr, buf, eob)
	ResRQ *rptr;
	char *buf, *eob;
	HEADER *hptr;
{
	char *cp, **alias;
	struct hent *hp;
	int  class, type, dlen, len, ans = 0, n;
	struct IN_ADDR dr, *adr;

	cp = buf + sizeof(HEADER);
	hp = (struct hent *)&(rptr->he);
	adr = &hp->h_addr;
#ifdef INET6
	while (adr->s6_laddr[0] | adr->s6_laddr[1] | adr->s6_laddr[2] |
	    adr->s6_laddr[3])
#else
	while (adr->s_addr)
#endif
		adr++;
	alias = hp->h_aliases;
	while (*alias)
		alias++;
#if SOLARIS_2 && !defined(__GNUC__)	/* brain damaged compiler it seems */
	for (; hptr->qdcount > 0; hptr->qdcount--)
#else
	while (hptr->qdcount-- > 0)
#endif
		if ((n = __ircd_dn_skipname((u_char *)cp, (u_char *)eob)) == -1)
			break;
		else
			cp += (n + QFIXEDSZ);
	/*
	 * proccess each answer sent to us blech.
	 */
	while (hptr->ancount-- > 0 && cp && cp < eob)
	{
		n = ircd_dn_expand((u_char *)buf, (u_char *)eob, (u_char *)cp,
		    hostbuf, sizeof(hostbuf));
		if (n <= 0)
			break;

		cp += n;
		type = (int)ircd_getshort((u_char *)cp);
		cp += 2;	/* INT16SZ */
		class = (int)ircd_getshort((u_char *)cp);
		cp += 2;	/* INT16SZ */
		rptr->ttl = ircd_getlong((u_char *)cp);
		cp += 4;	/* INT32SZ */
		dlen = (int)ircd_getshort((u_char *)cp);
		cp += 2;	/* INT16SZ */
		rptr->type = type;

		len = strlen(hostbuf);
		/* name server never returns with trailing '.' */
		if (!index(hostbuf, '.') && (ircd_res.options & RES_DEFNAMES))
		{
			(void)strcat(hostbuf, dot);
			len++;
			(void)strncat(hostbuf, ircd_res.defdname,
			    sizeof(hostbuf) - 1 - len);
			len = MIN(len + strlen(ircd_res.defdname),
			    sizeof(hostbuf) - 1);
		}

		switch (type)
		{
#ifdef INET6
		  case T_AAAA:
#endif
		  case T_A:
#ifdef INET6
			  if (dlen != ((type == T_AAAA) ? sizeof(dr) :
			      sizeof(struct in_addr)))
#else
			  if (dlen != sizeof(dr))
#endif
			  {
				  sendto_one
				      ("Bad IP length (%d) returned for %s",
				      dlen, hostbuf);
				  Debug((DEBUG_DNS,
				      "Bad IP length (%d) returned for %s",
				      dlen, hostbuf));
				  return -2;
			  }
			  hp->h_length = dlen;
			  if (ans == 1)
				  hp->h_addrtype = (class == C_IN) ?
				      AFINET : AF_UNSPEC;
#ifdef INET6
			  if (type == T_AAAA)
				  bcopy(cp, (char *)&dr, dlen);
			  else
			  {
				  dr.s6_laddr[0] = dr.s6_laddr[1] = 0;
				  dr.s6_laddr[2] = htonl(0xffff);
				  bcopy(cp, &dr.s6_laddr[3], INADDRSZ);
			  }
			  bcopy(dr.s6_addr, adr->s6_addr, IN6ADDRSZ);
#else
			  bcopy(cp, (char *)&dr, dlen);
			  adr->s_addr = dr.s_addr;
#endif
#ifdef INET6
			  Debug((DEBUG_INFO, "got ip # %s for %s",
			      inet_ntop(AF_INET6, (char *)adr, mydummy,
			      MYDUMMY_SIZE), hostbuf));
#else
			  Debug((DEBUG_INFO, "got ip # %s for %s",
			      inetntoa((char *)adr), hostbuf));
#endif
			  if (!hp->h_name)
			  {
				  hp->h_name = (char *)MyMalloc(len + 1);
				  (void)strcpy(hp->h_name, hostbuf);
			  }
			  ans++;
			  adr++;
			  cp += dlen;
			  break;
		  case T_PTR:
			  if ((n = ircd_dn_expand((u_char *)buf, (u_char *)eob,
			      (u_char *)cp, hostbuf, sizeof(hostbuf))) < 0)
			  {
				  cp = NULL;
				  break;
			  }
			  cp += n;
			  len = strlen(hostbuf);
			  Debug((DEBUG_INFO, "got host %s (%d vs %d)",
			      hostbuf, len, strlen(hostbuf)));
			  if (bad_hostname(hostbuf, len))
				  return -1;
			  /*
			   * copy the returned hostname into the host name
			   * or alias field if there is a known hostname
			   * already.
			   */
			  if (hp->h_name)
			  {
				  Debug((DEBUG_INFO, "duplicate PTR ignored"));
			  }
			  else
			  {
				  hp->h_name = (char *)MyMalloc(len + 1);
				  (void)strcpy(hp->h_name, hostbuf);
			  }
			  ans++;
			  break;
		  case T_CNAME:
			  cp += dlen;
			  Debug((DEBUG_INFO, "got cname %s", hostbuf));
			  if (bad_hostname(hostbuf, len))
				  return -1;	/* a break would be enough here */
			  if (alias >= &(hp->h_aliases[MAXALIASES - 1]))
				  break;
			  *alias = (char *)MyMalloc(len + 1);
			  (void)strcpy(*alias++, hostbuf);
			  *alias = NULL;
			  ans++;
			  break;
		  default:
#ifdef DEBUG
			  Debug((DEBUG_INFO, "proc_answer: type:%d for:%s",
			      type, hostbuf));
#endif
			  break;
		}
	}
	return ans;
}
#endif
/*
 * read a dns reply from the nameserver and process it.
 */
#ifndef _WIN32
struct hostent *get_res(lp)
	char *lp;
#else
struct hostent *get_res(lp,id)
	char *lp;
	long id
#endif
{
#ifndef _WIN32
	static char buf[sizeof(HEADER) + MAXPACKET];
	HEADER *hptr;
	ResRQ *rptr = NULL;
	aCache *cp = NULL;
#ifdef INET6
	struct sockaddr_in sin;
#else
	struct sockaddr_in sin;
#endif
	int  rc, a, max;
	SOCK_LEN_TYPE len = sizeof(sin);

	(void)alarm((unsigned)4);
#ifdef INET6
	rc = recvfrom(resfd, buf, sizeof(buf), 0, (struct sockaddr *)&sin,
	    &len);
#else
	rc = recvfrom(resfd, buf, sizeof(buf), 0, (struct sockaddr *)&sin,
	    &len);
#endif

	(void)alarm((unsigned)0);
	if (rc <= sizeof(HEADER))
		goto getres_err;
	/*
	 * convert DNS reply reader from Network byte order to CPU byte order.
	 */
	hptr = (HEADER *) buf;
	hptr->id = ntohs(hptr->id);
	hptr->ancount = ntohs(hptr->ancount);
	hptr->qdcount = ntohs(hptr->qdcount);
	hptr->nscount = ntohs(hptr->nscount);
	hptr->arcount = ntohs(hptr->arcount);
#ifdef	DEBUG
	Debug((DEBUG_NOTICE, "get_res:id = %d rcode = %d ancount = %d",
	    hptr->id, hptr->rcode, hptr->ancount));
#endif
#endif
	reinfo.re_replies++;
	/*
	 * response for an id which we have already received an answer for
	 * just ignore this response.
	 */
#ifndef _WIN32
	rptr = find_id(hptr->id);
#else
	rptr = find_id(id);
#endif
	if (!rptr)
		goto getres_err;
	/*
	 * check against possibly fake replies
	 */
#ifndef _WIN32
	max = MIN(ircd_res.nscount, rptr->sends);
	if (!max)
		max = 1;

	for (a = 0; a < max; a++)
#ifdef INET6
		if (!ircd_res.nsaddr_list[a].sin_addr.s_addr ||
		    !bcmp((char *)&sin.sin_addr,
		    (char *)&ircd_res.nsaddr_list[a].sin_addr,
		    sizeof(struct in_addr)))
#else
		if (!ircd_res.nsaddr_list[a].sin_addr.s_addr ||
		    !bcmp((char *)&sin.sin_addr,
		    (char *)&ircd_res.nsaddr_list[a].sin_addr,
		    sizeof(struct in_addr)))
#endif
			break;
	if (a == max)
	{
		reinfo.re_unkrep++;
		goto getres_err;
	}

	if ((hptr->rcode != NOERROR) || (hptr->ancount == 0))
	{
		switch (hptr->rcode)
		{
		  case NXDOMAIN:
			  h_errno = TRY_AGAIN;
			  break;
		  case SERVFAIL:
			  h_errno = TRY_AGAIN;
			  break;
		  case NOERROR:
			  h_errno = NO_DATA;
			  break;
		  case FORMERR:
		  case NOTIMP:
		  case REFUSED:
		  default:
			  h_errno = NO_RECOVERY;
			  break;
		}
		reinfo.re_errors++;
		/*
		   ** If a bad error was returned, we stop here and dont send
		   ** send any more (no retries granted).
		 */
		if (h_errno != TRY_AGAIN)
		{
			Debug((DEBUG_DNS, "Fatal DNS error %d for %d",
			    h_errno, hptr->rcode));
			rptr->resend = 0;
			rptr->retries = 0;
		}
		goto getres_err;
	}
	a = proc_answer(rptr, hptr, buf, buf + rc);
	if (a == -1)
	{
		sendto_ops("ad hostname returned from %s for %s",
#ifdef INET6
		    inetntop(AF_INET, &sin.sin_addr, mydummy2,
		    MYDUMMY_SIZE),
		    inetntop(AF_INET6, rptr->he.h_addr.s6_addr,
		    mydummy, MYDUMMY_SIZE));
#else
		    inetntoa((char *)&sin.sin_addr),
		    inetntoa((char *)&rptr->he.h_addr));
#endif
#ifdef INET6
		Debug((DEBUG_DNS, "Bad hostname returned from %s for %s",
		    inet_ntop(AF_INET, &sin.sin_addr, mydummy2, MYDUMMY_SIZE),
		    inet_ntop(AF_INET6, rptr->he.h_addr.s6_addr, mydummy,
		    MYDUMMY_SIZE)));
#else
		Debug((DEBUG_DNS, "Bad hostname returned from %s for %s",
		    inetntoa((char *)&sin.sin_addr),
		    inetntoa((char *)&rptr->he.h_addr)));
#endif
	}
#ifdef DEBUG
	Debug((DEBUG_INFO, "get_res:Proc answer = %d", a));
#endif
	if (a > 0 && rptr->type == T_PTR)
	{
		struct hostent *hp2 = NULL;

		if (BadPtr(rptr->he.h_name))	/* Kludge!      960907/Vesa */
			goto getres_err;

#ifdef INET6
		Debug((DEBUG_DNS, "relookup %s <-> %s",
		    rptr->he.h_name, inet_ntop(AF_INET6,
		    (char *)&rptr->he.h_addr, mydummy, MYDUMMY_SIZE)));
#else
		Debug((DEBUG_DNS, "relookup %s <-> %s",
		    rptr->he.h_name, inetntoa((char *)&rptr->he.h_addr)));
#endif
		/*
		 * Lookup the 'authoritive' name that we were given for the
		 * ip#.  By using this call rather than regenerating the
		 * type we automatically gain the use of the cache with no
		 * extra kludges.
		 */
		if ((hp2 = gethost_byname(rptr->he.h_name, &rptr->cinfo)))
			if (lp)
				bcopy((char *)&rptr->cinfo, lp, sizeof(Link));
		/*
		 * If name wasn't found, a request has been queued and it will
		 * be the last one queued.  This is rather nasty way to keep
		 * a host alias with the query. -avalon
		 */
		if (!hp2 && rptr->he.h_aliases[0])
			for (a = 0; rptr->he.h_aliases[a]; a++)
			{
				Debug((DEBUG_DNS, "Copied CNAME %s for %s",
				    rptr->he.h_aliases[a], rptr->he.h_name));
				last->he.h_aliases[a] = rptr->he.h_aliases[a];
				rptr->he.h_aliases[a] = NULL;
			}

		rem_request(rptr);
		return hp2;
	}

	if (a > 0)
	{
		if (lp)
			bcopy((char *)&rptr->cinfo, lp, sizeof(Link));
		cp = make_cache(rptr);
#ifdef	DEBUG
		Debug((DEBUG_INFO, "get_res:cp=%#x rptr=%#x (made)", cp, rptr));
#endif

		rem_request(rptr);
	}
	else if (!rptr->sent)
		rem_request(rptr);
	return cp ? (struct hostent *)&cp->he : NULL;

      getres_err:
	/*
	 * Reprocess an error if the nameserver didnt tell us to "TRY_AGAIN".
	 */
	if (rptr)
	{
		if (h_errno != TRY_AGAIN)
		{
			/*
			 * If we havent tried with the default domain and its
			 * set, then give it a try next.
			 */
			if (ircd_res.options & RES_DEFNAMES
			    && ++rptr->srch == 0)
			{
				rptr->retries = ircd_res.retry;
				rptr->sends = 0;
				rptr->resend = 1;
#ifdef INET6
/* Comment out this ifdef to get names like ::ffff:a.b.c.d */
				if (rptr->type == T_AAAA)
					query_name(rptr->name, C_IN, T_A, rptr);
				Debug((DEBUG_DNS,
				    "getres_err: didn't work with T_AAAA, now also trying with T_A for %s",
				    rptr->name));
#endif
				resend_query(rptr);
			}
			else
			{
#ifdef INET6
/* Comment out this ifdef to get names like ::ffff:a.b.c.d */
				if (rptr->type == T_AAAA)
					query_name(rptr->name, C_IN, T_A, rptr);
				Debug((DEBUG_DNS,
				    "getres_err: didn't work with T_AAAA, now also trying with T_A for %s",
				    rptr->name));
#endif
				resend_query(rptr);
			}
		}
		else if (lp)
			bcopy((char *)&rptr->cinfo, lp, sizeof(Link));
	}
#else
        he = rptr->he;
         if (he && he->h_name && ((struct IN_ADDR *)he->h_addr)->S_ADDR &&
	             rptr->locked < 2)
             {
                 /*
                  * We only need to re-check the DNS if its a "byaddr" call,
                  * the "byname" calls will work correctly. -Cabal95
                  */
                 char        tempname[120];
                 int        i;
                 long        amt;
                 struct        hostent        *hp, *he = rptr->he;
                                                     
                 strcpy(tempname, he->h_name);
                 hp = gethostbyname(tempname);
                 if (hp && !bcmp(hp->h_addr, he->h_addr, sizeof(struct IN_ADDR)))
                     {
                     }
                                                     
                 else
                         rptr->he->h_name = NULL;
             }
                                                     
         if (lp)
                 bcopy((char *)&rptr->cinfo, lp, sizeof(Link));
                                                     
         cp = make_cache(rptr);
 # ifdef DEBUG
         Debug((DEBUG_INFO,"get_res:cp=%#x rptr=%#x (made)", cp, rptr));
 # endif
         rptr->locked = 0;
         rem_request(rptr);
         return cp ? (struct hostent *)cp->he : NULL;
                                                     
 getres_err:
         if (lp && rptr)
                 bcopy((char *)&rptr->cinfo, lp, sizeof(Link));
                                                     
#endif
	return (struct hostent *)NULL;
}

static int hash_number(ip)
	u_char *ip;
{
	u_int hashv = 0;

	/* could use loop but slower */
	hashv += (int)*ip++;
	hashv += hashv + (int)*ip++;
	hashv += hashv + (int)*ip++;
#ifdef INET6
	hashv += hashv + (int)*ip++;
	hashv += hashv + (int)*ip++;
	hashv += hashv + (int)*ip++;
	hashv += hashv + (int)*ip++;
	hashv += hashv + (int)*ip++;
	hashv += hashv + (int)*ip++;
	hashv += hashv + (int)*ip++;
	hashv += hashv + (int)*ip++;
	hashv += hashv + (int)*ip++;
	hashv += hashv + (int)*ip++;
	hashv += hashv + (int)*ip++;
	hashv += hashv + (int)*ip++;
#endif
	hashv += hashv + (int)*ip;
	hashv %= ARES_CACSIZE;
	return (hashv);
}

static int hash_name(name)
	register char *name;
{
	u_int hashv = 0;

	for (; *name && *name != '.'; name++)
		hashv += *name;
	hashv %= ARES_CACSIZE;
	return (hashv);
}

/*
** Add a new cache item to the queue and hash table.
*/
static aCache *add_to_cache(ocp)
	aCache *ocp;
{
	aCache *cp = NULL;
	int  hashv;

#ifdef DEBUG
	Debug((DEBUG_INFO,
	    "add_to_cache:ocp %#x he %#x name %#x addrl %#x 0 %#x",
	    ocp, &ocp->he, ocp->he.h_name, ocp->he.h_addr_list,
	    ocp->he.h_addr_list[0]));
#endif
	ocp->list_next = cachetop;
	cachetop = ocp;

	hashv = hash_name(HE(ocp)->h_name);
	ocp->hname_next = hashtable[hashv].name_list;
	hashtable[hashv].name_list = ocp;

	hashv = hash_number((u_char *)HE(ocp)->h_addr);
	ocp->hnum_next = hashtable[hashv].num_list;
	hashtable[hashv].num_list = ocp;

#ifdef	DEBUG
#ifdef INET6
	Debug((DEBUG_INFO, "add_to_cache:added %s[%08x%08x%08x%08x] cache %#x.",
	    ocp->he.h_name,
	    ((struct in6_addr *)HE(ocp)->h_addr_list)->s6_laddr[0],
	    ((struct in6_addr *)HE(ocp)->h_addr_list)->s6_laddr[1],
	    ((struct in6_addr *)HE(ocp)->h_addr_list)->s6_laddr[2],
	    ((struct in6_addr *)HE(ocp)->h_addr_list)->s6_laddr[3], ocp));
#else
	Debug((DEBUG_INFO, "add_to_cache:added %s[%08x] cache %#x.",
	    HE(ocp)->h_name, HE(ocp)->h_addr_list[0], ocp));
#endif
	Debug((DEBUG_INFO,
	    "add_to_cache:h1 %d h2 %x lnext %#x namnext %#x numnext %#x",
	    hash_name(HE(ocp)->h_name), hashv, ocp->list_next,
	    ocp->hname_next, ocp->hnum_next));
#endif

	/*
	 * LRU deletion of excessive cache entries.
	 */
	if (++incache > MAXCACHED)
	{
		for (cp = cachetop; cp->list_next; cp = cp->list_next)
			;
		rem_cache(cp);
	}
	cainfo.ca_adds++;

	return ocp;
}

/*
** update_list does not alter the cache structure passed. It is assumed that
** it already contains the correct expire time, if it is a new entry. Old
** entries have the expirey time updated.
*/
static void update_list(rptr, cachep)
	ResRQ *rptr;
	aCache *cachep;
{
	aCache **cpp, *cp = cachep;
	char *s, *t, **base;
	int  i, j;
	int  addrcount;

	/*
	   ** search for the new cache item in the cache list by hostname.
	   ** If found, move the entry to the top of the list and return.
	 */
	cainfo.ca_updates++;

	for (cpp = &cachetop; *cpp; cpp = &((*cpp)->list_next))
		if (cp == *cpp)
			break;
	if (!*cpp)
		return;
	*cpp = cp->list_next;
	cp->list_next = cachetop;
	cachetop = cp;
#ifndef _WIN32
	if (!rptr)
		return;

#ifdef	DEBUG
	Debug((DEBUG_DEBUG, "u_l:cp %#x na %#x al %#x ad %#x",
	    cp, HE(cp)->h_name, HE(cp)->h_aliases, HE(cp)->he.h_addr));
	Debug((DEBUG_DEBUG, "u_l:rptr %#x h_n %#x", rptr, HE(rptr)->h_name));
#endif
	/*
	 * Compare the cache entry against the new record.  Add any
	 * previously missing names for this entry.
	 */
	for (i = 0; cp->he.h_aliases[i]; i++)
		;
	addrcount = i;
	for (i = 0, s = HE(rptr)->h_name; s && i < MAXALIASES;
	    s = HE(rptr)->h_aliases[i++])
	{
		for (j = 0, t = HE(cp)->h_name; t && j < MAXALIASES;
		    t = HE(cp)->h_aliases[j++])
			if (!mycmp(t, s))
				break;
		if (!t && j < MAXALIASES - 1)
		{
			base = HE(cp)->h_aliases;

			addrcount++;
			base = (char **)MyRealloc((char *)base,
			    sizeof(char *) * (addrcount + 1));
			HE(cp)->h_aliases = base;
#ifdef	DEBUG
			Debug((DEBUG_DNS, "u_l:add name %s hal %x ac %d",
			    s, HE(CP)->h_aliases, addrcount));
#endif
			base[addrcount - 1] = strdup(s);
			base[addrcount] = NULL;
		}
	}
#ifdef INET6
	for (i = 0; HE(cp)->h_addr_list[i]; i++)
#else
	for (i = 0; &HE(cp)->h_addr_list[i]; i++)
#endif
		;
	addrcount = i;

	/*
	 * Do the same again for IP#'s.
	 */
#ifdef INET6
	for (s = (char *)HE(rptr)->h_addr.S_ADDR;
	    ((struct IN_ADDR *)s)->S_ADDR; s += sizeof(struct IN_ADDR))
#else
	for (s = (char *)&HE(rptr)->h_addr.S_ADDR;
	    ((struct IN_ADDR *)s)->S_ADDR; s += sizeof(struct IN_ADDR))
#endif
	{
#ifdef INET6
		for (i = 0; (t = HE(cp)->h_addr_list[i]); i++)
#else
		for (i = 0; (t = HE(cp)->h_addr_list[i]); i++)
#endif
			if (!bcmp(s, t, sizeof(struct IN_ADDR)))
				break;
		if (i >= MAXADDRS || addrcount >= MAXADDRS)
			break;
		/*
		 * Oh man this is bad...I *HATE* it. -avalon
		 *
		 * Whats it do ?  Reallocate two arrays, one of pointers
		 * to "char *" and the other of IP addresses.  Contents of
		 * the IP array *MUST* be preserved and the pointers into
		 * it recalculated.
		 */
		if (!t)
		{
			struct IN_ADDR **ab;

			ab = (struct IN_ADDR **)HE(cp)->h_addr_list;
			addrcount++;
			t = (char *)MyRealloc((char *)*ab,
			    addrcount * sizeof(struct IN_ADDR));
			base = (char **)MyRealloc((char *)ab,
			    (addrcount + 1) * sizeof(*ab));
			HE(cp)->h_addr_list = base;
#ifdef	DEBUG
			Debug((DEBUG_DNS, "u_l:add IP %x hal %x ac %d",
			    ntohl(((struct IN_ADDR *)s)->S_ADDR),
			    HE(cp)->h_addr_list, addrcount));
#endif
			for (; addrcount; addrcount--)
			{
				*ab++ = (struct IN_ADDR *)t;
				t += sizeof(struct IN_ADDR);
			}
			*ab = NULL;
			bcopy(s, (char *)*--ab, sizeof(struct IN_ADDR));
		}
	}
#endif
	return;
}

static aCache *find_cache_name(name)
	char *name;
{
	aCache *cp;
	char *s;
	int  hashv, i;

	hashv = hash_name(name);

	cp = hashtable[hashv].name_list;
#ifdef	DEBUG
	Debug((DEBUG_DNS, "find_cache_name:find %s : hashv = %d", name, hashv));
#endif

	for (; cp; cp = cp->hname_next)
		for (i = 0, s = HE(cp)->h_name; s; s = HE(cp)->h_aliases[i++])
			if (mycmp(s, name) == 0)
			{
				cainfo.ca_na_hits++;
				update_list(NULL, cp);
				return cp;
			}

	for (cp = cachetop; cp; cp = cp->list_next)
	{
		/*
		 * if no aliases or the hash value matches, we've already
		 * done this entry and all possiblilities concerning it.
		 */
		if (!*HE(cp)->h_aliases)
			continue;
		if (hashv == hash_name(HE(cp)->h_name))
			continue;
		for (i = 0, s = HE(cp)->h_aliases[i]; s && i < MAXALIASES; i++)
			if (!mycmp(name, s))
			{
				cainfo.ca_na_hits++;
				update_list(NULL, cp);
				return cp;
			}
	}
	return NULL;
}

/*
 * find a cache entry by ip# and update its expire time
 */
static aCache *find_cache_number(rptr, numb)
	ResRQ *rptr;
	char *numb;
{
	aCache *cp;
	int  hashv, i;
#ifdef	DEBUG
	struct IN_ADDR *ip = (struct IN_ADDR *)numb;
#endif

	hashv = hash_number((u_char *)numb);

	cp = hashtable[hashv].num_list;
#ifdef DEBUG
#ifdef INET6
	Debug((DEBUG_DNS,
	    "find_cache_number:find %s[%08x%08x%08x%08x]: hashv = %d",
	    inet_ntop(AF_INET6, numb, mydummy, MYDUMMY_SIZE), ip->s6_laddr[0],
	    ip->s6_laddr[1], ip->s6_laddr[2], ip->s6_laddr[3], hashv));
#else
	Debug((DEBUG_DNS, "find_cache_number:find %s[%08x]: hashv = %d",
	    inetntoa(numb), ntohl(ip->s_addr), hashv));
#endif
#endif
	for (; cp; cp = cp->hnum_next)
	{
#ifdef INET6
		for (i = 0; HE(cp)->h_addr_list[i]; i++)
#else
		for (i = 0; HE(cp)->h_addr_list[i]; i++)
#endif
		{
			if (!bcmp(HE(cp)->h_addr_list[i], numb,
			    sizeof(struct IN_ADDR)))
			{
				cainfo.ca_nu_hits++;
				update_list(rptr, cp);
				return cp;
			}
		}
	}
	for (cp = cachetop; cp; cp = cp->list_next)
	{
		if (!HE(cp)->h_addr_list && !HE(cp)->h_aliases)
		{
			cp = rem_list(cp);
			continue;
		}
		/*
		 * single address entry...would have been done by hashed
		 * search above...
		 */
#ifdef INET6
		if (!HE(cp)->h_addr_list[1])
#else
		if (!HE(cp)->h_addr_list[1])
#endif
			continue;
		/*
		 * if the first IP# has the same hashnumber as the IP# we
		 * are looking for, its been done already.
		 */
		if (hashv == hash_number((u_char *)HE(cp)->h_addr_list[0]))
			continue;
#ifdef INET6
		for (i = 1; HE(cp)->h_addr_list[i]; i++)
#else
		for (i = 1; HE(cp)->h_addr_list[i]; i++)
#endif
			if (!bcmp(HE(cp)->h_addr_list[i], numb,
			    sizeof(struct IN_ADDR)))
			{
				cainfo.ca_nu_hits++;
				update_list(rptr, cp);
				return cp;
			}
	}
	return NULL;
}

static aCache *make_cache(rptr)
	ResRQ *rptr;
{
	aCache *cp;
	int  i, n;
	struct hostent *hp;
	char *s, **t;

	/*
	   ** shouldn't happen but it just might...
	 */
	if (!rptr->he.h_name || !WHOSTENTP(rptr->he.h_addr.S_ADDR))
		return NULL;
	/*
	   ** Make cache entry.  First check to see if the cache already exists
	   ** and if so, return a pointer to it.
	 */
	for (i = 0; WHOSTENTP(rptr->he.h_addr_list[i].S_ADDR); i++)
		if ((cp = find_cache_number(rptr,
#ifdef INET6
		    (char *)(rptr->he.h_addr_list[i].S_ADDR))))
#else
		    (char *)&(rptr->he.h_addr_list[i].S_ADDR))))
#endif
		    return cp;

	/*
	   ** a matching entry wasnt found in the cache so go and make one up.
	 */
	cp = (aCache *)MyMalloc(sizeof(aCache));
	bzero((char *)cp, sizeof(aCache));
#ifdef _WIN32
                                                     
         cp->he = (struct hostent *)MyMalloc(MAXGETHOSTSTRUCT);
                                                     
         res_copyhostent(rptr->he, cp->he);
#else
	hp = &cp->he;
	for (i = 0; i < MAXADDRS - 1; i++)
		if (!WHOSTENTP(rptr->he.h_addr_list[i].S_ADDR))
			break;

	/*
	   ** build two arrays, one for IP#'s, another of pointers to them.
	 */
	t = hp->h_addr_list = (char **)MyMalloc(sizeof(char *) * (i + 1));
	bzero((char *)t, sizeof(char *) * (i + 1));

	s = (char *)MyMalloc(sizeof(struct IN_ADDR) * i);
	bzero(s, sizeof(struct IN_ADDR) * i);

	for (n = 0; n < i; n++, s += sizeof(struct IN_ADDR))
	{
		*t++ = s;
		bcopy((char *)&rptr->he.h_addr_list[n], s,
		    sizeof(struct IN_ADDR));
	}
	*t = (char *)NULL;

	/*
	   ** an array of pointers to CNAMEs.
	 */
	for (i = 0; i < MAXALIASES - 1; i++)
		if (!rptr->he.h_aliases[i])
			break;
	i++;
	t = hp->h_aliases = (char **)MyMalloc(sizeof(char *) * i);
	for (n = 0; n < i; n++, t++)
	{
		*t = rptr->he.h_aliases[n];
		rptr->he.h_aliases[n] = NULL;
	}

	hp->h_addrtype = rptr->he.h_addrtype;
	hp->h_length = rptr->he.h_length;
	hp->h_name = rptr->he.h_name;
#endif
	if (rptr->ttl < 600)
	{
		reinfo.re_shortttl++;
		cp->ttl = 600;
	}
	else
		cp->ttl = rptr->ttl;
	cp->expireat = TStime() + cp->ttl;
	HE(rptr)->h_name = NULL;
#ifdef DEBUG
	Debug((DEBUG_INFO, "make_cache:made cache %#x", cp));
#endif
	return add_to_cache(cp);
}

/*
 * rem_list
 */
static aCache *rem_list(cp)
	aCache *cp;
{
	aCache **cpp, *cr = cp->list_next;

	/*
	 * remove cache entry from linked list
	 */
	for (cpp = &cachetop; *cpp; cpp = &((*cpp)->list_next))
		if (*cpp == cp)
		{
			*cpp = cp->list_next;
			MyFree((char *)cp);
			break;
		}
	return cr;
}


/*
 * rem_cache
 *     delete a cache entry from the cache structures and lists and return
 *     all memory used for the cache back to the memory pool.
 */
static void rem_cache(ocp)
	aCache *ocp;
{
	aCache **cp;
#ifndef _WIN32
	struct hostent *hp = &ocp->he;
#else
	struct hostent *hp = ocp->he;
#endif
	int  hashv;
	aClient *cptr;

#ifdef	DEBUG
	Debug((DEBUG_DNS, "rem_cache: ocp %#x hp %#x l_n %#x aliases %#x",
	    ocp, hp, ocp->list_next, hp->h_aliases));
#endif
	/*
	   ** Cleanup any references to this structure by destroying the
	   ** pointer.
	 */
	for (hashv = highest_fd; hashv >= 0; hashv--)
		if ((cptr = local[hashv]) && (cptr->hostp == hp))
			cptr->hostp = NULL;
	/*
	 * remove cache entry from linked list
	 */
	for (cp = &cachetop; *cp; cp = &((*cp)->list_next))
		if (*cp == ocp)
		{
			*cp = ocp->list_next;
			break;
		}
	/*
	 * remove cache entry from hashed name lists
	 */
	hashv = hash_name(hp->h_name);
#ifdef	DEBUG
	Debug((DEBUG_DEBUG, "rem_cache: h_name %s hashv %d next %#x first %#x",
	    hp->h_name, hashv, ocp->hname_next, hashtable[hashv].name_list));
#endif
	for (cp = &hashtable[hashv].name_list; *cp; cp = &((*cp)->hname_next))
		if (*cp == ocp)
		{
			*cp = ocp->hname_next;
			break;
		}
	/*
	 * remove cache entry from hashed number list
	 */
	hashv = hash_number((u_char *)hp->h_addr);
#ifdef	DEBUG
# ifdef INET6
	Debug((DEBUG_DEBUG, "rem_cache: h_addr %s hashv %d next %#x first %#x",
	    inet_ntop(AF_INET6, hp->h_addr, mydummy, MYDUMMY_SIZE),
	    hashv, ocp->hnum_next, hashtable[hashv].num_list));
# else
	Debug((DEBUG_DEBUG, "rem_cache: h_addr %s hashv %d next %#x first %#x",
	    inetntoa(hp->h_addr),
	    hashv, ocp->hnum_next, hashtable[hashv].num_list));
# endif
#endif
	for (cp = &hashtable[hashv].num_list; *cp; cp = &((*cp)->hnum_next))
		if (*cp == ocp)
		{
			*cp = ocp->hnum_next;
			break;
		}
#ifdef _WIN32
         MyFree((char *)hp);
#else
	/*
	 * free memory used to hold the various host names and the array
	 * of alias pointers.
	 */
	if (hp->h_name)
		MyFree(hp->h_name);
	if (hp->h_aliases)
	{
		for (hashv = 0; hp->h_aliases[hashv]; hashv++)
			MyFree(hp->h_aliases[hashv]);
		MyFree((char *)hp->h_aliases);
	}

	/*
	 * free memory used to hold ip numbers and the array of them.
	 */
	if (hp->h_addr_list)
	{
		if (*hp->h_addr_list)
			MyFree((char *)*hp->h_addr_list);
		MyFree((char *)hp->h_addr_list);
	}
#endif
	MyFree((char *)ocp);

	incache--;
	cainfo.ca_dels++;

	return;
}

/*
 * removes entries from the cache which are older than their expirey times.
 * returns the time at which the server should next poll the cache.
 */
time_t expire_cache(now)
	time_t now;
{
	aCache *cp, *cp2;
	time_t next = 0;

	for (cp = cachetop; cp; cp = cp2)
	{
		cp2 = cp->list_next;

		if (now >= cp->expireat)
		{
			cainfo.ca_expires++;
			rem_cache(cp);
		}
		else if (!next || next > cp->expireat)
			next = cp->expireat;
	}
	return (next > now) ? next : (now + AR_TTL);
}

/*
 * remove all dns cache entries.
 */
void flush_cache()
{
	aCache *cp;

	while ((cp = cachetop))
		rem_cache(cp);
}

int m_dns(cptr, sptr, parc, parv)
	aClient *cptr, *sptr;
	int parc;
	char *parv[];
{
	aCache *cp;
	int  i;

	if (IsOper(sptr) && parv[1] && *parv[1] == 'l')
	{
		sendto_realops("%s did a DNS cache list");
		for (cp = cachetop; cp; cp = cp->list_next)
		{
			sendto_one(sptr, "NOTICE %s :Ex %d ttl %d host %s(%s)",
			    parv[0], cp->expireat - TStime(), cp->ttl,
#ifdef INET6
			    HE(cp)->h_name, inetntop(AF_INET6,
			    HE(cp)->h_addr, mydummy, MYDUMMY_SIZE));
#else
			    HE(cp)->h_name, inetntoa(HE(cp)->h_addr));
#endif
			for (i = 0; HE(cp)->h_aliases[i]; i++)
				sendto_one(sptr, "NOTICE %s : %s = %s (CN)",
				    parv[0], HE(cp)->h_name,
				    HE(cp)->h_aliases[i]);
#ifdef INET6
			for (i = 1; HE(cp)->h_addr_list[i]; i++)
			{
#else
			for (i = 1; HE(cp)->h_addr_list[i]; i++)
			{
#endif
				sendto_one(sptr, "NOTICE %s : %s = %s (IP)",
				    parv[0], HE(cp)->h_name,
#ifdef INET6
				    inetntop(AF_INET6,
				    HE(cp)->h_addr_list[i],
				    mydummy, MYDUMMY_SIZE));
#else
				    inetntoa(HE(cp)->h_addr_list[i]));
#endif
			}
		}
		return 2;
	}
	sendto_one(sptr, "NOTICE %s :Ca %d Cd %d Ce %d Cl %d Ch %d:%d Cu %d",
	    sptr->name,
	    cainfo.ca_adds, cainfo.ca_dels, cainfo.ca_expires,
	    cainfo.ca_lookups,
	    cainfo.ca_na_hits, cainfo.ca_nu_hits, cainfo.ca_updates);

	sendto_one(sptr, "NOTICE %s :Re %d Rl %d/%d Rp %d Rq %d",
	    sptr->name, reinfo.re_errors, reinfo.re_nu_look,
	    reinfo.re_na_look, reinfo.re_replies, reinfo.re_requests);
	sendto_one(sptr, "NOTICE %s :Ru %d Rsh %d Rs %d(%d) Rt %d", sptr->name,
	    reinfo.re_unkrep, reinfo.re_shortttl, reinfo.re_sent,
	    reinfo.re_resends, reinfo.re_timeouts);
	return 2;
}

u_long cres_mem(sptr, nick)
	aClient *sptr;
	char *nick;
{
	register aCache *c = cachetop;
	register struct hostent *h;
	register int i;
	u_long nm = 0, im = 0, sm = 0, ts = 0;

	for (; c; c = c->list_next)
	{
		sm += sizeof(*c);
#ifndef _WIN32
		h = &c->he;
#else
		h = c->he;
#endif
#ifdef INET6
		for (i = 0; h->h_addr_list[i]; i++)
#else
		for (i = 0; h->h_addr_list[i]; i++)
#endif
		{
			im += sizeof(char *);
			im += sizeof(struct IN_ADDR);
		}
		im += sizeof(char *);
		for (i = 0; h->h_aliases[i]; i++)
		{
			nm += sizeof(char *);
			nm += strlen(h->h_aliases[i]);
		}
		nm += i - 1;
		nm += sizeof(char *);
		if (h->h_name)
			nm += strlen(h->h_name);
	}
	ts = ARES_CACSIZE * sizeof(CacheTable);
	sendto_one(sptr, ":%s %d %s :RES table %d",
	    me.name, RPL_STATSDEBUG, nick, ts);
	sendto_one(sptr, ":%s %d %s :Structs %d IP storage %d Name storage %d",
	    me.name, RPL_STATSDEBUG, nick, sm, im, nm);
	return ts + sm + im + nm;
}


static int bad_hostname(name, len)
	char *name;
	int len;
{
	char *s, c;

	for (s = name; (c = *s) && len; s++, len--)
		if (isspace(c) || (c == 0x7) || (c == ':') ||
		    (c == '*') || (c == '?'))
			return -1;
	return 0;
}

#ifdef _WIN32
/*
 * Main thread function for handling DNS requests.
 */
void	async_dns(void *parm)
{
	ResRQ	*rptr = (ResRQ *)parm;
	struct hostent	*hp, *he = rptr->he;
	int	i, x;
	long	amt;

	if (rptr->type == T_A)
	    {
		rptr->locked = 2;
		hp = gethostbyname(rptr->name);
	    }
	else
	    {
		rptr->locked = 1;
		hp = gethostbyaddr((char *)(&rptr->addr.S_ADDR), 4, PF_INET);
	    }
	if ( !hp )
	    {
		/*
		 * Now heres a stupid check to forget, this apprently is
		 * what hasbeen causing most of the crashes.  I hope anyway.
		 */
		do_dns_async(rptr->id);
		_endthread();
	    }
	if ( (hp->h_aliases[0] && (hp->h_aliases[0]-(char *)hp)>MAXGETHOSTSTRUCT) ||
	     (hp->h_addr_list[0] && (hp->h_addr_list[0]-(char *)hp)>MAXGETHOSTSTRUCT))
	    {
		/*
		 * Seems windows does some weird, aka stupid, stuff with DNS.
		 * If the address is resolved from the HOSTS file, then the
		 * pointers will exceed MAXGETHOSTSTRUCT. Good and bad. Good
		 * because its an easy way to tell if the Admin is spoofing
		 * with his HOSTS file, bad because it also causes invalid
		 * pointers without this check. -Cabal95
		 */
		do_dns_async(rptr->id);
		_endthread();
	    }

	res_copyhostent(hp, rptr->he);
	do_dns_async(rptr->id);
	_endthread();
}

int	res_copyhostent(struct hostent *from, struct hostent *to)
{
	int	amt, x, i;

	to->h_addrtype = from->h_addrtype;
	to->h_length = from->h_length;
	/*
	 * Get to "primary" offset in to hostent buffer and copy over
	 * to hostname.
	 */
	amt = (long)to + sizeof(struct hostent);
	to->h_name = (char *)amt;
	strcpy(to->h_name, from->h_name);
	amt += strlen(to->h_name)+1;
	/* Setup tto alias list */
	if (amt&0x3)
		amt = (amt&0xFFFFFFFC)+4;	
	to->h_aliases = (char **)amt;
	for (x = 0; from->h_aliases[x]; x++)
		;
	x *= sizeof(char *);
	amt += sizeof(char *);
	for (i = 0; from->h_aliases[i]; i++)
	    {
		to->h_aliases[i] = (char *)(amt+x);
		strcpy(to->h_aliases[i], from->h_aliases[i]);
		amt += strlen(to->h_aliases[i])+1;
		if (amt&0x3)
			amt = (amt&0xFFFFFFFC)+4;	
	    }
	to->h_aliases[i] = NULL;
	/* Setup tto IP address list */
	to->h_addr_list = (char **)amt;
	for (x = 0; from->h_addr_list[x]; x++)
		;
	x *= sizeof(char *);
	for (i = 0; from->h_addr_list[i]; i++)
	    {
		amt += 4;
		to->h_addr_list[i] = (char *)(amt+x);
		((struct IN_ADDR *)to->h_addr_list[i])->S_ADDR = ((struct IN_ADDR *)from->h_addr_list[i])->S_ADDR;
	    }
	to->h_addr_list[i] = NULL;
}
#endif /*_WIN32*/
