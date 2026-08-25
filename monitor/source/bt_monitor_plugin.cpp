#include "bt_monitor_plugin.hpp"

#include <tinyxml2.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <plugin_core/sdk/plugin_logx.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

  namespace fs = std::filesystem;

  // 与原行为树插件 /behavior_tree/read 使用完全相同的目录规则。
  // rpc_gateway 启动后，该相对路径由 Host 进程的当前工作目录解析。
  constexpr const char* BT_TREE_DIRECTORY = "../bt_trees";
  constexpr const char* BT_NODES_FILE_NAME = "bt_nodes.xml";

  /**
   * @brief 定位部署目录中的 bt_trees/bt_nodes.xml。
   *
   * 这里故意与 BehaviorTreePlug::handleRead() 保持一致：
   *   ../bt_trees + bt_nodes.xml
   *
   * 既然 /backend/plugin-http/behavior_tree/read 已经能够列出该目录，
   * 那么同一插件、同一 Host 进程中的 load.node 也应使用同一基准路径。
   */
  std::optional<fs::path> resolve_bt_nodes_path() {
    const fs::path file_path = fs::path(BT_TREE_DIRECTORY) / BT_NODES_FILE_NAME;
    std::error_code ec;
    if (!fs::is_regular_file(file_path, ec) || ec) return std::nullopt;

    const fs::path canonical_path = fs::weakly_canonical(file_path, ec);
    if (ec) return std::nullopt;
    return canonical_path;
  }

  /**
   * @brief
   */
  std::string strip_namespace(std::string tag) {
    if (const auto brace = tag.rfind('}'); brace != std::string::npos) {
      tag.erase(0, brace + 1);
    }
    if (const auto colon = tag.rfind(':'); colon != std::string::npos) {
      tag.erase(0, colon + 1);
    }
    return tag;
  }

  std::optional<std::uint32_t> parse_unsigned(const char* text) {
    if (!text || *text == '\0') return std::nullopt;
    try {
      std::size_t consumed = 0;
      const unsigned long value = std::stoul(text, &consumed, 10);
      if (text[consumed] != '\0' || value > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
      }
      return static_cast<std::uint32_t>(value);
    } catch (...) {
      return std::nullopt;
    }
  }

  std::optional<std::uint32_t> get_uid_from_element(const tinyxml2::XMLElement* element) {
    static constexpr std::array<const char*, 5> keys = {"_uid", "UID", "uid", "uid16", "node_uid"};
    for (const char* key : keys) {
      if (const char* value = element->Attribute(key)) {
        if (auto uid = parse_unsigned(value)) return uid;
      }
    }
    return std::nullopt;
  }

  std::string get_node_name(const tinyxml2::XMLElement* element) {
    static constexpr std::array<const char*, 3> keys = {"name", "ID", "id"};
    for (const char* key : keys) {
      if (const char* value = element->Attribute(key); value && *value != '\0') {
        return value;
      }
    }
    return strip_namespace(element->Name() ? element->Name() : "Unknown");
  }

  std::string status_name(std::uint8_t code) {
    switch (code) {
      case 0:
        return "IDLE";
      case 1:
        return "RUNNING";
      case 2:
        return "SUCCESS";
      case 3:
        return "FAILURE";
      case 4:
        return "SKIPPED";
      case 11:
        return "IDLE_FROM_RUNNING";
      case 12:
        return "IDLE_FROM_SUCCESS";
      case 13:
        return "IDLE_FROM_FAILURE";
      case 14:
        return "IDLE_FROM_SKIPPED";
      default:
        return "UNKNOWN_" + std::to_string(code);
    }
  }

}  // namespace

/**
 * @brief 构造函数
 */
BehaviorTreeMonitor::BehaviorTreeMonitor(WsSendText ws_send_text,
                                         WsSendLatestText ws_send_latest_text)
    : ws_send_text_(std::move(ws_send_text)),
      ws_send_latest_text_(std::move(ws_send_latest_text)) {}

