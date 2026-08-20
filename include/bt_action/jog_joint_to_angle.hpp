#pragma once

#include <behaviortree_cpp/action_node.h>

#include <chrono>
#include <cmath>
#include <vector>

#include "bt_manager.hpp"

class JogJointToAngle final : public BT::StatefulActionNode {
public:
  JogJointToAngle(const std::string& name, const BT::NodeConfig& config)
      : BT::StatefulActionNode(name, config) {}

  static BT::PortsList providedPorts() {
    return {
        BT::InputPort<int>("robot_sel", 0, "Robot selection"),
        BT::InputPort<int>("increase_count", 50, "Jog step count"),
        BT::InputPort<int>("motion_id", 0, "Joint index / motion id"),
        BT::InputPort<double>("target", 0.0, "Target joint angle, deg"),
        BT::InputPort<double>("tolerance", 0.5, "Position tolerance, deg"),
    };
  }

  BT::NodeStatus onStart() override {
    start_ = std::chrono::steady_clock::now();
    pressing_ = false;
    active_robot_ = -1;
    active_direction_ = 0;
    last_pressing_ = {};
    return tickImpl();
  }

  BT::NodeStatus onRunning() override { return tickImpl(); }

  void onHalted() override { stopPressing(); }

private:
  BT::NodeStatus tickImpl() {
    if (G_BT_MANAGER()->is_stopping() || !G_BT_MANAGER()->is_conn_codeit()) {
      LOG_ERROR("[JogJointToAngle] codeit not connected");
      return BT::NodeStatus::FAILURE;
    }

    int robot_sel = 0;
    int motion_id = 0;
    double target = 0.0;
    double tolerance = 0.01;
    int increase_count = 1;
    int pressing_interval_ms = 1000;

    if (!getInput("robot_sel", robot_sel)) {
      LOG_ERROR("[JogJointToAngle] missing robot_sel");
      stopPressing();
      return BT::NodeStatus::FAILURE;
    }
    getInput("motion_id", motion_id);
    getInput("target", target);
    getInput("tolerance", tolerance);
    getInput("increase_count", increase_count);

    last_motion_id_ = motion_id;
    last_increase_count_ = increase_count;

    if (robot_sel != 0 && robot_sel != 1) {
      LOG_ERROR("[JogJointToAngle] invalid robot_sel: {}, expected 0 or 1", robot_sel);
      stopPressing();
      return BT::NodeStatus::FAILURE;
    }
    if (motion_id < 0) {
      LOG_ERROR("[JogJointToAngle] invalid motion_id: {}", motion_id);
      stopPressing();
      return BT::NodeStatus::FAILURE;
    }
    if (tolerance < 0.0) {
      LOG_ERROR("[JogJointToAngle] invalid tolerance: {}", tolerance);
      stopPressing();
      return BT::NodeStatus::FAILURE;
    }
    if (increase_count <= 0) {
      LOG_ERROR("[JogJointToAngle] invalid increase_count: {}", increase_count);
      stopPressing();
      return BT::NodeStatus::FAILURE;
    }
    if (pressing_interval_ms <= 0) {
      LOG_ERROR("[JogJointToAngle] invalid pressing_interval_ms: {}", pressing_interval_ms);
      stopPressing();
      return BT::NodeStatus::FAILURE;
    }

    constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
    target *= kDegToRad;
    tolerance *= kDegToRad;

    const auto now = std::chrono::steady_clock::now();
    if (now - start_ >= std::chrono::milliseconds(60000)) {
      LOG_ERROR("[JogJointToAngle] timeout, target={}", target);
      stopPressing();
      return BT::NodeStatus::FAILURE;
    }

    const auto rt_msg = G_BT_MANAGER()->get_rt();
    if (!rt_msg.system_running_state()) {
      LOG_ERROR("[JogJointToAngle] system not running");
      stopPressing();
      return BT::NodeStatus::FAILURE;
    }

    if (robot_sel >= rt_msg.model_size()) {
      LOG_ERROR("[JogJointToAngle] invalid model index: {}", robot_sel);
      stopPressing();
      return BT::NodeStatus::FAILURE;
    }

    const auto& model = rt_msg.model(robot_sel);
    if (motion_id >= model.joint_size()) {
      LOG_ERROR("[JogJointToAngle] invalid motion_id={}, joint_size={}", motion_id,
                model.joint_size());
      stopPressing();
      return BT::NodeStatus::FAILURE;
    }

    const auto& joint = model.joint(motion_id);
    if (!joint.is_enabled()) {
      LOG_ERROR("[JogJointToAngle] model={}, motion_id={} not enabled", robot_sel, motion_id);
      stopPressing();
      return BT::NodeStatus::FAILURE;
    }

    const double error = target - joint.position();
    if (std::abs(error) <= tolerance) {
      stopPressing();
      return BT::NodeStatus::SUCCESS;
    }

    const int direction = error > 0.0 ? 1 : -1;
    if (!pressing_ || robot_sel != active_robot_ || direction != active_direction_) {
      stopPressing();

      if (!sendJog(robot_sel, direction, increase_count, motion_id,
                   bt_manager_cmd::UserAction::LongPressStart)) {
        stopPressing();
        return BT::NodeStatus::FAILURE;
      }

      active_robot_ = robot_sel;
      active_direction_ = direction;
      pressing_ = true;
      last_pressing_ = now;
      return BT::NodeStatus::RUNNING;
    }

    if (now - last_pressing_ >= std::chrono::milliseconds(pressing_interval_ms)) {
      if (!sendJog(active_robot_, active_direction_, increase_count, motion_id,
                   bt_manager_cmd::UserAction::LongPressing)) {
        stopPressing();
        return BT::NodeStatus::FAILURE;
      }

      last_pressing_ = now;
    }

    return BT::NodeStatus::RUNNING;
  }

