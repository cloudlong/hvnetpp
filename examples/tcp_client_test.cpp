// TCP 客户端示例：指数退避重连 + 心跳 + 连接超时
// 用法: ./tcp_client_test [port] [host] [run_seconds]
#include "hvnetpp/TcpClient.h"
#include "rtclog.h"

#include <cstdlib>
#include <thread>

int main(int argc, char* argv[]) {
    rtclog_init("tcp_client_test");
    rtclog_set_level(RTC_INFO);

    int port = argc > 1 ? atoi(argv[1]) : 1234;
    const char* host = argc > 2 ? argv[2] : "127.0.0.1";
    int runSeconds = argc > 3 ? atoi(argv[3]) : 30;

    hvnetpp::TcpClient cli("demo-cli");
    if (cli.createsocket(port, host) < 0) {
        return 1;
    }
    cli.setConnectTimeout(3000);

    hvnetpp::reconn_setting_t reconn;
    reconn.min_delay = 1000;
    reconn.max_delay = 10000;
    reconn.delay_policy = 2; // 指数退避
    reconn.max_retry_cnt = 10;
    cli.setReconnect(&reconn);

    cli.onConnection = [&cli](const hvnetpp::SocketChannelPtr& ch) {
        if (ch->isConnected()) {
            RTCLOG(RTC_INFO, "connected to %s (local %s)", ch->peeraddr().c_str(), ch->localaddr().c_str());
            ch->write(std::string("hello from client"));
        } else {
            RTCLOG(RTC_INFO, "disconnected from %s", ch->peeraddr().c_str());
        }
    };
    cli.onMessage = [](const hvnetpp::SocketChannelPtr& ch, hvnetpp::Buffer* buf) {
        std::string msg = buf->retrieveAllAsString();
        RTCLOG(RTC_INFO, "recv %zu bytes: %s", msg.size(), msg.c_str());
    };
    cli.onConnectFailed = [] {
        RTCLOG(RTC_WARN, "connect attempt failed");
    };

    cli.start();

    // 心跳：每 3 秒发一次 ping
    for (int i = 0; i < runSeconds; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (i % 3 == 2 && cli.isConnected()) {
            cli.send(std::string("ping"));
        }
    }

    cli.stop();
    RTCLOG(RTC_INFO, "bye");
    return 0;
}