BehaviorTreeMonitor::~BehaviorTreeMonitor() {
  // Host 在插件对象析构前已回收托管 worker。此处不调用
  // stop() 记日志，因为 on_unload() 可能已销毁日志实例。
  running_.store(false, std::memory_order_release);
  event_cv_.notify_all();
}

/**
 * @brief 将监控组件置为可接收事件状态。
 *
 * 本函数不创建线程。实际的事件消费入口 run_event_loop()
 * 由 BehaviorTreePlug 使用 start_managed_worker() 交给 Host 运行。
 */
void BehaviorTreeMonitor::start() {
  running_.store(true, std::memory_order_release);
  LOG_INFO("[{}] Started in event-driven mode", TAG);
}

void BehaviorTreeMonitor::stop() noexcept {
  // 不 join 线程：Host 会回收 start_managed_worker() 创建的 worker。
  // 这里只改变状态并唤醒可能阻塞在条件变量上的 worker。
  running_.store(false, std::memory_order_release);
  event_cv_.notify_all();

  {
    std::lock_guard<std::mutex> lock(event_mutex_);
    event_queue_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(viewers_mutex_);
    viewers_.clear();
  }
  {
    // 插件下次 start 会构造新的 BTManager。清理旧树快照，
    // 避免新行为树尚未加载时 HTTP 误返回上一次运行的数据。
    std::lock_guard<std::mutex> lock(state_mutex_);
    latest_tree_message_.reset();
    latest_status_.clear();
  }
  last_success_ms_.store(0, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(health_mutex_);
    last_error_.clear();
  }
  LOG_INFO("[{}] Stopped", TAG);
}

/**
 * @brief 投递“行为树已重建”事件。
 *
 * 该函数会在 BTManager 持有树锁时被调用，因此只移动字符串并
 * 入队。XML 解析和 WebSocket 广播由 Host 托管的
 * run_event_loop() 异步执行。
 */
void BehaviorTreeMonitor::enqueue_tree_reset(std::string tree_xml) noexcept {
  // 1. 如果监控已停止，直接丢弃事件。
  if (!running_.load(std::memory_order_acquire)) return;

  try {
    // 2. 构造事件并入队。队列中只保留最新的树切换事件，旧的树切换事件会被丢弃。
    BehaviorTreeMonitorEvent event;
    event.type = BehaviorTreeMonitorEvent::Type::TreeReset;
    event.tree_xml = std::move(tree_xml);
    {
      std::lock_guard<std::mutex> lock(event_mutex_);
      if (!running_.load(std::memory_order_relaxed)) return;

      // 新树已经使队列中尚未处理的旧树状态失效。先清理它们，
      // 可以保证切树后不会再把旧 UID 的状态发给前端。
      coalesced_events_.fetch_add(event_queue_.size(), std::memory_order_relaxed);
      event_queue_.clear();
      event_queue_.push_back(std::move(event));
      received_events_.fetch_add(1, std::memory_order_relaxed);
    }
    event_cv_.notify_one();
  } catch (const std::exception& e) {
    LOG_ERROR("[{}] Failed to enqueue tree reset event: {}", TAG, e.what());
  } catch (...) {
    LOG_ERROR("[{}] Failed to enqueue tree reset event", TAG);
  }
}

/**
 * @brief 投递节点状态变化事件。
 *
 * 状态回调处于行为树 tick 路径上，这里绝不直接调用
 * ws_send_*()，以免前端慢连接影响行为树的执行周期。
 */
