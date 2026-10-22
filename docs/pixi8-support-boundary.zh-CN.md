# Pixi Native Kit 支持范围与已知限制

更新日期：2026-10-22。适用基线：pixi_native v0.1 公开开发版，Pixi 公共接口对照版本 `pixi.js@8.20.0`。

**当前底座没有完整支持 Pixi 8。** 它提供一部分 Pixi 8 公共接口，内部使用原生 C++ 场景树和 OpenGL ES 绘制。接口存在、APK 编译成功、游戏能启动，都不能证明该游戏的动画、文字、输入、资源释放和性能已经通过验收。

本文件是接入和打包前的限制清单。对照源码核实的缺口、已有实现的边界，以及尚未完成的验证分别列出；没有列为已验证的 API 或参数组合，需要单独核对，不默认承诺兼容。本文不是未来版本的支持承诺。

## 状态怎么理解

| 状态 | 含义 | 接入时怎么处理 |
| --- | --- | --- |
| 明确不支持 | 没有公开接口，或当前实现明确拒绝该用法 | 游戏用到时，提前列为接入缺口 |
| 部分支持 | 常见路径已有实现，但部分参数、事件或组合缺失 | 写清游戏实际用法是否落在已实现范围 |
| 尚未验证 | 可能已有实现，但缺少该平台、设备或场景的运行证据 | 不写成“已支持”或“实机通过” |
| 已有验证 | 指定测试、平台和版本检查通过 | 只承诺该证据覆盖的范围 |

缺口的表现也不同：部分路径抛出 `UnsupportedNativeAPIError`；未导出的 API 可能是 `undefined`；部分未适配参数可能没有完整处理，缺字等问题也不一定报错。**目前还不能保证所有不支持的用法都会被自动拦截。**

## 明确不支持的能力

| 能力或 API | 当前边界和调用表现 | 对游戏的影响 |
| --- | --- | --- |
| WebGPU | `Application.init({ preference: 'webgpu' })` 和对应自动检测路径明确报错 | 不能按官方 WebGPU 渲染器运行 |
| 自定义 GPU 程序和 Filter | `Filter`、`Shader`、`GlProgram`、`GpuProgram` 是拒绝调用的占位接口；`GlProgram.from`、`GpuProgram.from` 也报错 | 依赖自定义 GLSL/WGSL、第三方 Shader Filter 的效果不能直接接入 |
| 自定义 Mesh 或粒子 Shader | `Mesh`、`MeshSimple`、`ParticleContainer` 的自定义 `shader` 明确报错 | 自定义网格材质、粒子材质不能直接使用 |
| RenderGroup 和 RenderLayer | `enableRenderGroup`、`disableRenderGroup`、`isRenderGroup: true` 明确报错；`RenderGroup`、`RenderLayer` 未导出 | 官方分组渲染、独立图层排序能力尚未适配 |
| HTML 富文本 | `HTMLText`、`HTMLTextStyle` 未导出 | HTML/CSS 排版文字不能直接显示 |
| 原生文字的渐变和纹理填充 | Text 样式转换遇到渐变或纹理填充、描边会明确报错，可能在同步或绘制时触发 | 纯色文字可走已有路径；渐变、图案文字不能直接使用 |
| 像素线 | `Graphics.stroke({ pixelLine: true })` 以及相应批次明确报错 | 依赖缩放后保持固定像素宽度的线条无法按该 API 实现 |
| 部分遮罩选项 | `setMask({ inverse: true })` 和非 `red` 的 `channel` 明确报错 | 反向遮罩、其他通道遮罩不能直接使用 |
| 缓存纹理的 MSAA | `cacheAsTexture({ antialias: true })` 明确报错；`resolution` 和 `antialias: false` 已接入 | 缓存与刷新走原生 RenderTexture，不能启用多采样抗锯齿 |
| 多采样离屏目标 | TextureSource 的 `antialias: true`、`sampleCount` 非 1，以及对象形式 `generateTexture` 的 `antialias: true` 明确报错 | 不支持这些路径的 MSAA 离屏抗锯齿 |
| 非 RGBA8 或非二维纹理源 | 公共 TextureSource 接受 `rgba8unorm`、二维、单层、单采样、单 mip 层；其他格式、维度或数量明确拒绝 | 浮点目标、整数格式、立方体、3D、数组纹理、多 mip 层不能直接使用 |
| 非字节像素缓冲 | BufferImageSource 需要调用方提供 `Uint8Array` 或 `Uint8ClampedArray` RGBA8 数据；Float32、Uint16 等明确拒绝；不支持官方的默认浮点缓冲用法 | 像素处理代码必须核对格式，不能直接照搬官方默认值 |
| 部分高级混合模式 | 字符串 `overlay`、`darken`、`lighten`、`color-dodge`、`color-burn`、`hard-light`、`soft-light`、`difference`、`exclusion`、`hue`、`saturation`、`color`、`luminosity` 明确拒绝 | 用这些模式实现的叠色、光效不能直接使用；数字和字符串均进行同样校验 |
| 视频和压缩纹理的公开 Source API | `VideoSource`、`CompressedSource` 未导出 | 不能按这些 Pixi 8 公共接口接入；已有资源加载器的 KTX2/ASTC 路径不等于完整支持 CompressedSource |
| 自定义网格属性布局和实例化 | `Geometry` 已接入二维 `float32x2` 的 `aPosition`/`aUV`（含交错 stride/offset），支持 Uint16/Uint32 索引及无索引；其他位置/UV 格式和实例化明确拒绝 | 不能据此推断任意顶点属性、自定义 Shader 或 3D 网格布局可用 |
| 静态 BitmapFont 缺失字形 | 加载的外部静态字体不会凭空生成缺失字形；自动字体和本适配层安装的动态字体支持按 Unicode 码点追加图集页 | 静态图集仍须包含所需字符；字体文件缺字、复杂文字塑形和新增字符跨批次 kerning 不等于完整浏览器排版 |
| iOS 的浏览器形态震动接口 | 当前没有提供等价的 `navigator.vibrate` 桥 | Android 的震动实现不能作为 iOS 震动已支持的依据 |

