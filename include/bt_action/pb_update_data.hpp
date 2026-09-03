#pragma once

#include <string>
#include <vector>

namespace pb_update_data {
#pragma pack(push, 1)
  enum class NavState : int {
    NONE = 0,      // 无任务
    WAITING = 1,   // 等待执行导航
    RUNNING = 2,   // 正在执行导航
    PAUSED = 3,    // 导航暂停
    ARRIVED = 4,   // 到达
    FAILED = 5,    // 失败
    CANCELED = 6,  // 取消
    TIMEOUT = 7    // 超时
  };

  struct MobileRobotModbusTCPJGBOTData {
    // Input Registers
    float x_axis;                         // 位置x，单位mm
    float y_axis;                         // 位置y，单位mm
    float theta;                          // 角度theta，单位0.1度
    int16_t current_navigate_station_id;  // 当前站点id
    // 机器人定位状态
    uint16_t positioning_status;
    // 机器人当前导航状态
    NavState nav_state;
    // 机器人当前导航类型，0：没有导航；1：自由导航到任意点；2：自由导航到站点；3：路径导航到站点；7：平动移动；100：其他导航
    uint16_t nav_type;
    // 机器人定位置信度，0~1
    float positioning_confidence;
    // 电池电量百分比，0~100
    uint16_t battery_percentage;
    // 电池温度，单位摄氏度
    float battery_temperature;
    // 电池电压，单位V
    float battery_voltage;
    // 电池电流，单位A
    float battery_current;
    // 控制器温度，单位摄氏度
    float controller_temperature;
    // 控制器湿度，单位百分比
    float controller_humidity;
    // 控制器电压
    float controller_voltage;
    // 总里程
    float total_distance;
    // 累计运行时间，单位小时
    float total_runtime_hours;
    // Fatal错误代码，0表示无错误
    uint16_t fatal_error_code;
    // Error错误代码，0表示无错误
    uint16_t error_code;
    // Warning错误代码，0表示无错误
    uint16_t warning_code;
    // 机器人当前所在站点
    int16_t robot_current_station_id;
    // 机器人上次所在站点，0表示没有上次站点
    int16_t last_station_id;
    // 机器人下一个要经过的站点，0表示没有下一个站点
    int16_t next_station_id;
    // Major版本号
    uint16_t major_version;
    // Minor版本号
    uint16_t minor_version;
    // Patch版本号
    uint16_t patch_version;
    // 当前地图名
    uint16_t current_map_id;
    // 机器人地图状态
    uint16_t map_status;
    // 机器人控制权是否被抢占
    uint16_t is_control_preempted;
    // 被阻挡原因
    uint16_t blocked_reason;
    // 发生阻挡的超声id号
    int16_t blocked_ultrasonic_id;
    // 发生阻挡的DI的id号
    int16_t blocked_di_id;
    // 阻挡位置的x坐标，单位m
    float blocked_position_x;
    // 阻挡位置的y坐标，单位m
    float blocked_position_y;
    // 机器人vx速度，单位m/s
    float vx;
    // 机器人vy速度，单位m/s
    float vy;
    // 机器人角速度，单位rad/s
    float vtheta;
    // 货叉高度，单位m
    float fork_height;
    // 机器人舵角，单位度rad
    float rudder_angle;
    // 顶升机构状态
    uint16_t lifting_status;
    // 皮带状态
    uint16_t belt_status;
    // 顶升机构实时高度，单位m
    uint16_t lifting_current_height;
    // 顶升机构错误码
    uint16_t lifting_error_code;
    // 皮带错误码
    uint16_t belt_error_code;
    // 今日总里程
    float today_distance;
    // 当前SRC模式
    uint16_t src_mode;
    // 机器人减速原因
    uint16_t slowdown_reason;
    // 托盘角度，单位度rad
    float pallet_angle;
    // 舵角信息
    float rudder_angle_info;

    // Discrete Inputs
    // 是否减速
    bool is_slowdown;
    // 是否被阻挡
    bool is_blocked;
    // 是否充电
    bool is_charging;
    // 是否急停
    bool is_emergency_stop;
    // 是否抱闸
    bool is_brake_engaged;
    // 货叉是否到位
    bool is_fork_in_position;
    // 叉车控制模式
    bool is_auto_mode;

    // 是否有Fatal
    bool has_fatal_error;
    // 是否有Error
    bool has_error;
    // 是否有Warning
    bool has_warning;
    // 顶升机构是否启用
    bool is_lifting_enabled;
    // 顶升机构是否急停
    bool is_lifting_emergency_stopped;
    // 顶升机构是否有料
    bool is_lifting_loaded;
    // 皮带是否启用
    bool is_belt_enabled;
    // 皮带是否急停
    bool is_belt_emergency_stopped;
    // 皮带是否有料
    bool is_belt_loaded;
    // 机器人是否载货
    bool is_loaded;
    // 底盘是否静止
    bool is_stopped;
  };
#pragma pack(pop)
}  // namespace pb_update_data
