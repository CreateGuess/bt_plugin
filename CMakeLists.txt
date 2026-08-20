cmake_minimum_required(VERSION 3.14)
project(BehaviorTreePlugin VERSION 0.1.0 LANGUAGES C CXX)

# 主插件及监控子项目统一使用 C++17。关闭编译器扩展可以避免代码无意中依赖
# gnu++17 等非标准特性；POSITION_INDEPENDENT_CODE 确保静态依赖能够安全地
# 链接进 Linux 动态插件。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 行为树插件部署到 rpc_gateway 的插件扫描目录；监控功能编译在同一插件中。
# 使用 CACHE PATH 后，部署环境可以通过
# -DRPC_GATEWAY_PLUGIN_OUTPUT_DIR=/absolute/path 覆盖默认位置。
set(RPC_GATEWAY_PLUGIN_OUTPUT_DIR
    "${CMAKE_BINARY_DIR}"
    CACHE PATH "Output directory for compiled rpc_gateway plugins")

# 保留原工程的全局头文件搜索路径。主插件源码可以直接包含 include/ 下的行为树
# 节点、管理器、JSON 以及自动注册头文件。
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# --------- 第三方依赖 ---------
# CPM.cmake 负责在本地依赖不存在时拉取指定版本。生产或离线构建通常使用 3rd/
# 中已经固定版本的源码，从而保证主插件和监控插件使用完全相同的 ABI。
include(cmake/CPM.cmake)

# fmt 优先使用仓库内的 11.2.0；只有本地目录不存在时才通过 CPM 下载同一版本。
# 主插件及其监控源码均使用 fmt::fmt-header-only，不会额外生成需要部署的
# fmt 动态库。
if(EXISTS "${CMAKE_SOURCE_DIR}/3rd/fmt-11.2.0/CMakeLists.txt")
  message(STATUS "Using local fmt")
  CPMAddPackage(
    NAME fmt
    VERSION 11.2.0
    SOURCE_DIR "${CMAKE_SOURCE_DIR}/3rd/fmt-11.2.0"
  )
else()
  message(STATUS "Downloading fmt via CPM")
  CPMAddPackage(
    NAME fmt
    VERSION 11.2.0
    GITHUB_REPOSITORY fmtlib/fmt
    GIT_TAG 11.2.0
  )
endif()

# BehaviorTree.CPP 的构建选项统一集中在这里：
#   - 不构建命令行工具、示例、测试和 SQLite 日志；
#   - 构建静态库并最终嵌入插件；
#   - 使用随源码提供的 cppzmq 和 tinyxml2；
#   - 保留默认开启的 Groot2 接口，通过 1667 端口发布行为树状态。
set(BT_OPTIONS
    "BTCPP_BUILD_TOOLS OFF"
    "BTCPP_SHARED_LIBS OFF"
    "BTCPP_EXAMPLES OFF"
    "BTCPP_SQLITE_LOGGING OFF"
    "USE_VENDORED_CPPZMQ ON"
    "USE_VENDORED_TINYXML2 ON"
    "BUILD_TESTING OFF"
)

# 与 fmt 相同，优先使用 3rd/BehaviorTree.CPP-4.9.0；本地源码缺失时才由 CPM
# 获取固定的 4.9.0 版本，避免构建时意外升级行为树库。
if(EXISTS "${CMAKE_SOURCE_DIR}/3rd/BehaviorTree.CPP-4.9.0/CMakeLists.txt")
  message(STATUS "Using local BehaviorTree.CPP")
  CPMAddPackage(
    NAME BehaviorTree.CPP
    SOURCE_DIR "${CMAKE_SOURCE_DIR}/3rd/BehaviorTree.CPP-4.9.0"
    OPTIONS ${BT_OPTIONS}
  )
else()
  message(STATUS "Downloading BehaviorTree.CPP via CPM")
  CPMAddPackage(
    NAME BehaviorTree.CPP
    GITHUB_REPOSITORY BehaviorTree/BehaviorTree.CPP
    GIT_TAG 4.9.0
    OPTIONS ${BT_OPTIONS}
  )
endif()
# if(TARGET behaviortree_cpp)
#     message(STATUS "FOUND behaviortree_cpp ✅")
# endif()
# if(TARGET BehaviorTree::behaviortree_cpp)
#     message(STATUS "FOUND BehaviorTree::behaviortree_cpp ✅")
# endif()

