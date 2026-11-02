# Blaz Blue Entropy Effect Trainer

该项目为AI生成，README也为AI辅助生成。

适用于苍翼：混沌效应 Steam版。旧版本地址标记仅用于快速识别；游戏更新后会继续尝试验证当前活动模型，而不是仅因版本变化关闭功能。无法确认对象布局或目标唯一性时仍会安全阻断写入。

## 编译说明

需要 Windows x64、支持 C++20 的 Visual Studio 桌面 C++ 工具、兼容 Windows App SDK 的 Windows SDK，vcpkg，以及 CMake 4.2 或更高版本。依赖为 yaml-cpp 和 LZ4；Windows 音效使用系统多媒体 API，不需要额外音频文件。

```powershell
cmake --preset vs2026-x64
cmake --build --preset vs2026-x64 --config Release
```

如果 vcpkg 未加入 `PATH`，可明确指定自己的安装目录：

```powershell
cmake --fresh --preset vs2026-x64 -DVCPKG_ROOT="<自己的 vcpkg 根目录>"
```

如果不想依赖vcpkg

```powershell
cmake -S . -B out\build\custom -G "Visual Studio 18 2026" -A x64 -DCMAKE_TOOLCHAIN_FILE= -DCMAKE_PREFIX_PATH="<yaml-cpp 与 LZ4 安装前缀>"
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

覆盖资源运算、活动对象所有权、失效地址、AP 保存标记、中英切换、UTF-8 安全截断、快捷键音效、更新识别、窗口/DPI，以及合成存档的添加、修改、确认取消、过期文件、备份失败和游戏启动时阻断。

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

默认主键盘 `1`、`2`、`3` 分别增加刷新次数、兑换点和 AP，使用界面中的当前增量。快捷键仅在游戏前台且对应目标可写时启用，按住不会连续触发。成功播放短上行双音，失败播放低音；播放异步进行，音频设备不可用会明确报错。

## 存档数据体编辑

只读诊断:

```powershell
.\Binaries\Release\BlazblueTests.exe --databody-probe "<游戏目录>" "<存档副本>"
```

只读性能诊断:

```powershell
.\Binaries\Release\BlazblueTests.exe --editor-performance "<游戏目录>" "<存档副本>"
```
