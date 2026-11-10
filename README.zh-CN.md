<p align="center">
  <img src="assets/ame-logo.svg" alt="AME." width="480">
</p>

# AMExclusive

### Make Apple Music on Windows a “Pro” Hi-Fi Player

[English](README.md)

[官网](https://amexclusive.tgwgroup.ltd/zh/) · [使用文档](https://amexclusive.tgwgroup.ltd/zh/docs/)

## 描述

### 本 Fork 的改动

**0.3.2：修复新版 Apple Music 播放状态监听**

0.3.1 在 Apple Music 1.7 上仍使用旧 IPlayer/INowPlayingItem 接口，可能报 E_NOINTERFACE，使暂停标记无法随实际播放状态更新，切到新歌后停在已绑定但未启动输出的状态。现移植上游 1.8 的新旧接口探测，保留 96 kHz 音源策略。已在本机包 1.1792.39942.0 确认新版监听连接成功；实际听感验证另行确认。

**0.3.1：连续播放恢复与设置自动应用**

- 自然曲尾后，重新启用仍有效的同一个 Apple 解码器；新曲已有替代解码器时优先使用新曲。
- 暂停后选择其他歌曲，清除旧暂停/恢复任务；单纯选中行仍需实际播放信号才会出声。
- 曲尾排空等待按剩余音频时长加 3 秒余量限时，避免下一首永久等待；用户暂停时不计时。
- 修改音源上限、输出设备、独占模式、硬件缓冲或兼容转换后，应用完成再自动重开当前曲目。连续修改会合并；暂停时等待手动恢复，期间换歌则取消旧曲重开。

重开会从歌曲开头播放，通过上一首/下一首往返并核对完整曲目信息实现。Apple 禁止队列导航时（包括部分单曲队列）可能无法自动重开，设置仍保留，可手动重新打开歌曲；结果记录在 `am-exclusive-recovery.log`。本次未合并上游 1.8 的其他改动。已通过离线回归，安装后的实际 Apple Music/光纤播放仍需验证。

**0.3.0：按设备上限选择 Apple 已有的无损音源**

在音量按钮旁的“输出设备”菜单底部，可选择 **Apple 无损音源上限：48 / 96 / 192 kHz**，默认 **96 kHz**。Apple Music 原生播放设置保持“高解析度无损”即可。设置保存在本机，播放中修改后会自动尝试重开当前歌曲，暂停时等待恢复播放。

选择 96 kHz 时，有 96 就选 96，有 88.2 就选 88.2；如果某首歌只有 176.4/192 与 48 kHz 两个无损版本，就选 Apple 提供的 48 kHz 版本。这是在下载/解码之前选择已有音源，不进行本机重采样，不需要开启“兼容格式转换”。原生 48 kHz 无损档位仍保留 48 kHz 上限。

本功能需要 Apple 提供符合上限的无损版本，不能让服务器按需生成新版本。存在无损但所有候选都超过上限时，会拒绝这些超限候选并记录日志；仅有 AAC 的歌曲保持原生播放策略。自动重开成功后当前歌曲使用新上限；无法重开时，下一次打开歌曲生效。

已用实际 Apple 候选和生产筛选回调核对：96+48 在 96 上限下选择 96，176.4+48 选择 48；192 上限仍可选择 176.4。通过测试探针在独立临时过滤器中执行，未替换正在使用的播放器选择。

- 合并 [lucap9056 的输出设备选择器](https://github.com/lucap9056/AMExclusive/tree/output-device-selector)，可在音量按钮旁选择共享或独占输出设备。
- 独占模式只在播放时占用设备。暂停后排空已经提交的短硬件缓冲并释放设备，浏览器和其他播放器可以正常使用；恢复播放时重新打开所选设备，保留音乐队列和转换器状态。
- 其他程序仍独占设备时显示等待并自动重试，恢复时不再依赖新的解码块，避免已缓存队列满时卡住。暂停后更改输出设备，恢复播放也会重新查询所选/默认设备。
- 多个播放后台进程同时存活时，按系统验证的进程创建时间选定所有者，较旧进程保持待命连接，不再互相断线、重连争抢。
- 新增 Apple Music 包 `1.1792.39682.0` 的 CoreMedia 最高无损锁定适配；保留原有版本及函数指纹检查，未知二进制仍不应用私有地址 Hook。
- 修复安装验证依赖账户显示名称反查 SID 的问题，直接验证任务 XML 中的 SID，避免计算机名与账户名重名或名称解析失败。

构建与回归测试说明见 [mod/tests/README.md](mod/tests/README.md)。新版本适配不代表全部 Apple Music 私有 ABI 或所有设备已经验证；切换到不能承载当前转换器格式的设备时，需要重新打开歌曲建立新音频链路。

AMExclusive（AME）是一个专为 Windows 版 Apple Music 设计的开源项目，旨在为 Apple Music 提供 WASAPI 独占音频输出以及强制最高音频规格输出的能力，并以此将 Apple Music 优化为一款在受支持硬件与配置下可实现 bit-perfect 输出的“专业”Hi-Fi 音乐播放器。

## 开发与测试环境

- Windows 11 x64
- Windows 版 Apple Music 文件版本 `1.6.4.90`
- 已验证包/框架版本 `1.1540.23042.0` x64

本 Fork 另外核对了包 `1.1792.39682.0` x64 的 CoreMedia 过滤器布局，完成严格过滤器构造与原生过滤树调用测试。

本项目仅在上述开发与测试版本中完成验证，不保证在其他 Apple Music 版本上的运行效果。

## 特性

1. **WASAPI 独占输出**  
   以独占模式直接向当前 Windows 输出设备提交音频，绕过常规共享模式下的 Windows 音频混音路径。

2. **强制最高音频规格**  
   去除 Apple Music 原生“低带宽自动降低音频规格”的逻辑，在最高可用无损规格尚未就绪时继续加载等待，而不是自动回退到更低音频规格。

3. **Bit-perfect 输出**  
   在受支持的播放路径中保持源音频规格，并将整数 PCM 直接提交给 WASAPI；当音源、输出设备与目标格式匹配时，可实现 bit-perfect 播放。

4. **可选采样率与位深转换以兼容设备**

   开启兼容开关后，先尝试原采样率的无损输出；设备不支持时，允许升／降采样、位深转换及 float32 输出，保留 WASAPI 独占和现有音质锁定。例如，44.1 kHz/24-bit 和 96 kHz/24-bit 音源都可转换至仅支持 48 kHz/16-bit 的设备。每个输出格式都必须通过驱动的独占支持检查与初始化，不回退到共享模式。探测覆盖常见整数容器、float32、WAVEFORMATEX／EXTENSIBLE 格式声明；设备的混音采样率也会作为候选，不能直接当作独占支持的证据。

   在 44.1 kHz 及以上（音源更低时以原采样率为准），优先保留音源精度，再尝试 float32／24-bit、16-bit；同一精度组先原采样率，再从最近的更高采样率尝试，最后从高到低尝试更低采样率。更低带宽的输出作为最后选择。升采样使用带限插值抑制频谱镜像，降采样抑制混叠；降低位深使用 TPDF 抖动和饱和舍入。兼容模式也允许本地 int32 音频通过 Apple 解码后的 float32 数据播放，不宣称恢复原始 int32 的 bit-perfect。转换后的输出会明确标注。关闭开关时保留现有无损格式策略与 PCM 路径。

## 推荐设置

在 Apple Music 的 **设置 > 播放** 中：

- 关闭 **交叉渐入渐出**。
- 关闭 **声音增强器**。
- 关闭 **音量平衡**。
- 开启 **无损音频**。
- 将无损音频下方的两项音频规格均设为 **高解析度无损（ALAC）**。
- 关闭 **杜比全景声**。
- 开启 **独占模式**。
- 在 **硬件缓冲区** 中输入 `1`–`100` 毫秒的整数并点击 **确定**。该数值会在下一首歌曲打开独占输出时应用。默认值为 `20` 毫秒，遇到播放问题时可尝试增加数值。
- 设备无法播放原规格时，可开启 **设备不支持原规格时允许兼容格式转换**，下次打开歌曲时生效。
- 关闭 **空间音效**。

使用独立 DAC 和放大器时，将 Apple Music 应用内音量、Windows 输出音量和 DAC 输出音量保持在最大值，只通过放大器调整实际播放音量。使用 DAC/AMP 一体机时，将 Apple Music 应用内音量和 Windows 输出音量保持在最大值，通过 DAC/AMP 一体机的硬件音量控制调整实际播放音量。首次应用这些设置前，请先降低放大器或 DAC/AMP 一体机的硬件音量，避免突然出现过高声压。

## 编译与安装

### 使用 GitHub Actions 编译

1. 打开 [Actions → Build Setup](https://github.com/AntiOblivionis/AMExclusive/actions/workflows/build-setup.yml)。编译自己的修改时，先 Fork 仓库，并在自己的仓库中启用 Actions。
2. 点击 **Run workflow**，选择分支并运行。
3. 成功后，在该次运行的 **Artifacts** 中下载 **AMExclusive-Setup-x64**，解压得到 `AMExclusive-Setup.exe`。

工作流仅手动触发，编译 Windows x64 Release 并检查安装器内嵌文件，不自动发布 Release。构建机不需要安装 Apple Music：工作流会从 NuGet 下载固定版本的 Microsoft Windows App SDK / WinUI 元数据包，并在编译前使用固定 SHA-256 校验 `Microsoft.UI.Xaml.winmd`。安装 AMExclusive 的电脑仍需安装 Apple Music。构建产物保留 14 天，下载需登录 GitHub。

### 本地编译

从源码编译需要 Visual Studio 2022 或更新版本的 MSVC Build Tools、Windows 11 SDK、CMake 3.24 或更新版本和 PowerShell。默认情况下，构建脚本会从本机安装的 Windows 版 Apple Music 中读取所需 WinUI 元数据：

```powershell
cd mod
.\scripts\Build-Mod.ps1 -Configuration Release
```

构建机没有安装 Apple Music 时，也可以先准备独立元数据目录再编译：

```powershell
cd mod
.\scripts\Get-WinUIMetadata.ps1 -OutputDirectory .\build\winui-metadata
.\scripts\Build-Mod.ps1 -Configuration Release -WinUIMetadataDirectory .\build\winui-metadata
```

安装器输出位置：

```text
mod\build\dist\AMExclusive-Setup.exe
```

安装或升级时，运行 `AMExclusive-Setup.exe` 并选择 `1`。随后打开 Apple Music，进入 **设置 > 播放**，开启 **独占模式**。

卸载时，运行同一个 Setup 并选择 `2`，或在 **Windows 设置 > 应用 > 已安装的应用** 中卸载 **AMExclusive**。

## 致谢

感谢 [LINUX DO 社区](https://linux.do/) 在项目开发与测试过程中提供的交流、支持与反馈。

本项目使用了以下开源项目：

- [MinHook](https://github.com/TsudaKageyu/minhook) — 提供 Windows x64/x86 API Hook 基础能力。
- [rigtorp/SPSCQueue](https://github.com/rigtorp/SPSCQueue) — 提供音频实时路径使用的单生产者、单消费者有界队列。

感谢上述项目及其贡献者。完整版本、版权和许可证信息见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 协议与声明

项目自行编写的源码使用 [MIT License](LICENSE)。第三方组件继续遵循其各自的上游许可证，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

AMExclusive 是一个独立且非官方的开源项目，与 Apple Inc. 没有任何关联，也未获得其许可或背书。Apple Music 是 Apple Inc. 的商标。

本项目不保证在不同软件、硬件、驱动程序或音频设备条件下能够正常运行。使用本项目可能造成不可预知的后果，使用者应自行承担相关风险。

本项目不分发 Apple 软件、Apple 二进制文件、解密后的媒体或 DRM-free 参考文件，也不提供用于绕过 DRM 保护的功能。
