

#include "bt_manager.hpp"

#include <fstream>

#include "auto_registration.hpp"
#include "bt_action/jog_joint.hpp"
#include "bt_action/jog_joint_to_angle.hpp"
#include "bt_action/move_absj.hpp"
#include "bt_action/move_absj_joint_target.hpp"
#include "bt_condition/is_enable.hpp"
#include "bt_condition/is_system_running.hpp"
#include "tinyxml2.h"
#include <behaviortree_cpp/xml_parsing.h>

constexpr auto CONFIG_PATH_BT_NODES = "../bt_trees/bt_nodes.xml";

bool BTManager::init() {
  stopping_.store(false, std::memory_order_release);

  blackboard_ = BT::Blackboard::create();
  // blackboard_->set<int>("gateway_status", 0);  // 默认空闲

  std::ofstream xmlFile(CONFIG_PATH_BT_NODES);
  if (!xmlFile) {
    LOG_ERROR("[{}] ❌ 无法打开配置文件进行保存: {}", TAG, CONFIG_PATH_BT_NODES);
    return false;
  }

  try {
    xmlFile << dump_nodes_xml();
  } catch (const std::exception& e) {
    LOG_ERROR("[{}] ❌ 写入行为树节点时发生异常: {}", TAG, e.what());
    return false;
  }

  LOG_INFO("[{}] ✅ 行为树节点已生成: {}", TAG, CONFIG_PATH_BT_NODES);
  return true;
}

void BTManager::setMonitorCallbacks(MonitorTreeResetCallback tree_reset,
                                    MonitorNodeStatusCallback node_status) {
  std::lock_guard<std::mutex> lock(monitor_callbacks_mtx_);
  monitor_tree_reset_callback_ = std::move(tree_reset);
  monitor_node_status_callback_ = std::move(node_status);
}

void BTManager::clearMonitorCallbacks() noexcept {
  std::lock_guard<std::mutex> lock(monitor_callbacks_mtx_);
  monitor_tree_reset_callback_ = {};
  monitor_node_status_callback_ = {};
}

bool BTManager::loadTree(const std::string& xml) {
  {
    std::lock_guard lk(tree_mtx_);
    pending_xml_ = xml;
    has_pending_ = true;
  }
  cv_.notify_all();
  return true;
}

BTManager::OperationResult BTManager::start(int period) {
  std::lock_guard lifecycle_lock(lifecycle_mtx_);

  if (stopping_.load(std::memory_order_acquire)) {
    return {false, "behavior tree manager is stopping"};
  }

  // 保留 start() 的幂等性：行为树已经在运行时，重复调用不再创建第二个线程。
  if (running_.load(std::memory_order_acquire)) {
    return {true, {}};
  }

  // loadTree() 只负责记录待加载 XML。如果没有待加载文件，start 不能
  // 继续返回成功，否则前端会误以为空行为树已经启动。
  if (!has_pending_.load(std::memory_order_acquire)) {
    return {false, "no behavior tree has been loaded"};
  }

  // 在创建 tick 工作线程前同步创建行为树。这样 XML 结构错误、未注册节点、
  // 保留节点名等 createTreeFromFile() 异常可以直接返回 HTTP 调用方。
  const OperationResult switch_result = switchTree();
  if (!switch_result.success) {
    return switch_result;
  }

  tick_period_ = std::chrono::milliseconds(period > 0 ? period : 50);
  running_.store(true, std::memory_order_release);
  paused_.store(false, std::memory_order_release);

  try {
    worker_ = std::thread([this] { BTManager::loop(); });
  } catch (const std::exception& e) {
    running_.store(false, std::memory_order_release);
    paused_.store(true, std::memory_order_release);

    // 线程创建失败时回滚已创建的树和 Publisher，避免下次 start() 看到
    // “无 pending XML，但内部仍残留一棵树”的不一致状态。
    {
      std::lock_guard<std::mutex> tree_lock(tree_mtx_);
      if (tree_.rootNode()) tree_.haltTree();
      reset();
    }
    return {false, std::string("failed to create behavior tree thread: ") + e.what()};
  } catch (...) {
    running_.store(false, std::memory_order_release);
    paused_.store(true, std::memory_order_release);
    {
      std::lock_guard<std::mutex> tree_lock(tree_mtx_);
      if (tree_.rootNode()) tree_.haltTree();
      reset();
    }
    return {false, "failed to create behavior tree thread: unknown error"};
  }

  return {true, {}};
}

void BTManager::pause() { paused_ = true; }

void BTManager::resume() {
  paused_ = false;
  cv_.notify_all();
}

