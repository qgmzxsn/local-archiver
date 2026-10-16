# 本地归档

这是一个打开本机压缩包的 Windows 程序。它不下载任何东西。

## 能做什么

- ZIP、7z，以及用 gz、bz2、xz、zstd 压缩的 tar
- RAR 和 RAR5 只能解压
- 分卷，并会指出缺了哪一卷
- 后缀是 mp4 或图片，但内容是 ZIP、7z 或 RAR；或者压缩包接在视频或图片后面
- 一层嵌套解压。密码不对、缺卷、同一层有好几个压缩包，或超过大小限制时会停下来
- 本机密码本，先试已经保存的密码

## 还不要当成已经完成

GitHub Release v0.2.0 是更早的 Windows 安装包，不包含后来的修正。目前还没有更新的 GitHub Release。Mac 没有发布。Android 暂停。像素隐写、创建 RAR、破解密码、网盘和磁力链接都不做。

## 许可证

我们写的代码是 MIT，见 `LICENSE`。7-Zip 是 LGPL，运行时加载 `7z.dll`。UnRAR 只能用来解压。说明在 `LICENSES/`。
