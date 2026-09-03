#include <behaviortree_cpp/action_node.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

#include "bt_json_request.hpp"
#include "bt_manager.hpp"
#include "pb_update_data.hpp"

class MobileRobotModbusTCPJGBOT final : public BT::StatefulActionNode {
public:
  /**
   * @brief 构造函数
   * @param name 节点名称
   * @param config 节点配置
   */
  MobileRobotModbusTCPJGBOT(const std::string& name, const BT::NodeConfig& config)
      : BT::StatefulActionNode(name, config) {
    models_ = G_BT_MANAGER()->get_model_size();
  }

  /**
   * @brief 析构函数，取消未完成的请求
   * @note 如果行为树节点正在运行，析构时会取消请求
   */
  ~MobileRobotModbusTCPJGBOT() override { bt_req_.cancel(); }

  /**
   * @brief 提供行为树节点的输入端口列表
   * @return 输入端口列表
   */
  static BT::PortsList providedPorts() {
    return {
        BT::InputPort<std::vector<int>>("robot_sel", "0;1", "Robot selection"),
        BT::InputPort<std::string>("target_station_id", "0", "Target station ID"),
    };
  }

  /**
   * @brief onStart 在行为树节点开始执行时调用，发送异步命令
   * @return 返回节点状态，RUNNING 表示命令正在执行，FAILURE 表示命令发送失败
   * @note 如果行为树管理器正在停止或未连接到 Codeit，返回 FAILURE
   * @note 如果缺少 robot_sel 输入端口，返回 FAILURE
   * @note 如果缺少 target_station_id 输入端口，返回 FAILURE
   */
  BT::NodeStatus onStart() override {
    using namespace std::chrono_literals;
    // 1. 检查行为树管理器状态
    if (!G_BT_MANAGER()->is_conn_codeit()) return BT::NodeStatus::FAILURE;
    // 2. 查看子系统是否添加
    auto nrt = G_BT_MANAGER()->get_nrt();
    for (int i = 0; i < nrt.subsystem_size(); i++) {
      const auto& sub = nrt.subsystem(i);
      if (sub.subsystem_name().find("MobileRobotModbusTCPJGBOT") != std::string::npos) {
        break;
      }
      if (i == nrt.subsystem_size() - 1) {
        LOG_ERROR("[MobileRobotModbusTCPJGBOT] subsystem not found");
        return BT::NodeStatus::FAILURE;
      }
    }
    // 3. 获取 robot_sel 输入端口
    std::vector<int> robot_sel;
    if (!getInput("robot_sel", robot_sel)) {
      LOG_ERROR("[MobileRobotModbusTCPJGBOT] missing robot_sel");
      return BT::NodeStatus::FAILURE;
    }
    // 4. 获取 target_station_id 输入端口
    std::string target_station_id;
    getInput("target_station_id", target_station_id);

    // 5. 构建 JSON 命令
    nlohmann::json j = bt_manager_cmd::CmdDynBuilder{}
                           .init_models(models_)
                           .select_models(robot_sel)
                           .set_event("MobileRobotModbusTCPJGBOT")
                           .add_field({{"navigate_open", 1}})
                           .add_field({{"target_station_id", target_station_id}})
                           .build();

    // 6. 发送异步命令
    if (!bt_req_.start(G_BT_MANAGER()->get_reqrep(), "main.handler.req.dyncmd", j, 10s)) {
      LOG_ERROR("[MobileRobotModbusTCPJGBOT] request start failed: {}", bt_req_.message());
      return BT::NodeStatus::FAILURE;
    }

    return BT::NodeStatus::RUNNING;
  }

  /**
   * @brief onRunning 在行为树节点运行时调用，检查命令执行状态
   * @return 返回节点状态，RUNNING 表示命令仍在执行，SUCCESS
   * @note 如果行为树管理器正在停止或未连接到 Codeit，返回 FAILURE
   */
  BT::NodeStatus onRunning() override {
    // 1. 检查行为树管理器状态，如果正在停止或未连接到 Codeit，取消命令并返回 FAILURE
    if (G_BT_MANAGER()->is_stopping() || !G_BT_MANAGER()->is_conn_codeit()) {
      bt_req_.cancel();
      return BT::NodeStatus::FAILURE;
    }
    // 2. 获取行为树管理器的非实时数据
    auto nrt = G_BT_MANAGER()->get_nrt();
    // 3. 解析出当前的 MobileRobotModbusTCPJGBOT 子系统状态
    pb_update_data::MobileRobotModbusTCPJGBOTData bot_data{};
    for (int i = 0; i < nrt.subsystem_size(); i++) {
      const auto& sub = nrt.subsystem(i);
      if (sub.subsystem_name().find("MobileRobotModbusTCPJGBOT") != std::string::npos) {
        if (sub.data().size() >= sizeof(bot_data)) {
          std::memcpy(&bot_data, sub.data().data(), sizeof(bot_data));
        }
        break;
      }
    }
    // 4. 检查导航状态，如果到达目标站点，取消命令并返回 SUCCESS
    std::string target_station_id;
    getInput("target_station_id", target_station_id);
    if (bot_data.nav_state == pb_update_data::NavState::ARRIVED
        && bot_data.current_navigate_station_id == std::stoi(target_station_id)) {
      bt_req_.cancel();
      return BT::NodeStatus::SUCCESS;
    }

    return bt_req_.tick("MobileRobotModbusTCPJGBOT");
  }

  void onHalted() override { bt_req_.cancel(); }

private:
  int models_{1};

  BtJsonRequest bt_req_;
};
