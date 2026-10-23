# Blaz Blue Entropy Effect Trainer

该项目为AI生成，README也为AI辅助生成。

适用于苍翼：混沌效应 Steam版，1.1.2.147157.0。


## 编译说明

需要 Windows x64、支持 C++20 的 Visual Studio 桌面 C++ 工具、兼容 Windows App SDK 的 Windows SDK，vcpkg，以及 CMake 4.2 或更高版本。

```powershell
cmake --preset vs2026-x64
cmake --build --preset vs2026-x64 --config Release
```

如果不想依赖vcpkg

```powershell
cmake -S . -B out\build\custom -G "Visual Studio 18 2026" -A x64 -DCMAKE_TOOLCHAIN_FILE= -DCMAKE_PREFIX_PATH="<yaml-cpp 安装前缀>"
cmake --build out\build\custom --config Release
```

如果环境改变

```powershell
cmake --fresh --preset vs2026-x64
cmake --build --preset vs2026-x64 --config Release
```

## 测试与使用说明

```powershell
ctest --test-dir out\build\vs2026-x64 -C Release --output-on-failure
```

覆盖资源运算、活动对象所有权、失效地址、AP 保存标记、中英文本与运行时切换、UTF-8 安全截断、快捷键解析和窗口/DPI 行为。

交互桌面上的窗口与快捷键消息集成测试会短暂创建独立测试窗口，只触发测试回调，不连接游戏：

```powershell
.\Binaries\Release\BlazblueTests.exe --window-integration
```

需要核对运行中的游戏时，可执行只读诊断：

```powershell
.\Binaries\Release\BlazblueTests.exe --runtime-probe
.\Binaries\Release\BlazblueTests.exe --engine-probe --ap
.\Binaries\Release\BlazblueTests.exe --runtime-probe --watch-seconds 600
```

可在 YAML 中修改组合键，重启修改器后生效。如：

```yaml
hotkeys:
  refresh_add: 'Ctrl+1'
  exchange_add: 'Ctrl+2'
  ap_add: 'Ctrl+3'
```
