# 已克隆的参考项目

本文记录导入前的参考核对。现在 DKB 已导入为主项目，具体来源及改动以 `config/upstream.json`、`THIRD_PARTY_NOTICES.md` 和 `port-status.md` 为准。

核对日期：2026-11-05。实际提交固定在 `config/references.lock.json`。克隆位于 `work/references`，使用浅克隆和稀疏检出，仅展开需要阅读的源码目录，其他文件可按需取回。没有运行参考仓库的安装脚本或游戏程序。

| 仓库 | 性质 | 本项目借鉴的内容 |
| --- | --- | --- |
| [DKB Recomp](https://github.com/Lucas-blip-png/dkb-pc-recomp) | NS2《Donkey Kong Bananza》静态重编译 | 模块布局、函数发现和分阶段验证；README 说尚不可完整游玩 |
| [Pokopia Recomp](https://github.com/drakolordx7/pokopia-recomp) | NS2《Pokémon Pokopia》Windows 静态重编译 | 游戏版本配置与共享翻译器/运行时分离；其可玩性是作者陈述，本地未验证 |
| [Eglantine](https://github.com/Djihads80/Eglantine) | NS2 模拟器，不是独立游戏重编译 | NUP 虚拟文件系统、更新包选择、Ampere 着色器与服务适配的工作范围 |
| [NUP](https://github.com/a55uka/nup) | 未加密 NX1/2 转储的容器规范 | 按公开 SPEC 实现只读解析、TOC/文件哈希和边界校验 |

## 已阅读的关键文件

- DKB：`recomp/layout.py`、`recomp/module.py`、`recomp/gen.py`。其运行时自行链接模块，因此省略 rtld；这不是对《封印战记》也可以省略 rtld 的证明。
- Pokopia：`recomp/games/pkp/config.py`、`recomp/nxrecomp/config.py`。其中版本、模块基址、hook 地址属于另一款游戏，不能照搬。
- Eglantine：`src/core/loader/nup.cpp`、`src/core/file_sys/nup_container.cpp`。模拟器加载能力不等于独立 AOT 能力。
- NUP：`SPEC.md`。版本字段按不透明 NCM 数值保存，没有擅自把 `196608` 转成游戏显示版本。
- NSO 格式：[Switchbrew NSO0](https://switchbrew.org/wiki/NSO0)。当前文件的段压缩为 LZ4；ZBIC 尚未实现并明确拒绝。

## 如何影响我们的设计

1. 容器与翻译器分离：不复制 47.5 GB ROM，只读取必要程序文件。
2. 每个版本固定模块 SHA-256，地址型验证配置只对匹配版本生效。
3. 指令翻译、宿主运行时、游戏适配独立推进；小函数测试通过不标记成游戏可玩。
4. 未知指令直接拒绝，不生成“假成功”的系统服务或空白图形后端。
5. 原创源码与外部参考分开，派生游戏代码和参考仓库均不加入本项目 Git。

## 许可证与来源

DKB 整体为 GPL-3.0-or-later，部分继承文件保留 MIT；Pokopia 为 GPL-3.0，具体文件可能声明 or-later；Eglantine 为 GPL-3.0-or-later；NUP 为 MIT。当前主项目已导入 DKB 并保留全部原始许可证及第三方声明，其余仓库仍仅作为参考。
