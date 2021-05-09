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

void kaki_function(){
    printf("kaki\n");
}

int main(int argc, char const *argv[])
{




    // int pid = getpid();
    // pid = fork();
    // if(pid == 0){
    //     void *start = malloc(4000);
    //     kthread_create(kaki_function, start);
    //     int id = kthread_id();
    //     printf("id of child: %d\n", id);
    //     for(int i = 0; i < 100000000; i++){
    //         i++;
    //         i--;
    //     }
    //     exit(0);
    // }
    void *start_1 = malloc(4000);
    kthread_create(kaki_function, start_1);
    int id = kthread_id();
    printf("id of parent: %d\n", id);
    for(int i = 0; i < 100000000; i++){
            i++;
            i--;
    }
    // wait(&pid);

    // int pid = getpid();
    // pid = fork();
    // if(pid == 0){
    //     printf("in child\n");
    //     printf("going to sleep\n");
    //     sleep(10);
    //     printf("waki waki\n");
    //     exit(0);
    // }
    // kill(pid, SIGSTOP);
    // sleep(50);
    // printf("before sig cont\n");
    // kill(pid, SIGCONT);
    // printf("after sig cont\n");
    // wait(&pid);
    exit(0);
}


