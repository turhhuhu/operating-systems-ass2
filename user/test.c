#include "kernel/signals.h"
#include "user.h"


void sahandler1(int sig){
    printf("ka1ki\n");
}

void sahandler2(int sig){
    printf("ka2ki\n");
}

void sahandler3(int sig){
    printf("ka3ki\n");
}

int main(int argc, char const *argv[])
{
    int pid = getpid();
    pid = fork();
    if(pid == 0){
        printf("in child\n");
        printf("going to sleep\n");
        sleep(10);
        printf("waki waki\n");
        exit(0);
    }
    kill(pid, SIGSTOP);
    sleep(50);
    printf("before sig cont\n");
    //kill(pid, SIGCONT);
    printf("after sig cont\n");
    wait(&pid);
    exit(0);
}


