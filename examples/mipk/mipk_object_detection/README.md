# MIPK 目标检测

从 CV610 摄像头采集图像，使用 YOLO11n 检测目标，并通过 RTSP 或 UDP 输出带检测框的 H264 视频。
随附模型 `yolo11n.mipk`，支持 COCO 80 类目标。

## 编译

按[仓库说明](../../../README.md)准备 V610-App-SDK，在本目录执行：

```sh
source /path/to/V610-App-SDK/export.sh
hifw.py set-target hi3516cv610
hifw.py build
```

产物位于 `out/`，包含程序 `mipk_object_detection` 和模型 `yolo11n.mipk`。

## 使用

将 `out/` 中的文件复制到开发板同一目录，进入该目录运行：

```sh
./mipk_object_detection --rtsp 8554
```

用 VLC 或 ffplay 打开 `rtsp://<开发板IP>:8554/live` 查看视频。
也可通过 UDP 发送到电脑，将下面的 IP 改为电脑地址，接收端需支持应用的分片协议：

```sh
./mipk_object_detection --host 192.168.137.1 --port 9000
```

`--camera 0|1` 选择摄像头，`--model PATH` 指定其他兼容的模型包。
按 `Ctrl-C` 停止，完整参数见 `./mipk_object_detection --help`。

参数默认使用模型包内配置。需要热更新时，自行创建 `options.json`，启动时加
`--overrides options.json`，保存文件后自动生效。默认模型的检测阈值示例：

```json
{"schema": "mipk.runtime_overrides.v1", "nodes": {"postprocess_output": {"score_threshold": 0.3}}}
```

代码调参可在 `main/src/main.c` 的 `mipk_video_open()` 成功后调用
`mipk_video_update_node_options()`；检测阈值节点为 `postprocess_output`，NMS 节点为 `detections`。
更换模型时，按其清单中的节点 `id` 填写，省略 `id` 时使用 `output` 名称。
