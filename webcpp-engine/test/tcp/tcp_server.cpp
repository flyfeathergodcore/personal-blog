// TCP 测试服务：字节流 echo 或简化 HTTP QPS 基准模式。
// Usage: tcp_server [-h host] [-p port] [-a single|reuseport] [-m echo|http]
#include "test/tcp/echo_handler.hpp"
#include "test/tcp/http_bench_handler.hpp"
#include "tcp/server.hpp"
#include "tcp/tcp_log.hpp"

#include <cstdint>
#include <cstring>
#include <string>

namespace {

bool ParsePort(const char* value, uint16_t& port)
{
    try {
        const unsigned long parsed = std::stoul(value);
        if (parsed == 0 || parsed > 65535) return false;
        port = static_cast<uint16_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

int main(int argc, char** argv)
{
    tcp::InitLogging();
    std::string host = "127.0.0.1";
    uint16_t port = 8081;
    tcp::AcceptStrategyType accept_strategy = tcp::AcceptStrategyType::ReusePort;
    bool http_mode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (std::strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            if (!ParsePort(argv[++i], port)) {
                tcp::Log(tcp::LogLevel::Error, "TCP_CLI", "invalid TCP port");
                return 2;
            }
        } else if (std::strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value == "single") {
                accept_strategy = tcp::AcceptStrategyType::Single;
            } else if (value == "reuseport") {
                accept_strategy = tcp::AcceptStrategyType::ReusePort;
            } else {
                tcp::Log(tcp::LogLevel::Error, "TCP_CLI",
                          "accept strategy must be single or reuseport");
                return 2;
            }
        } else if (std::strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value == "echo") {
                http_mode = false;
            } else if (value == "http") {
                http_mode = true;
            } else {
                tcp::Log(tcp::LogLevel::Error, "TCP_CLI",
                          "server mode must be echo or http");
                return 2;
            }
        } else {
            tcp::Log(tcp::LogLevel::Error, "TCP_CLI",
                    "usage: " + std::string(argv[0]) +
                        " [-h host] [-p port] [-a single|reuseport] [-m echo|http]");
            return 2;
        }
    }

    tcp::ConnectionHandler handler = http_mode ? tcp::HttpBench : tcp::Echo;
    tcp::Server server(std::move(host), port, std::move(handler), accept_strategy);
    return server.Start();
}
