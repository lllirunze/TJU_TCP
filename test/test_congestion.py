# -*- coding: UTF-8 -*-
import time
import pdb
import socket
from fabric import Connection
import signal
import os


TEST_USER = 'vagrant'
TEST_PASS = 'vagrant'
EXITING = False


def my_exit(signum, frame):
    global EXITING
    if EXITING==True:
        print('\n正在退出测试程序, 请稍等, 不需要多次停止\n')
        return
        
    EXITING = True
    print('\n正在退出测试程序, 请稍等\n')
    with Connection(host="10.0.0.1", user=TEST_USER, connect_kwargs={'password':TEST_PASS}) as conn:
        print("[退出测试] 结束抓包进程")
        stop_tcpdump_cmd = 'sudo pkill -f "tcpdump -i enp0s8 -w /home/vagrant/server.pcap udp"'
        rst = conn.run(stop_tcpdump_cmd, timeout=10)
        if (rst.failed):
            print('[退出测试] 这里为什么会出错')
        
        print("[退出测试] 结束客户端和服务端进程运行")
        stop_server_cmd = 'sudo pkill -f "/vagrant/tju_tcp/test/rdt_server"'
        stop_client_cmd = 'sudo pkill -f "/vagrant/tju_tcp/test/rdt_client"'
        try:
            rst = conn.local(stop_client_cmd)
            if (rst.failed):
                print("[退出测试] 关闭本地测试程序失败")
        except Exception as e:
            pass 
        try:
            rst = conn.run(stop_server_cmd)
            if (rst.failed):
                print("[退出测试] 关闭服务端测试程序失败")            
        except Exception as e: 
            pass 
        
        print("[退出测试] 重置网络环境")    
        reset_networf_cmd = 'sudo tcset enp0s8 --rate 100Mbps --delay 20ms --overwrite'
        rst = conn.run(reset_networf_cmd)
        if (rst.failed):
            print("[退出测试程序] 重置服务端网络失败")    
        rst = conn.local(reset_networf_cmd)
        if (rst.failed):
            print("[退出测试程序] 重置客户端网络失败")

    exit()

