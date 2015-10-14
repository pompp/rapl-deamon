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
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>
#include "struct.h"
//#define   CODE_DEBUG 0
#define   PIDFILE   "/var/tmp/rapld.pid"
#define   DEFAULT_INTERVAL   200      // 1000 = 10.00s, 100 = 1.0s 

#ifdef   COREi
#undef   MSR_DRAM_ENERGY_STATUS
#define  MSR_DRAM_ENERGY_STATUS   MSR_PP1_ENERGY_STATUS
static   int   cores[NUM_CPUS]={0,0};
#else
static   int   cores[NUM_CPUS]={0,12}; 
#endif

static   double power_units[NUM_CPUS];
static   double energy_units[NUM_CPUS];
static   double time_units[NUM_CPUS];
static   int    msr_fds[NUM_CPUS];

static   int measuring;                    /* 未測定(0), 測定中(1) */
static   double   dpackage_start[NUM_CPUS];  /* 測定開始時の値 */
static   double   dpp0_start[NUM_CPUS];      /* 測定開始時の値 */
static   double   ddram_start[NUM_CPUS];     /* 測定開始時の値 */
static   int      ipackage_carry[NUM_CPUS];  /* 桁あふれ回数, for overflow case */
static   int      ipp0_carry[NUM_CPUS];     /* 桁あふれ回数 */
static   int      idram_carry[NUM_CPUS];    /* 桁あふれ回数 */
static   double   dpackage_prev[NUM_CPUS];   /* MSR 前回値 */
static   double   dpp0_prev[NUM_CPUS];      /* MSR 前回値 */
static   double   ddram_prev[NUM_CPUS];     /* MSR 前回値 */
static   int      interval_sec;            /* 記録間隔(sec) */
static   int      interval_usec;           /* 記録間隔(usec) */

static   char   _curr_time[32];           /* currtime() */

static   int   num_cores;                 /* コア数 */
static   int   core_number[MAXCORES];      /* コア番号 */

// 20150701 cao added

static double   dpackage_last[NUM_CPUS];
static double   dpp0_last    [NUM_CPUS];
static double   ddram_last   [NUM_CPUS];
static double   pwpackage_now[NUM_CPUS];
static double   pwpp0_now    [NUM_CPUS];
static double   pwdram_now   [NUM_CPUS];
static int      adaptive_pkg_pwcap = 0;    //  > 1 if adaptive pw cap
static int      fixed_pw_cap = 0;         // = fixed power cap value
static double   deltaP = 1.0;             // delta P for the adaptive pwcap
static double   pwcap_now[NUM_CPUS];       // current pw cap, use in adaptive pw cap
#define PW_CAP_MIN 50
#define PW_CAP_MAX 110

//struct	timeval	tv_tick_last,tv_tick_now;
// 20150701 cao added ended

/***********************************************************************
  MSR を読む(その都度、MSR を open/close する)
***********************************************************************/
long long   read_msr(icore,imsr)   /* ret: value or -1(error) */
int   icore;      /* I: core number */
int   imsr;      /* I: MSR number */
{
   char   fname[BUFSIZ];
   int   fd;
   int   ret;
   uint64_t   data;

   snprintf(fname,BUFSIZ,"/dev/cpu/%d/msr",icore);
   fd=open(fname,O_RDONLY);
   if (fd<0) return -1;

   ret=pread(fd,&data,sizeof data,imsr);
   close(fd);
   if (ret!= sizeof data) return -1;

   return (long long)data;
}

/***********************************************************************
  MSR に書く(その都度、MSR を open/close する)
***********************************************************************/
int   write_msr(icore,imsr,data)   /* ret: 0(ok), -1(error) */
int   icore;      /* I: core number */
int   imsr;      /* I: MSR number */
uint64_t   data;   /* I: value to set MSR */
{
   char   fname[BUFSIZ];
   int   fd;
   int   ret;

   snprintf(fname,BUFSIZ,"/dev/cpu/%d/msr",icore);
   fd=open(fname,O_WRONLY);
   if (fd<0) return -1;

   ret=pwrite(fd,&data,sizeof data,imsr);
   close(fd);
   if (ret!= sizeof data) return -1;

   return 0;
}

