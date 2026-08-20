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
  JsonRpcResult handleStart(const nlohmann::json& data);
  JsonRpcResult handleStop(const nlohmann::json& data);
  JsonRpcResult handleSaveNode(const nlohmann::json& data);

  // 监控是主插件内部的附加组件。启动失败只记录日志，不改变原行为树插件的
  // 启停结果；这样监控异常不会影响行为树执行逻辑。
  void startMonitor() noexcept;
  void stopMonitor() noexcept;

private:
  const std::string TAG = "行为树插件";

  std::atomic_bool started_{false};
  std::shared_ptr<const JsonRpcRouter::RouteMap> routes_;
  std::unique_ptr<BehaviorTreeMonitor> monitor_;
};