  bool sendJog(int robot_sel, int direction, int increase_count, int motion_id,
               bt_manager_cmd::UserAction action) {
    using namespace std::chrono_literals;
    if (G_BT_MANAGER()->is_stopping()) return false;
    if (robot_sel < 0 || direction == 0) return true;

    const int model_count = G_BT_MANAGER()->get_model_size() > 0
                                ? G_BT_MANAGER()->get_model_size()
                                : G_BT_MANAGER()->get_rt().model_size();

    nlohmann::json j = bt_manager_cmd::CmdDynBuilder{}
                           .init_models(static_cast<size_t>(model_count))
                           .select_models(std::vector<int>{robot_sel})
                           .set_event("JogJoint")
                           .set_action(action)
                           .add_field({{"increase_count", increase_count}})
                           .add_field({{"motion_id", motion_id}})
                           .add_field({{"direction", direction}})
                           .build();

    auto ret
        = ReqRepJson::call_sync(G_BT_MANAGER()->get_reqrep(), "main.handler.req.dyncmd", j, 5000ms);
    if (!ret) {
      LOG_ERROR(
          "[JogJointToAngle] jog request failed, action={}, direction={}, req_status={}, code={}, "
          "message={}",
          action, direction, static_cast<int>(ret.req_status), ret.code, ret.message);
      return false;
    }

    return true;
  }

  void stopPressing() {
    if (!pressing_) return;

    if (!G_BT_MANAGER()->is_stopping()) {
      sendJog(active_robot_, active_direction_, last_increase_count_, last_motion_id_,
              bt_manager_cmd::UserAction::LongPressEnd);
    }

    pressing_ = false;
    active_robot_ = -1;
    active_direction_ = 0;
    last_pressing_ = {};
  }

private:
  std::chrono::steady_clock::time_point start_{};
  std::chrono::steady_clock::time_point last_pressing_{};
  bool pressing_{false};
  int last_motion_id_{0};
  int last_increase_count_{1};
  int active_robot_{-1};
  int active_direction_{0};
};
