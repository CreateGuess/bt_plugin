#pragma once

#include <behaviortree_cpp/action_node.h>
#include <behaviortree_cpp/bt_factory.h>
#include <behaviortree_cpp/loggers/bt_observer.h>
#include <behaviortree_cpp/loggers/groot2_publisher.h>
#include <behaviortree_cpp/tree_node.h>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "overall_system_nrtstate.pb.h"
#include "overall_system_rtstate.pb.h"
#include "plugin_core/sdk/plugin_logx.hpp"
#include "plugin_core/sdk/plugin_sdk.hpp"
#include "singleton_dclp.hpp"
#include <unordered_map>

namespace bt_manager_cmd {
  using ParamValue = nlohmann::json;
  struct CmdField {
    using FieldMap = std::unordered_map<std::string, ParamValue>;
    using CommandFieldMap = std::unordered_map<std::string, FieldMap>;

    bool selected{false};
    FieldMap default_fields;
    CommandFieldMap command_overrides;

    std::optional<ParamValue> get_field(const std::string& key) const {
      if (key == "selected") return selected;

      auto it = default_fields.find(key);
      if (it != default_fields.end()) return it->second;

      return std::nullopt;
    }

    std::optional<ParamValue> get_field(const std::string& command_uuid,
                                        const std::string& key) const {
      auto cmd_it = command_overrides.find(command_uuid);
      if (cmd_it != command_overrides.end()) {
        auto field_it = cmd_it->second.find(key);
        if (field_it != cmd_it->second.end()) return field_it->second;
      }
      return get_field(key);
    }
  };

  inline void set_dynamic_field(std::unordered_map<std::string, ParamValue>& fields,
                                const std::string& key, const nlohmann::json& val) {
    if (val.is_string()) {
      fields[key] = val.get<std::string>();
    } else if (val.is_number_integer()) {
      fields[key] = val.get<int>();
    } else if (val.is_number_float()) {
      fields[key] = val.get<double>();
    } else if (val.is_boolean()) {
      fields[key] = val.get<bool>();
    } else if (val.is_null()) {
      fields[key] = nullptr;
    } else if (val.is_array()) {
      fields[key] = val;
    } else if (val.is_object()) {
      fields[key] = val;
    }
  }

  inline void from_json(const nlohmann::json& j, CmdField& data) {
    if (j.contains("selected") && j["selected"].is_boolean()) {
      data.selected = j["selected"].get<bool>();
    } else {
      data.selected = false;
    }

    for (auto it = j.begin(); it != j.end(); ++it) {
      const std::string& key = it.key();
      if (key == "selected") continue;
      if (key == "commands" && it.value().is_object()) {
        for (const auto& [command_uuid, command_overrides] : it.value().items()) {
          if (!command_overrides.is_object()) continue;
          auto& scoped_fields = data.command_overrides[command_uuid];
          for (const auto& [field_key, field_value] : command_overrides.items()) {
            set_dynamic_field(scoped_fields, field_key, field_value);
          }
        }
        continue;
      }

      const auto& val = it.value();
      set_dynamic_field(data.default_fields, key, val);
    }
  }

  inline void to_json(nlohmann::json& j, const CmdField& data) {
    j = nlohmann::json{};
    j["selected"] = data.selected;
    for (const auto& [key, val] : data.default_fields) {
      if (key == "selected" || key == "commands") continue;
      j[key] = val;
    }
    if (!data.command_overrides.empty()) {
      j["commands"] = nlohmann::json::object();
      for (const auto& [command_uuid, fields] : data.command_overrides) {
        j["commands"][command_uuid] = nlohmann::json::object();
        for (const auto& [key, val] : fields) {
          j["commands"][command_uuid][key] = val;
        }
      }
    }
  }

  enum class UserAction : int {
    Down = 0,
    Up = 1,
    Cancel = 2,

    Click = 10,
    DoubleClick = 11,

    LongPressStart = 20,
    LongPressing = 21,
    LongPressEnd = 22,

    Repeat = 30
  };

  NLOHMANN_JSON_SERIALIZE_ENUM(UserAction, {{UserAction::Down, "Down"},
                                            {UserAction::Up, "Up"},
                                            {UserAction::Cancel, "Cancel"},
                                            {UserAction::Click, "Click"},
                                            {UserAction::DoubleClick, "DoubleClick"},
                                            {UserAction::LongPressStart, "LongPressStart"},
                                            {UserAction::LongPressing, "LongPressing"},
                                            {UserAction::LongPressEnd, "LongPressEnd"},
                                            {UserAction::Repeat, "Repeat"}})