/***********************************************************************
  消費電力量(J)を求める
***********************************************************************/
int      get_joule(dpackage,dpp0,ddram)   /* ret: 0(ok), -1(error) */
double   dpackage[NUM_CPUS];      /* O: Package (raw data) */
double   dpp0[NUM_CPUS];         /* O: PP0 (raw data) */
double   ddram[NUM_CPUS];        /* O: DRAM (raw data) */
{
   int   i;
   int   ret;
   long long   result;

   for(i=0;i<NUM_CPUS;i++) {
      dpackage[i]=0.0;
      dpp0[i]=0.0;
      ddram[i]=0.0;
   }
   for(i=0;i<NUM_CPUS;i++) {
      ret=pread(msr_fds[i],&result,sizeof result,MSR_PKG_ENERGY_STATUS);
      if (ret!=sizeof result) return -1;
      dpackage[i]=(double)(result & 0xffffffff)*energy_units[i];
      if (dpackage[i]<dpackage_prev[i]) ipackage_carry[i]++;
      dpackage_prev[i]=dpackage[i];

      ret=pread(msr_fds[i],&result,sizeof result,MSR_PP0_ENERGY_STATUS);
      if (ret!=sizeof result) return -1;
      dpp0[i]=(double)(result & 0xffffffff)*energy_units[i];
      if (dpp0[i]<dpp0_prev[i]) ipp0_carry[i]++;
      dpp0_prev[i]=dpp0[i];

      ret=pread(msr_fds[i],&result,sizeof result,MSR_DRAM_ENERGY_STATUS);
      if (ret!=sizeof result) return -1;
      ddram[i]=(double)(result & 0xffffffff)*energy_units[i];
      if (ddram[i]<ddram_prev[i]) idram_carry[i]++;
      ddram_prev[i]=ddram[i];
   }
   return 0;
}

/***********************************************************************
  クロック周波数(MHz)を指定する
***********************************************************************/
int   set_cpufreq(gov,mhz)      /* ret: 0(ok), -1(error) */
char  *gov;                    /* I: scaling_governor */
int   mhz;                     /* I: MHz, 0 */
{
   FILE   *fp;
   char   fname[BUFSIZ];
   int   i;
   int   khz;
   int   p;

   for(i=0;i<num_cores;i++) {
      snprintf(fname,BUFSIZ,"/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor",core_number[i]);
      fp=fopen(fname,"w");
      if (fp==NULL) continue;
      fprintf(fp,"%s",gov);
      fclose(fp);
      if (mhz>0) {
         snprintf(fname,BUFSIZ,"/sys/devices/system/cpu/cpu%d/cpufreq/scaling_setspeed",core_number[i]);
         fp=fopen(fname,"w");
         if (fp==NULL) continue;
         fprintf(fp,"%d",mhz*1000);
         fclose(fp);
      }
   }
}

/***********************************************************************
  クロック周波数(MHz)を取得する
***********************************************************************/
int   get_cpufreq(buf,n)      /* ret: 0(ok), -1(error) */
char   *buf;            /* O: strings */
int   n;            /* I: size of buf */
{
   FILE   *fp;
   char   fname[BUFSIZ];
   int   i;
   int   khz;
   int   p;

   memset(buf,0,n);
   p=0;
   for(i=0;i<num_cores;i++) {
      snprintf(fname,BUFSIZ,"/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_cur_freq",core_number[i]);
      fp=fopen(fname,"r");
      if (fp==NULL) continue;
      fscanf(fp,"%d",&khz);
      fclose(fp);
      
      p+=snprintf(&buf[p],BUFSIZ-p,"%d ",khz/1000);
   }
   if (p>0) {
      buf[p-1]=0;
      return 0;
   }
   else {
      return -1;
   }
}

/***********************************************************************
  消費電力量(J)を出力する(SIGALRM で起動)
***********************************************************************/
void   putJ()
{
   struct   timeval   tv;
   struct   tm       nowtm;
   char   buf[BUFSIZ];
   char   add[BUFSIZ];
   int   fd;
   int   i;
   int   ret;
   int   p;
   double   dpackage[NUM_CPUS];
   double   dpp0[NUM_CPUS];
   double   ddram[NUM_CPUS];
   struct   itimerval   timer;

   timer.it_value.tv_sec =interval_sec;
   timer.it_value.tv_usec=interval_usec;
   timer.it_interval.tv_sec =interval_sec;
   timer.it_interval.tv_usec=interval_usec;
   
   if (setitimer(ITIMER_REAL,&timer,NULL) < 0){
      perror("setitimer");
   }

   gettimeofday(&tv,NULL);
   ret=get_joule(dpackage,dpp0,ddram);
   if (ret<0) return;

   (void)localtime_r(&(tv.tv_sec),&nowtm);
   snprintf(buf,BUFSIZ,"%s/rapl.%04d%02d%02d.log",RAPL_LOGDIR, //defined in struct.h: "/var/log/rsch"
      nowtm.tm_year+1900,nowtm.tm_mon+1,nowtm.tm_mday);
   fd=open(buf,O_WRONLY | O_CREAT | O_APPEND,0600);
   if (fd>=0) {
      memset(buf,0,BUFSIZ);
      p=snprintf(buf,BUFSIZ,"%02d:%02d:%02d.%06d ",
         nowtm.tm_hour,nowtm.tm_min,nowtm.tm_sec,tv.tv_usec);
      for(i=0;i<NUM_CPUS;i++) {
         p+=snprintf(&buf[p],BUFSIZ-p,"%.6f %.6f %.6f ",
            dpackage[i],dpp0[i],ddram[i]);
      }
      /* (追加)クロック周波数 */
      ret=get_cpufreq(add,BUFSIZ);
      if (ret==0) {
         p+=snprintf(&buf[p],BUFSIZ-p,"%s",add);
      }

      p+=snprintf(&buf[p],BUFSIZ-p,"\n");
      write(fd,buf,++p);
      close(fd);
   }
   return;
}
 // 20150701, cao added
