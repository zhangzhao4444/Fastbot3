/**
 * @authors Zhao Zhang
 */

# DFS 算法详解与优势分析

## 执行摘要

本文档详细解释 **DFSAgent** 在 Android UI 测试中采用的**深度优先探索（Depth-First Search, DFS）**算法的原理、实现细节。DFS 不依赖 Q 值或强化学习模型，完全基于状态图的结构与访问计数，通过显式栈实现“先纵深、后回溯”的探索策略，并配有卡住检测与 escalation（BACK → DEEP_LINK → CLEAN_RESTART）以应对死循环与探索陷阱。

**当前实现**：`DFSAgent` 在共享状态图之上实现 **显式栈 + 动作优先级 + 目标状态饱和判断** 的 DFS，并与 root 保护、activity/state block 计数、tarpit 检测结合，形成一套完整的无模型探索算法。

---

## 一、算法背景与核心问题

### 1.1 为何需要“无模型”探索

在 UI 自动化测试中，常见策略：

1. **基于强化学习（如 Double SARSA）**：维护 Q 值，通过奖励信号学习“哪些动作更易带来新页面/新 activity”。优势是长期可收敛到较优策略；代价是需要奖励设计、超参（alpha、gamma、epsilon）和一定步数才能见效。

2. **基于结构的探索（如 DFS）**：不维护 Q 值，仅根据“是否访问过”“是否饱和”等结构信息选动作。优势是**无需奖励、无超参、行为可解释、从第一步起就有明确策略**；适合需要快速覆盖、或奖励难以定义的场景。

**DFS 要解决的核心问题**：

- 在**未知或难以定义奖励**的 UI 图上，如何**系统性地覆盖更多状态与 activity**？
- 如何避免在**同一页面/同一 activity 打转**（卡住）？
- 如何在**根页面**避免 BACK 导致退出应用或 BACK 死循环？

### 1.2 DFS 的定位

DFS 是一种**确定性（在给定栈与访问计数下）+ 随机性（动作顺序 shuffle）**的探索策略：

- **深度优先**：优先选“未访问的、有目标的”动作，使探索沿一条 UI 路径尽量往深处走。
- **回溯**：当前状态所有有效动作耗尽后，通过 BACK 退回上一状态，再尝试该状态的其它分支。
- **无模型**：不估计“动作价值”，只区分“未访问 / 已访问但未饱和 / 已饱和”，不依赖奖励或 Q 值更新。

---

## 二、DFS 算法原理

### 2.1 基本概念

**状态图**：节点为 UI 状态（由 state hash 区分），边为“状态 + 动作”导致的转移。图由 `Model`/`Graph` 维护，与 agent 无关；DFSAgent 只使用“当前状态、动作列表、访问计数、是否饱和”等接口。

**显式栈（Stack of Frames）**：每个栈帧为 `(state, nextIndex, order)`。

- `state`：该帧对应的状态。
- `order`：该状态动作下标的一个**随机排列**（每帧独立 shuffle），用于决定尝试顺序。
- `nextIndex`：下一轮扫描动作时从 `order[nextIndex]` 开始，实现“断点续扫”。

**回溯（Trim）**：若从某状态 S 经过一系列动作后再次到达**已在栈中的状态** S'（即 S' 的 hash 与栈中某帧相同），则**将栈 trim 到 S' 所在帧**，并置该帧的 `nextIndex = 0`，从而从 S' 重新扫描所有动作（包括之前选过的，可能通过 bestReusable 再选）。这对应经典 DFS 中的“回到已访问节点并继续尝试其它分支”。

### 2.2 动作选择优先级（单帧内）

对**当前栈顶帧**对应的状态，按以下优先级选一个动作（只选一个即返回）：

```
1. 未访问的、有目标（requireTarget）的动作
   → 若有多个，按 getActionTypeWeight 取更高权重的（如 CLICK > SCROLL > BACK）
2. 未访问的、其它有效动作
   → 同样按 typeWeight 择优
3. 已访问但“未饱和”的动作（reusable）
   → 饱和定义：该动作在 state 上 isSaturated，或目标状态已全访问（isActionSaturatedByTargetState）
   → 在未饱和中按 getEdgeNoveltyScore * typeWeight 择优
4. 若以上都没有 → 当前帧“耗尽”，尝试 BACK 并 pop 本帧；若为 root 则不返回 BACK，继续 pop 或最终 random
```

**设计意图**：

