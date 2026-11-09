# redroid-hwenc

**语言:** [English](README.md) | [Español](README.es.md) | 中文

> 本项目的官方语言是**英语**。本文件仅为方便阅读而提供，翻译可能不完全准确或未及时更新——
> 如有疑问，请以 [README.md](README.md)（英文版）为准。

为 [redroid](https://github.com/remote-android/redroid-doc) 提供硬件视频编码支持（H.264/H.265，基于 VA-API）。

> **只想直接用上这个功能？** 这个项目的代码正是 [**redroid-forge**](https://github.com/fogelmanjg/redroid-forge)
> 的 `hwenc`/`hwdec` 模块背后的实现——redroid-forge 是构建在这之上的完整 Android 容器管理工具
> （GApps、ARM 翻译层、WiFi、备份、真正的操作界面）。如果你只是想要一个能用的实例，直接去那边
> 就好——这个仓库是底层逆向工程真正发生和被记录的地方，按阶段递进，面向想理解或在此基础上
> 继续构建的人，而不只是想用现成结果的人。

## 问题所在

BlueStacks、Nox、MuMu 在 Linux 桌面上都没有真正的替代品。[redroid](https://github.com/remote-android/redroid-doc)
是最接近的方案——Android 运行在 Docker 容器中，渲染部分有 GPU 直通加速——但屏幕串流
（scrcpy，或者任何 Android 应用内部的视频播放）依然只能依赖**纯软件编码器**
（`OMX.google.h264.encoder` / `c2.android.avc.encoder`）。`gpuMode=host` 只加速渲染
（通过 Mesa 的 OpenGL/Vulkan），从不涉及编码器。

这个需求在官方仓库里**从 2022 年就一直开着**，至今没有人解决并公开发布过：

- [remote-android/redroid-doc#126](https://github.com/remote-android/redroid-doc/issues/126) — AMD VA-API（OMX/Codec2）
- [remote-android/redroid-doc#172](https://github.com/remote-android/redroid-doc/issues/172) — 同样的需求，AMD
- [remote-android/redroid-doc#168](https://github.com/remote-android/redroid-doc/issues/168) — 同样的需求，Intel
- [remote-android/redroid-doc#535](https://github.com/remote-android/redroid-doc/issues/535) — H.265 需求，2025 年 9 月的留言至今无人回应

项目维护者（`zhouziyang`）的回答一直是同一句话：VA-API 驱动已经打包进 redroid 了，但真正
调用它来编码的 Codec2/OMX 组件，得社区自己去写。四年过去了，没有人把它写完并发布出来。

## 为什么是现在

这不是不可能的事——只是"真正关心这个具体问题"和"愿意深入 AOSP/Codec2/VA-API"这两者
的交集太少见了。那些 issue 里大部分人是在求这个功能，而不是愿意亲自去写的人。这个仓库是在
公开场合尝试解决它，按难度递增分成几个阶段，并随进度记录过程（见 [DEVLOG.md](DEVLOG.md)，英文）。

## 路线图（按难度分级，不按时间）

每个阶段都假设前一个阶段已经完成。⭐ 标记的是最关键、杠杆最大的检查点。这份列表记录的是
当初的预期和最终的结果——真正遇到的 bug、走过的弯路、每个结论是怎么得出的，见
[DEVLOG.md](DEVLOG.md)（按会话逐次记录，英文），以及（如果存在的话）各阶段自己子目录下的
README，链接见下方。

- [x] **阶段 0 — 纯调研。** 确认编码器选择是按能力匹配的，不是写死的，而且这台硬件上
      确实存在可用的 VA-API 编码 entrypoint。
- [x] **阶段 1 — 阅读与梳理。** 梳理了 Codec2 组件的结构，以及 VA-API 编码部分的 API 接口。
- [x] **阶段 2 — 独立的原生原型。** 宿主机上的独立程序通过 VA-API 编码出真正的 H.264。
      → [tier2-vaapi-encode/](tier2-vaapi-encode/)
- [x] **阶段 3 — ⭐ 决定成败的关键验证。** 确认了 gralloc buffer 导出的 dma-buf 能在真实的
      redroid 容器里被 VA-API 零拷贝导入——在三块不同的 GPU 上都验证了——这正是四年来
      upstream issue 里从没人回答过的问题。→ [tier3-dmabuf-import/README.md](tier3-dmabuf-import/README.md)
- [x] **阶段 4 — Android 端的 Codec2 骨架。** 一个能被 Android 通过 `dumpsys media.c2` 识别
      并列出的组件，暂时还没接入真实的编码逻辑。
      → [tier4-codec2-skeleton/README.md](tier4-codec2-skeleton/README.md)
- [x] **阶段 5 — ⭐ 真正的整合，这才是最终目标。** 一个真实的、未经修改的应用（`scrcpy`）
      通过 Android 框架的正规路径，录制出正确的硬件编码 H.264——已在 AMD/Intel 的 3 块 GPU 上
      验证（见下方"硬件兼容性"）。后续追加：真正的 P 帧支持（5.14，在 Intel 上有问题）、
      面向应用的厂商无关硬件解码（5.15），以及基于 libavcodec 的第二套编码后端，修复了 Intel
      的 P 帧 bug（5.16）。→ [tier5-vaapi-daemon/README.md](tier5-vaapi-daemon/README.md)
- [x] **阶段 6（取决于阶段 0 的结果）。** 结果证明没必要——阶段 0 已经确认编码器选择是按
      能力匹配的，所以 `scrcpy` 会自动识别并使用新的编码器。
- [x] **阶段 7 — ⭐ 硬件视频解码，第四块 GPU（NVIDIA）。** 通过 VA-API 实现的 NVDEC 解码，
      逐位精确，完整接入了 daemon，并对接了真实的 `c2.hardware.decoder.h264` Codec2 组件
      （见下方"硬件兼容性"）。**下一步**：真机上的 `MediaCodec` 测试，和阶段 5 的编码部分
      当初需要的是同一个里程碑。→ [tier6-vaapi-decode/README.md](tier6-vaapi-decode/README.md)、
      [tier5-vaapi-daemon/README.md](tier5-vaapi-daemon/README.md)
- [x] **阶段 8 — Android 11（HIDL Codec2）。** 阶段 4–7 都建立在 AIDL 版本的 Codec2 之上，
      而这套 HAL 在 Android 14 之前根本不存在——直接查阅 AOSP 源码（android-11.0.0_r48、
      12.0.0_r34、13.0.0_r83）确认过这一点。这个阶段把同一个组件移植到 HIDL，再加上一个
      内核层面的 `libion.so` 修复（这些老版本 AOSP 在任何现代主机内核上都需要这个修复），
      就能实现同样的硬件编码——**在 Android 11 上，AMD（Ryzen 5700G/Renoir）和 Intel（Iris Xe）
      都已端到端验证通过**。Intel 需要用 `ffmpeg` 编码后端（5.16）才能修复默认后端上的一个
      码流损坏 bug（`Overread SPS by N bits`）——09/11/2026 确认，这其实和 5.14 那个 Intel
      专属的 P 帧 bug 是同一类问题，并不是最初怀疑的另一个独立问题。**Android 12/13 被默认
      认为可以同样方式工作，但没有单独测试过**——同样的 AOSP 源码缺口，同样的修复方式，
      组件本身也没有任何版本专属的逻辑，所以没理由认为它们在那两个版本上会表现不同，
      只是还没在真实的 12/13 硬件上跑过。如果你实际跑了，发现有什么不对，那正是这个假设
      可能出错的地方——开一个 issue 说明你遇到的情况，会被针对性地查一遍。HIDL 解码那一侧
      （`VaapiDecComponent`）同样还没被实际跑过，情况一样。
      → [tier8-android11-13-hidl/README.md](tier8-android11-13-hidl/README.md)

哪怕最终没能走到最后一步，每个阶段单独拿出来都是一份可以发布的贡献——这些内容此前从没
有人整理记录下来过。

## 硬件兼容性

真实硬件上的真实结果，不是推测。"完整流程可用"指的是一个真实的、未经修改的应用，通过
Android 框架的正规路径录制出正确的视频——每块 GPU 具体的修复方式，见各阶段自己的 README
和 DEVLOG.md。

| GPU | 架构 | 状态 | 备注 |
|---|---|---|---|
| AMD Renoir（Ryzen 核显） | GFX9 | **完整流程可用** | 参考实现。P 帧（5.14）可用；`ffmpeg` 后端（5.16）也在这块 GPU 上验证过。 |
| AMD Radeon RX 480（Polaris，独显） | GFX8 | **完整流程可用** | 这块 GPU/格式组合完全没有 DRM modifier 可用——需要走 VA-API 旧版的 pre-modifier 导入路径。P 帧（5.14）可用。**这块 GPU 要避免使用 `ffmpeg` 后端（5.16）**：验证过程中出现过真实的 GPU 挂起（超时、完整复位、还有一次需要重启主机才能恢复）——请继续使用默认的 `legacy` 后端。 |
| Intel Iris Xe（TigerLake-LP 核显） | Gen12 | **完整流程可用** | 和 AMD 用的是同一份未经修改的厂商镜像，不需要重新编译。有一个厂商特有的 tiling bug，已经在 daemon 里修复。默认（`legacy`）后端下的 P 帧是**有问题的**（5.14：约 60% 的帧解码失败）——这块 GPU 要用 `ffmpeg` 后端（5.16）才能让 P 帧正常工作。 |

面向应用的厂商无关硬件**解码**（5.15，`hwdec.c`——H.264/HEVC/VP9，和上面的编码表、以及
阶段 7 里 NVIDIA 专属的解码都是分开的）：已在上面三块 GPU 上确认与软件解码逐位一致，见
[tier5-vaapi-daemon/README.md](tier5-vaapi-daemon/README.md#515-vendor-agnostic-hardware-decode-hwdecc)。

这份列表只有 3 台机器，是因为这就是这个项目目前能接触到的硬件范围，不是说覆盖面已经完整。
如果你有其他硬件、愿意帮忙补充，见下方"参与贡献"——针对这张表提一个带着你自己验证结果的
PR，正是这个项目现在最需要的那种贡献。

### 解码（阶段 7，NVIDIA）

这是单独的一张表——解码走的是另一条流水线（`tier5-vaapi-daemon` 的解码后端，通过 VA-API
使用 `nvidia-vaapi-driver`/NVDEC），和上面的编码表是分开的，目前只支持 NVIDIA。

| GPU | 架构 | 状态 | 备注 |
|---|---|---|---|
| GTX 1050 Ti | Pascal | **确认逐位精确** | 通过 PSNR 对比软件解码验证过（`y:inf u:inf v:inf average:inf`），走的是真实的 daemon/Codec2 全流程，不只是独立验证程序。 |
| RTX 4060 | Ada Lovelace | **这里还没跑过** | 这个项目自己的解码代码还没有在这块 GPU 上实际跑过。按理说应该能用（和上面 Pascal 同样的机制），但按照本项目自己"真实结果，不是推测"的标准，在真正跑起来并用 PSNR 验证之前，都算未验证。 |

**为什么编码表里没有 NVIDIA**：`nvidia-vaapi-driver` 只支持解码（`vainfo` 列出的
`VAEntrypointEncSlice` entry 数量为零）——这是机制层面的不匹配，不是成熟度不够。不过 NVIDIA
在加速和编码方面**并没有**被卡住：这两块都在姊妹项目
[**redroid-nvidia**](https://github.com/fogelmanjg/redroid-nvidia) 里，走的是另一套机制
（自研的 Venus-proxy 流水线 + NVENC）——在上表两块 GPU 上都确认可用，RTX 4060 上有一个
已知的、这块 GPU 特有的驱动限制（具体细节见那个仓库）。

## 参与贡献

没有 CLA，没有额外门槛——纯 Apache-2.0。如果你对这个问题感兴趣，直接提 PR 或在 issue 里
留言，比先问"能不能参与"更有价值。

## 许可证

Apache License 2.0——详见 [LICENSE](LICENSE)。特意选择和 AOSP（`frameworks/av`）相同的许可证：
如果将来这里的成果值得往上游合并，不会有法律上的阻碍。