void BehaviorTreeMonitor::enqueue_node_status(std::uint16_t uid, std::uint8_t status_code,
                                              std::int64_t timestamp_ms) noexcept {
  // 1. 如果监控已停止，直接丢弃事件。
  if (!running_.load(std::memory_order_acquire)) return;

  try {
    // 2. 构造事件并入队。队列中同一 UID 的状态变化只保留最新的，旧的状态会被丢弃。
    BehaviorTreeMonitorEvent event;
    event.type = BehaviorTreeMonitorEvent::Type::NodeStatus;
    event.status = {uid, status_code, status_name(status_code), timestamp_ms};
    {
      std::lock_guard<std::mutex> lock(event_mutex_);
      if (!running_.load(std::memory_order_relaxed)) return;

      // WebSocket 使用 ws_send_latest_text 时，同一 UID 只需要保留最新
      // 待发状态。如果托管 worker 短时间跟不上 tick，在这里先合并
      // 队列中同 UID 的旧事件，避免队列无界增长。
      auto previous = event_queue_.end();
      for (auto it = event_queue_.begin(); it != event_queue_.end(); ++it) {
        if (it->type == BehaviorTreeMonitorEvent::Type::TreeReset) {
          previous = event_queue_.end();
        } else if (it->status.uid == uid) {
          previous = it;
        }
      }
      if (previous != event_queue_.end()) {
        event_queue_.erase(previous);
        coalesced_events_.fetch_add(1, std::memory_order_relaxed);
      }
      event_queue_.push_back(std::move(event));
      received_events_.fetch_add(1, std::memory_order_relaxed);
    }
    event_cv_.notify_one();
  } catch (const std::exception& e) {
    LOG_ERROR("[{}] Failed to enqueue node status event: {}", TAG, e.what());
  } catch (...) {
    LOG_ERROR("[{}] Failed to enqueue node status event", TAG);
  }
}

PluginHttpResponse BehaviorTreeMonitor::handle_http_request(const PluginHttpRequest& req) {
  try {
    if (req.stop_token.stop_requested()) {
      return json_response(503, -1, "request canceled");
    }
    if (req.body_spooled_to_file()) {
      return json_response(413, -1, "request body too large");
    }

    std::string route = req.route_pattern;
    if (!route.empty() && route.front() == '/') route.erase(0, 1);

    if (route == "tree.snapshot") return handle_tree_snapshot();
    if (route == "status.snapshot") return handle_status_snapshot();
    if (route == "bridge.health") return handle_bridge_health();
    if (route == "load.node") return handle_load_node();

    return json_response(404, -1, "handler not found: " + route);
  } catch (const std::exception& e) {
    return json_response(500, -1, std::string("HTTP handler error: ") + e.what());
  } catch (...) {
    return json_response(500, -1, "HTTP handler unknown error");
  }
}

/**
 * @brief 处理行为树快照请求
 */
PluginHttpResponse BehaviorTreeMonitor::handle_tree_snapshot() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!latest_tree_message_) {
    return json_response(503, -1, "behavior tree is not available yet");
  }
  return json_response(200, 0, "ok", *latest_tree_message_);
}

/**
 * @brief 处理行为树状态快照请求
 */
PluginHttpResponse BehaviorTreeMonitor::handle_status_snapshot() {
  nlohmann::json statuses = nlohmann::json::array();
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (const auto& [uid, status] : latest_status_) {
      statuses.push_back({
          {"uid", uid},
          {"status", status},
      });
    }
  }
  return json_response(200, 0, "ok",
                       {{"statuses", std::move(statuses)}, {"timestamp_ms", current_time_ms()}});
}

/**
 * @brief 处理行为树桥接健康检查请求
 */
PluginHttpResponse BehaviorTreeMonitor::handle_bridge_health() {
  std::string last_error;
  {
    std::lock_guard<std::mutex> lock(health_mutex_);
    last_error = last_error_;
  }

  std::size_t viewer_count = 0;
  {
    std::lock_guard<std::mutex> lock(viewers_mutex_);
    viewer_count = viewers_.size();
  }

  std::size_t pending_events = 0;
  {
    std::lock_guard<std::mutex> lock(event_mutex_);
    pending_events = event_queue_.size();
  }

  nlohmann::json data = {
      {"running", running_.load()},
      {"mode", "event_driven"},
      {"event_source_connected", running_.load()},
      // 保留旧健康接口字段，避免已部署前端因字段突然消失
      // 而报错。port=0 和 poll_interval_ms=0 明确表示已不再使用
      // TCP/ZeroMQ 轮询链路。
      {"backend_connected", running_.load()},
      {"backend", {{"host", "in-process"}, {"port", 0}}},
      {"poll_interval_ms", 0},
      {"viewer_count", viewer_count},
      {"pending_events", pending_events},
      {"received_events", received_events_.load()},
      {"coalesced_events", coalesced_events_.load()},
      {"processed_events", processed_events_.load()},
      {"last_success_ms", last_success_ms_.load()},
      {"last_error", last_error},
      {"ws_send_failures", ws_send_failures_.load()},
  };

  return json_response(200, 0, "ok", std::move(data));
}

