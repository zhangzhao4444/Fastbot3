/**
 * @authors Zhao Zhang
 */

# LLMExplorerAgent 算法详解与分析

## 执行摘要

本文档详细解释 **LLMExplorerAgent** 在 Android UI 测试中采用的**知识引导探索（Knowledge-guided Exploration）**算法的原理与实现。算法对齐 LLM-Explorer 论文（ACM MobiCom '25，arXiv:2505.10593）：在**抽象交互图（AIG）**上维护**抽象状态**与**抽象动作**（带 Unexplored/Explored/Ineffective 标志），通过 **App-wide 动作选择**（优先当前界面未探索、否则全应用未探索随机）与 **AIG 最短路径导航** 实现“先导航到目标界面再执行目标动作”；执行后 **UpdateKnowledge** 更新 AIG 与标志，偏离路径时 **UpdateNavigatePath** 重规划，失败时**故障容忍**（替代路径、重启后重试、不可达删边）。**LLM 仅在两处使用**：新抽象状态出现时的**知识组织**（按功能聚类动作）与可编辑框的**内容感知输入生成**，选动作本身不调用 LLM，从而在保证探索质量的前提下控制成本。

**当前实现**：LLMExplorerAgent 在共享 Model/State 与自维护的 AIG（`_abstractKeyToId`、`_aigNextState`、`_absStateToActionHashes`）、动作标志与分组（`_actionFlags`、`_actionToGroup`、`_groupToActionHashes`、`_groupFunction`）、导航队列与故障容忍状态（`_navActionHashes`、`_navTargetAbsId`、`_navRetryAfterRestart` 等）之上，实现上述完整流程；与 LLMTaskAgent 共用同一 LlmClient（Payload 路径、Java 拼 prompt）。使用方式：**`--agent llmexplorer`** 或 **`--agent llm_explorer`**。

---

## 一、算法背景与核心问题

### 1.1 为何需要「知识引导」探索

在 UI 自动化测试中，除 BFS/DFS、前沿驱动（Frontier）、好奇心驱动（ICM）外，另一种策略是**知识引导**（Knowledge-guided）：

1. **BFS/DFS**：显式队列或栈、层序或深度；覆盖有结构，但依赖图与访问标记，状态爆炸时图大。
2. **FrontierAgent**：全局候选 + 信息增益 × 距离，选目标再寻路执行；需维护边与距离。
3. **ICMAgent**：当前状态合法动作上算 curiosity 得分（episode×全局新颖性），ε-greedy 选动作；无显式图、无路径规划。
4. **知识引导（LLM-Explorer）**：维护**抽象状态 + 抽象动作 + AIG**，每步选一个**未探索的抽象动作**；若该动作不在当前界面，则在 AIG 上**寻路**到能执行该动作的界面，再执行。这样既避免“只盯当前页”（app-wide 选择），又用图结构做**导航**，减少无效尝试；**选动作不每步调 LLM**，仅在新状态时用 LLM 做知识组织、在输入框时用 LLM 生成文本，成本与步数弱相关。

**LLMExplorerAgent 要解决的核心问题**：

- 如何用**规则**将具体 UI 状态归纳为**抽象状态**、将具体动作归纳为**抽象动作**（含探索标志），并维护 **AIG**（有向图：节点=抽象状态，边=抽象动作→下一状态）？
- 如何**选动作**：优先当前界面未探索、否则从全应用未探索中随机，且当目标动作不在当前界面时通过 **FindNavigatePath** 在 AIG 上寻路并执行路径第一步？
- 如何**更新知识**：每步后匹配抽象状态、更新抽象动作标志（explored/ineffective）、更新 AIG 边？如何在新抽象状态出现时用 **LLM** 将同功能控件聚类为抽象动作（可选）？
- 如何**容错**：导航一步后若实际状态与预期不符则 **UpdateNavigatePath** 重规划；若重规划失败则尝试替代路径、重启后重试，仍不可达则从 AIG 删边？

### 1.2 LLMExplorerAgent 的定位

LLMExplorerAgent 是一种**基于 AIG 与探索标志的知识引导探索策略**：