void   putPw()
{
   struct   timeval   tv;
   struct   tm   nowtm;
   char   buf[BUFSIZ];
   //char   add[BUFSIZ];
   int   fd;
   int   i;
   int   ret;
   int   p;
   double   dpackage[NUM_CPUS];
   double   dpp0[NUM_CPUS];
   double   ddram[NUM_CPUS];
   struct   itimerval   timer;

   timer.it_value.tv_sec =interval_sec;
   timer.it_value.tv_usec=interval_usec;
   timer.it_interval.tv_sec =interval_sec;
   timer.it_interval.tv_usec=interval_usec;
   if (setitimer(ITIMER_REAL,&timer,NULL) < 0){
      perror("setitimer");
   }

   gettimeofday(&tv,NULL);
   // get raw joule data
   ret=get_joule(dpackage,dpp0,ddram);
   
   //gettimeofday(&tv_tick_now,NULL);
   
   if (ret<0) return;

   double dt=1.0/((double)(interval_sec)+ (double)(interval_usec)*0.000001);

   // cao: update the power now
   for(i=0;i<NUM_CPUS;i++) {
   
      if (dpackage[i] < dpackage_last[i]) pwpackage_now[i] = (((double)(0xffffffff)+1.0)*energy_units[i] + dpackage[i]-dpackage_last[i])*dt;
      else                              pwpackage_now[i] = (dpackage[i]-dpackage_last[i])*dt;
      if (dpp0[i] < dpp0_last[i])        pwpp0_now[i]    = (((double)(0xffffffff)+1.0)*energy_units[i] + dpp0[i]-dpp0_last[i])*dt;
      else                              pwpp0_now[i]    = (dpp0[i]-dpp0_last[i])*dt;
      if (ddram[i] <ddram_last[i])       pwdram_now[i]   = (((double)(0xffffffff)+1.0)*energy_units[i] + ddram[i]-ddram_last[i])*dt;
      else                              pwdram_now[i]   = (ddram[i]-ddram_last[i])*dt;
      pwcap_now[i]    = pwpackage_now[i] + deltaP;
   }    

   // cao 20150727, set adaptive pwcap here
   if (adaptive_pkg_pwcap)
   {
      long long   data; 
      for(i=0;i<NUM_CPUS;i++) {
         if (pwcap_now[i] < PW_CAP_MIN) pwcap_now[i] = PW_CAP_MIN;
         if (pwcap_now[i] > PW_CAP_MAX) pwcap_now[i] = PW_CAP_MAX;
         // write the adaptive pw cap
         data&=0xffffffffffff0000UL;
         data|=0x8000+((int)(pwcap_now[i]/power_units[i])&0x7fff);
         ret=write_msr(cores[i],MSR_PKG_POWER_LIMIT,data);   
      }
   }
   
   (void)localtime_r(&(tv.tv_sec),&nowtm);
   snprintf(buf,BUFSIZ,"%s/rapl_pw.%04d%02d%02d.log",RAPL_LOGDIR, //RAPL_LOGDIR is defined in struct.h: "/var/log/rsch"
      nowtm.tm_year+1900,nowtm.tm_mon+1,nowtm.tm_mday);
   fd=open(buf,O_WRONLY | O_CREAT | O_APPEND,0600);
   if (fd>=0) {
      memset(buf,0,BUFSIZ);
      p=snprintf(buf,BUFSIZ,"%02d:%02d:%02d.%06d ",
         nowtm.tm_hour,nowtm.tm_min,nowtm.tm_sec,tv.tv_usec);
      for(i=0;i<NUM_CPUS;i++) {
         p+=snprintf(&buf[p],BUFSIZ-p,"%.2f %.2f %.2f ", pwpackage_now[i],pwpp0_now[i],pwdram_now[i]);
      }

      char strtmp[32];
      if (adaptive_pkg_pwcap)    sprintf(strtmp,"adaptive %.2f %.2f", pwcap_now[0], pwcap_now[1]);
      else if (fixed_pw_cap > 0) sprintf(strtmp,"fixed %i", fixed_pw_cap);
      else                      sprintf(strtmp,"no_pwcap");

      p+=snprintf(&buf[p],strlen(strtmp)+1, strtmp);
      p+=snprintf(&buf[p],BUFSIZ-p,"\n");
      write(fd,buf,++p);
      close(fd);
   }
   for(i=0;i<NUM_CPUS;i++) {
      dpackage_last[i] = dpackage[i];
      dpp0_last[i]     = dpp0[i];
      ddram_last[i]    = ddram[i];
   }
   //gettimeofday(&tv_tick_last,NULL);            
   return;
}

