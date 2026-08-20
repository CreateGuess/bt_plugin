#pragma once

#include "auto_registration.hpp"
#include "behaviortree_cpp/condition_node.h"
#include "bt_manager.hpp"

/*
InputPort	别人写	当前节点读	依赖外部数据
OutputPort	当前节点写	别人读	产生数据
*/
class IsSystemRunning : public BT::ConditionNode {
public:
  using RT = overall_system_rtstate::SystemRtState;

  IsSystemRunning(const std::string& name, const BT::NodeConfig& config)
      : BT::ConditionNode(name, config) {}

  static BT::PortsList providedPorts() { return {}; }

  BT::NodeStatus tick() override {
    auto running
        = G_BT_MANAGER()->get_rt().system_running_state() && G_BT_MANAGER()->is_conn_codeit();
    return running ? BT::NodeStatus::SUCCESS : BT::NodeStatus::FAILURE;
  }
};