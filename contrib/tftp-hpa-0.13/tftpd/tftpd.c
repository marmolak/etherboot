/* tftp-hpa: $Id: tftpd.c,v 1.4 1999/09/26 07:12:54 hpa Exp $ */

/* $OpenBSD: tftpd.c,v 1.13 1999/06/23 17:01:36 deraadt Exp $	*/

/*
 * Copyright (c) 1983 Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef lint
static const char *copyright =
"@(#) Copyright (c) 1983 Regents of the University of California.\n\
 All rights reserved.\n";
/*static char sccsid[] = "from: @(#)tftpd.c	5.13 (Berkeley) 2/26/91";*/
/*static char rcsid[] = "$OpenBSD: tftpd.c,v 1.13 1999/06/23 17:01:36 deraadt Exp $: tftpd.c,v 1.6 1997/02/16 23:49:21 deraadt Exp $";*/
static const char *rcsid = "tftp-hpa $Id: tftpd.c,v 1.4 1999/09/26 07:12:54 hpa Exp $";
#endif /* not lint */

/*
 * Trivial file transfer protocol server.
 *
 * This version includes many modifications by Jim Guyton <guyton@rand-unix>
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/tftp.h>
#include <netdb.h>

#include <setjmp.h>
#include <syslog.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <pwd.h>
#include <unistd.h>
#include <limits.h>

#include "tftpsubs.h"

#define	TIMEOUT 5		/* Default timeout (seconds) */
#define TRIES   4		/* Number of attempts to send each packet */
#define TIMEOUT_LIMIT (TRIES*(TRIES+1)/2)

#ifndef OACK
#define OACK	6
#endif
#ifndef EOPTNEG
#define EOPTNEG	8
#endif

extern	int errno;
extern	char *__progname;
struct	sockaddr_in s_in = { AF_INET };
int	peer;
int	timeout    = TIMEOUT;
int	rexmtval   = TIMEOUT;
int	maxtimeout = TIMEOUT_LIMIT*TIMEOUT;

#define	PKTSIZE	MAX_SEGSIZE+4
char	buf[PKTSIZE];
char	ackbuf[PKTSIZE];
struct	sockaddr_in from;
int	fromlen;
off_t	tsize;
int     tsize_ok;

int	ndirs;
char	**dirs;

int	secure = 0;
int	cancreate = 0;

struct formats;

int tftp(struct tftphdr *, int);
void nak(int);
void timer(int);
void justquit(int);
void do_opt(char *, char *, char **);

int set_blksize(char *, char **);
int set_blksize2(char *, char **);
int set_tsize(char *, char **);
int set_timeout(char *, char **);

struct options {
        char    *o_opt;
        int     (*o_fnc)(char *, char **);
} options[] = {
        { "blksize",    set_blksize  },
        { "blksize2",   set_blksize2  },
        { "tsize",      set_tsize },
	{ "timeout",	set_timeout  },
        { NULL,         NULL }
};



static void
usage(void)
{
	syslog(LOG_ERR, "Usage: %s [-cs] [-r option...] [directory ...]",
	       __progname);
	exit(1);
}

