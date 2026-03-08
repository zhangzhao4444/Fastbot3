/**
 * @authors Zhao Zhang
 */

# 历史经验 Model 复用 

本文档描述 Fastbot 中**历史经验模型（Reuse Model）**的架构、数据流与持久化机制，便于理解与维护。

---

## 1. 概述

### 1.1 功能

- **防局部循环**：利用历史统计识别易形成局部环路的动作，在 reward/选动作时降权，而非硬性禁止。
- **加速覆盖**：优先选择历史上更易到达未访问 Activity 的动作，并引入多样性（目标 Activity 数量）等偏置。

### 1.2 适用范围

| 组件 | 说明 |
|------|------|
| **SarsaAgent** | 单 Q 表 + Reuse 统计，经验落盘为 `.fbm` |
| **DoubleSarsaAgent** | 双 Q 表 + 同一套 Reuse 统计，落盘格式与 Sarsa 相同 |

Reuse 能力仅在 **Sarsa** 与 **DoubleSarsa** 中启用；由 JNI 初始化时根据算法类型加载对应 Agent 的 Reuse Model。

---

## 2. 架构

### 2.1 总体架构图

```
  +------------------ 应用层 ------------------+
  |  [ JNI: initAgent + packageName ]          |
  +--------------------+----------------------+
                       |
                       v
  +------------------ Agent 层 ----------------+
  |   [ SarsaAgent ]      [ DoubleSarsaAgent ]  |
  +--------+------------------------+----------+
           |                        |
           v                        v
  +------------------ Reuse 核心 --------------+
  |  ( 内存: _reuseModel / ReuseEntryIntMap )  |
  |  [ loadReuseModel ]  [ saveReuseModel ]     |
  |  [ updateReuseModel ] [ getNewReward /     |
  |    computeRewardOfLatestAction ]           |
  |  [ selectActionInModel / selectActionByQValue ]
  +--------+------------------------+----------+
           |                        |
           v                        v
  +------------------ 公共 --------------------+
  |  ReuseDecisionTuning (loopBias/diversity/prior)
  |  Preference (max.reuse.*)                  |
  +--------------------------------------------+
           |
  +--------+--------+                          |
  v                 v                         v
  [ .fbm 文件 ]  <-- load/save 读写
  ( FlatBuffers )    Storage
```

### 2.2 模块职责

