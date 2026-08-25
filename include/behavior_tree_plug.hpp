#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "nlohmann/json.hpp"
#include "plugin_core/sdk/plugin_sdk.hpp"

class BehaviorTreeMonitor;

class BehaviorTreePlug final : public PluginBase {
public:
  BehaviorTreePlug();
  ~BehaviorTreePlug() override;

private:
  bool on_init(IPluginContext& ctx) noexcept override;
  bool on_start() noexcept override;
  void on_stop() noexcept override;
  void on_unload() noexcept override;

private:
  void handleConnCodeit(const char*, const void*, size_t);
  void handleGatewayStatus(const char*, const void*, size_t);
  void handleRtState(const char*, const void*, size_t);
  void handleNrtState(const char*, const void*, size_t);

  JsonRpcRouter::RouteMap buildRoutes();
  PluginHttpResponse handleHttpRequest(const PluginHttpRequest& req);
  JsonRpcResult handleLoad(const nlohmann::json& data);
  JsonRpcResult handleRead(const nlohmann::json& data);
  JsonRpcResult handleReadXml(const nlohmann::json& data);
  JsonRpcResult handleStart(const nlohmann::json& data);
  JsonRpcResult handleStop(const nlohmann::json& data);
  JsonRpcResult handleSaveNode(const nlohmann::json& data);

  // 监控是主插件内部的组件。返回 false 表示 HTTP/WS 端点或
  // Host 托管 worker 启动失败，on_start() 将回滚已创建的 BTManager。
  bool startMonitor() noexcept;
  void stopMonitor() noexcept;

private:
  const std::string TAG = "行为树插件";

  std::atomic_bool started_{false};
  std::shared_ptr<const JsonRpcRouter::RouteMap> routes_;
  std::unique_ptr<BehaviorTreeMonitor> monitor_;
};
