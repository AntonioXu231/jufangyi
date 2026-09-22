# PD 采集 TCP 服务 V1 使用说明

## 功能范围

本版本将已经验证的 `pd_acquisition_service.c` 采集引擎接入 lwIP TCP。

- IP：`192.168.1.10`
- 端口：`6001`
- 保留 UART 命令行；UART 与 TCP 共享同一采集状态机。
- 支持控制和元数据查询；不传输事件/快照二进制内容。

## TCP 文本协议

每条命令以 `\n` 或 `\r\n` 结束；命令与响应均为 ASCII。

```text
HELP
STATUS
START 128
START 0
STOP
EVENT 0
SNAP 0
CLEAR
```

`START 0` 表示连续采集。`STOP` 在 DMA 等待期间也会被轮询处理；当前包会被 DMA reset 丢弃，已归档数据保持有效。

## Vitis 导入方式

本文件不能与原 UART 应用同时作为同一个 Vitis Application 的 `main.c` 编译。

1. 保留现有 `app_component` 不动；
2. 复制现有 `lwip_echo_server` Application，命名为 `pd_acquisition_tcp_service`；
3. 在新 Application 的 `src` 中：
   - 用 `pd_acquisition_tcp_service_main.c` 覆盖 `main.c`；
   - 加入同目录的 `pd_acquisition_service.c`；
   - 保留原 lwIP 模板的 `platform*.c`、`lscript.ld`、`UserConfig.cmake` 等文件；
   - `echo.c` 可保留但不参与主功能；
4. 确认 CMake 链接库仍包括 `lwip220`、`xiltimer`、`xilstandalone`、`xil`；
5. Build 后下载 ELF。

## 上位机快速测试

```powershell
$c = [System.Net.Sockets.TcpClient]::new('192.168.1.10', 6001)
$s = $c.GetStream()
$w = [System.IO.StreamWriter]::new($s)
$r = [System.IO.StreamReader]::new($s)
$w.AutoFlush = $true
$r.ReadLine()
$w.WriteLine('STATUS')
$r.ReadLine()
$w.WriteLine('START 128')
$r.ReadLine()
$w.WriteLine('STATUS')
$r.ReadLine()
$c.Close()
```

预期首行：

```text
PD_ACQ TCP V1 READY; type HELP
```

## 边界与下一期

本版本故意不实现 `SNAP_READ`。单个快照约 3.12 MB，必须使用有长度限制、偏移量和错误处理的分块二进制协议；直接在 TCP 回调中发送整个快照会造成 lwIP 缓冲耗尽、DMA 服务停顿和数据一致性风险。