  inline const char* to_string(UserAction action) {
    switch (action) {
      case UserAction::Down:
        return "Down";
      case UserAction::Up:
        return "Up";
      case UserAction::Cancel:
        return "Cancel";
      case UserAction::Click:
        return "Click";
      case UserAction::DoubleClick:
        return "DoubleClick";
      case UserAction::LongPressStart:
        return "LongPressStart";
      case UserAction::LongPressing:
        return "LongPressing";
      case UserAction::LongPressEnd:
        return "LongPressEnd";
      case UserAction::Repeat:
        return "Repeat";
      default:
        return "Unknown";
    }
  }

}  // namespace bt_manager_cmd

template <> struct fmt::formatter<bt_manager_cmd::UserAction> {
  constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(const bt_manager_cmd::UserAction& action, FormatContext& ctx) const {
    return fmt::format_to(ctx.out(), "{}", to_string(action));
  }
};

namespace bt_manager_cmd {
  struct CmdDynamic {
    std::string event_name;
    UserAction user_action{UserAction::Click};
    std::vector<CmdField> sub_commands;
  };
  inline void from_json(const nlohmann::json& j, CmdDynamic& cmd) {
    if (j.contains("event_name") && j["event_name"].is_string())
      j.at("event_name").get_to(cmd.event_name);
    if (j.contains("user_action") && j["user_action"].is_string())
      j.at("user_action").get_to(cmd.user_action);

    if (j.contains("sub_commands") && j["sub_commands"].is_array()) {
      cmd.sub_commands.clear();
      for (const auto& sub_j : j["sub_commands"]) {
        CmdField sub_data;
        from_json(sub_j, sub_data);
        cmd.sub_commands.push_back(std::move(sub_data));
      }
    }
  }
  inline void to_json(nlohmann::json& j, const CmdDynamic& cmd) {
    j = nlohmann::json{};
    j["event_name"] = cmd.event_name;
    j["user_action"] = cmd.user_action;

    j["sub_commands"] = nlohmann::json::array();
    for (const auto& sub : cmd.sub_commands) {
      j["sub_commands"].push_back(sub);
    }
  }

  // 动态指令生成器
  class CmdDynBuilder final {
  public:
    CmdDynamic cmd;

    CmdDynBuilder() = default;
    CmdDynBuilder(const CmdDynBuilder&) = delete;
    CmdDynBuilder& operator=(const CmdDynBuilder&) = delete;
    CmdDynBuilder(CmdDynBuilder&&) = default;
    CmdDynBuilder& operator=(CmdDynBuilder&&) = default;

    CmdDynBuilder& init_models(size_t model_size) {
      cmd.sub_commands.clear();
      cmd.sub_commands.resize(model_size);
      return *this;
    }

    CmdDynBuilder& set_event(std::string name) {
      cmd.event_name = std::move(name);
      return *this;
    }

    CmdDynBuilder& set_action(UserAction action) {
      cmd.user_action = action;
      return *this;
    }

    CmdDynBuilder& add_field(std::unordered_map<std::string, ParamValue> f) {
      bool any_selected = false;
      for (auto& sc : cmd.sub_commands) {
        if (sc.selected) {
          any_selected = true;
          for (auto& [k, v] : f) sc.default_fields[k] = v;
        }
      }
      if (!any_selected) LOG_WARN("add_field called but no model selected");

      return *this;
    }

    CmdDynBuilder& select_models(std::initializer_list<size_t> indices) {
      for (auto& sc : cmd.sub_commands) sc.selected = false;

      for (auto idx : indices) {
        if (idx < cmd.sub_commands.size()) {
          cmd.sub_commands[idx].selected = true;
        } else {
          LOG_ERROR("sel index {} out of range {}", idx, cmd.sub_commands.size());
        }
      }
      return *this;
    }

    CmdDynBuilder& select_models(const std::vector<int>& indices) {
      for (auto& sc : cmd.sub_commands) sc.selected = false;

      for (auto idx : indices) {
        if (idx < cmd.sub_commands.size()) {
          cmd.sub_commands[idx].selected = true;
        } else {
          LOG_ERROR("sel index {} out of range {}", idx, cmd.sub_commands.size());
        }
      }
      return *this;
    }

    CmdDynBuilder& select_all() {
      for (auto& sc : cmd.sub_commands) sc.selected = true;
      return *this;
    }

    CmdDynamic build() { return cmd; }
  };

}  // namespace bt_manager_cmd

