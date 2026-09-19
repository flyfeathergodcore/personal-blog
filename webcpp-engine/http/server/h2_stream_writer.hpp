#pragma once

#include "http/handler/request_handler.hpp"
#include <cstdint>

class H2Session;

// 单条 HTTP/2 流的 handler 写入门面。帧编码与 socket 刷出仍由会话层协调。
class H2StreamWriter final : public StreamSink {
public:
    H2StreamWriter(H2Session& session, int32_t stream_id);

    coro::Task<bool> Write(std::string_view data) override;
    coro::Task<void> End() override;
    bool Writable() const;
    bool IsDisconnected() const override;

private:
    H2Session& session_;
    int32_t stream_id_;
    bool ended_ = false;
};