- 优先**未访问 target**：尽快沿一条路径“钻到底”，实现深度优先。
- 其次 **reusable**：允许同一边多次走（例如同一按钮多次点击到达不同后续状态），但优先“曾经带来新 state/新 activity 比例高”的边（novelty）。
- **饱和**：避免在“目标状态已完全探索”的边上浪费步数。

### 2.3 回溯与 BACK

- 当当前帧所有有效动作都**不满足** 1/2/3 时，该帧视为耗尽。
- 若存在 BACK 且**不是 root state/root activity**：返回 BACK，并 **pop 当前帧**（下次选动作时栈顶变为上一帧）。
- 若是 root：**不返回 BACK**（避免退出应用或 BACK 死循环），只 pop，继续循环；若栈空则进入“栈空回退”（random）。

### 2.4 栈空回退

当栈被 pop 空后，无法再按栈回溯，此时：

- 以**当前状态**（即 `_newState`，当前页）做多次 `randomPickAction`，尽量选**非 BACK** 的动作。
- 若仍无动作，再试 `randomPickUnvisitedAction`。
- 保证在“无结构可依”时仍能给出动作，避免返回 null（由调用方处理）。

---

## 三、实现细节

### 3.1 核心数据结构

```cpp
// 显式 DFS 栈：每帧 = 状态 + 下一尝试下标 + 动作下标排列
struct Frame {
    StatePtr state;
    size_t nextIndex;              // 当前扫到 order 的哪一维
    std::vector<size_t> order;     // 动作下标 shuffle 后的顺序
};
std::vector<Frame> _stack;

// 已访问状态/activity（用于 block 与 edge 统计）
std::unordered_set<uintptr_t> _visitedStates;
std::unordered_set<std::string> _visitedActivities;

// 边统计：(stateHash, actionHash) -> 使用次数、带来新状态次数、带来新 activity 次数
std::unordered_map<std::uint64_t, EdgeStats> _edgeStats;

// 目标状态饱和：(stateHash, actionHash) -> 目标 state hash；state hash -> (目标动作总数, 已访问数)
std::unordered_map<std::uint64_t, uintptr_t> _edgeToTarget;
std::unordered_map<uintptr_t, std::pair<int, int>> _stateCoverage;

// Root 与卡住检测
std::string _rootActivity;
int _stateBlockCounter, _activityBlockCounter;
uintptr_t _lastStateHash;
std::string _lastActivity;

// VET 风格 tarpit：最近 N 步的 activity 滑动窗口，用于判断“是否陷在少数 activity 内”
std::deque<std::string> _recentActivities;
```

### 3.2 moveForward：栈与统计的更新

每次执行完动作并进入新状态后，`Model::selectAction` 会调用 `agent->moveForward(state)`（此时 `state` 为当前页状态）。`DFSAgent::moveForward` 在基类更新 `_currentState/_newState` 之后：

1. **Block 计数**：若当前 state hash 与上一步相同则 `_stateBlockCounter++`，否则置 0；对 activity 仅在“当前状态无有效非 BACK 动作”时累加 `_activityBlockCounter`，否则置 0。
2. **Root**：首次遇到某 activity 时设为 `_rootActivity`。
3. **Edge 统计**：对 (prevState, currentAction) 更新 `_edgeStats`（total、newStates、newActivities）。
4. **目标饱和**：若当前动作 `requireTarget()` 且已知下一状态，记录 `_edgeToTarget` 和 `_stateCoverage`。
5. **栈**：
   - 若当前状态 hash **已在栈中某帧**：找到该帧， Trim 到该帧（`erase(it+1, end)`），并置 `it->nextIndex = 0`。
   - 否则：**Push** 新帧 (state, order=shuffle(0..n-1), nextIndex=0)。

这样，同一状态再次出现时会被当作“回溯到该状态”，从该状态重新扫动作；新状态则压栈继续纵深。

### 3.3 selectNewAction：策略与 escalation

**顺序**（在真正进入 DFS 循环前）：

1. **状态/activity 变化**：若 `_newState->hash() != _lastStateHash` 或 activity 变化，重置 block 计数器，避免“已经离开卡住点仍继续 escalation”。
2. **当前状态与栈同步**：若当前决策状态为 `_newState` 且与栈顶状态不同（例如 DEEP_LINK 后新页），则**先为 `_newState` 压入一帧**，保证后续 DFS 循环针对的是当前页。
3. **Block escalation**（`blockTimes = max(_stateBlockCounter, _activityBlockCounter)`）：
   - `blockTimes > 15` → **CLEAN_RESTART**，并清零 block。
   - `blockTimes > 10` 或 **inTarpit() 且 > 5** → **DEEP_LINK**（并递增 block，以便未缓解时最终触发 CLEAN_RESTART）。
   - `blockTimes > 5` 且当前状态**仅有 BACK 可用**且**非 root** → 返回 **BACK** 并清零 block。
   - `blockTimes >= 12` → **self-rescue**：优先选未访问非 BACK 动作，其次任意非 BACK 动作，若仍无则继续正常 DFS。
