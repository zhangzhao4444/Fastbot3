/**
 * @authors Zhao Zhang
 */

# BFS 算法详解与优势分析

## 执行摘要

本文档详细解释 **BFSAgent** 在 Android UI 测试中采用的**广度优先探索（Breadth-First Search, BFS）**算法的原理与实现。BFS 与 DFSAgent 一样不依赖 Q 值或强化学习模型，完全基于状态图的结构与访问计数；通过**显式队列（FIFO）**实现“先一层、再下一层”的层序探索，并配有**显式深度、严格层序、帧耗尽时 BACK、覆盖驱动 DEEP_LINK、Activity 级去重**等改进，在保持无模型、可解释的前提下，更偏向均匀覆盖与“先近后远”的语义。

**当前实现**：`BFSAgent` 在共享状态图之上实现 **显式队列 + 层深 + 动作优先级 + 目标状态饱和判断** 的 BFS，并与 root 保护、block/tarpit、覆盖驱动 DEEP_LINK、同一 activity 至多一帧入队等结合，形成一套完整的无模型、层序探索算法。

---

## 一、算法背景与核心问题

### 1.1 为何需要“层序”探索

在 UI 自动化测试中，除深度优先（DFS）外，另一种自然策略是**广度优先（BFS）**：

1. **深度优先（DFS）**：沿一条路径尽量往深处走，再回溯；容易“先打穿一条路”，适合快速触及深层页面。
2. **广度优先（BFS）**：先穷尽“一步可达”的交互，再“两步可达”，依此类推；在无权图上形成**同心圆式**扩展，**到每个可达节点的步数最短**，覆盖更均匀。

**BFS 要解决的核心问题**：

- 在**未知或难以定义奖励**的 UI 图上，如何**按“层”系统性地覆盖**，使先近后远、覆盖更均匀？
- 如何避免在同一页面打转（卡住），并与 DFS 一样做 escalation（BACK → DEEP_LINK → CLEAN_RESTART）？
- 如何在根页面避免 BACK 导致退出应用或 BACK 死循环？
- 如何控制队列规模（同一 activity 多状态导致队列膨胀）？

### 1.2 BFS 的定位

BFS 是一种**确定性（在给定队列与深度下）+ 随机性（动作顺序 shuffle）**的探索策略：

- **层序优先**：维护显式深度（depth）与当前层（_currentDepth），优先处理当前层的帧，更深层帧移到队尾延后，实现“先一层、再下一层”。
- **队列 FIFO**：新发现的状态从队尾入队，选动作从队首取帧；同一状态只入队一次（_stateInQueue），同一 activity 至多一帧在队（activity 级去重）。
- **无模型**：不估计“动作价值”，只区分未访问 / 已访问但未饱和 / 已饱和，与 DFS 一致。

---

## 二、BFS 算法原理

### 2.1 基本概念

**状态图**：与 DFS 相同，节点为 UI 状态（state hash），边为“状态 + 动作”的转移；图由 Model/Graph 维护，BFSAgent 只使用状态、动作列表、访问计数、饱和等接口。

**显式队列（Queue of Frames）**：每帧为 `(state, nextIndex, order, depth)`。

- `state`：该帧对应的状态。
- `order`：该状态动作下标的随机排列（每帧独立 shuffle），决定尝试顺序。
- `nextIndex`：下一轮扫描从 `order[nextIndex]` 开始，实现断点续扫。
- `depth`：从根状态到该状态的步数（用于严格层序与可复用动作的深度权重）。

**无回溯 trim**：BFS 不像 DFS 那样“回到已访问节点并 trim 栈”；同一状态只入队一次（由 _stateInQueue 保证），不会从队中移除再重新入队（除非 sync-to-front 将当前状态挪到队首）。

### 2.2 动作选择优先级（单帧内）

对**当前队首帧**对应的状态，按以下优先级选一个动作（只选一个即返回）：