/***********************************************************************
  文字列→IPアドレス
***********************************************************************/
unsigned int   getip(buf)   /* ret: 32bit IP address */
char   *buf;               /* I:   IP address string */
{
   unsigned int   ip;
   int   i1,i2,i3,i4;
   int   ret;

   ret=sscanf(buf,"%d.%d.%d.%d",&i1,&i2,&i3,&i4);
   if (ret!=4) {
      fprintf(stderr,
         "Illegal IPv4 address %s\n",
         (buf==NULL ? "(NULL)" : buf)
      );
      return 0;
   }
   i1 &= 0xff;
   i2 &= 0xff;
   i3 &= 0xff;
   i4 &= 0xff;
   ip=(i1<<24)+(i2<<16)+(i3<<8)+i4;
   return ip;
}

/***********************************************************************
  usage
***********************************************************************/
void   usage(pname)
char   *pname;
{
   fprintf(stderr,"Usage: %s server-IPv4 socket-port\n",pname);
   fprintf(stderr,"       server-IPv4 must be in number, not hostname.\n");
   fprintf(stderr,"       socket-port must be greater than 1023.\n");
   exit(-1);
}

/***********************************************************************
  デーモン化
***********************************************************************/
void dodaemon()
{
   int   fd;

   if (fork()) {
      _exit(EXIT_SUCCESS);
   }
   setsid();
   fd=open("/dev/null",O_RDWR);
   dup2(fd,STDIN_FILENO);
   dup2(fd,STDOUT_FILENO);
   dup2(fd,STDERR_FILENO);
   if (fd>STDERR_FILENO) {
      close(fd);
   }
}

/***********************************************************************
  pidファイル作成
***********************************************************************/
void   pidfile()
{
   int   fd;
   pid_t   pid;
   char   buf[32];
   int   len;

   // PIDFILE is defined as "/var/tmp/rapld.pid" above
   fd=open(PIDFILE,O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd<0) return;
   pid=getpid();
   memset(buf,0,32);
   len=snprintf(buf,32,"%u\n",pid);
   write(fd,buf,len);
   close(fd);
}

/***********************************************************************
  時刻文字列作成
***********************************************************************/
char   *currtime()
{
   time_t      t;   /* time */
   struct   tm   *nowtm;   /* time for log */

   t=time(NULL);
   nowtm=localtime(&t);
   memset(_curr_time,0,32);
   sprintf(_curr_time,"%04d/%02d/%02d %02d:%02d:%02d",
      nowtm->tm_year+1900,
      nowtm->tm_mon+1,nowtm->tm_mday,
      nowtm->tm_hour,nowtm->tm_min,nowtm->tm_sec
   );
   return _curr_time;
}

/***********************************************************************
  受信関数(ソケット通信)
***********************************************************************/
int   RECV(s,buf,len)
int   s;
char   buf[];
int   len;
{
   int   recvlen;
   int   recv_count=0;
   int   loop=0;

   while(len > 0) {
      recvlen=recv(s,&buf[recv_count],len,0);
      len -= recvlen;
      recv_count += recvlen;
      if (recvlen==0) loop++;
      else loop=0;
      if (loop>100) {
         fprintf(stderr,"%s: trap, len=%d\n",currtime(),len);
         break;
      }
   }
   return recv_count;
}

/***********************************************************************
  RAPL情報を送る
***********************************************************************/
void   _send_rapl_info_cao(f)
int   f;
{
   int   i;
   int   ret;
   int   err=0;
   double   dpackage0[NUM_CPUS],dpackage1[NUM_CPUS];
   double   dpp00[NUM_CPUS],dpp01[NUM_CPUS];
   double   ddram0[NUM_CPUS],ddram1[NUM_CPUS];
   double   dt;
   long long   data;
   struct   timeval   tv0,tv1;
   struct   node_info   node;

   gettimeofday(&tv0,NULL);
   ret=get_joule(dpackage0,dpp00,ddram0);
   if (ret<0) err=1;

   for(i=0;i<NUM_CPUS;i++) {
      data=read_msr(cores[i],MSR_PKG_POWER_LIMIT);
      data&=0x000000000000ffffUL;
      node.pkg_limit[i]=(data>>15)*(data & 0x7fffUL)*power_units[i];
      data=read_msr(cores[i],MSR_PP0_POWER_LIMIT);
      data&=0x000000000000ffffUL;
      node.pp0_limit[i]=(data>>15)*(data & 0x7fffUL)*power_units[i];
      data=read_msr(cores[i],MSR_DRAM_POWER_LIMIT);
      data&=0x000000000000ffffUL;
      node.dram_limit[i]=(data>>15)*(data & 0x7fffUL)*power_units[i];
   }
         
   for(i=0;i<NUM_CPUS;i++) {
      node.pkg_watts[i]=pwpackage_now[i];
      node.pp0_watts[i]=pwpp0_now[i];//(dpp01[i]-dpp00[i])*dt;
      node.dram_watts[i]=pwdram_now[i];//(ddram1[i]-ddram0[i])*dt;
   }
#ifdef      CODE_DEBUG   
   printf("%.6f %.6f %.6f %.6f %.6f %.6f \n", node.pkg_watts[0], node.pp0_watts[0], node.dram_watts[0], node.pkg_watts[1], node.pp0_watts[1], node.dram_watts[1]);
#endif   
   ret=send(f,&(node.pkg_limit[0]),sizeof(double)*NUM_CPUS*2*3,0);
   return;
}

