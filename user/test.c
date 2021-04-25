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
    struct sigaction old_sigact;
    struct sigaction sigact;
    sigact.sa_handler = sahandler2;
    sigact.sigmask = 5;
    sigaction(15, &sigact, &old_sigact);
    int pid = getpid();
    kill(pid, 15);
    pid = fork();
    if(pid == 0){
        printf("in child\n");
        printf("going to sleep\n");
        sleep(10);
        printf("waki waki\n");
        exit(0);
    }
    sleep(10);
    kill(pid, SIGKILL);
    exit(0);
}