```
1. 未访问的、有目标（requireTarget）的动作
   → 若有多个，按 getActionTypeWeight 取更高权重的（如 CLICK > SCROLL > BACK）
2. 未访问的、其它有效动作
   → 同样按 typeWeight 择优
3. 已访问但“未饱和”的动作（reusable）
   → 饱和定义：该动作在 state 上 isSaturated，或目标状态已全访问（isActionSaturatedByTargetState）
   → 得分 = getEdgeNoveltyScore * typeWeight * depthWeight（depthWeight 使浅层略优先）
   → novelty 中 activityPart 系数为 3.0，优先“能打开新 activity”的边（Delm 思路）
4. 若以上都没有 → 当前帧“耗尽”，可选返回 BACK（若非 root）并 pop 本帧；否则直接 pop，继续队首
```

**设计意图**：

- 与 DFS 相同的 1/2/3 优先级，保证未访问与 reusable 的语义一致。
- **depthWeight**：浅层帧的 reusable 动作略优先，在不改队列顺序的前提下偏向“先近后远”。
- **activityPart 权重 3.0**：可复用边中，历史上更容易带来新 activity 的边更优先，利于 activity 覆盖。

### 2.3 严格层序（当前层优先）

- 维护 **`_currentDepth`**（当前要探索的 BFS 层）。
- 在 BFS 循环中：若队首帧的 **`frame.depth > _currentDepth`**，说明本层已处理完，将该帧**移到队尾**并置 `_currentDepth = frame.depth`，continue，从而保证先处理完当前层再处理更深层。
- 若 **`frame.depth < _currentDepth`**（例如 DEEP_LINK 后 sync 到队首的浅层帧），则置 `_currentDepth = frame.depth` 后正常处理，实现浅层优先。

### 2.4 帧耗尽与 BACK

- 当当前帧所有有效动作都不满足 1/2/3 时，该帧视为耗尽。
- 若存在 BACK 且**不是 root state / root activity**：先 pop 本帧并更新 _stateInQueue、_activityInQueueCount，再**返回 BACK**，使设备回到父界面，便于从同一父层探索其它子状态（层间切换）。
- 若是 root：不返回 BACK，只 pop，继续循环；若队列空则进入“队列空回退”（random）。

### 2.5 覆盖驱动 DEEP_LINK

- 除 block/tarpit 触发的 DEEP_LINK 外，**每 N 步**（默认 25，`kCoverageDrivenDeepLinkInterval`）主动尝试一次 **DEEP_LINK**，不依赖“未覆盖 activity 列表”，给系统机会跳转到新入口（Delm 思路的轻量版）。
- 步数由 `_selectCallCount` 计数，每次 selectNewAction 自增；任一 DEEP_LINK 或 CLEAN_RESTART 后置 0。

### 2.6 队列空回退

当队列被 pop 空后：

- 以**当前状态**（`_newState`）做多次 `randomPickAction`，尽量选非 BACK；再试 `randomPickUnvisitedAction`。
- 与 DFS 的栈空回退一致，保证无结构可依时仍能给出动作。

---

## 三、实现细节

### 3.1 核心数据结构

```cpp
// 显式 BFS 队列：每帧 = 状态 + 下一尝试下标 + 动作下标排列 + 深度
struct Frame {
    StatePtr state;
    size_t nextIndex;
    std::vector<size_t> order;
    int depth = 0;   // 从根的步数，用于严格层序与深度权重
};
std::deque<Frame> _queue;

// 队中状态 hash 集合（同一状态只入队一次）
std::unordered_set<uintptr_t> _stateInQueue;
// 状态 hash -> 深度（moveForward 时维护）
std::unordered_map<uintptr_t, int> _stateDepth;
// 当前 BFS 层（严格层序时只处理 depth == _currentDepth 的帧）
int _currentDepth = 0;
// activity 名 -> 队中该 activity 的帧数（同一 activity 至多一帧在队）
std::unordered_map<std::string, int> _activityInQueueCount;

std::unordered_set<uintptr_t> _visitedStates;
std::unordered_set<std::string> _visitedActivities;
std::unordered_map<std::uint64_t, EdgeStats> _edgeStats;

std::string _rootActivity;
int _stateBlockCounter, _activityBlockCounter;
uintptr_t _lastStateHash;
std::string _lastActivity;

// 覆盖驱动 DEEP_LINK：每 N 步尝试一次
static constexpr int kCoverageDrivenDeepLinkInterval = 25;
int _selectCallCount = 0;

std::deque<std::string> _recentActivities;  // tarpit 滑动窗口
std::unordered_map<std::uint64_t, uintptr_t> _edgeToTarget;
std::unordered_map<uintptr_t, std::pair<int, int>> _stateCoverage;
```

