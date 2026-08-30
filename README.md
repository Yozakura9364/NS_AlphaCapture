# NS Alpha Capture

FFXIV（DX11 + ReShade）透明底截图插件。按一个热键，直接输出带透明通道的 RGBA PNG，无需后期抠图。

## 原理

FF14 的大量半透明效果（头发边缘、纱、睫毛等）不是真 alpha 混合，而是 **ordered-dither 网格**：用棋盘格镂空模拟半透明。常规截图对这类像素无能为力——要么带格子，要么整个丢掉。

本插件的做法：

1. **网格学习**：addon 在 shader 创建时反汇编并记录所有含 `discard` 的像素 shader；运行时只检查这些 shader 是否使用 4x4 `A8_UNORM` 网格纹理，命中的 draw 记为 dither 网格签名。
2. **同帧双底重放**：按下捕获键后的那一帧，普通不透明 draw 和命中的 dither draw 会被**按原顺序实际执行两次**，分别重放到插件私有的纯黑底和纯白底 `RGBA32F` 渲染目标上（dither draw 用连续 alpha 替代原网格）。
3. **差值重建 alpha**：同一像素在黑底和白底上的颜色差，就是它的真实透明度——`alpha = 1 - (white - black) / (白底理论值 - 黑底理论值)`，逐通道解出干净的 RGBA。

整个过程在同一帧内完成，不读深度缓冲、不替换游戏 shader、不做统计估算，镜片（透镜反光）也在同一条链路里处理。

## 输出

- `*_Final.png` — 透明底 RGBA（默认输出）
- `*_Black.png` / `*_White.png` — 同帧真实渲染的黑底/白底中间结果（默认关闭，用于检查链路）

## 安装

需要 ReShade 6.3.0+（含 Add-on 支持）。

1. `NS_AlphaCapture.addon64` 和 `NS_AlphaCapture.ini` 放进 ReShade 插件搜索路径。
2. `NS_AlphaBase.fx`、`NS_VFXCapture.fx` 放进 `reshade-shaders\NS\`。
3. 进游戏，按 `Ctrl+Shift+F10` 捕获（`Ctrl+Shift+F9` 重读配置）。

## 配置

`NS_AlphaCapture.ini` 主要项（设置面板可改，改完 `Ctrl+Shift+F9` 生效）：

| 项 | 默认 | 说明 |
|---|---|---|
| `OutputTransparent` | 1 | 输出透明图 |
| `OutputBlack/White` | 0 | 输出黑底/白底图 |
| `SaveGameImage` | 1 | 同时保存原游戏帧 |
| `AutoMatch` | 1 | 自动网格签名匹配（关闭则回退 hash 表） |
| `LensCapture` | 1 | 镜片隔离捕获 |
| `OutputDirectory` / `FileNaming` | 空 | 留空自动继承 ReShade/GShade 的截图路径与命名 |

日志写入插件同目录 `NS_AlphaCapture.log`。

## 许可证

MIT，见 `LICENSE.txt`。