BT::NodeStatus BTManager::step() {
  pause();
  std::lock_guard<std::mutex> lk(tree_mtx_);
  if (!tree_.rootNode()) return BT::NodeStatus::IDLE;
  return tree_.tickExactlyOnce();
}

void BTManager::stop() {
  std::lock_guard lifecycle_lock(lifecycle_mtx_);
  if (!running_.exchange(false, std::memory_order_acq_rel)) return;

  cv_.notify_all();

  if (worker_.joinable()) worker_.join();

  std::lock_guard<std::mutex> lk(tree_mtx_);
  if (tree_.rootNode()) tree_.haltTree();

  reset();
}

std::string BTManager::dump_nodes_xml() {
  using namespace tinyxml2;

  XMLDocument doc;

  // <root>
  auto* root = doc.NewElement("root");
  root->SetAttribute("BTCPP_format", "4");
  doc.InsertEndChild(root);

  auto* model = doc.NewElement("TreeNodesModel");
  root->InsertEndChild(model);

  for (const auto& node : NodeRegistry::instance().getAllMeta()) {
    // Action / Condition / Decorator / Control
    auto* node_elem = doc.NewElement(node.type.c_str());
    node_elem->SetAttribute("ID", node.name.c_str());

    for (const auto& p : node.ports) {
      const char* tag = nullptr;

      if (p.direction == "Input") {
        tag = "input_port";
      } else if (p.direction == "Output") {
        tag = "output_port";
      } else {
        tag = "inout_port";
      }

      auto* port = doc.NewElement(tag);
      port->SetAttribute("name", p.name.c_str());
      port->SetAttribute("type", p.type.c_str());

      if (!p.default_value.empty()) {
        port->SetAttribute("default", p.default_value.c_str());
      }

      if (!p.description.empty()) {
        port->SetAttribute("description", p.description.c_str());
      }

      node_elem->InsertEndChild(port);
    }

    model->InsertEndChild(node_elem);
  }

  XMLPrinter printer;
  doc.Print(&printer);

  return printer.CStr();
}

bool BTManager::is_running() const { return running_.load(); }

void BTManager::set_model_size(const int& sz) { model_size.store(sz, std::memory_order_relaxed); }

int BTManager::get_model_size() const { return model_size.load(std::memory_order_relaxed); }

void BTManager::set_gateway_status(const int& st) {
  gateway_status.store(st, std::memory_order_relaxed);
  if (st == 2 && is_running()) stop();
}

int BTManager::get_gateway_status() const { return gateway_status.load(std::memory_order_relaxed); }

void BTManager::set_idle_stable_count(int v) {
  idle_stable_count.store(v, std::memory_order_relaxed);
}

int BTManager::inc_idle_stable_count() {
  return idle_stable_count.fetch_add(1, std::memory_order_relaxed) + 1;
}

bool BTManager::update_rt(const void* data, size_t size) {
  overall_system_rtstate::SystemRtState updated;
  if (!updated.ParseFromArray(data, size)) return false;

  const int model_count = updated.model_size();
  {
    std::lock_guard state_lock(state_mtx_);
    rt_msg = std::move(updated);
  }
  set_model_size(model_count);
  return true;
}

bool BTManager::update_nrt(const void* data, size_t size) {
  overall_system_nrtstate::SystemNrtState updated;
  if (!updated.ParseFromArray(data, size)) return false;

  std::lock_guard state_lock(state_mtx_);
  nrt_msg = std::move(updated);
  return true;
}

overall_system_rtstate::SystemRtState BTManager::get_rt() const {
  std::lock_guard state_lock(state_mtx_);
  return rt_msg;
}

overall_system_nrtstate::SystemNrtState BTManager::get_nrt() const {
  std::lock_guard state_lock(state_mtx_);
  return nrt_msg;
}

void BTManager::loop() {
  std::unique_lock<std::mutex> lk(cv_mtx_);

  while (running_) {
    cv_.wait(lk, [this] { return !paused_ || !running_ || has_pending_; });
    if (!running_) break;

    if (has_pending_) {
      lk.unlock();
      // 运行期间收到新的 load 请求时仍保留原有异步切树能力。
      // switchTree() 内部会记录详细错误；首次 start 则由上面的同步路径返回错误。
      (void)switchTree();
      lk.lock();
      continue;
    }

    {
      std::lock_guard<std::mutex> tree_lock(tree_mtx_);
      if (tree_.rootNode()) {
        tree_.tickOnce();
      }
    }

    cv_.wait_for(lk, tick_period_, [this] { return paused_ || !running_ || has_pending_; });
  }
}

