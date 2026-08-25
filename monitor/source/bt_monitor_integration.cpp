#include <exception>
#include <memory>
#include <plugin_core/api/plugin_websocket_api.hpp>
#include <plugin_core/sdk/plugin_logx.hpp>
#include <string_view>
#include <utility>

#include "behavior_tree_plug.hpp"
#include "bt_manager.hpp"
#include "bt_monitor_plugin.hpp"

/*
 * BehaviorTreePlug 与 BehaviorTreeMonitor 的生命周期桥接
 * ========================================================
 *
 * 本文件是“一个插件动态库 + 一个内部监控静态库”结构的连接点：
 *
 *   BehaviorTreePlugin.so / .dll              最终唯一部署的插件动态库
 *       |
 *       +-- BehaviorTreePlug                  唯一 PLUGIN_DECLARE
 *       |
 *       +-- BehaviorTreeMonitor              监控缓存与 WebSocket 分发
 *
 * BehaviorTreeMonitor 是普通 C++ 类，不能直接调用 PluginBase 的 protected Host
 * API。因此端点注册留在 BehaviorTreePlug 成员函数中；WebSocket 发送能力通过
 * std::function 注入监控对象。这样监控实现保持独立，同时不会产生第二套插件
 * 导出符号，也不会让最终插件依赖自研监控动态库。
 */

// BehaviorTreeMonitor 在主头文件中只有前置声明。构造和析构都必须放在能看到
// 完整类型的翻译单元中，兼容 GCC 9 对 unique_ptr 删除器的实例化规则。
BehaviorTreePlug::BehaviorTreePlug() = default;
BehaviorTreePlug::~BehaviorTreePlug() = default;

/**
 * @brief 监控功能的总入口
 */