### 3.2 moveForward：队列与统计的更新

执行完动作并进入新状态后，`moveForward(state)` 被调用。BFSAgent::moveForward 在基类更新状态之后：

1. **Block 计数**：与 DFS 相同，按 state hash 与 activity 更新 _stateBlockCounter、_activityBlockCounter。
2. **Root**：首次遇到某 activity 时设为 _rootActivity。
3. **Edge 统计与目标饱和**：与 DFS 相同，更新 _edgeStats、_edgeToTarget、_stateCoverage。
4. **深度**：`curDepth = (prev 存在且 _stateDepth 中有 prev) ? parentDepth+1 : 0`，写入 `_stateDepth[cur->hash()]`。
5. **入队**：若 `_stateInQueue` 中无 cur 且 **同一 activity 在队中帧数为 0**（activity 级去重），则构造新帧（含 depth=curDepth），shuffle order，**从队尾入队**，并更新 _stateInQueue、_activityInQueueCount。

这样，同一 state 不会重复入队；同一 activity 至多一帧在队，控制队列规模。

### 3.3 selectNewAction：策略与 escalation

**顺序**（在进入 BFS 循环前）：

1. **`_selectCallCount++`**，用于覆盖驱动 DEEP_LINK。
2. **状态/activity 变化**：若 _newState 的 hash 或 activity 变化，重置 block 计数器。
3. **当前状态与队首同步**：若决策状态为 _newState 且与队首状态不同（例如 DEEP_LINK/CLEAN_RESTART 后），则**将 _newState 挪到队首**（若该 state 已在队中则先 erase 一帧再 push_front，否则直接 push_front 并更新 _stateInQueue/_activityInQueueCount），保证 BFS 循环针对当前页选动作。
4. **Block escalation**（与 DFS 一致）：
   - blockTimes > 15 → CLEAN_RESTART，清空队列、_stateDepth、_currentDepth、_activityInQueueCount、_selectCallCount。
   - blockTimes > 10 或 inTarpit 且 > 5 → DEEP_LINK，_selectCallCount = 0。
   - blockTimes > 5 且仅 BACK 可用且非 root → 返回 BACK。
   - blockTimes >= 12 → self-rescue（优先未访问非 BACK，其次任意非 BACK）。
5. **覆盖驱动 DEEP_LINK**：若 `_selectCallCount >= kCoverageDrivenDeepLinkInterval`，则 _selectCallCount = 0 并返回 DEEP_LINK。
6. **BFS 循环**：若队首 frame.depth > _currentDepth，将该帧移到队尾并更新 _currentDepth，continue；若 frame.depth < _currentDepth，更新 _currentDepth 后继续。然后按 §2.2 的优先级对队首帧选动作；若当前帧耗尽则 BACK（若非 root）+ pop 并更新 _stateInQueue、_activityInQueueCount，直至队列空或返回某动作。
7. **队列空**：按 §2.6 random 回退（优先非 BACK）。

### 3.4 动作类型权重与 novelty

- **getActionTypeWeight**：与 DFS 相同，CLICK/LONG_CLICK=3，SCROLL 系列=2，BACK/NOP/SHELL=0.5。
- **getEdgeNoveltyScore**：`statePart + 3.0 * activityPart`（activity 权重高于 DFS 的 2.0），优先“能打开新 activity”的边。
- **可复用动作**：得分乘以 **depthWeight = 1.0 / (1.0 + 0.05 * frame.depth)**，浅层帧略优先。

