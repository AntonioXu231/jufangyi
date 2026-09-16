# 第 2 步教程：给 `pd_ddr_defines.vh` 增加四槽地址与容量宏

> 配套文档：`pd_ddr_slot_mgr_decisions_20260916.md`（决议冻结记录，含 Codex 回复与 §9 增补）  
> 本步性质：**纯常量新增**，不写任何逻辑、不碰任何现有宏的值。  
> 目标文件：`pd_feature_bd_2020_2.srcs/sources_1/imports/rtl/pd_ddr_defines.vh`（原 96 行）
>
> **状态：已实施并通过自检（2026-09-16）。** 原 §3–§4 列出的决策点 A/B/C 已由助手代为定案并写入文件
> —— 工程文件内新增 `// 3b) 四槽快照区` 一节，含完整对齐推导注释与 9 个宏；
> §6 的自检脚本已在改动后实测，A/B/C 三组共 20 项判定全部通过、0 项失败。
> §7 的「动作清单」由此转为历史记录，本文件保留其推导与教学价值。



---

## 0. 本步的边界（做什么 / 不做什么）

**做**：

- 在第 83 行 `DDR_SNAP_SIZE` 之后，新增一节注释 + 一组四槽常量宏。
- 沿用文件现有的命名风格与常量书写风格（`32'hXXXX_XXXX`，下划线分隔）。

**不做**：

- 不改动第 21–93 行任何**现有**宏的名字或数值。
- 不新建 RTL，不动 `pd_ddr_axil.v` / `pd_ddr_wr_top.v` / `pd_ddr_ring_wr.v`。
- 不动工程目录之外的 `接口契约v3.0.md` 与 `版本管理/`。

**为什么这步能独立先做**：宏是编译期文本常量，它不依赖修正 3（`drop_count` 位域归属）、也不依赖修正 4（`copy_start`/`copy_grant` 握手形式）。握手形式只决定端口表，不决定地址常量。所以本步可以干净收口。

---

## 1. 背景知识：Verilog `` `define `` 的六条机制

你写宏之前，先把这六条记住。**这六条里有三条是新手必踩的坑。**

### 1.1 它是预处理器，不是变量

`` `define `` 和 C 语言的 `#define` 是同一类东西：在**编译（elaboration）之前**做**纯文本替换**。  
推论有三条：

- 它不占任何硬件资源，没有 LUT/FF 开销；
- 它**没有"运行时"**，不存在"宏的值变了"这回事；
- 它**不是类型化的**，你写 `32'h1000_2000` 只是文本，宽度语义在使用点才被解释。

对比：`parameter` / `localparam` 是**语言级常量**，有类型、有位宽、作用域在模块内、可以在 `defparam`/实例化时覆盖。宏是"文本级"，作用域是"从定义点到文件末尾（或 `undef`）"。

**这个文件为什么用宏？** 因为它要被多个模块 `include` 共享，而且 PS 侧文档要能直接引用同一组数字。宏是唯一能跨模块共享常量的 Verilog-2001 手段（SystemVerilog 的 `package` 当然更好，但这个工程用的是 `.vh` 约定）。

### 1.2 定义时不带反引号，使用时必须带

```verilog
`define DDR_SLOT_SIZE 32'h00C0_0000     // 定义：不带反引号
wire [31:0] x = `DDR_SLOT_SIZE;         // 使用：必须带反引号
```

漏掉反引号 `DDR_SLOT_SIZE` 会被综合器当成一个**信号名**去解析，报 `is not declared` 或者被推成一根悬空 wire——**而且不一定是硬错误**，可能只是一条 warning，然后你得到一个 X。这类问题最难查，因为它不阻断编译。

### 1.3 替换体到行尾为止，行尾**不能有分号**（坑 1）

```verilog
`define DDR_SLOT_SIZE 32'h00C0_0000;     // ← 这个分号是错的
```

展开后使用点变成 `32'h00C0_0000;`，多一个分号，语法错。  
对照本文件第 21 行 `PD_CH_NUM 4`、第 80 行 `DDR_RING_BASE 32'h1000_2000`——**全都没有分号**。

