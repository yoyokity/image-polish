# aa.exe — EEDI2 抗锯齿 + NLMeans 降噪 + Deband 去色带命令行工具（C++ 独立实现）

把 VapourSynth 抗锯齿 + 降噪链完整移植为独立 C++ 程序（源码按模块拆在
`src/`），无需安装 VapourSynth / 插件，直接对图片跑：

```
out/aa.exe -i 输入图片 [-o 输出图片] [--aa] [--denoise 5] [--deband]
```

处理步骤按命令行出现顺序执行：`--aa` 去锯齿（参数固定），`--denoise N` 做
NLMeans 降噪（N 即滤波强度 h），`--deband` 去色带（neo_f3kdb）;都不给则原样
输出。省略 `-o` 时输出为
`<输入名>_output.<原扩展名>`（如 `in.png` → `in_output.png`）。

## 对应的 VapourSynth 脚本

```python
aa_clip = core.std.ShufflePlanes(clip, 0, vs.GRAY)          # 只处理亮度
aa_clip = core.eedi2.EEDI2(aa_clip, field=1, mthresh=10, lthresh=20, vthresh=20,
                           maxd=24, nt=50)                 # 边缘方向插值，高度×2
aa_clip = core.fmtc.resample(aa_clip, w, h, 0, -0.5).std.Transpose()
aa_clip = core.eedi2.EEDI2(aa_clip, field=1, mthresh=10, lthresh=20, vthresh=20,
                           maxd=24, nt=50)
aa_clip = core.fmtc.resample(aa_clip, h, w, 0, -0.5).std.Transpose()
aaed = core.std.ShufflePlanes([aa_clip, clip], [0, 1, 2], vs.YUV)  # 色度保持原图
aaed = core.nlm_cuda.NLMeans(aaed, d=0, wmode=3, h=5)      # 可选降噪（CPU 版用 core.nlm_ispc）
aaed = core.rgvs.Repair(aaed, clip, 2)                      # 钳制到原图邻域
```

流程：亮度平面先做「EEDI2 高度翻倍 → spline36 缩回原尺寸（sy=-0.5）→ 转置」，
再换方向重复一遍。两次方向插值会把斜边的阶梯折线"抹匀"成平滑过渡，即抗锯齿效果；
色度平面保持原图不动；最后 Repair（模式 2）把结果逐像素钳制到原图 3×3 邻域的
[第 2 小, 第 2 大] 区间，防止过度偏离原图。降噪（NLMeans，d=0 只用当前帧、
wmode=3、h 由 `--denoise` 指定）在亮度平面上进行，可按命令行顺序插在链路的
任意位置（本工具与脚本一样只处理亮度，色度不动）。

## 使用方法

```
out/aa.exe -i input.png [-o output.png] [--aa] [--denoise N]
```

- 输入格式：PNG / BMP / TGA / JPG / PGM / PPM / PNM（stb_image 支持的范围），
  灰度（1 通道）或彩色（3 通道）均可。
- 输出格式：由 `-o` 的扩展名决定：`.png`、`.bmp`、`.tga`、`.jpg/.jpeg`,
  以及 `.pgm`（灰度）/ `.ppm` / `.pnm`（彩色）。默认 PNG。省略 `-o` 时自动
  生成为 `<输入名>_output.<输入扩展名>`（沿用输入格式）。JPG 输出质量
  默认 80（`1..100`），用 `--quality N` 调整（仅对 `.jpg/.jpeg` 生效，
  其它格式忽略该参数）。
- 灰度图：直接在整幅上按顺序执行选中的步骤。
- 彩色图：取 BT.601 全范围亮度做处理；色度不走有损的 8 位 YUV 量化，而是把
  「亮度变化量」加到原图 RGB 的三个通道上（等价于原脚本 ShufflePlanes
  的「只换亮度、色度原样保留」，且色度无量化误差）。
- 尺寸要求：宽和高都 ≥ 8（EEDI2 两遍转置后的隐含约束），输出与输入同尺寸。

### 处理步骤

| 参数 | 含义 |
|---|---|
| `--aa` | 去锯齿：EEDI2 链（两遍方向插值 + spline36 重采样）+ Repair 模式 2，参数固定 |
| `--denoise [N]` | NLMeans 降噪，`N` 为滤波强度 `h`（> 0，缺省 5）|
| `--sharpen [s]` | CAS 锐化，`s` 为 sharpness（0.0..1.0，缺省 0.7）|
| `--resize WxH` | 用 spline36 缩放（shift=0 中心对齐）；一侧可省略（`1920x` / `x1080`），另一侧按原宽高比计算 |
| `--dehalo` | 去光晕：FineDehalo（havsfunc 默认参数，固定，不可自定义）|
| `--deband [name=value,...]` | 去色带（neo_f3kdb，sample_mode=2、无 grain）：可用参数 `range`（0..255）、`y`（0..511）、`cbcr`（0..511），缺省 24/72/32；省略的参数沿用缺省、顺序任意（如 `--deband y=40`、`--deband range=16,cbcr=48`）|

