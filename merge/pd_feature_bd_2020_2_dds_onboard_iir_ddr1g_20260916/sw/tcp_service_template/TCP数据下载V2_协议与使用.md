# TCP 数据下载 V2

## 目的

V1 已验证 `START`、`STATUS`、`EVENT`、`SNAP` 等控制与元数据读取。本版在相同
TCP 端口 `6001` 增加归档数据下载，但不改变 PL、DMA、快照槽或 UART 契约。

## 安全边界

- 只允许在采集停止（`STATUS` 中 `run=0`）时执行 `GET`。
- 每次 GET 最多读取 16 KiB；完整 3.12 MB 快照由上位机分块循环读取。
- PS 每次仅向 lwIP 发送 1 KiB，因此不会在 TCP 回调中长时间阻塞。
- 传输期间客户端必须先读完声明的 raw payload，再发送下一条命令。
- `GET` 读取的是 PS DDR 归档区，不读取正在被 PL 写入的原始快照槽。

## 文本控制命令

```text
GET EVENT <index> <offset> <bytes>
GET SNAP  <index> <offset> <bytes>
```

`index` 先用 `EVENT n` / `SNAP n` 确认，`offset` 和 `bytes` 以字节计。合法范围为
`bytes = 1..16384` 且不超过对应归档记录边界。

成功时先返回一行 ASCII：

```text
DATA V2 kind=SNAP index=0 offset=0 bytes=16384 crc32=12AB34CD\r\n
```

紧接着是不带分隔符的 `bytes` 个原始二进制字节。CRC32 使用标准 IEEE CRC-32
（多项式 `0xEDB88320`）；首版 PC 脚本记录传输元数据，下一阶段可添加强制 CRC 校验。

## 上位机完整下载

1. 在 Vitis 新建的干净 lwIP 应用中，将本目录 `main.c` 和
   `pd_acquisition_service.inc` 复制到应用 `src`。不要把 `.inc` 改成 `.c`。
2. Build、下载 V2 ELF，串口看到 `pd acquisition TCP service V2`。
3. 使用 V1 命令完成一次采集，确认 `STATUS` 为 `run=0` 且 `snaps>0`。
4. 在 Windows PowerShell 执行：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
& 'F:\xinya\v5\merge\pd_feature_bd_2020_2_dds_onboard_iir_ddr1g_20260916\sw\tcp_service_template\pd_tcp_download_record.ps1' `
  -Kind SNAP -Index 0 -OutFile 'C:\Users\85980\Desktop\snap_000.bin'
```

事件包示例：

```powershell
& 'F:\xinya\v5\merge\pd_feature_bd_2020_2_dds_onboard_iir_ddr1g_20260916\sw\tcp_service_template\pd_tcp_download_record.ps1' `
  -Kind EVENT -Index 0 -OutFile 'C:\Users\85980\Desktop\event_000.bin'
```

下载完成会产生 `.bin` 和同名 `.bin.json` 元数据文件。

## 验收标准

```text
DOWNLOAD_PASS bytes=3120000 file=...snap_000.bin
```

并且文件大小与 `SNAP 0` 返回的 `bytes` 完全一致。

