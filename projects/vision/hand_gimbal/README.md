# 手势云台

`hand_gimbal` 用 MediaPipe 手部模型跟踪画面中的手，识别拇指与食指的捏合，捏住后驱动 pan/tilt 两路舵机，让云台跟着手转动，松开后云台停在原处。骨架和捏合状态画进 H.264 码流，通过 RTSP 观看。

```text
hand_gimbal/
├── components/
│   ├── box_tracker/      跨帧跟踪，让每只手保持在同一个槽位
│   └── servo_tracker/    舵机云台控制：引脚复用、sysfs PWM、控制律
├── main/src/
│   ├── main.c            推理、画骨架、RTSP 输出、捏合离合
│   └── hand_pinch.c      捏合判定状态机
└── ../../../components/media_video_rtsp/   RTSP 推流，与其他应用共用
```

模型包使用 `examples/mipk/mipk_keypoints/model/mediapipe_hand.mipk`，构建时复制到 `out/`。

## 硬件连接

| 舵机 | PWM1 通道 | GPIO | JP2 针脚 |
| --- | --- | --- | --- |
| pan（水平） | 5 | GPIO7_6 | 25 |
| tilt（俯仰） | 1 | GPIO1_0 | 21 |

- **运行前拔掉 LCD 屏的排线。** PWM1 的 8 个通道都落在 LCD 引脚上，21 脚是屏幕输出的 TE 信号，屏幕插着时会和舵机信号冲突。程序启动时通过 `/dev/mem` 把这两个引脚切换为 PWM 功能，不需要修改设备树或镜像，重启后恢复。
- **舵机要独立供电**，只和开发板共地。MG90S 堵转电流约 700 mA，从开发板取电会把电压拉低，导致开发板复位。
- 摄像头接 MIPI-1，云台装在摄像头上。

## 构建

```sh
source <V610-App-SDK>/export.sh
sh tools/build.sh hand_gimbal        # 在 V610-Apps 根目录执行
```

产物在 `out/`：`hand_gimbal`、`mediapipe_hand.mipk`、`mipk_capabilities.json`。

## 运行

把 `out/` 中的文件拷到开发板同一目录后运行：

```sh
./hand_gimbal
```

用 VLC 或 `ffplay` 打开 `rtsp://192.168.137.2:8554/live` 观看。手张开时骨架为绿色；拇指与食指捏住、其余三指伸直时变为红色，并在两个指尖标十字。捏住保持约 0.3 秒后云台开始跟随，松开立即停止。

| 参数 | 说明 |
| --- | --- |
| `--model PATH` | 模型包路径，默认 `./mediapipe_hand.mipk` |
| `--camera 0\|1` | 摄像头，默认 `0`（MIPI-1） |
| `--rtsp PORT` | RTSP 端口，默认 `8554` |
| `--frames N` | 处理 N 帧后退出，默认持续运行 |
| `--report-interval N` | 每 N 帧打印一次状态，默认 `30` |
| `--trace` | 每帧打印捏合比值，用于标定阈值 |

捏合事件随时打印：

```text
frame=182 hand=0 pinch-down pinch_ratio=0.238 extension=1.712 straight=0.951
frame=199 hand=0 pinch-up pinch_ratio=0.914 extension=1.688 straight=0.962
```

舵机通道打不开时（例如 `/dev/mem` 不可用）只打印一行 `servo tracking off:`，跟踪和画面照常工作。程序退出时先把两路舵机打回中位再关闭输出。

## 捏合判定

模型输出每只手 21 个关键点，捏合由应用层判定，用两个与距离远近无关的比值：

| 量 | 定义 |
| --- | --- |
| `pinch_ratio` | 拇指尖到食指尖的距离 ÷ 食指根到小指根的距离 |
| `extension` | 中指、无名指、小指指尖到手腕的距离 ÷ 手腕到中指根的距离，取最小值 |
| `straight` | 中指、无名指、小指从指根到指尖的直线距离 ÷ 沿关节累加的长度，取最小值 |

要求拇指与食指捏住、其余三指伸直，也就是 OK 手势。只看 `pinch_ratio` 无法区分捏合和握拳，`extension` 用来排除握拳，`straight` 用来排除其余手指弯曲时的捏合。进入和退出捏合都有滞回区间，并要求连续若干帧确认；手短暂漏检几帧不会打断捏合。

阈值在 `hifw.py menuconfig` 中调整，用 `--trace` 观察实际比值：

| 配置项 | 默认值 | 含义 |
| --- | --- | --- |
| `HAND_GIMBAL_CLOSE_PERCENT` | 45 | 进入捏合的 `pinch_ratio` 上限（百分比） |
| `HAND_GIMBAL_OPEN_PERCENT` | 70 | 释放捏合的 `pinch_ratio` 下限 |
| `HAND_GIMBAL_EXTENSION_PERCENT` | 110 | 其余手指需要达到的 `extension` |
| `HAND_GIMBAL_STRAIGHT_PERCENT` | 85 | 其余手指需要达到的伸直度 |
| `HAND_GIMBAL_CONFIRM_FRAMES` | 2 | 状态变化所需的连续帧数 |
| `HAND_GIMBAL_PINCH_FRAMES` | 5 | 捏住多少帧后云台才开始跟随 |

## 云台控制

云台跟随画面中最大的、处于捏合状态的那只手，也就是离镜头最近的那只，目标是把手的中心保持在画面中央。舵机不反馈位置，闭环完全靠图像。

- **角度型舵机**（MG90S、SG90，默认）：每帧按偏差累加角度，行程限制在中位 ±500 μs 以内。
- **连续旋转舵机**（360°、FS90R）：偏差直接对应转速，在 `menuconfig` 中切换。
- 画面中心留有不动区，避免云台追着检测框的抖动来回动；每帧的指令变化量也有上限。

调速相关：

| 现象 | 调整 |
| --- | --- |
| 云台转反了 | 打开 `SERVO_TRACKER_PAN_INVERT` 或 `SERVO_TRACKER_TILT_INVERT` |
| 在目标附近来回摆 | 降低 `SERVO_TRACKER_SLEW_PERCENT`，再降低对应轴的步长 |
| 手静止时轻微抖动 | 调大 `SERVO_TRACKER_DEAD_ZONE_PERCENT` |
| 跟不上 | 调大 `SERVO_TRACKER_STEP_PERCENT`（pan）或 `SERVO_TRACKER_TILT_STEP_PERCENT`（tilt） |
| 舵机发烫、有咔咔声 | 顶到了机械限位，调小 `SERVO_TRACKER_SPAN_US` 或单轴的 `*_SPAN_US` |

推理帧率是跟随速度的上限：画面中有手时单帧约 55 ms，约 18 帧/秒。

## 已知限制

- 手掌检测每帧都运行，占单帧耗时约六成。
- 跨帧跟踪不区分身份，手离开画面再回来算作新的手。
- 其余手指弯曲的捏合不会被识别，这是为了和握拳区分而有意为之。
- 写引脚复用寄存器需要 `/dev/mem`；内核不能开启 `CONFIG_IO_STRICT_DEVMEM`。
