# 本地归档（第一阶段）

这是一个打开本机压缩包的小工具。Windows、macOS、Android 各是一个独立应用，共用同一份 C++ 内核。它不联网，也不把任务或设置带到另一台设备。

## 第一阶段做什么

可以打开并解压：

- ZIP
- 7z
- tar，以及 tar.gz、tar.bz2、tar.xz、tar.zst（也认 tgz、tbz2、tbz、txz、tzst 这些后缀）
- RAR 和 RAR5，只能解压，不能创建

需要密码时，由你当场输入。程序不把密码写进文件，也不写进日志。

解压时会拒绝这些路径：`..`、绝对路径、指到目标文件夹外面的符号链接。不会运行解压出来的程序。

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

在 Linux 上实际解压通过的有：ZIP、7z、tar、tar.gz、tar.bz2、tar.xz、tar.zst，以及 RAR5（包括需要密码的 RAR5）。旧格式 RAR4 的处理代码在 7-Zip 动态库里，这个环境没有写出 RAR4 文件的工具，所以没有单独测过 RAR4。

## 还没有编出来的

- macOS：必须在 Mac 上编。`./scripts/build-macos.sh` 在 Linux 上直接退出，这里没有、也不会放一个假的 macOS 二进制。界面源码仍是 `apps/desktop`。
- Android APK：没有。NDK 能编 `.so`，但这里没有 Android SDK，仓库里也没有 Gradle Wrapper。SDK 要另外下载平台包并接受许可，不算一次干净的安装，所以停在 `.so`，没有做 APK。

不要把没编过的应用当成已经完成。

## 许可证

我们写的代码是 MIT，见仓库根目录的 `LICENSE`。

下面这些不是 MIT，二进制再分发时要带上：

- `LICENSES/7-zip-license.txt`：7-Zip 自己的许可说明。主体是 GNU LGPL。RAR 相关代码还有 unRAR 限制。少量代码是 BSD。
- `LICENSES/lgpl-2.1.txt`：GNU LGPL 2.1 全文。7-Zip 使用它。
- `LICENSES/unrar-license.txt`：UnRAR 许可。可以用来解压 RAR，不能用来做兼容的 RAR 压缩器。

7-Zip 版本是 26.04。源码从 <https://www.7-zip.org/a/7z2604-src.tar.xz> 下载，不放进 MIT 许可范围。

## English summary

Phase 1 is three separate local apps (Windows, macOS, Android) that open and extract ZIP, 7z, tar, and tar compressed with gz, bz2, xz, or zstd. RAR and RAR5 can be extracted only. A password is typed when needed and is not stored. There is no network, no password book, no split volumes, no renamed-extension or appended-archive detection, and no nested extract.

The C++ core loads 7-Zip 26.04 as a shared library. Original code is MIT. 7-Zip is LGPL (and not MIT); RAR decoding is under the UnRAR license (and not MIT). See `LICENSES/`. On this Linux machine: the Linux core and Qt shell (`./scripts/build-linux.sh`), a Windows CLI (`./scripts/build-windows.sh`, Wine-tested), a Windows Qt exe (`./scripts/build-windows-qt.sh`, linked, process stayed up under Wine), and Android arm64 `.so` files (`./scripts/build-android.sh`). macOS was not built; a Mac is required. No Android APK was built.
