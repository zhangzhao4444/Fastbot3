/**
 * @authors Zhao Zhang
 */

# ICMAgent 算法详解与优势分析

## 执行摘要

本文档详细解释 **ICMAgent** 在 Android UI 测试中采用的**好奇心驱动探索（Curiosity-driven Exploration）**算法的原理与实现。ICMAgent 与 BFSAgent、DFSAgent、FrontierAgent 一样不依赖 Q 值或强化学习模型，完全基于**访问计数**与 **episode/全局双重新颖性**；通过 **「episode 内少重复 + 全局少访问 + 结构感知 bottleneck + 后继状态新颖性 + 轨迹多样性 + 乘性得分」** 的 WebRLED/NGU 风格框架，将 curiosity 得分定义为 **globalNovelty × episodeMod × stateFactor × successorFactor × pathFactor**（整体 cap 到 L=5），用 **ε-greedy**（ε 从 0.4 线性衰减到 0.05）在合法动作上选动作，在保持无模型、可解释的前提下，实现「少重复状态、少重复动作、优先能带来新状态的动作、优先在结构“路口”展开探索、避免重复走同一路径、鼓励离开过访状态」的探索语义。

**当前实现**：ICMAgent 在共享状态图与自维护的 `_episodeStateCount`、`_globalStateCount`、`_inDegree`/`_outDegree`（结构度）、`_succStats`（后继状态统计）、`_selfLoopCount`、`_recentStates`/`_pathCount`（轨迹签名）之上，实现 **episode 新颖性 1/√(1+n)**、**动作全局新颖性 1/(1+visitCount)**、**状态级全局因子 stateFactor（叠加结构 bottleneck 奖励）**、**后继状态新颖性因子 successorFactor（偏好通向全局访问少的后继状态的动作）**、**路径多样性因子 pathFactor（惩罚过度重复的轨迹模式）**、**加权随机的 ε-greedy 选动作**，并与防卡死（block → BACK / DEEP_LINK / CLEAN_RESTART）、有限长 episode（步数/状态数截断）、自环惩罚（多次自环的动作被降权）、状态抽象变更下的计数清空等结合，形成一套完整的无模型、好奇心驱动探索算法。使用方式：**`--agent icm`**。

---

## 一、算法背景与核心问题

### 1.1 为何需要「好奇心」探索

在 UI 自动化测试中，除深度优先（DFS）、广度优先（BFS）、前沿驱动（Frontier）外，另一种自然策略是**好奇心驱动（Curiosity-driven）**：

1. **深度优先（DFS）**：沿一条路径尽量往深处走，再回溯；容易「先打穿一条路」。
2. **广度优先（BFS）**：显式队列 + 层序，先一层再下一层；覆盖均匀、先近后远。
3. **前沿驱动（Frontier）**：全局候选 + 信息增益 × 距离，选一个目标再直接或沿路径执行。
4. **好奇心驱动（ICM）**：不建路径、不建队列，每步在**当前状态**的合法动作上算 **curiosity 得分**（少访问的动作得分高、当前状态在本 episode 内被访问越多则所有动作得分被 episode 项压低、当前状态全局被访问越多则 stateFactor 放大以鼓励离开），再 **ε-greedy** 选动作。兼顾「少重复动作」「少重复状态」「离开过访状态」。

**ICMAgent 要解决的核心问题**：

- 在**无奖励、无 Q 表**的 UI 图上，如何用**访问计数**构造一个标量得分，使「少访问的动作」和「离开重复状态」获得更高得分？
- 如何与 WebRLED 论文的 **episode 内新颖性 \(1/\sqrt{n}\)** 与 **全局新颖性乘性调制** 对齐，同时保持轻量（无 DNN、无 autoencoder）？
- 如何避免在同一页面打转（卡住），并与 BFS/DFS/Frontier 一样做 escalation（BACK → DEEP_LINK → CLEAN_RESTART）？
- 如何与动态状态抽象（refine/coarsen）兼容，在抽象变更时清空 episode/全局计数，避免旧 hash 与新 hash 混用？

