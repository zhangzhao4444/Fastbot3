/**
 * @authors Zhao Zhang
 */

## Fastbot 状态抽象技术说明（State / Widget / Action）

本文面向当前 `android/native` 实现，对 **widget / state / action** 这三个核心概念，以及 **静态状态抽象** 与 **动态状态抽象** 的实现与算法细节做一个系统说明，方便之后调试和迭代。

---

## 1. 核心概念：Widget / State / Action

### 1.1 Widget：GUI 树中的“结点”

Fastbot 首先把 Android GUI（XML 或二进制 dump）解析成一棵树，每个节点抽象为一个 **widget**，关键属性包括：

- **结构属性**
  - `clazz`：Android View 类名，例如 `android.widget.Button`、`TextView`。
  - `resourceId`：视图的资源 ID，例如 `com.xxx:id/btn_login`。
  - `scrollType`：是否可滚动、滚动方向（对应 `ScrollType` 枚举）。

- **语义属性**
  - `text`：当前显示的文本内容。
  - `contentDesc`：无障碍用的描述（contentDescription）。

- **可交互属性（operate mask）**
  - 是否 `clickable` / `longClickable` / `scrollable` / `inputable` 等。

这些属性最终会参与到 **widget hash** 的计算中，是状态抽象的最小单元。

### 1.2 State：强化学习中的“状态” s

**State** 是 RL 里的状态 \(s\)，在 Fastbot 中大致表示：

> 「某个 Activity + 在该 Activity 下的一棵 widget 树，经抽象后得到的一个等价类」。

实现层面，一个 `State` 包含：

- 当前 `activity` 名（例如 `com.jingdong.app.mall.MainFrameActivity`）。
- 一组抽象后的 widget（动态抽象下为普通 widget，静态抽象下为 `RichWidget`）。
- 绑定的 `actions`、访问次数、hash 等缓存字段。

状态的 hash 形式可以概括为：

```text
stateHash = fastStringHash(activityName) XOR combineHash(widgets, withOrder)
```

其中 `combineHash` 会遍历 widget 列表，利用每个 widget 的 hash 叠加得到整体 hash。  
**只要 widget 抽象一致，stateHash 就一致**，RL 会把它们视为同一状态。

### 1.3 Action：在 state 上执行的“操作” a

**Action** 是 RL 里的动作 \(a\)，对应“在当前状态上执行一次操作”。主要种类：

- `ActionType`：`CLICK`、`LONG_CLICK`、`SCROLL_TOP_DOWN`、`BACK`、`DEEP_LINK` 等。
- 每个 action 会绑定到一个 widget 或一个矩形区域（点击区域）。

在 C++ 实现中，对 RL 有意义的动作一般是：

- `ActivityStateAction` / `ActivityNameAction` 等子类；
- 每个 action 提供：
  - `hash()`：作为 Q 表和复用模型的 key；
  - `getPriority()`：动作优先级，用于加权随机选择；
  - `getVisitedCount()`：动作已被执行的次数；
  - `getQValue()` / `setQValue()`：Sarsa / DoubleSarsa 中的 Q(s,a)。

---

## 2. 抽象的总体目标

GUI 测试最大的挑战是：**原始状态空间巨大且噪声多**，任何轻微的文本或布局变化都会导致原始 dump 完全不同。

Fastbot 用一个抽象函数：

```text
f(raw GUI) -> stateHash
```

希望满足：

- 本质相同的界面映射到同一个 state（避免状态空间爆炸）。
- 又能区分在测试语义上重要的差异（新按钮、新入口、不同结果页等）。
- 能根据运行情况动态调节抽象粒度（动态抽象）。
- 能与旧 ReuseAgent 的行为保持兼容（静态抽象）。

因此实现了两套互不影响的方案：

1. **动态状态抽象（dynamic）**：在运行过程中 refine / coarsen；
2. **静态状态抽象（static reuse）**：用一套固定规则（RichWidget + 旧 hash），不做在线调整。

切换由 `max.staticStateAbstraction=true|false` 控制。

---

## 3. 动态状态抽象（Dynamic State Abstraction）

动态模式在如下条件下启用：

- `/sdcard/max.config` 中配置：

  ```properties
  max.staticStateAbstraction=false
  ```

### 3.1 WidgetKeyAttr 与默认 Mask

在 `Base.h` 中定义了 widget key 的位掩码：

```cpp
enum class WidgetKeyAttr : uint8_t {
    Clazz       = 1 << 0,
    ResourceID  = 1 << 1,
    OperateMask = 1 << 2,
    ScrollType  = 1 << 3,
    Text        = 1 << 4,
    ContentDesc = 1 << 5,
    Index       = 1 << 6,
};
using WidgetKeyMask = uint8_t;
```

默认 mask 为：