4. **DFS 循环**：以栈顶帧为准，按 §2.2 的优先级选动作；若当前帧耗尽则 BACK（若非 root）+ pop，直至栈空或返回某动作。
5. **栈空**：按 §2.4 random 回退（优先非 BACK）。

**Tarpit**：最近 50 步内若不同 activity 数 ≤ 3 且步数 ≥ 30，认为陷入“探索陷阱”，可提前触发 DEEP_LINK（在 block > 5 时）。

### 3.4 动作类型权重与 novelty

- **getActionTypeWeight**：CLICK/LONG_CLICK=3，SCROLL 系列=2，BACK/NOP/SHELL=0.5，其余=1。用于在“多个未访问或 reusable”中优先更可能改变页面的动作。
- **getEdgeNoveltyScore**：基于 `_edgeStats` 的 (newStates/total + 2*newActivities/total)，用于在 reusable 中优先“历史上更容易带来新状态/新 activity”的边。

---

## 四、DFS 的优势

### 4.1 无奖励、无 Q 值、无超参

**对比 Double SARSA**：

- Double SARSA 需要：奖励设计、alpha、gamma、epsilon、N-step 窗口等。
- DFS 仅依赖：图上的访问计数、是否饱和、动作类型。**零奖励、零 Q 值、零学习率**，行为完全由“当前栈 + 当前状态的动作列表 + 历史访问”决定。

**优势**：

- 部署简单：无需调参，也无需等待“学习收敛”。
- 可解释：选 BACK 是因为当前帧耗尽；选某 CLICK 是因为未访问或 reusable 中 novelty 最高。
- 适合“冷启动”：从第一步起就有明确的深度优先策略，不依赖前期随机探索来填 Q 表。

### 4.2 深度优先带来的覆盖特性

**深度优先**意味着：

- 会沿一条路径尽量走到“叶子”（无未访问 target 或全部饱和），再 BACK 回上一层尝试其它分支。
- 在树状/近似树状的 UI 图上，容易**先覆盖一条完整路径上的 activity**，再逐层回溯，有利于发现深层 crash 或深层界面问题。

**与随机或 BFS 的对比**：

- 纯随机：覆盖慢，且容易重复浅层状态。
- BFS：先广后深，需要维护队列和更多状态；在 UI 测试中往往步数有限，DFS 的“先打穿一条路”往往更快见到深层页面。

### 4.3 卡住自救与 root 保护

**Escalation 阶梯**：

- 同一 state 或同一 activity（且仅剩 BACK）连续多步未变 → 先尝试 BACK(>5)，再 DEEP_LINK(>10 或 tarpit)，再 CLEAN_RESTART(>15)。
- Root 下不返回 BACK，让 block 继续增长，最终用 DEEP_LINK/CLEAN_RESTART 跳出，避免 BACK 死循环或退出应用。

**优势**：

- 无需人工定义“卡住”的奖励惩罚，结构上即可检测并响应。
- 与状态抽象（按 activity 的 state 合并）配合良好：state hash 可能因抽象变化，activity block 仍能捕捉“同一 activity 内打转”。

### 4.4 目标状态饱和与 reusable

**isActionSaturatedByTargetState**：若 (s, a) 的目标状态已“全访问”（该目标状态下所有 requireTarget 的动作都被访问过），则不再优先选 (s, a)，除非没有更优选项。这样减少在“已经探完”的边上重复走。

**Reusable**：已访问但未饱和的边仍可被选，通过 **novelty * typeWeight** 排序，优先历史上带来新 state/新 activity 比例高的边。在动态 UI 或非确定性转移下，同一边多次走可能到达不同状态，DFS 仍能利用这些边继续探索。

### 4.5 与 Double SARSA 的互补

| 维度         | Double SARSA                    | DFS                          |
|--------------|----------------------------------|------------------------------|
| 依赖         | 奖励、Q1/Q2、alpha/gamma/epsilon | 仅图结构、访问计数、饱和      |
| 冷启动       | 需探索以学习 Q                  | 第一步就有明确策略            |
| 长期策略     | 可收敛到高回报策略              | 固定为深度优先+回溯           |
| 可解释性     | 依赖 Q 值解释                   | 栈+优先级+block 即可解释      |
| 卡住处理     | 依赖奖励/探索率                 | 内置 escalation+root 保护     |
| 适用场景     | 奖励可定义、追求长期最优        | 奖励难定义、追求快速覆盖      |

