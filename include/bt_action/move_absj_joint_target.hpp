#pragma once

#if 0
#  include <behaviortree_cpp/action_node.h>

#  include <atomic>
#  include <chrono>
#  include <memory>

#  include "bt_manager.hpp"
#  include "plugin_api/plugin_logx.hpp"

class MoveAbsJJointTarget final : public BT::StatefulActionNode {
public:
  MoveAbsJJointTarget(const std::string& name, const BT::NodeConfig& config)
      : BT::StatefulActionNode(name, config) {
    models_ = G_BT_MANAGER()->get_model_size();
  }

  static BT::PortsList providedPorts() {
    return {
        BT::InputPort<std::vector<int>>("robot_sel", "0;1", "Robot selection"),
        BT::InputPort<std::string>("jointtarget", "P0", "Joint target"),
        BT::InputPort<std::string>("speed", "v100", "Move speed"),
    };
  }

  // onStart：发送异步命令
  BT::NodeStatus onStart() override {
    if (G_BT_MANAGER()->is_stopping() || !G_BT_MANAGER()->is_conn_codeit()) {
      return BT::NodeStatus::FAILURE;
    }

    std::vector<int> robot_sel;
    if (!getInput("robot_sel", robot_sel)) {
      LOG_ERROR("[MoveAbsJointTarget] missing robot_sel");
      return BT::NodeStatus::FAILURE;
    }

    std::string jointtarget{};
    std::string speed{};
    getInput("jointtarget", jointtarget);
    getInput("speed", speed);

    nlohmann::json j = CmdDynBuilder()
                           .init_models(models_)              // 设置模型
                           .select_models(robot_sel)          // 选择模型
                           .set_event("MoveAbsJJointTarget")  // 设置事件
                           .add_field({{"jointtarget", jointtarget}})
                           .add_field({{"speed", speed}})
                           .build();

    const auto str = j.dump();
    // G_BT_MANAGER()->get_pubsub()->emit("main.handler.pub.dyncmd", str.data(), str.size());
    // return BT::NodeStatus::RUNNING;

    auto req
        = G_BT_MANAGER()->get_reqrep()->request("main.handler.req.btcmd", str.data(), str.size());
    if (!req) {
      LOG_WARN("[MoveAbsJJointTarget] Failed to send request");
      return BT::NodeStatus::FAILURE;
    }

    // 等待响应
    if (!req->wait(2000)) {
      LOG_WARN("[MoveAbsJJointTarget] Request timed out");
      return BT::NodeStatus::FAILURE;
    }

    if (req->result() != ReqStatus::Ok) {
      LOG_ERROR("[MoveAbsJJointTarget] Request failed with status {}", int(req->result()));
      return BT::NodeStatus::FAILURE;
    }

    // config().blackboard->set<int>("gateway_status", 1);
    G_BT_MANAGER()->set_gateway_status(1);
    return BT::NodeStatus::RUNNING;
  }

  // onRunning：轮询状态
  BT::NodeStatus onRunning() override {
    if (!G_BT_MANAGER()->is_conn_codeit()) return BT::NodeStatus::FAILURE;

    auto status = config().blackboard->get<int>("gateway_status");
    // LOG_DEBUG("get gateway_status: {}", status);
    switch (status) {
      case 0:
        return BT::NodeStatus::SUCCESS;
      case 1:
        return BT::NodeStatus::RUNNING;
      case 2:
        return BT::NodeStatus::FAILURE;
      default:
        return BT::NodeStatus::FAILURE;
    }
  }

  // onHalted：中断
  void onHalted() override {}

private:
  int models_{1};
};

#endif

#pragma once

#include <behaviortree_cpp/action_node.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

#include "bt_json_request.hpp"
#include "bt_manager.hpp"

class MoveAbsJJointTarget final : public BT::StatefulActionNode {
public:
  MoveAbsJJointTarget(const std::string& name, const BT::NodeConfig& config)
      : BT::StatefulActionNode(name, config) {
    models_ = G_BT_MANAGER()->get_model_size();
  }

  ~MoveAbsJJointTarget() override { bt_req_.cancel(); }

  static BT::PortsList providedPorts() {
    return {
        BT::InputPort<std::vector<int>>("robot_sel", "0;1", "Robot selection"),
        BT::InputPort<std::string>("jointtarget", "P0", "Joint target"),
        BT::InputPort<std::string>("speed", "v100", "Move speed"),
    };
  }

  BT::NodeStatus onStart() override {
    using namespace std::chrono_literals;

    if (!G_BT_MANAGER()->is_conn_codeit()) return BT::NodeStatus::FAILURE;

    std::vector<int> robot_sel;
    if (!getInput("robot_sel", robot_sel)) {
      LOG_ERROR("[MoveAbsJJointTarget] missing robot_sel");
      return BT::NodeStatus::FAILURE;
    }

    std::string jointtarget;
    std::string speed;

    getInput("jointtarget", jointtarget);
    getInput("speed", speed);

    nlohmann::json j = bt_manager_cmd::CmdDynBuilder{}
                           .init_models(models_)
                           .select_models(robot_sel)
                           .set_event("MoveAbsJJointTarget")
                           .add_field({{"jointtarget", jointtarget}})
                           .add_field({{"speed", speed}})
                           .build();

    if (!bt_req_.start(G_BT_MANAGER()->get_reqrep(), "main.handler.req.dyncmd", j, 10s)) {
      LOG_ERROR("[MoveAbsJJointTarget] request start failed: {}", bt_req_.message());
      return BT::NodeStatus::FAILURE;
    }

    return BT::NodeStatus::RUNNING;
  }

  BT::NodeStatus onRunning() override {
    if (G_BT_MANAGER()->is_stopping() || !G_BT_MANAGER()->is_conn_codeit()) {
      bt_req_.cancel();
      return BT::NodeStatus::FAILURE;
    }

    return bt_req_.tick("MoveAbsJJointTarget");
  }

  void onHalted() override { bt_req_.cancel(); }

private:
  int models_{1};

  BtJsonRequest bt_req_;
};
