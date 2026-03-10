/**
 * @authors Zhao Zhang
 */

# FrontierAgent 算法详解与优势分析

## 执行摘要

本文档详细解释 **FrontierAgent** 在 Android UI 测试中采用的**前沿驱动探索（Frontier-based Exploration）**算法的原理与实现。FrontierAgent 与 BFSAgent、DFSAgent 一样不依赖 Q 值或强化学习模型，完全基于状态图结构、访问计数与**自记录的出边**；通过 **「结构化地图 → 全局规划 → 局部执行」** 的 MA-SLAM 式框架，将「前沿」定义为未访问/少访问的 (状态, 动作)，用 **FH-DRL 风格的指数-双曲打分**（信息增益 × 距离衰减）选出目标，再通过 **一步直达** 或 **BFS 路径导航 + 执行** 完成动作选择，在保持无模型、可解释的前提下，实现「先近后远、信息增益与距离平衡」的探索语义。

**当前实现**：FrontierAgent 在共享状态图与自维护的 `_outEdges` 之上，实现 **全局 frontier 候选构建 + BFS 距离 + FH-DRL 打分 + 两阶段执行（当前状态直接执行 / 沿 BFS 路径走到目标再执行）**，并与防卡死（block → BACK / DEEP_LINK / CLEAN_RESTART）、路径偏离清空、无路径时退回当前状态最佳候选、动态状态抽象下的边表清空等结合，形成一套完整的无模型、前沿驱动探索算法。

---

## 一、算法背景与核心问题

### 1.1 为何需要「前沿」探索

在 UI 自动化测试中，除深度优先（DFS）、广度优先（BFS）外，另一种自然策略是**前沿驱动（Frontier-based）**：

1. **深度优先（DFS）**：沿一条路径尽量往深处走，再回溯；容易「先打穿一条路」。
2. **广度优先（BFS）**：显式队列 + 层序，先一层再下一层；覆盖均匀、先近后远。
3. **前沿驱动（Frontier）**：将「前沿」定义为**未访问或少访问的 (状态, 动作)**，每步在**全局**候选中按「信息增益 vs 距离」打分，选出**一个**目标 frontier，再**直接执行**（若目标在当前状态）或**先沿最短路径走到目标状态、再执行该动作**。兼顾「去哪更有信息」与「走过去要几步」。

**FrontierAgent 要解决的核心问题**：

- 在**未知或难以定义奖励**的 UI 图上，如何**系统性地走向「信息增益高」的 frontier**，同时考虑**距离**（避免总是选很远的目标）？
- 当目标 frontier 不在当前状态时，如何**先导航再执行**（BFS 路径），并处理路径偏离、无路径等情况？
- 如何避免在同一页面打转（卡住），并与 BFS/DFS 一样做 escalation（BACK → DEEP_LINK → CLEAN_RESTART）？
- 如何与动态状态抽象（refine/coarsen）兼容，不在旧/新 hash 混用时产生错误路径？

### 1.2 FrontierAgent 的定位

FrontierAgent 是一种**无模型、基于图与访问计数的探索策略**：

- **结构化地图**：使用 Model/Graph 的 `getStates()` 与自维护的 **`_outEdges`**（每次 `moveForward` 记录「从哪状态、用哪动作、到哪状态」），形成「状态 + 已观察边」的图。
- **全局规划**：从**当前状态**做 BFS 得到各状态距离，对图中所有 (状态, 动作) 建 frontier 候选，用 **FH-DRL 打分** `score = infoGain * exp(-β * distance)` 选出一个目标。
- **局部执行**：若目标在当前状态则**直接返回该 action**；否则用 BFS 建**当前状态 → 目标状态**的最短路径，维护 `_pathToFrontier`，每步返回路径上的下一步动作，到达目标后再执行 frontier action。

---

## 二、算法原理

### 2.1 基本概念

**状态图**：节点为 UI 状态（state hash），边由 FrontierAgent 在 `moveForward` 中记录为 `(fromHash, action, toHash)`，存于 `_outEdges`。图结构由 Model/Graph 提供 `getStates()`，边由 Agent 自行积累。