bool BehaviorTreePlug::startMonitor() noexcept {
  try {
    // 1. 判断监控对象是否已经存在。插件 stop -> start 时 Host 端点
    //    和 monitor_ 对象都会复用，但 BTManager 是新建的，因此必须
    //    重新注册事件回调。
    if (monitor_) {
      monitor_->start();
      BehaviorTreeMonitor* const monitor_ptr = monitor_.get();

      if (!start_managed_worker(
              "behavior-tree-monitor-events",
              [monitor_ptr](PluginStopToken stop) {
                monitor_ptr->run_event_loop(stop);
              })) {
        LOG_ERROR("[{}] Failed to start managed behavior tree monitor worker", TAG);
        monitor_->stop();
        return false;
      }

      if (auto* manager = G_BT_MANAGER()) {
        manager->setMonitorCallbacks(
            [monitor_ptr](std::string tree_xml) {
              monitor_ptr->enqueue_tree_reset(std::move(tree_xml));
            },
            [monitor_ptr](std::uint16_t uid, std::uint8_t status_code, std::int64_t timestamp_ms) {
              monitor_ptr->enqueue_node_status(uid, status_code, timestamp_ms);
            });
      }
      LOG_INFO("[{}] Monitor restarted in event-driven mode", TAG);
      return true;
    }
    // 2. 创建监控对象，传入 WebSocket 发送回调。回调由主插件提供，确保 Host
    //    持有的 WebSocket 发送函数始终指向有效实例。
    auto monitor = std::make_unique<BehaviorTreeMonitor>(
        [this](const char* session_id, std::string_view text) {
          return ws_send_text_result(session_id, text);
        },
        [this](const char* session_id, const char* topic, std::string_view text) {
          return ws_send_latest_text_result(session_id, topic, text);
        });
    BehaviorTreeMonitor* const monitor_ptr = monitor.get();

    // 3. 注册behavior-tree HTTP端点：三个监控查询路由，以及读取
    //    bt_trees/bt_nodes.xml 的 load.node 路由。
    //   curl -X POST http://192.168.2.36:80/backend/plugin-http/behavior_tree_monitor/tree.snapshot
    //   -H "Content-Type: application/json" -d '{}' curl -X POST
    //   http://192.168.2.36:80/backend/plugin-http/behavior_tree_monitor/status.snapshot -H
    //   "Content-Type: application/json" -d '{}' curl -X POST
    //   http://192.168.2.36:80/backend/plugin-http/behavior_tree_monitor/bridge.health -H
    //   "Content-Type: application/json" -d '{}'

    PluginHttpEndpointOptions http_options;
    http_options.methods = {"POST"};
    http_options.routes = {
        PluginHttpRoute{"/tree.snapshot", {"POST"}},
        PluginHttpRoute{"/status.snapshot", {"POST"}},
        PluginHttpRoute{"/bridge.health", {"POST"}},
        PluginHttpRoute{"/load.node", {"POST"}},
    };
    http_options.max_body_size = 1024 * 1024;
    http_options.max_memory_body_size = 1024 * 1024;
    http_options.timeout_ms = 10000;
    http_options.max_concurrency = 16;

    if (!register_http_endpoint(
            "behavior_tree_monitor",
            [monitor_ptr](const PluginHttpRequest& req) {
              return monitor_ptr->handle_http_request(req);
            },
            std::move(http_options))) {
      LOG_ERROR("[{}] Failed to register behavior tree monitor HTTP endpoint", TAG);
      return false;
    }

    // HTTP 注册成功后立即保存对象，确保 Host 持有的回调始终指向有效实例。
    monitor_ = std::move(monitor);

    // 4. 注册behavior-tree WebSocket 端点，提供行为树快照和状态更新的实时推送。
    PluginWsEndpointOptions ws_options;
    ws_options.max_receive_message_size = 64 * 1024;
    ws_options.max_send_message_size = 8 * 1024 * 1024;
    ws_options.max_sessions = 32;
    ws_options.max_send_queue_messages = 128;
    ws_options.max_send_queue_bytes = 8 * 1024 * 1024;

    // 三个回调：收到消息，建立连接，关闭连接
    // ws://192.168.2.36:80/backend/plugin-ws/behavior_tree_monitor
    const bool ws_registered = register_ws_endpoint(
        "behavior_tree_monitor",
        [monitor_ptr](const char* session_id, const void* data, std::size_t size,
                      PluginWsMessageType type) {
          monitor_ptr->handle_ws_message(session_id, data, size, type);
        },
        [monitor_ptr](const char* session_id) { monitor_ptr->handle_ws_open(session_id); },
        [monitor_ptr](const char* session_id) { monitor_ptr->handle_ws_close(session_id); },
        ws_options);

    // 5. 监控失败只写日志，不会让原行为树插件启动失败。
    if (!ws_registered) {
      LOG_ERROR("[{}] Failed to register behavior tree monitor WebSocket endpoint", TAG);
    }

    monitor_->start();

    // 6. 使用 xplugin Host 托管的 worker 消费事件队列。监控类
    //    不再自行创建 std::thread，Host 会通过 PluginStopToken 请求
    //    worker 停止并负责最终回收。
    if (!start_managed_worker(
            "behavior-tree-monitor-events",
            [monitor_ptr](PluginStopToken stop) {
              monitor_ptr->run_event_loop(stop);
            })) {
      LOG_ERROR("[{}] Failed to start managed behavior tree monitor worker", TAG);
      monitor_->stop();
      return false;
    }

    // 7. 将 BTManager 已有的 StatusObserver 回调接到监控事件队列。
    //    这两个 lambda 只负责入队，不在行为树 tick 线程中解析
    //    XML 或发送 WebSocket。clearMonitorCallbacks() 会在 stopMonitor()
    //    中先于 monitor_->stop() 执行，防止对象销毁后回调仍被调用。
    if (auto* manager = G_BT_MANAGER()) {
      manager->setMonitorCallbacks(
          [monitor_ptr](std::string tree_xml) {
            monitor_ptr->enqueue_tree_reset(std::move(tree_xml));
          },
          [monitor_ptr](std::uint16_t uid, std::uint8_t status_code, std::int64_t timestamp_ms) {
            monitor_ptr->enqueue_node_status(uid, status_code, timestamp_ms);
          });
    }

    LOG_INFO(
        "[{}] Monitor: HTTP /backend/plugin-http/behavior_tree_monitor/* | WS "
        "/backend/plugin-ws/behavior_tree_monitor ({}, event-driven)",
        TAG, ws_registered ? "enabled" : "unavailable");
    return true;
  } catch (const std::exception& e) {
    LOG_ERROR("[{}] Behavior tree monitor start failed: {}", TAG, e.what());
  } catch (...) {
    LOG_ERROR("[{}] Behavior tree monitor start failed with unknown error", TAG);
  }
  // 如果异常发生在 managed worker 已启动之后，必须唤醒它并
  // 让它返回，不能把半启动的监控任务留给 Host。
  if (monitor_) monitor_->stop();
  return false;
}

/**
 * @brief 停止监控功能
 */
void BehaviorTreePlug::stopMonitor() noexcept {
  // 先断开 BTManager -> monitor_ 的回调，再唤醒并停止托管 worker。
  // clearMonitorCallbacks() 内部的互斥锁会等待已经进入的一次投递结束。
  if (monitor_ && started_.load(std::memory_order_acquire)) {
    if (auto* manager = G_BT_MANAGER()) manager->clearMonitorCallbacks();
  }

  // stop() 是幂等的；on_stop 与 on_unload 都可以调用。保留对象到插件析构，避免
  // Host 中尚在收尾的回调观察到悬空 monitor_ 指针。
  if (monitor_) monitor_->stop();
}
