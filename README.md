# aa.exe — EEDI2 抗锯齿命令行工具（C++ 独立实现）

把一段经典 VapourSynth 抗锯齿链完整移植为单文件 C++ 程序，无需安装
VapourSynth / 插件，直接对图片跑：

```
out/aa.exe -i 输入图片 -o 输出图片
```

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
aaed = core.rgvs.Repair(aaed, clip, 2)                      # 钳制到原图邻域
```

流程：亮度平面先做「EEDI2 高度翻倍 → spline36 缩回原尺寸（sy=-0.5）→ 转置」，
再换方向重复一遍。两次方向插值会把斜边的阶梯折线"抹匀"成平滑过渡，即抗锯齿效果；
色度平面保持原图不动；最后 Repair（模式 2）把结果逐像素钳制到原图 3×3 邻域的
[第 2 小, 第 2 大] 区间，防止过度偏离原图。

## 使用方法

```
out/aa.exe -i input.png -o output.png
```

- 输入格式：PNG / BMP / TGA / JPG / PGM / PPM / PNM（stb_image 支持的范围），
  灰度（1 通道）或彩色（3 通道）均可。
- 输出格式：由 `-o` 的扩展名决定：`.png`、`.bmp`、`.tga`、`.jpg/.jpeg`,
  以及 `.pgm`（灰度）/ `.ppm` / `.pnm`（彩色）。默认 PNG。
- 灰度图：直接对整幅做 AA。
- 彩色图：转 BT.601 全范围 YUV，只对亮度做 AA，色度保持原图，做完转回 RGB
  ——与脚本行为一致。
- 尺寸要求：宽和高都 ≥ 8（EEDI2 两遍转置后的隐含约束），输出与输入同尺寸。

### 可选参数（默认值与原脚本一致）

| 参数 | 含义 | 默认 |
|---|---|---|
| `--mthresh N` | 运动（边缘掩码）阈值 | 10 |
| `--lthresh N` | 线性插值阈值 | 20 |
| `--vthresh N` | 方差阈值 | 20 |
| `--maxd N` | 最大搜索距离，1..29 | 24 |
| `--nt N` | 噪声阈值 | 50 |
| `--field N` | 场奇偶，0 或 1 | 1 |
| `--repair N` | 2 = 启用 Repair(模式2)；0 = 禁用 | 2 |
| `-h` / `--help` | 帮助 | — |

示例：

```
out/aa.exe -i clip.png -o clip_aa.png
out/aa.exe -i in.bmp -o out.png --maxd 32 --nt 30
out/aa.exe -i gray.pgm -o gray_aa.pgm --repair 0
```

## 编译

任意 C++17 编译器，无第三方库（stb_image 已内置）。源码按模块拆在 `src/`
（main / eedi2 / resample / repair / imageio），构建产物输出到 `out/`：

VS Code 用户直接运行 build 任务（`Ctrl+Shift+B` 或 `Terminal → Run Build Task`），
会自动创建 `out/` 目录再编译。

默认工具链是 LLVM clang（https://llvm.org 或 `winget install LLVM.LLVM`），
以 GNU 模式配合 MinGW-w64 运行库（例如
<https://github.com/niXman/mingw-builds-binaries/releases>，选带 UCRT 的
x86_64 版本，解压后把 `bin` 目录加进 PATH），编译时用 `--target` 指定 target：

```
clang++ --target=x86_64-w64-windows-gnu -O2 -std=c++17 -o out/aa.exe \
    src/main.cpp src/eedi2.cpp src/resample.cpp src/repair.cpp src/imageio.cpp
```

或者直接用 MinGW-w64 自带的 g++：

```
g++ -O2 -std=c++17 -o out/aa.exe src/main.cpp src/eedi2.cpp \
    src/resample.cpp src/repair.cpp src/imageio.cpp
```

没有安装 MinGW 时，可用 pip 安装的 ziglang 自带的 clang 编译：

```
python -m pip install ziglang
python -m ziglang c++ -O2 -std=c++17 -o out/aa.exe src/main.cpp src/eedi2.cpp \
    src/resample.cpp src/repair.cpp src/imageio.cpp
```

## 实现要点

- **EEDI2**（tritical/HolyWu 算法 v0.9.2 的逐行移植）：14 个阶段流水线——
  边缘掩码 → 腐蚀/膨胀/去小水平缝隙 → 方向场计算（5 个候选方向，median 与
  limlut 限幅）→ 方向场滤波/扩张/填缝 → 逐行拼接 → 2× 上采样 → 2× 方向场
  处理 → 晶格插值（噪声阈值 nt8 主搜索 + nt7 回退）→ 后处理。
- **重采样**：fmtconv 约定，`srcPos(o) = (o+0.5)*ratio - 0.5 + shift`
  （像素中心对齐），spline36 核按 `max(ratio,1)` 缩放、按权值总和归一化。
- **Repair 模式 2**：对每个像素取参考图 b 的 3×3 邻域排序后的
  `clamp(a, n[1], n[7])`，边界行列直接复制 a。

## 验证

与真实 VapourSynth（vapoursynth pip 包 + eedi2 / fmtconv / RemoveGrainVS 插件）
逐像素对比，同一张 64×64 测试图：

| 环节 | 对比方式 | 结果 |
|---|---|---|
| EEDI2（8bit） | 与 eedi2.dll 输出直接比 | 100% 逐像素一致 |
| Repair（模式2，8bit） | 与 RemoveGrainVS.dll 输出直接比 | 100% 逐像素一致 |
| spline36 重采样 | 与 fmtconv 16bit 输出 >>8 比 | 100% 像素差 ≤1 |
| 整条 AA 链 | VS 全程 16bit >>8 vs 本工具 8bit | 100% 像素差 ≤2，99.9% ≤1 |

剩余 ±1..2 是「本工具全程 8bit 取整」与「VS 在 16bit 中间精度下计算再截断」
之间的正常差异（fmtconv 强制 16bit 中间精度）。彩色路径用 PPM 测试卡验证：
色块往返零误差，对角线硬边出现中间过渡色（AA 生效）。