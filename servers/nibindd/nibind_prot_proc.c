/*
 * nibindd remote procedure implementation
 * Copyright 1989-94, NeXT Computer Inc.
 */

#include <NetInfo/config.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <netinfo/ni.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <signal.h>
#include <sys/dir.h>
#include <sys/param.h>
#include <sys/wait.h>
#include <sys/fcntl.h>
#include <errno.h>
#include <pwd.h>
#include <NetInfo/mm.h>
#include <NetInfo/socket_lock.h>
#include <NetInfo/system.h>
#include <NetInfo/system_log.h>
#include <NetInfo/network.h>
#include "nibind_globals.h"

#ifdef _OS_NEXT_
#define nibind_ping_1_svc		nibind_ping_1
#define nibind_register_1_svc		nibind_register_1
#define nibind_unregister_1_svc		nibind_unregister_1
#define nibind_getregister_1_svc	nibind_getregister_1
#define nibind_listreg_1_svc		nibind_listreg_1
#define nibind_createmaster_1_svc	nibind_createmaster_1
#define nibind_createclone_1_svc	nibind_createclone_1
#define nibind_destroydomain_1_svc	nibind_destroydomain_1
#define nibind_bind_1_svc		nibind_bind_1
#endif

#ifndef __SLICK__
/* This is needed until nibind_prot.h in System.framework has been
 * generated by the new rpcgen.
 */
extern ni_status * nibind_unregister_1_svc(ni_name *, struct svc_req *);
#endif /* __SLICK__ */

#define BIND_TIMEOUT 2
#define BIND_RETRIES 0
#define MAX_WAIT 3
#define NI_TIMEOUT 5
#define NI_PING_TIMEOUT 1
#define	NI_ALIAS_NAME	"alias_name"
#define	NI_ALIAS_ADDRS	"alias_addrs"

/* the list of server aliases */

typedef struct ni_aliasinfo {
	char *name;
	char *alias;
	unsigned long address;
	unsigned long mask;
} ni_aliasinfo;

int	serverCount = 0;			/* num servers that have been checked */
int aliasCount = 0;				/* num aliases in the list */
ni_aliasinfo *aliasList;		/* the list of aliases */

char *find_ni_alias(char *server_tag, unsigned long client_addr);
void get_ni_aliases();
static void clnt_destroy_lock(CLIENT *cl, int sock);


extern const char NETINFO_PROG[];
extern const char NIBINDD_PROG[];

int waitreg;

static const char NI_SUFFIX_CUR[] = ".nidb";
static const char NI_SUFFIX_MOVED[] = ".move";
static const char NI_SUFFIX_TMP[] = ".temp";

/*
 * The list of registered servers
 */
struct {
	unsigned regs_len;
	nibind_registration *regs_val;
} regs;

typedef struct ni_serverinfo {
	int pid;
	ni_name name;
} ni_serverinfo;

ni_serverinfo *servers;
unsigned nservers;

void destroydir(ni_name);

void
storepid(int pid, ni_name name)
{
	MM_GROW_ARRAY(servers, nservers);
	servers[nservers].pid = pid;
	servers[nservers].name = ni_name_dup(name);
	nservers++;
}


void killservers(void)
{
	int i;

	system_log(LOG_ERR, "Shutting down NetInfo servers");

	signal(SIGCHLD, SIG_IGN);
	for (i = 0; i < nservers; i++) kill(servers[i].pid, SIGUSR1);
}


void killchildren(void)
{
	killservers();
	exit(1);
}


/*
 * Note that respawn doesn't preserve debug flags.
 */
void respawn(void)
{
	int i;

	killservers();
	
	system_log(LOG_ERR, "Restarting NetInfo");

	for (i = getdtablesize() - 1; i >= 0; i--) close(i);

	open("/dev/null", O_RDWR, 0);
	dup(0);
	dup(0);

	execl(NIBINDD_PROG, NIBINDD_PROG, 0);
	exit(1);
}