/**
 * @brief 读取插件部署目录旁的 bt_trees/bt_nodes.xml。
 *
 * 成功响应中的 data.content 保存完整XML文本，字段命名与主行为树接口的read
 * 响应保持一致；node_count用于调用方快速确认节点模型数量。
 */
PluginHttpResponse BehaviorTreeMonitor::handle_load_node() {
  const auto file_path = resolve_bt_nodes_path();
  if (!file_path) {
    return json_response(404, -1,
                         "bt_nodes.xml not found; expected <rpc_gateway>/bt_trees/bt_nodes.xml");
  }

  std::ifstream input(*file_path, std::ios::in | std::ios::binary);
  if (!input) {
    LOG_ERROR("[{}] Failed to open node model file: {}", TAG, file_path->string());
    return json_response(500, -1, "failed to open bt_nodes.xml");
  }

  std::ostringstream stream;
  stream << input.rdbuf();
  if (input.bad()) {
    LOG_ERROR("[{}] Failed while reading node model file: {}", TAG, file_path->string());
    return json_response(500, -1, "failed to read bt_nodes.xml");
  }

  std::string xml_text = stream.str();
  if (xml_text.empty()) {
    return json_response(422, -1, "bt_nodes.xml is empty");
  }

  // 返回文件之前先验证XML，避免前端直到解析阶段才发现配置文件损坏。
  tinyxml2::XMLDocument document;
  const tinyxml2::XMLError parse_result = document.Parse(xml_text.data(), xml_text.size());
  if (parse_result != tinyxml2::XML_SUCCESS) {
    LOG_ERROR("[{}] Invalid node model XML {}: {}", TAG, file_path->string(), document.ErrorStr());
    return json_response(422, -1, std::string("invalid bt_nodes.xml: ") + document.ErrorStr());
  }

  const tinyxml2::XMLElement* root = document.RootElement();
  const tinyxml2::XMLElement* model = nullptr;
  if (root) {
    const std::string root_name = strip_namespace(root->Name() ? root->Name() : "");
    if (root_name == "TreeNodesModel") {
      model = root;
    } else {
      for (const tinyxml2::XMLElement* child = root->FirstChildElement(); child;
           child = child->NextSiblingElement()) {
        if (strip_namespace(child->Name() ? child->Name() : "") == "TreeNodesModel") {
          model = child;
          break;
        }
      }
    }
  }
  if (!model) {
    return json_response(422, -1, "bt_nodes.xml has no <TreeNodesModel> element");
  }

  std::size_t node_count = 0;
  for (const tinyxml2::XMLElement* node = model->FirstChildElement(); node;
       node = node->NextSiblingElement()) {
    ++node_count;
  }

  LOG_INFO("[{}] Loaded node model file: {} ({} nodes, {} bytes)", TAG, file_path->string(),
           node_count, xml_text.size());

  return json_response(
      200, 0, "ok",
      {{"file", BT_NODES_FILE_NAME}, {"content", std::move(xml_text)}, {"node_count", node_count}});
}

void BehaviorTreeMonitor::handle_ws_open(const char* session_id) {
  if (!session_id) return;
  {
    std::lock_guard<std::mutex> lock(viewers_mutex_);
    viewers_.insert(session_id);
  }
  LOG_INFO("[{}] Viewer connected: {}", TAG, session_id);
  send_initial_snapshot(session_id);
}

