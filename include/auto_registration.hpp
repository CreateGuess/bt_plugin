#pragma once

#include <behaviortree_cpp/bt_factory.h>

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// 宏工具
#define CONCAT_IMPL(x, y) x##y
#define CONCAT(x, y) CONCAT_IMPL(x, y)

#define REGISTER_NODE(NodeType, NodeName)                                           \
  namespace {                                                                       \
    const NodeRegistrar CONCAT(_registrar_, __COUNTER__)(                           \
        NodeName,                                                                   \
        [](BT::BehaviorTreeFactory& factory) {                                      \
          factory.registerBuilder<NodeType>(                                        \
              NodeName, [](const std::string& name, const BT::NodeConfig& config) { \
                return std::make_unique<NodeType>(name, config);                    \
              });                                                                   \
        },                                                                          \
        convertPorts(NodeType::providedPorts()), deduceNodeType<NodeType>());       \
  }

// 注册函数类型
using RegisterFunction = std::function<void(BT::BehaviorTreeFactory&)>;

// 元信息结构
struct PortMeta {
  std::string name;
  std::string type;
  std::string default_value;
  std::string direction;
  std::string description;
};

struct NodeMeta {
  std::string name;
  std::string type;
  std::vector<PortMeta> ports;
};

template <typename T> std::string deduceNodeType() {
  if constexpr (std::is_base_of_v<BT::ConditionNode, T>) {
    return "Condition";
  } else if constexpr (std::is_base_of_v<BT::DecoratorNode, T>) {
    return "Decorator";
  } else if constexpr (std::is_base_of_v<BT::ControlNode, T>) {
    return "Control";
  } else {
    return "Action";
  }
}

inline std::string anyToString(const BT::Any& any) {
  if (any.empty()) return "";

  try {
    return BT::toStr(any);
  } catch (...) {
    return "<non-stringable>";
  }
}

inline std::string normalizeDefault(std::string str) {
  if (str.empty()) return str;
  if (str.rfind("json:", 0) == 0) {
    str = str.substr(5);
  }

  if (str.size() >= 2 && str.front() == '"' && str.back() == '"') {
    str = str.substr(1, str.size() - 2);
  }

  return str;
}
inline std::vector<PortMeta> convertPorts(const BT::PortsList& ports) {
  std::vector<PortMeta> out;
  out.reserve(ports.size());

  for (const auto& [name, info] : ports) {
    std::string dir;
    switch (info.direction()) {
      case BT::PortDirection::INPUT:
        dir = "Input";
        break;
      case BT::PortDirection::OUTPUT:
        dir = "Output";
        break;
      case BT::PortDirection::INOUT:
        dir = "InOut";
        break;
    }

    std::string raw_def = anyToString(info.defaultValue());
    std::string def = normalizeDefault(raw_def);
    std::string desc = info.description();

    out.push_back({name, info.typeName(), def, dir, desc});
  }

  return out;
}

// Ports 转换（核心）
#if 0
inline std::vector<PortMeta> convertPorts(const BT::PortsList& ports) {
  std::vector<PortMeta> out;
  out.reserve(ports.size());

  for (const auto& [name, info] : ports) {
    std::string dir;
    switch (info.direction()) {
      case BT::PortDirection::INPUT:
        dir = "Input";
        break;
      case BT::PortDirection::OUTPUT:
        dir = "Output";
        break;
      case BT::PortDirection::INOUT:
        dir = "InOut";
        break;
    }

    std::string def = anyToString(info.defaultValue());

    std::string desc = info.description();

    out.push_back({name, info.typeName(), def, dir, desc});
  }

  return out;
}
#endif

// 注册中心
class NodeRegistry {
public:
  static NodeRegistry& instance() {
    static NodeRegistry inst;
    return inst;
  }

  // 注册（带 meta）
  void registerNode(const std::string& name, RegisterFunction func, std::vector<PortMeta> ports,
                    std::string type) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (registry_.count(name)) {
      std::cout << "Duplicate node registration: " << name << std::endl;
      return;
    }

    registry_[name] = std::move(func);
    meta_[name] = NodeMeta{name, std::move(type), std::move(ports)};
  }

  // 注册所有节点到 factory
  void registerAll(BT::BehaviorTreeFactory& factory) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [name, func] : registry_) {
      func(factory);
    }
  }

  // ⭐ 获取所有节点信息（用于上报）
  std::vector<NodeMeta> getAllMeta() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<NodeMeta> out;
    out.reserve(meta_.size());

    for (const auto& [_, m] : meta_) {
      out.push_back(m);
    }

    return out;
  }

private:
  std::unordered_map<std::string, RegisterFunction> registry_;
  std::unordered_map<std::string, NodeMeta> meta_;

  mutable std::mutex mutex_;
};

// 静态注册器
struct NodeRegistrar {
  NodeRegistrar(const std::string& name, RegisterFunction func, std::vector<PortMeta> ports,
                std::string type) {
    NodeRegistry::instance().registerNode(name, std::move(func), std::move(ports), std::move(type));
  }
};
