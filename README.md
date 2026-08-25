# BehaviorTreePlugin

本项目构建一个集成行为树监控功能的 `BehaviorTreePlugin` 动态插件。

顶层 `CMakeLists.txt` 通过 `add_subdirectory(monitor)` 引入监控子目录；
`monitor/CMakeLists.txt` 使用 `target_sources` 将监控源码加入已有的
`BehaviorTreePlugin` 目标。监控功能不会生成第二个动态库，最终只需部署
一个 `libBehaviorTreePlugin.so`。

## 依赖安装

### 依赖结论

监控代码直接接收 `BTManager` 中 `StatusObserver` 产生的节点
状态回调，不再轮询 `Groot2Publisher` 的 1667 端口。监控组件
本身没有新增必须安装的外部系统库。事件消费任务通过
`PluginBase::start_managed_worker()` 交给 xplugin Host 托管，monitor
不创建 `std::thread`，也不单独链接 `Threads::Threads`。

主行为树插件仍然创建 `Groot2Publisher` 供 Groot2 调试工具使用，
因此完整编译该项目时仍然需要 **ZeroMQ (`libzmq`)**；它不再是
monitor 的直接依赖。

Ubuntu/Debian 构建环境可安装：

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
  libzmq3-dev
```

包用途：

| 软件包 | 是否必需 | 用途 |
| --- | --- | --- |
| `build-essential` | 是 | 提供 GCC/G++、标准 C/C++ 头文件和基本构建工具。 |
| `cmake` | 是 | 配置主插件及 `monitor` 子目录。项目要求 CMake 3.14 或更高版本。 |
| `libzmq3-dev` | 完整插件必需 | 供 BehaviorTree.CPP 的 `Groot2Publisher` 使用；monitor 不再直接调用它。 |

### 由部署环境提供的主插件依赖

以下两项不是 monitor 新增的依赖，但构建完整
`BehaviorTreePlugin` 时必须已安装，并且能被 CMake 找到：

| CMake 包/目标 | 用途 |
| --- | --- |
| `xplugin-dev` / `xplugin::sdk` | 提供插件生命周期、日志、HTTP 端点和 WebSocket 端点 API。 |
| `gateway_proto` / `gateway_proto::gateway_proto` | 提供行为树插件使用的系统状态 Protobuf 消息。 |

如果它们安装在非标准目录，通过 `CMAKE_PREFIX_PATH` 指定 CMake
包路径：

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/path/to/xplugin;/path/to/gateway_proto"
```

### 项目已内置，无需单独安装

| 依赖 | 来源 | 用途 |
| --- | --- | --- |
| BehaviorTree.CPP 4.9.0 | `3rd/BehaviorTree.CPP-4.9.0` | 行为树执行以及 `Groot2Publisher`。 |
| cppzmq | BehaviorTree.CPP 的 `3rdparty/cppzmq` | 供主插件中的 `Groot2Publisher` 使用，monitor 不直接使用。 |
| tinyxml2 | BehaviorTree.CPP 的 `3rdparty/tinyxml2` | 解析行为树 XML 和 `bt_nodes.xml`。 |
| fmt 11.2.0 | `3rd/fmt-11.2.0` | 格式化监控日志。 |
| nlohmann/json | 项目头文件/现有依赖 | 生成 HTTP 响应和 WebSocket JSON 消息。 |

不需要额外安装系统 `cppzmq-dev`、`libtinyxml2-dev`、`libfmt-dev`
或 `nlohmann-json3-dev`，否则可能与项目中固定的版本混用。

### 依赖检查

构建前可以检查 ZeroMQ 头文件和动态库：

```bash
test -f /usr/include/zmq.h && echo "zmq.h: found"
ldconfig -p | grep libzmq
```

CMake 配置成功时应出现类似信息：

```text
Found Threads: TRUE
Found ZeroMQ: /usr/lib/x86_64-linux-gnu/libzmq.so
```

构建完成后可检查最终插件是否存在缺失的运行时库：

```bash
ldd /path/to/plugins/libBehaviorTreePlugin.so | grep "not found"
```

该命令没有输出才表示所有动态库都能正常解析。

## 构建

在项目根目录执行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

构建完成后会生成包含主行为树功能和监控功能的：

```text
build/libBehaviorTreePlugin.so
```

默认输出目录是 CMake 构建目录。可以在配置时通过
`RPC_GATEWAY_PLUGIN_OUTPUT_DIR` 指定 rpc_gateway 的插件目录：

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DRPC_GATEWAY_PLUGIN_OUTPUT_DIR=/home/codeit/Desktop/rpc_gateway/current_version/plugins

