# Needle 自然语言控制

`needle_control` 在板上用 [Needle 3](https://github.com/cactus-compute/needle) 小模型把一句英文指令转成函数调用，并直接执行：拍照、响铃、倒计时、查询内存和 IP 地址。完全离线，在 CPU 上运行。

```text
> take a picture
call: [{"name":"take_photo","arguments":{}}]  (confidence 0.78, 10 tokens, 0.4 s)
done: photo saved to /tmp/photo_1.jpg
```

```text
needle_control/
├── components/
│   └── needle_engine/ Needle 3 推理引擎（C99，Apache-2.0，原样引入）
├── main/src/
│   ├── main.c         串口交互、HTTP 接口、把调用分发给各个工具
│   ├── agent.c        Needle 推理：加载模型、工具前缀快照、受约束解码
│   └── board.c        工具的实现
├── tools.json         工具定义，模型只会从这里选
└── tools/prepare_model.sh   下载并裁剪模型
```

## 准备模型

模型不在仓库里。在本应用目录执行：

```sh
sh tools/prepare_model.sh
```

脚本从 Hugging Face 下载 `needle3.cact`（20 层，34 MB），裁剪成 8 层的 `out/needle3-l8.cact`（15.4 MB）。需要 Python 3 和 numpy。`hifw.py fullclean` 会删除 `out/`，之后要重新执行一次。

## 构建

```sh
source <V610-App-SDK>/export.sh
sh tools/build.sh needle_control      # 在 V610-Apps 根目录执行
```

把 `out/` 中的 `needle_control`、`tools.json` 和 `needle3-l8.cact` 拷到开发板同一目录。`needle3.cact` 不需要拷。

## 使用

```sh
./needle_control
```

启动时会把工具定义预填充一次，之后每个请求只处理这一句话。在终端里直接输入英文指令：

| 指令示例 | 调用 |
| --- | --- |
| take a picture | `take_photo`：MIPI-1 摄像头拍一张 1280×720 的 JPEG，存到 `/tmp/photo_<n>.jpg` |
| beep three times | `beep`：扬声器响 3 声 |
| remind me in 2 minutes | `set_timer`：120 秒后响 3 声 |
| how much memory is free | `get_memory` |
| what is my ip address | `get_ip_address` |

也可以通过 HTTP 调用，默认端口 8080：

```sh
curl -X POST http://192.168.137.2:8080/command -d '{"text":"beep twice"}'
```

```json
{"text":"beep twice","call":[{"name":"beep","arguments":{"times":2}}],"ok":true,"result":"beeped 2 times","confidence":0.4065,"ms":498}
```

| 参数 | 说明 |
| --- | --- |
| `--model FILE` | 模型路径，默认 `./needle3-l8.cact` |
| `--tools FILE` | 工具定义，默认 `./tools.json` |
| `--port N` | HTTP 端口，默认 `8080`；`0` 关闭 HTTP |
| `--think` | 先输出推理过程再给出调用；生成的 token 更多、更慢，准确率在本应用的测试里没有提高 |

用 `S40app` 开机自启动时没有终端输入，程序只提供 HTTP 接口。

`confidence` 是模型对这次调用的置信度，偏低时结果更可能不对。没有合适的工具时输出空调用 `[]`，不执行任何动作。

## 修改工具

工具定义在 `tools.json`，对应的实现在 `board.c`，分发在 `main.c` 的 `run_command()`。修改时注意：

- **工具前缀不能超过 192 个 token**。模型的 KV 缓存窗口是 256，前缀只有前 192 个 token 会被固定保留，超出的部分会被后续请求覆盖，导致连续请求的结果互相干扰。启动时打印的 `prefix N tokens` 就是前缀长度，现在的 5 个工具是 159 个。
- 描述要短，并且每个工具的用途要明显不同。用一个枚举参数区分几种查询（例如 `get_status` 带 `item`），效果比拆成几个独立的工具差很多。
- 参数只支持字符串、整数、数字和布尔值，不支持嵌套对象和数组。

## 准确率与速度

未经微调的模型在 10 条测试指令上对了 8 条，例如 `make a sound twice` 会被当成拍照。需要更高准确率时，可以按 Needle 的方法针对这几个工具做 LoRA 微调。

Cove-610 实测（8 层模型）：

| 项目 | 数值 |
| --- | --- |
| 启动时预填充工具前缀（159 token） | 约 13 秒，只在启动时一次 |
| 每个请求 | 2.1～2.6 秒，其中拍照、响铃等动作本身的时间另计 |
| 进程 RSS | 约 19 MB，大部分是映射的模型文件页 |
| 系统可用内存 | 运行期间保持在约 35 MB |

推理用两个 CPU 核：`agent.c` 通过引擎的 `nd_parallel_rows` 接口，把矩阵-向量乘法和注意力头按输出行拆给两个线程，比单核快约 1.5 倍。这个模型的计算是逐 token 的矩阵-向量乘法，权重为 2-bit 量化，并有 engram 查表等非标准算子，不适合用 NPU。

模型以 `mmap` 方式加载，不读入进程内存，由内核按需从文件加载。
