# 7z-pigz

基于 **7-Zip 26.04** 的修改版，将 **pigz 2.8 的并行 Deflate 分块算法**接入 GZIP 压缩。压缩后端在进程内调用 zlib，使用 7-Zip 的原生线程和流接口。

启用整合选项后，GZIP 默认使用 pigz 后端，可通过 `-mmt` 指定线程数。当前提供 Windows x64 命令行构建脚本；生成的压缩文件可由普通 gzip、pigz 和原版 7-Zip 解压。

## 构建

Windows 需要 MinGW-w64 GCC、GNU Make、windres、Git for Windows，以及与工具链匹配的 zlib 头文件和静态库（zlib 1.2.6 或更高版本）。在仓库根目录运行：

```powershell
.\scripts\build-pigz.ps1 -Jobs 8
```

程序输出到 `bin/7z-pigz.exe`。

Linux/macOS 可以使用已有的 GNU Make 构建入口：

```sh
cd CPP/7zip/Bundles/Alone2
make -j8 -f ../../cmpl_gcc.mak O=_o_pigz USE_PIGZ=1
```

目前实际验证的平台是 Windows x64 / MinGW-w64。完整构建选项见 [DOC/pigz.txt](DOC/pigz.txt)。

## 使用

```powershell
# 使用 8 个线程，将单个文件或已有的 tar 文件压缩为 gzip
.\bin\7z-pigz.exe a -tgzip -mmt=8 output.tar.gz input.tar

# 指定压缩级别和分块大小
.\bin\7z-pigz.exe a -tgzip -mm=Pigz -mmt=4 -mx=6 -mcc=256k output.gz input.bin

# 选择原版 7-Zip Deflate 编码器
.\bin\7z-pigz.exe a -tgzip -mm=Deflate output.gz input.bin

# 检查和解压
.\bin\7z-pigz.exe t output.gz
.\bin\7z-pigz.exe x output.gz
```

接入范围为 GZIP 压缩。支持 `-mx=0..9`、`-mmt=N` 和 `-mcc=SIZE`，默认分块大小为 128 KiB。压缩率会随数据和后端变化；pigz CLI 的 `-11` Zopfli 模式未接入。

## 验证

```powershell
python tests/test_pigz.py --exe bin/7z-pigz.exe
```

集成测试检查空文件、分块边界、不同线程数的输出一致性、压缩级别、标准输入输出、文件名和时间戳、CRC，以及 7z/ZIP 兼容性。`--baseline` 和 `--pigz` 可指定原版工具进行交叉校验，`--benchmark` 可记录本机测试耗时。

[tests/PigzEncoderTest.cpp](tests/PigzEncoderTest.cpp) 直接验证短读写、读写失败、取消、编码器复用和线程句柄清理。构建命令见文件开头。

## 来源与许可

这是修改后的实现。原始项目：

- [7-Zip](https://github.com/ip7z/7zip) / [官网](https://www.7-zip.org/)
- [pigz](https://github.com/madler/pigz)，作者 Mark Adler
- [zlib](https://zlib.net/)

7-Zip 原有许可证见 [DOC/License.txt](DOC/License.txt)。pigz 派生实现保留了原始许可和修改说明，详见 [PigzEncoder.cpp](CPP/7zip/Compress/PigzEncoder.cpp) 和 [DOC/pigz.txt](DOC/pigz.txt)。
