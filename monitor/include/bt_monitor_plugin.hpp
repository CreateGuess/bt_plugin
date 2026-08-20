#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include <plugin_core/api/plugin_api.hpp>
#include <plugin_core/sdk/plugin_sdk.hpp>
#include <plugin_core/api/plugin_http_api.hpp>
#include <plugin_core/api/plugin_websocket_api.hpp>


/**
 * @brief 一条从 Groot2 紧凑状态缓冲区解码出的节点状态
 */
struct BehaviorTreeNodeStatusUpdate {
  std::uint16_t uid{0};
  std::uint8_t status_code{0};
  std::string status;
  std::int64_t timestamp_ms{0};
};

/**
 * @brief Groot2 监控协议的同步请求客户端。
 *
 * 该类只负责“监控桥 -> Groot2Publisher”这一段通信：
 *
 *   BehaviorTree.CPP / Groot2Publisher
 *                 ^
 *                 | ZeroMQ REQ/REP
 *                 |
 *             Groot2Client
 *
 * 它不创建行为树、不执行 tick，也不注册 xplugin 的 HTTP/WebSocket 接口。
 * xplugin 生命周期仍由 BehaviorTreePlug 管理。这样可以把底层二进制协议与
 * 上层缓存、JSON 转换、WebSocket 推送分开，避免监控代码影响行为树业务逻辑。
 *
 * 线程约束：一个 Groot2Client 实例只在监控轮询线程中使用，不支持多个线程
 * 同时调用。ZeroMQ 的 socket 也不会跨线程移动。
 *
 * 异常策略：构造、发送、接收或协议校验失败时抛出异常，由外层 poll_loop()
 * 统一记录健康状态、通知 WebSocket 客户端，并在延迟后重建整个连接。
 */
class Groot2Client {
public:
  Groot2Client(std::string host, int port, int recv_timeout_ms, int send_timeout_ms);
  ~Groot2Client();  

  Groot2Client(const Groot2Client&) = delete;
  Groot2Client& operator=(const Groot2Client&) = delete;

  std::string get_full_tree_xml();
  std::vector<std::uint8_t> get_status_buffer();

private:
  std::vector<std::vector<std::uint8_t>> request(std::uint8_t request_type);
  static std::vector<std::uint8_t> make_header(std::uint8_t request_type);
  void close() noexcept;

private:
  std::string address_;
  void* context_{nullptr};
  void* socket_{nullptr};
};

/**
 * @brief 负责行为树监控功能的实现
 * - 管理后台线程。
 * - 调用 Groot2Client。
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

  static constexpr std::uint8_t PROTOCOL_ID = 2;
  static constexpr std::uint8_t REQ_FULLTREE = static_cast<std::uint8_t>('T');
  static constexpr std::uint8_t REQ_STATUS = static_cast<std::uint8_t>('S');

  BehaviorTreeMonitor(WsSendText ws_send_text, WsSendLatestText ws_send_latest_text);
  ~BehaviorTreeMonitor();

  BehaviorTreeMonitor(const BehaviorTreeMonitor&) = delete;
  BehaviorTreeMonitor& operator=(const BehaviorTreeMonitor&) = delete;

  void start();
  void stop() noexcept;

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

  void start_poll_thread();
  void stop_poll_thread() noexcept;
  void poll_loop() noexcept;

  nlohmann::json convert_xml_to_tree(const std::string& xml_text) const;
  std::vector<BehaviorTreeNodeStatusUpdate> parse_status_buffer(
      const std::vector<std::uint8_t>& buffer) const;

  void broadcast_tree(const nlohmann::json& tree_message) noexcept;
  void broadcast_status(const BehaviorTreeNodeStatusUpdate& update) noexcept;
  void broadcast_bridge_status(std::string status, std::string message) noexcept;
  void send_initial_snapshot(const char* session_id) noexcept;

  bool send_ws_text_checked(const char* session_id, std::string_view text) noexcept;
  bool send_ws_latest_text_checked(const char* session_id, const char* topic,
                                   std::string_view text) noexcept;
  void remove_dead_session(const char* session_id, PluginWsSendResult result) noexcept;

  static PluginHttpResponse json_response(int status_code, int code, std::string message,
                                          nlohmann::json data = nullptr);
  static std::int64_t current_time_ms() noexcept;

private:
  WsSendText ws_send_text_;
  WsSendLatestText ws_send_latest_text_;

  std::string bt_host_{"192.168.2.36"};
  int bt_port_{1667};
  int zmq_recv_timeout_ms_{5000};
  int zmq_send_timeout_ms_{5000};
  int status_poll_interval_ms_{50};
  int reconnect_delay_ms_{2000};

  std::atomic<bool> running_{false};
  std::thread poll_thread_;

  std::mutex viewers_mutex_;
  std::unordered_set<std::string> viewers_;

  mutable std::mutex state_mutex_;
  std::optional<nlohmann::json> latest_tree_message_;
  std::unordered_map<std::uint16_t, std::string> latest_status_;

  mutable std::mutex health_mutex_;
  std::string last_error_;
  std::atomic<bool> backend_connected_{false};
  std::atomic<std::int64_t> last_success_ms_{0};

  std::atomic<std::uint64_t> ws_send_failures_{0};

  static constexpr const char* TAG = "BehaviorTreeMonitor";
};
