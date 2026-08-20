# BehaviorTreePlugin

This project builds one behavior-tree plugin with monitoring support compiled
into the same dynamic library. The root `CMakeLists.txt` includes the
monitor through `add_subdirectory(monitor)`; `monitor/CMakeLists.txt` uses
`target_sources` to add the monitoring implementation to `BehaviorTreePlugin`
without creating a second plugin library.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The build produces:

- `BehaviorTreePlugin`: the behavior-tree plugin including the monitoring
  bridge.
- `behavior-tree-monitor/index.html`: the monitoring web page.

The output directory defaults to `../../rpc_gateway/plugins`. It can be changed
with:

```bash
cmake -S . -B build \
  -DRPC_GATEWAY_PLUGIN_OUTPUT_DIR=/path/to/rpc_gateway/plugins
```

## Monitor

Load the single `BehaviorTreePlugin` module in the xplugin host, then open
either `web/index.html` from the source tree or the copied
`behavior-tree-monitor/index.html` from the output directory. Enter the HTTP
origin of the running xplugin host and connect.

The page uses:

```text
POST /backend/plugin-http/behavior-tree/tree.snapshot
POST /backend/plugin-http/behavior-tree/status.snapshot
POST /backend/plugin-http/behavior-tree/bridge.health
POST /backend/plugin-http/behavior-tree/load.node
WS   /backend/plugin-ws/behavior-tree
```

`load.node` reads the generated node model from the deployment layout below
and returns the complete XML in `data.content`:

```text
v1.2.1/
├── bin/
├── bt_trees/bt_nodes.xml
└── plugins/BehaviorTreePlugin.so
```

On Linux the path is resolved from the loaded plugin first, so it does not
depend on the directory from which `rpc_gateway` was started.

The built-in monitoring component connects to the Groot2 publisher exposed by
the same behavior-tree plugin. Its defaults are defined in
`monitor/include/bt_monitor_plugin.hpp`.
