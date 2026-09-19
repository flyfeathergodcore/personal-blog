// protoc 插件入口：protoc --rpc_out 调用本程序生成 .rpc.h 桩代码。
// 协议：protoc 通过 stdin 传 CodeGeneratorRequest（序列化），插件处理后
// 把 CodeGeneratorResponse 写回 stdout；PluginMain 封装了该编解码流程。
#include <google/protobuf/compiler/plugin.h>

#include "codegen.h"

int main(int argc, char** argv) {
    rpcgen::RpcCodeGenerator generator;
    return google::protobuf::compiler::PluginMain(argc, argv, &generator);
}
