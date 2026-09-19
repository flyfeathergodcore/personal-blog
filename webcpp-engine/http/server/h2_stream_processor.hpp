#pragma once
#include "coro/task.h"
#include <cstddef>
#include <cstdint>
#include <memory>

class H2Session;
class H2StreamContext;
class Response;

// 单条 HTTP/2 流的业务处理入口。只通过 Session 的流级接口输出数据，
// 不直接管理帧缓冲、流控账本或流容器。
class H2StreamProcessor {
public:
    H2StreamProcessor(std::shared_ptr<H2Session> session, int32_t stream_id);
    coro::Task<void> Run();

private:
    coro::Task<void> RunHandlerStream(H2StreamContext& context,
                                      class RequestHandler& handler);
    coro::Task<size_t> WriteResponse(H2StreamContext& context,
                                     const Response& response);

    std::shared_ptr<H2Session> session_;
    int32_t stream_id_;
};
