#ifndef	_STRUCT_H_
#define	_STRUCT_H_

/* rapl-collector */
#define	SOCK_PORT	5964
#define	COLLECT_INTERVAL	60
#define RAPL_LOGDIR "/var/log/rsch"
 
/* 以下はツール内で必要な定義 */
#define	ISNULL(a)	(a==NULL ? "NULL" : a)

/* ノードの状態 */
#define	STAT_FREE	0
#define	STAT_OFFLINE	(-1)
#define	STAT_BUSY	100

/* 変数の長さ */
#define	QNAME_LENGTH	16
#define	JNAME_LENGTH	16
#define	UNAME_LENGTH	16
#define	GNAME_LENGTH	16
#define	NNAME_LENGTH	16
#define	MAXNODES	1024
#define	NUM_CPUS	2
#define	MAXCORES	256

/* MSR number */
#define MSR_RAPL_POWER_UNIT		0x606

#define MSR_PKG_POWER_LIMIT		0x610
#define MSR_PKG_ENERGY_STATUS		0x611
#define MSR_PKG_PERF_STATUS		0x613
#define MSR_PKG_POWER_INFO		0x614

#define MSR_DRAM_POWER_LIMIT		0x618
#define MSR_DRAM_ENERGY_STATUS		0x619
#define MSR_DRAM_PERF_STATUS		0x61B
#define MSR_DRAM_POWER_INFO		0x61C

#define MSR_PP0_POWER_LIMIT		0x638
#define MSR_PP0_ENERGY_STATUS		0x639
#define MSR_PP0_POLICY			0x63A
#define MSR_PP0_PERF_STATUS		0x63B

#define MSR_PP1_POWER_LIMIT		0x640
#define MSR_PP1_ENERGY_STATUS		0x641
#define MSR_PP1_POLICY			0x642

/* rapld function number */
#define	REQ_SET_PKG_RAPL	1
#define	REQ_SET_PP0_RAPL	2
#define	REQ_SET_DRAM_RAPL	3
#define	REQ_CLEAR_RAPL		4
#define	REQ_SET_PKG_RAPL_ADAPTIVE	5   // cao 201507016, for adaptive pw capping
#define	REQ_START_MEASURE	11
#define	REQ_STOP_MEASURE	12
#define	REQ_RAPL_INFO		21
#define	REQ_SET_INTERVAL	22
#define REQ_FREQ_ONDEMAND	31
#define REQ_FREQ_CONSERVATIVE	32
#define REQ_FREQ_POWERSAVE	33
#define REQ_FREQ_PERFORMANCE	34
#define REQ_FREQ_USERSPACE	35


/* 定数 */
#define	CPUFREQ_LOWER_LIMIT	800	/* MHz */
#define	CPUFREQ_UPPER_LIMIT	5000	/* MHz */

struct	queue_info	{
	char	qname[QNAME_LENGTH];
	int	stat;
	int	priority;
	int	qrunlimit;
	int	urunlimit;
	int	grunlimit;
};

struct	job_info	{
	char	jname[JNAME_LENGTH];
	char	uname[UNAME_LENGTH];
	char	gname[GNAME_LENGTH];
	char	qname[QNAME_LENGTH];
	char	stat;	/* Queued, Running, Transit, Held, Wait, Exit, Suspend, Complete */
	int	priority;
	int	nodes;
	time_t	qtime;
	time_t	etime;
	int	rapl_package;
	int	rapl_dram;
	int	rapl_pp0;
	int	rapl_interval;
};

struct	node_info	{
	char	nname[NNAME_LENGTH];
	int	stat;
	double	pkg_limit[NUM_CPUS];
	double	pkg_watts[NUM_CPUS];
	double	pp0_limit[NUM_CPUS];
	double	pp0_watts[NUM_CPUS];
	double	dram_limit[NUM_CPUS];
	double	dram_watts[NUM_CPUS];
};

#endif