- **抽象状态**：规则方法——activity + 控件集合（排除 Text/ContentDesc 等动态属性）的 hash，相同则同一抽象状态；无 LLM。
- **抽象动作与标志**：每个 (抽象状态, 具体动作 hash) 对应一个逻辑“抽象动作”，标志为 **Unexplored** / **Explored** / **Ineffective**；新状态出现时可选用 **LLM** 将同功能控件合并为组，执行组内任一动作后整组标为同一标志。
- **AIG**：有向图，边 (srcAbsId, actionHash) → tgtAbsId；每步后若新边不存在则添加。
- **探索策略**：SelectExploreAction 优先当前状态 Unexplored 随机，否则全应用 Unexplored 随机；若选中动作不在当前界面则 FindNavigatePath 得到路径，执行路径第一步（进入导航模式）。
- **LLM 使用**：仅两处——(1) 知识组织：新抽象状态首次处理时，LLM 按功能分组返回 `{"groups": [[0,1],[2],...], "functions": [...]}`；(2) 内容感知输入：可编辑框时根据上下文生成输入文本。与 LLMTaskAgent 共用 LlmClient，prompt 在 Java 层由 payload 拼装（Payload 路径）。

---

## 二、算法原理

### 2.1 基本概念

**抽象状态（abstract UI state）**  
- 将“功能等价”的多个具体状态归为一类。实现上：**抽象状态键** = activity 字符串 + 当前状态下所有 widget 的 **hashWithMask(mask)** 的有序集合的混合 hash；mask 默认排除 Text/ContentDesc（可由 Model 的 getActivityKeyMask 覆盖，但若含 Text/ContentDesc 则退化为 Default），从而相同结构、不同动态内容的界面落入同一抽象状态。  
- **抽象状态 id**：对每个不同的 key 分配唯一 id（`_abstractKeyToId`、`_nextAbstractId`），用于 AIG 节点与边 key 的组成。

**抽象动作与探索标志**  
- **抽象动作**：在实现中对应 (抽象状态 id, 具体动作 hash)。每个这样的二元组有一个 **Exploration Flag**：**Unexplored**（未执行）、**Explored**（执行后到达新状态）、**Ineffective**（执行后状态未变或无效）。  
- **分组（可选）**：若启用 LLM 知识组织，同一抽象状态下的若干具体动作可被 LLM 归为同一“功能组”；执行组内任一动作后，**整组**的标志更新为相同值（探索一个即代表整组）。

**AIG（Abstract Interaction Graph）**  
- **节点**：抽象状态 id。  
- **边**：(srcAbsId, actionHash) → tgtAbsId，表示在 srcAbsId 对应界面上执行 actionHash 对应动作后，到达 tgtAbsId 对应界面。  
- **用途**：寻路（从当前抽象状态到目标抽象状态的最短路径）、以及判断某抽象状态上有哪些动作可走（`_absStateToActionHashes`）。

### 2.2 知识更新（UpdateKnowledge）

每执行一步得到 (srcState, actionTaken, tgtState) 后：

1. **抽象状态匹配**：对 srcState、tgtState 分别计算 `computeAbstractStateKey`，得到 srcKey、tgtKey；`getOrCreateAbstractStateId` 得到 srcAbsId、tgtAbsId。若为新 key 则分配新 id。  
2. **确保抽象动作已注册**：对 srcAbsId 调用 `ensureAbstractActionsForState`；**不对 tgtAbsId 在此处 ensure**（性能优化：推迟到下一步 selectNewAction 当该状态为当前状态时再 ensure）。因此每个新抽象状态仍在其**首次被处理**时（即第一次作为当前状态进入 selectNewAction 时）至多触发一次 **tryLlmKnowledgeOrganization**，与论文「新抽象状态首次处理时」语义一致；AIG 边仍在本步正常记录。  
3. **更新标志**：对 actionTaken 的 hash 与 srcAbsId 得到 flagKey；若 srcAbsId == tgtAbsId 则新标志为 **Ineffective**，否则为 **Explored**。写 `_actionFlags[flagKey]`；若该动作属于某组（`_actionToGroup`），则将该组内**所有**动作的 `_actionFlags` 设为同一新标志。  
4. **更新 AIG**：边 key = (srcAbsId, actionHash)；若该边尚不存在，则 `_aigNextState[edgeKey] = tgtAbsId`。

### 2.3 探索策略（SelectExploreAction）

- **输入**：当前 State、当前抽象状态 id currentAbsId。  
- **步骤**：  
  1. 收集当前 state 中、在 currentAbsId 下标志为 **Unexplored** 的动作（且通过 _validateFilter），放入 currentUnexplored。  
  2. 若 currentUnexplored 非空：在其中**均匀随机**选一个 actionHash，返回 (currentAbsId, actionHash)。  
  3. 否则：从 `_actionFlags` 中收集**所有**标志为 Unexplored 的 (absId, actionHash)，放入 allUnexplored。  
  4. 若 allUnexplored 为空则返回 (0, 0)；否则在其中**均匀随机**选一个，返回 (targetAbsId, chosenActionHash)。  