# gateway_proto 和 xplugin SDK 由部署环境提供：
#   gateway_proto::gateway_proto 提供机器人网关使用的 protobuf 消息；
#   xplugin::sdk 提供插件生命周期、日志、订阅、HTTP 和 WebSocket API。
# 若上层工程已创建对应目标，则不重复 find_package，方便本项目独立构建或作为
# 更大工程的子目录构建。
# find_package(Protobuf REQUIRED)
if(NOT TARGET gateway_proto::gateway_proto)
  find_package(gateway_proto REQUIRED)
endif()
if(NOT TARGET xplugin::sdk)
  find_package(xplugin-dev REQUIRED)
endif()

# 这里只递归收集顶层 source/ 中原有的主插件实现。监控源码位于
# monitor/source/，不会被该 GLOB 匹配，而是在 monitor/CMakeLists.txt 中通过
# target_sources 显式加入同一个 BehaviorTreePlugin 动态库。
# CONFIGURE_DEPENDS 会在顶层 source/ 新增或删除 .cpp 后触发 CMake 重新配置。
file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/source/*.cpp")
add_library(${PROJECT_NAME} SHARED ${sources})

# 整个 BehaviorTreePlugin 使用日志实例名 BtPlug。由于监控源码属于同一目标，
# 它也复用这个日志实例，不再创建第二套插件日志生命周期。
target_compile_definitions(${PROJECT_NAME} PRIVATE PLUGIN_LOG_NAME=BtPlug)

# 将主插件直接写入 rpc_gateway 插件目录。隐藏普通 C/C++ 符号可以减小导出表，
# 并降低多个插件加载到同一 Host 进程时的符号冲突风险。
set_target_properties(${PROJECT_NAME} PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${RPC_GATEWAY_PLUGIN_OUTPUT_DIR}"
  RUNTIME_OUTPUT_DIRECTORY "${RPC_GATEWAY_PLUGIN_OUTPUT_DIR}"
  CXX_VISIBILITY_PRESET hidden
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

# 多配置生成器会为每种配置读取独立的输出属性；全部显式设置后，无论构建
# Debug、Release、RelWithDebInfo 还是 MinSizeRel，插件位置都保持一致。
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set_target_properties(${PROJECT_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY_${config} "${RPC_GATEWAY_PLUGIN_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_${config} "${RPC_GATEWAY_PLUGIN_OUTPUT_DIR}"
  )
endforeach()

# 在链接主插件之前确保目标目录存在，避免首次构建因输出目录尚未创建而失败。
add_custom_command(TARGET ${PROJECT_NAME} PRE_LINK
  COMMAND ${CMAKE_COMMAND} -E make_directory "${RPC_GATEWAY_PLUGIN_OUTPUT_DIR}"
)

# 主插件依赖保持原样：
#   gateway_proto     解析实时和非实时系统状态；
#   fmt               提供格式化日志；
#   xplugin::sdk      提供插件生命周期及 HTTP 接口；
#   behaviortree_cpp  提供行为树执行、节点和 Groot2 Publisher；
#   tinyxml2          用于生成和读取行为树 XML。
# PRIVATE 表示这些依赖不会成为 BehaviorTreePlugin 的公共 CMake 接口。
target_link_libraries(${PROJECT_NAME} PRIVATE 
  gateway_proto::gateway_proto fmt::fmt-header-only xplugin::sdk
  # protobuf::libprotobuf 
  behaviortree_cpp tinyxml2::tinyxml2
)

# --------- 行为树监控组件 ---------
# 监控模块拥有独立的 CMakeLists.txt，但不创建独立动态库。主工程只负责：
#   1. 提供统一的第三方依赖目标（fmt、BehaviorTree.CPP、tinyxml2、xplugin）；
#   2. 将插件输出目录 RPC_GATEWAY_PLUGIN_OUTPUT_DIR 传递给子目录；
#   3. 通过 add_subdirectory 引入监控构建规则。
#
# monitor/CMakeLists.txt 使用 target_sources 将监控实现加入 BehaviorTreePlugin，
# 最终只有一个插件动态库和一个 PLUGIN_DECLARE 入口。监控只在主插件生命周期
# 中附加启停，不改变 BTManager 的树加载、节点注册和 tick 执行逻辑。
add_subdirectory(monitor)

# cmake .. -DCMAKE_DISABLE_FIND_PACKAGE_ament_cmake=ON
