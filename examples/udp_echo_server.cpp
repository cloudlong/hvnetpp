// UDP echo 服务器
// 用法: ./udp_echo_server [port]
#include "hvnetpp/UdpServer.h"
#include "hvnetpp/Buffer.h"
#include "rtclog.h"

#include <csignal>
#include <cstdlib>
#include <semaphore.h>

static sem_t g_quitSem;

static void onSignal(int) {
    sem_post(&g_quitSem);
}

int main(int argc, char* argv[]) {
    rtclog_init("udp_echo_server");
    rtclog_set_level(RTC_INFO);

    int port = argc > 1 ? atoi(argv[1]) : 9999;

    hvnetpp::UdpServer srv("udp-echo");
    if (srv.createsocket(port) < 0) {
        return 1;
    }
    srv.onMessage = [&srv](const hvnetpp::InetAddress& peer, hvnetpp::Buffer* buf) {
        std::string msg = buf->retrieveAllAsString();
        RTCLOG(RTC_INFO, "recv %zu bytes from %s: %s", msg.size(), peer.toIpPort().c_str(), msg.c_str());
        srv.sendto(msg, peer);
    };
    srv.start();
    RTCLOG(RTC_INFO, "udp echo server started on port %d, Ctrl-C to quit", port);

    sem_init(&g_quitSem, 0, 0);
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    while (sem_wait(&g_quitSem) != 0) {}

    srv.stop();
    return 0;
}
