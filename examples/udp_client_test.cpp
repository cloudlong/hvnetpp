// UDP 客户端示例：发 N 条消息并打印回包（connected-UDP 模式）
// 用法: ./udp_client_test [port] [host] [count]
#include "hvnetpp/UdpClient.h"
#include "hvnetpp/Buffer.h"
#include "rtclog.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

int main(int argc, char* argv[]) {
    rtclog_init("udp_client_test");
    rtclog_set_level(RTC_INFO);

    int port = argc > 1 ? atoi(argv[1]) : 9999;
    const char* host = argc > 2 ? argv[2] : "127.0.0.1";
    int count = argc > 3 ? atoi(argv[3]) : 5;

    std::atomic<int> received{0};

    hvnetpp::UdpClient cli("udp-cli");
    if (cli.createsocket(port, host) < 0) {
        return 1;
    }
    cli.setConnectedUdp(true);
    cli.onMessage = [&received](const hvnetpp::InetAddress& peer, hvnetpp::Buffer* buf) {
        std::string msg = buf->retrieveAllAsString();
        RTCLOG(RTC_INFO, "reply from %s: %s", peer.toIpPort().c_str(), msg.c_str());
        ++received;
    };
    cli.start();

    for (int i = 0; i < count; ++i) {
        char msg[64];
        snprintf(msg, sizeof msg, "udp message %d", i);
        cli.send(msg, strlen(msg));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    cli.stop();
    RTCLOG(RTC_INFO, "sent %d, received %d", count, received.load());
    return received == count ? 0 : 1;
}
