#include "tju_tcp.h"
#include <string.h>

int main(int argc, char **argv) {
    // 开启仿真环境 
    startSimulation();

    tju_tcp_t* my_server = tju_socket();
    //printf("my_tcp state %d\n", my_server->state);

    tju_sock_addr bind_addr;
    bind_addr.ip = inet_network("10.0.0.1");
    bind_addr.port = 1234;

    tju_bind(my_server, bind_addr);
    
    tju_listen(my_server);

    tju_tcp_t* new_conn = tju_accept(my_server);

    // pthread_t thread_id_time = 1002;
    // int rst_timer = pthread_create(&thread_id_time, NULL, time_thread, (void*)new_conn);
    // if (rst_timer<0){
    //     printf("ERROR open timer_thread");
    //     exit(-1); 
    // }

    sleep(2);
    // tju_send(new_conn, "hello world", 12);

    for (int i=0; i<200; i++){
        char buf[MAX_DLEN];
        tju_recv(new_conn, (void*)buf, MAX_DLEN);
        printf("[RDT TEST] server recv %s", buf);
    }
    

    while(new_conn->state != CLOSED);
    printf("服务端当前状态为:%d\n",new_conn->state);
    printf("服务端关闭\n");
    return EXIT_SUCCESS;
}
