#pragma once
#include "coro/task.h"
#include <cstddef>
#include <cstdint>

class H2Session;
class H2StreamContext;
class Response;

// 单条 HTTP/2 流的业务处理入口。WS 生命周期仍由会话层协调。
class H2StreamProcessor {
public:
    H2StreamProcessor(H2Session& session, int32_t stream_id);
    coro::Task<void> Run();

private:
    friend class H2Session;

    // Writes the response frames for ordinary, file, and native SSE responses.
    // The session owns frame buffering and connection-level I/O coordination.
    coro::Task<size_t> WriteResponse(H2StreamContext& context,
                                     const Response& response);

    H2Session& session_;
    int32_t stream_id_;
};