int
main(int argc, char **argv)
{
	struct tftphdr *tp;
	struct passwd *pw;
	struct options *opt;
	int n = 0;
	int on = 1;
	int fd = 0;
	int pid;
	int i, j;
	int c;

	openlog("tftpd", LOG_PID | LOG_NDELAY, LOG_DAEMON);

	while ((c = getopt(argc, argv, "csr:")) != -1)
		switch (c) {
		case 'c':
			cancreate = 1;
			break;
		case 's':
			secure = 1;
			break;
		case 'r':
		  for ( opt = options ; opt->o_opt ; opt++ ) {
		    if ( !strcasecmp(optarg, opt->o_opt) ) {
		      opt->o_opt = ""; /* Don't support this option */
		      break;
		    }
		  }
		  if ( !opt->o_opt ) {
		        syslog(LOG_ERR, "Unknown option: %s", optarg);
			exit(1);
		  }
		  break;

		default:
			usage();
			break;
		}

	for (; optind != argc; optind++) {
		if (dirs)
			dirs = realloc(dirs, (ndirs+2) * sizeof (char *));
		else
			dirs = calloc(ndirs+2, sizeof(char *));
		if (dirs == NULL) {
			syslog(LOG_ERR, "malloc: %m");
			exit(1);
		}			
		dirs[n++] = argv[optind];
		dirs[n] = NULL;
		ndirs++;
	}

	if (secure) {
		if (ndirs == 0) {
			syslog(LOG_ERR, "no -s directory");
			exit(1);
		}
		if (ndirs > 1) {
			syslog(LOG_ERR, "too many -s directories");
			exit(1);
		}
		if (chdir(dirs[0])) {
			syslog(LOG_ERR, "%s: %m", dirs[0]);
			exit(1);
		}
	}

	pw = getpwnam("nobody");
	if (!pw) {
		syslog(LOG_ERR, "no nobody: %m");
		exit(1);
	}

	if (secure && chroot(".")) {
		syslog(LOG_ERR, "chroot: %m");
		exit(1);
	}

	(void) setegid(pw->pw_gid);
	(void) setgid(pw->pw_gid);
	(void) seteuid(pw->pw_uid);
	(void) setuid(pw->pw_uid);

	if (ioctl(fd, FIONBIO, &on) < 0) {
		syslog(LOG_ERR, "ioctl(FIONBIO): %m");
		exit(1);
	}
	fromlen = sizeof (from);
	n = recvfrom(fd, buf, sizeof (buf), 0,
	    (struct sockaddr *)&from, &fromlen);
	if (n < 0) {
		syslog(LOG_ERR, "recvfrom: %m");
		exit(1);
	}
	/*
	 * Now that we have read the message out of the UDP
	 * socket, we fork and exit.  Thus, inetd will go back
	 * to listening to the tftp port, and the next request
	 * to come in will start up a new instance of tftpd.
	 *
	 * We do this so that inetd can run tftpd in "wait" mode.
	 * The problem with tftpd running in "nowait" mode is that
	 * inetd may get one or more successful "selects" on the
	 * tftp port before we do our receive, so more than one
	 * instance of tftpd may be started up.  Worse, if tftpd
	 * break before doing the above "recvfrom", inetd would
	 * spawn endless instances, clogging the system.
	 */
	for (i = 1; i < 20; i++) {
		pid = fork();
		if (pid < 0) {
			sleep(i);
			/*
			 * flush out to most recently sent request.
			 *
			 * This may drop some request, but those
			 * will be resent by the clients when
			 * they timeout.  The positive effect of
			 * this flush is to (try to) prevent more
			 * than one tftpd being started up to service
			 * a single request from a single client.
			 */
			j = sizeof from;
			i = recvfrom(fd, buf, sizeof (buf), 0,
			    (struct sockaddr *)&from, &j);
			if (i > 0) {
				n = i;
				fromlen = j;
			}
		} else
			break;
	}
	if (pid < 0) {
		syslog(LOG_ERR, "fork: %m");
		exit(1);
	} else if (pid != 0)
		exit(0);

	from.sin_family = AF_INET;
	alarm(0);
	close(fd);
	close(1);
	peer = socket(AF_INET, SOCK_DGRAM, 0);
	if (peer < 0) {
		syslog(LOG_ERR, "socket: %m");
		exit(1);
	}
	if (bind(peer, (struct sockaddr *)&s_in, sizeof (s_in)) < 0) {
		syslog(LOG_ERR, "bind: %m");
		exit(1);
	}
	if (connect(peer, (struct sockaddr *)&from, sizeof(from)) < 0) {
		syslog(LOG_ERR, "connect: %m");
		exit(1);
	}
	tp = (struct tftphdr *)buf;
	tp->th_opcode = ntohs(tp->th_opcode);
	if (tp->th_opcode == RRQ || tp->th_opcode == WRQ)
		tftp(tp, n);
	exit(0);
}

