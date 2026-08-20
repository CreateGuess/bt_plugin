

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

bool BTManager::loadTree(const std::string& xml) {
  {
    std::lock_guard lk(tree_mtx_);
    pending_xml_ = xml;
    has_pending_ = true;
  }
  cv_.notify_all();
  return true;
}

void BTManager::start(int period) {
  std::lock_guard lifecycle_lock(lifecycle_mtx_);
  if (running_.load(std::memory_order_acquire) || stopping_.load(std::memory_order_acquire)) {
    return;
  }

  tick_period_ = std::chrono::milliseconds(period > 0 ? period : 50);
  running_.store(true, std::memory_order_release);
  paused_.store(false, std::memory_order_release);

  worker_ = std::thread([this] { BTManager::loop(); });
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
      switchTree();
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

void BTManager::switchTree() {
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

    has_pending_ = false;

    onTreeReset();

  } catch (const std::exception& e) {
    LOG_ERROR("[BT] load failed: {}", e.what());
    has_pending_ = false;
  }

  LOG_DEBUG("switchTree()");
}

void BTManager::onNodeStatusChanged(const BT::TreeNode& node, BT::NodeStatus prev,
                                    BT::NodeStatus curr) {
  // spdlog::info("[BT] {} {} -> {}", node.fullPath(), toStr(prev), toStr(curr));
  nlohmann::json msg = {{"type", "status"},        {"name", node.name()}, {"path", node.fullPath()},
                        {"uid", node.UID()},       {"prev", toStr(prev)}, {"curr", toStr(curr)},
                        {"timestamp", ts_now_ms()}};
  // G_WS_MANAGER.broadcastText(msg.dump());
}

void BTManager::onTreeReset() {
  LOG_INFO("[BT] tree reset");

  // 👉 通知前端清空
  // G_WS_MANAGER.broadcastText("tree reset");
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
