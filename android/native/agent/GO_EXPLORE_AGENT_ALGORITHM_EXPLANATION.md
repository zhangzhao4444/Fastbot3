/**
 * @authors Zhao Zhang
 */

# GOExploreAgent 算法详解与优势分析

## 执行摘要

本文档详细解释 **GOExploreAgent** 在 Android UI 测试中采用的 **Go-Explore 风格「先回再探」(First return, then explore)** 算法的原理与实现。与 DoubleSarsa、Curiosity 等基于 Q 值或内在奖励的策略不同，GOExploreAgent 显式维护一个 **archive（关键 UI 状态集合）**，周期性地从 archive 中**选择一个 cell（UI 状态）作为子目标**，通过 **BFS 路径导航回该 cell**，然后在该 cell 附近进行若干步**局部探索**，再重新选择新的 cell，如此循环。

**当前实现**：GOExploreAgent 在共享状态图 `_outEdges` 与自维护的 `_archive`（cell 元数据）、路径缓存 `_pathToCell`、探索阶段 `_exploreTargetCell` / `_exploreStepsLeft` 之上，实现：

- 将 UI 状态 `state->hash()` 直接作为 **cell_id**；
- archive 中对每个 cell 记录 `seenTimes` / `chosenTimes`；
- 按 `1/(1+seenTimes) + 1/(1+chosenTimes)` 这类权重在 archive 中 **加权随机选择 cell**，并可结合 BFS 距离对较远 cell 做轻微放大权重；
- 利用 `_outEdges` 上的 **BFS 路径**从当前状态导航回目标 cell；
- 到达 cell 后在该状态 **连续探索 `kExploreStepsPerCell=10` 步**（少访问动作优先，辅以 `kExploreRandomEpsilon=0.15` 的随机）；
- 与现有 agent 一样支持 **防卡死（`kBlockBackThreshold=5` / `kBlockDeepLinkThreshold=10` / `kBlockCleanRestartThreshold=15`，分别触发 BACK / DEEP_LINK / CLEAN_RESTART）**。

GOExploreAgent 不依赖环境「状态快照/重置」，采用的是 **Go-Explore 方案 B：子目标重访 + 基于图的导航**。使用方式：命令行传入 `--agent goexplore`（或配置 `algorithm=goexplore`），JNI 层映射到 `AlgorithmType::GoExplore`。

---

## 一、算法背景与核心问题

### 1.1 Go-Explore 思想在 Monkey/Fastbot 中的动机

在 UI 自动化探索中，随机策略、BFS/DFS、Frontier、Curiosity/DoubleSarsa 等方法往往存在两类典型问题：

1. **Detachment（脱节）**  
   之前曾经到达过的「远端、高价值」UI 状态（例如深层设置页面），随着状态图扩张与策略更新，很少被再次访问，后续探索难以在这些前沿继续深入。

2. **Derailment（脱轨）**  
   想「回到某个前沿状态再从那里探索」时，多数算法只是从当前策略上加噪声，很难稳定「站到」那个状态，再向前迈出新的步子。

Go-Explore 提出的三条核心原则是：

- **记住（Remember）**：显式存档「有潜力」状态（archive）；
- **先回再探（First return, then explore）**：先可靠回到这些状态，再从那里探索；
- **先求解再鲁棒化（Solve then robustify）**：探索阶段可以在较确定的环境里进行。

在 Monkey/Fastbot 场景下，即使没有 Atari 那样的精确重置能力，我们仍希望实现一个 **弱化版 Go-Explore**：在当前已构建的 UI 状态图上，**选择一个历史关键状态 → 用 BFS 路径导航回去 → 从那继续探索一小段时间**。

### 1.2 GOExploreAgent 的定位

GOExploreAgent 是一个 **独立的、基于 archive + 图导航的探索策略**：

