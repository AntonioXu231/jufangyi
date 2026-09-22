# TCP 采集服务干净模板

此目录用于创建新的 Vitis `lwIP Echo Server` 应用时导入，避免复制旧工程的
`build`、`_ide` 或自动生成的 `CMakeLists.txt`。

## 导入规则

1. 用 `platform3` / `standalone_ps7_cortexa9_0` 创建全新 lwIP Echo Server 应用。
2. 删除该新应用 `src` 内的 `main.c` 和 `echo.c`；备份放在 `src` 目录之外。
3. 将本目录的 `main.c` 和 `pd_acquisition_service.inc` 一起复制到新应用的 `src`。
4. 不要将 `.inc` 改回 `.c`，它只由 `main.c` 包含，不能单独参与编译。
5. 保留模板生成的 `platform*.c`、`iic_phyreset.c` 等平台支持文件。

完成后，Vitis 的自动源文件扫描只会发现唯一的应用入口 `main.c`。

