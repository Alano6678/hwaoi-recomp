# HWAoI Recomp — 封印战记

《塞尔达无双：封印战记》（Hyrule Warriors: Age of Imprisonment）的实验性 Windows 原生重编译项目，基于 [DKB Recomp](https://github.com/Lucas-blip-png/dkb-pc-recomp)。使用静态重编译生成器、C/C++ 运行时和 Vulkan 图形后端，逐步适配系统服务、资源加载及 NVN2 着色器。

**已完成全量编译、链接，并渲染出人物、草地和遗迹的真实 3D 场景；尚未验证战斗可玩。** 当前帧率低，光照、稳定性和退出流程仍需完善。本仓库提供研究源码，没有可直接游玩的发行版。

Experimental native Windows recompilation of **Hyrule Warriors: Age of Imprisonment**, adapted from DKB Recomp. Full compilation/linking and an actual 3D scene have been observed. Combat, responsive gameplay, rendering correctness and stable performance remain unverified. **Not ready to play. No game assets or game-derived binaries are included.**

## 当前进度 · 2026-11-05

| 阶段 | 已验证结果 | 限制 |
| --- | --- | --- |
| 输入校验 | 游戏 ID、程序模块 SHA-256、NSO 段摘要检查 | 只接受当前精确匹配的输入配置 |
| 静态生成 | 3 个模块、142,393 个候选函数、362 个 C 分块 | 生成成功不代表所有指令和可达路径正确 |
| 编译与链接 | 完整 Windows x64 EXE 已构建，加载探针强未解析导入为 0 | 游戏派生 C、目标文件和 EXE 仅留在本地 |
| 启动与界面 | 已进入 `nnMain`，显示中文设置及自动保存说明 | 未验证完整游戏流程 |
| 3D 渲染 | 已显示人物、植被、草地和遗迹 | 光照/阴影表现和帧率仍有问题 |
| 输入与存档 | 交互启动支持键盘/手柄，已识别 Xbox 手柄，使用独立本地存档路径 | 未验证战斗控制及存档往返正确性 |
| 回归验证 | 最新 HWAoI 测试集 79 项通过 | 自动测试不能替代游戏流程验证 |

最近一次完成的限时验证为 `hang-21`：开启 Vulkan 校验后再次显示 3D 场景，完整日志的致命错误、指令转换失败和 Vulkan 校验错误均为 **0**。探针在 180 秒时请求关闭，最终记录超时退出码 `124`；这不是正常退出或长期稳定性通过。最后一个测量区间为 39 帧 / 54.2 秒，新着色器缓存下的这次运行不能当作稳定帧率基准。

最近修复包括 F16 纹理采样/收集的半精度打包、D32F/S8 深度模板复制、受限 RGBA8 比较纹理转换、存储图像读取能力声明，以及精确限定调用点的资源分配重试和虚拟核心调度。未支持的模式继续明确报错。

下一步优先处理 CPU 回放与加载性能、光照/阴影正确性、交互操作和战斗流程，以及关闭窗口后的退出收尾。里程碑和测试边界见 [进度说明](docs/hwaoi/port-status.md)，逐轮诊断记录见 [工作记录](docs/hwaoi/current-work.md)。原始报告、截图和着色器输入位于本地忽略目录，不随源码发布。

## 输入与构建

当前目标为 title ID `04003F40232DE000`、NCM version `196608`。NCM version 是内部值，**不能据此推断游戏显示版本**。入口会检查精确模块摘要，不支持任意版本。原始 NUP 只读。

以下为现有开发环境的流程，需自行准备合法取得且匹配的 ExeFS、NUP 及运行时依赖；不是自动安装器。默认 ExeFS 目录为 `work/game/exefs`，更换时将 `--exefs <目录>` 放在子命令前。Python 仅用于生成和构建工具，游戏由编译后的 C/C++ 运行时执行。

开发环境使用 Windows x64、Python、llvm-mingw/Clang、Ninja、.NET 着色器转换工具及 Vulkan。Python 依赖见 [requirements-hwaoi.txt](tools/requirements-hwaoi.txt)。复用已有工具；本项目的本地环境约定要求新下载的编译器/SDK 安装在 `F:\工具名称`。下列依赖目录应替换为自己的实际路径。

```powershell
# 校验输入并检查链接候选
python tools/hwaoi.py verify
python tools/hwaoi.py audit

# 全量生成；默认不带 --limit 0 时只生成各模块前 256 个函数
python tools/hwaoi.py generate --limit 0 --out work/generated/hwaoi196608-full --jobs 2

# 首次准备资源缓存；已有有效缓存时无需重做，路径仅为示例
python tools/hwaoi.py prepare-romfs --nup 'E:\Games\HyruleWarriors.nup'

# 完整构建，复用未变化的目标文件
python tools/hwaoi.py build --romfs work/game/romfs --deps F:/TotKDependencies/runtime/deps --jobs 4

# 显式实验启动：实时输入、本地存档和独立日志，无自动运行时限
python tools/hwaoi.py launch
```

构建输出为 `work/build/hwaoi196608/hwaoi.exe`。直接运行的默认启动仍有保护，交互实验请使用 `tools/hwaoi.py launch`。当前启动器和着色器工具配置包含本地开发路径，首次配置需检查 [入口实现](tools/hwaoi.py) 和 [转换器构建工具](tools/build_xlate_hwaoi.py)。上游 `installer/` 和发布脚本尚未适配本游戏，不应作为安装说明执行。

## 验证

```powershell
python tools/check_content.py
python -m unittest discover -s tests/hwaoi -v

# 探针输出目录必须是新目录；完整探针配置见各子命令 --help
python tools/hwaoi_probe.py selftest --out work/reports/selftest-new
python tools/hwaoi_probe.py probe-romfs --out work/reports/romfs-new
python tools/hwaoi_probe.py probe-loader --out work/reports/loader-new
```

部分原生/着色器测试需要本地编译器和转换器。最近一次 79 项测试使用了已验证的 `HWAOI_TEST_XLATE` 工具；缺少依赖时的跳过不能作为该验证结果。仓库 CI 只检查提交内容及其保护规则，不运行或生成游戏。

## 目录与来源

| 目录 | 用途 |
| --- | --- |
| `recomp/` | 静态重编译生成器 |
| `runtime/` | C/C++ 系统服务、图形、音频和输入运行时 |
| `tools/hwaoi.py` | 本分支的校验、生成、构建和启动入口 |
| `tools/hwrecomp/` | 早期 NUP/NSO 分析原型 |
| `config/` | 精确输入配置和上游来源 |
| `tests/hwaoi/` | 本分支回归测试 |
| `docs/hwaoi/` | 本游戏进度与适配记录 |
| `docs/upstream/` | 导入时的历史说明，不代表本游戏已验证 |
| `work/` | 本地输入、生成代码、构建和诊断产物；Git 忽略 |

上游固定为 DKB Recomp 提交 `42fcd9d78b4b44c2831b68ba21e5807d74a96f45`，见 [config/upstream.json](config/upstream.json)。上游针对 Bananza/TotK 的地址补丁和可玩性结论不能直接用于本游戏。

仓库不包含 ROM、密钥、提取资源、游戏生成代码、游戏着色器载荷或游戏派生二进制；也不要在 Issue/PR 中上传这些内容。参与方式见 [CONTRIBUTING.md](CONTRIBUTING.md)。

整体继承 GPL-3.0-or-later；MIT 和其他第三方文件保留原始许可及署名，详见 [LICENSE](LICENSE)、[LICENSE-MIT](LICENSE-MIT) 和 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。本项目与 Nintendo、Koei Tecmo 无关联，游戏名称和商标属于各自权利人。
