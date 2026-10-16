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

下面这条命令会下载 7-Zip 26.04 源码、编出 `7z.so`、编出内核和命令行，并跑测试：

```sh
./scripts/build-linux.sh
```

然后可以这样用（把路径换成你的文件）：

```sh
./build/archiver-cli list 你的文件.zip --engine ./build/7z.so
./build/archiver-cli extract 你的文件.zip 目标文件夹 --engine ./build/7z.so
```

需要密码时，程序会问你。不要把密码写在命令后面。

如果装了 Qt 6，同一条 CMake 也会编出 `./build/local-archiver`。这是 Windows/macOS 那份界面在 Linux 上编出来的结果，用来确认界面代码能编过。它不是 Windows 或 macOS 的安装包。

在 Linux 上实际解压通过的有：ZIP、7z、tar、tar.gz、tar.bz2、tar.xz、tar.zst，以及 RAR5（包括需要密码的 RAR5）。旧格式 RAR4 的处理代码在 7-Zip 动态库里，这个环境没有写出 RAR4 文件的工具，所以没有单独测过 RAR4。

## 还没有编出来的

这个仓库是在 Linux 上做的。

- Windows：没有编出 exe。源码在 `apps/desktop`。`./scripts/build-windows.sh` 会说明这一点并退出。到 Windows 上需要 Qt 6，并用 7-Zip 官方 makefile 编出 `7z.dll`，再跑 CMake。
- macOS：没有编出 app。界面源码和 Windows 是同一份。`./scripts/build-macos.sh` 会说明这一点并退出。
- Android：没有编出 APK，也没有编出 Android 用的 `.so`。Kotlin 和 JNI 源码在 `apps/android`。这里没有 Android SDK / NDK。`./scripts/build-android.sh` 会说明这一点并退出。

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

The C++ core loads 7-Zip 26.04 as a shared library. Original code is MIT. 7-Zip is LGPL (and not MIT); RAR decoding is under the UnRAR license (and not MIT). See `LICENSES/`. On this Linux machine the core, the command-line checker, and (if Qt 6 is installed) the shared desktop shell were built. Windows, macOS, and Android packages were not built here.
