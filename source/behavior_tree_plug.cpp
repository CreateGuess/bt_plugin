#include "behavior_tree_plug.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <regex>

#include "bt_manager.hpp"

namespace fs = std::filesystem;

namespace {
  constexpr const char* BT_TREE_DIRECTORY = "../bt_trees";

  /**
   * @brief 解析并验证行为树 XML 文件路径
   * 
   * @return std::optional<fs::path> 返回解析后的绝对路径，如果路径无效或不安全则返回 std::nullopt
   */
  std::optional<fs::path> resolveTreePath(const std::string& file) {
    if (file.empty() || fs::path(file).is_absolute()) return std::nullopt;

    std::error_code ec;
    const auto base = fs::weakly_canonical(fs::path(BT_TREE_DIRECTORY), ec);
    if (ec) return std::nullopt;

    const auto candidate = fs::weakly_canonical(base / file, ec);
    if (ec) return std::nullopt;

    const auto relative = candidate.lexically_relative(base);
    if (relative.empty()) return std::nullopt;
    for (const auto& component : relative) {
      if (component == "..") return std::nullopt;
    }

    return candidate;
  }

}  // namespace

namespace {
  PluginHttpResponse toHttpResponse(const JsonRpcResult& result) {
    // PluginHttpResponse res;
    // res.status_code = result.http_status;
    // res.content_type = "application/json; charset=utf-8";
    // nlohmann::json out{{"code", result.code}, {"message", result.message}};
    // if (!result.data.is_null()) out["data"] = result.data;
    // const auto text = out.dump();
    // res.body.assign(text.begin(), text.end());
    // return res;
    return ::to_http_response(result);
  }
}  // namespace

bool BehaviorTreePlug::on_init(IPluginContext& ctx) noexcept {
  LOGX_GLOBAL_CLASS(PLUGIN_LOG_NAME)::Construct();
  G_LOG()->set(logger());

  LOG_INFO("[{}] Initialized!", TAG);
  return true;
}

bool BehaviorTreePlug::on_start() noexcept {
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
    return true;
  }

  BTManager::Construct(pubsub(), reqrep());
  if (!G_BT_MANAGER()->init()) {
    BTManager::Destruct();
    started_.store(false, std::memory_order_release);
    return false;
  }

  subscribe("main.conn.codeit", [this](const char* topic, const void* data, size_t size) {
    handleConnCodeit(topic, data, size);
  });
  subscribe("main.handler.pub.gateway.status",
            [this](const char* topic, const void* data, size_t size) {
              handleGatewayStatus(topic, data, size);
            });
  subscribe("overall_system_rtstate", [this](const char* topic, const void* data, size_t size) {
    handleRtState(topic, data, size);
  });
  subscribe("overall_system_nrtstate", [this](const char* topic, const void* data, size_t size) {
    handleNrtState(topic, data, size);
  });

  routes_ = std::make_shared<const JsonRpcRouter::RouteMap>(buildRoutes());

  // 路由声明交给 Host
  PluginHttpEndpointOptions http_options;
  http_options.methods = {"POST"};
  http_options.routes.reserve(routes_->size());
  for (const auto& entry : *routes_) {
    http_options.routes.push_back(PluginHttpRoute{"/" + entry.first, {"POST"}});
  }
  http_options.max_body_size = 64 * 1024;
  http_options.timeout_ms = 10000;
  http_options.max_concurrency = 8;
  if (!register_http_endpoint(
          "behavior_tree", [this](const PluginHttpRequest& req) { return handleHttpRequest(req); },
          std::move(http_options))) {
    LOG_ERROR("[{}] Failed to register behavior_tree http endpoint", TAG);

    if (auto* manager = G_BT_MANAGER()) {
      manager->beginShutdown();
      manager->stop();
    }
    BTManager::Destruct();
    started_.store(false, std::memory_order_release);
    return false;
  }

  LOG_INFO("[{}] http: POST /backend/plugin-http/behavior_tree/<module> ({} routes)", TAG,
           routes_->size());

  // 监控作为当前插件库内的组件启动。托管 worker 或端点启动失败时
  // 回滚 BTManager，避免插件处于“行为树可用但监控未启动”的部分启动状态。
  if (!startMonitor()) {
    LOG_ERROR("[{}] Failed to start behavior tree monitor", TAG);
    if (auto* manager = G_BT_MANAGER()) {
      manager->beginShutdown();
      manager->stop();
    }
    BTManager::Destruct();
    started_.store(false, std::memory_order_release);
    return false;
  }

  LOG_INFO("[{}] Started!", TAG);
  return true;
}