ni_status validate(struct svc_req *req)
{
	struct authunix_parms *ap;
	char *p;
	struct passwd *pwent;
	struct sockaddr_in *sin = svc_getcaller(req->rq_xprt);
	int auth_ok = 0;

	if (sys_is_my_address(&sin->sin_addr) && ntohs(sin->sin_port) < IPPORT_RESERVED)
	{
		return (NI_OK);
	}

	if (req->rq_cred.oa_flavor != AUTH_UNIX)
	{
		return(NI_PERM);
	}

	/* validate requests if root password supplied - MM 1994.02.19 */
	/* this used the unix auth to pass a vaguely-encrypted */
	/* root password in the machine name string */
	ap = (struct authunix_parms *)req->rq_clntcred;

	/* our stunning encryption scheme */
	for (p = ap->aup_machname; *p != '\0'; p++) *p = ~(*p);

	/* look up root */
	pwent = getpwuid(0);
	if (pwent == NULL)
	{
		system_log(LOG_ERR, "Can't find user account data for root!");
		return(NI_PERM);
	}

	/* check for no password */
	if (pwent->pw_passwd[0] == '\0') auth_ok = 1;
	else
	{
		/* check for password match */
		if (!strcmp(pwent->pw_passwd, crypt(ap->aup_machname, pwent->pw_passwd)))
			auth_ok = 1;
	}
	
	if (auth_ok == 1)
	{
		system_log(LOG_NOTICE,
			"Connection (sender = %s) authenticated as root",
			inet_ntoa(sin->sin_addr));
		return(NI_OK);
	}

	system_log(LOG_NOTICE,
		"Connection (sender = %s) failed authenticated as root",
		inet_ntoa(sin->sin_addr));
	return(NI_PERM);
}


void deletepid(int pid)
{
	int i;

	for (i = 0; i < nservers; i++)
	{
		if (servers[i].pid == pid)
		{
			nibind_unregister_1_svc(&servers[i].name, NULL);
			ni_name_free(&servers[i].name);
			for (i += 1; i < nservers; i++)
			{
				servers[i - 1] = servers[i];
			}

			MM_SHRINK_ARRAY(servers, nservers);
			nservers--;
			waitreg--;
			return;
		}
	}
}


void register_done(ni_name tag)
{
	int i;

	for (i = 0; i < nservers; i++)
	{
		if (ni_name_match(servers[i].name, tag))
		{
			waitreg--;
			return;
		}
	}
}


/*
 * Signal handler: must save and restore errno
 */
void catchchild(void)
{
	int i, pid;
	union wait status;
	int save_errno;
	char *tag = "UNKNOWN";

	save_errno = errno;
	pid = wait3((_WAIT_TYPE_ *)&status, WNOHANG, NULL);
	if (pid > 0)
	{
		if ((status.w_termsig != 0) || (status.w_retcode != 0))
		{
			/* Find the child with this PID */
			for (i = 0; i < nservers; i++)
			{
				if (servers[i].pid == pid)
				{
					tag = servers[i].name;
					break;
				}
			}

			if (status.w_termsig == 0)
			{
				system_log(LOG_WARNING, "netinfod %s [%d] exited %d",
					tag, pid, status.w_retcode);
			}
			else if (status.w_retcode != 0)
			{
				system_log(LOG_WARNING, "netinfod %s [%d] exited %d, "
					"signal %d", tag, pid, status.w_retcode, status.w_termsig);
			}
			else
			{
				system_log(LOG_WARNING, "netinfod %s [%d] terminated with "
					"signal %d", tag, pid, status.w_termsig);
			}
		}
		deletepid(pid);
	}
	errno = save_errno;
}


void *nibind_ping_1_svc(void *arg, struct svc_req *req)
{
	struct sockaddr_in *sin = svc_getcaller(req->rq_xprt);
	system_log(LOG_DEBUG, "ping (sender = %s)", inet_ntoa(sin->sin_addr));
	return((void *)~0);
}


ni_status *nibind_register_1_svc(nibind_registration *arg, struct svc_req *req)
{
	static ni_status status;
	int i;

	system_log(LOG_DEBUG, "register %s tcp %u udp %u",
		arg->tag, arg->addrs.tcp_port, arg->addrs.udp_port);

	status = validate(req);
	if (status != NI_OK) return(&status);

	for (i = 0; i < regs.regs_len; i++)
	{
		if (ni_name_match(arg->tag, regs.regs_val[i].tag))
		{
			register_done(arg->tag);
			ni_name_free(&regs.regs_val[i].tag);
			regs.regs_val[i] = *arg;
			bzero(arg, sizeof(*arg));
			status = NI_OK;
			return(&status);
		}
	}

	register_done(arg->tag);
	MM_GROW_ARRAY(regs.regs_val, regs.regs_len);
	regs.regs_val[regs.regs_len++] = *arg;
	bzero(arg, sizeof(*arg));
	status = NI_OK;
	return(&status);
}