void BehaviorTreeMonitor::handle_ws_close(const char* session_id) {
  if (!session_id) return;
  {
    std::lock_guard<std::mutex> lock(viewers_mutex_);
    viewers_.erase(session_id);
  }
  LOG_INFO("[{}] Viewer disconnected: {}", TAG, session_id);
}

void BehaviorTreeMonitor::handle_ws_message(const char* session_id, const void* data,
                                            std::size_t size, PluginWsMessageType type) {
  if (!session_id || !data || type != PluginWsMessageType::Text) return;

  nlohmann::json message;
  try {
    message = nlohmann::json::parse(std::string_view(static_cast<const char*>(data), size));
  } catch (const std::exception& e) {
    const nlohmann::json error = {
        {"type", "error"},
        {"message", std::string("invalid JSON: ") + e.what()},
    };
    send_ws_text_checked(session_id, error.dump());
    return;
  }

  const std::string action = message.value("action", "");
  if (action == "ping") {
    send_ws_text_checked(
        session_id, nlohmann::json{{"type", "pong"}, {"timestamp_ms", current_time_ms()}}.dump());
    return;
  }
  if (action == "snapshot") {
    send_initial_snapshot(session_id);
    return;
  }

  send_ws_text_checked(
      session_id,
      nlohmann::json{{"type", "error"}, {"message", "unknown action: " + action}}.dump());
}

/**
 * @brief Host 托管 worker 执行的监控事件分发循环。
 *
 * worker 在没有状态变化时通过条件变量休眠，不再向
 * 1667 端口发送 STATUS 请求。
 *
 * wait_for 的超时只用于观察 PluginStopToken，不会触发数据查询。
 * 当 Host 请求停止但此时没有新的行为树事件时，worker
 * 最多 250 ms 就能观察到停止请求并返回。
 */
void BehaviorTreeMonitor::run_event_loop(PluginStopToken stop) noexcept {
  while (running_.load(std::memory_order_acquire) && !stop.stop_requested()) {
    BehaviorTreeMonitorEvent event;
    {
      // 1. 等待事件队列非空或 Host 请求停止
      std::unique_lock<std::mutex> lock(event_mutex_);
      event_cv_.wait_for(lock, std::chrono::milliseconds(250), [this, &stop] {
        return !running_.load(std::memory_order_acquire) || stop.stop_requested()
               || !event_queue_.empty();
      });
      // 2. 如果监控已停止或 Host 请求停止，退出循环
      if (!running_.load(std::memory_order_acquire) || stop.stop_requested()) break;
      // 3. 如果队列仍然为空，继续等待
      if (event_queue_.empty()) continue;
      // 4. 从队列中取出事件并移除
      event = std::move(event_queue_.front());
      // 5. 移除事件后再解锁，避免在处理事件时阻塞其他线程投递新事件
      event_queue_.pop_front();
    }
    // 6. 处理事件。此处不加锁状态缓存，避免阻塞行为树 tick 线程。
    process_event(std::move(event));
    processed_events_.fetch_add(1, std::memory_order_relaxed);
  }

  running_.store(false, std::memory_order_release);
  LOG_INFO("[{}] Managed event worker stopped", TAG);
}

/**
 * @brief 更新监控快照并将变化推送给 WebSocket 客户端。
 */