BTManager::OperationResult BTManager::switchTree() {
  std::lock_guard<std::mutex> lk(tree_mtx_);

  try {
    if (tree_.rootNode()) tree_.haltTree();

    observer_.reset();
    publisher_.reset();

    if (!factory_) {
      factory_ = std::make_unique<BT::BehaviorTreeFactory>();
      // registerNodes(*factory_);
      NodeRegistry::instance().registerAll(*factory_);
    }

    tree_ = factory_->createTreeFromFile(pending_xml_, blackboard_);
    // tree_ = factory_->createTreeFromText(pending_xml_, blackboard_);

    publisher_ = std::make_unique<BT::Groot2Publisher>(tree_);

    observer_ = std::make_unique<StatusObserver>(
        tree_,
        [this](const auto& node, auto prev, auto curr) { onNodeStatusChanged(node, prev, curr); });

    has_pending_.store(false, std::memory_order_release);

    onTreeReset();

    LOG_DEBUG("switchTree()");
    return {true, {}};

  } catch (const std::exception& e) {
    LOG_ERROR("[BT] load failed: {}", e.what());
    has_pending_.store(false, std::memory_order_release);

    // createTreeFromFile() 可能在部分成员已创建后抛出异常。失败时统一清理，
    // 确保 start() 返回失败后管理器中不会留下半初始化的树或 Publisher。
    if (tree_.rootNode()) tree_.haltTree();
    reset();
    return {false, e.what()};
  } catch (...) {
    LOG_ERROR("[BT] load failed with unknown error");
    has_pending_.store(false, std::memory_order_release);
    if (tree_.rootNode()) tree_.haltTree();
    reset();
    return {false, "unknown behavior tree loading error"};
  }
}

void BTManager::onNodeStatusChanged(const BT::TreeNode& node, BT::NodeStatus prev,
                                    BT::NodeStatus curr) {
  // StatusObserver 在 tick 线程内同步调用本函数。监控回调只将
  // UID、新状态和时间戳放入队列，不在此处发送 WebSocket。
  // 当节点回到 IDLE 时，使用 10 + prev 编码。这与
  // Groot2Publisher 原状态缓冲区的 11~14 编码保持一致，
  // 前端无需因从轮询切换为事件驱动而修改状态解析。
  std::uint8_t status_code = static_cast<std::uint8_t>(curr);
  if (curr == BT::NodeStatus::IDLE) {
    status_code = static_cast<std::uint8_t>(10 + static_cast<int>(prev));
  }

  std::lock_guard<std::mutex> lock(monitor_callbacks_mtx_);
  if (monitor_node_status_callback_) {
    try {
      monitor_node_status_callback_(node.UID(), status_code, ts_now_ms());
    } catch (const std::exception& e) {
      // 监控是附加功能；投递失败不能中断行为树 tick。
      LOG_ERROR("[BT] monitor status callback failed: {}", e.what());
    } catch (...) {
      LOG_ERROR("[BT] monitor status callback failed with unknown error");
    }
  }
}

void BTManager::onTreeReset() {
  LOG_INFO("[BT] tree reset");

  // WriteTreeToXML(..., true, true) 与 Groot2Publisher 使用相同的生成方式，
  // 会在 XML 中附加 _uid/_fullPath。前端正是通过这个 UID
  // 把后续节点状态事件匹配到正确的树节点。
  try {
    const std::string tree_xml = BT::WriteTreeToXML(tree_, true, true);

    std::lock_guard<std::mutex> lock(monitor_callbacks_mtx_);
    if (monitor_tree_reset_callback_) monitor_tree_reset_callback_(tree_xml);
  } catch (const std::exception& e) {
    // 树已经创建成功时，不能因为监控快照生成失败而让
    // switchTree() 判定整棵行为树加载失败。
    LOG_ERROR("[BT] monitor tree reset callback failed: {}", e.what());
  } catch (...) {
    LOG_ERROR("[BT] monitor tree reset callback failed with unknown error");
  }
}

void BTManager::registerNodes(BT::BehaviorTreeFactory& f) {
  f.registerSimpleAction("SayHello", [](BT::TreeNode&) { return BT::NodeStatus::SUCCESS; });
}

void BTManager::reset() {
  observer_.reset();
  publisher_.reset();
  factory_.reset();
  tree_ = {};
}

// bt action
REGISTER_NODE(JogJoint, "JogJoint");
REGISTER_NODE(JogJointToAngle, "JogJointToAngle");
REGISTER_NODE(MoveAbsJ, "MoveAbsJ");
REGISTER_NODE(MoveAbsJJointTarget, "MoveAbsJJointTarget");

// bt condition
REGISTER_NODE(IsSystemRunning, "IsSystemRunning");
REGISTER_NODE(IsEnable, "IsEnable");
