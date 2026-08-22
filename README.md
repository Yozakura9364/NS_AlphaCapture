# NS Alpha Capture

这是 FFXIV DX11 + ReShade 的透明 RGBA 捕获研究工具。addon 自动识别 ordered-dither 半透明网格，并据此学习主场景颜色 RT。捕获帧中，同一主场景 RT 上的普通 indexed draw 会按原顺序分别重放到私有黑底和白底 `R32G32B32A32_FLOAT` RT；命中的半透明颜色 draw 则使用连续 alpha 重放到两路 RT。帧末由 addon 直接读回两张 RT 并重建 RGBA。用户可以在设置面板选择是否保存黑底图、白底图和透明图；该选择只影响落盘，内部双底渲染始终完整执行。

私有双底链路不读取 Generic Depth，不做 4x4 统计回推，也不替换游戏 shader。黑 RT 从纯黑开始，白 RT 从纯白开始，每个命中的游戏 draw 都实际执行两次，再用两张 RT 的差值重建 RGBA；不存在 `White = Black + (1 - Alpha)` 数学白底。镜片 draw 继续集成在同一黑白回放链中，不再额外导出独立的镜片诊断文件。

## 使用

1. 确认安装目录中有 `NS_AlphaCapture.addon64`、`NS_AlphaCapture.ini`、完整的 `NS_AlphaBase.fx` 和 `NS_VFXCapture.fx`，并在 Shader Toggler 中启用 `ALPHA - Base` 与 `ALPHA - Capture` 两组。需要眼镜输出时再启用默认关闭的 `ALPHA - Lens Base` 与 `ALPHA - Lens Capture`。
2. 启动游戏并进入要捕获的画面。addon 平时只学习网格签名，不会重放颜色 draw；真正的双重颜色重放只发生在捕获帧。
3. 按一次 `Ctrl+Shift+F10`。顶部提示出现后，下一帧会捕获并在帧末自动保存。
4. 文件保存在设置面板显示的截图路径。默认只保存 `*_Final.png`；需要检查双底时再勾选黑底图或白底图。

设置面板提供三个独立输出选项，默认仅启用透明图：

- `*_Final.png`：addon 直接使用真实黑白 RT 差值重建的透明 RGBA。
- `*_Black.png` / `*_White.png`：同一帧由 ReShade 实际渲染的黑底和白底中间结果，可用于检查双底链路；不是由最终 alpha 反推出来的假底图。

例如：

```text
20260818_12-00-00-000_Black.png
20260818_12-00-00-000_White.png
20260818_12-00-00-000_Final.png
```

`Ctrl+Shift+F9` 会重新读取热键、`OutputDirectory` 和输出选择。`OutputDirectory=` 为空时，首次载入会继承 `ReShade.ini` 或 `GShade.ini` 的 `[SCREENSHOT] SavePath`；两者都没有有效设置时使用 Windows“图片”目录。设置面板可以直接编辑并保存截图路径，也可以一键恢复使用 ReShade/GShade 路径。输出选择会立即写入 `OutputBlack`、`OutputWhite`、`OutputTransparent`，至少要保留一种。日志固定保存在 addon 所在目录的 `NS_AlphaCapture.log`（与 `NS_AlphaCapture.addon64` 同目录），不会写入用户截图输出目录；会记录捕获武装、目标 hash、RT 创建、indexed/non-indexed draw 数、主 RT clear 数、像素覆盖统计和失败原因。

`FileNaming` 控制导出文件名模板，默认值为 `%Date%_%TimeHour%-%TimeMinute%-%TimeSecond%-%TimeMS%`。支持 `%Date%`、`%TimeHour%`、`%TimeMinute%`、`%TimeSecond%` 和 `%TimeMS%` 占位符；导出时分别追加 `_Black.png`、`_White.png`、`_Final.png`。设置面板中的“截图文件名”可直接修改并保存，非法 Windows 文件名字符会被替换为下划线。

### 自动网格匹配

`AutoMatch=1` 是默认模式。addon 只在 shader 创建时反汇编一次，记录包含
`discard` 的像素 shader；运行时仅对这些 shader 检查 4x4 `A8_UNORM` 纹理、
深度写入和零颜色写掩码。满足条件的 draw 会记录网格签名，捕获时自动选择同
一网格的颜色 draw，并要求颜色 RT 是已验证的单采样
`R16G16B16A16_FLOAT`，且使用已知的普通或 additive blend。

这样装备变体不再需要逐个追加 shader hash。设置 `AutoMatch=0` 时保留原有
hash/网格表作为回退。非捕获帧不会改写颜色 draw；捕获帧会用连续结果替代
命中的原 ordered-dither 颜色 draw。

### 镜片隔离

