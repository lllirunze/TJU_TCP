#include "tju_tcp.h"

/*
创建 TCP socket 
初始化对应的结构体
设置初始状态为 CLOSED
*/

//半连接列表当前数量
int SynQueue_n = 0;
//全连接列表当前数量
int AcceptQueue_n = 0;


/*
    TODO: 可靠数据传输-超时重传
*/
void* timeout_thread(void* sock){

    //用于监控定时器的线程
    printf("DEBUG: Open thread.\n");
    
    while(1){

        //定时器启用
        if(((tju_tcp_t*)sock)->timer.timer_state == 1){

            //检测到超时
            gettimeofday(&(((tju_tcp_t*)sock)->timer.now_time), NULL);

            struct timeval c = sub_time(((tju_tcp_t*)sock)->timer.now_time, ((tju_tcp_t*)sock)->timer.send_time);
            
            if(c.tv_sec > ((tju_tcp_t*)sock)->timer.RTO.tv_sec || (c.tv_sec == ((tju_tcp_t*)sock)->timer.RTO.tv_sec && c.tv_usec > ((tju_tcp_t*)sock)->timer.RTO.tv_usec) ){
                
                //超时间隔加倍
                //定时器加锁
                while(pthread_mutex_lock(&(((tju_tcp_t*)sock)->timer_lock)) != 0);
                
                ((tju_tcp_t*)sock)->timer.RTO = mul_time(((tju_tcp_t*)sock)->timer.RTO, 2);

                //定时器解锁
                pthread_mutex_unlock(&(((tju_tcp_t*)sock)->timer_lock));
                
                
                printf("WARNING CONGESTION%d: timeout interval bonus%ld.%ld\n", 
                    ((tju_tcp_t*)sock)->window.wnd_send->congestion_status, ((tju_tcp_t*)sock)->timer.RTO.tv_sec, ((tju_tcp_t*)sock)->timer.RTO.tv_usec);
                

                /*
                如果nextseq==base，就不用重传了
                这是用来防止当handle里处理到base==nextseq时，计时器终止前，已经进入了此循环，防止重传序号为nextseq的不存在数据
                */
                if(((tju_tcp_t*)sock)->window.wnd_send->nextseq == ((tju_tcp_t*)sock)->window.wnd_send->base) continue;

                ((tju_tcp_t*)sock)->window.wnd_send->ssthresh = ((tju_tcp_t*)sock)->window.wnd_send->cwnd / 2;
                ((tju_tcp_t*)sock)->window.wnd_send->cwnd = MAX_DLEN;
                
                //冗余ACK加锁
                while(pthread_mutex_lock(&(((tju_tcp_t*)sock)->window.wnd_send->ack_cnt_lock)) != 0);
                ((tju_tcp_t*)sock)->window.wnd_send->ack_cnt = 0;
                //冗余ACK解锁
                pthread_mutex_unlock(&(((tju_tcp_t*)sock)->window.wnd_send->ack_cnt_lock));

                /*
                    TODO: 拥塞控制-超时
                */
                //超时，状态改为慢启动
                ((tju_tcp_t*)sock)->window.wnd_send->congestion_status = SLOW_START;

                //定时器加锁
                while(pthread_mutex_lock(&(((tju_tcp_t*)sock)->timer_lock)) != 0);
                ((tju_tcp_t*)sock)->timer.computing_RTT = 0;
                //定时器解锁
                pthread_mutex_unlock(&(((tju_tcp_t*)sock)->timer_lock));

                /*
                printf("DEBUG CONGESTION%d: cwnd%d, rwnd%d, ssthresh%d\n",
                ((tju_tcp_t*)sock)->window.wnd_send->congestion_status, ((tju_tcp_t*)sock)->window.wnd_send->cwnd, ((tju_tcp_t*)sock)->window.wnd_send->rwnd, ((tju_tcp_t*)sock)->window.wnd_send->ssthresh);
                */

                int send_again_len = ((tju_tcp_t*)sock)->packet_len[((tju_tcp_t*)sock)->window.wnd_send->base];
                int plen = DEFAULT_HEADER_LEN + send_again_len;
                
                char* send_again_data = malloc(sizeof(char)*send_again_len);
                memcpy(send_again_data, ((tju_tcp_t*)sock)->sending_buf + ((tju_tcp_t*)sock)->window.wnd_send->base, send_again_len);
                
                char* msg = create_packet_buf(((tju_tcp_t*)sock)->established_local_addr.port, ((tju_tcp_t*)sock)->established_remote_addr.port, 
                                                ((tju_tcp_t*)sock)->window.wnd_send->base, ((tju_tcp_t*)sock)->window.wnd_recv->expect_seq, 
                                                DEFAULT_HEADER_LEN, plen, 
                                                NO_FLAG, 0 , 0, 
                                                send_again_data, send_again_len);
                sendToLayer3(msg, plen);

                printf("DEBUG: timeout retransmit packet seq%d, ack%d\n", ((tju_tcp_t*)sock)->window.wnd_send->base, ((tju_tcp_t*)sock)->window.wnd_recv->expect_seq);
                
            }
        }
    }
}


