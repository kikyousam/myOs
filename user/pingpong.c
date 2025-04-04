#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char* argv[])
{
    int pipe1[2],pipe2[2];
    pipe(pipe1);
    pipe(pipe2); 
    char buf[5];
    if(fork() == 0) { // 创建子进程
        close(pipe1[1]);
        close(pipe2[0]);
        read(pipe1[0], buf, 5);
        // snprintf(pid_info, sizeof(pid_info), "%d:received %s\n", getpid(), buf);
        // write(1, pid_info, strlen(pid_info));
        fprintf(1, "%d: received %s\n", getpid(), buf);
        write(pipe2[1], "pong", 5);

        close(pipe1[0]);
        close(pipe2[1]);
        exit(0);
    } else {
        close(pipe1[0]);
        close(pipe2[1]);    
        write(pipe1[1], "ping", 5);
        read(pipe2[0], buf, 5);
        // snprintf(pid_info, sizeof(pid_info), "%d:received %s\n", getpid(), buf);
        // write(1, pid_info, strlen(pid_info));
        fprintf(1, "%d: received %s\n", getpid(), buf);

        close(pipe1[1]);
        close(pipe2[0]);
        wait(0);
        exit(0);
    }
}