// RpcServerConnection 实现：服务端连接读循环 + 一元请求分发
#include "rpc/rpc_connection.h"

#include <utility>

#include "coro/event_loop.h"
#include "rpc/codegen.h"
#include "rpc/error.h"
#include "rpc/frame.h"
#include "rpc/rpc_server.h"

namespace rpc {

namespace {
// 一元处理协程：持有 shared_ptr 保活连接直至 handler 完成。慢 handler 期间
// 对端断开时 Serve 先返回（其 shared_ptr 释放），此处的副本让连接存活到
// handler 写完响应，杜绝 HandleUnary 用悬垂 this 访问已析构连接。
static coro::Task<void> RunUnary(std::shared_ptr<RpcServerConnection> conn, RpcFrame req) {
    co_await conn->HandleUnary(std::move(req));
}
}  // namespace

RpcServerConnection::RpcServerConnection(RpcServer* server, net::TcpStream stream)
    : server_(server), stream_(std::move(stream)) {}

RpcServerConnection::~RpcServerConnection() = default;

// 读循环：只做 ReadFrame → 分发；每个请求 spawn 独立协程，慢 handler 不阻塞读循环
coro::Task<void> RpcServerConnection::Serve() {
    for (;;) {
        RpcFrame f;
        net::IoResult r = co_await ReadFrame(stream_, &f, -1);
        if (!r.ok()) {
            // 连接断开：唤醒阻塞在流 Read() 的 handler（可能正等客户端 DONE 而客户端
            // 已消失），否则流协程永久挂起
            for (auto& kv : streams_) {
                kv.second->reader.Post(StreamEvent{StreamEventKind::Done, RpcCode::Closed,
                                                   "", "connection closed"});
            }
            streams_.clear();
            break;
        }
        switch (f.kind()) {
        case RpcKind::UNARY_REQUEST: {
            // 按值拷贝帧给处理协程（f 为读循环局部变量，下一轮迭代会复用）；
            // shared_from_this 保活连接（见 RunUnary 注释）
            coro::Task<void> h = RunUnary(shared_from_this(), f);
            coro::EventLoop::current().post(h.handle());
            break;
        }
        case RpcKind::STREAM_INIT: {
            const MethodHandler* mh = server_->FindHandler(f.service_name(), f.method_name());
            if (!mh || mh->type != MethodHandler::Type::Stream) {
                // 拒绝：非流方法 → 回 STREAM_DONE{NOT_FOUND}（协议无显式 ACK，拒绝即 DONE）
                RpcFrame resp;
                resp.set_kind(RpcKind::STREAM_DONE);
                resp.set_call_id(f.call_id());
                resp.set_status(RpcStatus::ST_NOT_FOUND);
                resp.set_error_message("stream method not found");
                co_await SendFrameLocked(std::move(resp), -1);
                break;
            }
            // 建流上下文并登记：写出端绑定本连接（sender 持 shared_ptr 保活连接）
            auto ctx = std::make_shared<StreamContext>();
            ctx->writer.SetSender(f.call_id(),
                [self = shared_from_this(), cid = f.call_id()](RpcFrame fr, int64_t tmo)
                    -> coro::Task<bool> {
                    return self->SendFrameLocked(std::move(fr), tmo);
                });
            streams_[f.call_id()] = ctx;
            coro::Task<void> h = RunServerStream(shared_from_this(), mh, ctx, f);
            coro::EventLoop::current().post(h.handle());
            break;
        }
        case RpcKind::STREAM_DATA: {
            // 投递到对应流的读端；流已回收/未知 call_id → 丢弃
            auto it = streams_.find(f.call_id());
            if (it != streams_.end()) {
                it->second->reader.Post(StreamEvent{StreamEventKind::Data, RpcCode::Ok,
                                                    f.payload(), ""});
            }
            break;
        }
        case RpcKind::STREAM_DONE: {
            auto it = streams_.find(f.call_id());
            if (it != streams_.end()) {
                auto ctx = it->second;
                ctx->client_done = true;
                ctx->reader.Post(StreamEvent{StreamEventKind::Done, from_proto_status(f.status()),
                                             "", f.error_message()});
                RecycleStream(f.call_id());  // 若服务端也已 DONE → 回收
            }
            break;
        }
        default:
            break;  // 未知类型，忽略
        }
    }
    co_return;
}

// 经 WriteLock 串行化写一帧：所有写出的唯一出口（一元响应/流帧共用）。
// 按值接收帧并 move 进协程帧：协程挂起在 Acquire/WriteFrame 上时调用方的
// 临时对象（流 sender lambda 的按值参数）已析构，引用即悬垂——必须持有副本。
coro::Task<bool> RpcServerConnection::SendFrameLocked(RpcFrame f, int64_t timeout_ms) {
    if (!stream_.is_open()) co_return false;
    if (!co_await write_lock_.Acquire(-1)) co_return false;
    const bool ok = co_await WriteFrame(stream_, f, timeout_ms);
    write_lock_.Release();  // 无论成败都释放，防止锁泄漏死锁
    co_return ok;
}

// 回收流：双端 DONE 才移除（释放流上下文，其 writer 持有 shared_ptr 连接）
void RpcServerConnection::RecycleStream(std::uint64_t call_id) {
    auto it = streams_.find(call_id);
    if (it == streams_.end()) return;
    if (it->second->client_done && it->second->server_done) streams_.erase(it);
}

// 流处理协程：持 self（shared_ptr）保活连接与流上下文（handler 可能比 Serve 活得更久）。
// 调用户流 handler；handler 未显式 Finish 时框架补发 STREAM_DONE{OK}（幂等）；
// 双端 DONE 后回收流上下文（call_id 可复用）。
// 注意：co_await 不能出现在 catch handler 中（C++ 限制），故 catch 只收集错误，
// DONE 统一在 try 块之后发送。
coro::Task<void> RpcServerConnection::RunServerStream(
    std::shared_ptr<RpcServerConnection> self, const MethodHandler* mh,
    std::shared_ptr<StreamContext> ctx, RpcFrame init) {
    (void)self;  // 仅用于协程帧保活连接（this 裸指针不保活）

    ServerCallContext sctx;
    sctx.call_id = init.call_id();
    sctx.service_name = init.service_name();
    sctx.method_name = init.method_name();
    sctx.reader = &ctx->reader;
    sctx.writer = &ctx->writer;

    RpcCode err_code = RpcCode::Ok;
    std::string err_msg;
    bool failed = false;
    if (mh == nullptr) {
        failed = true;
        err_code = RpcCode::NotFound;
        err_msg = "stream method not found";
    } else {
        try {
            co_await mh->stream(sctx);
        } catch (const RpcException& e) {
            failed = true;
            err_code = e.code();
            err_msg = e.what();
        } catch (const std::exception& e) {
            failed = true;
            err_code = RpcCode::Error;
            err_msg = e.what();
        } catch (...) {
            failed = true;
            err_code = RpcCode::Error;
            err_msg = "internal handler error";
        }
    }
    if (failed) {
        co_await ctx->writer.Finish(err_code, err_msg);  // 异常/缺处理器 → DONE{错误}
    } else if (!ctx->writer.write_done()) {
        co_await ctx->writer.Finish(RpcCode::Ok);  // handler 未显式 Finish，框架补发
    }
    ctx->server_done = true;
    RecycleStream(init.call_id());
    co_return;
}

// 一元处理：查注册表 → 调 handler → 序列化响应 → 经 WriteLock 写回
coro::Task<void> RpcServerConnection::HandleUnary(RpcFrame req) {
    RpcFrame resp;
    resp.set_kind(RpcKind::UNARY_RESPONSE);
    resp.set_call_id(req.call_id());

    const MethodHandler* mh = server_->FindHandler(req.service_name(), req.method_name());
    if (!mh) {
        resp.set_status(RpcStatus::ST_NOT_FOUND);
        resp.set_error_message("service/method not found");
    } else {
        try {
            ServerCallContext ctx;
            ctx.call_id = req.call_id();
            ctx.service_name = req.service_name();
            ctx.method_name = req.method_name();
            std::string out = co_await mh->unary(req.payload(), ctx);
            resp.set_status(RpcStatus::ST_OK);
            resp.set_payload(out);
        } catch (const RpcException& e) {
            resp.set_status(to_proto_status(e.code()));
            resp.set_error_message(e.what());
        } catch (const std::exception& e) {
            resp.set_status(RpcStatus::ST_ERROR);
            resp.set_error_message(e.what());
        } catch (...) {
            resp.set_status(RpcStatus::ST_ERROR);
            resp.set_error_message("internal handler error");
        }
    }

    co_await SendFrameLocked(std::move(resp), -1);
    co_return;
}

}  // namespace rpc