int	validate_access(char *, int, struct formats *);
void	sendfile(struct formats *, struct tftphdr *, int);
void	recvfile(struct formats *, struct tftphdr *, int);

struct formats {
	char	*f_mode;
	int	(*f_validate)(char *, int, struct formats *);
	void	(*f_send)(struct formats *, struct tftphdr *, int);
	void	(*f_recv)(struct formats *, struct tftphdr *, int);
	int	f_convert;
} formats[] = {
	{ "netascii",	validate_access,	sendfile,	recvfile, 1 },
	{ "octet",	validate_access,	sendfile,	recvfile, 0 },
	{ NULL }
};

/*
 * Handle initial connection protocol.
 */
int
tftp(struct tftphdr *tp, int size)
{
	char *cp;
	int argn, ecode;
	struct formats *pf = NULL;
	char *filename, *mode = NULL;

        char *val = NULL, *opt = NULL;
        char *ap = ackbuf + 2;

        ((struct tftphdr *)ackbuf)->th_opcode = ntohs(OACK);

	filename = cp = tp->th_stuff;
	argn = 0;

	while ( cp < buf + size && *cp ) {
	     do {
		  cp++;
	     } while (cp < buf + size && *cp);

	     if ( *cp ) {
		  nak(EBADOP);	/* Corrupt packet - no final NULL */
		  exit(0);
	     }

	     argn++;
	     if (argn == 1) {
		  mode = ++cp;
	     } else if (argn == 2) {
		  for (cp = mode; *cp; cp++)
			    *cp = tolower(*cp);
		  for (pf = formats; pf->f_mode; pf++) {
		       if (!strcmp(pf->f_mode, mode))
			    break;
		  }
		  if (!pf->f_mode) {
		       nak(EBADOP);
		       exit(0);
		  }
		  ecode = (*pf->f_validate)(filename, tp->th_opcode, pf);
		  if (ecode) {
		       nak(ecode);
		       exit(0);
		  }
		  opt = ++cp;
	     } else if ( argn & 1 ) {
		  val = ++cp;
	     } else {
		  do_opt(opt, val, &ap);
		  opt = ++cp;
	     }
	}

	if (!pf) {
	     nak(EBADOP);
	     exit(0);
	}

	if ( ap != (ackbuf+2) ) {
	     if ( tp->th_opcode == WRQ )
		  (*pf->f_recv)(pf, ackbuf, ap-ackbuf);
	     else
		  (*pf->f_send)(pf, ackbuf, ap-ackbuf);
	} else {
	     if (tp->th_opcode == WRQ)
		  (*pf->f_recv)(pf, NULL, 0);
	     else
		  (*pf->f_send)(pf, NULL, 0);
	}
	exit(1);
}

static int blksize_set;

/*
 * Set a non-standard block size (c.f. RFC2348)
 */
int
set_blksize(char *val, char **ret)
{
  	static char b_ret[6];
        unsigned int sz = atoi(val);

	if ( blksize_set )
	  return 0;

        if (sz < 8)
                return(0);
        else if (sz > MAX_SEGSIZE)
                sz = MAX_SEGSIZE;

        segsize = sz;
        sprintf(*ret = b_ret, "%u", sz);

	blksize_set = 1;

        return(1);
}

/*
 * Set a power-of-two block size (nonstandard)
 */
int
set_blksize2(char *val, char **ret)
{
  	static char b_ret[6];
        unsigned int sz = atoi(val);

	if ( blksize_set )
	  return 0;

        if (sz < 8)
                return(0);
        else if (sz > MAX_SEGSIZE)
	        sz = MAX_SEGSIZE;

	/* Convert to a power of two */
	if ( sz & (sz-1) ) {
	  unsigned int sz1 = 1;
	  /* Not a power of two - need to convert */
	  while ( sz >>= 1 )
	    sz1 <<= 1;
	  sz = sz1;
	}

        segsize = sz;
        sprintf(*ret = b_ret, "%u", sz);

	blksize_set = 1;

        return(1);
}