```cpp
constexpr WidgetKeyMask DefaultWidgetKeyMask =
    static_cast<WidgetKeyMask>(WidgetKeyAttr::Clazz)      |
    static_cast<WidgetKeyMask>(WidgetKeyAttr::ResourceID) |
    static_cast<WidgetKeyMask>(WidgetKeyAttr::OperateMask)|
    static_cast<WidgetKeyMask>(WidgetKeyAttr::ScrollType);
```

含义：

- 初始只区分 **类名 + 资源 ID + 操作能力 + 滚动属性**；
- 不把 `Text` / `ContentDesc` / `Index` 纳入 hash，避免一开始就因为文本/顺序导致状态数激增。

### 3.2 动态 refine / coarsen 的触发条件

动态抽象的核心逻辑在 `Model.cpp::runRefinementAndCoarseningIfScheduled()` 中：

```cpp
void Model::runRefinementAndCoarseningIfScheduled() {
    if (Preference::inst() && Preference::inst()->useStaticReuseAbstraction()) {
        return; // 静态模式下直接关闭动态抽象
    }
    if (_stepCountSinceLastCheck < RefinementCheckInterval) return;

    // 1. 对需要 alpha refine 的 activity 进行 refine + coarsen
    // 2. 检测非确定性，根据结果对活动进行 refine + coarsen
}
```

大致流程：

1. **记录转移日志**：对每次 `(sourceState, action, targetState)` 保存记录。
2. **检测非确定性**：
   - 若同一 `(state, action)` 对应多个不同的 `targetState`，说明当前抽象太粗，需要 refine。
3. **ActionRefinement(α) + Coarsening(β)**：
   - 当某 activity 下的某些 state 分裂过多时，需要 coarsen；反之，在非确定性或过于粗糙时 refine。

### 3.3 refineActivity：如何“加细”抽象

`refineActivity(activity)` 会对某个 activity 的 widget key mask 做增量调整，典型策略是：

- 先使用 `Clazz | ResourceID | OperateMask | ScrollType`。
- 若发现：
  - 同一抽象下包含过多 widget；
  - 或同一 `(state, action)` 导致多目标 state（非确定性）；
  则逐步给该 activity 的 mask **加入**：
  - `Text`：区分文本不同的 widget；
  - `ContentDesc`：区分有不同 contentDescription 的控件；
  - `Index`：在某些场景按视图顺序区分同类控件。

mask 一旦增加，后续该 activity 的 widget hash / state hash 都会变得更细，旧的状态会自然“冻结”在历史 graph 中，新数据会落在更精细的新 state 上。

### 3.4 coarsenActivityIfNeeded：如何“变粗”抽象

与 refine 相反，当某 activity 的 state 数量或复杂度过高时，`coarsenActivityIfNeeded(activity)` 会尝试把 mask 简化，例如：

- 去掉 `Text` / `ContentDesc` / `Index` 中一些对测试价值不大的字段；
- 通过合并来降低状态数，防止状态空间爆炸。

这类似于在抽象空间中做「正则化」，避免过拟合到某些非常细微但不重要的差异。

### 3.5 动态模式下的 state hash

对某个 activity A：

1. 查询该 activity 当前的 `WidgetKeyMask maskA`。
2. 对每个 widget：
   - 仅提取 maskA 指定的属性构造 key，例如：

     ```text
     key(widget) = concat(
       [clazz?], [resourceId?], [operateMask?], [scrollType?],
       [text?], [contentDesc?], [index?]
     )
     ```

   - 传入 `fastStringHash(key)` 得到 widgetHash。
3. 再用 `combineHash(widgets, withOrder)` 把整个页面的 widgetHash 序列合成一个整体 hash。
4. 与 `fastStringHash(activityName)` XOR 得到最终 `stateHash`。

这种模式下，不同 activity 可以拥有**不同的 mask**，从而针对复杂/简单页面分别调整抽象粒度。

---

## 4. 静态状态抽象（Static Reuse Abstraction）

静态模式在如下条件下启用：

```properties
max.staticStateAbstraction=true
```

此时：

- `Preference::useStaticReuseAbstraction()` 返回 true；
- `Model::runRefinementAndCoarseningIfScheduled()` 直接 return，不再进行任何 refine/coarsen；
- 状态抽象完全由 `reuse` 模块中的 `RichWidget` / `ReuseState` 实现，刻意对齐旧 ReuseAgent。

### 4.1 RichWidget：更贴近旧 ReuseAgent 的 widget 抽象

在静态模式下，每个原始 widget 不再使用 `WidgetKeyMask` 系统，而是被转换为一个 `RichWidget`，大致规则如下：

- 参与 hash 的字段：
  - class 名；
  - resource-id；
  - 支持的 action 类型（click/scroll/input 等）；
  - 处理后的“有效文本”（包括自身文本 + 子节点文本）。
- 文本处理遵守旧 Reuse 的一些约束：
  - 过滤无意义或噪声文本；
  - 对某些场景使用「clickable-children masking」来保持 hash 稳定。

