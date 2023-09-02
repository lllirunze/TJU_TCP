#include "tju_tcp.h"
#include <string.h>


int main(int argc, char **argv) {
    // 开启仿真环境 
    startSimulation();

    tju_tcp_t* my_socket = tju_socket();
    
    tju_sock_addr target_addr;
    target_addr.ip = inet_network("10.0.0.1");
    target_addr.port = 1234;

    tju_connect(my_socket, target_addr);

    sleep(1);
    
    for(int i=0;i<200;i++){
        char buf[MAX_DLEN];
        sprintf(buf , "test message%d\n", i);
        tju_send(my_socket, buf, MAX_DLEN);
    }
    
    while(1){
        if(my_socket->window.wnd_send->nextseq == my_socket->window.wnd_send->base)
            break;
    }
    printf("客户端发起close\n");
    tju_close(my_socket);
    while (my_socket->state != CLOSED);
    printf("客户端当前状态为:%d\n",my_socket->state);
    printf("客户端关闭\n");
    return EXIT_SUCCESS;
}
