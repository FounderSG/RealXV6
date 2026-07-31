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
    struct tty *p_ttyp; /* controlling tty */
    int     p_pid;      /* unique process id */
    int     p_ppid;     /* process id of parent */
    int     p_addr;     /* swappable image base page; EXE block = [u][data][stack] */
    int     p_size;     /* size of swappable image in pages; includes the u page */
    int     p_taddr;    /* EXE code segment base page; 0 = single-segment */
    int     p_tsize;    /* EXE code segment size in pages; 0 = single-segment */
    int     p_dsize;    /* EXE data+bss page count (low window slots) */
    int     p_ssize;    /* EXE stack page count (high window slots); the V6 u_ssize,
                         * kept here with tsize/dsize so sureg reads all three sizes
                         * from proc -- decoupled from p_size, which swgrow may
                         * transiently inflate as a swap reservation */
    int     p_wchan;    /* event process is awaiting */
    struct text *p_textp;   /* pointer to text structure */
};
extern struct proc proc[NPROC];

/*
 * Physical page of the process's u-area / kernel stack: for an EXE process it
 * is slot 0 of the swappable block; a single-seg process keeps it at the top
 * of its USIZE-page block (p_addr + USIZE - 1).
 */
#define UPAGE(p) ((p)->p_tsize ? (p)->p_addr : (p)->p_addr + USIZE - 1)

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
