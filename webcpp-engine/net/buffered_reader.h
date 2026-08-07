// net 层带缓冲读取：BufferedReader 封装（协程 read_until / read_exact）。
// 设计：buf_ 跨读累积，pos_ 记录已消费偏移；read_until 按分隔符定位，
// 未命中时仅把"不可能成为跨读分隔符尾部"的字节移入 out，保留末尾
// delim.size()-1 字节作为候选——分隔符即使横跨两次 TCP 分段也能被识别，
// 不会像"未命中即清空 buf_"那样把分隔符永久丢失（见 buffered_reader.cpp 注释）。
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "coro/task.h"
#include "net/tcp_stream.h"

namespace net {

class BufferedReader {
public:
    // 构造：绑定底层流并预留缓冲容量（默认 16KB）
    // 参数：s - 底层 TcpStream 引用；cap - 缓冲预分配容量（字节）
    explicit BufferedReader(TcpStream& s, size_t cap = 16384);
    // 按分隔符读取：跨读累积缓冲，命中 delim 时返回其前全部数据
    // 参数：delim - 分隔符；out - 输出
    coro::Task<IoResult> read_until(std::string_view delim, std::string& out);
    // 精确读取 n 字节（先消费缓冲，再补读流）
    // 参数：n - 需读取字节数；out - 输出
    coro::Task<IoResult> read_exact(size_t n, std::string& out);
    // 返回已缓冲未消费的数据视图
    std::string_view buffered() const;      // 已缓冲未消费数据
private:
    TcpStream& s_;
    std::string buf_;
    size_t pos_ = 0;
};

}  // namespace net