void BehaviorTreeMonitor::process_event(BehaviorTreeMonitorEvent event) noexcept {
  try {
    // 1. 根据事件类型更新快照并广播给 WebSocket 客户端
    if (event.type == BehaviorTreeMonitorEvent::Type::TreeReset) {
      nlohmann::json tree_message = convert_xml_to_tree(event.tree_xml);
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        latest_tree_message_ = tree_message;
        latest_status_.clear();
      }
      // 广播给所有 WebSocket 客户端。此处不加锁 viewers_，避免阻塞行为树 tick 线程。
      broadcast_tree(tree_message);
    } else {
      // 2. 更新节点状态快照并广播给 WebSocket 客户端。此处不加锁 viewers_，避免阻塞行为树 tick 线程。
      bool changed = false;
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        // 3. 如果状态变化与快照中已有状态相同，则不广播，避免前端重复渲染。
        const auto it = latest_status_.find(event.status.uid);
        if (it == latest_status_.end() || it->second != event.status.status) {
          latest_status_[event.status.uid] = event.status.status;
          changed = true;
        }
      }
      // 4. 如果状态发生变化，则广播给所有 WebSocket 客户端。
      if (changed) broadcast_status(event.status);
    }
    // 5. 更新健康检查的最后成功时间戳和清空错误信息。
    last_success_ms_.store(current_time_ms(), std::memory_order_release);
    std::lock_guard<std::mutex> lock(health_mutex_);
    last_error_.clear();
  } catch (const std::exception& e) {
    {
      std::lock_guard<std::mutex> lock(health_mutex_);
      last_error_ = e.what();
    }
    LOG_ERROR("[{}] Event processing failed: {}", TAG, e.what());
    broadcast_bridge_status("error", e.what());
  } catch (...) {
    {
      std::lock_guard<std::mutex> lock(health_mutex_);
      last_error_ = "unknown event processing error";
    }
    LOG_ERROR("[{}] Event processing failed with unknown error", TAG);
    broadcast_bridge_status("error", "unknown event processing error");
  }
}

/**
 * @brief 将行为树 XML 转换为 JSON 树结构
 * @param xml_text 行为树 XML 文本
 * @return 行为树 JSON 树结构
 */
nlohmann::json BehaviorTreeMonitor::convert_xml_to_tree(const std::string& xml_text) const {
  // 1. 解析 XML 文本
  tinyxml2::XMLDocument document;
  const tinyxml2::XMLError error = document.Parse(xml_text.data(), xml_text.size());
  // 2. 检查解析结果，如果失败则抛出异常
  if (error != tinyxml2::XML_SUCCESS) {
    throw std::runtime_error(std::string("failed to parse behavior tree XML: ")
                             + document.ErrorStr());
  }
  // 3. 获取 XML 根元素，如果没有根元素则抛出异常
  const tinyxml2::XMLElement* document_root = document.RootElement();
  if (!document_root) throw std::runtime_error("behavior tree XML has no root element");
  // 4. 查找 <BehaviorTree> 元素，如果没有找到则抛出异常
  const tinyxml2::XMLElement* behavior_tree = nullptr;
  if (strip_namespace(document_root->Name() ? document_root->Name() : "") == "BehaviorTree") {
    behavior_tree = document_root;
  } else {
    for (const tinyxml2::XMLElement* element = document_root->FirstChildElement(); element;
         element = element->NextSiblingElement()) {
      if (strip_namespace(element->Name() ? element->Name() : "") == "BehaviorTree") {
        behavior_tree = element;
        break;
      }
    }
  }
  if (!behavior_tree) throw std::runtime_error("No <BehaviorTree> found in XML");

  std::uint32_t generated_uid = 100000;
  std::size_t real_uid_count = 0;
  std::size_t generated_uid_count = 0;

  // 5. 递归遍历 XML 元素，构建 JSON 树结构
  std::function<std::optional<nlohmann::json>(const tinyxml2::XMLElement*)> walk;
  // Lambda 函数 walk 用于递归遍历 XML 元素，并将其转换为 JSON 节点
  walk = [&](const tinyxml2::XMLElement* element) -> std::optional<nlohmann::json> {
    const std::string tag = strip_namespace(element->Name() ? element->Name() : "Unknown");
    // 过滤掉 <TreeNodesModel> 和 <include> 元素，这些元素不需要出现在 JSON 树结构中
    if (tag == "TreeNodesModel" || tag == "include") return std::nullopt;
    // 获取节点的 UID，如果没有 UID，则生成一个新的 UID
    std::uint32_t uid = 0;
    if (const auto parsed_uid = get_uid_from_element(element)) {
      uid = *parsed_uid;
      ++real_uid_count;
    } else {
      uid = generated_uid++;
      ++generated_uid_count;
    }
    // 构建 JSON 节点，包含 UID、名称、类型、状态和子节点列表
    nlohmann::json node = {
        {"uid", uid},       {"name", get_node_name(element)},      {"kind", tag},
        {"status", "IDLE"}, {"children", nlohmann::json::array()},
    };
    // 递归遍历子元素，构建子节点列表
    for (const tinyxml2::XMLElement* child = element->FirstChildElement(); child;
         child = child->NextSiblingElement()) {
      if (auto child_node = walk(child)) node["children"].push_back(std::move(*child_node));
    }
    return node;
  };

  std::vector<nlohmann::json> runtime_children;
  // 遍历 <BehaviorTree> 元素的子元素，构建运行时子节点列表
  for (const tinyxml2::XMLElement* child = behavior_tree->FirstChildElement(); child;
       child = child->NextSiblingElement()) {
    if (auto child_node = walk(child)) runtime_children.push_back(std::move(*child_node));
  }
  // 如果没有运行时子节点，则抛出异常
  if (runtime_children.empty()) {
    throw std::runtime_error("<BehaviorTree> has no runtime child nodes");
  }
  // 如果只有一个运行时子节点，则将其作为根节点，否则创建一个新的根节点，包含所有运行时子节点
  nlohmann::json tree_root;
  if (runtime_children.size() == 1) {
    tree_root = std::move(runtime_children.front());
  } else {
    const char* tree_id = behavior_tree->Attribute("ID");
    tree_root = {
        {"uid", 999999},
        {"name", tree_id && *tree_id ? tree_id : "BehaviorTree"},
        {"kind", "BehaviorTree"},
        {"status", "IDLE"},
        {"children", std::move(runtime_children)},
    };
    ++generated_uid_count;
  }

  LOG_INFO("[{}] XML parsed: real_uid={}, generated_uid={}, root={}", TAG, real_uid_count,
           generated_uid_count, tree_root.value("name", ""));

  return {
      {"type", "tree"},
      {"root", std::move(tree_root)},
      {"raw_xml", xml_text},
  };
}

