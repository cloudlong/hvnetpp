// TCP echo 服务器：4 个 worker 线程，可选 keepalive 超时踢空闲连接
// 用法: ./tcp_echo_server [port] [keepalive_ms]
#include "hvnetpp/TcpServer.h"
#include "rtclog.h"

#include <csignal>
#include <cstdlib>
#include <semaphore.h>

static sem_t g_quitSem;

static void onSignal(int) {
    sem_post(&g_quitSem);
}

int main(int argc, char* argv[]) {
    rtclog_init("tcp_echo_server");
    rtclog_set_level(RTC_INFO);

    int port = argc > 1 ? atoi(argv[1]) : 1234;
    int keepaliveMs = argc > 2 ? atoi(argv[2]) : 0;

    hvnetpp::TcpServer srv("echo");
    if (srv.createsocket(port) < 0) {
        return 1;
    }
    srv.setThreadNum(4);
    if (keepaliveMs > 0) {
        srv.setKeepaliveTimeout(keepaliveMs);
    }

    srv.onConnection = [&srv](const hvnetpp::SocketChannelPtr& ch) {
        if (ch->isConnected()) {
            RTCLOG(RTC_INFO, "connected: id=%u %s -> %s (total %zu)",
                   ch->id(), ch->peeraddr().c_str(), ch->localaddr().c_str(), srv.connectionNum());
        } else {
            RTCLOG(RTC_INFO, "disconnected: id=%u %s", ch->id(), ch->peeraddr().c_str());
        }
    };
    srv.onMessage = [](const hvnetpp::SocketChannelPtr& ch, hvnetpp::Buffer* buf) {
        ch->write(buf);
    };

    srv.start();
    RTCLOG(RTC_INFO, "echo server started on port %d, keepalive=%dms, Ctrl-C to quit", port, keepaliveMs);

    sem_init(&g_quitSem, 0, 0);
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    while (sem_wait(&g_quitSem) != 0) {}

    srv.stop();
    RTCLOG(RTC_INFO, "bye");
    return 0;
}
