# MIPK 关键点检测

从 CV610 摄像头采集图像，检测人体或手部关键点，并通过 RTSP 或 UDP 输出带骨架的视频。
随附以下模型和关键点布局：

| 模型 | 布局文件 | 检测内容 |
| --- | --- | --- |
| `person_pose.mipk` | `coco.keypoints` | 人体 17 个关键点，默认使用 |
| `mediapipe_hand.mipk` | `hand.keypoints` | 手部 21 个关键点 |

人体模型来自 Ultralytics（AGPL-3.0），手部模型来自 opencv_zoo（Apache-2.0）。

## 编译

按[仓库说明](../../../README.md)准备 V610-App-SDK，在本目录执行：

```sh
source /path/to/V610-App-SDK/export.sh
hifw.py set-target hi3516cv610
hifw.py build
```

产物位于 `out/`，包含程序 `mipk_keypoints`、模型包和 `.keypoints` 布局文件。

## 使用

将 `out/` 中的文件复制到开发板同一目录，进入该目录运行人体检测：

```sh
./mipk_keypoints --rtsp 8554
```

用 VLC 或 ffplay 打开 `rtsp://<开发板IP>:8554/live` 查看视频。切换为手部检测：

```sh
./mipk_keypoints --model mediapipe_hand.mipk --keypoints hand.keypoints --rtsp 8554
```

只显示关键点时加 `--keypoints ""`。布局文件中的 `point`、`bone`、`color`、`size`
可调整显示的点、连线、颜色和大小，参考 `keypoints/` 中的两个文件。
使用 UDP 时，将 `--rtsp 8554` 替换为 `--host <电脑IP> --port 9000`，接收端需支持应用的分片协议。
按 `Ctrl-C` 停止，完整参数见 `./mipk_keypoints --help`。

参数默认使用模型包内配置。需要热更新时，自行创建 `options.json`，启动时加
`--overrides options.json`，保存文件后自动生效。默认人体模型的检测阈值示例：

```json
{"schema": "mipk.runtime_overrides.v1", "nodes": {"persons": {"score_threshold": 0.6}}}
```

代码调参可在 `main/src/main.c` 的 `mipk_video_open()` 成功后调用
`mipk_video_update_node_options()`。人体模型使用 `persons` 节点；手部模型使用 `palms`
节点调整检测阈值，使用 `hands` 节点的 `presence_threshold` 调整关键点输出阈值。