- **不依赖 Q 值/奖励/内在奖励**，也不与 DoubleSarsa/Frontier/Curiosity 组合；
- 显式维护 archive：每个已访问 UI 状态对应一个 cell，记录其被到达/被选为目标的次数；
- 按权重从 archive 中选一个 cell，**要么就是当前状态 → 直接从当前状态探索**，要么是历史状态 → 先 BFS 导航回去，再局部探索；
- 在「有限步数内提高广度与深度覆盖」的前提下，缓解 detachment/derailment。

---

## 二、算法原理

### 2.1 基本概念

**Cell 与 UI 状态**：

- 每个 UI 状态对应一个 cell，`cell_id = state->hash()`（`uintptr_t`）。
- 通过共享状态图 `_outEdges`，维护 `sourceHash -> (action, targetHash)` 的有向边。

**Archive**：

- 结构：`archive[cell_id] = CellMeta{ seenTimes, chosenTimes }`。
- 语义：
  - `seenTimes`：执行过程中到达该 cell 的次数（仅在 `moveForward` 中真实落地时自增）；
  - `chosenTimes`：该 cell 被选为「回去并探索的目标」的次数。

**路径与探索阶段**：

- `_pathToCell`：从当前状态到目标 cell 的 BFS 路径（若存在），每步包含 `(sourceHash, action)`；
- `_pathIndex`：当前执行到路径的第几步；
- `_exploreTargetCell`：当前正在「从其上探索」的 cell 的 hash；
- `_exploreStepsLeft`：在该 cell 上还剩多少步探索（最大为 `kExploreStepsPerCell=10`）。

### 2.2 Archive 加权选 Cell

从 archive 中选择下一个目标 cell 时，GOExploreAgent 使用 **访问次数与被选次数的反比权重**，并可选叠加 BFS 距离因子：

\[
weight(c) = \frac{1}{1 + \text{seenTimes}_c} + \frac{1}{1 + \text{chosenTimes}_c}
\]

- `seenTimes` 少 → 表示该 cell 访问不充分，权重大；
- `chosenTimes` 少 → 表示很久没从该 cell 出发探索，权重大。

在实现中（`chooseCellFromArchive`）：

- 对于每个 cell 计算基础权重 `1/(1+seenTimes)+1/(1+chosenTimes)`；
- 若提供了从当前状态出发的 BFS 距离表 `distToCurrent`，对于距离为 `d>0` 的 cell，再乘以因子  
  \[
  (1 + kDistanceAlpha \times \text{clip}(d, kMaxDistanceForWeight))
  \]
  其中 `kDistanceAlpha=0.1`，`kMaxDistanceForWeight=32`，轻微偏置较远 cell，鼓励深度探索；
- 使用 **加权随机抽样**（总权重为 `sumWeight`，在 `[0,sumWeight)` 上采样一个随机数并按顺序扣减）选择目标 cell，避免总是贪心选同一个。

### 2.3 三阶段模式：回 → 探 → 选

在任意时刻，GOExploreAgent 处于以下三种模式之一：

1. **沿路径回 cell（Return）**  
   - `_pathToCell` 非空且 `_pathIndex` 未到末尾；
   - 目标是从当前状态沿 BFS 路径执行一系列动作，尽量到达目标 cell。

2. **从 cell 探索（Explore）**  
   - 当前状态 hash == `_exploreTargetCell`，且 `_exploreStepsLeft > 0`；
   - 在该 cell 上执行若干步「局部探索动作」，探索其附近未见过的状态。

3. **重新选择 cell（Select）**  
   - 不处于上述两种模式时，重新在 archive 中选择一个目标 cell，并决定是：
     - 直接在当前 cell 上探索（targetCell == currentHash），还是
     - 规划一条 BFS 路径回到目标 cell（targetCell != currentHash）。

---

## 三、selectNewAction 流程

### 3.1 高层伪代码

整体逻辑与实现 `selectNewAction()` 对齐，可概括如下：

