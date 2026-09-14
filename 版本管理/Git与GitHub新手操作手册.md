# Git 与 GitHub 新手操作手册

适用仓库：`F:\xinya\v5`  
远程仓库：`https://github.com/AntonioXu231/jufangyi.git`

本手册的目标不是背命令，而是让你每一次上传都清楚回答四个问题：**我改了什么？为什么改？是否验证？如何回退？**

---

## 1. 先理解四个概念

| 名词 | 可以把它理解为 | 在本项目中的例子 |
|---|---|---|
| 工作区 | 你电脑里正在编辑的文件夹 | `F:\xinya\v5` |
| Git | 本地的“带时间线文件保险箱” | 每次 `commit` 保存一个可回看的快照 |
| GitHub | 放在网上的 Git 仓库 | `AntonioXu231/jufangyi` |
| 分支 | 同一项目的一条独立工作线 | `main` 是正式线；`feature/...` 用来做一个新修改 |
| 提交（commit） | 带说明的一次本地存档 | “提高事件门限并验证仿真” |
| 推送（push） | 将本地提交传到 GitHub | `git push` |
| 标签（tag） | 给某个确定快照贴上版本号 | `v1.0.0-baseline` |

最重要的一点：**`git commit` 只是保存在本机；`git push` 才上传到 GitHub。**

---

## 2. 你现在已经完成的初始化

下面这些事情已经由本仓库完成，不需要重复执行：

```text
本地仓库：F:\xinya\v5\.git
远程仓库：origin -> https://github.com/AntonioXu231/jufangyi.git
正式分支：main
初始基线：baseline/initial 分支 + v1.0.0-baseline 标签
本地代理：127.0.0.1:7890（仅本仓库 Git 使用）
```

每次开始工作前，打开 PowerShell，执行：

```powershell
cd F:\xinya\v5
git status --short --branch
```

正常、干净的结果应类似：

```text
## main...origin/main
```

如果下面出现文件名，说明你有尚未保存为 Git 提交的改动；此时不要直接 `git pull` 或切换分支，先看清楚这些改动来自哪里。

---

## 3. 一次标准“修改并上传”的完整实例

假设你要把事件门限相关逻辑调整为 `v1.1.0`，主题叫“threshold”。不要直接改 `main`，按下面做。

### 第一步：回到最新版正式版本

```powershell
cd F:\xinya\v5
git switch main
git pull
git status --short --branch
```

预期：状态干净，且在 `main` 分支。

### 第二步：创建自己的修改分支

```powershell
git switch -c feature/v1.1.0-threshold
```

名称格式固定为：

```text
feature/v版本号-英文或拼音主题
```

例如：

```text
feature/v1.1.0-threshold
feature/v1.1.0-prpd-window
fix/v1.0.1-axi-timeout
docs/v1.0.1-learning-note
```

现在可以编辑 RTL、仿真或文档。一次分支只做一个主题，避免把多个互不相关的改动混在一起。

### 第三步：先检查，而不是立刻上传

```powershell
git status
git diff
```

- `git status` 告诉你有哪些文件新增、修改、删除。
- `git diff` 告诉你每一行具体怎样变化。

这一步是防止误上传、误覆盖的关键。对于 FPGA 项目，重点检查是否意外修改了：XDC、时钟/CDC、AXI 地址映射、BD、寄存器定义。

### 第四步：填写本次变更说明

复制模板：

```powershell
Copy-Item "版本管理\变更记录\模板.md" "版本管理\变更记录\v1.1.0-threshold.md"
```

填写以下内容：为什么改、修改哪些文件、哪些接口没有改、如何验证、结果如何、如何回退。然后更新 `版本管理\CHANGELOG.md` 的 `[Unreleased]` 区域。

### 第五步：只暂存本次应上传的文件

不要第一反应就用 `git add .`。第一次学习阶段建议明确列文件：

```powershell
git add "merge\pd_feature_bd_2020_2\pd_feature_bd_2020_2.srcs\sources_1\imports\rtl\pd_feature_core.v"
git add "版本管理\CHANGELOG.md"
git add "版本管理\变更记录\v1.1.0-threshold.md"
git diff --cached
```

`git diff --cached` 显示的就是下一次提交将保存的内容。确认无误才继续。

### 第六步：创建本地提交

```powershell
git commit -m "feat(v1.1.0): adjust event threshold"
```

提交信息格式：

| 前缀 | 何时使用 | 例子 |
|---|---|---|
| `feat` | 新增功能 | `feat(v1.1.0): add PRPD export` |
| `fix` | 修复错误 | `fix(v1.0.1): preserve AXIS tlast` |
| `docs` | 只改文档 | `docs: explain DMA data path` |
| `test` | 测试或仿真激励 | `test: add low-amplitude stimulus` |
| `chore` | 配置、忽略项等维护 | `chore: ignore Vivado UI files` |