需要跨行时用反斜杠 `\` 续行。本文件的宏全是单行，建议你也保持单行。

### 1.4 替换体里只要是表达式，一律套括号（坑 2）

宏展开是**纯文本**，所以表达式里各运算符的优先级是由**调用点**决定的，不是由定义点决定的：

```verilog
`define A 1+2
wire [7:0] x = `A * 3;      // 展开成 1+2*3 = 7   ← 不是 9！
```

本文件严格遵守了这条规则，你可以自己验证：

- 第 21 行 `PD_CH_NUM 4` —— 单个数字，**不加**括号；
- 第 23 行 `PD_SAMPLE_W (`PD_CH_NUM * `PD_ADC_W)` —— 是乘法，**加**括号；
- 第 26 行 `PD_BLK_BYTES (`PD_BLK_W / 8)\` —— 是除法，**加**括号；
- 第 93 行 `DDR_BURST_BYTES (`DDR_BURST_BLOCKS * `PD_BLK_BYTES)` —— 加括号。

**规律：宏体是一个"算式"就加括号，是一个"数"就不加。** 你的四槽基址如果走派生路线（见 §4），每条都必须带括号。

### 1.5 宏体内部可以引用其他宏，同样带反引号

第 25–26 行就是嵌套引用的范例：

```verilog
`define PD_SAMPLE_W  (`PD_CH_NUM * `PD_ADC_W)   // 引用两个宏
`define PD_BLK_W     (`PD_SAMPLE_W * `PD_WORD_PER_BLK)  // 再引用上一个结果
`define PD_BLK_BYTES (`PD_BLK_W / 8)
```

展开是**递归**的，最终收敛成 `((((4) * (12)) * (4)) / 8)` 这样一个纯数字表达式，综合器做常量折叠。这是你可以放心做多层派生的依据。

### 1.6 头尾的 `ifndef` 是重复包含保护

第 15–16 行与第 95 行：

```verilog
`ifndef _PD_DDR_DEFINES_VH_
`define _PD_DDR_DEFINES_VH_
...
`endif
```

**你新增的宏必须写在这两者之间**（即第 16 行之后、第 95 行之前）。写到 `endif` 后面，等于写到了文件外——如果这个 `.vh` 被两个模块 `include`，就会出现宏重定义告警。

---

## 2. 把"对齐铁律"推到你能自己复现

文件第 68–71 行写了这条铁律，但没有展开"为什么"。这一步必须真正想通，否则你以后每次改地址都要重新推。

### 2.1 三条独立约束

| 编号  | 约束来源                                              | 数学表达                  |
| --- | ------------------------------------------------- | --------------------- |
| (a) | DataMover 关闭 DRE 时起始地址须 8 B 对齐（= AXI 数据位宽 64 bit） | `addr ≡ 0 (mod 8)`    |
| (b) | 契约 §3.3 要求块 24 B 对齐                               | `addr ≡ 0 (mod 24)`   |
| (c) | 4 KiB 页边界对齐（避免跨页/跨 bank 的额外开销）                    | `addr ≡ 0 (mod 4096)` |

### 2.2 求交集

- `24 = 8 × 3`，所以"能被 24 整除"已经蕴含"能被 8 整除" → **(a) 被 (b) 吸收**，不用单独考虑。
- 剩下要同时被 4096 和 24 整除 → 被二者的最小公倍数整除：
  - `4096 = 2¹²`
  - `24   = 2³ × 3`
  - `LCM(4096, 24) = 2¹² × 3 = 12288 = 0x3000` ✔

**结论：所有槽基址与槽容量都必须是 `0x3000` 的整数倍。**

### 2.3 为什么 `0x2000_0000` 一票否决

```
0x2000_0000 = 2²⁹
```

2²⁹ 的质因数**只有 2，没有因子 3**。所以它 `mod 24 = 8 ≠ 0`，约束 (b) 直接不成立。  
注意它 `mod 4096 = 0` 是满足的——**所以它不是"完全不对齐"，而是"恰好差在 24 B 这一条上"**。这正是它危险的地方：看起来是个漂亮的 2 的幂，肉眼检查很容易放过。