上表列出最可能影响游戏接入的明确缺口。完整导出项清单见后面的“完整 API 清单”；未导出的其他接口同样不能当作已支持。

## 已有实现但仍有限制

| 范围 | 已有路径 | 仍需提前说明的边界 |
| --- | --- | --- |
| Application 和 renderer | 异步 init、Ticker、基础 render、resize、generateTexture、extract 适配 | 不是官方 WebGL/WebGPU renderer 的完整系统接口；全部初始化、渲染、提取参数没有逐项建立一致性证据 |
| Container 和变换 | 层级、标签、矩阵、origin、reparent、onRender、基础裁剪 | 任意 effect 链尚未完整适配；origin 与矩阵分解沿用 8.20 的相关行为限制 |
| Graphics 和 SVG | 常见路径、形状、曲线、填充、cut、共享 GraphicsContext；SVG 基础形状和路径 | 字符串 SVG 需要原生 XML 桥；SVG CSS、外部资源、滤镜等完整浏览器 SVG 语义尚未建立 |
| 渐变和图案 | 原生线性渐变、双圆径向渐变、纹理填充；repeat-x 有像素验证 | 复杂径向旋转、透明度、其他平铺和变换组合需要更多原版像素对比；不适用于上表拒绝的文字填充 |
| 缓存纹理和 TexturePool | 原生缓存构建、冻结、显式刷新、分辨率、禁用/销毁、子节点继续命中；TexturePool 的 POT 目标复用、裁剪 frame、分辨率、归还和 clear | 不支持 MSAA 和池纹理自动 mipmap；不是官方 RenderGroup 的完整实现；复杂嵌套滤镜、遮罩和长期 GPU 内存仍需专项验证 |
| Texture 和 Assets | 常见图片、图集、生命周期、源切换、重载、共享样式、高 DPI 字节源 | 任意 resource 替换、全部 metadata/cache 选项、Assets 扩展机制尚未完整迁移；RenderTexture 构造的 source 需是无图片资源的原生离屏源 |
| Sprite 系列 | Sprite、AnimatedSprite、TilingSprite、NineSliceSprite 的适配路径 | 所有 trim、anchor、bounds、事件和变换组合尚未逐项与官方对比 |
| Mesh | Geometry、MeshGeometry、PlaneGeometry、RopeGeometry、PerspectivePlaneGeometry、PerspectiveMesh、MeshSimple、MeshPlane、MeshRope；可见对象缓冲更新、Plane 自动 resize、官方 CPU 透视细分算法 | 非三角形拓扑虽有参数映射，原生绘制语义尚未完整验证，不能承诺 line/point 模式正确 |
| 粒子 | 轻量 Particle、批量 ParticleContainer、常见动态/静态属性、增删和隐藏分支恢复 | 同一容器的粒子必须共享一个 TextureSource，否则报错；全部属性组合未验证 |
| Text 和 BitmapText | 原生字形栅格化、常见纯色填充/描边/阴影、样式更新、静态字体、动态字体安装、自动 Unicode 码点追加、自动字体引用释放 | 字形追加保留旧纹理引用；跨追加批次的 kerning、复杂文字塑形、字体回退、彩色 Emoji 和全部排版选项尚未完整对比 |
| 内置 Filter 和混合 | ColorMatrix、Blur、Alpha、Noise、Displacement 有原生适配路径；部分固定功能混合已接入 | 全部滤镜选项、组合、边缘和分数像素 Sprite mask 仍需对比，不能推断任意第三方 Filter 可用 |
| 事件和生命周期 | 原生命中、capture/once、独立触摸 ID、primary/type、pointercancel 清理、visibilitychange | Federated 类和边界仍有 Pixi 7 桥接部分；不是完整 Pixi 8 EventSystem。Android 压力值并非完整硬件压力透传；高级触控笔字段未完整适配 |
| 浏览器环境接口 | 面向游戏的 document/canvas、事件、fetch/XHR、编码等桥接 | 不是完整浏览器；不承诺任意 DOM/CSS 页面、浏览器插件或全部 Web API 可用。当前 TextDecoder 仅支持 UTF-8 |