**Frontier（前沿）**：在 Monkey 语境下，**frontier = 从已访问状态出发的、未访问或少访问的动作**。每个候选是一个 **(sourceState, action)**，附带 **infoGain**（信息增益估计）与 **distance**（从当前状态到 sourceState 的 BFS 步数）。

**两阶段执行**：

- **A. 目标在当前状态**：直接返回该 action。
- **B. 目标在其它状态**：先沿 BFS 路径走到 sourceState，再执行 action；路径用 `_pathToFrontier` 与 `_pathIndex` 维护，每步返回路径当前段的 action，到达后清空路径并执行目标 action。

### 2.2 信息增益（infoGain）

对每个候选动作估计「执行该动作能带来的信息增益」：

- **未访问**（`visitCount <= 0`）：`infoGain = kBaseInfoGain + typePriority * 0.25`，其中 `typePriority` 来自 `getPriorityByActionType()`（如 DEEP_LINK/ACTIVATE 等更高），鼓励优先尝试高类型权重的未访问动作。
- **已访问**：`infoGain = kBaseInfoGain / (1 + visitCount)`，随访问次数衰减。

常量：`kBaseInfoGain = 1.0`。

### 2.3 距离与 BFS

- **距离**：从**当前状态**到候选所在 **sourceState** 的**最短步数**，由 BFS 在 `_outEdges` 上从当前状态 hash 出发得到，存为 `stateHash -> distance`。
- 不可达状态视为距离 `kMaxBFSDistance`（256），在打分时被指数项压低，不会误选为优先目标。
- **BFS 距离缓存**：当当前状态未变（例如沿路径执行时本步不重算候选），使用 `_cachedDistMap` 与 `_cachedDistMapStartHash` 避免重复 BFS；在 `moveForward` 记录新边或 `onStateAbstractionChanged` 时失效。

### 2.4 FH-DRL 打分与候选选择

采用 FH-DRL（arXiv:2407.18892）风格的**指数-双曲**平衡「距离」与「信息增益」：

```
score = infoGain * exp(-β * distance)
```

其中 `β = kBeta = 0.6`。对所有候选算分后，取 **score 最大** 的 (state, action) 作为本轮的**目标 frontier**。

**同分处理**：若有多个候选同分，先按 **action 类型优先级**（`getPriorityByActionType()`）取最优类型，若仍有多者则**随机**选一，保证确定性可复现时需固定随机种子。

### 2.5 路径执行与偏离

- 当目标 **targetHash ≠ currentHash** 时，调用 `buildPathTo(currentHash, targetHash, path)` 在 `_outEdges` 上 BFS 建路径，得到 `[(sourceHash, action), ...]`，再 `push_back({targetHash, best.action})` 得到「到达目标后执行 frontier action」的完整序列。
- `_pathToFrontier` 存该序列，`_pathIndex` 表示当前应执行到第几步。每步若 `currentHash == _pathToFrontier[_pathIndex].first`，返回 `_pathToFrontier[_pathIndex].second`；否则视为**路径偏离**（如状态抽象变化、非确定性等），清空路径与 `_pathIndex`，下一帧重新做全局规划。
- `moveForward` 中若路径未空且 `_pathIndex` 未越界，则 `_pathIndex++`；若已到路径末尾则清空路径与 `_pathIndex`。

### 2.6 无路径到目标

若 `buildPathTo` 返回 false（当前状态到不了目标状态），**不能**返回 `best.action`（该 action 属于目标 state，不在当前界面）。正确行为：在**当前状态**的候选中选得分最高者返回；若当前状态无候选则 `fallbackPickAction()`。

### 2.7 防卡死（Anti-stuck）

与 BFS/DFS 对齐，在 `selectNewAction` 开头根据 `getCurrentStateBlockTimes()`：

- `blockTimes > 15` → **CLEAN_RESTART**，清空路径；
- `blockTimes > 10` → **DEEP_LINK**，清空路径；
- `blockTimes > 5` 且当前仅有 BACK 可用（非 BACK 均不可用或为 NOP）→ 返回 **BACK**。