### 2.4 为什么 `+0x1000` 恰好补上缺的因子

```
0x1000 = 4096 = 24 × 170 + 16   →  4096 ≡ 16 (mod 24)
0x2000_0000 ≡ 8 (mod 24)
8 + 16 = 24 ≡ 0 (mod 24)   ✔
```

同时加 `0x1000` **不破坏** 4 KiB 对齐，因为 `0x2000_1000` 仍然是 `0x1000` 的整数倍。

换个更直观的说法：`0x1000` 是 4 KiB 网格的步长，`0x3000` 是 12 KiB 网格的步长。`0x2000_0000` 落在 4 KiB 网格上但不是 12 KiB 网格点，**沿 4 KiB 网格向前走一格就撞上了 12 KiB 网格点**。

### 2.5 步长 12 MiB 为什么天然合格

```
12 MiB = 0x00C0_0000 = 12,582,912 = 1024 × 12288
```

步长本身是 12288 的整数倍，所以**从任一个已对齐的基址出发，连续加 12 MiB 都停在网格上**。  
这就是为什么本次修正"只改基址、不动步长"是安全的。

### 2.6 四个槽的落点（已用脚本实测，不是手算）

| 槽     | 基址            | 末字节           | mod 12288 | mod 24 | mod 4096 | mod 8 |
| ----- | ------------- | ------------- | --------- | ------ | -------- | ----- |
| slot0 | `0x2000_1000` | `0x20C0_0FFF` | 0         | 0      | 0        | 0     |
| slot1 | `0x20C0_1000` | `0x2180_0FFF` | 0         | 0      | 0        | 0     |
| slot2 | `0x2180_1000` | `0x2240_0FFF` | 0         | 0      | 0        | 0     |
| slot3 | `0x2240_1000` | `0x2300_0FFF` | 0         | 0      | 0        | 0     |

- 槽区整体：`0x2000_1000` .. `0x2300_1000`，恰好 48 MiB。
- 槽区之后到 1 GiB 顶端仍空余 **464 MiB**。
- `0x2000_0000` 对照行：`mod 12288 = 8192`、`mod 24 = 8` —— 不合格。

### 2.7 一个容易搞错的边界细节：末地址 vs 末地址+1

```
slot3 末字节       0x2300_0FFF   mod 24 = 23   ← 不在网格上
slot3 哨兵（末+1） 0x2300_1000   mod 24 = 0    ← 在网格上
```

这不是错误，而是**必然结果**：只要"起始地址 ≡ 0 (mod 24)"且"拷贝长度 ≡ 0 (mod 24)"，那么"最后一个字节 + 1"必然也在网格上。

→ **推论（写 `o_cfg_err` 时会用到）**：槽边界判断要用**哨兵地址**做开区间上界，写 `dst_addr + len <= 0x2300_1000`，而不是 `<= 0x2300_0FFF`。用错会白丢一个字节容量的判断，更糟的是可能把"恰好写满整槽"误判为越界。

---

## 3. 命名与位置（你的决策点 A/B）

### 3.1 位置

紧跟第 83 行 `DDR_SNAP_SIZE` 之后、仍在"3) DDR 分区"章节内，插入一个小节注释：

```
// -----------------------------------------------------------------------------
// 3b) 四槽快照区（决议：pd_ddr_slot_mgr_decisions_20260916.md §D1）
// -----------------------------------------------------------------------------
```

注意第 93 行 `DDR_BURST_BYTES` 属于"4) 突发参数"章节，**不要插到它后面去**。

### 3.2 命名（**决策 A，由你定**）

现有风格是 `<域>_<对象>_<属性>`：`DDR_RING_BASE` / `DDR_SNAP_SIZE`。全大写、下划线分词。两个候选：

**方案 A：平行字面量**

```
DDR_SLOT0_BASE  DDR_SLOT1_BASE  DDR_SLOT2_BASE  DDR_SLOT3_BASE
DDR_SLOT_SIZE   DDR_SLOT_NUM
```

- 优点：与 `DDR_RING_*` / `DDR_SNAP_*` 完全同构，PS 侧看代码一眼就懂；`grep 0x2000_1000` 能直接命中。
- 缺点：四个平行宏，宏**不是数组**，将来在 slot_mgr 里按索引取址只能写 `case` 或四路 mux。