- **语义**：优先当前界面未探索；若当前没有则从全应用未探索中随机选一个（可能在其他界面），后续由主流程通过 FindNavigatePath 先导航再执行。

### 2.4 寻路（FindNavigatePath）

- **输入**：当前抽象状态 id currentAbsId、目标抽象状态 id targetAbsId；可选 **excludeEdgeKey**（用于故障容忍时排除某条边）。  
- **算法**：在 AIG 上 **BFS**。从 currentAbsId 出发，对每个节点的每条出边 (u, actHash) 若边 key 不等于 excludeEdgeKey 且边存在，则得到后继 v = _aigNextState[edgeKey]；若 v 未访问则入队并记录 parent。到达 targetAbsId 时回溯 parent 得到 action hash 序列（从 current 到 target 的边上的 actionHash 顺序），返回该序列。  
- **输出**：若存在路径则 outActionHashes 为从当前到目标需执行的 action hash 列表；否则返回 false。

### 2.5 主循环（selectNewAction）顺序

1. **无状态**：无 _newState 则 fallbackPickAction()。  
2. **防卡死**：与 BFS/DFS/ICM 一致，按 getCurrentStateBlockTimes()：> 15 → CLEAN_RESTART；> 10 → DEEP_LINK；> 5 且仅 BACK 可用 → BACK。同时清空导航与重试相关状态。  
3. **当前抽象状态**：computeAbstractStateKey、getOrCreateAbstractStateId、ensureAbstractActionsForState。  
4. **故障容忍：重启后重试**：若 _navRetryAfterRestart 且 _navTargetAbsId 非 0，则从当前状态寻路到 _navTargetAbsId；若找到路径则恢复 _navActionHashes、清空重试状态、返回路径第一步；若未找到且 _navRetryRestartCount >= kMaxNavRetryRestarts(1)，则从 AIG 删除 _navFailedEdgeKey、清空重试与目标；否则 _navRetryRestartCount++ 并返回 CLEAN_RESTART。  
5. **导航模式**：若 _navActionHashes 非空，取队首 actionHash，在当前 state 中 findActionByHash；若找到且通过 filter 则返回该动作；否则清空导航与重试状态。  
6. **探索**：调用 selectExploreAction 得到 (targetAbsId, chosenActionHash)。若 chosenActionHash==0 则 fallbackPickAction()。  
7. **当前界面有该动作**：若 findActionByHash(state, chosenActionHash) 找到且通过 filter，直接返回该动作。  
8. **需导航**：若 targetAbsId != currentAbsId 且 targetAbsId != 0，则 findNavigatePath(currentAbsId, targetAbsId, pathHashes)；若得到非空路径则写入 _navActionHashes、设 _navTargetAbsId、返回路径第一步；否则清空 _navTargetAbsId。  
9. **fallbackPickAction()**：在当前 state 的合法非 BACK 动作中随机，若无则尝试 BACK。

### 2.6 UpdateNavigatePath（论文 Algorithm 2 第 5 步）

在 **moveForward** 中，执行完动作并更新知识、且若本次是导航步则根据是否“执行了队首”做 pop 之后：

- 若 _navTargetAbsId != 0 且存在 fromState、actionTaken、nextState：  
  - 计算 currentAbsId（nextState 的抽象状态 id）、srcAbsId（fromState 的抽象状态 id）、edgeKey = (srcAbsId, actionTaken->hash())，以及 expectedTgt = _aigNextState[edgeKey]（若存在）。  
  - **偏离判定**：若本次并非执行的队首导航步，或虽执行了队首但 currentAbsId != expectedTgt，则视为**偏离路径**。  
  - **重规划**：从 currentAbsId 到 _navTargetAbsId 调用 findNavigatePath；若得到非空路径则用其替换 _navActionHashes。  
  - **故障容忍**：若重规划为空，再调用 findNavigatePath(..., excludeEdgeKey) 尝试**排除失败边**的替代路径；若仍为空则清空 _navActionHashes、设 _navRetryAfterRestart = true、_navFailedEdgeKey = edgeKey，等待下次 selectNewAction 中“重启后重试”或“删边放弃”。

