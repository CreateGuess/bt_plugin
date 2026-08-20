# 行为树监控组件
# ----------------
# 本目录不创建动态库。监控源码通过 target_sources 直接加入顶层已经创建的
# BehaviorTreePlugin 目标，因此最终仍然只有一个行为树插件动态库和一个
# PLUGIN_DECLARE 入口。

# 本文件依赖顶层 CMakeLists.txt 先创建 BehaviorTreePlugin 目标。禁止直接在
# monitor 目录配置，避免误以为这里会生成一个可独立加载的插件。
if(NOT TARGET BehaviorTreePlugin)
  message(FATAL_ERROR
    "The monitor is an internal BehaviorTreePlugin component. "
    "Configure from the bt_plugin root directory: cmake -S . -B build")
endif()

# BehaviorTree.CPP 在启用 Groot2 支持时创建 cppzmq 接口目标。监控组件通过它
# 获取 zmq.h、libzmq 以及平台所需的链接信息，用于访问主插件运行时创建的
# Groot2Publisher。
if(NOT TARGET cppzmq)
  message(FATAL_ERROR
    "The monitor requires cppzmq. "
    "Keep BTCPP_GROOT_INTERFACE and USE_VENDORED_CPPZMQ enabled.")
endif()

# 监控后台轮询使用 std::thread。Threads::Threads 会为不同平台提供正确的线程
# 编译和链接参数，避免在 CMake 中写死 -pthread。
find_package(Threads REQUIRED)

# 把监控源码直接加入已存在的 BehaviorTreePlugin 目标。
target_sources(BehaviorTreePlugin PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/source/bt_monitor_plugin.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/source/bt_monitor_integration.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/bt_monitor_plugin.hpp"
)

target_include_directories(BehaviorTreePlugin PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(BehaviorTreePlugin PRIVATE
  cppzmq
  Threads::Threads
)

# 允许部署环境单独指定网页输出目录；默认复制到主插件输出目录下的
# behavior-tree-monitor 子目录。该设置只影响静态网页，不创建额外库。
# set(BEHAVIOR_TREE_MONITOR_WEB_OUTPUT_DIR
#   "${RPC_GATEWAY_PLUGIN_OUTPUT_DIR}/behavior-tree-monitor"
#   CACHE PATH "Output directory for the behavior tree monitor web page"
# )

# 使用独立的构建辅助目标复制自包含的 index.html。这里不能使用
# add_custom_command(TARGET BehaviorTreePlugin POST_BUILD)，因为 CMake 要求这种
# TARGET 形式的命令必须和 BehaviorTreePlugin 写在创建它的同一目录；主插件在
# 顶层创建，而当前文件位于 monitor 子目录。
#
# BehaviorTreeMonitorWeb 不会生成动态库。add_dependencies 保证先完成主插件
# 构建，再执行页面复制；copy_if_different 避免页面没有变化时反复更新时间戳。
# 部署产物仍是：
#   1. 一个 BehaviorTreePlugin 动态库；
#   2. 一个供浏览器打开的静态 index.html。
# add_custom_target(BehaviorTreeMonitorWeb ALL
#   COMMAND ${CMAKE_COMMAND} -E make_directory
#           "${BEHAVIOR_TREE_MONITOR_WEB_OUTPUT_DIR}"
#   COMMAND ${CMAKE_COMMAND} -E copy_if_different
#           "${PROJECT_SOURCE_DIR}/web/index.html"
#           "${BEHAVIOR_TREE_MONITOR_WEB_OUTPUT_DIR}/index.html"
#   COMMENT "Copying behavior tree monitor web page"
#   VERBATIM
# )

# add_dependencies(BehaviorTreeMonitorWeb BehaviorTreePlugin)
