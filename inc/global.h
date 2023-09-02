#ifndef _GLOBAL_H_
#define _GLOBAL_H_

#include <netinet/in.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "global.h"
#include <pthread.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <math.h>

// 单位是byte
#define SIZE32 4
#define SIZE16 2
#define SIZE8  1

// 一些Flag
#define NO_FLAG 0
#define NO_WAIT 1
#define TIMEOUT 2
#define TRUE 1
#define FALSE 0

// 定义最大包长 防止IP层分片
// #define MAX_DLEN 1375    // 最大包内数据长度
#define MAX_DLEN 30 	// 最大包内数据长度
#define MAX_LEN 1400 	// 最大包长度

// TCP socket 状态定义
#define CLOSED 0
#define LISTEN 1
#define SYN_SENT 2
#define SYN_RECV 3
#define ESTABLISHED 4
#define FIN_WAIT_1 5
#define FIN_WAIT_2 6
#define CLOSE_WAIT 7
#define CLOSING 8
#define LAST_ACK 9
#define TIME_WAIT 10

// TCP 拥塞控制状态
#define SLOW_START 0
#define CONGESTION_AVOIDANCE 1
#define FAST_RECOVERY 2

// TCP 接受窗口大小
// #define TCP_RECVWN_SIZE 32*MAX_DLEN
#define TCP_RECVWN_SIZE 200*MAX_DLEN 
// TCP 发送窗口大小
// #define TCP_SENDWN_SIZE 32*MAX_DLEN
#define TCP_SENDWN_SIZE 200*MAX_DLEN


// TCP 发送窗口
// 注释的内容如果想用就可以用 不想用就删掉 仅仅提供思路和灵感
typedef struct {
	uint16_t window_size;

	uint32_t base;
	uint32_t nextseq;
	int ack_cnt;
	pthread_mutex_t ack_cnt_lock;
	uint16_t rwnd; 
	int congestion_status;
	uint16_t cwnd; 
	uint16_t ssthresh; 
} sender_window_t;


// TCP 接受窗口
// 注释的内容如果想用就可以用 不想用就删掉 仅仅提供思路和灵感
typedef struct {

    //最后读取的数据位置
    uint32_t LastByteRead;
    //最后接收的数据位置 
    uint32_t LastByteRcvd;
    //预期的seq，用于判定是否收到应收到的seq
	uint32_t expect_seq;

} receiver_window_t;


// TCP 窗口 每个建立了连接的TCP都包括发送和接受两个窗口
typedef struct {
	sender_window_t* wnd_send;
  	receiver_window_t* wnd_recv;
} window_t;

typedef struct {
	uint32_t ip;
	uint16_t port;
} tju_sock_addr;

typedef struct {

    //标识计时器是否启动
	int timer_state;
    //表示是否正在测量RTT
	int computing_RTT;
	int first_time;

    //上一次启动计时器的时间
	struct timeval send_time;
    //当前时间
	struct timeval now_time;
    //测量往返时间
	struct timeval RTT;
    //平滑往返时间
	struct timeval SRTT;
    //往返时间变化
	struct timeval RTTVAR;
    //超时重传时间
	struct timeval RTO;

} sock_time;

// TJU_TCP 结构体 保存TJU_TCP用到的各种数据
typedef struct {
	int state; // TCP的状态
	tju_sock_addr bind_addr; // 存放bind和listen时该socket绑定的IP和端口
	tju_sock_addr established_local_addr; // 存放建立连接后 本机的 IP和端口
	tju_sock_addr established_remote_addr; // 存放建立连接后 连接对方的 IP和端口

	pthread_mutex_t send_lock; // 发送数据锁
	char* sending_buf; // 发送数据缓存区
	uint16_t packet_len[TCP_SENDWN_SIZE];

	pthread_mutex_t recv_lock; // 接收数据锁
	char* received_buf; // 接收数据缓存区
	uint8_t data_mark[TCP_RECVWN_SIZE]; // 标记缓冲区是否有数据 

	pthread_cond_t wait_cond; // 可以被用来唤醒recv函数调用时等待的线程
	
	window_t window; // 发送和接受窗口

    pthread_mutex_t timer_lock;	//时间锁
	sock_time timer; //记录时间

} tju_tcp_t;


#endif