tju_tcp_t* tju_socket(){
    tju_tcp_t* sock = (tju_tcp_t*)malloc(sizeof(tju_tcp_t));
    sock->state = CLOSED;

    //初始化发送缓存区
    pthread_mutex_init(&(sock->send_lock), NULL);
    sock->sending_buf = malloc(sizeof(char)*TCP_SENDWN_SIZE);
    //初始化接收缓存区
    pthread_mutex_init(&(sock->recv_lock), NULL);
    sock->received_buf = malloc(sizeof(char)*TCP_RECVWN_SIZE);

    pthread_mutex_init(&(sock->timer_lock), NULL);

    //初始化所有标记缓冲区没有数据
    int i;
    for(i=0;i<TCP_RECVWN_SIZE;i++){
        sock->data_mark[i]=0;
    }

    if(pthread_cond_init(&sock->wait_cond, NULL) != 0){
        perror("ERROR condition variable not set\n");
        exit(-1);
    }

    //初始化发送窗口
    sock->window.wnd_send = (sender_window_t*)malloc(sizeof(sender_window_t));
    sock->window.wnd_send->base = 0;
    sock->window.wnd_send->nextseq = 0;
    sock->window.wnd_send->ack_cnt = 0;
    pthread_mutex_init(&(sock->window.wnd_send->ack_cnt_lock), NULL);
    sock->window.wnd_send->rwnd = TCP_RECVWN_SIZE;
    sock->window.wnd_send->congestion_status = SLOW_START;
    sock->window.wnd_send->cwnd = MAX_DLEN;
    sock->window.wnd_send->ssthresh = TCP_SENDWN_SIZE;
    sock->window.wnd_send->window_size = sock->window.wnd_send->cwnd;

    //初始化接收窗口
    sock->window.wnd_recv = (receiver_window_t*)malloc(sizeof(receiver_window_t));
    sock->window.wnd_recv->LastByteRead = 0;
    sock->window.wnd_recv->LastByteRcvd = 0;
    sock->window.wnd_recv->expect_seq = 0;
    
    //初始化计时器
    sock->timer.timer_state = 0;
    sock->timer.computing_RTT = 0;
    sock->timer.first_time = 1;

    sock->timer.send_time.tv_sec = 0;
    sock->timer.send_time.tv_usec = 0;
    gettimeofday(&(sock->timer.now_time), NULL);
    
    sock->timer.RTT.tv_sec = 0;
    sock->timer.SRTT.tv_sec = 0;
    sock->timer.RTTVAR.tv_sec = 0;
    sock->timer.RTT.tv_usec = 0;
    sock->timer.SRTT.tv_usec = 0;
    sock->timer.RTTVAR.tv_usec = 0;
    //RTO初始为1s
    sock->timer.RTO.tv_sec = 1;
    sock->timer.RTO.tv_usec = 0;       

    return sock;
}



/*
    TODO: 连接管理-建立连接
*/

/*
绑定监听的地址 包括ip和端口
*/
int tju_bind(tju_tcp_t* sock, tju_sock_addr bind_addr){

    //printf("Server: bind\n"); 
    sock->bind_addr = bind_addr;
    return 0;

}


/*
被动打开 监听bind的地址和端口
设置socket的状态为LISTEN
注册该socket到内核的监听socket哈希表
*/
int tju_listen(tju_tcp_t* sock){
    
    //printf("Server: Listen\n"); 
    sock->state = LISTEN;
    //printf("DEBUG：Server STATE %d.\n",sock->state); 

    //放入lhash表
    int hashval = cal_hash(sock->bind_addr.ip, sock->bind_addr.port, 0, 0);
    listen_socks[hashval] = sock;

    return 0;
}


/*
接受连接 
返回与客户端通信用的socket
这里返回的socket一定是已经完成3次握手建立了连接的socket
因为只要该函数返回, 用户就可以马上使用该socket进行send和recv
*/
tju_tcp_t* tju_accept(tju_tcp_t* sock){

    tju_tcp_t* new_conn = (tju_tcp_t*)malloc(sizeof(tju_tcp_t));

    //全连接队列为空，阻塞
    while(AcceptQueue_n <= 0);
    AcceptQueue_n = 0;
    new_conn = AcceptQueue[AcceptQueue_n];

    /*
    如果new_conn的创建过程放到了tju_handle_packet中 那么accept怎么拿到这个new_conn呢
    在linux中 每个listen socket都维护一个已经完成连接的socket队列
    每次调用accept 实际上就是取出这个队列中的一个元素
    队列为空,则阻塞 
    返回全连接队列里的一个元素   
    */

    //设置服务序号和确认号
    new_conn->window.wnd_send->base = 0;
    new_conn->window.wnd_send->nextseq = 0;
    new_conn->window.wnd_recv->LastByteRead = 0;
    new_conn->window.wnd_recv->LastByteRcvd = 0;
    new_conn->window.wnd_recv->expect_seq = 0;

    return new_conn;
}


/*
连接到服务端
该函数以一个socket为参数
调用函数前, 该socket还未建立连接
函数正常返回后, 该socket一定是已经完成了3次握手, 建立了连接
因为只要该函数返回, 用户就可以马上使用该socket进行send和recv
*/
int tju_connect(tju_tcp_t* sock, tju_sock_addr target_addr){

    //printf("Client: Connect\n");
    sock->established_remote_addr = target_addr;

    tju_sock_addr local_addr;
    local_addr.ip = inet_network("10.0.0.2");
    local_addr.port = 5678; // 连接方进行connect连接的时候 内核中是随机分配一个可用的端口
    sock->established_local_addr = local_addr;

    //客户端状态为SYN_SENT
    sock->state = SYN_SENT;

    // 将建立了连接的socket放入内核 已建立连接哈希表中
    int hashval = cal_hash(local_addr.ip, local_addr.port, target_addr.ip, target_addr.port);
    established_socks[hashval] = sock;

    //客户端发送SYN给服务端
    char* shakehand1 = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, 
                                            0, 0, 
                                            DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, 
                                            SYN_FLAG_MASK, TCP_RECVWN_SIZE, 0, 
                                            NULL, 0);
    sendToLayer3(shakehand1, DEFAULT_HEADER_LEN);

    // printf("DEBUG: Client has sent SYN\n");

    //等待接收SYNACK报文，阻塞
    while(sock->state != ESTABLISHED);

    //设置客户端序号和确认号
    sock->window.wnd_send->base = 0;
    sock->window.wnd_send->nextseq = 0;
    sock->window.wnd_recv->LastByteRead = 0;
    sock->window.wnd_recv->LastByteRcvd = 0;
    sock->window.wnd_recv->expect_seq = 0;

    // printf("Success: Client has connected!!\n");
    //开启客户端超时重传线程
    pthread_t thread_id_time = 1003;
    int rst_timer = pthread_create(&thread_id_time, NULL, timeout_thread, (void*)sock);
    if (rst_timer<0){
        // printf("ERROR: There is no thread.\n");
        exit(-1); 
    }

    return 0;
}