最终，`RichWidget` 的 hash 类似：

```text
widgetHash = fastStringHash(
  clazz + "|" + resourceId + "|" + supportedActions + "|" + effectiveText
)
```

这正是 README 中提到的：

> `(class + resource-id + supported actions + valid text/children text, with clickable-children masking)`

### 4.2 静态模式下的 state hash 与模型文件

静态模式下的状态 hash 仍然形如：

```text
stateHash = fastStringHash(activityName) XOR combineHash(RichWidgets, withOrder)
```

但：  
**RichWidget 的构造规则是固定的**，不会因运行时的 refine/coarsen 而改变，因此：

- 相同页面在静态抽象下的 stateHash 是完全稳定的；
- 便于长期收集和重用复用模型。

对应的模型文件也与动态模式分离：

- 动态模式：`/sdcard/fastbot_{pkg}.fbm` / `.tmp.fbm`
- 静态模式：`/sdcard/fastbot_{pkg}.static.fbm` / `.static.tmp.fbm`

Agent（Sarsa / DoubleSarsa）根据 `useStaticReuseAbstraction()` 选择加载哪一个文件。

### 4.3 配置与日志确认

- 配置：

  ```properties
  max.staticStateAbstraction=true   # 静态抽象
  # max.staticStateAbstraction=false  # 动态抽象（默认）
  ```

- 解析时日志（Preference.cpp）：

  ```text
  state abstraction: static (legacy static reuse state abstraction enabled)
  ```

- Agent 加载模型时的日志（例如 SarsaAgent）：

  ```text
  SarsaAgent: begin load model: /sdcard/fastbot_com.xxx.static.fbm
  SarsaAgent: loaded model contains N actions
  ```

---

## 5. 抽象方式对强化学习算法的影响

### 5.1 对状态空间与复用模型的影响

- **动态抽象**：
  - 优点：可根据非确定性、状态数量等自动 refine/coarsen，适应复杂应用；
  - 缺点：state hash 会随 mask 调整而变化，老的 `.fbm` 在新的 mask 体系下不再一一对应；
  - 对 Sarsa/DoubleSarsa：Q(s,a) 和 `_reuseModel` 的 key 所在空间会随时间「缓慢漂移」，更强调在线学习，而不太适合直接复用老模型。

- **静态抽象**：
  - 优点：hash 定义完全固定，与旧 ReuseAgent 对齐，复用历史模型方便；  
  - 缺点：无法自动根据 app 特性变化调整粒度，抽象质量依赖这套固定规则本身是否足够好；
  - 对 Sarsa/DoubleSarsa：Q(s,a) 和 `_reuseModel` 始终在同一状态空间中演化，便于长期收敛和跨版本/跨任务迁移。

### 5.2 SarsaAgent / DoubleSarsaAgent 如何使用抽象结果

SarsaAgent / DoubleSarsaAgent 对“抽象是动态还是静态”本身是透明的，它们只依赖：

- `stateHash`：由 `Graph` 管理 state 节点；
- `Action::hash()`：基于 widget/state hash + 动作类型得到 actionHash；
- `_reuseModel[actionHash][activity] = visitCount`：复用模型统计。

一旦抽象方式确定，Agent 只在给定的 `(state, action)` 空间里做：

- reward 计算（基于 visitedActivities + `_reuseModel`）；
- N-step SARSA 或 Double SARSA 的 Q 值更新；
- 按「未在模型 / 在模型 / 未访问 / Q 值 / epsilon-greedy」的优先级选择 action。

换句话说：

- **抽象 = 定义坐标系**；
- **Agent = 在这个坐标系上跑 RL 算法**。

动态抽象提供了一个可自适应的坐标系，静态抽象提供了一个与老系统完全兼容、稳定的坐标系。

---

## 6. 总结

- **Widget**：从 GUI 树中抽出的基本元素，承载结构、语义和可交互信息。
- **State**：Activity + widget 集合，通过 hash 定义 RL 状态。
- **Action**：在某个 state 上执行的一次操作，作为 Q(s,a) 和复用模型的 key。

- **动态抽象**：
  - 基于 `WidgetKeyMask`，在运行时通过非确定性检测和阈值策略自动 refine/coarsen；
  - 适合探索新应用、自动寻找合适抽象粒度。

- **静态抽象**：
  - 使用 `RichWidget` + 旧 ReuseAgent 的 hash 定义，不做在线调整；
  - 由 `max.staticStateAbstraction=true|false` 控制；
  - 用单独的 `.static.fbm` 文件存储模型，便于跨版本、跨测试任务复用。

在实践中，可以：

- 对需要完全对齐历史数据 / 论文实验的场景使用**静态抽象**；
- 对新 app 或结构变化频繁的 app，先用**动态抽象**探索合适的粒度，再视情况切到静态抽象固化策略。