### 2.7 故障容忍（Fault-tolerant）

- **替代路径**：UpdateNavigatePath 中重规划失败时，用 **excludeEdgeKey** 排除刚走过的边再寻路一次。  
- **重启后重试**：若仍失败则设 _navRetryAfterRestart；在 selectNewAction 开头，若处于该状态则从当前抽象状态再次寻路到 _navTargetAbsId；若找到则恢复导航队列并执行第一步；若未找到且 _navRetryRestartCount < kMaxNavRetryRestarts 则返回 **CLEAN_RESTART** 并增加计数，期待重启后再次进入时能从新状态找到路径。  
- **删边放弃**：若 _navRetryRestartCount >= kMaxNavRetryRestarts 仍无路径，则 **\_aigNextState.erase(_navFailedEdgeKey)**，并清空 _navTargetAbsId 与重试相关状态，避免后续再选到不可达路径。

### 2.8 防卡死与故障容忍的区别

| 维度 | 防卡死（Anti-stuck） | 故障容忍（Fault-tolerant） |
|------|----------------------|----------------------------|
| **触发条件** | **同一状态**连续多步无进展：`getCurrentStateBlockTimes()` 由基类在 `onAddNode` 中更新——当 `equals(_newState, _currentState)` 时自增，否则置 0。即「连续 N 次进入同一状态」后触发。 | **导航过程**中发生偏离或寻路失败：执行导航步后实际状态与 AIG 预期不符，或重规划/替代路径均失败，需重启后重试或删边。 |
| **适用场景** | 不限于导航：例如当前界面所有动作都是 Ineffective（每步后抽象状态不变）、或陷入弹窗/死循环、或设备无响应，导致多步停留在同一 state。 | 仅当存在导航目标（_navTargetAbsId）且本次或此前导航步骤「走错」或「路径不存在」时。 |
| **动作** | 按 blockTimes 阈值返回 BACK（>5 且仅 BACK 可用）/ DEEP_LINK（>10）/ CLEAN_RESTART（>15），并清空导航与重试状态。 | 重规划、替代路径、设 _navRetryAfterRestart、或返回 CLEAN_RESTART 一次后重试寻路、或删边并放弃该目标。 |

因此：**防卡死**解决的是「任意情况下同一状态连续多步无进展」的**通用逃逸**（与 BFS/DFS/ICM 一致）；**故障容忍**解决的是「正在按 AIG 导航到某目标时计划失败」的**专项处理**。二者互补，不重复；若去掉防卡死，在非导航场景下（如全 Ineffective 或死循环）将无法在 5/10/15 步内强制 BACK/DEEP_LINK/CLEAN_RESTART。

### 2.9 LLM 使用

- **知识组织（tryLlmKnowledgeOrganization）**：当某抽象状态**首次**被 ensureAbstractActionsForState 处理时，若 LlmClient 可用且合法动作数 ≥ 2，则构造 payload（elements 数组：class/resource_id/text/content_desc；max_index），调用 **predictWithPayload("knowledge_org", payload)**；Java 层拼 prompt（含 CoT 与 JSON 格式说明），请求 LLM 返回 `{"groups": [[0,1],[2],...], "functions": ["desc1", ...]}`。解析后写入 _actionToGroup、_groupToActionHashes、_groupFunction；解析失败或无 LLM 时退化为 1:1 分组。  
- **内容感知输入（getInputTextForAction）**：当所选动作的目标控件可编辑时，Model 在得到 Operate 后若 agent 的 getInputTextForAction 返回非空则写入 Operate 的 text。实现上构造 payload（package、activity、class、resource_id、text、content_desc），调用 **predictWithPayload("content_aware_input", payload)**，对返回 content 做 trim 与长度上限（200 字符）后返回。

### 2.10 从 knowledge_org 日志理解算法

典型 log 示例：

```text
// [LLM Explorer knowledge_org] reasoning: All ViewPager elements (0-3) are identical and likely serve as a carousel/slider. RecyclerViews (4-5) ...
// [LLM Explorer knowledge_org] action_clusters=17 (semantic groups of UI actions on this screen), cluster_descriptions=["carousel slider", "product list grid", ...]
```