int tju_send(tju_tcp_t* sock, const void *buffer, int len){

    //当发送端认为接收方没有可用空间时，持续发送0字节的探测报文确认最新的rwnd。
    while(sock->window.wnd_send->rwnd < len){
        uint16_t temp_rwnd;
        temp_rwnd = TCP_RECVWN_SIZE - (sock->window.wnd_recv->expect_seq - sock->window.wnd_recv->LastByteRead);
        char* get_new_rwnd = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, 
                                            sock->window.wnd_send->nextseq , sock->window.wnd_recv->expect_seq, 
                                            DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, 
                                            GET_NEW_RWND, 0, 0, 
                                            NULL, 0);
        sendToLayer3(get_new_rwnd, DEFAULT_HEADER_LEN);
        printf("WARNING: There is not enough rwnd.\n");
    }
    
    /*
    当窗口无法接收当前所有数据时，
    所有数据全部等待发送，
    等待前面的数据收到ACK后，
    才可以退出循环，
    继续发送数据
    */

    sock->window.wnd_send->window_size = MIN(sock->window.wnd_send->rwnd, sock->window.wnd_send->cwnd);
    
    while(sock->window.wnd_send->nextseq - sock->window.wnd_send->base + len > sock->window.wnd_send->window_size){
        // printf("WARNING: There is not enough sending_buf. WAITING...\n");
        sock->window.wnd_send->window_size = MIN(sock->window.wnd_send->rwnd, sock->window.wnd_send->cwnd);
    }

    //将要发送的数据放入发送缓冲区中
    while(pthread_mutex_lock(&(sock->send_lock)) != 0); // 发送端加锁

    memcpy(sock->sending_buf + sock->window.wnd_send->nextseq, buffer, len);

    pthread_mutex_unlock(&(sock->send_lock)); // 发送端解锁

    char* send_data = malloc(len);
    memcpy(send_data, buffer, len);

    //生成TCP报文段
    char* sending_pkt = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port,
                                    sock->window.wnd_send->nextseq, sock->window.wnd_recv->expect_seq, 
                                    DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN + len, 
                                    NO_FLAG, 0, 0, 
                                    send_data, len);

    //如果定时器没有启动，则启动定时器
    if(sock->timer.timer_state == 0){
        
        start_timer(sock);
        
        //定时器加锁
        while(pthread_mutex_lock(&(sock->timer_lock)) != 0);
        sock->timer.computing_RTT = 1;
        //定时器解锁
        pthread_mutex_unlock(&(sock->timer_lock));
    }

    sendToLayer3(sending_pkt, DEFAULT_HEADER_LEN + len);

    //发送缓存区加锁
    while(pthread_mutex_lock(&(sock->send_lock)) != 0);
    sock->packet_len[sock->window.wnd_send->nextseq] = len;
    sock->window.wnd_send->nextseq += len;
    //发送缓存区解锁
    pthread_mutex_unlock(&(sock->send_lock));

    /*
    printf("DEBUG:\n\t%d：sending packet, seq%d, ack%d,\ncwnd%d, rwnd%d, ssthresh%dm, window_size%d\n",
                    sock->window.wnd_send->congestion_status, 
                    sock->window.wnd_send->nextseq-len, sock->window.wnd_recv->expect_seq, 
                    sock->window.wnd_send->cwnd, sock->window.wnd_send->rwnd, sock->window.wnd_send->ssthresh, sock->window.wnd_send->window_size);
    */

    return 0;
}



int tju_recv(tju_tcp_t* sock, void *buffer, int len){
    // printf("Waiting for receiving data...\n");
    while(sock->window.wnd_recv->expect_seq - sock->window.wnd_recv->LastByteRead < len);

    //接收缓存区加锁
    while(pthread_mutex_lock(&(sock->recv_lock)) != 0);

    memcpy(buffer, sock->received_buf + sock->window.wnd_recv->LastByteRead, len);
    sock->window.wnd_recv->LastByteRead += len;

    //接收缓存区解锁
    pthread_mutex_unlock(&(sock->recv_lock));

    return 0;
}


/*
关闭一个TCP连接
这里涉及到四次挥手
*/
int tju_close (tju_tcp_t* sock){

    /*
        TODO: 连接管理-关闭连接
    */

    //客户端发送FIN给服务端
    char* wavehand1 = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, 
                                sock->window.wnd_send->nextseq, sock->window.wnd_recv->expect_seq, 
                                DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, 
                                FIN_FLAG_MASK, 0, 0, 
                                NULL, 0);
    sendToLayer3(wavehand1, DEFAULT_HEADER_LEN);

    printf("DEBUG: Client has sent FIN(wavehand1)\n");

    //客户端改变状态为FIN_WAIT_1
    sock->state = FIN_WAIT_1;
    sock->window.wnd_send->nextseq += 1;
    
    /*
    用于延迟判断，避免死锁
    sleep(5);
    */

    return 0;

}

