// 聊天室服务器：4 字节大端长度头拆包 + broadcast 广播 + 周期统计
// 协议: [4 字节 BE 包体长度][包体]
// 用法: ./tcp_chat_server [port]
#include "hvnetpp/hvnetpp.h"
#include "rtclog.h"

#include <csignal>
#include <cstdlib>
#include <semaphore.h>

static sem_t g_quitSem;

static void onSignal(int) {
    sem_post(&g_quitSem);
}

// 按协议封包：长度头 + 包体
static std::string packMessage(const std::string& body) {
    hvnetpp::Buffer buf;
    buf.appendInt32(static_cast<int32_t>(body.size()));
    buf.append(body);
    return buf.retrieveAllAsString();
}

int main(int argc, char* argv[]) {
    rtclog_init("tcp_chat_server");
    rtclog_set_level(RTC_INFO);

    int port = argc > 1 ? atoi(argv[1]) : 1234;

    hvnetpp::TcpServer srv("chat");
    if (srv.createsocket(port) < 0) {
        return 1;
    }
    srv.setThreadNum(4);

    hvnetpp::unpack_setting_t unpack;
    unpack.mode = hvnetpp::UNPACK_BY_LENGTH_FIELD;
    unpack.body_offset = 4;
    unpack.length_field_offset = 0;
    unpack.length_field_bytes = 4;
    unpack.length_field_coding = hvnetpp::ENCODE_BY_BIG_ENDIAN;
    unpack.package_max_length = 64 * 1024;
    srv.setUnpack(&unpack);

    srv.onConnection = [&srv](const hvnetpp::SocketChannelPtr& ch) {
        if (ch->isConnected()) {
            RTCLOG(RTC_INFO, "join: id=%u %s (total %zu)", ch->id(), ch->peeraddr().c_str(), srv.connectionNum());
            srv.broadcast(packMessage("user#" + std::to_string(ch->id()) + " joined"));
        } else {
            RTCLOG(RTC_INFO, "leave: id=%u %s", ch->id(), ch->peeraddr().c_str());
            srv.broadcast(packMessage("user#" + std::to_string(ch->id()) + " left"));
        }
    };
    srv.onMessage = [&srv](const hvnetpp::SocketChannelPtr& ch, hvnetpp::Buffer* buf) {
        // buf 是一个完整包（含 4 字节头）
        buf->retrieve(4);
        std::string body = buf->retrieveAllAsString();
        srv.broadcast(packMessage("user#" + std::to_string(ch->id()) + ": " + body));
    };

    srv.start();

    // 周期统计在线人数
    srv.loop()->runEvery(10.0, [&srv] {
        RTCLOG(RTC_INFO, "online: %zu", srv.connectionNum());
    });

    RTCLOG(RTC_INFO, "chat server started on port %d, Ctrl-C to quit", port);
    sem_init(&g_quitSem, 0, 0);
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    while (sem_wait(&g_quitSem) != 0) {}

    srv.stop();
    return 0;
}
