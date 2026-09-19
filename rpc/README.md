# rpc — 基于 protobuf + C++20 协程的完整 RPC 框架

`rpc/` 是仓库的第 4 个子项目（在 `coro/`、`mysql_connection_pool/`、`webcpp-engine/` 之上），
提供一套**自包含的 RPC 框架**：服务端 + 客户端 + 连接池/超时/重试/并发控制 +
独立 registry 服务发现 + protoc 插件代码生成 + 一元/双向流 + greeter 示例。

无 gRPC 依赖，仅依赖 `protobuf` 与仓库自身的 `coro` / `webcpp-engine`（C++20 标准协程 + kqueue/epoll 事件循环）。

## 特性

- **一元调用 + 双向流**（含 client/server streaming），同连接按 `call_id` 多路复用；
- **protoc 插件 codegen**：`.proto` → 生成服务端基类 + 注册函数 + 客户端桩（`<name>.rpc.h`）；
- **独立 registry 注册中心**：业务服务端注册 / 心跳续租 / 主动注销，客户端按服务名发现实例（租约 TTL 自动剔除过期实例）；
- **RpcClient 客户端门面**：服务发现（带缓存）+ 实例轮询负载 + 实例间 fallback + 一元失败重试；
- **连接池 + 并发上限**：按实例池化连接，超上限请求排队等待，失效连接自动剔除重建；
- **完整超时控制**：连接建立 / 每次调用 / 开流均可配超时，精确到毫秒；
- 单线程 EventLoop 模型，写路径唯一互斥出口，测试覆盖并发写入字节不交错。

## 目录结构

```
rpc/
├── CMakeLists.txt          # 框架库 + protoc 插件 + 构建期生成 + 示例 + 测试
├── proto/
│   ├── rpc_meta.proto      # 帧格式 RpcFrame（长度前缀 + protobuf 序列化）
│   └── registry.proto      # 注册中心协议（Register/Heartbeat/Deregister/Discover）
├── include/rpc/
│   ├── frame.h             # FrameCodec 帧编解码
│   ├── error.h             # RpcCode + RpcException
│   ├── codegen.h           # MethodHandler + ServerReaderWriter/ClientStream 运行时模板
│   ├── write_lock.h        # 写互斥（协程转移式）
│   ├── rpc_server.h        # RpcServer：监听/方法注册表/连接分发
│   ├── rpc_connection.h    # 服务端连接：读循环 + 流上下文
│   ├── rpc_stream.h        # 流 Reader/Writer + 类型化流
│   ├── rpc_channel.h       # RpcChannel：客户端单连接多路复用
│   ├── rpc_pool.h          # RpcConnectionPool：连接池 + 并发上限
│   ├── rpc_client.h        # RpcClient：发现 + 池 + fallback + 重试门面
│   └── registry/registry_service.h  # RegistryService 内存实现 + TTL
├── src/                    # 各模块实现
├── plugin/                 # protoc-gen-rpc 插件（FileDescriptor → .rpc.h 文本）
├── examples/
│   ├── greeter.proto       # 示例 service：一元 SayHello + 双向流 Chat
│   ├── registry_server.cpp # 独立注册中心进程
│   ├── greeter_server.cpp  # 业务服务端：注册 + 心跳 + 注销
│   └── greeter_client.cpp  # 客户端：RpcClient 门面调用
└── test/
    ├── rpc_test.cpp        # 单元：帧/多路复用/超时/流/写并发/连接池/codegen/门面
    └── e2e_test.cpp        # 端到端：registry + greeter server + client 三线程
```

## 依赖与构建

- 依赖：`protobuf`（protoc 35+，macOS 经 Homebrew 安装）、仓库内 `coro`、`webcpp-engine`、`yaml-cpp`、`OpenSSL`。
- 作为顶层统一构建的子项目（推荐）：

```bash
cmake -S . -B build
cmake --build build -j4
```

- 单独构建 `rpc/`（需自备 webcpp-engine 与 coro 路径）：

```bash
cmake -S rpc -B build/rpc \
  -DWEBCPP_ENGINE_DIR=$PWD/webcpp-engine \
  -DCORO_DIR=$PWD/coro \
  -Dyaml-cpp_DIR=/opt/homebrew/lib/cmake/yaml-cpp \
  -DCMAKE_PREFIX_PATH="/opt/homebrew"
cmake --build build/rpc -j4
```