## 公开版平台和功能边界

- Android 使用 V8 和 EGL/OpenGL ES；iOS 使用 JavaScriptCore 和原生 OpenGL ES；macOS 使用 JavaScriptCore 和外部 ANGLE/Metal。构建说明和平台条件见 [README](../README.md)。
- 公开版没有广告、支付、登录、Firebase、归因统计或 Unity SDK，也没有旧游戏的内容版本、机台清单和商店组件特例。启用旧服务配置会明确拒绝，业务接入由项目自行实现。
- 原生 Spine 桥仍链接当前第三方 runtime，不能当作所有 Spine 版本和资源都兼容。Spine 使用独立许可，根目录 MIT 不能替代它；见 [第三方说明](../THIRD_PARTY_NOTICES.md)。
- Android 已实现振动 pattern 和取消；实际效果需要设备验证。iOS 没有等价 `navigator.vibrate`。模拟器不能证明物理振动。
- Apple 已预乘编码图片的保留字节解码目前只支持 PNG/JPEG；Android 还支持 WebP；KTX2 按 DFD 元数据处理。这不等于普通图片全部不支持。
- 长时间游戏、GPU 内存、不同设备持续帧率和原版全部画面一致性没有完整验证，不能承诺长期 60 FPS。
- 当前提供 Debug/模拟器构建。正式签名、商店发行、Archive/IPA 和发布 CI 不在现有验收范围。

## 验证证据和完整 API 清单

当前公开版的实际编译、运行和测试结果以 [验证记录](validation.md) 为准。旧底座的手机结果不自动转移到公开版。独立验收示例在 `examples/pixi8-conformance` 和 `examples/runtime-conformance`，它们不依赖游戏业务。

固定版本 [API 导出清单](pixi8-api-audit.json) 共 739 项，其中 88 项适配层存在、4 项构造接口明确拒绝、647 项未适配导出。新增导出包括官方 CPU 数据算法（Transform、UV、对象池和数学工具），不会替换原生 GPU 渲染器。许多未适配项是 renderer 基础设施或工具；这些数字不是游戏兼容率。接口存在也不表示全部参数和语义一致。

源码依据为 [公开接口](../src/compat/pixi8.mjs) 及 `src/compat/pixi8/` 各模块。接口变更后执行 `npm run audit:pixi8` 更新清单。

## 接入和交付前必须说明

先检查游戏实际使用的 API、参数、插件版本、资源和 Web 依赖，提前列出明确不支持、部分支持和待验证项，以及可能影响的画面或操作。导出审计不会自动验收动态调用、参数和视觉效果。

交付时记录游戏版本、底座提交、已解决问题、剩余限制、编译平台、实际运行设备及验证场景。不支持的能力不能通过浏览器回退、替代特效或吞错来宣称兼容；编译通过、启动通过和完整验收必须分别表述。