ni_status * nibind_unregister_1_svc(ni_name *tag, struct svc_req *req)
{
	static ni_status status;
	ni_index i;

	system_log(LOG_DEBUG, "unregister %s", *tag);

	if (req != NULL)
	{
		status = validate(req);
		if (status != NI_OK) return(&status);
	}

	for (i = 0; i < regs.regs_len; i++)
	{
		if (ni_name_match(*tag, regs.regs_val[i].tag))
		{
			ni_name_free(&regs.regs_val[i].tag);
			regs.regs_val[i] = regs.regs_val[regs.regs_len - 1];
			MM_SHRINK_ARRAY(regs.regs_val, regs.regs_len);
			regs.regs_len--;
			break;
		}
	}

	/* remove the entries from the alias list as well */
	for (i = 0; i < aliasCount; i++)
	{
		if (strcmp((char *) *tag, aliasList[i].name))
		{
			free(aliasList[i].name);
			free(aliasList[i].alias);
			aliasList[i] = aliasList[aliasCount - 1];
			MM_SHRINK_ARRAY(aliasList, aliasCount);
			aliasCount--;
		}
	}	

	status = NI_OK;
	return(&status);
}


nibind_getregister_res *nibind_getregister_1_svc(ni_name *tag, struct svc_req *req)
{
	ni_index i;
	static nibind_getregister_res res;
	char *new_tag;
	struct sockaddr_in loopback;
	int sock;
	CLIENT *cl;
	enum clnt_stat clstat;
	struct timeval tv;

	res.status = NI_NOTAG;

	/* Check to see if we should substitute an alias here */
	new_tag = find_ni_alias(*tag, (req->rq_xprt->xp_raddr).sin_addr.s_addr);
	for (i = 0; i < regs.regs_len; i++)
	{
		if (ni_name_match(new_tag, regs.regs_val[i].tag))
		{
			res.nibind_getregister_res_u.addrs = regs.regs_val[i].addrs;
			res.status = NI_OK;
			break;
		}
	}

	/*
	 * If we have a tag to use, let's check to make sure that netinfod is
	 * alive.  We have to use low-level RPC here, since the ni library
	 * routines might try and talk to nibindd (which won't work!)
	 *
	 */
		
	if ((NI_PING_TIMEOUT != 0) && (res.status == NI_OK))
	{
		tv.tv_sec = NI_PING_TIMEOUT;
		tv.tv_usec = 0;
		loopback.sin_addr.s_addr  = htonl(INADDR_LOOPBACK);
		loopback.sin_family = AF_INET;
 
		loopback.sin_port = htons(res.nibind_getregister_res_u.addrs.udp_port);
		sock = socket_open(&loopback, NI_PROG, NI_VERS);
		if (sock < 0)
		{
			res.status = NI_NORESPONSE;
			return (&res);
		}

		cl = clntudp_create(&loopback, NI_PROG, NI_VERS, tv, &sock);
		if (cl == NULL)
		{
			socket_close(sock);
			res.status = NI_NORESPONSE;
			return (&res);
		}

		clstat = clnt_call(cl, _NI_PING, xdr_void, NULL, xdr_void, NULL, tv);
		if (clstat != RPC_SUCCESS) res.status = NI_NORESPONSE;

		clnt_destroy_lock(cl, sock);
	}

	return(&res);
}


nibind_listreg_res *nibind_listreg_1_svc(void *arg, struct svc_req *req)
{
	static nibind_listreg_res res;
	struct sockaddr_in *sin = svc_getcaller(req->rq_xprt);

	system_log(LOG_DEBUG, "listreg (sender = %s)", inet_ntoa(sin->sin_addr));

	res.status = NI_OK;
	res.nibind_listreg_res_u.regs.regs_len = regs.regs_len;
	res.nibind_listreg_res_u.regs.regs_val = regs.regs_val;
	return (&res);
}


ni_status *nibind_createmaster_1_svc(ni_name *tag, struct svc_req *req)
{
	static ni_status status;
	ni_index i;
	int pid;
	struct sockaddr_in *sin = svc_getcaller(req->rq_xprt);

	system_log(LOG_DEBUG, "createmaster %s (sender = %s)",
		*tag, inet_ntoa(sin->sin_addr));

	status = validate(req);
	if (status != NI_OK) return(&status);

	for (i = 0; i < regs.regs_len; i++)
	{
		if (ni_name_match(*tag, regs.regs_val[i].tag))
		{
			status = NI_DUPTAG;
			return(&status);
		}
	}

	pid = sys_spawn(NETINFO_PROG, "-m", *tag, 0);
	if (pid < 0)
	{
		status = NI_SYSTEMERR;
	}
	else
	{
		storepid(pid, *tag);
		status = NI_OK;
	}

	return(&status);
}