### 第七步：上传你的分支

确保代理软件仍启动后，执行：

```powershell
git push -u origin feature/v1.1.0-threshold
```

首次推送某个分支使用 `-u origin 分支名`；以后在同一分支只需 `git push`。

### 第八步：在 GitHub 网页创建 Pull Request

打开仓库网页。GitHub 通常会显示 **Compare & pull request** 按钮；点击后确认：

```text
base: main
compare: feature/v1.1.0-threshold
```

标题写：`v1.1.0：调整事件门限`。说明中粘贴变更记录的摘要和验证结果。确认后创建 Pull Request；检查无误再点击 **Merge pull request**。

合并的好处是：GitHub 自动保留“为什么改、谁改、改之前是什么”的完整记录。不要在网页手工拖拽上传同名 RTL 文件来替代此流程。

### 第九步：发布新版本标签

合并后回到本地：

```powershell
git switch main
git pull
git tag -a v1.1.0 -m "v1.1.0：调整事件门限，详见 CHANGELOG"
git push origin main --tags
```

此后 `v1.1.0` 即为可回看的正式版本。不要重用、移动或删除已经发布的标签。

---

## 4. 只新增文档时的简化流程

例如新增一份学习笔记：

```powershell
cd F:\xinya\v5
git switch main
git pull
git switch -c docs/v1.0.1-adc-note

# 编辑或新建文档后：
git status
git add "项目学习路线\你的新笔记.md" "版本管理\CHANGELOG.md"
git diff --cached
git commit -m "docs(v1.0.1): add ADC learning note"
git push -u origin docs/v1.0.1-adc-note
```

然后仍然在 GitHub 创建 Pull Request 合并到 `main`。

---

## 5. 如何查看旧版本、比较版本与回退

### 查看版本列表

```powershell
git tag
git log --oneline --decorate -10
```

### 比较当前版本和初始版本

```powershell
git diff v1.0.0-baseline..main --stat
git diff v1.0.0-baseline..main -- "merge/pd_feature_bd_2020_2/pd_feature_bd_2020_2.srcs/sources_1/imports/rtl/pd_feature_core.v"
```

### 只查看初始版本的一个文件，不改变当前工作区

```powershell
git show v1.0.0-baseline:"接口契约v3.0.md"
```

### 某次提交发现问题，推荐的安全回退方式

不要用 `reset --hard`。在 `main` 已上传的情况下，创建一个“反向提交”：

```powershell
git log --oneline
git revert <要撤销的提交号>
git push
```

这样错误提交和撤销原因都会保留，不会覆盖历史。

---

## 6. GitHub 网页上最常用的入口

| 位置 | 用途 |
|---|---|
| **Code** | 浏览文件、切换分支、查看标签 |
| **Commits** | 按时间查看每次改动 |
| **Branches** | 查看 `main`、`baseline/initial`、功能分支 |
| **Tags / Releases** | 查看每个正式版本快照 |
| **Pull requests** | 审阅并合并功能分支 |
| **Settings → Rules → Rulesets** | 禁止强制推送、保护基线与 `main` |

建议立即设置两条保护规则：

1. `baseline/initial`：禁止删除、禁止 force push、禁止直接更新。
2. `main`：禁止 force push；建议要求 Pull Request 后才能合并。

---

## 7. 代理、登录与常见报错

本项目 Git 已使用本地代理 `127.0.0.1:7890`。若你的代理软件关闭，`git push` 可能报：

```text
Failed to connect to github.com:443
```

处理方法：先启动代理软件并确认系统代理开启，再执行：

```powershell
git push
```

查看本仓库代理：

```powershell
git config --local --get http.proxy
git config --local --get https.proxy
```

若端口变了，例如改为 `7897`，只改当前仓库：

```powershell
git config --local http.proxy http://127.0.0.1:7897
git config --local https.proxy http://127.0.0.1:7897
```

GitHub 不再接受账户密码作为 Git HTTPS 密码。出现登录窗口时，在浏览器中自己完成授权；绝不在聊天、代码或提交信息中填写账号密码、访问令牌或代理密码。

---

## 8. 每次上传前的 30 秒检查清单

- [ ] 当前不是 `baseline/initial` 分支。
- [ ] 分支名称包含版本号与主题。
- [ ] `git status` 中只有本次有意修改的文件。
- [ ] 已阅读 `git diff` 或 `git diff --cached`。
- [ ] 已更新 `CHANGELOG.md` 和对应变更说明。
- [ ] 仿真/综合/实现的结果被如实记录；未执行就写“未执行”。
- [ ] 不使用 `git push --force`、`git reset --hard`。
- [ ] 上传后在 GitHub 的 Pull Request 中再次检查文件差异。

