#pragma once

#include "coro/task.h"
#include <cstdint>
#include <memory>

class H2Session;
class RequestHandler;

// RFC 8441 扩展 CONNECT 的流级生命周期：握手、启动 handler 和最终清理。
class H2WsStream {
public:
    H2WsStream(std::shared_ptr<H2Session> session, int32_t stream_id,
               RequestHandler& handler);

    // 完成握手并投递 WS handler。失败时由调用方按普通流清理。
    coro::Task<bool> Start();

private:
    static coro::Task<void> RunHandler(std::shared_ptr<H2Session> session,
                                       int32_t stream_id,
                                       RequestHandler* handler,
                                       std::shared_ptr<class H2WsConnection> connection);

    std::shared_ptr<H2Session> session_;
    int32_t stream_id_;
    RequestHandler& handler_;
};