---

## 四、BFS 的优势

### 4.1 无奖励、无 Q 值、无超参

与 DFS 相同：不依赖奖励、Q 值、学习率；行为由当前队列、深度、访问计数与饱和决定，部署简单、可解释、适合冷启动。

### 4.2 层序带来的覆盖特性

**广度优先**意味着：

- 先处理“一步可达”的状态，再“两步可达”，在无权图上到每个可达节点的**步数最短**。
- 配合严格层序（深帧移队尾）与 BACK 层间切换，物理界面上也体现“先浅层、再深层”，有利于**均匀覆盖**各层界面，减少“一条路钻到底再回溯”的偏向。

**与 DFS 的对比**：

- DFS：先纵深、后回溯，容易先打穿一条路径上的 activity。
- BFS：先广后深，同一“层”内的状态会更早被尝试，适合希望**各入口/各 activity 更均衡**被触发的场景。

### 4.3 严格层序与 BACK 层间切换

- **严格层序**：队首帧深度大于当前层时移队尾，保证本层处理完再处理下一层，避免“队中有深有浅时乱序处理”。
- **帧耗尽时 BACK**：退回父界面，使设备与“队列中下一帧可能对应的父状态”一致，便于继续从父层扩展其它子状态，与“先一层、再下一层”语义一致。
- **Root 保护**：与 DFS 相同，根状态/根 activity 下不返回 BACK，避免退出应用或 BACK 死循环。

### 4.4 Activity 级去重与覆盖驱动 DEEP_LINK

- **同一 activity 至多一帧在队**：减少同一 activity 下多状态导致的队列膨胀，同时仍能通过“不同 activity 各一代表”实现 activity 级覆盖。
- **覆盖驱动 DEEP_LINK**：每 N 步主动尝试 DEEP_LINK，在不提供“未覆盖 activity 列表”的前提下，增加跳转到新入口的机会（Delm 思路的轻量版）。

### 4.5 与 DFS、Double SARSA 的互补

| 维度         | Double SARSA              | DFS                          | BFS                               |
|--------------|----------------------------|------------------------------|------------------------------------|
| 探索顺序     | 由 Q 与 ε-greedy 决定      | 先纵深、后回溯                | 先一层、再下一层（层序）           |
| 依赖         | 奖励、Q、超参              | 图结构、访问计数、饱和        | 同 DFS                             |
| 覆盖特性     | 依赖奖励与学习             | 先打穿一条路                  | 更均匀、先近后远                   |
| 队列/栈      | 无                         | 显式栈                        | 显式队列 + 深度 + activity 去重   |
| 卡住处理     | 依赖奖励/探索              | BACK/DEEP_LINK/CLEAN_RESTART  | 同 DFS + 覆盖驱动 DEEP_LINK        |

---

## 五、算法对比

### 5.1 BFS vs DFS（策略层面）

| 特性           | DFS                           | BFS                                    |
|----------------|-------------------------------|----------------------------------------|
| **结构**       | 显式栈，trim 回溯             | 显式队列，无 trim，同一状态只入队一次 |
| **顺序**       | 先纵深、后回溯                 | 先当前层、再下一层（严格层序）         |
| **帧耗尽**     | BACK + pop                    | BACK（若非 root）+ pop，层间切换       |
| **去重**       | 栈中 state 可重复（不同路径）  | 队中 state 唯一；同一 activity 至多一帧 |
| **深度**       | 无显式 depth                  | 显式 depth + _currentDepth              |
| **覆盖驱动**   | 仅 block/tarpit DEEP_LINK      | 每 N 步 DEEP_LINK + block/tarpit       |
| **可解释性**   | 高（栈+优先级+block）         | 高（队列+层+优先级+block）             |

### 5.2 BFS vs Double SARSA