- **reasoning**：LLM 在输出 JSON 前的简短推理（CoT），说明各 element 索引对应的控件类型与功能（如 0–3 是 ViewPager 轮播、4–5 是 RecyclerView 列表等）。仅用于可读与调试；算法**不解析** reasoning，只使用后面的 JSON。
- **action_clusters=17**：当前界面被 LLM 分成了 **17 个语义簇**（即 `groups` 数组长度为 17）。每个簇是一组“功能相同”的 UI 元素（例如多个轮播点归为一簇）。对应到实现：解析得到的 `groups` 写入了 `_groupToActionHashes`（每个 groupId 对应一簇 actionHash 集合），**选动作**时仍是在「未探索」里随机，不按簇描述排序；簇的作用是 **updateKnowledge** 时**整簇同标**（执行簇内任一动作后，该簇所有动作一起标为 Explored 或 Ineffective），从而减少重复尝试同功能控件。
- **cluster_descriptions**：与 17 个簇一一对应的**功能描述**（即 `functions` 数组），如 "carousel slider"、"product list grid"。存入 `_groupFunction`，便于人工看 log 理解当前界面的语义划分；**选动作逻辑当前未使用**这些描述，仅作可观测性。

因此：这条 log 表示「当前抽象状态首次做知识组织」：LLM 把界面上的可操作元素按功能聚成 17 簇，并给出了每簇的短描述；后续探索中，同一簇内任一点击都会导致整簇被标记，避免对“同一功能”的多个控件逐个试一遍。

---

## 三、实现细节

### 3.1 核心数据结构

```cpp
// 抽象状态：key -> id
std::unordered_map<uintptr_t, uintptr_t> _abstractKeyToId;
uintptr_t _nextAbstractId = 1;

// (absStateId, actionHash) -> Unexplored/Explored/Ineffective
std::unordered_map<uintptr_t, LLMExplorerActionFlag> _actionFlags;
// 键: kActionFlagKey(absId, actHash) = (absId << 32) | (actHash & 0xFFFFFFFFu)

// AIG: (srcAbsId, actionHash) -> tgtAbsId
std::unordered_map<uintptr_t, uintptr_t> _aigNextState;
// 键: kAigEdgeKey(absId, actHash)

// 每抽象状态的可选 action hash 集合（寻路用）
std::unordered_map<uintptr_t, std::unordered_set<uintptr_t>> _absStateToActionHashes;

// 知识组织：(absId, actionHash) -> groupId; (absId, groupId) -> action hashes; (absId, groupId) -> Function 描述
std::unordered_map<uintptr_t, uintptr_t> _actionToGroup;
std::unordered_map<uintptr_t, std::unordered_set<uintptr_t>> _groupToActionHashes;
std::unordered_map<uintptr_t, std::string> _groupFunction;
std::unordered_set<uintptr_t> _llmGroupingDoneForState;

// 导航：队首为下一步要执行的 action hash；目标抽象状态 id；故障容忍状态
std::deque<uintptr_t> _navActionHashes;
uintptr_t _navTargetAbsId = 0;
bool _navRetryAfterRestart = false;
uintptr_t _navFailedEdgeKey = 0;
int _navRetryRestartCount = 0;

// 防卡死（与 BFS/DFS/ICM 一致）
static constexpr int kBlockBackThreshold = 5;
static constexpr int kBlockDeepLinkThreshold = 10;
static constexpr int kBlockCleanRestartThreshold = 15;
static constexpr int kMaxNavRetryRestarts = 1;
```

### 3.2 moveForward：知识更新与 UpdateNavigatePath

1. 保存 fromState、actionTaken；调用 AbstractAgent::moveForward(nextState)。  
2. 若 fromState、actionTaken、nextState 均非空，调用 **updateKnowledge(fromState, actionTaken, nextState)**。  
3. 若本次是导航步且执行的动作等于队首 expectedHash，则 _navActionHashes.pop_front()。  
4. **UpdateNavigatePath**：若 _navTargetAbsId != 0 且存在 from/action/next，计算 currentAbsId、srcAbsId、edgeKey、expectedTgt；若偏离路径则先 findNavigatePath(currentAbsId, _navTargetAbsId, newPath)；成功则替换 _navActionHashes；失败则 findNavigatePath(..., edgeKey) 试替代路径；仍失败则设 _navRetryAfterRestart、_navFailedEdgeKey，清空 _navActionHashes。

### 3.3 selectNewAction：顺序与分支

见 **2.5 主循环顺序**；其中 ensureAbstractActionsForState 会触发 tryLlmKnowledgeOrganization（仅对尚未在 _llmGroupingDoneForState 中的抽象状态）。

### 3.4 动态状态抽象（onStateAbstractionChanged）