**方案 B：基址 + 索引现算**

```
DDR_SLOT_AREA_BASE  DDR_SLOT_SIZE  DDR_SLOT_NUM  DDR_SLOT_IDX_W
```

- 优点：`busy_slot[1:0]` 索引可直接参与运算，改槽数只改 `DDR_SLOT_NUM`。
- 缺点：`DDR_SLOT0_BASE` 这类"肉眼可查"的锚点没有了。

**我的建议：混合（A 的锚 + B 的派生）。** 保留 `DDR_SLOT0_BASE` 作为唯一手写的字面量锚（对齐推导注释就挂在它上面），其余三个由它派生；再补 `DDR_SLOT_SIZE` / `DDR_SLOT_NUM` / `DDR_SLOT_AREA_END`。这样"改一处全联动"和"肉眼可查"两个好处都要到了。

### 3.3 还需不需要一个"槽区结束"宏（**决策 B**）

`pd_ddr_slot_mgr` 里要判 `freeze_base + freeze_len <= 槽区上界`。这个上界是**编译期常量**，所以建议定义出来：

```
DDR_SLOT_AREA_END  =  0x2300_1000        // 哨兵，开区间上界
```

用哨兵而不是 `0x2300_0FFF`，理由见 §2.7。  
另外 `DDR_SLOT_IDX_W = 2` 也有用——它决定 `busy_slot` 的位宽。要不要单独立一个宏，你定。

---

## 4. 两条写法路线（**决策 C**）

### 路线 1：全字面量

七个宏全部手写 `32'hXXXX_XXXX`。

- 优点：肉眼直读，无展开歧义，`grep` 友好。
- 风险：手抄错一位数字**没有任何机制会拦住你**。

### 路线 2：锚 + 派生

```
`define DDR_SLOT0_BASE    32'h2000_1000
`define DDR_SLOT_SIZE     32'h00C0_0000
`define DDR_SLOT1_BASE    (`DDR_SLOT0_BASE + `DDR_SLOT_SIZE)
`define DDR_SLOT2_BASE    (`DDR_SLOT1_BASE + `DDR_SLOT_SIZE)
`define DDR_SLOT3_BASE    (`DDR_SLOT2_BASE + `DDR_SLOT_SIZE)
`define DDR_SLOT_NUM      4
`define DDR_SLOT_AREA_END (`DDR_SLOT3_BASE + `DDR_SLOT_SIZE)
```

（上面只是**格式示范**，用来让你看清括号位置与派生写法；实际那一节的注释推导由你按 §2 补全。）

- 优点：改一处全联动；`DDR_SLOT_AREA_END` 由 `SLOT3 + SIZE` 推出来，不可能与槽区位图脱节。
- 缺点：加法在**编译期**求值，位宽规则是"两个 32 位操作数相加 → 结果 32 位"，会丢弃进位。

**关于位宽的提醒**：`32'h2000_1000 + 32'h00C0_0000 = 0x20C0_1000 < 2³²`，本工程不溢出，安全。但如果将来基址往高位走，就要留意这个截断规则——用 `33'h` 或加宽字面量可以规避。

**我建议路线 2。** 理由不是"少打字"，而是**它把一致性交给编译器而不是交给你**：只要锚点对、步长对，剩下三个槽不可能错位。

---

## 5. 三种典型写错方式（写完拿这个清单对一遍）

| 错误     | 症状                                             | 正确写法              |
| ------ | ---------------------------------------------- | ----------------- |
| 行尾带分号  | 使用点语法错，报 `syntax error near ";"`               | 宏体后**不加**分号       |
| 派生宏没括号 | 数值莫名偏差（如少了几十），无报错                              | 宏体是算式一律 `( ... )` |
| 漏写反引号  | `is not declared` 或悬空 wire + 一条 warning，跑出来是 X | 定义不带、使用必带         |

对**宏文件本身**额外注意：`.vh` 不参与综合。也就是说——**你改完这个文件，Vivado 不一定会立刻给你任何反馈**，因为此刻还没有任何 `.v` `include` 它。所以本步的验证只能靠 §6 的自检，不能指望综合器。