并在 `fallbackPickAction` 中：若 `randomPickAction` 返回 null，再尝试 `state->getBackAction()`（且通过 filter 时）再返回，仅当 BACK 也不可用时才返回 null，减少 NOP 死循环。

---

## 三、实现细节

### 3.1 核心数据结构

```cpp
// 单个 frontier 候选
struct FrontierCandidate {
    StatePtr sourceState;
    ActivityStateActionPtr action;
    double infoGain = 0.0;
    int distance = 0;   // 0 表示当前状态（局部 frontier）
};

// 自记录的出边：sourceHash -> [(action, targetHash)]
std::unordered_map<uintptr_t, std::vector<std::pair<ActivityStateActionPtr, uintptr_t>>> _outEdges;

// 当前目标路径：[(sourceHash, action), ...]，pathIndex 表示当前步
std::vector<std::pair<uintptr_t, ActivityStateActionPtr>> _pathToFrontier;
size_t _pathIndex = 0;

// BFS 距离缓存（当前状态未变时复用）
mutable std::unordered_map<uintptr_t, int> _cachedDistMap;
mutable uintptr_t _cachedDistMapStartHash = 0;

// 防卡死阈值（与 BFS 一致）
static constexpr int kBlockBackThreshold = 5;
static constexpr int kBlockDeepLinkThreshold = 10;
static constexpr int kBlockCleanRestartThreshold = 15;

// FH-DRL 与信息增益
static constexpr double kBeta = 0.6;
static constexpr double kBaseInfoGain = 1.0;
static constexpr int kMaxBFSDistance = 256;
```

### 3.2 moveForward：边记录与路径推进

执行完动作并进入新状态后，`moveForward(nextState)` 被调用。FrontierAgent::moveForward 在调用基类**之前**保存 `fromState = _newState`、`actionTaken = _newAction`，基类调用之后：

1. **记录边**：若 fromState、nextState、actionTaken 均非空，则 `_outEdges[fromHash].push_back({actionTaken, toHash})`，并置 `_cachedDistMapStartHash = 0` 使 BFS 缓存失效。
2. **路径推进**：若 `_pathToFrontier` 非空且 `_pathIndex` 未越界，则 `_pathIndex++`；若已到达路径末尾则清空 `_pathToFrontier` 与 `_pathIndex`。

### 3.3 selectNewAction：策略与回退

**顺序**：

1. **无状态**：无 `_newState` 则 `fallbackPickAction()`。
2. **防卡死**：按 `getCurrentStateBlockTimes()` 做 CLEAN_RESTART / DEEP_LINK / BACK（仅当仅 BACK 可用时），并清空路径。
3. **沿路径执行**：若 `_pathToFrontier` 非空且 `_pathIndex` 在范围内且 `currentHash == _pathToFrontier[_pathIndex].first`，返回 `_pathToFrontier[_pathIndex].second`；否则清空路径（路径偏离）。
4. **全局规划**：`buildFrontierCandidates()` 得到候选；若无候选则 `fallbackPickAction()`。
5. **打分与选目标**：对每个候选算 `getScore(infoGain, distance)`，取最高分；同分按类型优先级再随机，得到 `best` 与 `targetHash`。
6. **执行方式**：
   - 若 `targetHash == currentHash`：直接返回 `best.action`。
   - 否则调用 `buildPathTo(currentHash, targetHash, path)`；若成功则 `path.push_back({targetHash, best.action})`，赋给 `_pathToFrontier`，`_pathIndex = 0`，返回路径第一步的 action。
   - 若 `buildPathTo` 失败：在**当前状态**的候选中选得分最高者返回；若无则 `fallbackPickAction()`。
7. **fallbackPickAction**：优先少访问、非 BACK；否则 `randomPickAction`；再否则尝试 `getBackAction()`（通过 filter 时）；避免返回 null 导致 NOP 死循环。

### 3.4 buildFrontierCandidates：全局 vs 局部