ni_status * nibind_createclone_1_svc(nibind_clone_args *args, struct svc_req *req)
{
	struct in_addr addr;
	static ni_status status;
	ni_index i;
	int pid;
	struct sockaddr_in *sin = svc_getcaller(req->rq_xprt);

	system_log(LOG_DEBUG, "createclone %s of %s/%s (sender = %s)",
		args->tag, args->master_name, args->master_tag, 
		inet_ntoa(sin->sin_addr));

	status = validate(req);
	if (status != NI_OK) return(&status);

	/* XDR will have byte-swapped the master address if this is a */
	/* little-endian system, since it gets passed as an unsigned long */
	addr.s_addr = htonl(args->master_addr);

	for (i = 0; i < regs.regs_len; i++)
	{
		if (ni_name_match(args->tag, regs.regs_val[i].tag))
		{
			status = NI_DUPTAG;
			return(&status);
		}
	}

	pid = sys_spawn(NETINFO_PROG, "-c", args->master_name, inet_ntoa(addr),
		args->master_tag, args->tag, 0);

	if (pid < 0)
	{
		status = NI_SYSTEMERR;
	}
	else
	{
		status = NI_OK;
		storepid(pid, args->tag);
	}

	return(&status);
}

ni_status *nibind_destroydomain_1_svc(ni_name *tag, struct svc_req *req)
{
	static ni_status status;
	int i;

	status = validate(req);
	if (status != NI_OK) return (&status);

	/* * Unregister it */
	status = NI_NOTAG;
	for (i = 0; i < regs.regs_len; i++)
	{
		if (ni_name_match(*tag, regs.regs_val[i].tag))
		{
			regs.regs_val[i] = regs.regs_val[regs.regs_len - 1];
			MM_SHRINK_ARRAY(regs.regs_val, regs.regs_len);
			regs.regs_len--;
			status = NI_OK;
			break;
		}
	}

	if (status != NI_OK) return(&status);

	/* remove the entries from the alias list as well */
	for (i = 0; i < aliasCount; i++)
	{
		if (strcmp((char *) *tag, aliasList[i].name))
		{
			free(aliasList[i].name);
			free(aliasList[i].alias);
			aliasList[i] = aliasList[aliasCount - 1];
			MM_SHRINK_ARRAY(aliasList, aliasCount);
			aliasCount--;
		}
	}	

	/* Then kill it */
	for (i = 0; i < nservers; i++)
	{
		if (ni_name_match(servers[i].name, *tag))
		{
			kill(servers[i].pid, SIGKILL);
			ni_name_free(&servers[i].name);
			servers[i] = servers[nservers - 1];
			MM_SHRINK_ARRAY(servers, nservers);
			nservers--;
			break;
		}
	}

	destroydir(*tag);
	return(&status);
}

void *nibind_bind_1_svc(nibind_bind_args *args, struct svc_req *req)
{
	unsigned port = 0;
	ni_index i;
	ni_binding binding;
	struct timeval tv;
	CLIENT *cl;
	struct sockaddr_in sin;
	int sock;
	char *new_srvr_tag;

	/* Check to see if we should substitute an alias here */
	new_srvr_tag = find_ni_alias(args->server_tag,htonl(args->client_addr));	
	for (i = 0; i < regs.regs_len; i++)
	{
		if (ni_name_match(new_srvr_tag, regs.regs_val[i].tag))
		{
			port = regs.regs_val[i].addrs.udp_port;
			break;
		}
	}

	if (port == 0)
	{
		/* Report an RPC error so the caller doesn't need to time out */
		svcerr_systemerr(req->rq_xprt);
		return(NULL);
	}

	sin.sin_port = htons(port);
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_family = AF_INET;
	MM_ZERO(&sin.sin_zero);
	sock = RPC_ANYSOCK;
	tv.tv_sec = BIND_TIMEOUT;
	tv.tv_usec = 0;
	cl = clntudp_create(&sin, NI_PROG, NI_VERS, tv, &sock);
	if (cl == NULL)
	{
		svcerr_systemerr(req->rq_xprt);
		return (NULL);
	}

	binding.tag = args->client_tag;
	binding.addr = args->client_addr;  /* GS - in host byte order */
	tv.tv_sec /= (BIND_RETRIES + 1);
	if (clnt_call(cl, _NI_BIND, xdr_ni_binding, &binding,
			xdr_void, NULL, tv) != RPC_SUCCESS)
	{
		clnt_destroy(cl);
		svcerr_systemerr(req->rq_xprt);
		return (NULL);
	}

	clnt_destroy(cl);
	return((void *)~0);
}


