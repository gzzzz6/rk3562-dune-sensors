# 从 DUNE 基线构建

从 [LSTS/dune](https://github.com/LSTS/dune) 取 **09da1c45aa3ba4ce94cc86cdff555bc7691b869c**。使用 Linux CMake 及 ARM64 GCC 9.4，sysroot 需与 RK3562 镜像兼容，可用交付程序跳过此步骤。

- 惯导：复制 `MFOSN/{Task.cpp,Task.cmake,Tasks.cmake,Protocol.hpp,Alignment.hpp}` 到 DUNE 的 `src/Sensors/MFOSN/`，配置 CMake 时传入 `-DTASK_FILE=/path/to/dune/src/Sensors/MFOSN/Tasks.cmake`。
- 深度计：复制 `KellerDepth/{Task.cpp,Task.cmake,Protocol.hpp}` 到 DUNE 的 `src/Sensors/KellerDepth/`，用默认任务表构建。可把 `test_KellerDepth.cpp` 放在 DUNE 的 `programs/tests/` 运行协议测试。
- 分别用不同的 CMake 构建目录，将各自生成的 `dune` 置于 `board/bin` 对应位置。自行修改程序后要重算 `board/SHA256SUMS`。

```sh
cmake -S /path/to/dune -B /path/to/build \
  -DCMAKE_BUILD_TYPE=Release -DCROSS=/path/to/aarch64-linux-gnu \
  -DCMAKE_SYSROOT=/path/to/arm64-sysroot
cmake --build /path/to/build --target dune -j4
```

旧试验插件源码在 `neptus/`（相对于本页），以 Neptus Java 11 类路径编译，涉及原模拟控制与校准环境；本套件的实物台架查看器独立于这些插件。