void BehaviorTreePlug::on_stop() noexcept {
  // 先断开监控事件回调并停止分发线程，再销毁行为树管理器。
  stopMonitor();

  bool expected = true;
  if (!started_.compare_exchange_strong(expected, false, std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
    LOG_INFO("[{}] Stopped!", TAG);
    return;
  }

  if (auto* manager = G_BT_MANAGER()) {
    manager->beginShutdown();
    manager->stop();
  }
  BTManager::Destruct();

  LOG_INFO("[{}] Stopped!", TAG);
}

void BehaviorTreePlug::on_unload() noexcept {
  LOG_INFO("[{}] onUnload called!", TAG);

  stopMonitor();
  LOGX_GLOBAL_CLASS(PLUGIN_LOG_NAME)::Destruct();
}

void BehaviorTreePlug::handleConnCodeit(const char*, const void* data, size_t size) {
  if (!data || size != sizeof(bool)) return;

  bool value{};
  std::memcpy(&value, data, sizeof(bool));

  G_BT_MANAGER()->set_conn_codeit(value);
  if (!value && G_BT_MANAGER()->is_running()) {
    G_BT_MANAGER()->stop();
  }
}

void BehaviorTreePlug::handleGatewayStatus(const char*, const void* data, size_t size) {
  if (!data || size != sizeof(int)) return;

  int status{};
  std::memcpy(&status, data, sizeof(int));

  G_BT_MANAGER()->set_gateway_status(status);
}

void BehaviorTreePlug::handleRtState(const char*, const void* data, size_t size) {
  try {
    if (!data || size == 0) return;

    G_BT_MANAGER()->update_rt(data, size);
  } catch (...) {
  }
}

void BehaviorTreePlug::handleNrtState(const char*, const void* data, size_t size) {
  try {
    if (!data || size == 0) return;

    G_BT_MANAGER()->update_nrt(data, size);
  } catch (...) {
  }
}

JsonRpcRouter::RouteMap BehaviorTreePlug::buildRoutes() {
  return {
      {"load", [this](const nlohmann::json& data) { return handleLoad(data); }},
      {"read", [this](const nlohmann::json& data) { return handleRead(data); }},
      {"start", [this](const nlohmann::json& data) { return handleStart(data); }},
      {"stop", [this](const nlohmann::json& data) { return handleStop(data); }},
      {"save", [this](const nlohmann::json& data) { return handleSaveNode(data); }},
  };
}

/**
 * @brief 保存行为树 XML 文件
 */
JsonRpcResult BehaviorTreePlug::handleSaveNode(const nlohmann::json& data) {
  try {
    // 1. 检查请求数据是否包含 file 和 content 字段，且类型为字符串
    if (!data.contains("file") || !data["file"].is_string()) {
      return JsonRpcResult::error("Missing or invalid string field: file", -1, 400);
    }
    // 2. 检查请求数据是否包含 content 字段，且类型为字符串
    if (!data.contains("content") || !data["content"].is_string()) {
      return JsonRpcResult::error("Missing or invalid string field: content", -1, 400);
    }
    // 3. 获取 file 和 content 字段的值
    const std::string file = data["file"].get<std::string>();
    const std::string content = data["content"].get<std::string>();

    if (file.empty()) {
      return JsonRpcResult::error("File name cannot be empty", -1, 400);
    }

    if (content.empty()) {
      return JsonRpcResult::error("XML content cannot be empty", -1, 400);
    }

    // 4. 验证文件名规则
    /*
     * 文件名规则：
     *
     * 1. 第一个字符必须是英文字母或数字；
     * 2. 中间只允许英文字母、数字、下划线和连字符；
     * 3. 必须以 .xml 结尾；
     * 4. 不允许目录分隔符和相对路径。
     *
     * 合法示例：
     *   custom_node.xml
     *   tree-01.xml
     *   MyTree.xml
     *
     * 非法示例：
     *   ../test.xml
     *   subdir/test.xml
     *   test.json
     *   test node.xml
     */
    static const std::regex file_name_pattern(R"(^[A-Za-z0-9][A-Za-z0-9_-]{0,126}\.xml$)");

    if (!std::regex_match(file, file_name_pattern)) {
      return JsonRpcResult::error(
          "Invalid file name; expected "
          "[A-Za-z0-9][A-Za-z0-9_-]*.xml",
          -1, 400);
    }

    // 5. 检查是否为保留文件
    /*
     * bt_nodes.xml 是 BTManager 自动生成的节点模型文件。
     * 禁止前端覆盖，避免影响插件下一次初始化。
     */
    if (file == "bt_nodes.xml") {
      return JsonRpcResult::error("The reserved file bt_nodes.xml cannot be overwritten", -1, 400);
    }

    /*
     * 再通过现有 resolveTreePath() 做一次路径边界检查。
     * 即使以后修改了文件名规则，也能阻止文件写出 bt_trees 目录。
     */
    const auto resolved = resolveTreePath(file);
    if (!resolved) {
      return JsonRpcResult::error("Invalid behavior tree file path", -1, 400);
    }

    // 6. 检查目录是否存在，如果不存在则返回错误
    const fs::path& file_path = *resolved;
    // 确保父目录存在
    const fs::path& directory = file_path.parent_path();
    
    if (!fs::exists(directory) || !fs::is_directory(directory)) {
      return JsonRpcResult::error("Behavior tree directory not found: " + directory.string(), -1,
                                  404);
    }

    // 7. 将 content 写入指定的 XML 文件
    std::ofstream output(file_path, std::ios::out | std::ios::binary | std::ios::trunc);
    // 8. 检查文件是否成功打开
    if (!output.is_open()) {
      return JsonRpcResult::error("Failed to open file for writing: " + file_path.string(), -1,
                                  500);
    }
    // 9. 写入内容并检查写入是否成功
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    // 10. 检查写入是否成功
    output.flush();

    if (!output) {
      return JsonRpcResult::error("Failed to write XML file: " + file_path.string(), -1, 500);
    }
    // 11. 关闭文件流
    output.close();
    // 12. 返回成功结果，包含文件名和写入的字节数
    return JsonRpcResult::ok("XML saved successfully", nlohmann::json{
                                                           {"file", file},
                                                           {"size", content.size()},
                                                       });

  } catch (const std::exception& e) {
    return JsonRpcResult::error("Save XML exception: " + std::string(e.what()), -1, 500);
  } catch (...) {
    return JsonRpcResult::error("Save XML unknown exception", -1, 500);
  }
}

PluginHttpResponse BehaviorTreePlug::handleHttpRequest(const PluginHttpRequest& req) {
  try {
    // Host 已超时(504)或客户端已断开时不再继续处理
    if (req.stop_token.stop_requested()) {
      return toHttpResponse(JsonRpcResult::error("request canceled", -1, 503));
    }

    // Host 已按声明的 routes 匹配
    std::string module = req.route_pattern;
    if (!module.empty() && module.front() == '/') module.erase(0, 1);

    if (req.body_spooled_to_file()) {
      return toHttpResponse(JsonRpcResult::error("request body too large", -1, 413));
    }

    const auto route = routes_->find(module);
    if (route == routes_->end()) {
      return toHttpResponse(JsonRpcResult::error("Handler not found: " + module, -1, 404));
    }

    nlohmann::json data = nlohmann::json::object();
    if (!req.text().empty()) {
      data = nlohmann::json::parse(req.text());
      if (!data.is_object()) {
        return toHttpResponse(JsonRpcResult::error("Request body must be a json object", -1, 400));
      }
    }

    return toHttpResponse(route->second(data));
  } catch (const nlohmann::json::parse_error& e) {
    return toHttpResponse(JsonRpcResult::error(std::string("Invalid json: ") + e.what(), -1, 400));
  } catch (const std::exception& e) {
    return toHttpResponse(JsonRpcResult::error(std::string("Http RPC error: ") + e.what()));
  } catch (...) {
    return toHttpResponse(JsonRpcResult::error("Http RPC unknown error"));
  }
}

JsonRpcResult BehaviorTreePlug::handleLoad(const nlohmann::json& data) {
  try {
    std::string file = data.value("file", "");
    if (file.empty()) return JsonRpcResult::error("No file specified");

    auto resolved = resolveTreePath(file);
    if (!resolved) return JsonRpcResult::error("Invalid tree file path", -1, 400);

    const fs::path& file_path = *resolved;
    if (!fs::exists(file_path)) {
      return JsonRpcResult::error("File not found: " + file_path.string());
    }

    bool ok = G_BT_MANAGER()->loadTree(file_path.string());
    if (!ok) return JsonRpcResult::error("Failed to load tree: " + file_path.string());

    return JsonRpcResult::ok("Load successful");
  } catch (const std::exception& e) {
    return JsonRpcResult::error("Handler exception: " + std::string(e.what()));
  } catch (...) {
    return JsonRpcResult::error("Handler unknown exception");
  }
}

JsonRpcResult BehaviorTreePlug::handleRead(const nlohmann::json& data) {
  try {
    std::string file = data.value("file", "");
    fs::path base_path = BT_TREE_DIRECTORY;

    if (!file.empty()) {
      auto resolved = resolveTreePath(file);
      if (!resolved) return JsonRpcResult::error("Invalid tree file path", -1, 400);

      const fs::path& file_path = *resolved;
      if (!fs::exists(file_path)) {
        return JsonRpcResult::error("File not found: " + file_path.string(), -1, 404);
      }

      std::ifstream ifs(file_path, std::ios::in);
      if (!ifs) return JsonRpcResult::error("Failed to open file: " + file_path.string());

      std::ostringstream oss;
      oss << ifs.rdbuf();
      return JsonRpcResult::ok("ok", nlohmann::json{{"content", oss.str()}});
    }

    if (!fs::exists(base_path) || !fs::is_directory(base_path)) {
      return JsonRpcResult::error("Directory not found: " + base_path.string(), -1, 404);
    }

    nlohmann::json files = nlohmann::json::array();
    for (auto& entry : fs::directory_iterator(base_path)) {
      if (entry.is_regular_file()) {
        files.push_back(entry.path().filename().string());
      }
    }

    return JsonRpcResult::ok("ok", nlohmann::json{{"files", files}});
  } catch (const std::exception& e) {
    return JsonRpcResult::error("Handler exception: " + std::string(e.what()));
  } catch (...) {
    return JsonRpcResult::error("Handler unknown exception");
  }
}

JsonRpcResult BehaviorTreePlug::handleStart(const nlohmann::json& data) {
  try {
    const int period = data.value("period", 1000);
    const auto result = G_BT_MANAGER()->start(period);

    // start() 只有在 XML 解析、行为树创建、Groot2Publisher 创建以及 tick 线程
    // 启动全部成功后才返回 success=true。将 BehaviorTree.CPP 的具体解析错误
    // 原样返回前端，避免日志中已失败但 HTTP 仍显示 start 成功。
    if (!result.success) {
      return JsonRpcResult::error("Failed to start behavior tree: " + result.error, -1, 422);
    }

    return JsonRpcResult::ok("ok");
  } catch (const std::exception& e) {
    return JsonRpcResult::error("Start behavior tree exception: " + std::string(e.what()), -1,
                                500);
  } catch (...) {
    return JsonRpcResult::error("Start behavior tree unknown exception", -1, 500);
  }
}

JsonRpcResult BehaviorTreePlug::handleStop(const nlohmann::json&) {
  try {
    G_BT_MANAGER()->stop();
    return JsonRpcResult::ok();
  } catch (const std::exception& e) {
    return JsonRpcResult::error("Handler exception: " + std::string(e.what()));
  } catch (...) {
    return JsonRpcResult::error("Handler unknown exception");
  }
}

PLUGIN_DECLARE(BehaviorTreePlug, "BehaviorTreePlugin", "1.0", "liuyan", "BehaviorTree process")
