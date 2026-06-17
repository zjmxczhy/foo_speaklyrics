# foo_speaklyrics

foobar2000 朗读 LRC 歌词组件。

本组件用于在 foobar2000 播放歌曲时读取 LRC 歌词，并通过 Tolk 或 SAPI 朗读当前歌词，适合需要读屏辅助或语音播报歌词的使用场景。

## 主要功能

- 自动加载和朗读 LRC 歌词。
- 支持手动加载本地 `.lrc` 歌词文件。
- 支持设置固定 LRC 歌词目录和临时歌词目录。
- 支持本地歌曲同目录 LRC 自动识别。
- 支持调用外部歌词下载器搜索和下载歌词。
- 支持选择歌词下载来源。
- 支持复制无时间戳歌词。
- 支持按歌词行跳转播放位置。
- 支持通过 Tolk 调用屏幕阅读器朗读歌词。
- 支持 SAPI 语音朗读。

## 适用环境

- foobar2000 2.1 或更高版本。
- Windows。
- 支持 x64 和 x86 版本 foobar2000。

## 仓库内容

- `third_party/foobar2000-sdk/foobar2000/foo_speaklyrics/`：组件源码。
- `tools/LrcDownloader/`：LRC 歌词下载工具源码。
- `build.ps1`：构建脚本。
- `package.ps1`：打包脚本。
- `朗读LRC歌词组件使用方法.md`：详细使用说明。
- `朗读LRC歌词组件更新日志.md`：更新日志。

## 构建

在仓库根目录运行：

```powershell
.\build.ps1
```

构建输出会生成到 `build/` 目录。

## 打包

在仓库根目录运行：

```powershell
.\package.ps1
```

打包输出会生成到 `dist/` 目录，包含 x64、x86 和组合安装包。

## 安装

可以使用打包生成的组件安装包：

- `foo_speaklyrics-x64.fb2k-component`
- `foo_speaklyrics-x86.fb2k-component`
- `foo_speaklyrics.fb2k-component`

在 foobar2000 中打开：

```text
文件 -> 参数选项 -> 组件
```

选择组件包安装，安装完成后重启 foobar2000。

## 使用

安装后菜单位置：

```text
视图 -> 朗读歌词
```

常用功能包括朗读歌词设置、加载本地 LRC 歌词、搜索 LRC 歌词、复制无时间戳歌词、按歌词跳转、开启或关闭自动朗读歌词等。

详细说明见：

- [朗读LRC歌词组件使用方法.md](朗读LRC歌词组件使用方法.md)
- [朗读LRC歌词组件更新日志.md](朗读LRC歌词组件更新日志.md)

## 说明

本仓库只维护 `foo_speaklyrics` 朗读歌词组件相关代码。M3U 导入组件不属于本仓库内容。

## 许可证

许可证信息见 [LICENSE](LICENSE)。
