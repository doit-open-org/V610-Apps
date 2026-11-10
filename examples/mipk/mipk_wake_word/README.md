# MIPK 唤醒词

使用 CV610 内置麦克风检测英文唤醒词“Okay Nabu”，命中后打印 `WAKEUP` 并播放提示音。
支持向电脑发送 Opus 音频，以及接收电脑回传的音频。
模型来自 [micro-wake-word-models](https://github.com/esphome/micro-wake-word-models)（Apache-2.0）。

## 编译

按[仓库说明](../../../README.md)准备 V610-App-SDK，在本目录执行：

```sh
source /path/to/V610-App-SDK/export.sh
hifw.py set-target hi3516cv610
hifw.py build
```

产物位于 `out/`，包含程序 `mipk_wake_word` 和模型 `okay_nabu.mipk`。

## 使用

将 `out/` 中的文件复制到开发板同一目录，进入该目录运行，将 IP 改为电脑地址：

```sh
./mipk_wake_word --host 192.168.137.1 --port 9001 --rx-port 9001
```

`--port` 为音频发送端口，`--rx-port` 为开发板接收端口。
音频收发使用 `XPAU/Opus` UDP 协议，电脑端需支持此协议。
`--mic-channel 0|1` 选择麦克风通道，`--no-aec` 关闭回声消除，`--play-local` 开启本地回放。
按 `Ctrl-C` 停止，完整参数见 `./mipk_wake_word --help`。

默认使用模型包内参数：唤醒阈值 0.97、5 次推理取均值、命中 1 次即唤醒。
需要热更新时，自行创建 `options.json`，启动时加 `--overrides options.json`，保存后自动生效：

```json
{"schema": "mipk.runtime_overrides.v1", "nodes": {"wake_word": {"score_threshold": 0.95}}}
```

代码调参可在 `main/src/main.c` 的 `mipk_audio_open()` 成功后调用
`mipk_audio_update_wake_word()`，默认模型的节点名称为 `wake_word`。
