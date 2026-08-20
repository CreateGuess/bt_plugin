#pragma once

#include <behaviortree_cpp/action_node.h>

#include <atomic>
#include <chrono>
#include <memory>

#include "bt_manager.hpp"

class JogJoint final : public BT::StatefulActionNode {
public:
  JogJoint(const std::string& name, const BT::NodeConfig& config)
      : BT::StatefulActionNode(name, config) {
    models_ = G_BT_MANAGER()->get_model_size();
  }

  static BT::PortsList providedPorts() {
    return {
        BT::InputPort<std::vector<int>>("robot_sel", "0;1", "Robot selection"),
        BT::InputPort<int>("increase_count", 50, "Increase step count"),
        BT::InputPort<int>("motion_id", 0, "Motion id"),
        BT::InputPort<int>("direction", 1, "Direction"),
    };
  }

  // onStart：发送异步命令
  BT::NodeStatus onStart() override {
    using namespace std::chrono_literals;
    if (G_BT_MANAGER()->is_stopping() || !G_BT_MANAGER()->is_conn_codeit())
      return BT::NodeStatus::FAILURE;

    std::vector<int> robot_sel;
    if (!getInput("robot_sel", robot_sel)) {
      LOG_ERROR("[JogJoint] missing robot_sel");
      return BT::NodeStatus::FAILURE;
    }

    int increase_count = 50;
    int motion_id = 0;
    int direction = 1;

    getInput("increase_count", increase_count);
    getInput("motion_id", motion_id);
    getInput("direction", direction);

    nlohmann::json j = bt_manager_cmd::CmdDynBuilder()
                           .init_models(models_)      // 设置模型
                           .select_models(robot_sel)  // 选择模型
                           .set_event("JogJoint")     // 设置事件
                           .add_field({{"increase_count", increase_count}})
                           .add_field({{"motion_id", motion_id}})
                           .add_field({{"direction", direction}})
                           .build();

    auto ret
        = ReqRepJson::call_sync(G_BT_MANAGER()->get_reqrep(), "main.handler.req.dyncmd", j, 5000ms);
    if (!ret) {
      LOG_ERROR("[JogJoint] request failed, req_status={}, code={}, message={}",
                static_cast<int>(ret.req_status), ret.code, ret.message);

      return BT::NodeStatus::FAILURE;
    }

    return BT::NodeStatus::SUCCESS;
  }

  // onRunning：轮询状态
  BT::NodeStatus onRunning() override { return BT::NodeStatus::SKIPPED; }

  // onHalted：中断
  void onHalted() override {}

private:
  int models_{1};
};