int tju_handle_packet(tju_tcp_t* sock, char* pkt){

    uint8_t flags = get_flags(pkt);

    /*
        TODO: 连接管理-状态转化
    */
    
    //服务端LISTEN
    if(sock->state == LISTEN){
        
        /*
        服务端收到SYN报文
        新建一个全新的socket
        */
        tju_tcp_t* new_conn = (tju_tcp_t*)malloc(sizeof(tju_tcp_t));
        memcpy(new_conn, sock, sizeof(tju_tcp_t));

        tju_sock_addr local_addr, remote_addr;
        
        remote_addr.ip = inet_network("10.0.0.2");  //具体的IP地址
        remote_addr.port = get_src(pkt);  //端口

        local_addr.ip = sock->bind_addr.ip;  //具体的IP地址
        local_addr.port = sock->bind_addr.port;  //端口

        new_conn->established_local_addr = local_addr;
        new_conn->established_remote_addr = remote_addr;
        sock->established_local_addr = local_addr;
        sock->established_remote_addr = remote_addr;

        //服务端改变状态为SYN_RECV
        new_conn->state = SYN_RECV;
        sock->state = SYN_RECV;

        //服务端把其放到LISTEN的socket的半连接队列中
        SynQueue[SynQueue_n] = new_conn;
        SynQueue_n = 1;

        //服务端将新建的socket放到ehash中
        int hashval = cal_hash(local_addr.ip, local_addr.port, remote_addr.ip, remote_addr.port);
        established_socks[hashval] = new_conn;

        //服务端向客户端发送SYN_ACK
        char* shakehand2 = create_packet_buf(new_conn->established_local_addr.port, new_conn->established_remote_addr.port, 
                                                0, 1,
                                                DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, 
                                                SYN_FLAG_MASK, TCP_RECVWN_SIZE, 0, 
                                                NULL, 0);
        sendToLayer3(shakehand2, DEFAULT_HEADER_LEN);

        // printf("DEBUG: Server has received SYN and sent SYN_ACK\n");

    }
    //客户端SYN_SENT
    else if(sock->state == SYN_SENT){
        
        // printf("DEBUG: Client has received SYN_ACK and sent ACK\n");

        //客户端收到SYN_ACK后改变状态为ESTABLISHED
        sock->state = ESTABLISHED;

        //发送ACK给server
        char* shakehand3;
        shakehand3 = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, 
                                        1, get_seq(pkt)+1, 
                                        DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, 
                                        ACK_FLAG_MASK, TCP_RECVWN_SIZE, 0, 
                                        NULL, 0);
        sendToLayer3(shakehand3, DEFAULT_HEADER_LEN);
    }
    //服务端SYN_RECV
    else if(sock->state == SYN_RECV){
        
        //服务端收到ACK后改变状态为ESTABLISHED
        sock->state = ESTABLISHED;

        //服务端将其从半连接队列删除 放到全连接队列中
        AcceptQueue[AcceptQueue_n] = sock;
        AcceptQueue_n = 1;
        SynQueue_n = 0;
        
        // printf("DEBUG: Server has received ACK\n");
    }
    //服务端ESTABLISHED
    else if(sock->state == ESTABLISHED && flags == FIN_FLAG_MASK){
        
        sock->window.wnd_recv->expect_seq = get_seq(pkt) + 1;
        
        //服务端收到FIN,发送ACK
        char* wavehand2 = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, 
                                            sock->window.wnd_send->nextseq, sock->window.wnd_recv->expect_seq, 
                                            DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, 
                                            ACK_FLAG_MASK, 0, 0,
                                            NULL, 0);
        sendToLayer3(wavehand2, DEFAULT_HEADER_LEN);
        
        printf("DEBUG: Server has received FIN and sent ACK(wavehand2)\n");

        //服务端改变状态为CLOSE_WAIT
        sock->state = CLOSE_WAIT;

        //服务端在一段时间后发送FIN
        sleep(1);

        char* wavehand3 = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, 
                                            sock->window.wnd_send->nextseq, sock->window.wnd_recv->expect_seq, 
                                            DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, 
                                            FIN_FLAG_MASK, 0, 0, 
                                            NULL, 0);
        sendToLayer3(wavehand3, DEFAULT_HEADER_LEN);

        sock->window.wnd_send->nextseq += 1;
        
        printf("DEBUG: Server has sent FIN(wavehand3)\n");

        //服务端改变状态为LAST_ACK
        sock->state = LAST_ACK;
    
    }
    //客户端FIN_WAIT_1
    else if(sock->state == FIN_WAIT_1){
        
        //客户端收到ACK
        printf("DEBUG: Client has received ACK(wavehand2)\n");
        sock->window.wnd_recv->expect_seq = get_seq(pkt) + 1;
        //客户端改变状态为FIN_WAIT_2
        sock->state = FIN_WAIT_2;
    
    }
    //客户端FIN_WAIT_2
    else if(sock->state == FIN_WAIT_2){

        //客户端收到服务端的FIN，发送ACK
        printf("DEBUG: Client has received FIN_ACK(wavehand3)\n");

        char* wavehand4 = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, 
                                            sock->window.wnd_send->nextseq, sock->window.wnd_recv->expect_seq, 
                                            DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, 
                                            ACK_FLAG_MASK, 0, 0, 
                                            NULL, 0);
        sendToLayer3(wavehand4, DEFAULT_HEADER_LEN);

        printf("DEBUG: Client has sent LAST ACK(wavehand4)\n");
        
        //客户端改变状态为TIME_WAIT
        sock->state = TIME_WAIT;

        // 等待3s,等待的时间放在close函数里,用于延迟判断,避免死锁
        // sleep(5);
        //客户端改变状态为CLOSED
        sock->state = CLOSED;

    }
    //服务端LAST_ACK
    else if(sock->state == LAST_ACK){
        
        //服务端发送FIN后接收ACK
        //等待5s
        //sleep(5);
        printf("DEBUG: Server has received LAST ACK(wavehand4)\n");
        sock->state = CLOSED;
    }
    
    /*
        TODO: 流量控制-窗口移动
    */
    else if(sock->state == ESTABLISHED){

        uint16_t data_len = get_plen(pkt) - DEFAULT_HEADER_LEN;

        //计算接收数据前的可接收大小
        uint16_t temp_rwnd;

        switch (flags){
            // 判断flags情况
            case ACK_FLAG_MASK:{
                //发送方收到正常的ACK
                if(get_ack(pkt) > sock->window.wnd_send->base){

                    /* 
                    发送方收到ACK，更新rwnd，base前移
                    因为TCP是累计确认，所以发送方收到ACK时，即可让base直接前移
                    */
                    
                    //发送端加锁
                    while(pthread_mutex_lock(&(sock->send_lock)) != 0);

                    sock->window.wnd_send->base = get_ack(pkt);
                    sock->window.wnd_send->rwnd = get_advertised_window(pkt);

                    //冗余ACK加锁
                    while(pthread_mutex_lock(&(sock->window.wnd_send->ack_cnt_lock)) != 0);
                    sock->window.wnd_send->ack_cnt = 0;
                    //冗余ACK解锁
                    pthread_mutex_unlock(&(sock->window.wnd_send->ack_cnt_lock));

                    if(sock->window.wnd_send->base != sock->window.wnd_send->nextseq){
                        //当前还有未被确认的报文段
                        if(sock->timer.computing_RTT == 1) update_timer(sock);
                        start_timer(sock);
                    }
                    else{
                        //如果当前所有报文都已经发送，则终止计时器
                        if(sock->timer.computing_RTT == 1) update_timer(sock);
                        end_timer(sock);
                    }
                    
                    /*
                        TODO: 拥塞控制-接收到正常的ACK
                    */
                    //慢启动
                    if(sock->window.wnd_send->congestion_status == SLOW_START){

                        sock->window.wnd_send->cwnd += MAX_DLEN;
                        /*
                        printf("CONGESTION %d:\n\treceive ack%d\n\tcwnd%d, rwnd%d, ssthresh%d\n",
                                sock->window.wnd_send->congestion_status, 
                                get_ack(pkt), sock->window.wnd_send->cwnd, sock->window.wnd_send->rwnd, sock->window.wnd_send->ssthresh);
                        */
                        if(sock->window.wnd_send->cwnd >= sock->window.wnd_send->ssthresh){
                            sock->window.wnd_send->congestion_status = CONGESTION_AVOIDANCE;
                            // printf("DEBUG: From SLOW_START to CONGESTION_AVOIDANCE\n");
                        }

                    }
                    //拥塞避免
                    else if(sock->window.wnd_send->congestion_status == CONGESTION_AVOIDANCE){

                        sock->window.wnd_send->cwnd += MAX_DLEN * MAX_DLEN / sock->window.wnd_send->cwnd;
                        /*
                        printf("CONGESTION %d:\n\treceive ack%d\n\tcwnd%d, rwnd%d, ssthresh%d\n",
                                sock->window.wnd_send->congestion_status, 
                                get_ack(pkt), sock->window.wnd_send->cwnd, sock->window.wnd_send->rwnd, sock->window.wnd_send->ssthresh);
                        */

                    }
                    //快速恢复
                    else{

                        sock->window.wnd_send->cwnd = sock->window.wnd_send->ssthresh;
                        /*
                        printf("CONGESTION %d:\n\treceive ack%d\n\tcwnd%d, rwnd%d, ssthresh%d\n",
                                sock->window.wnd_send->congestion_status, 
                                get_ack(pkt), sock->window.wnd_send->cwnd, sock->window.wnd_send->rwnd, sock->window.wnd_send->ssthresh);
                        */

                        sock->window.wnd_send->congestion_status = CONGESTION_AVOIDANCE;
                        // printf("DEBUG: From FAST_RECOVERY to CONGESTION_AVOIDANCE\n");
                    }

                    //发送端解锁
                    pthread_mutex_unlock(&(sock->send_lock));

                }

                /*
                    TODO: 可靠数据传输-冗余ACK
                */
                //处理冗余ACK
                else if(get_ack(pkt) == sock->window.wnd_send->base && sock->window.wnd_send->base < sock->window.wnd_send->nextseq){
                    
                    //发送缓冲区加锁
                    while(pthread_mutex_lock(&(sock->send_lock)) != 0);

                    printf("DEBUG: received duplicated ACK.\n");
                    sock->window.wnd_send->ack_cnt++;
                    sock->window.wnd_send->rwnd = get_advertised_window(pkt);

                    if(sock->window.wnd_send->congestion_status == FAST_RECOVERY){
                        
                        sock->window.wnd_send->cwnd += MAX_DLEN;
                        
                        printf("CONGESTION %d:\n\treceive depulicated ack%d\n\tcwnd%d, rwnd%d, ssthresh%d\n",
                                sock->window.wnd_send->congestion_status, 
                                get_ack(pkt), sock->window.wnd_send->cwnd, sock->window.wnd_send->rwnd, sock->window.wnd_send->ssthresh);

                    }
                    /*
                        TODO: 拥塞控制-冗余ACK
                    */
                    //3个冗余ACK
                    if(sock->window.wnd_send->ack_cnt == 3){
                        
                        //重传的分组不计算RTT，如果第一个分组就丢失的话，会触发超时/快速重传，那么就不再计算RTT了
                        sock->window.wnd_send->ack_cnt = 0;
                        sock->timer.computing_RTT = 0;
                        
                        //判断拥塞状态
                        if(sock->window.wnd_send->congestion_status == SLOW_START){
                            
                            sock->window.wnd_send->ssthresh = sock->window.wnd_send->cwnd / 2;
                            sock->window.wnd_send->cwnd = sock->window.wnd_send->ssthresh + 3 * MAX_DLEN;
                            
                            printf("CONGESTION %d:\n\treceive depulicated ack%d\n\tcwnd%d, rwnd%d, ssthresh%d\n",
                                    sock->window.wnd_send->congestion_status, 
                                    get_ack(pkt), sock->window.wnd_send->cwnd, sock->window.wnd_send->rwnd, sock->window.wnd_send->ssthresh);

                            sock->window.wnd_send->congestion_status = FAST_RECOVERY;
                            // printf("DEBUG: From SLOW_START to FAST_RECOVERY\n");
                        }
                        else if(sock->window.wnd_send->congestion_status == CONGESTION_AVOIDANCE){
                            
                            sock->window.wnd_send->ssthresh = sock->window.wnd_send->cwnd / 2;
                            sock->window.wnd_send->cwnd = sock->window.wnd_send->ssthresh + 3 * MAX_DLEN;
                            
                            printf("CONGESTION %d:\n\treceive depulicated ack%d\n\tcwnd%d, rwnd%d, ssthresh%d\n",
                                    sock->window.wnd_send->congestion_status, 
                                    get_ack(pkt), sock->window.wnd_send->cwnd, sock->window.wnd_send->rwnd, sock->window.wnd_send->ssthresh);
                            
                            sock->window.wnd_send->congestion_status = FAST_RECOVERY;
                            // printf("DEBUG: From CONGESTION_AVOIDANCE to FAST_RECOVERY\n");
                        }
                        else{
                        }
                        
                        //快速重传
                        int send_again_len = sock->packet_len[sock->window.wnd_send->base];
                        char* send_again_data = malloc(sizeof(char)*send_again_len);

                        memcpy(send_again_data, sock->sending_buf + sock->window.wnd_send->base, send_again_len);

                        char* retransmit_pkt = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, 
                                                                sock->window.wnd_send->base, sock->window.wnd_recv->expect_seq, 
                                                                DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN + send_again_len, 
                                                                NO_FLAG, 0, 0, 
                                                                send_again_data, send_again_len);
                        sendToLayer3(retransmit_pkt, DEFAULT_HEADER_LEN + send_again_len); 

                        printf("DEBUG: Fast retransmit packet seq%d, ack%d\n", sock->window.wnd_send->base, sock->window.wnd_recv->expect_seq);
                    
                    }

                    //发送缓存区解锁
                    pthread_mutex_unlock(&(sock->send_lock)); 
                    
                }
                else if(get_ack(pkt) == sock->window.wnd_send->base && sock->window.wnd_send->base == sock->window.wnd_send->nextseq){
                    
                    //发送缓存区加锁
                    while(pthread_mutex_lock(&(sock->send_lock)) != 0);
                    
                    sock->window.wnd_send->base = get_ack(pkt);
                    sock->window.wnd_send->rwnd = get_advertised_window(pkt);
                    printf("DEBUG: There is no packet in sending_buf\n\trwnd%d, base%d, nextseq%d, expect_seq%d, LastByteRead%d\n", 
                            sock->window.wnd_send->rwnd, sock->window.wnd_send->base, sock->window.wnd_send->nextseq, sock->window.wnd_recv->expect_seq, sock->window.wnd_recv->LastByteRead);
                    
                    end_timer(sock);

                    //发送缓存区解锁
                    pthread_mutex_unlock(&(sock->send_lock)); 
                }
                else{

                    // (get_ack(pkt) < sock->window.wnd_send->base)   
                    // printf("DEBUG: no deal.\n");
                
                }
                break;
            }
                
            /*
                TODO: 流量控制-探测报文
            */
            case GET_NEW_RWND:{

                /*
                此时发送端认为rwnd已经不够接收了，
                所以持续发0个字节的探测报文，
                从而使对方收到RETURN_NEW_RWND，
                以此来确认新的rwnd值
                */

                //接收缓存区加锁
                while(pthread_mutex_lock(&(sock->recv_lock)) != 0);

                int used_sum = 0;
                int i;
                for(i = sock->window.wnd_recv->expect_seq; i < sock->window.wnd_recv->LastByteRcvd; i++){
                    if(sock->data_mark[i] == 1) used_sum++;
                }
                
                temp_rwnd = TCP_RECVWN_SIZE - (sock->window.wnd_recv->expect_seq - sock->window.wnd_recv->LastByteRead) - used_sum;
                // 向对方发送ACK，并通知对方更新rwnd
                char* update_rwnd_pkt = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, 
                                                            sock->window.wnd_send->nextseq, sock->window.wnd_recv->expect_seq, 
                                                            DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, 
                                                            RETURN_NEW_RWND, temp_rwnd, 0, 
                                                            NULL, 0);
                sendToLayer3(update_rwnd_pkt, DEFAULT_HEADER_LEN);
                
                printf("DEBUG: Update sending rwnd %d\n", temp_rwnd);
                
                //接收缓存区解锁
                pthread_mutex_lock(&(sock->recv_lock));
                
                break;
            
            }
            
            case RETURN_NEW_RWND:{

                //发送方收到更新rwnd的报文段，然后更新自己的rwnd

                //发送缓存区加锁
                while(pthread_mutex_lock(&(sock->send_lock)) != 0);
                
                sock->window.wnd_send->rwnd = get_advertised_window(pkt);
                printf("DEBUG: rwnd%d\n", sock->window.wnd_send->rwnd);
                
                //发送缓存区解锁
                pthread_mutex_unlock(&(sock->send_lock));
                
                break;
            
            }

            /*
                TODO: 可靠数据传输-接收按序/失序数据
            */
            //接收方收到数据
            default:{
            
                /*
                此时接收到对方发来的数据，需要更新对方的rwnd值，temp_rwnd为接受此数据前，接收方缓冲区还有多少空余
                因为只有在发送方接收ACK报文后，才能更新rwnd值，有可能此时接收方已经没有可用空间了，
                当通知rwnd=0的报文到达时，发送方已经发送了超过接收方接收能力的报文，即temp_rwnd - data_len < 0
                */

                int used_sum = 0;
                int i;
                for(i = sock->window.wnd_recv->expect_seq; i < sock->window.wnd_recv->LastByteRcvd; i++){
                    if(sock->data_mark[i] == 1) used_sum++;
                }

                temp_rwnd = TCP_RECVWN_SIZE - (sock->window.wnd_recv->expect_seq - sock->window.wnd_recv->LastByteRead) - used_sum;
                while(temp_rwnd - data_len < 0){
                    
                    //接收不下数据，重新计算
                    // printf("WARNING: There is not enough received_buf.\n");
                    for(i = sock->window.wnd_recv->expect_seq; i < sock->window.wnd_recv->LastByteRcvd; i++){
                        if(sock->data_mark[i] == 1) used_sum++;
                    }
                    temp_rwnd = TCP_RECVWN_SIZE - (sock->window.wnd_recv->expect_seq - sock->window.wnd_recv->LastByteRead) - used_sum;
                    // sleep(1);
                }

                if(get_seq(pkt) == sock->window.wnd_recv->expect_seq){    
                    
                    //收到按序分组
                    
                    /*
                    等待一段时间，
                    如果另一个按序的报文段又到达了，则发送单个累计ACK
                    如果另一个按序的报文段没有到达了，则发送该分组的ACK
                    */
        
                    while(pthread_mutex_lock(&(sock->recv_lock)) != 0); // 接收端加锁

                    if(sock->window.wnd_recv->LastByteRcvd == sock->window.wnd_recv->expect_seq){
                        sock->window.wnd_recv->LastByteRcvd = get_seq(pkt) + data_len;
                    }

                    // 收到数据，把数据存入接收缓冲区       
                    memcpy(sock->received_buf + sock->window.wnd_recv->expect_seq, pkt + DEFAULT_HEADER_LEN, data_len);
                    
                    // 更新data_mark[]数组
                    for(i = sock->window.wnd_recv->expect_seq; i < sock->window.wnd_recv->expect_seq + data_len; i++){
                        sock->data_mark[i] = 1;
                    }
                    
                    // 查询之后的数据是否已经被缓存，更新expect_seq，再决定发送ACK的序号和确认号
                    for(i = sock->window.wnd_recv->expect_seq + data_len; i < TCP_RECVWN_SIZE; i++){
                        if(sock->data_mark[i] == 0) break;
                    }
                    sock->window.wnd_recv->expect_seq = i;

                    //计算当前可接收缓存
                    used_sum = 0;
                    for(i = sock->window.wnd_recv->expect_seq; i < sock->window.wnd_recv->LastByteRcvd; i++){
                        if(sock->data_mark[i] == 1) used_sum++;
                    }

                    temp_rwnd = TCP_RECVWN_SIZE - (sock->window.wnd_recv->expect_seq - sock->window.wnd_recv->LastByteRead) - used_sum;

                    //向对方发送ACK，并通知对方更新rwnd
                    char* ack_pkt = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, 
                                                        sock->window.wnd_send->nextseq, sock->window.wnd_recv->expect_seq, 
                                                        DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, 
                                                        ACK_FLAG_MASK, temp_rwnd, 0, 
                                                        NULL, 0);
                    sendToLayer3(ack_pkt, DEFAULT_HEADER_LEN);

                    // printf("DEBUG: received expected packet seq%d, ack%d\n", sock->window.wnd_send->nextseq, sock->window.wnd_recv->expect_seq);
                    
                    //接收缓冲区解锁
                    pthread_mutex_unlock(&(sock->recv_lock)); 
                }
                else if(sock->window.wnd_recv->expect_seq < get_seq(pkt) && get_seq(pkt) < sock->window.wnd_recv->LastByteRead + TCP_RECVWN_SIZE){
                    
                    //收到的失序分组

                    /*
                    将数据标记，
                    发送冗余ACK，
                    指示下一期待字节的序号
                    */

                    //接收缓冲区加锁
                    while(pthread_mutex_lock(&(sock->recv_lock)) != 0);

                    sock->window.wnd_recv->LastByteRcvd = get_seq(pkt) + data_len;

                    // 收到数据，把数据存入接收缓冲区       
                    memcpy(sock->received_buf + get_seq(pkt), pkt + DEFAULT_HEADER_LEN, data_len);

                    // 更新data_mark[]数组
                    for(i = get_seq(pkt); i < get_seq(pkt) + data_len; i++){
                        sock->data_mark[i] = 1;
                    }

                    used_sum = 0;
                    for(i = sock->window.wnd_recv->expect_seq; i < sock->window.wnd_recv->LastByteRcvd; i++){
                        if(sock->data_mark[i] == 1) used_sum++;
                    }

                    temp_rwnd = TCP_RECVWN_SIZE - (sock->window.wnd_recv->expect_seq - sock->window.wnd_recv->LastByteRead) - used_sum;

                    //发送冗余ACK
                    char* duplicated_ack = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, 
                                                            sock->window.wnd_send->nextseq, sock->window.wnd_recv->expect_seq, 
                                                            DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, 
                                                            ACK_FLAG_MASK, temp_rwnd, 0, 
                                                            NULL, 0);
                    sendToLayer3(duplicated_ack, DEFAULT_HEADER_LEN);
                    
                    // printf("WARNING: received unexpected packet seq%d, ack%d\n", get_seq(pkt), get_ack(pkt));
                                
                    //接收缓冲区解锁
                    pthread_mutex_unlock(&(sock->recv_lock));

                }
                else if(get_seq(pkt) < sock->window.wnd_recv->expect_seq){
                    
                    //接收到已经接收过且发送过ACK的数据
                    
                    /*
                    例如数p179图3-35
                    或者触发快速重传后，还没有接收到快速重传发来的ACK，又触发了超时重传，当接收到快速重传的ACK后，base后移，然后base前面的超时重传数据到达
                    或者发送方触发快速重传后，接收方收到数据，但返回的ACK丢失，此时接收方expect_seq已经前移，发送方以为快速重传数据又丢失了，于是继续重传数据，此时接收方必须给予回应
                    此时收到的是已被接收过的数据，但也要发送一个ACK
                    */

                    //接收缓冲区加锁
                    while(pthread_mutex_lock(&(sock->recv_lock)) != 0);

                    int used_sum = 0;
                    int i;
                    for(i = sock->window.wnd_recv->expect_seq; i < sock->window.wnd_recv->LastByteRcvd; i++){
                        if(sock->data_mark[i] == 1) used_sum++;
                    }

                    temp_rwnd = TCP_RECVWN_SIZE - (sock->window.wnd_recv->expect_seq - sock->window.wnd_recv->LastByteRead) - used_sum;
                    
                    char* sent_ack = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, 
                                                        sock->window.wnd_send->nextseq, sock->window.wnd_recv->expect_seq, 
                                                        DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, 
                                                        ACK_FLAG_MASK, temp_rwnd, 0, 
                                                        NULL, 0);
                    sendToLayer3(sent_ack, DEFAULT_HEADER_LEN);

                    // printf("WARNING: receive acked packet seq%d, ack%d", sock->window.wnd_send->nextseq, sock->window.wnd_recv->expect_seq);

                    //接收缓冲区解锁
                    pthread_mutex_unlock(&(sock->recv_lock));

                }
                //序号在窗口后的数据被发来，拒绝接收，无事发生
                else{}

                break;

            }

        }
    }
    return 0;    
}


