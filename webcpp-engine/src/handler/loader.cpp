#include "handler/loader.hpp"
#include "router/router.hpp"
#include <filesystem>
#include <iostream>

// ── register_routes 签名的函数指针类型 ──
using RegisterFunc = void (*)(Router&);

// 析构：卸载所有已加载的动态库
HandlerLoader::~HandlerLoader()
{
    UnloadAll();
}

// 加载指定目录下全部 .so 插件：dlopen + dlsym(register_routes) 并注册路由到 router
// 参数：dir - 插件目录（不存在则返回 0）；router - 路由注册目标；返回成功加载数量
size_t HandlerLoader::LoadAll(const std::string& dir, Router& router)
{
    namespace fs = std::filesystem;

    if (!fs::is_directory(dir)) {
        // 目录不存在不报错 —— 用户可能没有热插拔需求
        return 0;
    }

    size_t count = 0;
    for (auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".so")
            continue;

        auto path = entry.path().string();
        auto* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            std::cerr << "[loader] dlopen 失败: " << path
                      << " — " << ::dlerror() << std::endl;
            continue;
        }

        auto reg = reinterpret_cast<RegisterFunc>(
            ::dlsym(handle, "register_routes"));
        if (!reg) {
            std::cerr << "[loader] dlsym(register_routes) 失败: "
                      << path << " — " << ::dlerror() << std::endl;
            ::dlclose(handle);
            continue;
        }

        // ── 注册路由 —— .so 里调用 router.Add() ──
        try {
            reg(router);
        } catch (std::exception& e) {
            std::cerr << "[loader] register_routes 异常: "
                      << path << " — " << e.what() << std::endl;
            ::dlclose(handle);
            continue;
        }

        libs_.push_back({handle, path});
        std::cout << "[loader] 已加载: " << path << std::endl;
        ++count;
    }

    return count;
}

// 卸载全部已加载的动态库并清空句柄列表
void HandlerLoader::UnloadAll()
{
    for (auto& lib : libs_) {
        if (lib.handle) {
            ::dlclose(lib.handle);
            std::cout << "[loader] 已卸载: " << lib.path << std::endl;
        }
    }
    libs_.clear();
}