/*
 * Return a file size (c.f. RFC2349)
 * For netascii mode, we don't know the size ahead of time;
 * so reject the option.
 */
int
set_tsize(char *val, char **ret)
{
        static char b_ret[sizeof(off_t)*CHAR_BIT/3+2];
        off_t sz = atol(val);

	if ( !tsize_ok )
	  return 0;

        if (sz == 0)
                sz = tsize;
        sprintf(*ret = b_ret, "%lu", sz);
        return(1);
}

/*
 * Set the timeout (c.f. RFC2349)
 */
int
set_timeout(char *val, char **ret)
{
	static char b_ret[4];
	unsigned long to = atol(val);

	if ( to < 1 || to > 255 )
	  return 0;

	timeout    = to;
	rexmtval   = to;
	maxtimeout = TIMEOUT_LIMIT*to;

	sprintf(*ret = b_ret, "%lu", to);
	return(1);
}

/*
 * Parse RFC2347 style options
 */
void
do_opt(char *opt, char *val, char **ap)
{
     struct options *po;
     char *ret;

     /* Global option-parsing variables initialization */
     blksize_set = 0;
     
     if ( !*opt )
	  return;

     for (po = options; po->o_opt; po++)
	  if (!strcasecmp(po->o_opt, opt)) {
	       if (po->o_fnc(val, &ret)) {
		    if (*ap + strlen(opt) + strlen(ret) + 2 >=
			ackbuf + sizeof(ackbuf)) {
			 nak(ENOSPACE);	/* EOPTNEG? */
			 exit(0);
		    }
		    *ap = strrchr(strcpy(strrchr(strcpy(*ap, opt),'\0') + 1,
					 ret),'\0') + 1;
	       } else {
		    nak(EOPTNEG);
		    exit(0);
	       }
	       break;
	  }
     return;
}


FILE *file;

/*
 * Validate file access.  Since we
 * have no uid or gid, for now require
 * file to exist and be publicly
 * readable/writable.
 * If we were invoked with arguments
 * from inetd then the file must also be
 * in one of the given directory prefixes.
 * Note also, full path name must be
 * given as we have no login directory.
 */
int
validate_access(char *filename, int mode, struct formats *pf)
{
	struct stat stbuf;
	int	fd, wmode;
	char *cp, **dirp;

	tsize_ok = 0;

	if (!secure) {
		if (*filename != '/')
			return (EACCESS);
		/*
		 * prevent tricksters from getting around the directory
		 * restrictions
		 */
		for (cp = filename + 1; *cp; cp++)
			if(*cp == '.' && strncmp(cp-1, "/../", 4) == 0)
				return(EACCESS);
		for (dirp = dirs; *dirp; dirp++)
			if (strncmp(filename, *dirp, strlen(*dirp)) == 0)
				break;
		if (*dirp==0 && dirp!=dirs)
			return (EACCESS);
	}

	/*
	 * We use different a different permissions scheme if `cancreate' is
	 * set.
	 */
	wmode = O_TRUNC;
	if (stat(filename, &stbuf) < 0) {
		if (!cancreate)
			return (errno == ENOENT ? ENOTFOUND : EACCESS);
		else {
			if ((errno == ENOENT) && (mode != RRQ))
				wmode |= O_CREAT;
			else
				return(EACCESS);
		}
	} else {
		if (mode == RRQ) {
			if ((stbuf.st_mode&(S_IREAD >> 6)) == 0)
				return (EACCESS);
			tsize = stbuf.st_size;
			/* We don't know the tsize if conversion is needed */
			tsize_ok = !pf->f_convert;
		} else {
			if ((stbuf.st_mode&(S_IWRITE >> 6)) == 0)
				return (EACCESS);
			tsize = 0;
			tsize_ok = 1;
		}
	}
	fd = open(filename, mode == RRQ ? O_RDONLY : (O_WRONLY|wmode), 0666);
	if (fd < 0)
		return (errno + 100);
	/*
	 * If the file was created, set default permissions.
	 */
	if ((wmode & O_CREAT) && fchmod(fd, 0666) < 0) {
		int serrno = errno;

		close(fd);
		unlink(filename);

		return (serrno + 100);
	}
	file = fdopen(fd, (mode == RRQ)? "r":"w");
	if (file == NULL)
		return (errno + 100);
	return (0);
}

