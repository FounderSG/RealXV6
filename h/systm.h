/*
 * Random set of variables
 * used by more than one
 * routine.
 */
extern char canonb[CANBSIZ];    /* buffer for erase and kill (#@) */
extern int coremap[CMAPSIZ];    /* space for core allocation */
extern int swapmap[SMAPSIZ];    /* space for swap allocation */
extern struct inode *rootdir;   /* pointer to inode of root directory */
extern int execnt;              /* number of processes in exec */
extern int lbolt;               /* time of day in 60th not in time */
extern int time[2];             /* time in sec from 1970 */
extern int tout[2];             /* time of day of next sleep */
/*
 * The callout structure is for
 * a routine arranging
 * to be called by the clock interrupt
 * (clock.c) with a specified argument,
 * in a specified amount of time.
 * Used, for example, to time tab
 * delays on teletypes.
 */
struct callo
{
    int c_time;     /* incremental time */
    int c_arg;      /* argument to routine */
    int (*c_func)(int arg); /* routine */
};
extern struct callo callout[NCALL];
/*
 * Mount structure.
 * One allocated on every mount.
 * Used to find the super block.
 */
struct mount
{
    int m_dev;              /* device mounted */
    struct buf *m_bufp;     /* pointer to superblock */
    struct inode *m_inodp;  /* pointer to mounted on inode */
};
extern struct mount mount[NMOUNT];

extern int  mpid;       /* generic for unique process id's */
extern char runin;      /* scheduling flag */
extern char runout;     /* scheduling flag */
extern char runrun;     /* scheduling flag */
extern char curpri;     /* more scheduling */
extern int  rootdev;    /* dev of root see conf.c */
extern int  swapdev;    /* dev of swap see conf.c */
extern int  swplo;      /* block number of swap space */
extern int  nswap;      /* size of swap space */
extern int  maxmem;     /* actual max memory per process (pages) */
extern int  updlock;    /* lock for sync */
extern int  rablock;    /* block to be read ahead */

/*
 * structure of the system entry table (sysent.c)
 */
struct sysent
{
    int count;          /* argument count */
    void (*call)(void);  /* name of handler */
};
extern struct sysent sysent[64];

extern int core_cs;     /* kernel code segment, reg CS */
extern int user_dseg;   /* D-space segment for current proc: WDSEG (EXE) or p_addr*256 */

/* trap.c */
void nosys(void);
void nullsys(void);

/* subr.c */
int bmap(struct inode *ip, int bn);
int passc(char c);
int cpass(void);
void nodev(void);
int nulldev(int d, int flag);
void bcopy(void *from, void *to, int count);

/* prf.c */
void printf(char *fmt, ...);
void panic(char *s);
void prdev(char *str, int dev);
void deverror(struct buf *bp, int o1, int o2);

/* alloc.c */
void iinit(void);
struct buf *alloc(int dev);
void free(int dev, int bno);
int badblock(struct filsys *fp, int abn, int dev);
struct inode *ialloc(int dev);
void ifree(int dev, int ino);
struct filsys *getfs(int dev);
void update(void);

/* nami.c */
struct inode *namei(int (*func)(void), int flag);
int uchar(void);
int schar(void);

/* pipe.c */
void pipe(void);
void readp(struct file *rp);
void writep(struct file *rp);
void plock(struct inode *ip);
void prele(struct inode *ip);

/* bio.c */
struct buf* bread(int dev, int blkno);
struct buf* breada(int dev, int blkno, int rablkno);
void bwrite(struct buf *rbp);
void bdwrite(struct buf *rbp);
void bawrite(struct buf *rbp);
void brelse(struct buf *rbp);
struct buf* incore(int dev, int blkno);
struct buf* getblk(int dev, int blkno);
void iowait(struct buf *rbp);
void notavail(struct buf *rbp);
void iodone(struct buf *rbp);
void clrbuf(struct buf *bp);
void binit(void);
void mapfree(struct buf *bp);
void mapalloc(struct buf *bp);
int swap(int blkno, int coreaddr, int count, int rdflg);
void bflush(int dev);
void physio(void (*strat)(struct buf *), struct buf *bp, int dev, int rw);
void geterror(struct buf *bp);

/* iget.c */
struct inode* iget(int dev, int ino);
void iput(struct inode *rp);
void iupdat(struct inode *rp, int *tm);
void itrunc(struct inode *rp);
struct inode* maknode(int mode);
void wdir(struct inode *ip);

/* rdwri.c */
void readi(struct inode *ip);
void writei(struct inode *ip);
int max(uint a, uint b);
int min(uint a, uint b);
void iomove(struct buf *bp, int o, int n, int flag);

/* fio.c */
struct file *getf(int f);
void closef(struct file *fp);
void closei(struct inode *ip, int rw);
void openi(struct inode *ip, int rw);
int access(struct inode *aip, int mode);
struct inode *owner(void);
int suser(void);
int ufalloc(void);
struct file *falloc(void);

/* sys1.c */
void exec(void);
void rexit(void);
void exit(void);
void wait(void);
void fork(void);
void sbreak(void);

/* sys2.c */
void read(void);
void write(void);
void rdwr(int mode);
void open(void);
void creat(void);
void open1(struct inode *ip, int mode, int trf);
void close(void);
void seek(void);
void link(void);
void mknod(void);
void sslep(void);

/* sys3.c */
void fstat(void);
void stat(void);
void stat1(struct inode *ip, int ub);
void dup(void);
void smount(void);
void sumount(void);
int getmdev(void);