步骤按命令行出现顺序依次执行，每一步都在前一步的结果上继续；各步骤均可
省略、可重复、顺序任意。全部省略则输出与输入相同。彩色图缩放时色度用同一
spline36 核同样缩放（先换亮度，色度仍保持原图语义）。

示例：

```
out/aa.exe -i clip.png --aa                       # -> clip_output.png
out/aa.exe -i in.png --denoise 5                  # -> in_output.png
out/aa.exe -i in.png --deband                     # 默认 24,72,32 -> in_output.png
out/aa.exe -i in.png --deband y=40                # 只调亮度阈值，range/cbcr 用默认
out/aa.exe -i in.png --deband range=16,cbcr=48    # 顺序任意
out/aa.exe -i in.png --resize 1920x --aa          # 先缩放到 1920 宽，再去锯齿
out/aa.exe -i in.png --aa --resize x1080 --denoise 5
out/aa.exe -i in.png -o out.png --denoise 5 --aa
```

## 编译

任意 C++17 编译器，无第三方库（stb_image 已内置）。源码按模块拆在 `src/`：
框架在 `src/` 根（main / chain 处理链 / color 颜色辅助 / imageio 图像 I/O /
common / pfor），滤镜算法在 `src/filters/`
（eedi2 / resample / repair / nlmeans / dehalo / cas / deband），构建产物输出到 `out/`：

VS Code 用户直接运行 build 任务（`Ctrl+Shift+B` 或 `Terminal → Run Build Task`），
会自动创建 `out/` 目录再编译。

默认工具链是 LLVM clang（https://llvm.org 或 `winget install LLVM.LLVM`），
以 GNU 模式配合 MinGW-w64 运行库（例如
<https://github.com/niXman/mingw-builds-binaries/releases>，选带 UCRT 的
x86_64 版本，解压后把 `bin` 目录加进 PATH），编译时用 `--target` 指定 target：

```
clang++ --target=x86_64-w64-windows-gnu -O2 -std=c++17 -Isrc -o out/aa.exe \
    src/main.cpp src/chain.cpp src/color.cpp src/imageio.cpp src/filters/*.cpp
```

或者直接用 MinGW-w64 自带的 g++：

```
g++ -O2 -std=c++17 -Isrc -o out/aa.exe src/main.cpp src/chain.cpp \
    src/color.cpp src/imageio.cpp src/filters/*.cpp
```

没有安装 MinGW 时，可用 pip 安装的 ziglang 自带的 clang 编译：

```
python -m pip install ziglang
python -m ziglang c++ -O2 -std=c++17 -Isrc -o out/aa.exe src/main.cpp \
    src/chain.cpp src/color.cpp src/imageio.cpp src/filters/*.cpp
```

## 实现要点

- **EEDI2**（tritical/HolyWu 算法 v0.9.2 的逐行移植）：14 个阶段流水线——
  边缘掩码 → 腐蚀/膨胀/去小水平缝隙 → 方向场计算（5 个候选方向，median 与
  limlut 限幅）→ 方向场滤波/扩张/填缝 → 逐行拼接 → 2× 上采样 → 2× 方向场
  处理 → 晶格插值（噪声阈值 nt8 主搜索 + nt7 回退）→ 后处理。
- **重采样**：fmtconv 约定，`srcPos(o) = (o+0.5)*ratio - 0.5 + shift`
  （像素中心对齐），spline36 核按 `max(ratio,1)` 缩放、按权值总和归一化。
  `--resize` 用同一内核、shift=0 的中心对齐缩放（可分离两趟：转置 → 纵向
  重采样 → 转置）。
- **Repair 模式 2**：对每个像素取参考图 b 的 3×3 邻域排序后的
  `clamp(a, n[1], n[7])`，边界行列直接复制 a。
- **NLMeans**（vs-nlm-ispc / KNLMeansCL drop-in 的移植，参数固定 d=0、a=2、
  s=4、wmode=3、wref=1）：单帧空间降噪（d=0 不用时间邻域）。对 patch 上三角
  半区 12 个偏移逐像素求 `3·Δ²/255²`，经水平 9-tap、竖直 9-tap 滑动盒得到
  窗口距离 `d²`，权重 `w = max(1 - d²·κ, 0)⁸`，其中
  `κ = 255² / (3·h²·81)`，h 由 `--denoise` 提供；聚合 `Σw` 与 `Σw·v` 后输出
  `(wref·maxw·src + Σw·v) / (wref·maxw + Σw)`。