/*
 * Destroy a database directory
 */
static int
dir_destroy(char *dir)
{
	char path1[MAXPATHLEN + 1];
	char path2[MAXPATHLEN + 1];
	DIR *dp;
	struct direct *d;

	dp = opendir(dir);
	if (dp == NULL) return(0);

	while (NULL != (d = readdir(dp)))
	{
		sprintf(path1, "%s/%.*s", dir, d->d_namlen, d->d_name);
		sprintf(path2, "./%.*s.tmp", d->d_namlen, d->d_name);

		/*
		 * rename, then unlink in case NFS leaves tmp files behind
		 * (.nfs* files, that is).
		 */
		if ((rename(path1, path2) != 0) || (unlink(path2) != 0))
		{
			/* ignore error: rmdir will catch ENOTEMPTY */
		}
	}

	closedir(dp);
	return(rmdir(dir));
}


void dir_getnames(ni_name orig, ni_name *target, ni_name *moved, ni_name *tmp)
{
	if (target != NULL)
	{
		*target = malloc(strlen(orig) + strlen(NI_SUFFIX_CUR) + 1);
		sprintf(*target, "%s%s", orig, NI_SUFFIX_CUR);
	}

	if (moved != NULL)
	{
		*moved = malloc(strlen(orig) + strlen(NI_SUFFIX_MOVED) + 1);
		sprintf(*moved, "%s%s", orig, NI_SUFFIX_MOVED);
	}

	if (tmp != NULL)
	{
		*tmp = malloc(strlen(orig) + strlen(NI_SUFFIX_TMP) + 1);
		sprintf(*tmp, "%s%s", orig, NI_SUFFIX_TMP);
	}
}
  
void destroydir(ni_name tag)
{
	ni_name target = NULL;
	ni_name moved = NULL;
	ni_name tmp = NULL;

	dir_getnames(tag, &target, &moved, &tmp);
	dir_destroy(target);
	dir_destroy(moved);
	dir_destroy(tmp);
	ni_name_free(&target);
	ni_name_free(&moved);
	ni_name_free(&tmp);
}


/* 
 * Look in the alias list to see if we have a match for the requested tag
 * that applies to the given client source address.  If so, return the
 * real tag.
 */
 
char * find_ni_alias(char *server_tag, unsigned long client_addr)
{
	int	i;

	if (serverCount != regs.regs_len) get_ni_aliases();

	if (aliasCount == 0) return server_tag;

	/* look for first match in the table */
	for (i = 0; i < aliasCount; i++)
	{	
		if (((client_addr & aliasList[i].mask) == aliasList[i].address) &&
			(strcmp(server_tag, aliasList[i].alias) == 0))
		{
			system_log(LOG_INFO,
				"substituting alias %s for %s (client %s)",
				aliasList[i].name, server_tag,
				inet_ntoa(inet_makeaddr((htonl(client_addr) >> 16),
				htonl(client_addr))));
			return aliasList[i].name;
		}
	}

	return server_tag;
}


