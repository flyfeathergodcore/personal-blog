#pragma once
#include "handler/request_handler.hpp"
#include "server/ws_connection.hpp"
#include "protocol/ws_frame.hpp"
#include "coro/task.h"

/// Echo WS handler —— 回显收到的每一帧（coro/net 版）。
///
/// 兼容新 optional-return ReadFrame 语义：WsConnection::Read() 在
/// 连接结束（对端 Close / 错误 / 空闲超时）时返回 Close-opcode 帧，
/// 与合法的零长度 Text/Binary 数据帧明确区分。
class WsEchoHandler : public RequestHandler {
public:
    // 处理 WS 升级握手：校验 sec-websocket-key 并返回 101 升级响应
    // 参数：ctx - 请求上下文
    Response Handle(const Context& ctx) override {
        auto ws_key = ctx.Header("sec-websocket-key");
        if (!ws_key.empty()) {
            auto accept = ComputeWsAccept(ws_key);
            return Response::WebSocketUpgrade(*ctx.Pool(), std::move(accept));
        }
        return Response::Error(404, *ctx.Pool());
    }

    // 回显循环：持续读取帧并原样回写，收到 Close 帧即结束
    // 参数：ctx - 原始升级请求上下文；conn - WebSocket 连接
    coro::Task<void> HandleWebSocket(const Context& /*ctx*/,
                                     WsConnectionBase& conn) override
    {
        while (true) {
            auto frame = co_await conn.Read();
            // 连接结束以 Close-opcode 帧返回（新 ReadFrame 语义），
            // 与合法的零长度 Text/Binary 数据帧明确区分。
            if (frame.opcode == WsOpcode::Close) break;
            co_await conn.Send(frame.opcode, std::move(frame.payload), true);
        }
        co_return;
    }
};
