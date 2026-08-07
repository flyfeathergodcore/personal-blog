// BufferedReader：跨读累积缓冲 + 分隔符定位（read_until）/ 精确读取（read_exact）。
//
// 与任务简报的 read_until 实现有偏差（缺陷修复）：
//   简报未命中时执行 `out.append(buf_[pos_..]); buf_.clear(); pos_=0;`，
//   把未命中尾部整体移入 out 并清空 buf_——当分隔符（如 4 字节 "\r\n\r\n"）
//   恰好横跨两次 TCP 分段时，前缀字节已被移走，后续读入的字节永远拼不成
//   完整分隔符，read_until 只能循环读到 EOF/错误。参考消费者
//   handler/reverse_proxy.cpp 的 asio async_read_until 语义：分隔符应跨读保留。
//   本实现改为：buf_ 跨读累积、pos_ 只前进不清空；未命中时仅把
//   buf_[pos_..size()-delim.size()+1) 移入 out，末尾保留 delim.size()-1 字节
//   作为"下一段可能补全分隔符"的候选，随后读入新数据 append 到 buf_ 再查。
#include "net/buffered_reader.h"

#include <algorithm>

namespace net {

// 构造：绑定底层流并预留缓冲容量
// 参数：s - 底层 TcpStream 引用；cap - 缓冲预分配容量（字节）
BufferedReader::BufferedReader(TcpStream& s, size_t cap) : s_(s) {
    buf_.reserve(cap);
}

// 按分隔符读取：跨读累积缓冲定位 delim，返回其前全部数据（分隔符横跨两次分段也能识别）
// 参数：delim - 分隔符；out - 输出。成功返回 {out.size(), None}
coro::Task<IoResult> BufferedReader::read_until(std::string_view delim, std::string& out) {
    out.clear();
    for (;;) {
        auto hit = buf_.find(delim, pos_);
        if (hit != std::string::npos) {
            out.append(buf_.data() + pos_, hit - pos_);
            pos_ = hit + delim.size();   // 分隔符被消费；其后字节留在 buf_ 供下次使用
            co_return IoResult{out.size(), IoError::None};
        }
        // 未命中：只移出确定不属于"跨读分隔符尾部"的字节。
        // 末尾保留 delim.size()-1 字节作为候选（1 字节分隔符即保留 0 字节）。
        size_t avail = buf_.size() - pos_;
        if (avail > delim.size() - 1) {
            size_t move = avail - (delim.size() - 1);
            out.append(buf_.data() + pos_, move);
            pos_ += move;
        }
        char tmp[4096];
        auto r = co_await s_.read_some(tmp, sizeof(tmp));
        if (!r.ok()) co_return r;        // EOF/超时/关闭/错误原样返回
        buf_.append(tmp, r.bytes);
    }
}

// 精确读取 n 字节：先消费缓冲内未读数据，不足部分再从流读取
// 参数：n - 需读取字节数；out - 输出。成功返回 {n, None}
coro::Task<IoResult> BufferedReader::read_exact(size_t n, std::string& out) {
    out.clear();
    // 先消费缓冲区内已读未消费的部分
    size_t avail = buf_.size() - pos_;
    if (avail > 0) {
        size_t take = std::min(avail, n);
        out.append(buf_.data() + pos_, take);
        pos_ += take;
    }
    // 再从流中读取剩余部分；EOF/错误原样返回（out 保留已读部分）
    while (out.size() < n) {
        char tmp[4096];
        size_t want = std::min(n - out.size(), sizeof(tmp));
        auto r = co_await s_.read_some(tmp, want);
        if (!r.ok()) co_return r;
        out.append(tmp, r.bytes);
    }
    co_return IoResult{n, IoError::None};
}

// 返回已缓冲未消费数据的视图（供外部直接读取，不移动读指针）
std::string_view BufferedReader::buffered() const {
    return std::string_view(buf_).substr(pos_);
}

}  // namespace net