/**
 * @brief 广播行为树 JSON 树结构给所有 WebSocket 连接的客户端
 * @param tree_message 行为树 JSON 树结构
 */
void BehaviorTreeMonitor::broadcast_tree(const nlohmann::json& tree_message) noexcept {
  const std::string topic = "behavior_tree.tree";
  const std::string text = tree_message.dump();

  std::vector<std::string> targets;
  {
    std::lock_guard<std::mutex> lock(viewers_mutex_);
    targets.assign(viewers_.begin(), viewers_.end());
  }

  for (const auto& session_id : targets) {
    send_ws_latest_text_checked(session_id.c_str(), topic.c_str(), text);
  }
}

/**
 * @brief 广播节点状态更新给所有 WebSocket 连接的客户端
 * @param update 节点状态更新
 */
void BehaviorTreeMonitor::broadcast_status(const BehaviorTreeNodeStatusUpdate& update) noexcept {
  const std::string topic = "behavior_tree.node." + std::to_string(update.uid) + ".status";
  const nlohmann::json message = {
      {"type", "status"},
      {"uid", update.uid},
      {"status", update.status},
      {"status_code", update.status_code},
      {"timestamp_ms", update.timestamp_ms},
  };
  const std::string text = message.dump();

  std::vector<std::string> targets;
  {
    std::lock_guard<std::mutex> lock(viewers_mutex_);
    targets.assign(viewers_.begin(), viewers_.end());
  }

  for (const auto& session_id : targets) {
    send_ws_latest_text_checked(session_id.c_str(), topic.c_str(), text);
  }
}

/**
 * @brief 广播桥接状态给所有 WebSocket 连接的客户端
 * @param status 桥接状态
 * @param message 桥接消息
 */
void BehaviorTreeMonitor::broadcast_bridge_status(std::string status,
                                                  std::string message) noexcept {
  const std::string topic = "behavior_tree.bridge.status";
  const std::string text = nlohmann::json{
      {"type", "bridge_status"},
      {"status", std::move(status)},
      {"message", std::move(message)},
      {"timestamp_ms",
       current_time_ms()}}.dump();

  std::vector<std::string> targets;
  {
    std::lock_guard<std::mutex> lock(viewers_mutex_);
    targets.assign(viewers_.begin(), viewers_.end());
  }
  for (const auto& session_id : targets) {
    send_ws_latest_text_checked(session_id.c_str(), topic.c_str(), text);
  }
}