int	timeout;
jmp_buf	timeoutbuf;

void
timer(int sig)
{

	timeout += rexmtval;
	if (timeout >= maxtimeout)
		exit(0);
	longjmp(timeoutbuf, 1);
}

/*
 * Send the requested file.
 */
void
sendfile(struct formats *pf, struct tftphdr *oap, int oacklen)
{
	struct tftphdr *dp;
	struct tftphdr *ap;    /* ack packet */
	int block = 1, size, n;

	signal(SIGALRM, timer);
	ap = (struct tftphdr *)ackbuf;

        if (oap) {
	     timeout = 0;
	     (void)setjmp(timeoutbuf);
oack:
	     if (send(peer, oap, oacklen, 0) != oacklen) {
		  syslog(LOG_ERR, "tftpd: oack: %m\n");
		  goto abort;
	     }
	     for ( ; ; ) {
		  alarm(rexmtval);
		  n = recv(peer, ackbuf, sizeof(ackbuf), 0);
		  alarm(0);
		  if (n < 0) {
		       syslog(LOG_ERR, "tftpd: read: %m\n");
		       goto abort;
		  }
		  ap->th_opcode = ntohs((u_short)ap->th_opcode);
		  ap->th_block = ntohs((u_short)ap->th_block);
		  
		  if (ap->th_opcode == ERROR) {
		       syslog(LOG_ERR, "tftp: client does not accept "
			      "options\n");
		       goto abort;
		  }
		  if (ap->th_opcode == ACK) {
		       if (ap->th_block == 0)
			    break;
		       /* Resynchronize with the other side */
		       (void)synchnet(peer);
		       goto oack;
		  }
	     }
        }

	dp = r_init();
	do {
		size = readit(file, &dp, pf->f_convert);
		if (size < 0) {
			nak(errno + 100);
			goto abort;
		}
		dp->th_opcode = htons((u_short)DATA);
		dp->th_block = htons((u_short)block);
		timeout = 0;
		(void) setjmp(timeoutbuf);

send_data:
		if (send(peer, dp, size + 4, 0) != size + 4) {
			syslog(LOG_ERR, "tftpd: write: %m");
			goto abort;
		}
		read_ahead(file, pf->f_convert);
		for ( ; ; ) {
			alarm(rexmtval);	/* read the ack */
			n = recv(peer, ackbuf, sizeof (ackbuf), 0);
			alarm(0);
			if (n < 0) {
				syslog(LOG_ERR, "tftpd: read(ack): %m");
				goto abort;
			}
			ap->th_opcode = ntohs((u_short)ap->th_opcode);
			ap->th_block = ntohs((u_short)ap->th_block);

			if (ap->th_opcode == ERROR)
				goto abort;
			
			if (ap->th_opcode == ACK) {
				if (ap->th_block == block) {
					break;
				}
				/* Re-synchronize with the other side */
				(void) synchnet(peer);
				if (ap->th_block == (block -1)) {
					goto send_data;
				}
			}

		}
		block++;
	} while (size == segsize);
abort:
	(void) fclose(file);
}

void
justquit(int sig)
{
	exit(0);
}


/*
 * Receive a file.
 */
