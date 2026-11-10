// UDP echo 服务器
// 用法: ./udp_echo_server [port] [threads]
//   threads > 1 时每个线程一个 socket（SO_REUSEPORT 绑同一端口），内核按四元组分流
#include "hvnetpp/UdpServer.h"
#include "rtclog.h"

#include <csignal>
#include <cstdlib>
#include <semaphore.h>
#include <string>

static sem_t g_quitSem;

static void onSignal(int) {
    sem_post(&g_quitSem);
}

int main(int argc, char* argv[]) {
    rtclog_init("udp_echo_server");
    rtclog_set_level(RTC_INFO);

    int port = argc > 1 ? atoi(argv[1]) : 9999;
    int threads = argc > 2 ? atoi(argv[2]) : 1;

    hvnetpp::UdpServer srv("udp-echo");
    srv.setThreadNum(threads);
    hvnetpp::UdpSocketOptions opts;
    opts.recvBufferSize = 4 * 1024 * 1024;   // 突发流量下少丢包
    opts.recvPktInfo = true;                 // 多网卡时按收包的目的 IP 回包
    srv.setSocketOptions(opts);
    if (srv.createsocket(port) < 0) {
        return 1;
    }
    srv.onMessage = [&srv](const hvnetpp::UdpPacket& pkt) {
        RTCLOG(RTC_INFO, "recv %zu bytes from %s (to %s): %.*s", pkt.len, pkt.peer.toIpPort().c_str(),
               pkt.local.toIp().c_str(), static_cast<int>(pkt.len), pkt.data);
        srv.sendto(pkt.data, pkt.len, pkt.peer, &pkt.local);
    };
    if (srv.start() != 0) {
        return 1;
    }
    RTCLOG(RTC_INFO, "udp echo server started on port %d with %d thread(s), Ctrl-C to quit", port, threads);

    sem_init(&g_quitSem, 0, 0);
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    while (sem_wait(&g_quitSem) != 0) {}

    srv.stop();
    return 0;
}