- **条件**：若 model/graph 不可用、无当前状态、`states.size() <= 1` 或 `_outEdges.empty()`，则退化为 **buildFrontierCandidatesLocal()**：仅当前状态的合法动作，distance 恒为 0。
- **全局**：用 `graph->getStates()` 与 `bfsDistances(currentHash)`（或缓存）得到 distMap；对每个 state 算一次 `stateHash = state->hash()`，对每个合法 action 建 `FrontierCandidate`（infoGain、distance）；`candidates.reserve(estCandidates)` 预分配。若结果为空仍退回 local。

### 3.5 动态状态抽象（onStateAbstractionChanged）

当 Model 在 refine/coarsen 后调用 `onStateAbstractionChanged()` 时，FrontierAgent 清空：

- `_outEdges`
- `_pathToFrontier`、`_pathIndex`
- `_cachedDistMap`、`_cachedDistMapStartHash`

使 BFS 与路径仅使用**抽象变化之后**新记录的边，避免旧 hash 与新 hash 混用导致的边表碎片与错误路径。

---

## 四、FrontierAgent 的优势

### 4.1 无奖励、无 Q 值、无学习

与 BFS/DFS 相同：不依赖奖励、Q 值、学习率；行为由当前图、访问计数、BFS 距离与 FH-DRL 打分决定，部署简单、可解释、适合冷启动。

### 4.2 「信息增益 × 距离」的显式权衡

- **信息增益**：未访问/少访问动作获得更高 infoGain，类型权重（如 DEEP_LINK）进一步区分。
- **距离**：`exp(-β * distance)` 使近处 frontier 得分更高，避免总是选「很远但信息增益略高」的目标，实现**先近后远、兼顾信息**的探索。

### 4.3 全局规划 + 局部执行（MA-SLAM 风格）

- **全局**：在所有已访问状态及其动作上建候选，用 BFS 距离 + FH-DRL 选出一个目标，语义清晰。
- **局部**：要么一步执行，要么沿 BFS 路径逐步走到目标再执行，路径偏离则安全清空并重规划。

### 4.4 与 BFS/DFS、Double SARSA 的互补

| 维度           | Double SARSA        | BFS                    | FrontierAgent                          |
|----------------|---------------------|------------------------|----------------------------------------|
| 探索顺序       | Q 与 ε-greedy       | 层序（队列 + 深度）    | 信息增益 × 距离（FH-DRL）              |
| 依赖           | 奖励、Q、超参       | 图 + 访问计数、饱和    | 图 + 自记录边 + 访问计数               |
| 覆盖特性       | 依赖奖励与学习      | 先一层再下一层         | 先近后远、高信息 frontier 优先          |
| 结构           | 无                  | 显式队列 + 深度        | 候选 + BFS 距离 + 路径                 |
| 卡住处理       | 依赖探索/奖励       | BACK/DEEP_LINK/CLEAN   | 同 BFS/DFS（block 阈值 + BACK 仅当仅 BACK 可用） |

---

## 五、算法对比

### 5.1 Frontier vs BFS（策略层面）

| 特性           | BFS                                    | FrontierAgent                                  |
|----------------|----------------------------------------|------------------------------------------------|
| **结构**       | 显式队列 + 层深 + 同一状态只入队一次    | 候选列表 + BFS 距离 + 路径（仅当目标非当前）   |
| **选目标**     | 队首帧 + 动作优先级（未访问/reusable）  | 全局候选 + FH-DRL score = infoGain×exp(-β×d)   |
| **执行**       | 每帧从队首选一动作                     | 直接执行 或 沿 BFS 路径走到目标再执行          |
| **距离**       | 显式 depth，层序                       | BFS 步数，用于打分与路径                       |
| **边来源**     | Graph/Model 状态与动作                 | Agent 自记录 _outEdges（moveForward）          |

### 5.2 Frontier vs DFS

| 特性           | DFS                    | FrontierAgent                          |
|----------------|------------------------|----------------------------------------|
| **结构**       | 显式栈、trim 回溯      | 无栈；候选 + 路径                       |
| **选目标**     | 栈顶 + 未访问/reusable | 全局候选 + 信息增益与距离               |
| **执行**       | 纵深再回溯             | 选一个目标 → 直接或沿路径执行          |

