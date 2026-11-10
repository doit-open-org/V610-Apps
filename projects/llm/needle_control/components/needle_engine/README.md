# needle_engine

[Needle 3](https://github.com/cactus-compute/needle) 的 C99 推理引擎，用于在 CPU 上运行 Needle 3 模型（`.cact`），做工具调用：输入一句话和一组工具定义，输出符合定义的函数调用 JSON。

## 来源

`src/`、`include/` 和 `tools/slice_cact.py` 原样取自 [PruhaNLP/needle-3-esp32](https://github.com/PruhaNLP/needle-3-esp32)，commit `d4dfa1346c53cac8067792c856a91fd9ec63746b`，未做修改，许可证为 Apache-2.0，见 `LICENSE`。该引擎是对 Needle 3 的独立实现，不依赖第三方库；在非 ESP32 平台上，内存分配使用普通的 `malloc`。

## 使用

```cmake
hifw_component_register(
    SRCS src/main.c
    REQUIRES needle_engine)
```

接口见 `include/nd_model.h`（模型、前缀快照）、`include/nd_grammar.h`（由工具定义生成约束语法）、`include/nd_sample.h`（受约束采样）和 `include/nd_tokenizer.h`。用法示例见本项目的 `main/src/agent.c`。

`nd_model_open()` 直接使用传入的模型数据，不复制，在 Linux 上可以把 `.cact` 文件 `mmap` 后传入。

## 模型

模型文件不随源码提供。从 Hugging Face 的 `Cactus-Compute/needle3` 下载 `needle3.cact`（20 层，33.7 MiB），再用 `tools/slice_cact.py` 按层数裁剪，裁剪只挑选层、不重新量化：

```sh
python3 tools/slice_cact.py --layers 8 --in needle3.cact --out needle3-l8.cact
```

| 层数 | 大小 |
| --- | --- |
| 6 | 10.4 MiB |
| 8 | 15.4 MiB |
| 20 | 33.7 MiB |

脚本需要 Python 3 和 numpy。模型权重同样是 Apache-2.0 许可。