| 特性           | Double SARSA              | BFS                                    |
|----------------|----------------------------|----------------------------------------|
| **模型**       | 双 Q 表 + N-step 更新      | 无 Q 表，显式队列 + 深度 + 访问计数    |
| **超参**       | alpha, gamma, epsilon, N  | 无（仅阈值与间隔常量）                  |
| **奖励**       | 必须定义                  | 不需要                                |
| **探索逻辑**   | ε-greedy + Q 值           | 层序 + 未访问 > reusable > BACK        |
| **卡住处理**   | 依赖奖励/探索             | BACK/DEEP_LINK/CLEAN_RESTART + 覆盖 DEEP_LINK |

---

## 六、实现细节说明

### 6.1 当前状态与队首不同步时的修复（sync-to-front）

若本步状态来自 DEEP_LINK/CLEAN_RESTART 等，_newState 可能与队首不一致。在进入 BFS 的 while 前，若 `state == _newState` 且与队首 hash 不同，则**将 _newState 挪到队首**：若该 state 已在队中则从队中 erase 一帧（并更新 _activityInQueueCount），再 push_front 新帧并更新 _stateInQueue/_activityInQueueCount，保证 while 循环针对当前页选动作。详见 BFSAgent_REVIEW.md。

### 6.2 严格层序的“移队尾”语义

当 `frame.depth > _currentDepth` 时，将该帧移到队尾并 `_currentDepth = frame.depth`。这样当前层（_currentDepth）的帧会陆续出现在队首并被处理，更深层的帧暂时移到队尾，等本层处理完（队首不再出现当前层）后，下一轮会再次看到更深层帧并更新 _currentDepth，实现“先一层、再下一层”。若队首出现更浅层（frame.depth < _currentDepth），则直接更新 _currentDepth 并处理，实现浅层优先（例如 sync 来的新根）。

### 6.3 Root 的双重判断

与 DFS 相同：**isRootState**（队列仅一帧）、**isRootActivity**（当前 activity == _rootActivity）。两者任一满足时不返回 BACK，只 pop，避免退出应用或 BACK 死循环。

### 6.4 动作列表变化时重排

若某帧的 `frame.order.size() != actions.size()`，会重新 resize、shuffle 并置 nextIndex = 0，与 DFS 一致。

---

## 七、性能考虑

### 7.1 计算与内存

- **队列**：规模受 _stateInQueue 与 _activityInQueueCount 限制；同一 state 只一帧，同一 activity 只一帧，队列大小有界于“不同 state 数”与“不同 activity 数”的较小者（在 activity 去重下）。
- **深度与层**：每帧一个 int depth，_stateDepth 与 _currentDepth 开销小。
- **每步**：队首帧扫描、可选“移队尾”、hash 查找与 novelty 计算，与 DFS 同量级；无 Q 更新。

### 7.2 与状态抽象配合

BFS 同样不依赖 state 的细粒度，只要图按 state hash 去重、动作与访问计数由 Graph/State 维护即可；与动态状态抽象兼容。Activity 级去重可视为一种轻量状态抽象（按 activity 合并）。

---

## 八、使用建议

### 8.1 关键阈值（代码常量）

当前实现中的阈值可在 `BFSAgent.cpp` / `BFSAgent.h` 中调整：

- **Block**：BACK > 5，DEEP_LINK > 10，CLEAN_RESTART > 15，self-rescue >= 12；
- **Tarpit**：窗口 50 步，最少 30 步，不同 activity ≤ 3；
- **覆盖驱动 DEEP_LINK**：每 25 步（`kCoverageDrivenDeepLinkInterval`）；
- **State block 日志**：连续相同 state hash ≥ 12 步时打 log。

---

## 九、参考文献与相关文档

1. 本项目：`BFSAgent.cpp` / `BFSAgent.h`。
2. 本项目：`DOUBLE_SARSA_ALGORITHM_EXPLANATION.md` — Double SARSA 详解与对比参考。
3. 经典教材：Cormen et al., *Introduction to Algorithms* — 广度优先搜索与队列实现。