### 1.2 ICMAgent 的定位

ICMAgent 是一种**无模型、基于访问计数的探索策略**：

- **双重新颖性**：**(1) Episode 内**：当前状态在本 episode 内被进入的次数 \(n\)，用 **episodeMod = 1/√(1+n)** 压低得分，鼓励离开重复状态；**(2) 全局**：动作的 **getVisitedCount()** 得到 globalNovelty = 1/(1+visitCount)，少访问动作得分高；当前状态的**全局访问次数**用 **stateFactor = 1 + kGlobalStateBonus×min(globalCount, cap)** 放大得分，鼓励离开全局过访状态。
- **乘性组合**：score = globalNovelty × episodeMod × stateFactor，整体 cap 到 kRewardCap=5（与 WebRLED/NGU 的 L=5 一致）。
- **ε-greedy**：以概率 ε 在合法动作中均匀随机，否则贪心取 curiosity 得分最大；ε 从 kEpsilonInitial=0.4 线性衰减到 kEpsilonMin=0.05，经 kEpsilonDecaySteps=10000 步。
- **Episode 边界**：CLEAN_RESTART、onStateAbstractionChanged、步数达到 kMaxEpisodeSteps(500)、或 episode 内状态数超过 kMaxEpisodeStateCount(2000) 时清空 episode 计数，开始新 episode。

---

## 二、算法原理

### 2.1 基本概念

**状态与动作**：与其它 Agent 一致，状态为 UI 状态（state hash），动作为当前状态上的合法动作（ActivityStateAction）；动作有 **getVisitedCount()** 表示该动作被选中的全局次数。

**Episode**：从一次 CLEAN_RESTART 或 episode 截断（步数/状态数上限）之后，到下一次截断或 CLEAN_RESTART 之前，称为一个 **episode**。Episode 内维护 **\_episodeStateCount[stateHash]** = 本 episode 内进入该状态的次数。

**全局状态计数**：**\_globalStateCount[stateHash]** = 自上次 onStateAbstractionChanged 以来进入该状态的总次数；仅在状态抽象变更时清空。

**Curiosity 得分**：对每个合法动作 \(a\)，  
score = **globalNovelty(a)** × **episodeMod** × **stateFactor**，再整体 cap 到 kRewardCap。  
- globalNovelty(a) = 1/(1 + visitCount(a))，动作越少被选得分越高。  
- episodeMod = 1/√(1+n)，n = 当前状态在本 episode 内被进入的次数（cap kEpisodeCap=50）；当前状态重复访问越多，episodeMod 越小，所有动作得分同比例降低，相对仍会选「全局更新少」的动作，并鼓励离开。  
- stateFactor = 1 + kGlobalStateBonus×min(globalCount(currentState), kGlobalStateCap)；当前状态被全局访问越多，stateFactor 越大，整体得分放大，鼓励离开该状态。

### 2.2 Episode 新颖性（episodeMod）

与 WebRLED 论文 **\(r_t^{episodic} = 1/\sqrt{n(f_t)}\)** 对齐，采用**精确 hash 计数**下的形式：

- **n** = 本 episode 内**当前状态**被进入的次数（即 \_episodeStateCount[currentHash]，选动作时查表得到）。
- **episodeMod** = 1/√(1+n)，n 用 kEpisodeCap=50 做上界，避免 n 过大时 episodeMod 过小导致数值不稳定。
- 语义：当前状态在本 episode 内出现越多，episodeMod 越小，curiosity 得分越低，鼓励离开重复状态；1/√n 形式在探索后期仍提供较密集激励（论文指出可避免奖励过早消失）。

### 2.3 全局新颖性（globalNovelty + stateFactor）

- **动作级**：globalNovelty = 1/(1 + getVisitedCount(action))。动作被选中次数越多，得分越低，优先尝试少访问动作。
- **状态级**：stateFactor = 1 + kGlobalStateBonus × min(globalCount(currentState), kGlobalStateCap)。当前状态被全局进入次数越多，stateFactor 越大，score 同比例放大，鼓励离开该状态。与论文「状态级全局新颖性」思想一致；实现上用计数替代 autoencoder，轻量且无需训练。