两者可并存：同一框架下可选 DFSAgent 或 DoubleSarsaAgent，按需求切换。

---

## 五、算法对比

### 5.1 DFS vs Double SARSA（策略层面）

| 特性           | Double SARSA              | DFS                           |
|----------------|---------------------------|-------------------------------|
| **模型**       | 双 Q 表 + N-step 更新     | 无 Q 表，显式栈 + 访问计数    |
| **超参**       | alpha, gamma, epsilon, N  | 无                            |
| **奖励**       | 必须定义                 | 不需要                        |
| **探索逻辑**   | ε-greedy + Q 值          | 未访问 > reusable > BACK      |
| **卡住处理**   | 依赖奖励/探索            | BACK/DEEP_LINK/CLEAN_RESTART  |
| **内存**       | Q1/Q2 按动作             | 栈 + edge/coverage 统计       |
| **可解释性**   | 中等（看 Q）             | 高（栈+优先级+block）         |



---

## 六、实现细节说明

### 6.1 Trim 与 nextIndex = 0

回访到栈中已有状态时，将该帧的 `nextIndex` 置 0，以便重新扫描所有动作。这样：

- 之前选过的“未访问”动作此时已变为已访问，会进入 reusable 或饱和判断；
- 若该动作仍为 bestReusable（novelty 高、未饱和），可能再次被选，对应“同一边多次走以探索非确定性”。

### 6.2 Root 的双重判断

- **isRootState**：栈中仅剩一帧（即当前帧为唯一帧）。
- **isRootActivity**：当前状态的 activity 等于 `_rootActivity`（首个访问的 activity）。

两者任一满足时，不返回 BACK，避免退出应用或 BACK 无效导致的死循环。Pop 后若栈空则走 random 回退。

### 6.3 当前状态与栈不同步时的修复

若本步状态来自 DEEP_LINK/CLEAN_RESTART 等，`_newState` 可能与栈顶不一致。在进入 DFS 的 while 前，若 `state == _newState` 且与栈顶 hash 不同，则**先为 `_newState` 压入一帧**，保证 while 循环针对的是当前页选动作，避免“对错误状态返回动作”。详见 `DFSAgent_algorithm_review.md`。

### 6.4 动作列表变化时重排

若某帧的 `frame.order.size() != actions.size()`（例如 UI 动态增加/减少控件），会重新 resize、shuffle 并置 `nextIndex = 0`，保证顺序与当前动作列表一致。

---

## 七、性能考虑

### 7.1 计算与内存

- **栈**：深度通常为“已访问路径深度”，有界于单次运行步数；每帧存 state 指针 + 一个 vector（动作下标），开销小。
- **Edge/state 统计**：只存 hash 和少量计数，规模与“不同 (state, action) 对”和“不同 state”数量相关，与 Double SARSA 的 Q 表量级类似或更小（无 double 值）。
- **每步**：一次栈顶帧扫描、若干 hash 查找和比较，无 Q 更新或 N-step 回溯，**单步成本通常低于 Double SARSA**。

### 7.2 与状态抽象配合

DFS 不依赖 state 的“细粒度”：只要图按 state hash 去重、动作列表与访问计数由 Graph/State 维护，DFS 即可工作。因此与**动态状态抽象**（按 activity 的 refine/coarsen）兼容良好：抽象变化只改变 hash 与合并状态，DFS 的栈与优先级逻辑不变。

---

## 八、使用建议

### 8.1 关键阈值（代码常量）

当前实现中的阈值可在 `DFSAgent.cpp` 中调整：

- **Block**：BACK > 5，DEEP_LINK > 10，CLEAN_RESTART > 15，self-rescue >= 12；
- **Tarpit**：窗口 50 步，最少 30 步，不同 activity ≤ 3；
- **State block 日志**：连续相同 state hash ≥ 12 步时打 log。

---

## 九、参考文献与相关文档

1. 本项目：`DFSAgent.cpp` / `DFSAgent.h`。
2. 本项目：`DOUBLE_SARSA_ALGORITHM_EXPLANATION.md` — Double SARSA 详解与对比参考。
3. 经典教材：Cormen et al., *Introduction to Algorithms* — 深度优先搜索与栈实现。
