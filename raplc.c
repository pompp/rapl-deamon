/*The PomPP software including library and tool is developed by the PomPP (Power 
Managemant Framework for Post-Petascale Supoercomputers) research project supported
by the JST, CREST research project.  The copyrights below are in no particular order
and generally reflect members of the PomPP code team who have contributed code to
this release.   The copyrights for code used under license from other parties are
included in the corresponding files.

Copyright (c) 2015,	"The PomPP research team" supported by the JST, CREST
			research program.   All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
* Redistributions of source code must retain the above copyright notice, 
this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright notice, 
this list of conditions and the following disclaimer in the documentation 
and/or other materials provided with the distribution.
* Neither the name of the <organization> nor the names of its contributors 
may be used to endorse or promote products derived from this software 
without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.                  */

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include <time.h>
#include <inttypes.h>
#include <string.h>
#include "struct.h"

#define	FLAG0_PKG	16        // cao: for adaptive pkg power capping
#define	FLAG1_PKG	1
#define	FLAG1_PP0	2
#define	FLAG1_DRAM	4
#define	FLAG1_CLEAR	8
#define	FLAG2_START	1
#define	FLAG2_STOP	2

/***********************************************************************
  usage
***********************************************************************/
void	usage(pname)
char	*pname;
{
	fprintf(stderr,"Usage: %s [cmd] hostname1 [hostname2...]\n",pname);
	fprintf(stderr,"cmd:   -P port          port number\n");
   fprintf(stderr,"       -a               set adaptive PKG power capping\n");   
	fprintf(stderr,"       -p watts         set PKG  limit\n");
	fprintf(stderr,"       -c watts         set PP0  limit\n");
	fprintf(stderr,"       -d watts         set DRAM limit\n");
	fprintf(stderr,"       -r               remove limits\n");
	fprintf(stderr,"       -s               start measurement\n");
	fprintf(stderr,"       -t               stop  measurement\n");
	fprintf(stderr,"       -i time          interval (10ms)\n");
	fprintf(stderr,"       -v               show current data\n");
	fprintf(stderr,"       -f freq_param    set CPU speed\n");
	fprintf(stderr,"        ondemand | conservative | powersave | performance | value[MHz]\n");
	exit(-1);
}

/***********************************************************************
  受信関数(ソケット通信)
***********************************************************************/
int	RECV(s,buf,len)
int	s;
char	buf[];
int	len;
{
	int	recvlen;
	int	recv_count=0;
	int	loop=0;

	while(len > 0) {
		recvlen=recv(s,&buf[recv_count],len,0);
		len -= recvlen;
		recv_count += recvlen;
		if (recvlen==0) loop++;
		else loop=0;
		if (loop>100) {
			fprintf(stderr,"trap, len=%d\n",len);
			break;
		}
	}
	return recv_count;
}

/***********************************************************************
  送信関数(ソケット通信)
***********************************************************************/
int	SEND(sock,hostname,buf,len,opt)
struct sockaddr_in	*sock;
char	*hostname;
char	*buf;
int	len;
int	opt;
{
	int	sd;
	int	ret;
	int	i;
	double	val[32];		/* receive data */

	sd=socket(AF_INET, SOCK_STREAM, 0);
	if (sd<0) {
		fprintf(stderr,"can't get socket.\n");
		return -1;
	}
	if (connect(sd,(struct sockaddr *)sock,sizeof(struct sockaddr_in))<0) {
		fprintf(stderr,"can't connect to %s\n",hostname);
		fprintf(stderr,"Reason: %s\n",strerror(errno));
		close(sd);
		return -1;
	}
	ret=send(sd,buf,len,0);
	if (ret!=len) {
		fprintf(stderr,"can't send data. ret=%d != %d\n",ret,len);
		fprintf(stderr,"Reason: %s\n",strerror(errno));
		close(sd);
		return -1;
	}

	if (opt==1) {
		ret=RECV(sd,val,buf[1]*sizeof(double));
		for(i=0;i<NUM_CPUS;i++) {
			printf("%s[%d] %.6f %.6f %.6f\n",hostname,i,
				val[i*3],val[i*3+1],val[i*3+2]);
		}
	}
	if (opt=='v') {
		struct	node_info	node;
		ret=RECV(sd,&(node.pkg_limit[0]),sizeof(double)*NUM_CPUS*2*3);
		for(i=0;i<NUM_CPUS;i++) {
			printf("%s[%d] PKG  limit=%.6f, consume=%.6f\n",
				hostname,i,node.pkg_limit[i],node.pkg_watts[i]);
			printf("%s[%d] PP0  limit=%.6f, consume=%.6f\n",
				hostname,i,node.pp0_limit[i],node.pp0_watts[i]);
			printf("%s[%d] DRAM limit=%.6f, consume=%.6f\n",
				hostname,i,node.dram_limit[i],node.dram_watts[i]);
		}
	}
	close(sd);
	return 0;
}

