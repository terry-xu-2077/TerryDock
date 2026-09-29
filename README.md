# LightDock

面向 Windows 10 / Windows 11 的轻量级应用启动 Dock。

不替代任务栏，不做桌面增强。只做一件事：把常用的应用放在屏幕底部，点一下就启动，并且启动过程要好看。

---

## 技术栈

| 项 | 选择 |
| --- | --- |
| 语言 | C++20 |
| 窗口 | Win32 API（无框架） |
| 渲染 | Direct2D + WIC |
| 图标 | Windows Shell API |
| 构建 | CMake + 项目自带 MinGW-w64 |

不使用 Electron / Tauri / WebView2 / WinUI / WPF / Qt / Chromium。运行时只有 Windows 自带的系统 DLL。

---

## 渲染架构

```text
Win32 HWND (WS_EX_LAYERED)
    ↓
Direct2D DC RenderTarget，绑定到 32bpp DIB Section
    ↓
premultiplied BGRA 离屏位图
    ↓
UpdateLayeredWindow
    ↓
桌面
```

没有子窗口、没有普通控件、没有 GDI 绘制。Dock 的背景、圆角、半透明、描边、高光、阴影、图标、放大、状态条全部由 Direct2D 自绘。

透明区域是真正的逐像素 Alpha，不是矩形裁剪模拟的圆角。

---

## 构建

项目使用 MinGW-w64 与 CMake 工具链，Windows 上无需安装 Visual Studio。首次在新电脑上 clone 项目后，先双击 `setup-toolchain.bat` 下载并安装编译环境，再双击项目根目录的 `build.bat` 完成配置和编译。

```bat
build.bat
```

`setup-toolchain.bat` 会把当前固定版本的 WinLibs 工具链安装到项目内的 `_toolchain\mingw\mingw64`；`build.bat` 使用这个目录，并将构建输出放在 `build_mingw`：

```text
build_mingw\LightDock.exe
```

也可以手动执行：

```bat
set PATH=_toolchain\mingw\mingw64\bin;%PATH%
_toolchain\mingw\mingw64\bin\cmake.exe -G "MinGW Makefiles" -B build_mingw -S .
_toolchain\mingw\mingw64\bin\cmake.exe --build build_mingw --parallel 4
```

产物：`build_mingw\LightDock.exe`

单文件可执行程序，静态链接 GCC 与 C++ 运行库，不需要额外 DLL。直接拷贝即可使用。

开发测试时也可以直接双击：

```bat
update-build-run.bat
```

它会按顺序完成：关闭当前正在运行的 `LightDock.exe` → `git pull --ff-only` 拉取最新代码 → 调用 `build.bat` 编译 → 启动新的 `build_mingw\LightDock.exe`。脚本运行时会先把自身复制到临时目录，因此即使本次拉取更新了脚本自己，也不会中断当前流程。

---

## 使用

| 操作 | 行为 |
| --- | --- |
| 左键点击图标 | 启动应用，图标弹跳 |
| 右键点击图标 | 打开 / 图标配置… / 从 Dock 移除 |
| 右键点击 Dock 空白 | 添加应用 / Dock 设置… |
| 鼠标在 Dock 上移动 | 邻近放大 |
| 点击“开始”图标 | 打开 Windows 开始菜单 |
| 点击“搜索”图标 | 打开 Windows 搜索 |
| 点击右侧临时运行图标 | 激活该应用最近使用的窗口 |
| 悬停有多个窗口的应用 | 延迟后展开窗口列表，左侧显示实时 DWM 缩略图，点击精确切换窗口 |
| 右键右侧临时运行图标 | 打开 / 打开所在文件夹 / 固定到 Dock |
| 右键点击托盘图标 | Dock 设置… / 显示器 ▸ / 退出 |


> Start / Search 的弹出位置：Windows 10 下 LightDock 会尝试把原生 Shell 弹窗移动到被点击图标附近；Windows 11 的开始菜单与搜索界面由系统组合层控制，顶层 HWND 的位置会被系统忽略/重置，因此 Win11 目前保持系统默认弹出位置。无论哪一版 Windows，打开这些 Shell UI 时 LightDock 都会维持在任务栏之上，并将其排除在全屏检测之外。

全局操作（设置、选显示器、退出）都放在托盘菜单里 —— 屏幕底部那条窄条不是个好用的设置入口。