当 Model 在状态抽象变更后调用 onStateAbstractionChanged() 时，LLMExplorerAgent 清空：

- _abstractKeyToId、_nextAbstractId  
- _actionFlags、_aigNextState、_absStateToActionHashes  
- _actionToGroup、_groupToActionHashes、_groupFunction、_llmGroupingDoneForState  
- _navActionHashes、_navTargetAbsId、_navRetryAfterRestart、_navFailedEdgeKey、_navRetryRestartCount  

使 AIG 与知识仅使用**抽象变化之后**的新 key/id，避免旧图与新图混用。

---

## 四、LLMExplorerAgent 的优势

### 4.1 知识引导、选动作不每步调 LLM

与“每步都问 LLM 选动作”的方案相比，LLM 仅用于知识组织（新状态时聚类）与内容感知输入，选动作完全由 AIG + 探索标志 + 随机决定，成本与步数弱相关、易于控制。

### 4.2 App-wide 探索与导航

不局限于当前界面：当前无未探索时从全应用未探索中随机选，再通过 AIG 寻路到目标界面执行，系统性覆盖多界面。

### 4.3 故障容忍与路径修正

UpdateNavigatePath 在偏离时从当前状态重规划；失败时试替代路径、重启后重试、不可达则删边，减少无效导航与死循环。

### 4.4 与 LLMTaskAgent 共用 LLM 通道

LlmClient 由 LLMTaskAgent 持有，LLMExplorerAgent 通过 Model::getLlmClient() 获取；请求走 Payload 路径（Java 拼 prompt），与 LLMTaskAgent 一致，便于统一配置与运维。

---

## 五、算法对比

### 5.1 LLMExplorer vs ICM（策略层面）

| 特性         | ICMAgent                          | LLMExplorerAgent                                      |
|--------------|-----------------------------------|--------------------------------------------------------|
| **结构**     | 无显式图；当前状态 + episode/全局计数 | AIG（抽象状态 + 抽象动作 + 边）                         |
| **选动作**   | 当前状态合法动作 curiosity 得分 + ε-greedy | 优先当前 Unexplored，否则全应用 Unexplored 随机        |
| **导航**     | 无                                | 有；FindNavigatePath 在 AIG 上 BFS，执行路径第一步    |
| **新颖性**   | 1/√(1+n) × globalNovelty × stateFactor × successorFactor | Unexplored/Explored/Ineffective 标志 + 按组更新        |

### 5.2 LLMExplorer vs BFS/DFS

| 特性         | BFS/DFS                    | LLMExplorerAgent                          |
|--------------|----------------------------|-------------------------------------------|
| **图**       | 具体状态图（State hash）   | 抽象状态图（AIG）                         |
| **选目标**   | 队首/栈顶或未访问          | 未探索抽象动作（当前或 app-wide 随机）    |
| **执行**     | 沿队列/栈或直接            | 若目标不在当前界面则先寻路再执行         |

### 5.3 LLMExplorer vs LLMTaskAgent

| 特性         | LLMTaskAgent（任务驱动）       | LLMExplorerAgent（探索驱动）           |
|--------------|----------------------------------|----------------------------------------|
| **触发**     | 检查点匹配（max.llm.tasks）     | 每步由 selectNewAction 选动作          |
| **LLM 角色** | Planner/Executor/StepSummary   | 知识组织 + 内容感知输入                 |
| **图**       | 无                               | AIG                                    |

---

## 六、使用建议

### 6.1 使用方式

- 命令行：**`--agent llmexplorer`** 或 **`--agent llm_explorer`**（大小写不敏感）。  
- LLM 配置与 LLMTaskAgent 相同：`max.config` 中 `max.llm.enabled`、`max.llm.apiUrl`、`max.llm.model` 等；不配置则无知识组织与内容感知输入（退化为 1:1 分组与空输入）。

### 6.2 关键常数

- 防卡死：BACK > 5，DEEP_LINK > 10，CLEAN_RESTART > 15（与 BFS/DFS/ICM 一致）。  
- 故障容忍：kMaxNavRetryRestarts = 1（重启后最多再试一次，仍不可达则删边）。

---

## 七、参考文献与相关文档

1. 本项目：`LLMExplorerAgent.cpp` / `LLMExplorerAgent.h`。  
2. LLM-Explorer 论文：Shanhui Zhao et al., *LLM-Explorer: Efficient LLM-driven GUI Testing for Android Applications*, ACM MobiCom 2025, arXiv:2505.10593。  
