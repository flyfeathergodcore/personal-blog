#include "http/server/h2_ws_stream.hpp"

#include "http/server/h2_session.hpp"
#include "http/server/h2_stream_writer.hpp"
#include "http/server/ws_connection_h2.hpp"
#include "http/handler/request_handler.hpp"
#include "http/protocol/response.hpp"
#include "net/when_all.h"
#include <iostream>

H2WsStream::H2WsStream(std::shared_ptr<H2Session> session, int32_t stream_id,
                       RequestHandler& handler)
    : session_(std::move(session))
    , stream_id_(stream_id)
    , handler_(handler)
{
}

coro::Task<bool> H2WsStream::Start()
{
    auto it = session_->streams_.find(stream_id_);
    if (it == session_->streams_.end() || !session_->StreamWritable(stream_id_))
        co_return false;

    // RFC 8441 的扩展 CONNECT 以 2xx HEADERS 完成握手；没有 H1 的
    // Sec-WebSocket-Accept。实际帧编码和输出仍全部经由 Session。
    Response handshake(200, it->second.Region());
    handshake.Header("date", CachedDate());
    session_->WriteResponseHeaders(stream_id_, handshake);
    if (!co_await session_->FlushOutput())
        co_return false;

    it->second.ws_active_ = true;
    auto connection = std::make_shared<H2WsConnection>(
        H2StreamWriter(*session_, stream_id_), it->second, session_->loop_);
    net::spawn(RunHandler(session_, stream_id_, &handler_, std::move(connection)),
               session_->loop_);
    co_return true;
}

coro::Task<void> H2WsStream::RunHandler(
    std::shared_ptr<H2Session> session, int32_t stream_id,
    RequestHandler* handler, std::shared_ptr<H2WsConnection> connection)
{
    try {
        auto it = session->streams_.find(stream_id);
        if (it != session->streams_.end()) {
            co_await handler->HandleWebSocket(it->second, *connection);
            co_await connection->Finish();
        }
    } catch (const std::exception& e) {
        std::cerr << "[h2] WS handler error: " << e.what() << std::endl;
        session->WriteRstStream(stream_id, H2Error::INTERNAL_ERROR);
    }

    co_await session->FlushOutput();
    connection->MarkClosed();
    connection.reset();
    session->FinishStream(stream_id);
}