---

## 6. 写完之后的三层自检


### 第 1 层：算术层（必做，立刻可做）

把宏值抽出来做 mod 验算。**宏表达式在 Python 里无法直接 eval**，所以下面这段脚本先做三件事：用正则抽出全部 `define、剥掉行尾注释、把 `32'hXXXX` 转成十进制、把反引号引用的宏名递归展开，最后才求值。

**已在本工程实测通过**（改动后宏总数 44，A/B/C 三组共 20 项判定，0 项失败）。

在工程根目录执行：

```bash
python - <<'PY'
import re, pathlib
p = pathlib.Path("pd_feature_bd_2020_2.srcs/sources_1/imports/rtl/pd_ddr_defines.vh")
txt = p.read_text(encoding="utf-8", errors="ignore")
d = {}
for m in re.finditer(r"^\s*`define\s+(\w+)\s+([^\n]*)", txt, re.M):
    d[m.group(1)] = m.group(2).split("//")[0].strip()

def expand(s, depth=12):
    for _ in range(depth):
        out = re.sub(r"`(\w+)", lambda m: "(" + d[m.group(1)] + ")" if m.group(1) in d else m.group(0), s)
        if out == s: break
        s = out
    return s

def toint(s):
    return re.sub(r"\b(\d+)'[hH]([0-9A-Fa-f_]+)",
                  lambda m: str(int(m.group(2).replace("_",""),16)), s)

def val(n): return int(eval(toint(expand(d[n]))))

L = 12288
fail = 0
ADDR = ["DDR_RING_BASE","DDR_RING_SIZE","DDR_SNAP_BASE","DDR_SNAP_SIZE",
        "DDR_SLOT0_BASE","DDR_SLOT1_BASE","DDR_SLOT2_BASE","DDR_SLOT3_BASE",
        "DDR_SLOT_SIZE","DDR_SLOT_AREA_BASE","DDR_SLOT_AREA_END"]
CNT  = {"DDR_SLOT_NUM":4, "DDR_SLOT_IDX_W":2}

print("=== A. 地址/长度类：必须 mod 12288 == 0 ===")
for n in ADDR:
    v = val(n); ok = (v % L == 0); fail += (not ok)
    print("  %-18s 0x%08X  mod12288=%5d  mod24=%2d  %s"
          % (n, v, v % L, v % 24, "OK" if ok else "**FAIL**"))

print("\n=== B. 计数/位宽类：校验取值，不做对齐 ===")
for n, exp in CNT.items():
    v = val(n); ok = (v == exp); fail += (not ok)
    print("  %-18s = %-4d 期望 %-4d %s" % (n, v, exp, "OK" if ok else "**FAIL**"))

print("\n=== C. 派生关系自洽 ===")
b0, sz, e = val("DDR_SLOT0_BASE"), val("DDR_SLOT_SIZE"), val("DDR_SLOT_AREA_END")
rel = [("AREA_END = SLOT0 + NUM*SIZE", e == b0 + val("DDR_SLOT_NUM")*sz),
       ("区跨度 = 48 MiB", e - val("DDR_SLOT_AREA_BASE") == 48*1024*1024),
       ("SIZE = 1024 x 0x3000", sz == 1024*L),
       ("槽间距 = SIZE", val("DDR_SLOT1_BASE")-b0 == sz == val("DDR_SLOT3_BASE")-val("DDR_SLOT2_BASE")),
       ("IDX_W = log2(NUM)", val("DDR_SLOT_IDX_W") == (val("DDR_SLOT_NUM")-1).bit_length()),
       ("无区重叠 RING/SNAP/SLOT", b0 >= val("DDR_RING_BASE")+val("DDR_RING_SIZE")
                                  and val("DDR_SNAP_BASE") >= val("DDR_RING_BASE")+val("DDR_RING_SIZE")
                                  and b0 >= val("DDR_SNAP_BASE")+val("DDR_SNAP_SIZE")),
       ("哨兵语义: 末字节 mod24 == 23", (e-1) % 24 == 23)]