- **FineDehalo**（havsfunc `haf.FineDehalo` 的移植，参数固定为 havsfunc 默认：
  DeHalo_alpha rx=2/ry=2/darkstr=brightstr=1/lowsens=highsens=50/ss=1.5，
  FineDehalo thmi=80/thma=128/thlimi=50/thlima=100、excl=True）：先做
  DeHalo_alpha 去晕（Bicubic(a=1/3,b=1/3) 缩放的模糊晕层与其原图的差、
  1.5× Lanczos 上行采样 min/max 钳制再回落、亮度域混合），再用 Prewitt 边缘
  + 两档亮度阈值（thmi/thma 主边缘、thlimi/thlima 弱边缘）、矩形/菱形形态学
  扩张-腐蚀、3×3 盒卷积构造「排除区」，最后 `MaskedMerge` 只在掩码 255 处
  换成去晕结果。Expr 步骤按参考实现的逐运算 float + round-half-even(rint)
  语义复刻，输出钳位到 [0,255]。
- **CAS**（HolyWu/VapourSynth-CAS 的移植，即 AMD FidelityFX CAS）：3×3 软
  min/max 号出对比度自适应振幅 `amp = sqrt(clamp(min(mn, limit-mx)/mx))`，
  权重 `w = amp · (-1/lerp(16,5,s))`，输出十字滤波
  `((b+d+f+h)·w + e)/(1+4w)`；sharpness `s` 由 `--sharpen` 传入（0.0..1.0），
  边界行列复制、左右边缘镜像第 1/倒数第 2 列，结果半上取整到 [0,255]。
- **Deband**（neo_f3kdb `Deband(range,y,cb,cr,grainy=0,grainc=0,
  output_depth=16)` 的移植，即默认 sample_mode=2、blur_first=true、
  random_algo_ref=UNIFORM、无 grain 无 dither）：8 位样本先上采样进 16 位
  内部域（`<<8`，threshold 按参考 `scale=false` 取 `y<<2`），每个像素由 LUT
  里的随机距离 `ref1/ref2 ∈ [0, cur_range]`（`cur_range` 取 `range` 与四边
  距离的最小值，边界的随机序列严格复刻参考 `init_frame_luts`：每像素按
  Y/ref1/ref2/Cb/Cr 顺序推进同一个 LCG 种子）取 4 个对角参考点，用 SSE 例程
  的 `avg_4`（第一对平均后饱和减 1，再与第二对平均）求 16 位均值；只有当
  `|avg - px| >= threshold` 时保留原值，否则用均值，最后 `>>8` 回到 8 位。
  与工具其它步骤一致，只作用于亮度平面（`cb/cr` 参数为兼容参考调用而保留，
  不影响 luma-only 输出）。

## 验证

与真实 VapourSynth（vapoursynth pip 包 + eedi2 / fmtconv / RemoveGrainVS 插件）
逐像素对比，同一张 64×64 测试图：

| 环节 | 对比方式 | 结果 |
|---|---|---|
| EEDI2（8bit） | 与 eedi2.dll 输出直接比 | 100% 逐像素一致 |
| Repair（模式2，8bit） | 与 RemoveGrainVS.dll 输出直接比 | 100% 逐像素一致 |
| spline36 重采样 | 与 fmtconv 16bit 输出 >>8 比 | 100% 像素差 ≤1 |
| 整条 AA 链 | VS 全程 16bit >>8 vs 本工具 8bit | 100% 像素差 ≤2，99.9% ≤1 |
| NLMeans（8bit） | 与 nlm_ispc.dll（d=0,a=2,s=4,wmode=3,wref=1）直接比 | 100% 逐像素一致 |
| FineDehalo（8bit） | 与 havsfunc（全部默认参数）直接比 | 结构与掩码一致；光晕区像素差 ≤24，源于内部 resize 内核（Bicubic/Lanczos）的浮点细节差异 |
| CAS（8bit） | 与 cas.dll（sharpness=0.7）直接比 | 99.3% 像素逐字节一致；其余像素差 ≤2（本机插件走 AVX2/AVX512 路径，中间和顺序重排所致）|
| Deband（8bit） | 与 neo-f3kdb.dll r7（range/y 多组，输出 GRAY16）的 >>8 直接比 | 100% 逐像素一致（r7 dll 在 SSE4 CPU 上走 SSE 例程，本移植按该路径的 avg_4 语义实现）|

剩余 ±1..2 是「本工具全程 8bit 取整」与「VS 在 16bit 中间精度下计算再截断」
之间的正常差异（fmtconv 强制 16bit 中间精度）。彩色路径用 PPM 测试卡验证：
色块往返零误差（实测 1000 个随机色全数精确；唯一例外是亮度恰为 x.5 的
0.1% 颜色，会三通道整体 +1），对角线硬边出现中间过渡色（AA 生效）。
NLMeans 逐字节对比在 h=3/5/8 及 21×18～96×72 多尺寸噪声图/干净图上均成立。