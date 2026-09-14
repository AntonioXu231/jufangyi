# 版本与变更管理规则

本仓库从 `v1.0.0-baseline` 开始实行“保留基线、分支开发、记录变更、标签发布”的规则。

## 不覆盖初始版本的约定

- `baseline/initial` 分支与 `v1.0.0-baseline` 标签代表首次完整上传的项目基线；不在该分支开发、不强制推送、不移动或删除该标签。
- 日常修改从 `main` 创建新分支，例如 `feature/v1.1.0-adjust-threshold`；完成后再合并回 `main`。
- 每一次合并都必须在 `CHANGELOG.md` 增加一条记录，并在 `变更记录/` 新建一份说明。
- 发布一个可复现版本时，为 `main` 增加新标签，例如 `v1.1.0`。标签就是该版本永远可回看的快照。
- 严禁使用 `git push --force`、`git reset --hard` 或通过覆盖提交来替代版本记录。

Git 中“修改同一个 RTL 文件”不会抹掉旧版本：旧内容仍被基线标签和历史提交保存。需要回看初版时，使用：

```powershell
git show v1.0.0-baseline:"merge/pd_feature_bd_2020_2/pd_feature_bd_2020_2.srcs/sources_1/imports/rtl/pd_feature_core.v"
```

## 每次变更的标准流程

```powershell
# 1. 确保从最新版 main 开始
git switch main
git pull

# 2. 创建有版本号和主题的分支
git switch -c feature/v1.1.0-变更主题

# 3. 修改后，创建一份变更说明
#    复制“版本管理/变更记录/模板.md”，改名为“v1.1.0-变更主题.md”

# 4. 查看本次真正改变了什么
git status
git diff

# 5. 提交：代码与说明必须一起提交
git add <修改的文件> "版本管理/CHANGELOG.md" "版本管理/变更记录/v1.1.0-变更主题.md"
git commit -m "feat(v1.1.0): 简短说明"
git push -u origin feature/v1.1.0-变更主题
```

确认仿真、综合或文档审阅完成后，再将分支合并到 `main`，并创建发布标签：

```powershell
git switch main
git merge --no-ff feature/v1.1.0-变更主题
git tag -a v1.1.0 -m "v1.1.0：简短发布说明"
git push origin main --tags
```

## GitHub 保护设置（请在网页完成一次）

进入仓库 **Settings → Rules → Rulesets → New branch ruleset**，创建以下规则：

| 分支模式 | 建议规则 |
|---|---|
| `baseline/initial` | 禁止删除、禁止 force push、仅允许本人修改（最好完全不允许更新） |
| `main` | 禁止 force push；要求 Pull Request；合并前检查变更记录与验证结果 |

这样即使误操作，也无法在 GitHub 上覆盖基线或改写 `main` 历史。