| 模块 | 职责 |
|------|------|
| **SarsaAgent / DoubleSarsaAgent** | 持有 `_reuseModel`，负责 load/save/update、reward 计算与动作选择，并调用决策层偏置 |
| **ReuseDecisionTuning** | 公共函数：基于单条动作的 activity 分布计算 loopBias、diversity、reusePrior（见 [5. 决策层偏置](#5-决策层偏置)） |
| **Preference** | 配置：`max.reuse.decisionTuning`、`useStaticReuseAbstraction` 等 |
| **FlatBuffers (ReuseModel)** | 磁盘格式：`ReuseModel → [ReuseEntry]`，与具体 Agent 解耦 |

### 2.3 数据流总览

```
  启动
    |
    v
  [ loadReuseModel ] ------> [ 内存 _reuseModel ]
    ^                              |
    |                              v
    |                        [ 每步: 选动作 ]
    |                              |
    |                              v
    |                        [ 执行 → 新状态 ]
    |                              |
    |              +---------------+---------------+
    |              |                               |
    |              v                               v
    |        [ updateReuseModel ]          [ getNewReward / computeReward ]
    |              |                               |
    |              +---------> 更新 _reuseModel <--+
    |                              |
    |                              v
    |                    [ 周期/析构 saveReuseModel ]
    |                              |
    +------------------------ [ .fbm ] (下次启动再 load)
```

---

## 3. 数据结构与存储

### 3.1 内存结构（ReuseEntryIntMap）

两种 Agent 共用同一逻辑结构（类型名在各 Agent 内定义，结构一致）：

```
ReuseEntryIntMap = map<
  actionHash: uint64,           // 动作抽象 hash（如 ActivityStateAction）
  ReuseEntryM                  // activity → 到达次数
>

ReuseEntryM = map<
  activityPtr: stringPtr,      // 目标 Activity 名
  times: int                   // 该动作到达该 Activity 的次数
>
```

即：**「动作 → 可能到达的 Activity 及频次」** 的统计表。

### 3.2 磁盘格式（.fbm）

- **Schema**：`storage/ReuseModel.fbs` → 生成 `ReuseModel_generated.h`
- **结构**：
  - `ReuseModel { model: [ReuseEntry] }`
  - `ReuseEntry { action: ulong, targets: [ActivityTimes] }`
  - `ActivityTimes { activity: string, times: int }`
- **路径**（由 `ModelStorageConstants` + packageName + 是否静态抽象 决定）：
  - 前缀：`/sdcard/fastbot_`（Android）
  - 动态抽象：`/sdcard/fastbot_{package}.fbm`
  - 静态抽象：`/sdcard/fastbot_{package}.static.fbm`
- **说明**：Sarsa 与 DoubleSarsa 的 .fbm 结构相同；DoubleSarsa 的 Q1/Q2 不落盘，仅内存使用。

### 3.3 写入策略（防损坏）

- 先写 **临时文件** `${path}.tmp.fbm`，成功后再 `rename` 为最终 `.fbm`，保证原子性。
- 保存前对 snapshot 做**隐式时间衰减**：`times = floor(times * 0.99)`，并去掉衰减为 0 的项，避免历史经验无限放大。

---

## 4. 流程图

### 4.1 加载 Reuse Model（loadReuseModel）

```
  loadReuseModel(packageName)
           |
           v
  [ 根据 useStatic 确定路径 ]
           |
           v
  [ 设置 _modelSavePath / _tmpSavePath ]
           |
           v
  [ 打开 .fbm 文件 ]
           |
           +-- 文件可打开? -- 否 --> [ clearReuseModelOnLoadFailure ] --> return
           |
          是
           v
  [ 读入完整内容 ]
           |
           +-- 大小合法? -- 否 --> clearReuseModelOnLoadFailure --> return
           |
          是
           v
  [ VerifyReuseModelBuffer ]
           |
           +-- 校验通过? -- 否 --> clearReuseModelOnLoadFailure --> return
           |
          是
           v
  [ GetReuseModel / model() 非空? ]
           |
           +-- 否 --> clearReuseModelOnLoadFailure --> return
           |
          是
           v
  [ 遍历 ReuseEntry 构建 ReuseEntryIntMap ]
           |
           v
  [ 加锁 swap 进 _reuseModel ]
           |
           v
         return
```

**失败时**：任意一步失败则调用 `clearReuseModelOnLoadFailure()`，清空内存中的 `_reuseModel`（DoubleSarsa 还会清空 Q1/Q2），不抛异常；本次运行从空模型开始积累，保存时会覆盖损坏的 .fbm。

### 4.2 保存 Reuse Model（saveReuseModel）

```
  saveReuseModel(path)
           |
           v
  [ 加锁拷贝 _reuseModel → snapshot ]
           |
           v
  [ 对 snapshot 做时间衰减: times = floor(times*0.99) ]
           |
           v
  [ 移除 times≤0 的项 ]
           |
           v
  [ 构建 FlatBuffers: ReuseEntry[] ]
           |
           v
  [ 写入 path.tmp.fbm ]
           |
           +-- 写入成功? -- 否 --> return
           |
          是
           v
  [ rename(tmp, path) ]
           |
           v
         return
```

### 4.3 经验更新（updateReuseModel）

```
  updateReuseModel
       |
       v
  [ 取 lastAction + _newState ]
       |
       v
  [ actionHash = hash(lastAction), activity = _newState.getActivityString ]
       |
       v
  [ 加锁 ]
       |
       +-- _reuseModel 已有 actionHash? -- 否 --> [ 插入 ReuseEntryM{ activity → 1 } ] --+
       |                                                                                  |
      是                                                                                  v
       v                                                                            [ 解锁 ]
  [ 对应 activity 的 times += 1 ] -------------------------------------------------------^
```

### 4.4 Reward 计算与决策偏置（Sarsa 示意）

```
  getNewReward
       |
       v
  [ 取 lastAction, _newState, visitedActivities ]
       |
       v
  [ reuseValue = getReuseActionValue(lastAction, visited) ]
       |
       +-- decisionTuning 开启? -- 否 --> [ reward += reuseValue / sqrt(visitedCount+1) ] ----+
       |                                                                                     |
      是                                                                                     |
       v                                                                                     |
  [ loopBias = computeLoopBias(hash, curActivity) ]                                         |
       |                                                                                     |
       v                                                                                     |
  [ diversity = computeCoverageDiversity(hash) ]                                             |
       |                                                                                     |
       v                                                                                     |
  [ reusePrior = computeReusePrior(reuseValue, loopBias, diversity) ]                        |
       |                                                                                     |
       v                                                                                     |
  [ reward += kBetaReuse * reusePrior / sqrt(visitedCount+1) ] ------------------------------+
       |
       v
  [ 加上 state 价值项等 ]
       |
       v
  return reward
```

DoubleSarsa 的 `computeRewardOfLatestAction` 逻辑与之对应：用 `probabilityOfVisitingNewActivities` 得到 probValue，再按同样 decisionTuning 分支计算 reusePrior 与归一化 reward。

### 4.5 动作选择与 Reuse（Sarsa 示意）

```
  resolveNewAction
       |
       v
  selectActionNotInModel
       |
       +-- 有未在 model 且未访问的动作? -- 是 --> [ 按权重随机选一个 ] -------+
       |                                                                     |
      否                                                                     |
       v                                                                     v
  selectActionInModel                                              return action
       |                                                                     ^
       +-- 有在 model 中且未访问的动作? -- 是 --> [ getReuseActionValue + Gumbel 选 ] -+
       |                                                                     |
      否                                                                     |
       v                                                                     |
  selectActionByQValue                                                       |
       |                                                                     |
       v                                                                     |
  [ reuseValue + Q 值 + Gumbel 选 ] -----------------------------------------+
```

---

## 5. 决策层偏置（ReuseDecisionTuning）

当 `max.reuse.decisionTuning=true` 时，Sarsa 与 DoubleSarsa 在计算 reward 时使用同一套公共逻辑（`agent/ReuseDecisionTuning.h`）：

| 项目 | 说明 |
|------|------|
| **loopBias** | 该动作历史中「停留在当前 Activity」的占比，用于惩罚自环倾向 |
| **diversity** | `log1p(目标 Activity 种类数)`，用于鼓励多目标动作 |
| **reusePrior** | `reuseValue * exp(-kAlphaLoop * loopBias) + diversity` |
| **常量** | `kAlphaLoop=2.0`，`kBetaReuse=0.1`（Sarsa 侧 reward 中 reuse 项系数） |

调用约定：**由 Agent 加锁并查表得到对应动作的 ReuseEntryM**，再调用 `computeLoopBiasFromEntry(actMap, currentActivity)`、`computeCoverageDiversityFromEntry(actMap)`；prior 用 `computeReusePrior(reuseValue, loopBias, diversity)` 得到。

---

## 6. 配置与入口

| 配置项 | 含义 | 默认 |
|--------|------|------|
| `max.reuse.decisionTuning` | 是否启用 loop/diversity/reusePrior 决策偏置 | false |
| `max.reuse.useStaticAbstraction` | 是否使用静态抽象（.static.fbm 路径） | false |

**加载入口**：JNI `initAgent` 中根据 `algorithmType == Sarsa / DoubleSarsa` 调用对应 Agent 的 `loadReuseModel(packageName)`。保存由 Agent 析构与后台周期任务触发。

---

## 7. 相关文件

| 文件 | 说明 |
|------|------|
| `REUSE_TECH_DESIGN.md` | 详细设计、统计层/存储层/决策层优化与 TODO |
| `agent/ReuseDecisionTuning.h` | 决策层公共函数与常量 |
| `agent/SarsaAgent.cpp/.h` | Sarsa Reuse 实现 |
| `agent/DoubleSarsaAgent.cpp/.h` | DoubleSarsa Reuse 实现 |
| `storage/ReuseModel.fbs` | FlatBuffers 定义 |
| `storage/ReuseModel_generated.h` | 序列化/反序列化 |
| `events/Preference.cpp/.h` | max.reuse.* 配置读取 |
| `agent/ModelStorageConstants.h` | 路径前缀与扩展名 |

---

*文档版本与代码一致，如有变更请同步更新。*