/* sys4.c */
void getswit(void);
void gtime(void);
void stime(void);
void setuid(void);
void getuid(void);
void setgid(void);
void getgid(void);
void getpid(void);
void sync(void);
void nice(void);
void unlink(void);
void chdir(void);
void chmod(void);
void chown(void);
void ssig(void);
void kill(void);
void times(void);
void psinfo(void);

/* slp.c */
void swtch(void);
void setrun(struct proc *p);
void setpri(struct proc *up);
void sched(void);
void wakeup(void *chan);
void sleep(void *chan, int pri);
int save(label_t ctx);
int newproc(void);
uint swgrow(int need);
void expand(int newsize);

/* main.c */
int estabur(int nt, int nd, int ns, int sep);

/* VMM hypercall: load user segment windows from a sureg_desc. */
struct sureg_desc {
    int taddr, tsize;   /* WIN_TEXT: first page, count */
    int daddr, dsize;   /* WIN_DATA: first page, data page count */
    int ssize;          /* WIN_DATA: stack page count (high slots) */
    int uaddr;          /* WIN_U: physical page */
    int mode;           /* 0=single-seg (WIN_U only), 1=EXE (all three windows) */
};
void sureg(struct proc *p);             /* build sureg_desc from proc; WIN_TEXT/DATA/U via HVC_SUREG */
void segflt_setup(void);               /* register _segflt_isr address with VMM (call once at boot) */
void segflt(unsigned fa, unsigned usp, unsigned uss, unsigned kctx);
                                       /* trap.c: VMM #PF handler, entered on the kernel stack from
                                        * _segflt_isr; grow() and restart, or psignal(SIGSEG) */
void privflt_setup(void);              /* register _privflt_isr address with VMM (call once at boot) */
void privflt(unsigned type, unsigned usp, unsigned uss, unsigned kctx);
                                       /* trap.c: VMM privileged-op handler, entered on the kernel
                                        * stack from _privflt_isr; psignal(SIGINS) */

/* sig.c */
void signal(struct tty *tp, int sig);
void psignal(struct proc *p, int sig);
int issig(void);
void psig(void);
int core(void);
int grow(unsigned sp);

/* malloc.c */
uint malloc(void *mp, uint size);
void mfree(void *mp, uint size, uint aa);
void coremap_init(void);

/* text.c */
void xswap(struct proc *p, int ff, uint a);
void xalloc(struct inode *ip, uint tsize);
void xfree(void);
void xccdec(struct text *xp);

/* clock.c */
extern int intr_ps;                    /* interrupted PS image for clock()'s priority gate */
void clock(int mode);
void timeout(int (*fun)(int ), int arg, int tim);

/* tty.c */
void gtty(void);
void stty(void);
void wflushtty(struct tty *atp);
void cinit(void);
void flushtty(struct tty *atp);
void ttstart(struct tty *atp);
void ttread(struct tty *atp);
void ttwrite(struct tty *atp);
int ttystty(struct tty *atp, int *av);
void ttyinput(int ac, struct tty *atp);
int getc(struct clist *pList);
int putc(char c, struct clist *pList);

/* kl.c */
int klopen(int dev, int flag);
int klclose(int dev, int flag);
int klread(int dev);
int klwrite(int dev);
int klsgtty(int dev, int *v);
void kltxintr(void);
void klrxintr(void);

/* mem.c */
int mmread(int dev);
int mmwrite(int dev);
int mmsgtty(int dev, int *v);

/* rk.c */
void rkstrategy(struct buf *abp);
void rkintr(void);
int rkread(int dev);
int rkwrite(int dev);

/* m86.asm */
void memcpy(void far *dst, const void far *src, uint n);
void memset(void far *addr, int c, uint len);
int getps(void);
void setps(int integ);
int bios_getc(void);
void bios_putc(char c);
void move_to_user_mode(uint a);
void clock_isr(void);
void uart_isr(void);
void kbd_isr(void);
void ide_isr(void);
void trap_isr(void);

/* pc.c */
void savu(struct proc *p);
void retu(struct proc *p);
void resume(struct proc *p, label_t ctx);
void uswitch(struct proc *p, label_t ctx);
void trap_epilogue(void);
void dupframe(unsigned kctx, unsigned uss, unsigned usp);
void sendsig(int p);
void spl0(void);
void spl1(void);
void spl5(void);
void spl6(void);
void spl7(void);
int fubyte(int addr);
int fuword(int addr);
int subyte(int addr, char ch);
int suword(int addr, int value);
int fuibyte(int addr);
int suibyte(int addr, char ch);
void copyseg(uint src, uint dst);
void clearseg(uint dst);
int copyout(uint srcAddr, uint dstAddr, int iSize);
int copyin(uint srcAddr, uint dstAddr, int iSize);
extern int nofault;             /* V6 nofault: armed flag the VMM reads on kernel #PF */
extern label_t nofault_env;     /* recovery context armed by the copy primitives */
void kfault(void);              /* VMM kernel-#PF redirect target: resume() out of a primitive */
void nofault_setup(void);       /* register kfault/&nofault with VMM (call once at boot) */
void dpadd(int x[2], int y);
int dpcmp(int xh, int xl, int yh, int yl);
int ldiv(int x, int y);
int lrem(int x, int y);
int lshift (int num[2], int bits);
void outport(unsigned port, unsigned val);
void outportb(unsigned port, unsigned char val);
unsigned inport(unsigned port);
unsigned char inportb(unsigned port);
void idle(void);
void putchar(char c);
void pc_init(void);

/* kbd.c */
void kbd_init(void);
int kbd_getc(void);
void kbdintr(void);

/* ide.c */
void ideio(int sector, int count, char far *buf, int cmd);
void ideintr(void);

/* uart.c */
void uart_init(void);
void uartintr(void);
void uart_putc(char c);
int uart_getc(void);
