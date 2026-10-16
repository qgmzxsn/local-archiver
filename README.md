# 本地归档

Windows 图形版当前版本：**0.2.0**。可以打开或拖入本机压缩包、查看文件列表、选择输出目录，再点击“开始解压”。解压在后台进行，显示进度并支持取消。

普通用户使用 Windows 安装包（`setup.exe`）或便携包（`portable.zip`）。便携包需要先把整个文件夹解压出来，再双击 `local-archiver.exe`；不需要安装 Qt、7-Zip 或开发工具。源码 ZIP 供开发使用，不能直接当程序运行。

下载 **0.2.0**：[Windows 安装版](https://github.com/qgmzxsn/local-archiver/releases/download/v0.2.0/LocalArchiver-0.2.0-windows-x64-setup.exe) · [Windows 便携版](https://github.com/qgmzxsn/local-archiver/releases/download/v0.2.0/LocalArchiver-0.2.0-windows-x64-portable.zip) · [完整发布页与源码](https://github.com/qgmzxsn/local-archiver/releases/tag/v0.2.0)。

Windows 发布与构建方法见 `packaging/windows/RELEASING.md`。GitHub 发布时，二进制安装包/便携包放在 Releases，源码保留在仓库；生成本地安装包不会自动上传 GitHub。

这是一个打开本机压缩包的小工具。Windows、macOS、Android 各是一个独立应用，共用同一份 C++ 内核。它不联网，也不把任务或设置带到另一台设备。

## 第一阶段做什么

可以打开并解压：

- ZIP
- 7z
- tar，以及 tar.gz、tar.bz2、tar.xz、tar.zst（也认 tgz、tbz2、tbz、txz、tzst 这些后缀）
- RAR 和 RAR5，只能解压，不能创建

需要密码时，由你当场输入。桌面版只在本次打开期间把密码保留在内存中，更换压缩包或关闭窗口后清除；程序不把密码写进文件，也不写进日志。

解压时会拒绝这些路径：`..`、绝对路径、指到目标文件夹外面的符号链接。不会运行解压出来的程序。

目标位置已有同名文件时会停止并保留原文件，不会自动覆盖。密码错误或校验失败时会清理当前未完成的文件；之前已经成功解压的文件和新建目录可能保留。Windows 还会拒绝输出路径中的目录联接、符号链接及设备名（如 `NUL`、`CON.txt`）。Windows 暂不创建压缩包中的符号链接。

## 第一阶段不做什么

不做分卷、改错的扩展名、图种、一次嵌套穿透、密码本。不做网盘、分享链接、二维码、磁力链接，也不做跨设备迁移。

RAR 不能创建。不要用这份代码去重做 RAR 压缩算法。

## 程序怎么分

- `core/`：我们写的 C++ 内核。运行时加载 7-Zip 的动态库（Linux 上是 `7z.so`，Windows 上是 `7z.dll`，macOS 上是 `7z.so` 或 `lib7z.dylib`）。没有把 7-Zip 编进我们的程序里。
- `apps/desktop/`：Windows 和 macOS 共用的 Qt 界面。
- `apps/android/`：Android 的 Kotlin 界面，调用同一个内核编出的 `.so`。
- `tools/cli/`：Linux 上用来检查内核的命令行。它不是第四个产品。

安装包应保持小：7-Zip 动态库加上这层薄界面。不要改成 Electron，也不要静态吞进 7-Zip。不要用 p7zip。

## 在这台 Linux 上已经编过的

Linux 内核、命令行和 Qt 界面：

```sh
./scripts/build-linux.sh
./build/archiver-cli list 你的文件.zip --engine ./build/7z.so
./build/archiver-cli extract 你的文件.zip 目标文件夹 --engine ./build/7z.so
```

需要密码时程序会问你。不要把密码写在命令后面。装了 Linux 版 Qt 6 时，同一条命令还会编出 `./build/local-archiver`。那是这份界面在 Linux 上的结果，不是 Windows 或 macOS 安装包。

Windows 命令行（MinGW 交叉编译）。产物是 `build-windows/archiver-cli.exe`、`libarchiver.dll` 和 `7z.dll`：

```sh
./scripts/build-windows.sh
```

在这台机器上用 Wine 跑过：能列出并解压 ZIP，含 `..` 的路径会被拒绝。

Windows Qt 界面也能链上，命令是 `./scripts/build-windows-qt.sh`。它需要仓库外面的两套 Qt 6.8（Windows MinGW 套件，以及给 moc 用的 Linux Qt 6.8）。链出来的是 `build-windows-qt/local-archiver.exe`。Wine 里进程能停住，没有做完整的鼠标点击。系统自带的 Linux Qt 6.4 不能链进 Windows 程序。

Android arm64 动态库（NDK r27c，不进仓库）：

```sh
./scripts/build-android.sh
```

产物在 `build-android/arm64-v8a/`：`libarchiver.so`、`libarchiver_jni.so`、`lib7z.so`。`libarchiver.so` 运行时再打开 `7z.so`，没有把 7-Zip 编进去。

调试 APK 使用上面这些已经编好的 arm64 库，不再重编内核。需要本机 Android SDK（命令行工具即可）：

```sh
cd apps/android
gradle assembleDebug
```

产物：`apps/android/app/build/outputs/apk/debug/app-debug.apk`。

在 Linux 上实际解压通过的有：ZIP、7z、tar、tar.gz、tar.bz2、tar.xz、tar.zst，以及 RAR5（包括需要密码的 RAR5）。旧格式 RAR4 的处理代码在 7-Zip 动态库里，这个环境没有写出 RAR4 文件的工具，所以没有单独测过 RAR4。

## 还没有编出来的

- macOS：必须在 Mac 上编。`./scripts/build-macos.sh` 在 Linux 上直接退出，这里没有、也不会放一个假的 macOS 二进制。界面源码仍是 `apps/desktop`。
- Android 安装包不会放进 git。调试 APK 由 `apps/android` 里的 Gradle 打出来，库来自 `build-android/arm64-v8a`。

不要把没编过的应用当成已经完成。

## 本轮修复与 Windows 验证（2026-10-16）

已在真实 Windows 上，用便携 MinGW-w64 工具链编译内核、命令行及宿主 JNI 测试库，并连接官方 7-Zip 26.04 `7z.dll` 执行回归。修复了：

- Windows COM 字符串分配器不匹配，导致打开普通 ZIP 就出现堆损坏并退出。
- Windows 写入时跟随已有目录联接，导致解压文件落到目标目录外；改成基于目录句柄逐层打开并拒绝重解析点。
- 静默覆盖已有文件，以及错误密码/CRC 失败留下当前不完整文件的问题。
- Windows 命令行的中文、emoji 路径及交互式密码回显。
- JNI 把 Java modified UTF-8 当作 UTF-8、把 UTF-16 代理项分开编码，导致补充平面字符乱码的问题。
- Android 缓存文件丢失扩展名导致所有格式被拒绝；现在保留原文件名和复合扩展名，复制失败也不会替换之前打开的文件。
- 桌面端打开新压缩包后取消密码输入，仍显示上一份文件列表的问题。
- 部分解压错误状态被忽略，以及未加密文件损坏被误报为密码错误的问题。

原有 `scripts/build-windows*.sh` 仍是 Linux 交叉编译入口。在 Windows 上可使用便携 MinGW-w64（含 CMake 和 make），下载并校验 `scripts/fetch-7zip.sh` 指定的源码后，直接运行 CMake。以下路径需要换成自己的绝对路径：

```powershell
cmake -S . -B build-windows -G "MinGW Makefiles" `
  -DSEVENZIP_SRC=C:/deps/7zip-src `
  -DCMAKE_CXX_COMPILER=g++ `
  -DARCHIVER_TEST_ENGINE=C:/deps/7zip/7z.dll `
  -DARCHIVER_TEST_7Z=C:/deps/7zip/7z.exe `
  -DARCHIVER_TEST_JNI=ON
cmake --build build-windows -j 6
ctest --test-dir build-windows --output-on-failure
```

`ARCHIVER_TEST_ENGINE` 启用实际解压回归；`ARCHIVER_TEST_7Z` 可选，用于生成 7z 加密样本。宿主 JNI 回归需要 Python 3.9+、JDK 11+，用于验证 JNI 的文件名、路径和密码转换，不代表 Android 安装包或真机验证。没有 JDK 时可不设置 `ARCHIVER_TEST_JNI`。命令行运行时，把 `7z.dll` 放在 `libarchiver.dll` 旁边，或显式传 `--engine`。

上述第一轮修复之后，Windows Qt 图形界面也已用匹配的 Qt 6.8.3 / MinGW 13.1 原生编译并运行，并完成打开、解压、密码与取消操作的 Qt 自动回归。没有构建/运行 Android APK，未重新验证 Linux/macOS。RAR4、RAR5 本轮没有样本回归。Windows 控制台密码不回显的分支已编译，自动测试使用重定向输入，没有模拟真人终端输入。

## 许可证

我们写的代码是 MIT，见仓库根目录的 `LICENSE`。

下面这些不是 MIT，二进制再分发时要带上：

- `LICENSES/7-zip-license.txt`：7-Zip 自己的许可说明。主体是 GNU LGPL。RAR 相关代码还有 unRAR 限制。少量代码是 BSD。
- `LICENSES/lgpl-2.1.txt`：GNU LGPL 2.1 全文。7-Zip 使用它。
- `LICENSES/unrar-license.txt`：UnRAR 许可。可以用来解压 RAR，不能用来做兼容的 RAR 压缩器。

7-Zip 版本是 26.04。源码从 <https://www.7-zip.org/a/7z2604-src.tar.xz> 下载，不放进 MIT 许可范围。

## English summary

Phase 1 is three separate local apps (Windows, macOS, Android) that open and extract ZIP, 7z, tar, and tar compressed with gz, bz2, xz, or zstd. RAR and RAR5 can be extracted only. A password is typed when needed and is not stored. There is no network, no password book, no split volumes, no renamed-extension or appended-archive detection, and no nested extract.

The C++ core loads 7-Zip 26.04 as a shared library. Original code is MIT. 7-Zip is LGPL (and not MIT); RAR decoding is under the UnRAR license (and not MIT). See `LICENSES/`. On this Linux machine: the Linux core and Qt shell (`./scripts/build-linux.sh`), a Windows CLI (`./scripts/build-windows.sh`, Wine-tested), a Windows Qt exe (`./scripts/build-windows-qt.sh`, linked, process stayed up under Wine), and Android arm64 `.so` files (`./scripts/build-android.sh`). A debug APK is `apps/android` `gradle assembleDebug`, packaging those libraries. macOS was not built; a Mac is required.
