#include "server/h2_stream_writer.hpp"
#include "server/h2_session.hpp"

H2StreamWriter::H2StreamWriter(H2Session& session, int32_t stream_id)
    : session_(session), stream_id_(stream_id)
{
}

coro::Task<bool> H2StreamWriter::Write(std::string_view data)
{
    if (ended_) co_return false;
    session_.WriteData(stream_id_, reinterpret_cast<const uint8_t*>(data.data()), data.size(), false);
    const bool ok = co_await session_.FlushOutput();
    if (!ok) ended_ = true;
    co_return ok;
}

void H2StreamWriter::End()
{
    if (ended_) return;
    session_.WriteData(stream_id_, nullptr, 0, true);
    ended_ = true;
}

bool H2StreamWriter::IsDisconnected() const
{
    return ended_;
}
