#include "unix.h"

void SIGINT_Handler()
{
    printf(" Child: This is #SIGINT signal handler!\n");
}

int main(int argc, char* argv[])
{
    int r, status, pid;

    signal(SIGINT, SIGINT_Handler);
    pid = fork();
    if( pid == 0 )
    {
        printf(" Child: pid = %d, goto sleep.\n", getpid());
        r = sleep(50);
        printf(" Child: sleep returns %d.\n", r);
        perror(" Child");
        r = 0x88;
        printf(" Child: exit with code %d.\n", r);
        return r;
    }
    else
    {
        sleep(1);
        printf("Parent: Send #SIGINT to child.\n");
        r = kill(pid, SIGINT);
        if (r == -1)
            printf("Parent: SIGINT Sent failed.\n");

        r = waits(&status);
        printf("Parent: child exit code %d, wait returns %d.\n", (status >> 8) & 0377, r);
    }

    return 1;
}
