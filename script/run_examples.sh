#!/usr/bin/env bash
# 在指定构建目录下跑一遍示例场景（TCP echo / 断线重连 / 聊天室 / UDP 多线程 echo / timer_demo），
# 检查退出码与 sanitizer 报告。仅 Linux。
# 用法: ./script/run_examples.sh <build_dir>     例: ./script/run_examples.sh build-tsan
set -u
B=$(cd "${1:?用法: run_examples.sh <build_dir>}" && pwd)
cd "$B/examples"
rm -rf logs && mkdir -p logs
OUT=/tmp/ex-$(basename "$B")
rm -rf "$OUT" && mkdir -p "$OUT"
fail=0
check() { # name exit expected
    if [ "$2" != "$3" ]; then echo "  [FAIL] $1 exit=$2 (expect $3)"; fail=1; else echo "  [ok]   $1 exit=$2"; fi
}

echo "== tcp echo + client"
./tcp_echo_server 12345 3000 > "$OUT/echo.out" 2>&1 & S=$!
sleep 0.5
./tcp_client_test 12345 127.0.0.1 7 > "$OUT/cli.out" 2>&1; check tcp_client_test $? 0
kill -INT $S; wait $S; check tcp_echo_server $? 0
echo "  client recv lines: $(grep -c 'recv ' "$OUT/cli.out")  (hello + 2 ping)"

echo "== tcp reconnect（先起客户端，2.5s 后起服务端）"
./tcp_client_test 12346 127.0.0.1 8 > "$OUT/cli2.out" 2>&1 & C=$!
sleep 2.5
./tcp_echo_server 12346 > "$OUT/echo2.out" 2>&1 & S=$!
wait $C; check tcp_client_test-reconnect $? 0
kill -INT $S; wait $S; check tcp_echo_server-2 $? 0
echo "  reconnect attempts: $(grep -c 'reconnect to' "$OUT/cli2.out"), connected: $(grep -c 'connected to' "$OUT/cli2.out")"

echo "== tcp chat（长度头拆包 + broadcast）"
./tcp_chat_server 12347 > "$OUT/chat.out" 2>&1 & S=$!
sleep 0.5
exec 3<>/dev/tcp/127.0.0.1/12347
exec 4<>/dev/tcp/127.0.0.1/12347
sleep 0.3
printf '\x00\x00\x00\x05hello' >&3
sleep 0.5
exec 3>&- 4>&-
sleep 0.3
kill -INT $S; wait $S; check tcp_chat_server $? 0
echo "  join=$(grep -c 'join:' "$OUT/chat.out") leave=$(grep -c 'leave:' "$OUT/chat.out")"

echo "== udp echo（2 线程）+ client"
./udp_echo_server 12348 2 > "$OUT/udps.out" 2>&1 & S=$!
sleep 0.5
./udp_client_test 12348 127.0.0.1 5 > "$OUT/udpc.out" 2>&1; check udp_client_test $? 0
kill -INT $S; wait $S; check udp_echo_server $? 0
echo "  $(grep -o 'sent [0-9]*, received [0-9]*' "$OUT/udpc.out")"

echo "== timer_demo"
timeout 20 ./timer_demo > "$OUT/timer.out" 2>&1; check timer_demo $? 0
echo "  runEvery fires: $(grep -c 'runEvery 1s' "$OUT/timer.out") (expect 5), runAfter: $(grep -c 'runAfter 2.5s' "$OUT/timer.out")"

echo "== sanitizer 报告"
if grep -lE "ERROR: AddressSanitizer|WARNING: ThreadSanitizer|runtime error|LeakSanitizer" "$OUT"/*.out; then
    fail=1
    grep -hE -A12 "ERROR: AddressSanitizer|WARNING: ThreadSanitizer|runtime error" "$OUT"/*.out | head -80
else
    echo "  none"
fi
echo "== 日志文件"
ls logs/
exit $fail