只有两个窗口，各管各的，互不混装：

- **图标配置…**（针对单个图标）— 标题栏标明正在配置哪个应用。名称（添加应用时自动读取程序版本资源里的本地化描述，如"记事本"）、程序、附加命令、图标文件；下方是这只图标自己的底板设置：启用圆角底板、图标在底板内的比例、顶部颜色（色块直接预览，点击弹出颜色选择器）、"自定义第二颜色"（默认关，底色由程序自动派生；开后用自选色做渐变混合）。圆角与裁剪跟随全局，不可单设。**所有底板改动实时预览** —— 颜色、比例、开关一变，Dock 上的图标立刻刷新，不用点保存；取消则自动还原。
- **Dock 设置…**（全局，本 Dock 程序的设置）— 背景栏模式（固定宽度 / 弹性跟随）、整体大小、图标间距、放大倍率，以及**多窗口菜单延迟**；首次默认读取 Windows 的鼠标悬停时间，之后可单独设为 0–1500 ms。**背景栏外观**：自定义背景渐变开关与顶部/底部颜色（色块预览 + 系统颜色选择器）；以及**全局图标设置**：圆角半径、图标在底板内的比例默认值、不透明度。**所有设置实时预览** —— 滑块、下拉、颜色一变，Dock 立即重排刷新，不用点保存；取消则自动还原到打开前的状态。

图标在底板内的比例同时存在于两个层级：Dock 设置… 是所有图标的默认值，图标配置… 里可以给单只图标设一个不同的值（与全局相同时不落盘，改全局仍对它生效）。比例范围为 50%–150%。

Dock 左侧常驻“开始”和“搜索”两个系统入口；用户固定的应用位于中间。未固定但当前拥有任务栏窗口的应用会自动出现在 Dock 右侧，并以分隔线与固定区分开。Dock 始终按 macOS 风格保持“一应用一图标”：多个资源管理器文件夹、多个浏览器窗口不会重复占用 Dock 图标，而是收进该应用的窗口列表。鼠标悬停达到“多窗口菜单延迟”后展开窗口栈；每行左侧通过 DWM Thumbnail 显示对应窗口的实时缩略图，右侧显示窗口标题。列表包含最小化窗口，当前活动窗口带状态标识；点击任意一行会直接激活对应 HWND。关闭最后一个窗口后，未固定应用的临时图标自动消失；已固定应用则保留图标，仅取消运行状态点。

Dock 启动后会在任务栏通知区放一个小图标，这是全局设置入口。

