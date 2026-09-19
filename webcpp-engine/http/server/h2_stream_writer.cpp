#include "http/server/h2_stream_writer.hpp"
#include "http/server/h2_session.hpp"

H2StreamWriter::H2StreamWriter(H2Session& session, int32_t stream_id)
    : session_(session), stream_id_(stream_id)
{
}

coro::Task<bool> H2StreamWriter::Write(std::string_view data)
{
    if (ended_) co_return false;
    const bool ok = co_await session_.SendData(stream_id_, data);
    if (!ok) ended_ = true;
    co_return ok;
}

coro::Task<void> H2StreamWriter::End()
{
    if (ended_) co_return;
    ended_ = true;
    co_await session_.EndStream(stream_id_);
}

bool H2StreamWriter::Writable() const
{
    return !ended_ && session_.StreamWritable(stream_id_);
}

bool H2StreamWriter::IsDisconnected() const
{
    return !Writable();
}