/***********************************************************************
  実処理部分(本体)
***********************************************************************/
int   doit(f)      /* main program */
int   f;           /* I: socket */
{
   unsigned char   creq[4];   /* request data */
   unsigned char   buf[256];   /* reply buffer */
   int   ret;
   int   i;
   int   p;
   int   ival;
   int   mhz;
   long long   data;      /* work (MSR) */
   double   dpackage[NUM_CPUS];
   double   dpp0[NUM_CPUS];
   double   ddram[NUM_CPUS];

   /* receive function */
   ret=RECV(f,creq,4);
   if (ret!=4) {
      fprintf(stderr,"%s: error on receiving request data. ret=%d\n"
         ,currtime(),ret);
      close(f);
      return 0;
   }

   /* creq 復号処理？ */

#ifdef   CODE_DEBUG
   printf("%s received request code is 0x%02x  %02x  %02x  %02x\n",
      currtime(),creq[0],creq[1],creq[2],creq[3]);
#endif

   switch(creq[0]) {
       case REQ_SET_PKG_RAPL_ADAPTIVE:
         // cao: clear rapl setting and set adaptive_pkg_pwcap = 1
         for(i=0;i<NUM_CPUS;i++) {
            data=read_msr(cores[i],MSR_PKG_POWER_LIMIT);
            data&=0xffffffffffff0000UL;
            ret=write_msr(cores[i],MSR_PKG_POWER_LIMIT,data);
            data=read_msr(cores[i],MSR_PP0_POWER_LIMIT);
            data&=0xffffffffffff0000UL;
            ret=write_msr(cores[i],MSR_PP0_POWER_LIMIT,data);
            data=read_msr(cores[i],MSR_DRAM_POWER_LIMIT);
            data&=0xffffffffffff0000UL;
            ret=write_msr(cores[i],MSR_DRAM_POWER_LIMIT,data);
         }       
         adaptive_pkg_pwcap = 1;
         fixed_pw_cap = 0;
#ifdef      CODE_DEBUG
      printf("REQ_SET_PKG_RAPL_ADAPTIVE \n");
#endif       
       break;
       case REQ_SET_PKG_RAPL:   /* 精度向上が必要 */
       adaptive_pkg_pwcap = 0;
       fixed_pw_cap = (int)(creq[1]);
#ifdef      CODE_DEBUG
      printf("REQ_SET_PKG_RAPL %d watts\n",creq[1]);
#endif
      for(i=0;i<NUM_CPUS;i++) {
         data=read_msr(cores[i],MSR_PKG_POWER_LIMIT);
#ifdef         CODE_DEBUG
         printf("MSR_PKG_POWER_LIMIT=0x%llx",data);
#endif
         data&=0xffffffffffff0000UL;
         data|=0x8000+((int)(creq[1]/power_units[i])&0x7fff);
         ret=write_msr(cores[i],MSR_PKG_POWER_LIMIT,data);
#ifdef         CODE_DEBUG
         printf("->0x%llx, ret=%d\n",data,ret);
#endif
      }
      break;
       case REQ_SET_PP0_RAPL:
#ifdef      CODE_DEBUG
      printf("REQ_SET_PP0_RAPL %d watts\n",creq[1]);
#endif
      for(i=0;i<NUM_CPUS;i++) {
         data=read_msr(cores[i],MSR_PP0_POWER_LIMIT);
#ifdef         CODE_DEBUG
         printf("MSR_PP0_POWER_LIMIT=0x%llx",data);
#endif
         data&=0xffffffffffff0000UL;
         data|=0x8000+((int)(creq[1]/power_units[i])&0x7fff);
         ret=write_msr(cores[i],MSR_PP0_POWER_LIMIT,data);
#ifdef         CODE_DEBUG
         printf("->0x%llx, ret=%d\n",data,ret);
#endif
      }
      break;
       case REQ_SET_DRAM_RAPL:
#ifdef      CODE_DEBUG
      printf("REQ_SET_DRAM_RAPL %d watts\n",creq[1]);
#endif
      for(i=0;i<NUM_CPUS;i++) {
         data=read_msr(cores[i],MSR_DRAM_POWER_LIMIT);
#ifdef         CODE_DEBUG
         printf("MSR_DRAM_POWER_LIMIT=0x%llx",data);
#endif
         data&=0xffffffffffff0000UL;
         data|=0x8000+((int)(creq[1]/power_units[i])&0x7fff);
         ret=write_msr(cores[i],MSR_DRAM_POWER_LIMIT,data);
#ifdef         CODE_DEBUG
         printf("->0x%llx, ret=%d\n",data,ret);
#endif
      }
      break;
       case REQ_CLEAR_RAPL:
         adaptive_pkg_pwcap = 0;
         fixed_pw_cap = 0;       
#ifdef      CODE_DEBUG
      printf("REQ_CLEAR_RAPL\n");
#endif
      for(i=0;i<NUM_CPUS;i++) {
         data=read_msr(cores[i],MSR_PKG_POWER_LIMIT);
         data&=0xffffffffffff0000UL;
         ret=write_msr(cores[i],MSR_PKG_POWER_LIMIT,data);
         data=read_msr(cores[i],MSR_PP0_POWER_LIMIT);
         data&=0xffffffffffff0000UL;
         ret=write_msr(cores[i],MSR_PP0_POWER_LIMIT,data);
         data=read_msr(cores[i],MSR_DRAM_POWER_LIMIT);
         data&=0xffffffffffff0000UL;
         ret=write_msr(cores[i],MSR_DRAM_POWER_LIMIT,data);
      }
      break;
       case REQ_START_MEASURE:
#ifdef      CODE_DEBUG
      printf("REQ_START_MEASURE\n");
#endif
      measuring=1;
      ret=get_joule(dpackage_start,dpp0_start,ddram_start);
      if (ret<0) measuring=0;
      for(i=0;i<NUM_CPUS;i++) {
         ipackage_carry[i]=0;
         ipp0_carry[i]=0;
         idram_carry[i]=0;
#ifdef         CODE_DEBUG
         printf("CPU    %d: %.6f %.6f %.6f\n",i,
            dpackage_start[i],dpp0_start[i],ddram_start[i]);
#endif
      }
      break;
       case REQ_STOP_MEASURE:
#ifdef      CODE_DEBUG
      printf("REQ_STOP_MEASURE\n");
#endif
      ret=get_joule(dpackage,dpp0,ddram);
      if (ret<0) measuring=0;
      if (measuring<1) {
#ifdef         CODE_DEBUG
         printf("no data\n");
#endif
         memset(buf,0,creq[1]*sizeof(double));
         ret=send(f,buf,creq[1]*sizeof(double),0);
         break;
      }
      p=0;
      for(i=0;i<NUM_CPUS;i++) {
         dpackage[i]+=((double)(0xffffffff)+1.0)*energy_units[i]*ipackage_carry[i]-dpackage_start[i];
         dpp0[i]+=((double)(0xffffffff)+1.0)*energy_units[i]*ipp0_carry[i]-dpp0_start[i];
         ddram[i]+=((double)(0xffffffff)+1.0)*energy_units[i]*idram_carry[i]-ddram_start[i];
         memcpy(&buf[p],&dpackage[i],sizeof(double));
         p+=sizeof(double);
         memcpy(&buf[p],&dpp0[i],sizeof(double));
         p+=sizeof(double);
         memcpy(&buf[p],&ddram[i],sizeof(double));
         p+=sizeof(double);
#ifdef         CODE_DEBUG
         printf("CPU    %d: %.6f %.6f %.6f\n",i,
            dpackage[i],dpp0[i],ddram[i]);
#endif
      }
      ret=send(f,buf,creq[1]*sizeof(double),0);
      measuring=0;
      break;
       case REQ_RAPL_INFO:
#ifdef      CODE_DEBUG
      printf("REQ_RAPL_INFO\n");
#endif
      _send_rapl_info_cao(f);
      break;
       case REQ_SET_INTERVAL:
      ival=creq[1]+(creq[2]<<8)+(creq[3]<<16);
#ifdef      CODE_DEBUG
      printf("REQ_SET_INTERVAL %.2fs\n",ival*1.0/100.0);
#endif
      interval_sec=ival / 100;
      interval_usec=(ival % 100)*10000;
      
      //putJ();
      putPw(); // 20150701: cao added
      break;
       case REQ_FREQ_ONDEMAND:
#ifdef      CODE_DEBUG
      printf("REQ_FREQ_ONDEMAND\n");
#endif
      ret=set_cpufreq("ondemand",0);
      break;
       case REQ_FREQ_CONSERVATIVE:
#ifdef      CODE_DEBUG
      printf("REQ_FREQ_CONSERVATIVE\n");
#endif
      ret=set_cpufreq("conservative",0);
      break;
       case REQ_FREQ_POWERSAVE:
#ifdef      CODE_DEBUG
      printf("REQ_FREQ_POWERSAVE\n");
#endif
      ret=set_cpufreq("powersave",0);
      break;
       case REQ_FREQ_PERFORMANCE:
#ifdef      CODE_DEBUG
      printf("REQ_FREQ_PERFORMANCE\n");
#endif
      ret=set_cpufreq("performance",0);
      break;
       case REQ_FREQ_USERSPACE:
      mhz=creq[2]*256+creq[1];
#ifdef      CODE_DEBUG
      printf("REQ_FREQ_USERSPACE, %dMHz\n",mhz);
#endif
      ret=set_cpufreq("userspace",mhz);
      break;
   }
   close(f);
   return 0;
}