添加应用时选择 `.exe` 或 `.lnk`。名称自动读取程序版本资源里的本地化描述（中文系统上就是中文名，如"Windows 资源管理器"），进程名、图标全部自动读取，不需要手动填写。图标缓存在 `%APPDATA%\LightDock\icons\` 里用"程序名_短哈希.png"规范命名（如 `explorer_3f2a1b0c.png`），一眼能看出属于哪个程序。

---

## 配置

```text
%APPDATA%\LightDock\config.json     应用列表与外观参数
%APPDATA%\LightDock\icons\          图标缓存（PNG）
```

图标只在第一次添加时从可执行文件里提取，之后直接读缓存。启动 Dock 不会重新解析任何 EXE。

```json
{
  "iconSize": 52,
  "iconSpacing": 14,
  "magnification": 1.6,
  "windowMenuHoverDelayMs": 400,

  "background": {
    "opacity": 0.72,
    "cornerRadius": 18,
    "borderOpacity": 0.15,
    "shadowOpacity": 0.30,
    "top": "#3A2E5C",
    "bottom": "#141824"
  },

  "iconBackdrop": {
    "cornerRadius": 12,
    "opacity": 0.92,
    "iconScale": 0.86
  },

  "panelMode": "fixed",

  "monitor": "\\\\.\\DISPLAY1",

  "apps": [
    {
      "name": "记事本",
      "targetPath": "C:\\Windows\\notepad.exe",
      "arguments": "",
      "processName": "notepad.exe"
    },
    {
      "name": "画图",
      "targetPath": "C:\\Windows\\System32\\mspaint.exe",
      "resolvedPath": "C:\\Windows\\System32\\mspaint.exe",
      "processName": "mspaint.exe",
      "icon": "mspaint_263178dc.png",
      "plate": {
        "enabled": true,
        "iconScale": 0.7,
        "customBottom": true,
        "top": "#8A3B1E",
        "bottom": "#3E1A0C"
      }
    }
  ]
}
```

改完配置重启 Dock 即可生效（`显示器` 也可以在右键菜单里直接切）。

### 单个图标的底板（apps[].plate）

每只图标自带一份 `plate`：`enabled`（这只图标要不要圆角底板）、
`iconScale`（图标占底板比例，`0` 表示跟随全局默认）、`opacity`（可选的单图标底板
透明度覆盖；未设置时跟随全局）、`top`（底板颜色）、`customBottom` + `bottom`
（自定义渐变第二色；不开时程序自动从顶色派生一个偏暗的渐变伙伴色）。

圆角半径（以及满幅图标被裁出的圆角）始终跟随全局 `iconBackdrop`；底板透明度默认跟随全局，但图标配置里可以启用单图标透明度覆盖。全局层不负责
颜色；颜色永远属于每只图标自己，在图标右键 → 图标配置… 里用色块
（点击弹出系统颜色选择器）设置，改动实时预览。

### iconBackdrop

Windows 的图标形状五花八门：圆形、全幅矩形、不规则图形。开启某只图标的底板之后（图标配置… 里"启用圆角底板"），它背后会垫一块统一的圆角板。**圆角裁剪作用于所有比例**：图标缩小后居中放在底板里，超出圆角边界的部分仍会被裁掉；图标放大到 100% 以上时，超出底板边界的部分也会被裁掉。

底板永远是垂直渐变：只选一个颜色时，程序自动派生一个偏暗的渐变伙伴色；勾选"自定义第二颜色"后才用自选的底色混合。

圆角裁剪是在上传位图时一次性完成的（预乘到 alpha 通道），所以运行时仍然只是一次位图 blit，没有任何额外开销。

底板和 Dock 背景栏的边缘都有一圈 1px 的**渐变高光内描边**：半透明白（不是纯白），顶部最亮，侧面与底部保持可见的微光、底边再略抬一点 —— 四周都描得出轮廓，同时保留"光源在上方"的层次。描边画在图标之上，所以满幅被裁圆角的图标（如地图类）边缘同样带高光。

**背景栏渐变**：Dock 设置… 里可以给整个背景栏开"自定义背景渐变"，从顶部颜色渐变到底部颜色（存进 config.json 的 `background.top` / `background.bottom`）；关闭开关回到内置的炭黑配色。

---

## 实现要点

### 放大算法

鼠标 X 与每个图标**基础布局中心**的距离决定放大倍率，高斯衰减：

```cpp
targetScale = 1.0f + (magnification - 1.0f) * exp(-(d * d) / (2 * sigma * sigma));
```

`sigma` 取 0.9 个图标单元宽度。

距离必须对准**固定的基础中心**，而不是当前动画位置。对准动画位置会形成跨帧反馈回路，鼠标一滑过图标就明显抖动 —— 这是踩过的坑。

布局以光标为锚点重排：光标在基础布局中悬停的那个位置，放大后仍然停在光标下面。行平移用 `tanh` 平滑饱和而不是硬 clamp，鼠标到达 Dock 两端时不会整行跳一下。

### 面板宽度：两种模式

`panelMode` 可以切换两种行为，设置窗口里有下拉框。

**`fixed`（默认，macOS 风格）**

鼠标一进入 Dock，面板就一次性展开到"所有图标都放大到最大时"需要的宽度；图标在这个已经撑开的范围内缩放，永远不会越过面板的左右边缘；鼠标离开，面板再收回来。

展开产生的多余宽度不会闲置：它会**均摊进图标之间的间隙**，整排图标向两侧摊开、精确填满面板的可用宽度。悬停哪个图标，哪个图标放大并把它左右的邻居向外推，而不是整排挤在中间、两侧留两块死边距。

**`elastic`**

面板始终紧贴图标行：宽度等于当前行宽加内边距，放大、增删图标都会让它连续伸缩，并且会跟着光标轻微左右移动。

不论哪种模式，阴影都不会每帧重算 —— 阴影按**最大可能宽度**烘焙一次，绘制时分左 / 中 / 右三段横向九宫格，圆角的两端按 1:1 画，只有中间的直边带被拉伸。

### iconBackdrop 的 iconScale

`iconScale` 控制图标在底板内占多大。低于 1.0 时图标缩小并居中，超过 1.0 时图标放大并裁掉超出底板的部分。全局设置和图标配置…里的范围都是 50%–150%；单个图标与全局相同时记为 0，表示跟随全局。

### 图标提取

按质量择优，拿不到就逐级降级：

1. `PrivateExtractIconsW` **原生资源直取**：按 256→128→96→64→48→32 探测，只有返回的位图尺寸与请求一致（未被缩放）才算命中 —— 旧程序的小图标拿到的是原生的清晰像素，而不是 Image List 强行拉伸的糊图
2. Shell 的 **Jumbo image list**（`SHGetImageList(SHIL_JUMBO)`），现代应用的 256px 图标来源；若内容明显是放大的小位图（有效内容 < 96px）则弃用
3. `IShellItemImageFactory::GetImage(256)`
4. `SHGetFileInfoW` + HICON 兜底

拿到之后做一次**内容归一化**：扫描 alpha 边界，如果有效内容只占画布中间一小块，就裁剪出来再高质量放大，小图标也会撑满 Dock 槽位。

上传给 D2D 的位图是最大显示尺寸的 **2 倍**，所有绘制都是干净的缩小，不会出现 5 倍 minification 的锯齿。

### 动画

阻尼弹簧，基于 `deltaTime` 积分，内部再切成 ≤ 1/240 s 的子步：

```cpp
a = -ω²(x - target) - 2ζω·v;
v += a·dt;
x += v·dt;
```

60Hz / 120Hz / 144Hz 下手感一致，掉帧时也不会"跳"过去。

Hover 缩放与启动弹跳是两条独立的弹簧，最终变换是三者相加：

```text
最终位置 = 布局位置 + bounceOffset(竖直) + scale(底部锚定向上生长)
```

互不覆盖，不会打架。

### 刷新策略

空闲时不跑渲染循环。单个可等待定时器承担两种节奏：

* 动画中：按显示器刷新率（1000/Hz ms）触发，渲染 + 推进弹簧
* 空闲：50 ms 唤醒一次，只做 `GetCursorPos` 命中判断 + 1 Hz 的进程轮询

弹簧全部静止后自动退出动画状态，CPU 回到接近 0。

### 透明穿透

HWND 比可见 Dock 大得多（放大、弹跳、阴影都要地方）。`WM_NCHITTEST` 对 Dock 面板和当前图标矩形返回 `HTCLIENT`，其余全部返回 `HTTRANSPARENT`，鼠标直接落到底层窗口。同时 `WM_MOUSEACTIVATE` 返回 `MA_NOACTIVATE`，点击不会抢走当前应用的焦点。

### DPI

Per-Monitor DPI Aware v2。所有尺寸都按 `dpi/96` 缩放后参与布局，图标位图按目标显示尺寸重新用 WIC 高质量缩放生成 —— 不是把小图放大，不会模糊。

---

## 目录结构

```text
LightDock/
├─ CMakeLists.txt
├─ README.md
├─ LICENSE
├─ .gitignore
├─ build.bat
└─ src/
   ├─ main.cpp            程序入口（无控制台）
   ├─ App.{h,cpp}         消息循环、应用管理、输入分发
   ├─ DockWindow.{h,cpp}  Win32 分层窗口、DPI、命中测试
   ├─ DockRenderer.{h,cpp} Direct2D 离屏渲染、玻璃背景、阴影
   ├─ DockLayout.{h,cpp}  布局与放大倍率计算
   ├─ Animation.{h,cpp}   弹簧积分
   ├─ DockItem.{h,cpp}    数据模型与图标位图管理
   ├─ AppLauncher.{h,cpp} ShellExecuteEx 启动
   ├─ ProcessMonitor.{h,cpp} 运行状态轮询
   ├─ IconLoader.{h,cpp}  Shell 图标提取与 PNG 缓存
   ├─ Config.{h,cpp}      config.json 读写
   ├─ Json.{h,cpp}        轻量 JSON（仅本项目所需）
   └─ Utils.{h,cpp}       路径 / 编码 / ComPtr 等
```

---

## V0.1 不做

文件夹、最近应用、天气、时钟、Widget、插件、多 Dock、主题商店、云同步、自动更新、完整托盘管理、窗口预览、应用分组、Launchpad、真 Acrylic 背景模糊。

---

## License

MIT
