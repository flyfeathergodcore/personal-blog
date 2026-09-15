#include "server/h2_stream_processor.hpp"
#include "server/h2_session.hpp"
#include "server/h2_stream_writer.hpp"
#include "server/h2_ws_stream.hpp"
#include "server/sse_push.hpp"
#include "handler/request_handler.hpp"
#include "protocol/http2/stream_context.hpp"
#include "protocol/response.hpp"
#include "coro/awaiter.h"
#include <chrono>
#include <iostream>
#include <unistd.h>
#include <vector>

H2StreamProcessor::H2StreamProcessor(std::shared_ptr<H2Session> session,
                                     int32_t stream_id)
    : session_(std::move(session))
    , stream_id_(stream_id)
{
}

coro::Task<void> H2StreamProcessor::Run()
{
    auto it = session_->streams_.find(stream_id_);
    if (it == session_->streams_.end()) co_return;
    auto& ctx = it->second;

    if (ctx.stream_closed_) {
        session_->FinishStream(stream_id_);
        co_return;
    }
    if (session_->max_body_size_ > 0 &&
        ctx.ContentLength() > session_->max_body_size_) {
        session_->WriteRstStream(stream_id_, H2Error::REFUSED_STREAM);
        co_await session_->FlushOutput();
        session_->FinishStream(stream_id_);
        co_return;
    }

    const auto start_time = std::chrono::steady_clock::now();
    bool detached = false;
    bool flush_error = false;
    try {
        auto response = session_->middleware_.ExecutePre(ctx);
        RequestHandler* handler = nullptr;
        if (response.IsNone()) {
            std::vector<std::pair<std::string_view, std::string_view>> params;
            handler = session_->router_.Match(ctx.Path(), &params);
            ctx.SetParams(params);
            if (handler && handler->IsStream()) {
                co_await RunHandlerStream(ctx, *handler);
            } else if (handler && handler->IsAsync()) {
                response = co_await handler->HandleAsync(ctx);
            } else if (handler) {
                response = handler->Handle(ctx);
            } else {
                response = Response::Error(404, ctx.Region());
            }
        }

        if (handler && handler->IsStream()) {
            // 流式 handler 已完成全部输出，直接走统一清理。
        } else if (ctx.ws_extended_) {
            std::vector<std::pair<std::string_view, std::string_view>> ws_params;
            auto* ws_handler = session_->router_.Match(ctx.Path(), &ws_params);
            ctx.SetParams(ws_params);
            if (ws_handler && ws_handler->IsWebSocketUpgradeHandler()) {
                detached = co_await H2WsStream(session_, stream_id_, *ws_handler).Start();
                if (!detached) {
                    session_->WriteRstStream(stream_id_, H2Error::INTERNAL_ERROR);
                    co_await session_->FlushOutput();
                }
            } else {
                session_->WriteRstStream(stream_id_, H2Error::REFUSED_STREAM);
                co_await session_->FlushOutput();
            }
        } else {
            const auto body_len = co_await WriteResponse(ctx, response);
            if (!response.IsStream()) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start_time).count();
                session_->middleware_.ExecutePostSync(ctx, response.StatusCode(),
                                                      body_len, elapsed, session_->worker_id_);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[h2] handler error (stream " << stream_id_
                  << "): " << e.what() << std::endl;
        session_->WriteRstStream(stream_id_, H2Error::INTERNAL_ERROR);
        flush_error = true;
    }
    if (flush_error)
        co_await session_->FlushOutput();
    if (!detached)
        session_->FinishStream(stream_id_);
}

coro::Task<void> H2StreamProcessor::RunHandlerStream(
    H2StreamContext& context, RequestHandler& handler)
{
    auto response = Response::SSEStream(context.Region(), 0);
    session_->WriteResponseHeaders(stream_id_, response);
    H2StreamWriter writer(*session_, stream_id_);
    if (!(co_await writer.Write(SseInitialPayload(session_->MetricsForStreamProcessor()))))
        co_return;
    co_await handler.HandleStream(context, writer);
    co_await writer.End();
}

coro::Task<size_t> H2StreamProcessor::WriteResponse(
    H2StreamContext& context, const Response& response)
{
    session_->WriteResponseHeaders(stream_id_, response);
    H2StreamWriter writer(*session_, stream_id_);

    if (response.IsStream()) {
        const int push_ms = response.PushIntervalMs();
        auto init = SseInitialPayload(session_->MetricsForStreamProcessor());
        if (!(co_await writer.Write(init)))
            co_return 0;

        SsePushState sse;
        while (!session_->goaway_sent_ && !session_->goaway_received_
               && !context.stream_closed_)
        {
            co_await coro::sleep_for(push_ms);

            auto payload = sse.BuildPayload(session_->MetricsForStreamProcessor());
            if (payload.empty())
                payload = ":\n\n";

            if (!(co_await writer.Write(payload)))
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

    if (body_len > 0) {
        if (!(co_await writer.Write({reinterpret_cast<const char*>(body_ptr), body_len})))
            co_return body_len;
    }
    co_await writer.End();
    co_return body_len;
}
