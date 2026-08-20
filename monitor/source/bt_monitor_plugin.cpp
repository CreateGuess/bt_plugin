#include "bt_monitor_plugin.hpp"

#include <tinyxml2.h>
#include <zmq.h>

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
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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
   * @breif 生成随机数
   */
  std::uint32_t random_u32() {
    thread_local std::mt19937 generator(std::random_device{}());
    std::uniform_int_distribution<std::uint32_t> distribution(
        1U, std::numeric_limits<std::uint32_t>::max());
    return distribution(generator);
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

  void set_zmq_int_option(void* socket, int option, int value, const char* option_name) {
    if (zmq_setsockopt(socket, option, &value, sizeof(value)) != 0) {
      throw std::runtime_error(std::string("zmq_setsockopt(") + option_name
                               + ") failed: " + zmq_strerror(zmq_errno()));
    }
  }

}  // namespace

/**
 * @brief 构造函数
 * @param host Groot2 Publisher 的主机地址
 * @param port Groot2 Publisher 的端口
 * @param recv_timeout_ms 接收超时时间（毫秒）
 * @param send_timeout_ms 发送超时时间（毫秒）
 */
Groot2Client::Groot2Client(std::string host, int port, int recv_timeout_ms, int send_timeout_ms)
    : address_("tcp://" + std::move(host) + ":" + std::to_string(port)) {
  // 1. 创建 ZeroMQ 上下文
  context_ = zmq_ctx_new();
  if (!context_) {
    throw std::runtime_error(std::string("zmq_ctx_new failed: ") + zmq_strerror(zmq_errno()));
  }
  // 2. 创建 ZMQ_REQ 套接字，使用 REQ-REP 模式与 Groot2 Publisher 通信
  // 发送一次请求，接收一次响应，再发送下一次请求
  socket_ = zmq_socket(context_, ZMQ_REQ);
  if (!socket_) {
    const std::string error = zmq_strerror(zmq_errno());
    close();
    throw std::runtime_error("zmq_socket(ZMQ_REQ) failed: " + error);
  }
  // 3. 超时配置
  try {
    set_zmq_int_option(socket_, ZMQ_RCVTIMEO, recv_timeout_ms, "ZMQ_RCVTIMEO");
    set_zmq_int_option(socket_, ZMQ_SNDTIMEO, send_timeout_ms, "ZMQ_SNDTIMEO");
    set_zmq_int_option(socket_, ZMQ_LINGER, 0, "ZMQ_LINGER");

    if (zmq_connect(socket_, address_.c_str()) != 0) {
      throw std::runtime_error("zmq_connect(" + address_
                               + ") failed: " + zmq_strerror(zmq_errno()));
    }
  } catch (...) {
    close();
    throw;
  }
}

Groot2Client::~Groot2Client() { close(); }

/**
 * @brief 关闭 ZeroMQ 套接字和上下文
 */
void Groot2Client::close() noexcept {
  if (socket_) {
    zmq_close(socket_);
    socket_ = nullptr;
  }
  if (context_) {
    zmq_ctx_term(context_);
    context_ = nullptr;
  }
}

/**
 * @brief 构造 Groot2 请求头
 * - byte 0：协议版本
 * - byte 1：请求类型，'T' 获取完整树，'S' 获取状态
 * - byte 2~5：32位随机请求ID，小端序
 *
 * @param request_type 请求类型
 * @return 请求头
 */
std::vector<std::uint8_t> Groot2Client::make_header(std::uint8_t request_type) {
  const std::uint32_t unique_id = random_u32();
  return {
      BehaviorTreeMonitor::PROTOCOL_ID,
      request_type,
      static_cast<std::uint8_t>(unique_id & 0xFFU),
      static_cast<std::uint8_t>((unique_id >> 8U) & 0xFFU),
      static_cast<std::uint8_t>((unique_id >> 16U) & 0xFFU),
      static_cast<std::uint8_t>((unique_id >> 24U) & 0xFFU),
  };
}

/**
 * @brief 发送请求并接收 multipart 响应
 * @param request_type 请求类型
 * @return 响应数据
 */