- ASan 构建：附加 `-DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"`。

## 快速上手

### 1. 定义服务（.proto）

```proto
syntax = "proto3";
package greeter;

message HelloRequest { string name = 1; }
message HelloReply   { string message = 1; }

service Greeter {
  rpc SayHello(HelloRequest) returns (HelloReply);        // 一元
  rpc Chat(stream HelloRequest) returns (stream HelloReply);  // 双向流
}
```

### 2. 实现服务端

继承生成的 `greeter::GreeterServiceBase`，实现虚函数，再注册进 `RpcServer`：

```cpp
class GreeterImpl : public greeter::GreeterServiceBase {
public:
    coro::Task<::greeter::HelloReply> SayHello(const ::greeter::HelloRequest& req) override {
        ::greeter::HelloReply rep;
        rep.set_message("Hello, " + req.name() + "!");
        co_return rep;
    }
    coro::Task<void> Chat(rpc::ServerReaderWriter<::greeter::HelloRequest,
                                                  ::greeter::HelloReply>& stream) override {
        ::greeter::HelloRequest req;
        while (co_await stream.Read(&req)) {
            ::greeter::HelloReply rep;
            rep.set_message("Hi, " + req.name() + "!");
            co_await stream.Write(rep);
        }
        co_await stream.Finish();
    }
};

rpc::RpcServer server(loop);
server.Start("127.0.0.1", 56789);
greeter::RegisterGreeterService(server, impl);   // 生成代码：注册全部方法
loop.post(server.Serve().handle());
loop.run();
```

### 3. 客户端调用（RpcClient 门面）

```cpp
rpc::RpcClient client("127.0.0.1", 56788);   // registry 地址
greeter::HelloRequest req; req.set_name("World");
std::string bytes = co_await client.CallUnary("greeter.Greeter", "SayHello",
                                              req.SerializeAsString(), 3000);
// bytes 反序列化为 HelloReply
```

## 注册中心（registry）

- `registry_server`：独立进程承载 `RegistryService`（内存存储 + 租约 TTL）。
- 业务服务端启动时向 registry `Register`（声明 service 名 / host / port / 租约秒数），
  之后周期 `Heartbeat` 续租，退出前 `Deregister`。
- 客户端 `Discover(service_name)` 拿到该服务当前有效实例列表。
- 过期实例由 registry 周期 `SweepExpired()` 剔除，`Discover` 查询时也会过滤过期实例（双保险）。

## 协议设计

- 帧格式：`[4 字节大端长度][RpcFrame protobuf]`，单帧上限 16MB 防恶意长度。
- `RpcFrame` 携带 kind / call_id / service / method / status / error / payload。
- 一元调用 = `UNARY_REQUEST` + `UNARY_RESPONSE` 两帧；流 = `STREAM_INIT` / `STREAM_DATA` / `STREAM_DONE`。
- 同连接按 `call_id` 多路复用；连接断开 `FailAll()` 唤醒全部等待者防止帧泄漏。
- 所有帧写出一律经 `WriteLock` 协程互斥，单线程下无锁。

## 测试

```bash
./build/rpc/rpc_test     # 单元：帧编解码/写并发/多路复用/超时/三形态流/连接池/codegen 桩/RpcClient 门面
./build/rpc/e2e_test     # 端到端：registry + greeter server + client 三线程，含 Deregister 后列表变空
```

手动三进程演示：

```bash
./build/rpc/registry_server &            # 注册中心（默认 127.0.0.1:56788）
./build/rpc/greeter_server 127.0.0.1 56789 127.0.0.1 56788 &   # 注册 + 心跳
./build/rpc/greeter_client 127.0.0.1 56788                      # 发现 + 调用
```

## 关键实现约定

- 协程一律命名函数，严禁 `[&]` 捕获的协程 lambda（悬垂闭包陷阱）；
- `co_await` 不能出现在 catch handler 中——catch 只收集错误，重试逻辑放 try 块外；
- 服务端/客户端类生成在 proto `package` 命名的命名空间（含嵌套，如 `rpc.registry`），
  类型名全限定（`::greeter::HelloRequest`）避免歧义。