for name, ok in rel:
    fail += (not ok)
    print("  %-34s %s" % (name, "OK" if ok else "**FAIL**"))

print("\n>>> 总判定：%s（%d 项失败）" % ("全部通过" if fail == 0 else "存在失败", fail))
PY
```

**为什么 §A 与 §B 必须分开（一条已踩过的坑）**：

`DDR_SLOT_NUM`（= 4）与 `DDR_SLOT_IDX_W`（= 2）是**计数与位宽**，不是地址。把它们混进地址列表做
`mod 12288`，必然非零，跑出两行**假 FAIL**。

> **会误报的检查工具比没有检查工具更糟** —— 用两次你就会开始忽略它的 FAIL，
> 然后真正的对齐错误也会一起被忽略。所以地址类与计数类必须用不同判据。

**判据**：

- §A 共 11 行，每行都以 `OK` 结尾；
- §B 两项取值正确（`NUM = 4`、`IDX_W = 2`）；
- §C 七项派生关系全 `OK`，其中「`AREA_END = SLOT0 + NUM × SIZE`」与「无区重叠」最关键；
- 末行必须是「总判定：全部通过（0 项失败）」；
- 顺带确认现有四个 `DDR_RING_*` / `DDR_SNAP_*` 的值与改动前一致（没被误伤）。

这个脚本不只服务于本步——**以后每次改 DDR 地址都跑它**，比肉眼看可靠。

### 第 2 层：语法层（可选）

如果你想在真正的 Verilog 语义下确认宏能被解析，最轻的做法是加一个十行的自检文件（`include` 这个 `.vh` + 一段 `initial $display`），跑一次 Vivado behavioral simulation。**要不要做由你定**——它会往工程里多引入一个文件（综合时需设为 `disable` 或干脆不加入综合集）。不做也能过，因为第 1 层已经把数值验证掉了，剩下的只是"语法是否合法"，而你的写法会与我 §5 的清单逐条对照。

### 第 3 层：交叉层（我来做）

你贴回改动后，我按 `pd_ddr_snap_copy`（第 107–115 行 `pd_align24_check`、第 111–115 行 `start_valid`）与 `pd_ddr_axil` 的实际端口，逐条核宏名是否需要在下游引用一致。

---

## 7. 你的动作清单

1. 打开 `pd_ddr_defines.vh`。
2. 在第 83 行 `DDR_SNAP_SIZE` 之后插入：`// 3b) 四槽快照区` 注释小节 + 对齐推导注释（把 §2.2–§2.6 的推导浓缩成 5–8 行注释，**包括 `0x2000_0000` 缺因子 3 这一点**）+ 一组宏。
3. 决策 A（命名方案）、决策 B（是否定义 `DDR_SLOT_AREA_END` / `DDR_SLOT_IDX_W`）、决策 C（字面量 vs 派生）各自拍板，并说明理由。
4. 跑 §6 第 1 层的脚本，把输出贴给我。
5. 贴回你新写的那一段（注释 + 宏）。

---

## 8. 本步解不了、留给后续的两个开口

这两条不影响本步（宏不依赖握手形式），但会卡住第 3 步的端口表，所以本步收口后要优先关掉：

- **修正 3｜`drop_count` / `req_overflow` / `drop_busy` 的寄存器位域归属。**  
  `0x4C`–`0x58` 已被 `SLOT_CTRL` / `SLOT_STATUS` / `SLOT_LAST` / `SLOT_SEQ` 占满，`0x5C` 要留给 16 B 对齐。这三个东西住哪个地址、各几位，必须先规划出来。
- **修正 4｜"手动优先"的实现位置。**  
  若在顶层用与门挡掉 slot_mgr 的 `copy_start`，slot_mgr 会**误以为自己获准启动**，清掉 pending 并回 `req_ack`，而数据根本没搬——这份快照无声丢失。必须在"`o_copy_start`（请求）+ `i_copy_grant`（获准）两线握手"与"给 slot_mgr 一个手动待发输入让其自行避让"之间二选一。

---

*文档路径：工程目录内。本次未改动 `pd_ddr_defines.vh` 及任何 RTL。*