`LensCapture=1`（默认）会在普通捕获链中优先匹配已确认的镜片主颜色 draw：
`PS=3361469263`、`first_index=6448`、`index_count=276`。该 pass 的原始
blend 是禁用状态，因此镜片模式使用只写 RGB 的 opaque 回放；普通模式的
alpha/additive blend 白名单不会被放宽。镜片模式仍使用同一 addon、同一私有黑白 RT、
原 DSV 和单帧按键，不启用独立的 `NS_LensCapture` addon。

需要只验证镜片时可将 `LensOnly=1` 临时写入配置；此时捕获帧只回放该镜片 draw。
恢复 `LensOnly=0` 即回到完整的半透明、光效和镜片联合捕获链。将 `LensCapture=0`
则完全跳过镜片专用 SRV 隔离，保留旧的普通捕获行为。

## 捕获边界

自动模式不依赖固定 hash。捕获帧会把已学习主场景 RT 上的普通 indexed draw 按原 blend 和只读 depth/stencil 状态分别镜像到黑白 RT；命中的半透明颜色 draw 使用连续 alpha blend，并把严格的 `GREATER/LESS` 深度比较放宽为 `GREATER_EQUAL/LESS_EQUAL`，从而复用已有深度表面而不复制、清空或污染游戏深度。原 ordered-dither 颜色 draw 被抑制，避免再次覆盖连续结果。帧末的 ReShade 技术只采样 addon 提供的 `NS_ALPHA_CAPTURE_BLACK` 与 `NS_ALPHA_CAPTURE_WHITE`，不再初始化或合成白底。

旧版固定目标仍由 PS hash、VS hash、`index_count`、
`first_index` 和 `vertex_offset` 联合识别，作为 `AutoMatch=0` 的回退：

- `2184442637 / 2160856356`：6492@0、5472@11040
- `3782231024 / 696698206`：2484@6496
- `3401395384 / 696698206`：576@2920
- `1120170840 / 696698206`：576@2920

只有原 RTV 为单采样 `R16G16B16A16_FLOAT`、且绑定了同尺寸单采样 DSV 时才重放。黑 RT 从 `(0,0,0,0)` 开始，白 RT 从 `(1,1,1,1)` 开始；普通场景 draw 保留原 blend，目标透明 pass 使用 `SRC_ALPHA / INV_SRC_ALPHA` 或 `ONE / ONE` 的已验证连续混合。当前完整场景镜像覆盖 `draw_indexed`；非 indexed、compute 或写入其他 RT 的特效仍需运行时样本验证。

## 验证

```text
build.cmd
test.cmd
```

`capture-contract.test.mjs` 覆盖目标识别、单帧按需重放、私有 RGBA32F RT、两种 blend、OM 状态恢复、float staging 读回和 WIC PNG 导出。

## 着色器组规则格式（FormatVersion=2，st-dev 独立开发版）

本目录是分组规则编辑器的独立开发版。规则以“组 + 精确规则”存储，匹配只发生在同一条规则内部：PS、VS、FirstIndex、IndexCount、VertexOffset 全部相等才命中；启用规则必须五字段完整，否则只能作为 `Enabled=0` 的候选存在。

```ini
[General]
FormatVersion=2
AmountGroups=2

[Group0]
Name=主体材质
ToggleKey=0
IsActiveAtStartup=True

[Group0_Rules]
AmountRules=1
Rule0=1|2184442637|2160856356|0|6492|0
Rule0Name=可选的规则说明
```

- `RuleK=Enabled|PS|VS|FirstIndex|IndexCount|VertexOffset`，全部为十进制数值；规则名单独存于 `RuleKName=`，可容纳任意字符。
- 组的运行时开关状态不落盘；`IsActiveAtStartup` 只决定启动时的启用态。
- 保存永远是整文件重写并先移除全部 `GroupN*` 分节，删除组或规则不会留下残留。

### v1 → v2 迁移与回滚

- 加载时若检测到旧格式（有 `AmountGroups` 且无 `FormatVersion=2`），addon 会读取旧的 `[GroupN]`、`[GroupN_PixelShaders]`、`[GroupN_VertexShaders]`、`[GroupN_DrawRules]`，迁移为 v2 后重写 INI，并移除上述旧分节，旧 addon 不会再读到宽匹配 hash。
- 迁移前仅在 `NS_AlphaCapture.ini.formatv1.bak` 不存在时创建一次备份，之后绝不覆盖。
- 旧格式中只有 hash、没有完整 draw 规则的内容会迁移为 `Enabled=0` 候选，不会自动启用。
- 回滚：删除当前 INI，把 `NS_AlphaCapture.ini.formatv1.bak` 改名还原，并换回配套的旧 addon 二进制；v2 INI 与旧 addon 不兼容，不能混用。