```text
selectNewAction():
  1. 若无当前状态：fallbackPickAction()
  2. 防卡死：根据 blockTimes 返回 BACK / DEEP_LINK / CLEAN_RESTART，并清空路径与探索阶段
  3. 若正在执行路径回 cell：
       - 若当前 hash == path[pathIndex].first → 返回该步 action
       - 否则视为偏离 → 清空 _pathToCell 与 _pathIndex，进入「选新 cell」
  4. 若处于 cell 探索阶段：
       - 若当前 hash == _exploreTargetCell:
             * 调用 pickExploreAction() 返回探索动作
         否则：
             * 状态 hash 变化或已离开该 cell → 清空探索阶段，进入「选新 cell」
  5. 否则（需要新一轮「选 cell」）：
       - 调用 ensureInArchiveOnly(currentHash) 确保当前在 archive 中（不增加 seenTimes）
       - 调用 bfsDistances(currentHash) 得到 dist
       - 调用 chooseCellFromArchive(&dist) 选出 targetCell
       - 若 targetCell == currentHash:
             * 设置 _exploreTargetCell = currentHash
             * _exploreStepsLeft = kExploreStepsPerCell
             * 返回 pickExploreAction()（若失败则 fallbackPickAction()）
         否则:
             * 调用 buildPathTo(currentHash, targetCell, path)
             * 若成功：
                   - _pathToCell = path; _pathIndex = 0
                   - 返回路径第一步 action
               否则：
                   - 退化为在当前状态探索：
                       _exploreTargetCell = currentHash
                       _exploreStepsLeft = kExploreStepsPerCell
                       返回 pickExploreAction() 或 fallbackPickAction()
```

### 3.2 探索动作 pickExploreAction

在「从 cell 探索」阶段，GOExploreAgent 在当前状态上的合法动作中选择探索动作（`pickExploreAction`）：

- 收集所有合法、通过 filter 的非 NOP 动作作为 `valid`；
- 若 `valid` 为空，则尝试 `state->getBackAction()` 作为退路；
- 以概率 `kExploreRandomEpsilon=0.15` **完全随机**从 `valid` 中选一个动作，以保持多样性；
- 否则选择 **全局访问次数最少** 的动作（通过 `getVisitedCount()`），鼓励探索新边。

相比 CuriosityAgent 的好奇心得分，GOExploreAgent 的探索动作仅依赖简单的访问计数与少量随机性。

### 3.3 防卡死与 fallback

与 BFS/DFS/Frontier 一致，GOExploreAgent 在 `selectNewAction` 开头根据 `getCurrentStateBlockTimes()` 做防卡死处理：

- **blockTimes > kBlockCleanRestartThreshold=15** → 清空路径与探索阶段，返回 **CLEAN_RESTART**；
- **blockTimes > kBlockDeepLinkThreshold=10** → 清空路径与探索阶段，返回 **DEEP_LINK**；
- **blockTimes > kBlockBackThreshold=5** 时，若当前状态除 BACK 外无其他可用动作，则清空路径与探索阶段并返回 **BACK**。

在以下场景中使用 `fallbackPickAction`：

- 沿路径时检测到「当前状态与路径预期不符」；
- 无法 BFS 到 targetCell 或 chooseCellFromArchive 返回 0；
- 当前状态没有可用探索动作。

`fallbackPickAction` 内部直接调用 `pickExploreAction()`，确保退路策略与正常「从 cell 探索」一致。

---

## 四、moveForward 与图/Archive 更新

### 4.1 记录边与路径推进

在 `moveForward(StatePtr nextState)` 中，GOExploreAgent 完成以下工作：

