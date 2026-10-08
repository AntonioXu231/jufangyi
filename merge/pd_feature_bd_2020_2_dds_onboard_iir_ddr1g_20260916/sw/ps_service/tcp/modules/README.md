# PS TCP service modules

These files are private implementation fragments included by `../pd_tcp_service.c` in source order. They are not standalone C translation units: keeping one translation unit preserves the service's shared static state and avoids changing the Vitis link graph.

| Fragment | Responsibility |
|---|---|
| `metadata/pd_tcp_metadata.inc` | Status/config, catalog, event/snapshot metadata, PRPD metadata |
| `spectrum/pd_tcp_spectrum.inc` | FFT commands and PS spectrum records |
| `analysis/pd_tcp_analysis.inc` | Analysis, sweep, alert and batch-analysis commands |
| `scope/pd_tcp_scope_capture.inc` | Short SCOPE frames, live FFT and event-aligned candidate windows |
| `scope/pd_tcp_scope_events.inc` | Single and batched PL event transfers, phase-lock metadata |
| `scope/pd_tcp_scope_envelope.inc` | Full-cycle min/max envelope reduction and SCOPE command dispatch |
| `commands/pd_tcp_commands.inc` | GET, top-level command routing and lwIP callbacks/service entry points |

When copying the PS reference into a Vitis application, copy this entire `modules/` tree to `src/modules/` next to `src/pd_tcp_service.c`. Do not add these `.inc` files to `PROJECT_LIB_SOURCES`; the only TCP service translation unit remains `pd_tcp_service.c`.
