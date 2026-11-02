# Blaz Blue Entropy Effect Trainer

该项目为AI生成，README也为AI辅助生成。

适用于苍翼：混沌效应 Steam版。旧版本地址标记仅用于快速识别；游戏更新后会继续尝试验证当前活动模型，而不是仅因版本变化关闭功能。无法确认对象布局或目标唯一性时仍会安全阻断写入。


## 编译说明

需要 Windows x64、支持 C++20 的 Visual Studio 桌面 C++ 工具、兼容 Windows App SDK 的 Windows SDK，vcpkg，以及 CMake 4.2 或更高版本。依赖为 yaml-cpp 和 LZ4；Windows 音效使用系统多媒体 API，不需要额外音频文件。

vcpkg manifest 安装由 CMake 工具链统一管理。生成的 WinUI 工程仅使用 CMake 解析的库和头文件，不再启用第二套 MSBuild vcpkg 自动集成，因此不要为该工程额外开启 `VcpkgEnableManifest`。SDK 自动初始化源码保留，但同一编译项只加入一次。

全新配置会依次使用 `-DVCPKG_ROOT`、环境变量 `VCPKG_ROOT`，或 `PATH` 中的 `vcpkg` 自动选择工具链，并按 `vcpkg.json` 恢复依赖，不需要旧构建缓存。显式指定的 `CMAKE_TOOLCHAIN_FILE`、依赖安装前缀和包目录不会被自动检测覆盖。

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

清理缓存后也应先重新配置。`yaml-cpp_DIR-NOTFOUND` 等失败缓存可用 `--fresh` 清除；恢复配置不需要复制以前的 `vcpkg_installed` 或填写本项目开发机器的绝对路径。

## 测试与使用说明

```powershell
ctest --test-dir out\build\vs2026-x64 -C Release --output-on-failure
```

覆盖资源运算、活动对象所有权、失效地址、AP 保存标记、中英切换、UTF-8 安全截断、快捷键音效、更新识别、窗口/DPI，以及合成存档的添加、修改、确认取消、过期文件、备份失败和游戏启动时阻断。不会修改正式游戏存档。

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

默认主键盘 `1`、`2`、`3` 分别增加刷新次数、兑换点和 AP，使用界面中的当前增量。快捷键仅在游戏前台且对应目标可写时启用，按住不会连续触发。成功播放短上行双音，失败播放低音；播放异步进行，音频设备不可用会明确报错。

## 更新兼容

识别到配置的游戏进程，或通过其安装目录的稳定元数据确认游戏身份后，会重新验证 Puerts 脚本、类单例及资源所有权链。旧 IL2CPP RVA 不匹配不再是更新后的硬性版本锁。AP 与局内资源分别校验；陌生内存布局、读取失败或候选有歧义仍会阻断对应功能，不能保证任意未来更新均自动兼容。

## 离线存档数据体编辑

主窗口的“离线编辑存档数据体”仅在确认游戏完全退出时可用。先等待 Steam 云同步完成，再选存档槽或浏览文件。编辑器自动查找 Steam 安装，也可指定当前游戏目录；它只读解析当前安装中的角色、策略、品质、潜能和继承前置，不附带或分发提取的游戏资源。

存档与游戏目录的“浏览”使用异步系统选择器，不在 WinUI 界面线程同步打开 Shell 对话框。取消选择保留当前存档和草稿，关闭编辑器会取消尚未完成的选择；选择存档后仍检查游戏已退出，浏览本身不会写入存档。

“查看 / 修改”和“新增”使用独立页签，输入互不覆盖。已有数据体显示在可滚动的列表中，选择后仅编辑两项传承策略及该角色可传出的传承技。技能等级保持原值；HP、MP、自身策略、潜能、天赋、评分及其他数据完全不修改，也不提供全潜能选项。

“新增”页只需选择角色、紫色/金色、传承策略及传承技，创建一个独立编号的数据体。基础属性、天赋和初始潜能使用游戏默认数据；没有自定义 HP/MP、潜能或自身策略配置，也不会添加全潜能。新增所需的默认策略前置在后台校验，角色、DLC 和技能仍须在游戏中可用，本工具不解锁它们。

传承策略必须不同，品质必须在当前游戏配置允许范围内。策略下方只列出继承激活后还需在局内获取的策略；同一括号内的 `/` 表示任选其一，`+` 表示条件需要同时满足。没有额外条件时显示“没有激活条件”，已由另一项有效继承提供的条件不会重复列出。修改传承不改变数据体自身的策略或激活标记。

修改保存严格保留非传承字段、其他数据体及未知字段的原编码，不更新仓库累计分或角色最高分。只有新增会更新唯一编号、最高分与累计分，并以操作前的已解锁容量校验。

点击保存先显示变更摘要。确认后才在存档旁的 `BlazblueTrainerBackups` 目录创建并校验完整 `.bak`，然后生成、刷新和验证临时文件，原子覆盖目标存档并回读。备份失败、文件外部变化、过期确认或游戏启动都会中止覆盖。游戏运行时整个存档编辑区禁用，提交前还会重新检测。

游戏运行状态由主窗口和编辑器共用的后台监测更新，界面不再在每次选择、搜索或刷新时同步枚举全部进程。存档载入、修改准备和备份提交均在后台执行，界面显示当前阶段；打开存档和提交前仍重新检测真实游戏状态，不使用界面快照代替写入安全校验。关闭编辑器会取消尚未提交的后台操作。

策略名称预先缓存，列表批量更新，切换数据体、页签和语言不会重复同步枚举进程。两页的传承选择独立保存，修改不影响新增草稿。性能优化不改变继承规则或备份流程。

恢复时完全退出游戏、等待云同步，将对应 `.bak` 复制回原存档槽文件名；恢复会撤销该备份之后此槽的全部进度，备份不会自动删除。旧版存档应先在当前游戏载入并保存，不支持的结构会明确拒绝写入。

格式及单个数据体操作行为参考 [lzj960515/bb-ee-databody-tool](https://github.com/lzj960515/bb-ee-databody-tool)。本项目独立实现 C++ 编解码与事务，不复制该参考项目未提供授权的源码或数据目录。

只读诊断（请使用存档副本）：

```powershell
.\Binaries\Release\BlazblueTests.exe --databody-probe "<游戏目录>" "<存档副本>"
```

只读性能诊断：

```powershell
.\Binaries\Release\BlazblueTests.exe --editor-performance "<游戏目录>" "<存档副本>"
```

该模式分别报告实时进程检测、界面快照读取、策略计算及存档载入/准备耗时，不保存任何修改。默认回归另覆盖慢速后台监测、多个窗口共享状态和检测失败时继续阻断离线编辑。

当前游戏表格及原游戏 protobuf 解码器用于验证新增记录；离线事务使用副本/合成存档验证。编解码通过不代表所有战斗效果均已逐项实测。