1. **记录边**  
   - `fromState = _newState`，`actionTaken = _newAction`，在调用基类前保存；
   - 调用 `AbstractAgent::moveForward(nextState)` 更新当前状态；
   - 若 `fromState` / `nextState` / `actionTaken` 均非空，则：
     - 对于全局跳转/重启类操作（`CLEAN_RESTART`、`RESTART`、`START`、`ACTIVATE`、`DEEP_LINK`、`FUZZ`）**不记录边**，避免在 BFS 图中产生跨应用/跨会话的噪声；
     - 否则：
       - `fromHash = fromState->hash()`，`toHash = nextState->hash()`；
       - `_outEdges[fromHash].push_back({actionTaken, toHash})`。

2. **Archive 更新**  
   - 对 `nextState->hash()` 调用 `ensureArchiveAndSeen`：
     - 若该 hash 不在 archive 中 → 建档并 `seenTimes=1`；
     - 否则 `seenTimes++`。

3. **路径与探索阶段推进**  
   - 若当前处于「沿路径回 cell」模式（`_pathToCell` 非空且 `_pathIndex < _pathToCell.size()`）：
     - `_pathIndex++`；
     - 若 `_pathIndex` 到达路径末尾，则：
       - `arrivedHash = nextState ? nextState->hash() : 0`；
       - 清空 `_pathToCell` 与 `_pathIndex`；
       - 将 `_exploreTargetCell = arrivedHash`，`_exploreStepsLeft = kExploreStepsPerCell`，进入「从 cell 探索」阶段。
   - 否则，若 `_exploreStepsLeft > 0`，在每次 moveForward 后将其递减，用于控制探索阶段总步数。

### 4.2 Archive 计数语义

为保证计数语义清晰，GOExploreAgent 区分两类归档操作：

- `ensureArchiveAndSeen(hash)`：**仅在 moveForward 时调用**，表示「真实通过动作到达该状态一次」，负责 `seenTimes++`；
- `ensureInArchiveOnly(hash)`：**在选 cell 前调用**，仅保证当前状态存在于 archive 中，如当前 hash 首次出现则建档但不增加 `seenTimes`，避免「尚未通过真实动作到达时就增加 seen」。

被选中的目标 cell 在 `selectNewAction` 中增加 `chosenTimes`，用于下次加权选 cell。

---

## 五、动态状态抽象与 onStateAbstractionChanged

当状态抽象（state hash 设计）发生变化时，`onStateAbstractionChanged()` 会被调用。对于 GOExploreAgent：

- 清空 `_outEdges`（旧边的 hash 已失效）；
- 清空 `_pathToCell` / `_pathIndex` / `_exploreTargetCell` / `_exploreStepsLeft`；
- 清空 `_archive`，避免旧 hash 与新 hash 混用导致「回到不存在的 cell」；
- 通过日志输出原 edge/ archive 大小，便于调试。

这样可以确保新的抽象下，路径和 archive 仅使用新产生的 state hash。

---

## 六、GOExploreAgent 的优势

### 6.1 显式记忆前沿状态，缓解脱节

- 通过 archive 显式维护所有已访问 UI 状态，并记录其访问/被选频次；
- 通过 `1/(1+seenTimes)+1/(1+chosenTimes)` 的加权选 cell，使得「访问少、久未被选」的 cell 周期性地成为新的探索起点；
- 相比只依赖局部贪心或随机的策略，更不容易「遗忘」早期发现的深层页面。

### 6.2 先回再探，缓解脱轨

- 利用已有的状态图 `_outEdges` + BFS，从当前状态**规划一条路径回到目标 cell**；
- 到达 cell 后，在该处连续执行 `kExploreStepsPerCell=10` 步探索，而不是在当前局部盲目乱走；
- 虽然缺少“环境重置/轨迹精确重放”，但在「可达图」内部仍实现了 Go-Explore 的「先回再探」思想。

### 6.3 无奖励、无 Q 值、无学习，易部署

- GOExploreAgent 不依赖奖励函数，不维护 Q 表，也不需要训练神经网络；
- 仅使用简单的计数（seenTimes/ chosenTimes）与 BFS 路径规划，便于在资源有限的测试环境中运行；
- 适合作为 **冷启动**、无标注/无奖励场景下的 UI 探索策略。