uint16_t MIN(uint16_t a, uint16_t b){
    if(a <= b) return a;
    else return b;
}


//socket开始计时
void start_timer(tju_tcp_t* sock){

    //定时器加锁
    while(pthread_mutex_lock(&(sock->timer_lock)) != 0);
    
    sock->timer.timer_state = 1;
    gettimeofday(&(sock->timer.send_time), NULL);
    
    //定时器解锁
    pthread_mutex_unlock(&(sock->timer_lock));    

    // printf("DEBUG: Start timer, timeout interval%ld.%ld\n", sock->timer.RTO.tv_sec, sock->timer.RTO.tv_usec);

}


//socket停止计时
void end_timer(tju_tcp_t* sock){

    // printf("DEBUG: End timer.\n");
    
    //定时器加锁
    while(pthread_mutex_lock(&(sock->timer_lock)) != 0);
    
    sock->timer.timer_state = 0;
    
    //定时器解锁
    pthread_mutex_unlock(&(sock->timer_lock));
}


//socket更新时间
void update_timer(tju_tcp_t* sock){
    
    //挂起timer_thread线程
    while(pthread_mutex_lock(&(sock->timer_lock)) != 0);

    if(sock->timer.first_time == 1){

        //第一次更新定时器
        sock->timer.first_time = 0;
        sock->timer.computing_RTT = 0;
        gettimeofday(&(sock->timer.now_time), NULL);

        sock->timer.RTT = sub_time(sock->timer.now_time, sock->timer.send_time);
        sock->timer.SRTT = sock->timer.RTT;
        sock->timer.RTTVAR = mul_time(sock->timer.RTT, 0.5);
        sock->timer.RTO = add_time(sock->timer.SRTT, mul_time(sock->timer.RTTVAR, 4));

        // printf("DEBUG: First update timer.\n\tRTT%ld.%ld，SRTT%ld.%ld，RTTVAR%ld.%ld，RTO%ld.%ld\n", sock->timer.RTT.tv_sec, sock->timer.RTT.tv_usec, sock->timer.SRTT.tv_sec, sock->timer.SRTT.tv_usec, sock->timer.RTTVAR.tv_sec, sock->timer.RTTVAR.tv_usec, sock->timer.RTO.tv_sec, sock->timer.RTO.tv_usec);
    
    }
    else{

        //不是第一次更新定时器，有一点点差别
        sock->timer.computing_RTT = 0;
        gettimeofday(&(sock->timer.now_time), NULL);

        sock->timer.RTT = sub_time(sock->timer.now_time, sock->timer.send_time);
        sock->timer.SRTT = add_time(mul_time(sock->timer.SRTT, 0.875), mul_time(sock->timer.RTT, 0.125));
        sock->timer.RTTVAR = add_time(mul_time(sock->timer.RTTVAR, 0.75), mul_time(sub_time(sock->timer.SRTT, sock->timer.RTT), 0.25));
        sock->timer.RTO = add_time(sock->timer.SRTT, mul_time(sock->timer.RTTVAR, 4));

        // printf("DEBUG: Update timer.\n\tRTT%ld.%ld，SRTT%ld.%ld，RTTVAR%ld.%ld，RTO%ld.%ld\n", sock->timer.RTT.tv_sec, sock->timer.RTT.tv_usec, sock->timer.SRTT.tv_sec, sock->timer.SRTT.tv_usec, sock->timer.RTTVAR.tv_sec, sock->timer.RTTVAR.tv_usec, sock->timer.RTO.tv_sec, sock->timer.RTO.tv_usec);
    }

    pthread_mutex_unlock(&(sock->timer_lock));

}