void get_ni_aliases()
{
	int db, val, index;
	ni_id niID;
	ni_id_res niID_res;
	ni_proplist	niPropList;
	ni_proplist_res	niPropList_res;
	ni_namelist niNameList;
	char tmpAliasName[MAXPATHLEN + 1], tmpAddress[16], tmpMask[16];
	ni_aliasinfo aliasRecord;
	char *info, *ptr;
	struct sockaddr_in loopback;
	int sock;
	CLIENT *cl;
	enum clnt_stat clstat;
	struct timeval tv;

	tv.tv_sec = NI_TIMEOUT;
	tv.tv_usec = 0;
	loopback.sin_addr.s_addr  = htonl(INADDR_LOOPBACK);
	loopback.sin_family = AF_INET;

	/*
	 * Go through the list of registered netinfod processes, and find out what
	 * aliases each has registered for.  Start with the next database that
	 * hasn't been checked yet.
	 */
	
	for (db = serverCount; db < regs.regs_len; db++)
	{
		serverCount++;
		loopback.sin_port = htons(regs.regs_val[db].addrs.tcp_port);

		sock = socket_connect(&loopback, NI_PROG, NI_VERS);
		if (sock < 0) continue;

		cl = clnttcp_create(&loopback, NI_PROG, NI_VERS, &sock, 0, 0);
		if (cl == NULL)
		{
			socket_close(sock);
			continue;
		}
	
		/* 
		 * Get the object ID for the root directory (where the data 
		 * are stored).
		 */
	
		NI_INIT(&niID_res);
		clstat = clnt_call(cl, _NI_ROOT, xdr_void, NULL, xdr_ni_id_res,
			&niID_res, tv);
		if ((clstat != RPC_SUCCESS) || (niID_res.status != NI_OK))
		{
			clnt_destroy_lock(cl, sock);
			continue;
		}

		niID = niID_res.ni_id_res_u.id;
	
		/* read the property list for the root directory */

		NI_INIT(&niPropList_res);
		clstat = clnt_call(cl, _NI_READ, xdr_ni_id, &niID, 
			xdr_ni_proplist_res, &niPropList_res, tv);
		if ((clstat != RPC_SUCCESS) || (niPropList_res.status != NI_OK))
		{
			clnt_destroy_lock(cl, sock);
			continue;
		}

		niPropList = niPropList_res.ni_proplist_res_u.stuff.props;

		/* find the property for the alias name */
	
		for (index = 0; index < niPropList.ni_proplist_len; index++)
		{
			if (strcmp(niPropList.ni_proplist_val[index].nip_name,
				NI_ALIAS_NAME) == 0)
			{
				strcpy(tmpAliasName, niPropList.ni_proplist_val[index].nip_val.ni_namelist_val[0]);	
				break;
			}
		}

		if (index == niPropList.ni_proplist_len)
		{
			/* didn't find a match */
			clnt_destroy_lock(cl, sock);
			ni_proplist_free(&niPropList);
			continue;
		}

		/* find the property for the address list */

		for (index = 0; index < niPropList.ni_proplist_len; index++)
		{
			if (strcmp(niPropList.ni_proplist_val[index].nip_name,
				NI_ALIAS_ADDRS) == 0)
			{
				break;
			}
		}

		if (index == niPropList.ni_proplist_len)
		{
			/* didn't find a match */
			clnt_destroy_lock(cl, sock);
			ni_proplist_free(&niPropList);
			continue;
		}
		else
		{
			niNameList = niPropList.ni_proplist_val[index].nip_val;

			for (val = 0; val < niNameList.ni_namelist_len; val++)
			{
				aliasRecord.name = malloc(strlen(regs.regs_val[db].tag) + 1);
				strcpy(aliasRecord.name, regs.regs_val[db].tag);
				aliasRecord.alias = malloc(strlen(tmpAliasName) + 1);
				strcpy(aliasRecord.alias, tmpAliasName);
				info = niNameList.ni_namelist_val[val];
				ptr = strchr(info, '/');
				if (ptr == NULL) ptr = strchr(info, '&');
				if (ptr == NULL)
				{
					system_log(LOG_ERR,
						"Error adding alias record, malformed "
						"NetInfo property: %s", info);
					free(aliasRecord.name);
					free(aliasRecord.alias);
				}
				else
				{
					strncpy(tmpAddress, info, ptr - info);
					tmpAddress[ptr - info] = 0;
					strcpy(tmpMask, ptr + 1);
					aliasRecord.address = inet_addr(tmpAddress);
					aliasRecord.mask = inet_addr(tmpMask);
					system_log(LOG_INFO,
						"Adding alias record: %s %s %s %s",
						aliasRecord.name, aliasRecord.alias,
						tmpAddress, tmpMask);
					MM_GROW_ARRAY(aliasList, aliasCount);
					aliasList[aliasCount++] = aliasRecord;
				}
			}
		}

		clnt_destroy_lock(cl, sock);
		ni_proplist_free(&niPropList);
	}
}


/*
 * Destroys a client handle with locks
 */
static void clnt_destroy_lock(CLIENT *cl, int sock)
{
	socket_lock();
	clnt_destroy(cl);
	close(sock);
	socket_unlock();
}