/***********************************************************************
  cpuspeed オプション処理
***********************************************************************/
int	chk_cpuspeed(arg)  /* opt */
char	*arg;				/* I: arg */
{
	int	mhz;

	if (strcmp(arg,"ondemand"    )==0) return REQ_FREQ_ONDEMAND;
	if (strcmp(arg,"conservative")==0) return REQ_FREQ_CONSERVATIVE;
	if (strcmp(arg,"powersave"   )==0) return REQ_FREQ_POWERSAVE;
	if (strcmp(arg,"performance" )==0) return REQ_FREQ_PERFORMANCE;
	mhz=atoi(arg);
	if (mhz<CPUFREQ_LOWER_LIMIT || mhz>CPUFREQ_UPPER_LIMIT) {
		fprintf(stderr,"Illegal option '-f %s'\n",arg);
		mhz=0;
	}
	return mhz;
}

/***********************************************************************
  メイン
***********************************************************************/
int	main(argc,argv)
int	argc;
char	*argv[];
{
	int	i,j;	/* work */
	int	c;
	int	port;
	int	pkg,pp0,dram;
	int	flag_rapl;
	int	flag_joule;
	int	flag_i;
	int	flag_v;
	int	flag_f;
	int	ret;
	int	sd;
	unsigned char	creq[4];		   /* request data */
	extern	char	*optarg;		      /* getopt() */
	extern	int	 optind,opterr,optopt;	/* getopt() */
	struct sockaddr_in	sock;
	struct hostent		*hp;		   /* /etc/hosts */

	/* 引数チェック */
	port=SOCK_PORT; // SOCK_PORT is defined on struct.h
	pkg=0;
	pp0=0;
	dram=0;
	flag_rapl=0;
	flag_joule=0;
	flag_i=0;
	flag_v=0;
	flag_f=0;
	while((c=getopt(argc,argv,"P:p:c:d:i:f:rstva"))!= -1) {
		switch(c) {
		    case 'P':	/* -P port */
			 port=atoi(optarg);
			break;
		    case 'a':	/* adaptive power capping for */ 
			  flag_rapl|=FLAG0_PKG;
			break;
		    case 'p':	/* -p watts (PKG) */ 
			flag_rapl|=FLAG1_PKG;
			pkg=atoi(optarg);
			break;
		    case 'c':	/* -c watts (PP0) */
			flag_rapl|=FLAG1_PP0;
			pp0=atoi(optarg);
			break;
		    case 'd':	/* -d watts (DRAM) */
			flag_rapl|=FLAG1_DRAM;
			dram=atoi(optarg);
			break;
		    case 'r':	/* -r (remove) */
			flag_rapl|=FLAG1_CLEAR;
			break;
		    case 's':	/* -s (start) */
			flag_joule|=FLAG2_START;
			break;
		    case 't':	/* -t (stop) */
			flag_joule|=FLAG2_STOP;
			break;
		    case 'i':	/* -i (interval, 1/100 ms) */
			flag_i=atoi(optarg);
			break;
		    case 'v':	/* -v (debug) */
			flag_v=1;
			break;
		    case 'f':	/* -f cpuspeed (cpuspeed) */
			flag_f=chk_cpuspeed(optarg);
			break;
		    default:
			usage(argv[0]);
			break;
		}
	}

	/* 指定内容チェック */
   fprintf(stderr,"argc %i<=optind %i\n", argc, optind); // for cao checking only, please remove
	if (argc<=optind) {
      fprintf(stderr,"argc %i<=optind %i\n", argc, optind);
      usage(argv[0]);
   }
	if (port<1024) {
		fprintf(stderr,"Illegal port number.\n");
		usage(argv[0]);
	}
	if (((flag_rapl&FLAG1_CLEAR)!=0)&&(flag_rapl!=FLAG1_CLEAR)) {
		usage(argv[0]);
	}
	if (((flag_joule&FLAG2_START)!=0)&&(flag_joule!=FLAG2_START)) {
		usage(argv[0]);
	}
/*
	if ((flag_rapl!=0)&&(flag_joule!=0)) {
		usage(argv[0]);
	}
*/
	if (pkg<0 || pkg>200) {
		fprintf(stderr,"Illegal PKG_POWER_LIMIT %d.\n",pkg);
		usage(argv[0]);
	}
	if (pp0<0 || pp0>200) {
		fprintf(stderr,"Illegal PP0_POWER_LIMIT %d.\n",pp0);
		usage(argv[0]);
	}
	if (dram<0 || dram>200) {
		fprintf(stderr,"Illegal DRAM_POWER_LIMIT %d.\n",dram);
		usage(argv[0]);
	}

	/* ホストループ */
	for (i=optind;i<argc;i++) {
#ifdef		CODE_DEBUG
		fprintf(stderr,"host %s\n",argv[i]);
#endif
		hp=gethostbyname(argv[i]);
		if (hp==0) {
			fprintf(stderr,"unknown host %s\n",argv[i]);
			continue;
		}
		sock.sin_family = hp->h_addrtype;
		memcpy(&(sock.sin_addr),hp->h_addr,hp->h_length);
		sock.sin_port = htons(port);

		/* 引数処理 */
		if (flag_i>0) {
         // cao: xem lai cai nay
			memset(creq,0,4);
			creq[0]=REQ_SET_INTERVAL;
			creq[1]=(flag_i & 0xff);
			creq[2]=(flag_i >> 8) & 0xff;
			creq[3]=(flag_i >>16) & 0xff;
			ret=SEND(&sock,argv[i],creq,4,0);
		}
		if (flag_v!=0) {
			memset(creq,0,4);
			creq[0]=REQ_RAPL_INFO;
			ret=SEND(&sock,argv[i],creq,4,'v');
		}
		if (flag_joule==FLAG2_START) {
			memset(creq,0,4);
			creq[0]=REQ_START_MEASURE;
			ret=SEND(&sock,argv[i],creq,4,0);
		}
		if (flag_joule==FLAG2_STOP) {
			memset(creq,0,4);
			creq[0]=REQ_STOP_MEASURE;
			creq[1]=NUM_CPUS*3;
			ret=SEND(&sock,argv[i],creq,4,1);
		}
		if ((flag_rapl&FLAG1_CLEAR)==FLAG1_CLEAR) {
			memset(creq,0,4);
			creq[0]=REQ_CLEAR_RAPL;
			ret=SEND(&sock,argv[i],creq,4,0);
		}
		if ((flag_rapl&FLAG1_PKG)==FLAG1_PKG) {
			memset(creq,0,4);
			creq[0]=REQ_SET_PKG_RAPL;
			creq[1]=pkg;
			ret=SEND(&sock,argv[i],creq,4,0);
		}
		if ((flag_rapl&FLAG0_PKG)==FLAG0_PKG) {
         printf("set adaptive pw capping\n");
			memset(creq,0,4);
			creq[0]=REQ_SET_PKG_RAPL_ADAPTIVE;
			ret=SEND(&sock,argv[i],creq,4,0);
		}      
		if ((flag_rapl&FLAG1_PP0)==FLAG1_PP0) {
			memset(creq,0,4);
			creq[0]=REQ_SET_PP0_RAPL;
			creq[1]=pp0;
			ret=SEND(&sock,argv[i],creq,4,0);
		}
		if ((flag_rapl&FLAG1_DRAM)==FLAG1_DRAM) {
			memset(creq,0,4);
			creq[0]=REQ_SET_DRAM_RAPL;
			creq[1]=dram;
			ret=SEND(&sock,argv[i],creq,4,0);
		}
		if (flag_f>0 && flag_f<CPUFREQ_LOWER_LIMIT) {
			memset(creq,0,4);
			creq[0]=flag_f;
			creq[1]=(flag_i & 0xff);
			creq[2]=(flag_i >> 8) & 0xff;
			creq[3]=(flag_i >>16) & 0xff;
			ret=SEND(&sock,argv[i],creq,4,0);
		}
		if (flag_f>=CPUFREQ_LOWER_LIMIT) {
			memset(creq,0,4);
			creq[0]=REQ_FREQ_USERSPACE;
			creq[1]=(flag_f & 0xff);
			creq[2]=(flag_f >> 8) & 0xff;
			ret=SEND(&sock,argv[i],creq,4,0);
		}
	}
	return 0;
}