### 2.4 后继状态新颖性（successorFactor）

在仅使用当前状态/动作计数的版本中，ICM 无法区分「动作本身新」与「动作通常能否带来新页面」。为更快离开局部坑，当前实现为每个 `(state, action)` 近似维护一个「后继状态分布」，在得分中加入**后继状态新颖性因子**：

- 对每个 `ActivityStateAction`，用 `action->hash()` 作为 key，在 `moveForward(fromState, action, nextState)` 中维护：
  - `SuccessorStats.total`：该动作被执行的总次数；
  - `SuccessorStats.topNext`：该动作最常见的 **Top-K（默认 K=4）** 后继状态 hash 及其计数（使用 space-saving 近似，保证内存有界）。
- 在 `selectNewAction` 时，若存在后继统计：
  - 计算「期望全局访问次数」：
    \[
    \mathbb{E}[\text{globalCount(next)}] = \frac{\sum\_{(s',c)} c \cdot \text{globalCount}(s')}{\sum c}
    \]
  - 定义后继新颖性因子：
    \[
    \text{successorFactor}(a) = \max\left(\frac{1}{1 + \mathbb{E}[\text{globalCount(next)}]},\ kSuccessorMinFactor\right)^{kSuccessorAlpha}
    \]
    其中 `kSuccessorMinFactor` 默认为 0.1，用于避免因期望值过大导致得分几乎归零；`kSuccessorAlpha` 控制强度（当前取 1.0）。
- 语义：若某个动作历史上大多通向「全局访问次数很高」的旧状态，则 successorFactor 会小于 1，对其得分做**衰减**；反之，历史上通向全局访问少的新状态的动作，其 successorFactor 接近 1，甚至在 episodeMod 较高时仍可保持较高得分。

### 2.5 得分公式与截断

```
score(a) = globalNovelty(a) × episodeMod × stateFactor × successorFactor(a) × pathFactor
         = [1/(1+visitCount(a))] × [1/√(1+n)] × [1 + kGlobalStateBonus×min(globalCount, kGlobalStateCap)] × (1 + bottleneckBonus) × successorFactor(a) × pathFactor
```

- episodeMod 在 (0, 1]，getCuriosityScore 内若 &lt; 1e-6 则 clamp 到 1e-6。  
- stateFactor 在 getCuriosityScore 内 clamp 到 [1, kRewardCap]。  
- 最终 **raw = globalNovelty × episodeMod × stateFactor**，返回值 = min(raw, kRewardCap)，即整体 cap 到 L=5（与 WebRLED 论文一致）。

### 2.6 ε-greedy 与 ε 衰减

- 以概率 **ε** 在 scored 列表中**均匀随机**选一个动作；以概率 1-ε **贪心**取 score 最大的动作。
- **ε** 从 **kEpsilonInitial=0.4** 线性衰减到 **kEpsilonMin=0.05**：  
  decayProgress = min(_selectCount, kEpsilonDecaySteps) / kEpsilonDecaySteps，  
  epsilon = kEpsilonInitial - (kEpsilonInitial - kEpsilonMin) × decayProgress。  
  _selectCount 仅在「完成一次 curiosity 的 ε-greedy 选择」时自增（fallback/anti-stuck 提前返回时不增）。
- 论文 WebRLED §3.6 使用 ε=0.4；ICMAgent 初始 0.4 与之一致，后期衰减到 0.05 更贪心。

### 2.6 同分与 Tie-break

当多个动作 curiosity 得分相同（浮点容差 1e-9）时：先按 **getPriorityByActionType()** 取优先级最高的类型，若仍有多者则在其中**均匀随机**选一个。

### 2.7 防卡死（Anti-stuck）

与 BFS/DFS/Frontier 对齐，在 selectNewAction 开头根据 getCurrentStateBlockTimes()：

- blockTimes > 15 → **CLEAN_RESTART**；
- blockTimes > 10 → **DEEP_LINK**；
- blockTimes > 5 且当前仅有 BACK 可用（非 BACK 均不可用或为 NOP）→ 返回 **BACK**。

fallbackPickAction：优先选 **visitCount 最小且非 BACK** 的动作；若无则 randomPickAction；再否则尝试 getBackAction()（通过 filter 时），避免 NOP 死循环。

---

## 三、实现细节

### 3.1 核心数据结构

```cpp
// Episode 内状态访问次数（CLEAN_RESTART、步数截断、状态数截断、onStateAbstractionChanged 时清空）
std::unordered_map<uintptr_t, int> _episodeStateCount;

// 本 episode 已走步数（用于步数截断）
int _episodeSteps = 0;

// 全局状态访问次数（仅 onStateAbstractionChanged 时清空）
std::unordered_map<uintptr_t, int> _globalStateCount;

// 已完成「curiosity ε-greedy 选择」的次数（用于 ε 衰减，不重置）
int _selectCount = 0;

// 简单双向结构信息：用于 bottleneck 估计
std::unordered_map<uintptr_t, int> _inDegree;
std::unordered_map<uintptr_t, int> _outDegree;

struct SuccessorStats {
    int total = 0;
    // 仅保留每个动作最常见的 Top-K 后继状态，限制内存
    std::vector<std::pair<uintptr_t, int>> topNext;
};

// 每个动作的后继状态统计（按 ActivityStateAction::hash() 作为 key）
std::unordered_map<uintptr_t, SuccessorStats> _succStats;

// 自环统计：动作导致 fromHash == toHash 的次数（按 action hash 作为 key）
std::unordered_map<uintptr_t, int> _selfLoopCount;

// 轨迹多样性：最近 kPathWindowSize 个 state hash 组成的窗口，以及对应路径签名计数
std::deque<uintptr_t> _recentStates;
std::unordered_map<std::uint64_t, int> _pathCount;

// 防卡死阈值（与 BFS/DFS/Frontier 一致）
static constexpr int kBlockBackThreshold = 5;
static constexpr int kBlockDeepLinkThreshold = 10;
static constexpr int kBlockCleanRestartThreshold = 15;

// ε-greedy
static constexpr double kEpsilonInitial = 0.4;
static constexpr double kEpsilonMin = 0.05;
static constexpr int kEpsilonDecaySteps = 3000;

// Episode 新颖性：n 的上界（1/√(1+n)）
static constexpr int kEpisodeCap = 50;

// 得分上界（WebRLED L=5）
static constexpr double kRewardCap = 5.0;

// Episode 截断
static constexpr int kMaxEpisodeSteps = 300;
static constexpr size_t kMaxEpisodeStateCount = 2000;

// 状态级全局因子
static constexpr double kGlobalStateBonus = 0.15;
static constexpr int kGlobalStateCap = 20;

// 自环惩罚参数
static constexpr int kSelfLoopPenaltyThreshold = 3;
static constexpr double kSelfLoopPenaltyFactor = 0.2;

// 后继状态新颖性参数
static constexpr int kSuccessorTopK = 4;
static constexpr double kSuccessorMinFactor = 0.1;
static constexpr double kSuccessorAlpha = 1.0;

// 结构 bottleneck 参数
static constexpr double kBottleneckBeta = 0.1;

// 轨迹多样性参数
static constexpr int kPathWindowSize = 16;
static constexpr int kPathPenaltyThreshold = 3;
static constexpr double kPathPenaltyFactor = 0.5;
```

### 3.2 moveForward：计数更新与 episode 重置

执行完动作并进入新状态后，moveForward(nextState) 被调用。ICMAgent::moveForward 在调用基类**之前**保存 fromState、actionTaken，基类调用之后：

1. **CLEAN_RESTART**：若上一步动作为 CLEAN_RESTART，清空 \_episodeStateCount、\_episodeSteps。
2. **步数自增**：\_episodeSteps++。
3. **步数截断**：若 \_episodeSteps >= kMaxEpisodeSteps(500)，清空 episode 计数与步数。
4. **状态计数**：若 nextState 非空，则 toHash = nextState->hash()，\_episodeStateCount[toHash]++，\_globalStateCount[toHash]++。
5. **状态数截断**：若 \_episodeStateCount.size() > kMaxEpisodeStateCount(2000)，清空 episode 计数与步数。

注意：对 **nextState** 的 hash 自增，表示「进入该状态的次数」；选动作时用**当前状态**（即上一步的 nextState）的 hash 查表，故 episodeCount 表示「当前状态在本 episode 内被进入的次数」，语义一致。

### 3.3 selectNewAction：策略与回退

**顺序**：

1. **无状态**：无 \_newState 则 fallbackPickAction()。
2. **防卡死**：按 getCurrentStateBlockTimes() 做 CLEAN_RESTART / DEEP_LINK / BACK（仅当仅 BACK 可用时）。
3. **计算因子**：currentHash = state->hash()；episodeCount = \_episodeStateCount[currentHash] 或 0；n = min(episodeCount, kEpisodeCap)；episodeMod = 1/√(1+n)；globalCount = \_globalStateCount[currentHash] 或 0；stateFactor = 1 + kGlobalStateBonus×min(globalCount, kGlobalStateCap)，再叠加 bottleneck 奖励（按 inDegree/outDegree 计算）；同时基于最近窗口 \_recentStates 计算路径签名，看当前轨迹是否「过度重复」生成 pathOverused 标志。
4. **scored 列表**：对每个合法动作调用 getCuriosityScore(a, episodeMod, stateFactor)，随后叠加自环惩罚、后继状态新颖性 successorFactor、路径惩罚 pathFactor（当 pathOverused 为真时对所有动作统一降权），score≥0 的加入 scored；若 scored 为空则 fallbackPickAction()。
5. **ε-greedy**：_selectCount++，计算 epsilon（线性衰减）；若 unif(rng) < epsilon 则在 scored 中按得分加权随机选一个并返回；否则进入贪心。
6. **贪心**：在 scored 中取 score 最大的下标（同分用 1e-9 容差），若有多个同分则按 getPriorityByActionType() 再随机，返回对应动作。
7. **fallbackPickAction**：优先 visitCount 最小且非 BACK；否则 randomPickAction；再否则 getBackAction()（通过 filter 时）。

### 3.4 getCuriosityScore

- globalNovelty = 1/(1 + action->getVisitedCount())。
- episodeMod、stateFactor 由调用方传入；若 episodeMod &lt; 1e-6 则 clamp 到 1e-6；若 stateFactor &lt; 1 则 clamp 到 1，若 &gt; kRewardCap 则 clamp 到 kRewardCap。
- raw = globalNovelty × episodeMod × stateFactor；return min(raw, kRewardCap)。

### 3.5 动态状态抽象（onStateAbstractionChanged）

当 Model 在 refine/coarsen 后调用 onStateAbstractionChanged() 时，ICMAgent 清空：

- \_episodeStateCount
- \_episodeSteps
- \_globalStateCount

使 episode 与全局计数仅使用**抽象变化之后**新产生的 state hash，避免旧 hash 与新 hash 混用导致计数语义错误。

---

## 四、ICMAgent 的优势

### 4.1 无奖励、无 Q 值、无学习

与 BFS/DFS/Frontier 相同：不依赖奖励、Q 值、学习率；行为由当前状态、episode/全局计数与 curiosity 得分公式决定，部署简单、可解释、适合冷启动。

### 4.2 双重新颖性 + 乘性组合（WebRLED/NGU 对齐）

- **Episode 内**：1/√(1+n) 使重复访问状态时得分衰减，鼓励离开；1/√n 在后期仍提供较密集激励。
- **全局**：动作 visitCount 与状态 globalCount 双管齐下，少访问动作优先、过访状态放大因子鼓励离开。
- **乘性**：score = globalNovelty × episodeMod × stateFactor，整体 cap L=5，与论文及 NGU 一致，避免单一信号主导。

### 4.3 轻量、无神经网络

无需 DQN、autoencoder、MLP；仅用 hash 与整数计数，内存与计算开销小，易于在移动端或资源受限环境运行。

### 4.4 与 BFS/DFS/Frontier、Double SARSA 的互补

| 维度           | Double SARSA        | BFS                    | FrontierAgent              | ICMAgent                                |
|----------------|---------------------|------------------------|----------------------------|-----------------------------------------|
| 探索依据       | Q 与 ε-greedy       | 层序（队列 + 深度）    | 信息增益 × 距离（FH-DRL）  | curiosity 得分（episode×全局乘性）      |
| 依赖           | 奖励、Q、超参       | 图 + 访问计数、饱和    | 图 + 自记录边 + 访问计数   | 访问计数（episode+全局状态+动作）       |
| 覆盖特性       | 依赖奖励与学习      | 先一层再下一层         | 先近后远、高信息 frontier  | 少重复状态/动作、离开过访状态           |
| 结构           | 无                  | 显式队列 + 深度        | 候选 + BFS 距离 + 路径     | 无路径；当前状态动作 + 得分             |
| 卡住处理       | 依赖探索/奖励       | BACK/DEEP_LINK/CLEAN   | 同 BFS/DFS                 | 同 BFS/DFS/Frontier                     |

---

## 五、算法对比

### 5.1 ICM vs BFS（策略层面）

| 特性           | BFS                                    | ICMAgent                                          |
|----------------|----------------------------------------|---------------------------------------------------|
| **结构**       | 显式队列 + 层深 + 同一状态只入队一次    | 无队列；当前状态合法动作 + episode/全局计数       |
| **选动作**     | 队首帧 + 动作优先级（未访问/reusable）  | 当前状态所有合法动作 curiosity 得分 + ε-greedy   |
| **距离/层序**  | 显式 depth，层序                       | 无显式距离；episode 步数/状态数仅用于截断         |
| **新颖性**     | 未访问/饱和                            | 1/√(1+n) episode + 1/(1+visitCount) 动作 + stateFactor |

### 5.2 ICM vs DFS

| 特性           | DFS                    | ICMAgent                                  |
|----------------|------------------------|-------------------------------------------|
| **结构**       | 显式栈、trim 回溯      | 无栈；仅当前状态 + 计数                    |
| **选动作**     | 栈顶 + 未访问/reusable | 当前状态 curiosity 得分 + ε-greedy        |
| **执行**       | 纵深再回溯             | 每步独立算得分选动作，无显式路径          |

### 5.3 ICM vs Frontier

| 特性           | FrontierAgent                    | ICMAgent                                  |
|----------------|-----------------------------------|-------------------------------------------|
| **结构**       | 候选列表 + BFS 距离 + 路径         | 无路径；当前状态动作 + 得分                |
| **选目标**     | 全局候选 + FH-DRL score           | 仅当前状态动作 + curiosity score          |
| **执行**       | 直接执行 或 沿 BFS 路径走到再执行 | 每步直接选一个动作执行                    |
| **新颖性**     | infoGain×exp(-β×distance)         | episodeMod×globalNovelty×stateFactor      |

### 5.4 ICM vs Double SARSA

| 特性           | Double SARSA              | ICMAgent                    |
|----------------|----------------------------|-----------------------------|
| **模型**       | 双 Q 表 + N-step           | 无 Q；仅计数 + 得分公式     |
| **奖励**       | 必须定义                   | 不需要                      |
| **探索逻辑**   | ε-greedy + Q 值           | ε-greedy + curiosity score  |

---

## 六、实现细节说明

### 6.1 episodeCount 语义与时机

- moveForward(nextState) 中对 **nextState** 的 hash 自增 \_episodeStateCount；selectNewAction 中用 **当前状态** \_newState 的 hash 查 \_episodeStateCount。
- 因此 **episodeCount = 当前状态在本 episode 内被进入的次数**（包含「刚进入这一次」）。第一次站在某状态时 count=1，第二次为 2，…；n 越大 episodeMod 越小，符合「重复访问则压低得分、鼓励离开」的语义。

### 6.2 步数截断顺序

先 \_episodeSteps++，再判断是否 >= kMaxEpisodeSteps；若达到则清空 episode 与步数，再对 nextState 做计数。因此达到 300 步的那一步会清空，nextState 作为新 episode 的首状态被计为 1，下一步从 step 1 开始。UI 测试中采用较短 episode 可减少长时间局部盘旋。

### 6.3 _selectCount 与 ε 衰减范围

仅在有 scored 列表且未因 anti-stuck/fallback 提前返回时执行 _selectCount++ 与 ε-greedy。即 **ε 按「完成一次 curiosity 选择」的次数衰减**，fallback 或 CLEAN_RESTART/DEEP_LINK/BACK 不计数，属实现约定，非错误。

### 6.4 日志与可观测性

ICMAgent 在 moveForward 与 selectNewAction 中打 BDLOG（Debug 级），便于观察算法正确性：

- moveForward：CLEAN_RESTART 时、每次进入 nextState 后（toHash、episodeSteps、episodeCount[to]、globalCount[to]、#states）。
- selectNewAction：当前 stateHash、episodeCount、n、episodeMod、globalCount、stateFactor；随机/贪心时 u、epsilon、selectCount、所选 action 与 score。

查看方式：`adb logcat -s FastbotNative:D` 或过滤包含 `ICMAgent:` 的日志。

---

## 七、性能考虑

### 7.1 已做优化

- **episodeFactor 外提**：在 selectNewAction 中对 currentHash 只查一次 \_episodeStateCount、\_globalStateCount，算出 episodeMod 与 stateFactor 后传入 getCuriosityScore，避免对每个动作重复 map find。
- **Episode 状态数截断**：\_episodeStateCount.size() > kMaxEpisodeStateCount(2000) 时清空，限制单 episode 内存。
- **n 与 globalCount 上界**：kEpisodeCap=50、kGlobalStateCap=20，避免极端计数导致数值或放大过大。

### 7.2 常数与进一步优化

- **常数消融**：kEpsilonInitial、kRewardCap、kMaxEpisodeSteps、kGlobalStateBonus、kSelfLoopPenaltyThreshold、kSuccessorTopK、kBottleneckBeta、kPathWindowSize、kPathPenaltyThreshold 等可按覆盖率/卡死率做小规模消融；参见 WebRLED_technical_spec.md §12.7 常数与消融参考表。

---

## 八、使用建议

### 8.1 关键阈值（代码常量）

- **防卡死**：BACK > 5，DEEP_LINK > 10，CLEAN_RESTART > 15（与 BFS/DFS/Frontier 一致）。
- **ε**：kEpsilonInitial=0.4，kEpsilonMin=0.05，kEpsilonDecaySteps=3000。
- **得分**：kRewardCap=5.0，kEpisodeCap=50，kGlobalStateBonus=0.15，kGlobalStateCap=20。
- **Episode**：kMaxEpisodeSteps=300，kMaxEpisodeStateCount=2000。
- **自环/后继**：kSelfLoopPenaltyThreshold=3，kSelfLoopPenaltyFactor=0.2，kSuccessorTopK=4，kSuccessorMinFactor=0.1，kSuccessorAlpha=1.0。
- **结构/轨迹**：kBottleneckBeta=0.1，kPathWindowSize=16，kPathPenaltyThreshold=3，kPathPenaltyFactor=0.5。

### 8.2 配置与注册

- **使用方式**：命令行传入 **`--agent icm`**（大小写不敏感）。
- AgentFactory 已注册 ICMAgent；JNI 层将 agent 类型 "icm" 映射到对应 AlgorithmType 即可选用 ICMAgent。

---

## 九、参考文献与相关文档

1. 本项目：`ICMAgent.cpp` / `ICMAgent.h`。
2. WebRLED 论文：Gu et al., *Deep Reinforcement Learning for Automated Web GUI Testing*, arXiv:2504.19237, 2025（奖励模型 §3.5，探索策略 §3.6）。
3. NGU：Badia et al., *Never Give Up: Learning Directed Exploration Strategies*, 2020（episodic × life-long 乘性内在奖励）。