struct timeval add_time(struct timeval a, struct timeval b){
    struct timeval c;
    long c_usec = ((long)a.tv_usec + (long)b.tv_usec) % 1000000;
    long c_sec = a.tv_sec + b.tv_sec + (((long)a.tv_usec + (long)b.tv_usec) / 1000000);
    c.tv_usec = c_usec;
    c.tv_sec = c_sec;
    return c;
}

struct timeval sub_time(struct timeval a, struct timeval b){
    struct timeval c, d, e;
    long c_sec, c_usec;
    //a>=b
    if(a.tv_sec > b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_usec >= b.tv_usec)){
        d = a;
        e = b;
    }
    else{
        d = b;
        e = a;
    }
    //d>=b
    if(d.tv_usec >= e.tv_usec){
        c.tv_sec = d.tv_sec - e.tv_sec;
        c.tv_usec = d.tv_usec - e.tv_usec;
    }
    else{
        c.tv_sec = d.tv_sec - e.tv_sec - 1;
        c.tv_usec = 1000000 - (e.tv_usec - d.tv_usec);
    }
    return c;
}

struct timeval mul_time(struct timeval a, double b){
    long c_usec = b*(double)a.tv_usec;
    long c_sec;
    a.tv_usec = c_usec % 1000000;
    a.tv_sec = (long)(a.tv_sec*b) + (c_usec / 1000000);
    return a;
}