cmake --build build --parallel
```

重新编译和替换 `.so` 后，必须完整重启 rpc_gateway，已加载的动态库
不会因为文件被覆盖而自动刷新。

## 部署结构

推荐部署结构：

```text
current_version/
├── bin/
│   └── rpc_gateway
├── bt_trees/
│   ├── bt_nodes.xml
│   └── move_test.xml
├── plugins/
│   └── libBehaviorTreePlugin.so
└── static/
```

当前行为树主插件及 monitor 均使用 `../bt_trees` 相对路径。因此
rpc_gateway 应当从 `current_version/bin` 作为工作目录启动。

## 监控功能

监控功能作为 `BehaviorTreePlugin` 的内部组件启动，它不使用第二个
`PLUGIN_DECLARE`，也不需要单独部署监控动态库。

监控后台采用进程内事件驱动模式：`StatusObserver` 检测到节点
状态变化后，`BTManager` 只把事件写入线程安全队列；Host
托管的 worker 被条件变量唤醒，更新快照并通过 WebSocket 发送。
无状态变化时 worker 休眠，不会定时访问 1667。

监控实现位于：

```text
monitor/include/bt_monitor_plugin.hpp
```

### HTTP 接口

监控 HTTP 端点名称是 `behavior_tree_monitor`：

```text
POST /backend/plugin-http/behavior_tree_monitor/tree.snapshot
POST /backend/plugin-http/behavior_tree_monitor/status.snapshot
POST /backend/plugin-http/behavior_tree_monitor/bridge.health
POST /backend/plugin-http/behavior_tree_monitor/load.node
```

所有监控 HTTP 接口使用统一响应格式：

```json
{
  "data": {},
  "request_code": 0,
  "request_message": "ok",
  "request_source": "plugin"
}
```

失败时 `request_code` 为 `-1`，`request_message` 保存具体错误原因。

#### `tree.snapshot`

获取当前已加载行为树的结构快照。只有当主插件成功创建行为树，
并向 monitor 投递树重置事件后，该接口才有数据。

未就绪时返回：

```json
{
  "request_code": -1,
  "request_message": "behavior tree is not available yet",
  "request_source": "plugin"
}
```

#### `status.snapshot`

获取当前行为树节点状态快照，包括节点 UID、状态和时间戳。

#### `bridge.health`

获取监控桥健康状态，包括：

- Host 托管的监控 worker 是否运行；
- 监控模式（`event_driven`）和事件源状态；
- 待处理、已接收、已合并和已处理事件数；
- 最后一次成功处理事件的时间；
- 最近错误信息；
- WebSocket 观看者数量和发送失败次数。

`event_source_connected=true` 表示监控 worker 已运行且 BTManager 回调
已注册。行为树成功加载并投递树快照后，`last_success_ms`
应大于 `0`。

为了兼容已部署的前端，响应仍保留 `backend_connected`、`backend`
和 `poll_interval_ms` 字段。事件驱动模式下 `backend.port` 和
`poll_interval_ms` 均为 `0`，不表示端口或轮询间隔。

#### `load.node`

读取：

```text
../bt_trees/bt_nodes.xml
```

成功时将完整 XML 放在 `data.content` 中，并在 `data.node_count` 中返回
节点模型数量。

### WebSocket 接口

监控 WebSocket 地址：

```text
WS /backend/plugin-ws/behavior_tree_monitor
```

建立连接后，服务端会发送当前行为树快照，并在节点状态变化时持续
推送状态消息。

客户端可发送心跳请求：

```json
{
  "action": "ping"
}
```

服务端响应：

```json
{
  "type": "pong",
  "timestamp_ms": 0
}
```

WebSocket 消息是实时事件，不使用 HTTP 的
`request_code/request_message/request_source` 响应包装。

## 行为树加载与监控流程

1. 调用主插件 `/behavior_tree/read` 获取 `bt_trees` 下的行为树文件列表。
2. 调用 `/behavior_tree/load`，传入要加载的行为树 XML 文件名。
3. 调用 `/behavior_tree/start` 启动行为树工作线程。
4. `BTManager` 创建行为树和 `StatusObserver`，并生成带 UID 的树 XML。
5. `BTManager` 将树重置事件投递给 monitor，monitor 缓存完整树。
6. 节点状态变化时，`StatusObserver` 立即投递增量状态事件。
7. Host 托管的 monitor worker 更新状态快照，并通过 WebSocket 推送给前端。
8. 前端也可随时通过 HTTP 获取当前树和节点状态快照。

## 监控页面

监控页面源文件位于：

```text
web/index.html
```

当前 `monitor/CMakeLists.txt` 中的网页复制目标已注释，因此构建时不会自动将
`index.html` 复制到插件输出目录。可以从源码目录打开，或按部署需求手动
复制到 rpc_gateway 的静态资源目录。
