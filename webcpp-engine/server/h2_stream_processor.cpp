#include "server/h2_stream_processor.hpp"
#include "server/h2_session.hpp"
#include "server/sse_push.hpp"
#include "protocol/http2/stream_context.hpp"
#include "protocol/response.hpp"
#include "coro/awaiter.h"
#include <unistd.h>

H2StreamProcessor::H2StreamProcessor(H2Session& session, int32_t stream_id)
    : session_(session)
    , stream_id_(stream_id)
{
}

coro::Task<void> H2StreamProcessor::Run()
{
    co_await session_.ProcessStream(stream_id_);
}

coro::Task<size_t> H2StreamProcessor::WriteResponse(
    H2StreamContext& context, const Response& response)
{
    session_.WriteResponseHeaders(stream_id_, response);

    if (response.IsStream()) {
        const int push_ms = response.PushIntervalMs();
        auto init = SseInitialPayload(session_.MetricsForStreamProcessor());
        session_.WriteData(stream_id_,
            reinterpret_cast<const uint8_t*>(init.data()), init.size(), false);
        if (!co_await session_.FlushOutput())
            co_return 0;

        SsePushState sse;
        sse.Init(session_.MetricsForStreamProcessor());
        while (!session_.goaway_sent_ && !session_.goaway_received_
               && !context.stream_closed_)
        {
            co_await coro::sleep_for(push_ms);

            auto payload = sse.BuildPayload(session_.MetricsForStreamProcessor());
            if (payload.empty())
                payload = ":\n\n";

            session_.WriteData(stream_id_,
                reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), false);
            if (!co_await session_.FlushOutput())
                break;
        }
        co_return 0;
    }

    size_t body_len = 0;
    const uint8_t* body_ptr = nullptr;
    if (!response.BodyWire().empty()) {
        body_ptr = reinterpret_cast<const uint8_t*>(response.BodyWire().data());
        body_len = response.BodyWire().size();
    } else if (response.IsFile()) {
        const auto file_len = response.FileRangeLen() > 0
            ? response.FileRangeLen() : response.FileSize();
        context.file_buf_.resize(file_len);
        const auto n = ::pread(response.Fd(), context.file_buf_.data(), file_len,
                               static_cast<off_t>(response.FileRangeOffset()));
        if (n > 0) {
            context.file_buf_.resize(static_cast<size_t>(n));
            body_ptr = reinterpret_cast<const uint8_t*>(context.file_buf_.data());
            body_len = static_cast<size_t>(n);
        }
    }

    session_.WriteData(stream_id_, body_ptr, body_len, true);
    co_return body_len;
}