void
recvfile(struct formats *pf, struct tftphdr *oap, int oacklen)
{
	struct tftphdr *dp;
	struct tftphdr *ap;    /* ack buffer */
	int block = 0, acksize, n, size;

	signal(SIGALRM, timer);
	dp = w_init();
	do {
		timeout = 0;
		
		if (!block++ && oap) {
		     ap = (struct tftphdr *)ackbuf;
		     acksize = oacklen;
		} else {
		     ap = (struct tftphdr *)ackbuf;
		     ap->th_opcode = htons((u_short)ACK);
		     ap->th_block = htons((u_short)block);
		     acksize = 4;
		}
		(void) setjmp(timeoutbuf);
send_ack:
		if (send(peer, ackbuf, acksize, 0) != acksize) {
			syslog(LOG_ERR, "tftpd: write(ack): %m");
			goto abort;
		}
		write_behind(file, pf->f_convert);
		for ( ; ; ) {
			alarm(rexmtval);
			n = recv(peer, dp, PKTSIZE, 0);
			alarm(0);
			if (n < 0) {		/* really? */
				syslog(LOG_ERR, "tftpd: read: %m");
				goto abort;
			}
			dp->th_opcode = ntohs((u_short)dp->th_opcode);
			dp->th_block = ntohs((u_short)dp->th_block);
			if (dp->th_opcode == ERROR)
				goto abort;
			if (dp->th_opcode == DATA) {
				if (dp->th_block == block) {
					break;   /* normal */
				}
				/* Re-synchronize with the other side */
				(void) synchnet(peer);
				if (dp->th_block == (block-1))
					goto send_ack;		/* rexmit */
			}
		}
		/*  size = write(file, dp->th_data, n - 4); */
		size = writeit(file, &dp, n - 4, pf->f_convert);
		if (size != (n-4)) {			/* ahem */
			if (size < 0) nak(errno + 100);
			else nak(ENOSPACE);
			goto abort;
		}
	} while (size == segsize);
	write_behind(file, pf->f_convert);
	(void) fclose(file);		/* close data file */

	ap->th_opcode = htons((u_short)ACK);    /* send the "final" ack */
	ap->th_block = htons((u_short)(block));
	(void) send(peer, ackbuf, 4, 0);

	signal(SIGALRM, justquit);      /* just quit on timeout */
	alarm(rexmtval);
	n = recv(peer, buf, sizeof (buf), 0); /* normally times out and quits */
	alarm(0);
	if (n >= 4 &&			/* if read some data */
	    dp->th_opcode == DATA &&    /* and got a data block */
	    block == dp->th_block) {	/* then my last ack was lost */
		(void) send(peer, ackbuf, 4, 0);     /* resend final ack */
	}
abort:
	return;
}

struct errmsg {
	int	e_code;
	char	*e_msg;
} errmsgs[] = {
	{ EUNDEF,	"Undefined error code" },
	{ ENOTFOUND,	"File not found" },
	{ EACCESS,	"Access violation" },
	{ ENOSPACE,	"Disk full or allocation exceeded" },
	{ EBADOP,	"Illegal TFTP operation" },
	{ EBADID,	"Unknown transfer ID" },
	{ EEXISTS,	"File already exists" },
	{ ENOUSER,	"No such user" },
	{ EOPTNEG,	"Failure to negotiate RFC2347 options" },
	{ -1,		0 }
};

/*
 * Send a nak packet (error message).
 * Error code passed in is one of the
 * standard TFTP codes, or a UNIX errno
 * offset by 100.
 */
void
nak(int error)
{
	struct tftphdr *tp;
	int length;
	struct errmsg *pe;

	tp = (struct tftphdr *)buf;
	tp->th_opcode = htons((u_short)ERROR);
	tp->th_code = htons((u_short)error);
	for (pe = errmsgs; pe->e_code >= 0; pe++)
		if (pe->e_code == error)
			break;
	if (pe->e_code < 0) {
		pe->e_msg = strerror(error - 100);
		tp->th_code = EUNDEF;   /* set 'undef' errorcode */
	}
	strcpy(tp->th_msg, pe->e_msg);
	length = strlen(pe->e_msg);
	tp->th_msg[length] = '\0';
	length += 5;
	if (send(peer, buf, length, 0) != length)
		syslog(LOG_ERR, "nak: %m");
}