---

## 七、与其它 Agent 的对比

### 7.1 vs FrontierAgent

| 维度         | FrontierAgent                                   | GOExploreAgent                                        |
|--------------|-------------------------------------------------|-------------------------------------------------------|
| 目标选取     | frontier 边（信息增益 × 距离）                  | archive cell（按 seenTimes / chosenTimes + 距离加权） |
| 路径         | 「当前 → frontier 源状态」的 BFS 路径           | 「当前 → 目标 cell」的 BFS 路径                       |
| 到达后行为   | 执行一次 frontier 动作                          | 在该 cell 连续探索 `kExploreStepsPerCell` 步再重选    |
| 显式记忆前沿 | 无独立 archive（仅 frontier 集合）             | 有 archive（全局 cell 存储与统计）                    |

### 7.2 vs CuriosityAgent

| 维度           | CuriosityAgent（好奇心驱动）                          | GOExploreAgent（Go-Explore 风格）                         |
|----------------|-------------------------------------------------------|-----------------------------------------------------------|
| 核心信号       | episode 新颖性 × 全局新颖性 × 状态因子              | archive 权重 + BFS 路径 + 本地少访问动作                 |
| 决策粒度       | 在 **当前状态** 所有动作上算 curiosity score        | 在 **全局 archive** 上选目标 cell，再从 cell 局部探索   |
| 是否维护 cell  | 不维护显式 cell/archive                              | 显式维护 cell archive                                     |
| 对前沿记忆方式 | 借由全局计数 + ε-greedy 间接实现                     | 通过 archive 直接「点名」要回去的 cell                   |

---

## 八、关键常量与使用建议

### 8.1 关键常量（与实现一致）

- **防卡死阈值**：`kBlockBackThreshold=5`，`kBlockDeepLinkThreshold=10`，`kBlockCleanRestartThreshold=15`；
- **探索步数**：`kExploreStepsPerCell=10` —— 到达 cell 后连续探索的步数；
- **BFS 最大深度**：`kMaxBFSDistance=256` —— 避免追求过远目标导致 BFS 开销过大；
- **距离权重参数**：`kDistanceAlpha=0.1`，`kMaxDistanceForWeight=32` —— 在选 cell 时轻微偏置较远 cell；
- **探索随机 ε**：`kExploreRandomEpsilon=0.15` —— 探索阶段以该概率完全随机选动作，其余时间优先少访问动作。

### 8.2 使用与调参建议

- **使用方式**：命令行 `--agent goexplore`（或配置对应 algorithm）；JNI 侧传入 `AlgorithmType::GoExplore` 即可创建 GOExploreAgent。
- **调参策略**：
  - 若观察到「长时间停在少数几个页面」：适当减小 `seenTimes` / `chosenTimes` 权重衰减（或增大 `kDistanceAlpha`、`kMaxDistanceForWeight`），增大 `kExploreRandomEpsilon`；
  - 若 BFS 路径过长/频繁失败：减小 `kMaxBFSDistance` 或在选 cell 时降低对远距离 cell 的放大；
  - 可对照覆盖率、重复访问率与卡死次数，逐步调节 `kExploreStepsPerCell`（过小 → 深度不足，过大 → 局部打转）。

---

## 九、参考文献与相关文档

1. 本项目：`GOExploreAgent.cpp` / `GOExploreAgent.h`。   
2. Ecoffet et al., *Go-Explore: a New Approach for Hard-Exploration Problems*, arXiv:1901.10995, 2019.  
3. Ecoffet et al., *First return, then explore*, Nature 590, 2021, arXiv:2004.12919.  
4. Gallouédec et al., *Cell-Free Latent Go-Explore*, ICML 2023, arXiv:2208.14928。  

