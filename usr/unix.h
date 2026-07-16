typedef unsigned int uint;
typedef unsigned char uchar;

struct stat
{
    int s_dev;          /* device number	*/
    int s_inumber;      /* i-node number	*/
    int s_flags;        /* see below		*/
    char s_nlinks;      /* number of links	*/
    char s_uid;         /* owner user id	*/
    char s_gid;         /* owner group id	*/
    char s_size0;       /* file size high	*/
    int s_size1;        /* file size low	*/
    int s_addr[8];      /* block addr		*/
    int s_actime[2];    /* accessed time	*/
    int s_modtime[2];   /* modified time	*/
};

#define T_DIR  1   /* Directory */
#define T_FILE 2   /* File */
#define T_DEV  3   /* Device */

#define DIRSIZ 14

struct dirent {
  uint inum;
  char name[DIRSIZ];
};

/* signals */
#define SIGHUP  1       /* hangup */
#define SIGINT  2       /* interrupt (rubout) */
#define SIGQIT  3       /* quit (FS) */
#define SIGINS  4       /* illegal instruction */
#define SIGTRC  5       /* trace or breakpoint */
#define SIGIOT  6       /* iot */
#define SIGEMT  7       /* emt */
#define SIGFPT  8       /* floating exception */
#define SIGKIL  9       /* kill */
#define SIGBUS  10      /* bus error */
#define SIGSEG  11      /* segmentation violation */
#define SIGSYS  12      /* sys */
#define SIGPIPE 13      /* end of pipe */

#define SIG_IGN (void (*)())1

/* u_error codes */
#define EFAULT  -106
#define EPERM   -1
#define ENOENT  -2
#define ESRCH   -3
#define EINTR   -4
#define EIO     -5
#define ENXIO   -6
#define E2BIG   -7
#define ENOEXEC -8
#define EBADF   -9
#define ECHILD  -10
#define EAGAIN  -11
#define ENOMEM  -12
#define EACCES  -13
#define ENOTBLK -15
#define EBUSY   -16
#define EEXIST  -17
#define EXDEV   -18
#define ENODEV  -19
#define ENOTDIR -20
#define EISDIR  -21
#define EINVAL  -22
#define ENFILE  -23
#define EMFILE  -24
#define ENOTTY  -25
#define ETXTBSY -26
#define EFBIG   -27
#define ENOSPC  -28
#define ESPIPE  -29
#define EROFS   -30
#define EMLINK  -31
#define EPIPE   -32

/* file flags */
#define FREAD   01
#define FWRITE  02
#define FPIPE   04

int syscall(int fn, int r0, ...);
void exit(void);
void exit1(int code);
int fork(void);
int open(char *filename, int mode);
int read(int fd, void *buf, int len);
int write(int fd, void *buf, int len);
int close(int fd);
int wait(void);
int waits(int *status);
int creat(char *filename, int mode);
int link(char *filelink, char *filenew);
int unlink(char *filename);
int exec(char *file, char *argv[]);
int chdir(char *dirname);
int time(int tim[]);
int mknod(char *filename, uint mode, int dev);
int chmod(char *filename, uint mode);
int chown(char *filename, int uid, int gid);
int brk(char *addr);
char *sbrk(int incr);
int stat(char *filename, void *buf);
int seek(int fd, int offset, int flag);
int getpid(void);
int mount(char *pDevFile, char *pMountDir, int flag);
int umount(char *pDevFile);
int setuid(int uid);
int getuid(void);
int fstat(int fd, void *buf);
int stty(int fd, void *buf);
int gtty(int fd, void *buf);
int nice(int value);
int sleep(int nTicks);
int sync(void);
int kill(int pid, int signalNo);
int psinfo(int index, void *buf);
int dup(int fd);
int pipe(int fd[2]);
int setgid(int gid);
int getgid(void);
void* signal(int signalNo, void (*Handler)(void));

void free(void *ap);
void* malloc(uint nbytes);
char *ctime(int *at);
void dpadd(int *n, int a);
int dpcmp(int ah, int al, int bh, int bl);

char* strcpy(char *s, const char *t);
int strcmp(const char *p, const char *q);
uint strlen(const char *s);
char *strcat(char *s1, char *s2);
int strncmp(const char *p, const char *q, uint n);
char* strncpy(char *s, const char *t, int n);
char* safestrcpy(char *s, const char *t, int n);
char* strchr(const char *s, char c);
char* gets(char *buf, int max);
int atoi(const char *s);

void* memset(void *dst, int c, uint n);
int memcmp(const void *v1, const void *v2, uint n);
void* memmove(void *dst, const void *src, uint n);
void* memcpy(void *dst, const void *src, uint n);

#define stdin  0
#define stdout 1
#define stderr 2

void perror(char *s);
int mkdir(char *d);

/* from turboc */
int sprintf(char *buffer, const char *format, ...);
int fprintf(int fd, const char *format, ...);
int printf(const char *fmt, ...);
int getps(void);

#pragma aux (cdecl) main;
