// rpc 错误模型：框架层状态码 + 异常类型。
// 注意：协议层已有 proto 枚举 rpc::RpcStatus（rpc_meta.pb.h），
// 这里 C++ 层用 RpcCode 表示框架内部状态，并提供与协议枚举的互转。
#pragma once

#include <stdexcept>
#include <string>

#include "rpc_meta.pb.h"

namespace rpc {

// 框架内部状态码（C++ 层统一使用）
enum class RpcCode {
    Ok = 0,
    Error,          // 处理器异常/内部错误
    NotFound,       // service/method 不存在
    Timeout,        // 调用超时
    Cancelled,      // 调用被取消
    Closed,         // 连接关闭
    EncodeFail,     // 帧序列化失败
    DecodeFail,     // 帧反序列化失败/长度越界
    Unknown,
};

// RpcCode → 协议枚举 RpcStatus
inline RpcStatus to_proto_status(RpcCode c) noexcept {
    switch (c) {
        case RpcCode::Ok:        return RpcStatus::ST_OK;
        case RpcCode::Error:     return RpcStatus::ST_ERROR;
        case RpcCode::NotFound:  return RpcStatus::ST_NOT_FOUND;
        case RpcCode::Timeout:   return RpcStatus::ST_TIMEOUT;
        case RpcCode::Cancelled: return RpcStatus::ST_CANCELLED;
        case RpcCode::Closed:    return RpcStatus::ST_CLOSED;
        default:                 return RpcStatus::ST_UNKNOWN;
    }
}

// 协议枚举 RpcStatus → RpcCode
inline RpcCode from_proto_status(RpcStatus s) noexcept {
    switch (s) {
        case RpcStatus::ST_OK:        return RpcCode::Ok;
        case RpcStatus::ST_ERROR:     return RpcCode::Error;
        case RpcStatus::ST_NOT_FOUND: return RpcCode::NotFound;
        case RpcStatus::ST_TIMEOUT:   return RpcCode::Timeout;
        case RpcStatus::ST_CANCELLED: return RpcCode::Cancelled;
        case RpcStatus::ST_CLOSED:    return RpcCode::Closed;
        default:                      return RpcCode::Unknown;
    }
}

// RPC 异常：携带框架状态码与错误消息
class RpcException : public std::runtime_error {
public:
    // 构造：由状态码与消息构造异常
    explicit RpcException(RpcCode code, std::string msg = "")
        : std::runtime_error(msg.empty() ? StatusName(code) : std::move(msg)),
          code_(code) {}

    // 返回框架状态码
    RpcCode code() const noexcept { return code_; }

    // 状态码的人名可读名称
    static const char* StatusName(RpcCode c) noexcept {
        switch (c) {
            case RpcCode::Ok:        return "OK";
            case RpcCode::Error:     return "RPC_ERROR";
            case RpcCode::NotFound:  return "RPC_NOT_FOUND";
            case RpcCode::Timeout:   return "RPC_TIMEOUT";
            case RpcCode::Cancelled: return "RPC_CANCELLED";
            case RpcCode::Closed:    return "RPC_CLOSED";
            case RpcCode::EncodeFail:return "RPC_ENCODE_FAIL";
            case RpcCode::DecodeFail:return "RPC_DECODE_FAIL";
            default:                 return "RPC_UNKNOWN";
        }
    }

private:
    RpcCode code_;
};

}  // namespace rpc
