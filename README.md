# V610-Apps

V610 模组的应用集合，当前面向 Cove-610 开发板。各应用使用 V610-App-SDK 构建，具体功能、配置及使用方法见对应应用目录中的 README。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `examples/mipk/mipk_object_detection/` | 目标检测 |
| `examples/mipk/mipk_keypoints/` | 关键点检测 |
| `examples/mipk/mipk_wake_word/` | 唤醒词 |
| `projects/audio/voice_command/` | 离线中文语音唤醒 + 命令词识别（doit_kws） |
| `projects/vision/hand_gimbal/` | 手势云台：手部跟踪、捏合识别、舵机云台跟随 |
| `projects/llm/needle_control/` | 用 Needle 小模型把英文指令转成函数调用，控制板子 |
| `projects/robot/xgoduck/` | XGO Duck 机器鸭：IMU + 15 路总线舵机，50 Hz 强化学习策略行走、起身 |
| `components/media_video_rtsp/` | 共享 RTSP 推流组件 |
| `components/media_video_udp/` | 共享视频传输组件 |
| `components/media_audio_udp/` | 共享音频传输组件 |
| `tools/build.sh` | 统一构建脚本 |

## 构建

先按 V610-App-SDK 的 README 准备开发环境，并加载 SDK：

```sh
source /absolute/path/to/V610-App-SDK/export.sh
```

在本仓库根目录执行：

```sh
# 列出应用
sh tools/build.sh --list

# 构建指定应用
sh tools/build.sh mipk_object_detection

# 构建所有应用
sh tools/build.sh
```

构建脚本支持应用名称或项目路径（如 `examples/mipk/mipk_object_detection`），首次构建自动选择 `hi3516cv610`。可通过 `JOBS` 设置并行任务数，例如 `JOBS=4 sh tools/build.sh`。

各应用的构建中间文件位于自身的 `build/`，默认产物位于 `out/`。

## 添加应用

应用按 `<级别>/<分类>/<应用名>/` 组织，提供顶层 `CMakeLists.txt` 和 `main/` 组件后，可由统一构建脚本自动发现。

| 级别 | 放什么 |
| --- | --- |
| `examples/` | 示例：演示一种能力怎么用，代码尽量短，只用板子本身和摄像头、麦克风 |
| `projects/` | 完整项目：组合多种能力解决一个具体场景，可以接舵机等外设，README 说明接线、部署和调参 |

示例按演示的技术分类，例如 `examples/mipk/` 是 MIPK 推理框架的示例。完整项目按领域分类：

| 领域 | 放什么 |
| --- | --- |
| `vision/` | 输入是摄像头 |
| `audio/` | 输入是麦克风 |
| `llm/` | 输入是文本，用语言模型理解 |
| `robot/` | 输入是 IMU、舵机等传感器，控制机器人运动 |

同时用到多种输入的项目放 `multimodal/`。新分类有了第一个应用再建目录。

使用 V610-App-SDK 的 `hifw_project` 和 `hifw_component_register` 接入构建。应用自有配置放在 `Kconfig`，默认值放在 `sdkconfig.defaults`；只有一个应用使用的组件放在该应用自己的 `components/` 下，两个及以上应用使用时再移到仓库的 `components/`。

各应用的 README 负责说明其依赖、配置、运行资源及使用方法。

## 许可证

本仓库采用 [Apache License 2.0](LICENSE) 许可。应用中引入的第三方组件（如 mbedtls、cJSON、needle_engine、jmuxer）保留原许可证，见各自目录中的 LICENSE。