def main():
    
    if socket.gethostname() == "server":
        print("只能在client端运行自动测试")
        exit()

    signal.signal(signal.SIGINT, my_exit)
    signal.signal(signal.SIGTERM, my_exit)
        
    print("====================================================================")
    print("============================开始测试================================")
    print("====================================================================")
    with Connection(host="10.0.0.1", user=TEST_USER, connect_kwargs={'password':TEST_PASS}) as conn:

        print("[自动测试] 编译提交源码")
        print("> cd /vagrant/tju_tcp && make")
        rst = conn.run("cd /vagrant/tju_tcp && make", timeout=10)
        if (rst.failed):
            print('[自动测试] 编译提交源码错误 停止测试')
            print('{"scores": {"establish_connection": 0}}')
            exit()
        print("")
        
        # 判断是否使用用户提交的新Makefile
        if os.path.exists("/vagrant/tju_tcp/test_Makefile"):
            print("[自动测试] 检测到用户提供了自己的测试Makefile, 使用用户的测试Makefile替换自带的版本")
            print("> cp /vagrant/tju_tcp/test_Makefile /vagrant/tju_tcp/test/Makefile")
            rst = conn.local("cp /vagrant/tju_tcp/test_Makefile /vagrant/tju_tcp/test/Makefile", timeout=10)
            if (rst.failed):
                print("[自动测试] 移动测试Makefile失败")
                exit()
        print("")

        print("[自动测试] 编译测试源码")
        print("> cd /vagrant/tju_tcp/test && make")
        rst = conn.run("cd /vagrant/tju_tcp/test && make", timeout=10)
        if (rst.failed):
            print('[自动测试] 编译测试源码错误 停止测试')
            print('{"scores": {"establish_connection": 0}}')
            exit()    
        print("")


        print("============================ 抓包并绘制SeqNum随Time变化图 ================================")

        # 保证没有上一次的抓包进程在运行
        stop_tcpdump_cmd = 'sudo pkill -f "tcpdump -i enp0s8 -w /home/vagrant/server.pcap udp"'
        rst = conn.run(stop_tcpdump_cmd, timeout=10)
        if (rst.failed):
            print('[抓包并绘图] 这里为什么会出错')
        
        # 保证没有之前的客户端和服务端进程运行
        stop_server_cmd = 'sudo pkill -f "/vagrant/tju_tcp/test/rdt_server"'
        stop_client_cmd = 'sudo pkill -f "/vagrant/tju_tcp/test/rdt_client"'
        try:
            rst = conn.local(stop_client_cmd)
            if (rst.failed):
                print("[抓包并绘图] 关闭本地测试程序失败")
        except Exception as e:
            pass 
        try:
            rst = conn.run(stop_server_cmd)
            if (rst.failed):
                print("[抓包并绘图] 关闭服务端测试程序失败")            
        except Exception as e: 
            pass 

        # 保证建立连接的网络环境
        reset_networf_cmd = 'sudo tcset enp0s8 --rate 100Mbps --delay 20ms --overwrite'
        conn.run(reset_networf_cmd)
        if (rst.failed):
            print("[数据传输测试] 重置服务端网络失败")
        conn.local(reset_networf_cmd)
        if (rst.failed):
            print("[数据传输测试] 重置客户端网络失败")

        print("[抓包并绘图] 清理上一次的抓包结果")
        erase_cmd = "sudo rm -f ~/server.pcap"
        rst = conn.run(erase_cmd, timeout=10)
        if (rst.failed):
            print('[抓包并绘图] 清理上一次的抓包结果错误')
            exit()
        



        print("[抓包并绘图] 开始在服务端抓包")
        start_tcpdump_cmd = "sudo tcpdump -i enp0s8 -w ~/server.pcap udp > /dev/null 2> /dev/null < /dev/null & "
        rst = conn.run(start_tcpdump_cmd, timeout=10)
        if (rst.failed):
            print('[抓包并绘图] 服务端开启抓包失败')
            conn.run(stop_tcpdump_cmd, timeout=10)
            exit()

        print("[抓包并绘图] 开启服务端和客户端")
        start_server_cmd = 'nohup sudo /vagrant/tju_tcp/test/rdt_server > /vagrant/tju_tcp/test/server.log 2>&1 < /dev/null & '
        start_client_cmd = 'nohup sudo /vagrant/tju_tcp/test/rdt_client > /vagrant/tju_tcp/test/client.log 2>&1 < /dev/null & '
        rst = conn.run(start_server_cmd, pty=False)
        if (rst.failed):
            print("[抓包并绘图] 无法运行服务端测试程序")
            exit()
        rst = conn.local(start_client_cmd, pty=False)
        if (rst.failed):
            print("[抓包并绘图] 无法运行客户端测试程序")
            exit()
        
        print("[抓包并绘图] 等待5s建立连接")
        time.sleep(5)
        
        print("[抓包并绘图] 建立连接后  设置双方的网络通讯速率 丢包率 和延迟")
        set_networf_cmd = 'sudo tcset enp0s8 --rate 100Mbps --delay 300ms --loss 10% --overwrite'
        rst = conn.run(set_networf_cmd)
        if (rst.failed):
            print("[抓包并绘图] 无法设置服务端网络")
            exit()
        rst = conn.local(set_networf_cmd)
        if (rst.failed):
            print("[抓包并绘图] 无法设置客户端网络")
            exit()


        # print("[抓包并绘图] 等待60s 进行双方通信")
        # time.sleep(60)
        print("[抓包并绘图] 等待60s 进行双方通信")
        time.sleep(60)
        
        print("[抓包并绘图] 停止抓包")
        stop_tcpdump_cmd = 'sudo pkill -f "tcpdump -i enp0s8 -w /home/vagrant/server.pcap udp"'
        rst = conn.run(stop_tcpdump_cmd, timeout=10)
        if (rst.failed):
            print('[抓包并绘图] 这里为什么会出错')


        print("[抓包并绘图] 关闭双端")
        stop_server_cmd = 'sudo pkill -f "/vagrant/tju_tcp/test/rdt_server"'
        stop_client_cmd = 'sudo pkill -f "/vagrant/tju_tcp/test/rdt_client"'
        try:
            rst = conn.local(stop_client_cmd)
            if (rst.failed):
                print("[抓包并绘图] 关闭本地测试程序失败")
        except Exception as e:
            pass 
        try:
            rst = conn.run(stop_server_cmd)
            if (rst.failed):
                print("[抓包并绘图] 关闭服务端测试程序失败")            
        except Exception as e: 
            pass 

        
        print("[抓包并绘图] 取消网络设置")
        reset_networf_cmd = 'sudo tcset enp0s8 --rate 100Mbps --delay 20ms --overwrite'
        conn.run(reset_networf_cmd)
        if (rst.failed):
            print("[数据传输测试] 重置服务端网络失败")
        conn.local(reset_networf_cmd)
        if (rst.failed):
            print("[数据传输测试] 重置客户端网络失败")


        print("[抓包并绘图] 根据抓包结果绘制图像")
        gen_graph_cmd = "sudo python3 /vagrant/tju_tcp/test/gen_graph.py "
        rst = conn.run(gen_graph_cmd)
        if (rst.failed):
            print("[抓包并绘图] 绘图失败")
            exit()


if __name__ == "__main__":
    main()