// 状态字符串
inline const char* toStr(BT::NodeStatus s) noexcept {
  switch (s) {
    case BT::NodeStatus::IDLE:
      return "IDLE";
    case BT::NodeStatus::RUNNING:
      return "RUNNING";
    case BT::NodeStatus::SUCCESS:
      return "SUCCESS";
    case BT::NodeStatus::FAILURE:
      return "FAILURE";
    default:
      return "?";
  }
}

// Observer
class StatusObserver final : public BT::StatusChangeLogger {
public:
  using Callback = std::function<void(const BT::TreeNode&, BT::NodeStatus, BT::NodeStatus)>;

  StatusObserver(const BT::Tree& tree, Callback cb)
      : StatusChangeLogger(tree.rootNode()), cb_(std::move(cb)) {}

private:
  void callback(BT::Duration, const BT::TreeNode& node, BT::NodeStatus prev,
                BT::NodeStatus curr) override {
    if (cb_) cb_(node, prev, curr);
  }

  void flush() override {}

  Callback cb_;
};

// BTManager
class BTManager final : public SingletonDclp<BTManager> {
  int64_t ts_now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
  }
  int64_t ts_now_sec() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
  }

  enum class State { IDLE, RUNNING, PAUSED, SWITCHING };

public:
  explicit BTManager(IPubSub* pubsub, IReqRep* reqrep) : pubsub_(pubsub), reqrep_(reqrep) {};
  ~BTManager() = default;

  bool init();

  bool loadTree(const std::string& xml);

  void start(int = 50);
  void pause();
  void resume();
  BT::NodeStatus step();
  void stop();
  void beginShutdown() { stopping_.store(true, std::memory_order_release); }
  bool is_stopping() const { return stopping_.load(std::memory_order_acquire); }

  std::string dump_nodes_xml();

  BT::Blackboard::Ptr blackboard() { return blackboard_; }

  IPubSub* get_pubsub() { return pubsub_; };
  IReqRep* get_reqrep() { return reqrep_; };

  bool is_running() const;

  void set_conn_codeit(bool value) {
    LOG_INFO("[{}] 驱动连接: {}", TAG, value);
    conn_codeit_.store(value, std::memory_order_release);
  }
  bool is_conn_codeit() const { return conn_codeit_.load(std::memory_order_acquire); }

  void set_model_size(const int&);
  int get_model_size() const;

  void set_gateway_status(const int&);
  int get_gateway_status() const;

  void set_idle_stable_count(int v);
  int inc_idle_stable_count();

  bool update_rt(const void* data, size_t size);
  bool update_nrt(const void* data, size_t size);
  overall_system_rtstate::SystemRtState get_rt() const;
  overall_system_nrtstate::SystemNrtState get_nrt() const;

private:
  void loop();

  void switchTree();

  void onNodeStatusChanged(const BT::TreeNode& node, BT::NodeStatus prev, BT::NodeStatus curr);

  void onTreeReset();

  void registerNodes(BT::BehaviorTreeFactory& f);

  void reset();

private:
  const std::string TAG = "行为树管理器";

  IPubSub* pubsub_{nullptr};
  IReqRep* reqrep_{nullptr};

  std::unique_ptr<BT::BehaviorTreeFactory> factory_;
  BT::Tree tree_;

  std::unique_ptr<BT::Groot2Publisher> publisher_;
  std::unique_ptr<StatusObserver> observer_;
  BT::Blackboard::Ptr blackboard_;

  std::thread worker_;

  std::atomic_bool conn_codeit_{false};  // 是否连接驱动
  mutable std::mutex state_mtx_;
  overall_system_rtstate::SystemRtState rt_msg{};     // RT数据
  overall_system_nrtstate::SystemNrtState nrt_msg{};  // NRT数据
  std::atomic_int model_size{0};                      // 模型数量
  std::atomic_int gateway_status{-1};                 // rpc网关状态
  std::atomic_int idle_stable_count{0};               // 空闲去抖计数

  std::atomic_bool running_{false};
  std::atomic_bool paused_{true};
  std::atomic_bool stopping_{false};

  mutable std::mutex lifecycle_mtx_;
  mutable std::mutex tree_mtx_;

  std::condition_variable cv_;
  mutable std::mutex cv_mtx_;

  std::chrono::milliseconds tick_period_{50};

  std::string pending_xml_;
  std::atomic_bool has_pending_{false};
};

inline auto G_BT_MANAGER() { return BTManager::GetInstance(); }