/***********************************************************************
  子プロセスの刈り取り
***********************************************************************/
void   reapchild()
{
   wait(0);
   signal(SIGCHLD, reapchild);
}

/***********************************************************************
  メイン関数
***********************************************************************/
int   main(argc,argv)
int   argc;
char   *argv[];
{
   unsigned int   ip;    /* listening IP */
   unsigned int   ip1;   /* connecting IP */
   int      port;       /* listening port */
   int      f;          /* listening socket */
   int      ret;        /* work */
   struct   sockaddr_in   from;
   struct   sockaddr_in   ssin;
   int      i,j;       /* work */
   int      fd;        /* work */
   char      fname[BUFSIZ];   /* work */
   long long result;          /* work (MSR) */
   
#ifdef   CODE_DEBUG
   double   thermal_spec_power,minimum_power,maximum_power,time_window;
#endif
   struct   stat   stat_p;   /* work */

   /* IP filter: not use 
   if (argc<3) usage(argv[0]);
   ip=getip(argv[1]);
   if (ip==0) {
      fprintf(stderr,"Illegal IP address. %s\n",argv[1]);
      usage(argv[0]);
   }
   port=atoi(argv[2]);
   if (port<1024) {
      fprintf(stderr,"Illegal port number. %s\n",argv[2]);
      usage(argv[0]);
   }

#ifdef   CODE_DEBUG
   printf("IP address  [%s] -> %d.%d.%d.%d\n",argv[1],
      (ip & 0xff000000) >> 24,
      (ip & 0x00ff0000) >> 16,
      (ip & 0x0000ff00) >>  8,
      (ip & 0x000000ff)
   ); 
   fprintf(stderr,"socket port [%s] -> %d\n",argv[2],port);
#endif
   */
   
   port=SOCK_PORT; //SOCK_PORT is defined in struct.h

   /* set time zone environment */
   setenv("TZ","JST-9",1);

   // cao: 20150619: automatic find number of the core
   int nCoreOnline = sysconf(_SC_NPROCESSORS_ONLN);
   int nCore = sysconf(_SC_NPROCESSORS_CONF);
   if ((nCore == 16) || (nCore == 32))
   {
      cores[1] = 8;
   }
   else if ((nCore == 20) || (nCore == 40))
   {
      cores[1] = 10;
   }   
   else if ((nCore == 24) || (nCore == 48))
   {
      cores[1] = 12;
   }
   else if (nCore <= 12) // single socket
   {
      cores[1] = 0;
   }
   // cao: 20150619: end
#ifdef   CODE_DEBUG
   fprintf(stderr,"cores[0,1] = [%d, %d]\n",cores[0], cores[1]);
#endif  
   //num_cores = nCore;
   /* コア数カウント */
   num_cores=0;
   for(i=0;i<MAXCORES;i++) {
      snprintf(fname,BUFSIZ,"/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor",i);
      ret=stat(fname,&stat_p);
      if (ret==0) {
         core_number[num_cores]=i;
         num_cores++;
      }
   }
   
#ifdef   CODE_DEBUG
   fprintf(stderr,"number of cores=%d\n",num_cores);
#endif

   /* デーモン化(CODE_DEBUG 時はデーモン化しない) */
#if   !defined(CODE_DEBUG)
   dodaemon();
#endif

   /* pid file */
   pidfile();

   /* MSR 準備 */
   for(i=0;i<NUM_CPUS;i++) {
      snprintf(fname,BUFSIZ,"/dev/cpu/%d/msr",cores[i]);
      fd=open(fname,O_RDONLY);
      if (fd<0) {
         fprintf(stderr,"open error, core:%d\n",cores[i]);
         for(j=0;j<i;j++) close(msr_fds[j]);
         return -1;
      }
      msr_fds[i]=fd;
      dpackage_prev[i]=0.0;
      dpp0_prev[i]=0.0;
      ddram_prev[i]=0.0;
   }

   for(i=0;i<NUM_CPUS;i++) {
      ret=pread(msr_fds[i],&result,sizeof result,MSR_RAPL_POWER_UNIT);
      if (ret!=sizeof result) {
         fprintf(stderr,"Error on reading MSR\n");
         for(j=0;j<NUM_CPUS;j++) close(msr_fds[j]);
         return -1;
      }
  
      /* Power Units (bits 3:0) */
      power_units[i]=pow(0.5,(double)(result&0xf));
      /* Energy Status Units (bits 12:8) */
      energy_units[i]=pow(0.5,(double)((result>>8)&0x1f));
      /* Time Units (bits 19:16) */
      time_units[i]=pow(0.5,(double)((result>>16)&0xf));

#ifdef      CODE_DEBUG
      fprintf(stderr,"core %2d :\n",cores[i]);
      fprintf(stderr,"  Power Units = %.3fW\n",power_units[i]);
      fprintf(stderr,"  Energy Units = %.8fJ\n",energy_units[i]);
      fprintf(stderr,"  Time Units = %.8fs\n",time_units[i]);

      ret=pread(msr_fds[i],&result,sizeof result,MSR_PKG_POWER_INFO);
      thermal_spec_power=power_units[i]*(double)(result&0x7fff);
      fprintf(stderr,"  Package thermal spec: %.3fW\n",thermal_spec_power);
      minimum_power=power_units[i]*(double)((result>>16)&0x7fff);
      fprintf(stderr,"  Package minimum power: %.3fW\n",minimum_power);
      maximum_power=power_units[i]*(double)((result>>32)&0x7fff);
      fprintf(stderr,"  Package maximum power: %.3fW\n",maximum_power);
      time_window=time_units[i]*(double)((result>>48)&0x7fff);
      fprintf(stderr,"  Package maximum time window: %.3fs\n",time_window);
#endif
   }

   /* socket setup */
   ssin.sin_port=htons(port);
   ssin.sin_family=AF_INET;
   ssin.sin_addr.s_addr=0;

   f=socket(AF_INET,SOCK_STREAM,0);
   if (f<0) {
      fprintf(stderr,"%s: Can't get socket.\n",argv[0]);
      return -1;
   }

   if (bind(f,(struct sockaddr *)&ssin,sizeof(ssin)) < 0) {
      fprintf(stderr,"%s: Can't bind.\n",argv[0]);
      perror("bind");
      return -1;
   }

   /* signal 処理 */
   interval_sec=(DEFAULT_INTERVAL) / 100;
   interval_usec=(DEFAULT_INTERVAL % 100)*10000;
   //signal(SIGCHLD,reapchild);   /* 子プロセス刈り取り */
   //signal(SIGALRM,putJ);      /* 定期実行 */
   //putJ();
   signal(SIGALRM,putPw);      /* 定期実行 */
   putPw(); // 20150701: cao added

#ifdef   CODE_DEBUG
   printf("%s: listening...\n",currtime());
#endif
   listen(f,SOMAXCONN);

   /* server process */
   for(;;) {
      int      g;      /* communication socket */
      socklen_t   wk;   /* work */

      wk=sizeof(from); // struct sockaddr_in from

#ifdef      CODE_DEBUG
      printf("%s: accepting...\n", currtime());
#endif
      g=accept(f,(struct sockaddr *)&from,&wk); /* accept wait */
#ifdef      CODE_DEBUG
      printf("%s: accepted\n",currtime());
#endif
      if (g<0) {
         if (errno == EINTR) continue;
         fprintf(stderr,"%s: accept error ret=%d",currtime(),g);
         fprintf(stderr,",errno=%d(%s)\n",errno,strerror(errno));
         continue;
      }

      /* IP filtering */
      ip1=ntohl((from.sin_addr).s_addr);
#ifdef      CODE_DEBUG
      printf("%s: connecting IP address : %d.%d.%d.%d\n",
         currtime(),
         (ip1 & 0xff000000) >> 24,
         (ip1 & 0x00ff0000) >> 16,
         (ip1 & 0x0000ff00) >>  8,
         (ip1 & 0x000000ff)
      ); 
#endif
      /* IP filter は使用しない
      if (ip1!=ip) {
         fprintf(stderr,"%s: illegal connection ",currtime());
         fprintf(stderr,"from %d.%d.%d.%d\n",
            (ip1 & 0xff000000) >> 24,
            (ip1 & 0x00ff0000) >> 16,
            (ip1 & 0x0000ff00) >>  8,
            (ip1 & 0x000000ff)
         );
         close(g);
         continue;
      }
      */

      ret=doit(g);
   }

   close(f);
   fprintf(stderr,"%s: program end",currtime());

   return 0;
}
