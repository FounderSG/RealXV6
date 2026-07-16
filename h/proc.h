/*
 * One structure allocated per active
 * process. It contains all data needed
 * about the process while the
 * process may be swapped out.
 * Other per process data (user.h)
 * is swapped with the process.
 */
struct  proc
{
    char    p_stat;
    char    p_flag;
    char    p_pri;      /* priority, negative is high */
    char    p_sig;      /* signal number sent to this process */
    char    p_uid;      /* user id, used to direct tty signals */
    char    p_time;     /* resident time for scheduling */
    char    p_cpu;      /* cpu usage for scheduling */
    char    p_nice;     /* nice for scheduling */
    int     p_ttyp;     /* controlling tty */
    int     p_pid;      /* unique process id */
    int     p_ppid;     /* process id of parent */
    int     p_addr;     /* address of swappable image (data block for EXE) */
    int     p_size;     /* size of swappable image (*64 bytes) */
    int     p_taddr;    /* EXE code segment base page; 0 = single-segment */
    int     p_tsize;    /* EXE code segment size in pages; 0 = single-segment */
    int     p_uaddr;    /* EXE u-area own page; single-seg uses p_addr+USIZE-1 */
    int     p_dsize;    /* EXE data+bss page count; stack pages = p_size - p_dsize */
    int     p_wchan;    /* event process is awaiting */
    struct text *p_textp;   /* pointer to text structure */
};
extern struct proc proc[NPROC];

/* stat codes */
#define SSLEEP  1       /* sleeping on high priority */
#define SWAIT   2       /* sleeping on low priority */
#define SRUN    3       /* running */
#define SIDL    4       /* intermediate state in process creation */
#define SZOMB   5       /* intermediate state in process termination */
#define SSTOP   6       /* process being traced */

/* flag codes */
#define SLOAD   01      /* in core */
#define SSYS    02      /* scheduling process */
#define SLOCK   04      /* process cannot be swapped */
#define SSWAP   010     /* process is being swapped out */
#define STRC    020     /* process is being traced */
#define SWTED   040     /* another tracing flag */
