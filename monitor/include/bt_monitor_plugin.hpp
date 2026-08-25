#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include <plugin_core/api/plugin_api.hpp>
#include <plugin_core/sdk/plugin_sdk.hpp>
#include <plugin_core/api/plugin_http_api.hpp>
#include <plugin_core/api/plugin_websocket_api.hpp>


/**
 * @brief 一条由 BTManager 状态回调产生的节点状态
 */
struct BehaviorTreeNodeStatusUpdate {
  std::uint16_t uid{0};
  std::uint8_t status_code{0};
  std::string status;
  std::int64_t timestamp_ms{0};
};

/**
 * @brief Host 托管 worker 内部使用的监控事件。
 *
 * BTManager 的状态回调只负责把轻量数据放入队列；XML 解析、
 * 缓存更新和 WebSocket 发送由 xplugin Host 托管的 worker 执行。
 * 这样即使某个前端发送较慢，也不会阻塞行为树 tick 线程。
 */
struct BehaviorTreeMonitorEvent {
  enum class Type { TreeReset, NodeStatus };

  Type type{Type::NodeStatus};
  std::string tree_xml;
  BehaviorTreeNodeStatusUpdate status;
};

/**
 * @brief 负责行为树监控功能的实现
 * - 管理后台线程。
 * - 接收 BTManager 投递的行为树事件。
 * - 解析 XML 和状态。
 * - 缓存树和节点状态。
 * - 处理 HTTP 请求。
 * - 管理 WebSocket 会话。
 * - 推送实时变化。
 * - 记录健康状态。
 */
class BehaviorTreeMonitor {
public:
  using WsSendText =
      std::function<PluginWsSendResult(const char* session_id, std::string_view text)>;
  using WsSendLatestText = std::function<PluginWsSendResult(
      const char* session_id, const char* topic, std::string_view text)>;

  BehaviorTreeMonitor(WsSendText ws_send_text, WsSendLatestText ws_send_latest_text);
  ~BehaviorTreeMonitor();

  BehaviorTreeMonitor(const BehaviorTreeMonitor&) = delete;
  BehaviorTreeMonitor& operator=(const BehaviorTreeMonitor&) = delete;

  void start();
  void stop() noexcept;

  /**
   * @brief 由 PluginBase::start_managed_worker() 调用的事件消费入口。
   *
   * BehaviorTreeMonitor 不创建、不 join 任何 std::thread。工作线程的
   * 创建、停止请求和回收全部交给 xplugin Host 管理。
   */
  void run_event_loop(PluginStopToken stop) noexcept;

  // 下面两个函数可从行为树 tick 线程调用。它们只入队，
  // 不解析 XML、不加锁监控状态缓存、不执行任何网络发送。
  void enqueue_tree_reset(std::string tree_xml) noexcept;
  void enqueue_node_status(std::uint16_t uid, std::uint8_t status_code,
                           std::int64_t timestamp_ms) noexcept;

  PluginHttpResponse handle_http_request(const PluginHttpRequest& req);
  void handle_ws_open(const char* session_id);
  void handle_ws_close(const char* session_id);
  void handle_ws_message(const char* session_id, const void* data, std::size_t size,
                         PluginWsMessageType type);

private:
  PluginHttpResponse handle_tree_snapshot();
  PluginHttpResponse handle_status_snapshot();
  PluginHttpResponse handle_bridge_health();
  PluginHttpResponse handle_load_node();

  void process_event(BehaviorTreeMonitorEvent event) noexcept;

  nlohmann::json convert_xml_to_tree(const std::string& xml_text) const;
  void broadcast_tree(const nlohmann::json& tree_message) noexcept;
  void broadcast_status(const BehaviorTreeNodeStatusUpdate& update) noexcept;
  void broadcast_bridge_status(std::string status, std::string message) noexcept;
  void send_initial_snapshot(const char* session_id) noexcept;

  bool send_ws_text_checked(const char* session_id, std::string_view text) noexcept;
  bool send_ws_latest_text_checked(const char* session_id, const char* topic,
                                   std::string_view text) noexcept;
  void remove_dead_session(const char* session_id, PluginWsSendResult result) noexcept;

  /**
   * @brief 生成与主行为树插件格式一致的 HTTP JSON 响应。
   *
   * @param status_code HTTP 状态码，例如 200、404、500。
   * @param request_code 业务结果码：0 表示成功，-1 表示失败。
   * @param request_message 成功时为 "ok"，失败时为具体错误信息。
   * @param data 可选的业务数据；为 null 时不输出 data 字段。
   */
  static PluginHttpResponse json_response(int status_code, int request_code,
                                          std::string request_message,
                                          nlohmann::json data = nullptr);
  static std::int64_t current_time_ms() noexcept;

private:
  WsSendText ws_send_text_;
  WsSendLatestText ws_send_latest_text_;

  std::atomic<bool> running_{false};

  // 事件队列是 BTManager 回调线程和 Host 托管 worker 之间的边界。
  // 树切换事件必须保留；同一节点的状态更新会由 Host 的
  // ws_send_latest_text 再做一次主题级合并，防止慢客户端无限堆积。
  std::mutex event_mutex_;
  std::condition_variable event_cv_;
  std::deque<BehaviorTreeMonitorEvent> event_queue_;
  std::atomic<std::uint64_t> received_events_{0};
  std::atomic<std::uint64_t> coalesced_events_{0};
  std::atomic<std::uint64_t> processed_events_{0};

  std::mutex viewers_mutex_;
  std::unordered_set<std::string> viewers_;

  mutable std::mutex state_mutex_;
  std::optional<nlohmann::json> latest_tree_message_;
  std::unordered_map<std::uint16_t, std::string> latest_status_;

  mutable std::mutex health_mutex_;
  std::string last_error_;
  std::atomic<std::int64_t> last_success_ms_{0};

  std::atomic<std::uint64_t> ws_send_failures_{0};

  static constexpr const char* TAG = "BehaviorTreeMonitor";
};
