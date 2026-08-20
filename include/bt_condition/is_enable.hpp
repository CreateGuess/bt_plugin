#pragma once

#include "auto_registration.hpp"
#include "behaviortree_cpp/condition_node.h"
#include "bt_manager.hpp"

class IsEnable : public BT::ConditionNode {
public:
  using RT = overall_system_rtstate::SystemRtState;

  IsEnable(const std::string& name, const BT::NodeConfig& config)
      : BT::ConditionNode(name, config) {}

  static BT::PortsList providedPorts() {
    return {
        BT::InputPort<std::vector<int>>("robot_sel", "0", "Robot selection"),
    };
  }

  BT::NodeStatus tick() override {
    std::vector<int> robot_sel{};
    if (!getInput("robot_sel", robot_sel)) {
      LOG_ERROR("[IsEnable] missing robot_sel");
      return BT::NodeStatus::FAILURE;
    }

    const auto rt_msg = G_BT_MANAGER()->get_rt();
    bool all_enabled = true;

    for (int idx : robot_sel) {
      if (idx < 0 || idx >= rt_msg.model_size()) {
        LOG_ERROR("[IsEnable] invalid model index: {}", idx);
        return BT::NodeStatus::FAILURE;
      }

      const auto& mod = rt_msg.model(idx);

      for (const auto& joint : mod.joint()) {
        if (!joint.is_enabled()) {
          all_enabled = false;
          break;
        }
      }

      if (!all_enabled) break;
    }

    bool running = rt_msg.system_running_state() && G_BT_MANAGER()->is_conn_codeit();
    return (running && all_enabled) ? BT::NodeStatus::SUCCESS : BT::NodeStatus::FAILURE;
  }
};