/**
 * @brief 发送初始快照给指定的 WebSocket 会话
 * @param session_id WebSocket 会话 ID
 */
void BehaviorTreeMonitor::send_initial_snapshot(const char* session_id) noexcept {
  std::optional<nlohmann::json> tree;
  std::unordered_map<std::uint16_t, std::string> statuses;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    tree = latest_tree_message_;
    statuses = latest_status_;
  }

  if (tree) send_ws_text_checked(session_id, tree->dump());

  const std::int64_t timestamp = current_time_ms();
  for (const auto& [uid, status] : statuses) {
    const nlohmann::json message = {
        {"type", "status"},
        {"uid", uid},
        {"status", status},
        {"timestamp_ms", timestamp},
    };
    send_ws_text_checked(session_id, message.dump());
  }
}

/**
 * @brief 发送 WebSocket 文本消息，并检查发送结果
 * @param session_id WebSocket 会话 ID
 */
bool BehaviorTreeMonitor::send_ws_text_checked(const char* session_id,
                                               std::string_view text) noexcept {
  const auto result = ws_send_text_(session_id, text);
  if (result == PluginWsSendResult::Queued) return true;
  ws_send_failures_.fetch_add(1, std::memory_order_relaxed);
  remove_dead_session(session_id, result);
  LOG_WARN("[{}] WebSocket send failed: result={}, session={}, bytes={}", TAG,
           plugin_ws_send_result_name(result), session_id ? session_id : "", text.size());
  return false;
}

/**
 * @brief 发送 WebSocket 最新文本消息，并检查发送结果
 * @param session_id WebSocket 会话 ID
 */
bool BehaviorTreeMonitor::send_ws_latest_text_checked(const char* session_id, const char* topic,
                                                      std::string_view text) noexcept {
  const auto result = ws_send_latest_text_(session_id, topic, text);
  if (result == PluginWsSendResult::Queued) return true;
  ws_send_failures_.fetch_add(1, std::memory_order_relaxed);
  remove_dead_session(session_id, result);
  LOG_WARN("[{}] WebSocket latest send failed: result={}, session={}, topic={}, bytes={}", TAG,
           plugin_ws_send_result_name(result), session_id ? session_id : "", topic ? topic : "",
           text.size());
  return false;
}

/**
 * @brief 移除已关闭或不存在的 WebSocket 会话
 * @param session_id WebSocket 会话 ID
 */
void BehaviorTreeMonitor::remove_dead_session(const char* session_id,
                                              PluginWsSendResult result) noexcept {
  if (!session_id) return;
  if (result != PluginWsSendResult::SessionClosed
      && result != PluginWsSendResult::SessionNotFound) {
    return;
  }
  std::lock_guard<std::mutex> lock(viewers_mutex_);
  viewers_.erase(session_id);
}

PluginHttpResponse BehaviorTreeMonitor::json_response(int status_code, int request_code,
                                                      std::string request_message,
                                                      nlohmann::json data) {
  PluginHttpResponse response;
  response.status_code = status_code;
  response.content_type = "application/json; charset=utf-8";

  // 使用与主行为树 HTTP 接口相同的统一响应包装。所有成功和失败响应
  // 都带有 request_source，便于前端用同一套解析逻辑处理主插件和监控接口。
  nlohmann::json body = {
      {"request_code", request_code},
      {"request_message", std::move(request_message)},
      {"request_source", "plugin"},
  };
  if (!data.is_null()) body["data"] = std::move(data);

  const std::string text = body.dump();
  response.body.assign(text.begin(), text.end());
  return response;
}

/**
 * @brief 获取当前时间的毫秒时间戳
 */
std::int64_t BehaviorTreeMonitor::current_time_ms() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
