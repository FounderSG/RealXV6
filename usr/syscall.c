#include "unix.h"

#define SYS_exit        1
#define SYS_fork        2
#define SYS_read        3
#define SYS_write       4
#define SYS_open        5
#define SYS_close       6
#define SYS_wait        7
#define SYS_creat       8
#define SYS_link        9
#define SYS_unlink     10
#define SYS_exec       11
#define SYS_chdir      12
#define SYS_time       13
#define SYS_mknod      14
#define SYS_chmod      15
#define SYS_chown      16
#define SYS_break      17
#define SYS_stat       18
#define SYS_seek       19
#define SYS_getpid     20
#define SYS_mount      21
#define SYS_umount     22
#define SYS_setuid     23
#define SYS_getuid     24
#define SYS_stime      25
#define SYS_ptrace     26

#define SYS_fstat      28
#define SYS_stty       31
#define SYS_gtty       32

#define SYS_nice       34
#define SYS_sleep      35
#define SYS_sync       36
#define SYS_kill       37
#define SYS_switch     38
#define SYS_psinfo     39

#define SYS_dup        41
#define SYS_pipe       42
#define SYS_times      43
#define SYS_prof       44
#define SYS_setgid     46
#define SYS_getgid     47
#define SYS_sig        48

int syscall(int fn, int r0, ...);
int r0, r1, r3;

void exit(void)
{
    syscall(SYS_exit, 0);
}

void exit1(int code)
{
    syscall(SYS_exit, code);
}

int fork(void)
{
    return syscall(SYS_fork, -1);
}

int open(char *filename, int mode)
{
    return syscall(SYS_open, -1, filename, mode);
}

int read(int fd, void *buf, int len)
{
    return syscall(SYS_read, fd, buf, len);
}

int write(int fd, void *buf, int len)
{
    return syscall(SYS_write, fd, buf, len);
}

int close(int fd)
{
    syscall(SYS_close, fd);
    return r3;
}

int wait(void)
{
    syscall(SYS_wait, -1);
    return r0 < 0 ? -1 : r0;
}

int waits(int *status)
{
    syscall(SYS_wait, -1);
    if(status!=0) *status = r1;
    return r0 < 0 ? -1 : r0;
}

int creat(char *fileName, int mode)
{
    return syscall(SYS_creat, -1, fileName, mode);
}

int link(char *filelink, char *filenew)
{
    syscall(SYS_link, -1, filelink, filenew);
    return r3;
}

int unlink(char *filename)
{
    syscall(SYS_unlink, -1, filename);
    return r3;
}

int exec(char *file, char *argv[])
{
    return syscall(SYS_exec, -1, file, argv);
}

int chdir(char *dirname)
{
    syscall(SYS_chdir, -1, dirname);
    return r3;
}

int time(int tim[])
{
    syscall(SYS_time, -1);
    tim[0] = r0;
    tim[1] = r1;
    return 1;
}

int mknod(char *filename, uint mode, int dev)
{
    syscall(SYS_mknod, -1, filename, mode, dev);
    return r3;
}

int chmod(char *filename, uint mode)
{
    syscall(SYS_chmod, -1, filename, mode);
    return r3;
}

int chown(char *filename, int uid, int gid)
{
    int owner = (gid << 8) | uid;
    syscall(SYS_chown, -1, filename, owner);
    return r3;
}

int stat(char *filename, void *buf)
{
    syscall(SYS_stat, -1, filename, buf);
    return r3;
}

int seek(int fd, int offset, int flag)
{
    return syscall(SYS_seek, fd, offset, flag);
}

int getpid(void)
{
    return syscall(SYS_getpid, -1);
}

int mount(char *pDevFile, char *pMountDir, int flag)
{
    syscall(SYS_mount, -1, pDevFile, pMountDir, flag);
    return r3;
}

int umount(char *pDevFile)
{
    syscall(SYS_umount, -1, pDevFile);
    return r3;
}

int setuid(int uid)
{
    syscall(SYS_setuid, uid);
    return r3;
}

int getuid(void)
{
    return syscall(SYS_getuid, -1);
}

int fstat(int fd, void *buf)
{
    syscall(SYS_fstat, fd, buf);
    return r3;
}

int stty(int fd, void *buf)
{
    syscall(SYS_stty, fd, buf);
    return r3;
}

int gtty(int fd, void *buf)
{
    syscall(SYS_gtty, fd, buf);
    return r3;
}

int nice(int value)
{
    syscall(SYS_nice, value);
    return r3;
}

int sleep(int nTicks)
{
    syscall(SYS_sleep, nTicks);
    return r3;
}

int sync(void)
{
    syscall(SYS_sync, -1);
    return r3;
}

int kill(int pid, int signalNo)
{
    syscall(SYS_kill, pid, signalNo);
    return r3;
}

int psinfo(int index, void *buf)
{
    return syscall(SYS_psinfo, index, buf);
}

int dup(int fd)
{    
    return syscall(SYS_dup, fd);
}

int pipe(int fd[2])
{
    syscall(SYS_pipe, -1);
    if(r3 == 0) 
    {
        fd[0] = r0;
        fd[1] = r1;
    }
    return r3;
}

int setgid(int gid)
{
    syscall(SYS_setgid, gid);
    return r3;
}

int getgid(void)
{
    return syscall(SYS_getgid, -1);
}

void* signal(int signalNo, void (*Handler)(void))
{
    return (void *)syscall(SYS_sig, -1, signalNo, Handler);
}