### 5.3 Frontier vs Double SARSA

| 特性           | Double SARSA              | FrontierAgent                    |
|----------------|----------------------------|----------------------------------|
| **模型**       | 双 Q 表 + N-step           | 无 Q；候选 + BFS + FH-DRL 打分   |
| **奖励**       | 必须定义                   | 不需要                          |
| **探索逻辑**   | ε-greedy + Q 值           | infoGain × exp(-β×distance)     |

---

## 六、实现细节说明

### 6.1 路径格式与路径索引

`_pathToFrontier[i] = (sourceHash, action)` 表示「处于 sourceHash 时执行 action」。执行 `path[k].second` 后，`moveForward` 将 `_pathIndex` 加一，下一步用 `path[k+1].first` 与当前 state hash 比对；路径末尾再补一步 `(targetHash, best.action)`，即「到达目标后执行 frontier action」。顺序与语义一致。

### 6.2 无路径到目标时不可返回 best.action

若 `buildPathTo` 返回 false，`best.action` 属于**目标 state**，不在当前界面，执行会错误。因此必须只在**当前状态**的候选中选最高分返回，或无则 fallback，保证返回的 action 一定属于当前 state。

### 6.3 路径偏离与状态抽象

当 `currentHash != _pathToFrontier[_pathIndex].first` 时（例如执行后到达的是 refine 产生的新 hash），视为路径偏离，清空路径并下一帧重新规划，不跟错路径。结合 `onStateAbstractionChanged` 清空 `_outEdges` 与路径，BFS 仅用新边，避免旧/新 hash 混用。

### 6.4 BFS 距离缓存失效时机

- `moveForward` 中记录新边后置 `_cachedDistMapStartHash = 0`，下一帧 `buildFrontierCandidates` 会用新的 currentHash 重算 BFS。
- `onStateAbstractionChanged` 清空 `_cachedDistMap` 与 `_cachedDistMapStartHash`。

---

## 七、性能考虑

### 7.1 已做优化

- **BFS 距离缓存**：当前状态未变时复用 `_cachedDistMap`，沿路径执行时避免每步整图 BFS。
- **state hash 外提**：`buildFrontierCandidates` 中外层按 state 算一次 `stateHash`，内层用其查距离，避免同一 state 重复 `hash()`。
- **candidates / tie-break reserve**：`candidates.reserve(estCandidates)`，`bestIndices.reserve(32)`，`typeBest.reserve(bestIndices.size())`，减少 realloc。

### 7.2 可选优化（见 survey 3.3.6）

- **候选规模**：图极大时可选「单遍只保留最优」或「仅距离≤D 的 state 建候选」，当前百～低千 state 下通常不必做。
- **_outEdges 膨胀**：长跑时可按 (fromHash, toHash) 去重或每边只存一条，减少内存与 BFS 遍历。
- **buildPathTo 分配**：调用频率低，单次 parent/q/rev 分配可接受，成员复用收益小。

---

## 八、使用建议

### 8.1 关键阈值（代码常量）

- **防卡死**：BACK > 5，DEEP_LINK > 10，CLEAN_RESTART > 15（与 BFS 一致）。
- **FH-DRL**：`kBeta = 0.6`，`kBaseInfoGain = 1.0`，`kMaxBFSDistance = 256`。

### 8.2 配置与注册

- `AlgorithmType::Frontier = 16`（Base.h）；AgentFactory 已注册；JNI 传入 `agentType=16` 即可选用 FrontierAgent。

---

## 九、参考文献与相关文档

1. 本项目：`FrontierAgent.cpp` / `FrontierAgent.h`。
2. 本项目：`BFS_ALGORITHM_EXPLANATION.md` — BFS 算法详解与对比参考。
3. 本项目：`monkey_exploration_survey.md` — Frontier 定义、关键节点、正确性 Review、性能与动态抽象（3.3.x）。
4. MA-SLAM 框架思路：arXiv:2511.14330（结构化地图 + 全局规划 + 局部执行）。
5. FH-DRL 打分形式：arXiv:2407.18892（信息增益与距离的指数-双曲平衡）。