std::vector<std::vector<std::uint8_t>> Groot2Client::request(std::uint8_t request_type) {
  // 1. 检查套接字是否有效
  if (!socket_) throw std::runtime_error("Groot2 ZMQ socket is closed");
  // 2. 构造请求头
  const auto header = make_header(request_type);
  // 3. 发送请求头
  const int sent = zmq_send(socket_, header.data(), header.size(), 0);
  // 4. 检查发送结果
  if (sent < 0) {
    throw std::runtime_error(std::string("Groot2 request send failed: ")
                             + zmq_strerror(zmq_errno()));
  }
  if (static_cast<std::size_t>(sent) != header.size()) {
    throw std::runtime_error("Groot2 request header was only partially sent");
  }
  // 5. 接收 multipart 响应
  std::vector<std::vector<std::uint8_t>> parts;
  while (true) {
    // 初始化 ZMQ 消息
    zmq_msg_t message;
    if (zmq_msg_init(&message) != 0) {
      throw std::runtime_error(std::string("zmq_msg_init failed: ") + zmq_strerror(zmq_errno()));
    }
    // 接收消息
    const int received = zmq_msg_recv(&message, socket_, 0);
    if (received < 0) {
      const std::string error = zmq_strerror(zmq_errno());
      zmq_msg_close(&message);
      throw std::runtime_error("Groot2 reply receive failed: " + error);
    }
    // 检查接收的消息大小
    const auto* begin = static_cast<const std::uint8_t*>(zmq_msg_data(&message));
    const std::size_t size = zmq_msg_size(&message);
    parts.emplace_back(begin, begin + size);
    // 检查是否还有更多消息
    int more = 0;
    std::size_t more_size = sizeof(more);
    if (zmq_getsockopt(socket_, ZMQ_RCVMORE, &more, &more_size) != 0) {
      const std::string error = zmq_strerror(zmq_errno());
      zmq_msg_close(&message);
      throw std::runtime_error("zmq_getsockopt(ZMQ_RCVMORE) failed: " + error);
    }

    zmq_msg_close(&message);
    if (!more) break;
  }

  return parts;
}

/**
 * @brief 获取完整的行为树 XML
 */
std::string Groot2Client::get_full_tree_xml() {
  const auto reply = request(BehaviorTreeMonitor::REQ_FULLTREE);
  if (reply.size() < 2) {
    throw std::runtime_error("FULLTREE reply parts invalid: " + std::to_string(reply.size()));
  }
  return std::string(reply[1].begin(), reply[1].end());
}

/**
 * @brief 获取行为树节点状态字节流
 */
std::vector<std::uint8_t> Groot2Client::get_status_buffer() {
  const auto reply = request(BehaviorTreeMonitor::REQ_STATUS);
  if (reply.size() < 2) {
    throw std::runtime_error("STATUS reply parts invalid: " + std::to_string(reply.size()));
  }
  return reply[1];
}

/**
 * @brief 构造函数
 */
BehaviorTreeMonitor::BehaviorTreeMonitor(WsSendText ws_send_text,
                                         WsSendLatestText ws_send_latest_text)
    : ws_send_text_(std::move(ws_send_text)),
      ws_send_latest_text_(std::move(ws_send_latest_text)) {}

BehaviorTreeMonitor::~BehaviorTreeMonitor() { stop_poll_thread(); }

/**
 * @brief 启动监控线程
 */
void BehaviorTreeMonitor::start() {
  start_poll_thread();
  LOG_INFO("[{}] Started: Groot2 {}:{}", TAG, bt_host_, bt_port_);
}

void BehaviorTreeMonitor::stop() noexcept {
  stop_poll_thread();
  {
    std::lock_guard<std::mutex> lock(viewers_mutex_);
    viewers_.clear();
  }
  LOG_INFO("[{}] Stopped", TAG);
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
  return json_response(200, 0, "behavior tree snapshot", *latest_tree_message_);
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
  return json_response(200, 0, "behavior tree status snapshot",
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

  nlohmann::json data = {
      {"running", running_.load()},
      {"backend_connected", backend_connected_.load()},
      {"backend", {{"host", bt_host_}, {"port", bt_port_}}},
      {"poll_interval_ms", status_poll_interval_ms_},
      {"viewer_count", viewer_count},
      {"last_success_ms", last_success_ms_.load()},
      {"last_error", last_error},
      {"ws_send_failures", ws_send_failures_.load()},
  };

  return json_response(200, 0, "behavior tree bridge health", std::move(data));
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
    return json_response(
        404, -1,
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
    LOG_ERROR("[{}] Invalid node model XML {}: {}", TAG, file_path->string(),
              document.ErrorStr());
    return json_response(422, -1,
                         std::string("invalid bt_nodes.xml: ") + document.ErrorStr());
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

  LOG_INFO("[{}] Loaded node model file: {} ({} nodes, {} bytes)", TAG,
           file_path->string(), node_count, xml_text.size());

  return json_response(200, 0, "behavior tree node model loaded",
                       {{"file", BT_NODES_FILE_NAME},
                        {"content", std::move(xml_text)},
                        {"node_count", node_count}});
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
 * @brief 启动轮询线程
 */
void BehaviorTreeMonitor::start_poll_thread() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) return;
  poll_thread_ = std::thread([this] { poll_loop(); });
}

/**
 * @brief 停止轮询线程
 */
void BehaviorTreeMonitor::stop_poll_thread() noexcept {
  running_.store(false);
  if (poll_thread_.joinable()) poll_thread_.join();
  backend_connected_.store(false);
}

/**
 * @brief 轮询循环
 */
void BehaviorTreeMonitor::poll_loop() noexcept {
  while (running_.load()) {
    try {
      LOG_INFO("[{}] Connecting Groot2 publisher: {}:{}", TAG, bt_host_, bt_port_);
      // 1. 创建 Groot2Client，建立 ZeroMQ 连接
      Groot2Client client(bt_host_, bt_port_, zmq_recv_timeout_ms_, zmq_send_timeout_ms_);
      backend_connected_.store(true);
      {
        std::lock_guard<std::mutex> lock(health_mutex_);
        last_error_.clear();
      }
      // 2. 获取完整的行为树 XML
      const std::string xml_text = client.get_full_tree_xml();
      // 3. 将 XML 转换为 JSON 树结构
      nlohmann::json tree_message = convert_xml_to_tree(xml_text);

      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        latest_tree_message_ = tree_message;
        latest_status_.clear();
      }
      // 4. 广播行为树 JSON 树结构给所有 WebSocket 连接的客户端
      last_success_ms_.store(current_time_ms());
      broadcast_tree(tree_message);
      // 5. 进入状态轮询循环
      while (running_.load()) {
        const auto begin = std::chrono::steady_clock::now();
        // 获取状态字节流并解析为节点状态更新列表
        const auto buffer = client.get_status_buffer();
        const auto updates = parse_status_buffer(buffer);
        // 对每个UID的状态更新，检查是否有变化，如果有变化则广播给所有 WebSocket 连接的客户端
        for (const auto& update : updates) {
          bool changed = false;
          {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto it = latest_status_.find(update.uid);
            if (it == latest_status_.end() || it->second != update.status) {
              latest_status_[update.uid] = update.status;
              changed = true;
            }
          }
          if (changed) {
            LOG_INFO("[{}] Node {} -> {}", TAG, update.uid, update.status);
            broadcast_status(update);
          }
        }
        // 更新最后成功时间戳，并根据轮询间隔计算睡眠时间
        last_success_ms_.store(current_time_ms());
        const auto elapsed = std::chrono::steady_clock::now() - begin;
        const auto period = std::chrono::milliseconds(status_poll_interval_ms_);
        // 如果轮询耗时小于轮询间隔，则睡眠剩余时间（默认一次轮询50ms）
        if (elapsed < period) std::this_thread::sleep_for(period - elapsed);
      }
    } catch (const std::exception& e) {
      // 如果轮询线程抛出异常，记录错误日志，更新状态，并在运行标志为 true 时等待一段时间后重试
      backend_connected_.store(false);
      {
        std::lock_guard<std::mutex> lock(health_mutex_);
        last_error_ = e.what();
      }
      LOG_ERROR("[{}] Groot2 polling failed: {}", TAG, e.what());
      broadcast_bridge_status("error", e.what());
      // 如果轮询线程仍然运行，等待一段时间后重试连接
      if (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_delay_ms_));
      }
    } catch (...) {
      backend_connected_.store(false);
      {
        std::lock_guard<std::mutex> lock(health_mutex_);
        last_error_ = "unknown polling error";
      }
      LOG_ERROR("[{}] Groot2 polling failed with unknown error", TAG);
      broadcast_bridge_status("error", "unknown polling error");
      if (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_delay_ms_));
      }
    }
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
 * @brief 解析状态字节流为节点状态更新列表
 * @param buffer 状态字节流
 * @return 节点状态更新列表
 */
std::vector<BehaviorTreeNodeStatusUpdate> BehaviorTreeMonitor::parse_status_buffer(
    const std::vector<std::uint8_t>& buffer) const {
  // 1. 检查状态字节流的大小是否为3的倍数，如果不是则记录警告日志
  if (buffer.size() % 3 != 0) {
    LOG_WARN("[{}] Status buffer size is not a multiple of 3: {}", TAG, buffer.size());
  }
  // 2. 预分配节点状态更新列表的大小，避免频繁的内存分配
  std::vector<BehaviorTreeNodeStatusUpdate> updates;
  updates.reserve(buffer.size() / 3);
  const std::int64_t timestamp = current_time_ms();
  // 4. 遍历状态字节流，每3个字节表示一个节点的状态更新，解析出
  // UID、状态码和状态名称，并添加到节点状态更新列表中
  for (std::size_t offset = 0; offset + 3 <= buffer.size(); offset += 3) {
    const std::uint16_t uid = static_cast<std::uint16_t>(buffer[offset])
                              | (static_cast<std::uint16_t>(buffer[offset + 1]) << 8U);
    const std::uint8_t code = buffer[offset + 2];
    updates.push_back({uid, code, status_name(code), timestamp});
  }
  return updates;
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

PluginHttpResponse BehaviorTreeMonitor::json_response(int status_code, int code,
                                                      std::string message, nlohmann::json data) {
  PluginHttpResponse response;
  response.status_code = status_code;
  response.content_type = "application/json; charset=utf-8";

  nlohmann::json body = {
      {"code", code},
      {"message", std::move(message)},
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
