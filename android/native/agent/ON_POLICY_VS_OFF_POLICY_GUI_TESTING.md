/**
 * @authors Zhao Zhang
 */

# On-Policy vs Off-Policy 在GUI测试中的含义

## 执行摘要

本文档详细解释在GUI自动化测试场景下，**on-policy**和**off-policy**强化学习算法的区别、含义和实际影响。

**当前实现**：默认使用 **DoubleSarsaAgent**（N-step Double SARSA），属于 **off-policy** 算法。文档中部分示例代码曾针对已移除的 on-policy 实现（ModelReusableAgent），仅作概念参考。

---

## 一、核心概念

### 1.1 什么是"策略"（Policy）？

在GUI测试场景中，**策略**就是"如何选择动作的规则"：

```cpp
// 当前实现中的策略（简化版）
策略 = {
    // 优先级1: 选择未执行的新动作（不在reuse model中）
    if (有未执行的新动作) return 选择新动作;
    
    // 优先级2: 选择能到达未访问Activity的动作
    if (有能到达新Activity的动作) return 选择该动作;
    
    // 优先级3: 使用epsilon-greedy
    if (random() < epsilon) {
        return 随机选择动作;  // 探索
    } else {
        return 选择Q值最高的动作;  // 利用
    }
}
```

**策略决定了**：
- 什么时候探索（尝试新动作）
- 什么时候利用（使用已知好的动作）
- 如何平衡探索和利用

---

## 二、On-Policy vs Off-Policy 的核心区别

### 2.1 On-Policy（当前实现：SARSA）

**定义**：**学习当前执行的策略**

**关键特征**：
- 更新Q值时，使用**实际执行的动作**
- 学习的是"当前测试策略"（包含探索）
- 策略和执行策略是**同一个**

**在GUI测试中的含义**：

```cpp
// 当前实现（SARSA - On-Policy）
void updateQValues() {
    // 假设当前状态是：登录页面
    // 实际执行的动作：随机点击了一个按钮（探索）
    ActionPtr executedAction = _previousActions[i];  // 实际执行的动作
    
    // 下一个状态：进入了设置页面
    StatePtr nextState = _newState;
    
    // 下一个动作：实际选择的动作（可能是探索，也可能是利用）
    ActionPtr nextAction = _newAction;  // 实际选择的动作
    
    // 更新Q值时，使用实际选择的nextAction
    double target = reward + gamma * Q(nextState, nextAction);  // ← 使用实际动作
    Q(currentState, executedAction) += alpha * (target - Q(currentState, executedAction));
}
```

**实际场景示例**：

```
时间步1: 在登录页面
  - 策略选择: 随机点击"忘记密码"按钮（探索，epsilon=0.05）
  - 实际执行: 点击"忘记密码"
  - 结果: 进入忘记密码页面

时间步2: 在忘记密码页面
  - 策略选择: 选择Q值最高的"返回"按钮（利用）
  - 实际执行: 点击"返回"
  - 结果: 返回登录页面

更新Q值:
  - Q(登录页面, 点击忘记密码) 使用 Q(忘记密码页面, 点击返回)
  - 学习的是"当前策略"：在忘记密码页面会选择返回
```

**特点**：
- ✅ 学习的是**实际测试行为**
- ✅ 更保守，考虑探索的风险
- ❌ 如果探索导致进入死胡同，Q值会反映这个风险
- ❌ 可能学习到"次优策略"（因为包含探索）

---

### 2.2 Off-Policy（Q-Learning）

**定义**：**学习最优策略，但可以使用任意策略收集经验**

**关键特征**：
- 更新Q值时，使用**最优动作**（Q值最高的动作）
- 学习的是"最优测试策略"（不包含探索）
- 策略和执行策略可以**不同**

**在GUI测试中的含义**：

```cpp
// Off-Policy实现（Q-Learning）
void updateQValues() {
    // 假设当前状态是：登录页面
    // 实际执行的动作：随机点击了一个按钮（探索）
    ActionPtr executedAction = _previousActions[i];  // 实际执行的动作
    
    // 下一个状态：进入了设置页面
    StatePtr nextState = _newState;
    
    // 最优动作：Q值最高的动作（即使实际没有选择它）
    ActionPtr bestAction = argmax_a Q(nextState, a);  // ← 使用最优动作
    
    // 更新Q值时，使用最优动作
    double target = reward + gamma * Q(nextState, bestAction);  // ← 使用最优动作
    Q(currentState, executedAction) += alpha * (target - Q(currentState, executedAction));
}
```

**实际场景示例**：

```
时间步1: 在登录页面
  - 策略选择: 随机点击"忘记密码"按钮（探索）
  - 实际执行: 点击"忘记密码"
  - 结果: 进入忘记密码页面

时间步2: 在忘记密码页面
  - 最优动作: Q值最高的是"返回"按钮
  - 但实际可能选择: 随机点击其他按钮（探索）

更新Q值:
  - Q(登录页面, 点击忘记密码) 使用 max Q(忘记密码页面, 所有动作)
  - 学习的是"最优策略"：假设在忘记密码页面会选择最优动作
  - 即使实际选择了其他动作，也假设选择了最优动作
```

**特点**：
- ✅ 学习的是**最优测试策略**
- ✅ 更激进，假设总是选择最优动作
- ✅ 可以重用历史经验（经验回放）
- ❌ 可能高估Q值（因为假设总是选择最优）
- ❌ 在探索时可能不稳定

---

## 三、在GUI测试场景下的具体区别

### 3.1 动作选择阶段（相同）

无论是on-policy还是off-policy，**动作选择阶段都是相同的**：

```cpp
// 当前实现的动作选择（on-policy和off-policy都使用这个）
ActionPtr selectNewAction() {
    // 1. 优先选择未执行的新动作（探索）
    if (有未执行的新动作) return 选择新动作;
    
    // 2. 选择能到达未访问Activity的动作
    if (有能到达新Activity的动作) return 选择该动作;
    
    // 3. 使用epsilon-greedy
    if (random() < epsilon) {
        return 随机选择;  // 探索
    } else {
        return 选择Q值最高的动作;  // 利用
    }
}
```

**关键点**：动作选择阶段，on-policy和off-policy**没有区别**！

---

### 3.2 Q值更新阶段（不同）

**这是关键区别所在**：

#### On-Policy (SARSA) - 当前实现

```cpp
// 当前实现：使用实际选择的动作
void updateQValues() {
    // _newAction是实际选择的动作（可能是探索，也可能是利用）
    double value = getQValue(_newAction);  // ← 使用实际动作
    
    // 更新时，使用实际动作的Q值
    value = currentReward + gamma * value;  // value来自实际动作
    Q(state, action) += alpha * (value - Q(state, action));
}
```

**含义**：
- 如果实际选择了探索动作（随机选择），Q值更新会反映这个探索动作的价值
- 如果探索导致进入死胡同，Q值会降低，反映这个风险
- 学习的是"在当前策略下，这个动作的价值"

#### Off-Policy (Q-Learning)

```cpp
// Off-Policy：使用最优动作
void updateQValues() {
    // 找到最优动作（Q值最高的动作）
    ActionPtr bestAction = findBestAction(_newState);  // ← 使用最优动作
    double value = getQValue(bestAction);  // ← 使用最优动作
    
    // 更新时，使用最优动作的Q值
    value = currentReward + gamma * value;  // value来自最优动作
    Q(state, action) += alpha * (value - Q(state, action));
}
```

**含义**：
- 即使实际选择了探索动作，更新时也假设选择了最优动作
- 学习的是"在最优策略下，这个动作的价值"
- 可以探索，但学习的是最优策略

---

## 四、实际影响对比

### 4.1 场景1：探索导致进入死胡同

**场景**：
- 当前状态：主页面
- 实际选择：随机点击了一个按钮（探索）
- 结果：进入了一个没有出口的页面（死胡同）

#### On-Policy (SARSA)

```
更新Q值:
  Q(主页面, 随机按钮) = reward + gamma * Q(死胡同页面, 实际选择的动作)
  
如果实际选择的动作也是随机的（探索），Q值会反映：
  - 进入死胡同的风险
  - 从死胡同可能无法到达有价值的地方
  
结果: Q值会降低，反映探索的风险
```

**影响**：
- ✅ 更保守，避免重复进入死胡同
- ✅ 学习到探索的风险
- ❌ 可能过于保守，错过一些有价值的路径

#### Off-Policy (Q-Learning)

```
更新Q值:
  Q(主页面, 随机按钮) = reward + gamma * max Q(死胡同页面, 所有动作)
  
即使实际选择了随机动作，也假设选择了最优动作：
  - 假设在死胡同页面会选择最优动作（可能是返回）
  - Q值可能不会降低太多
  
结果: Q值可能不会充分反映探索的风险
```

**影响**：
- ✅ 更激进，可能发现更多路径
- ❌ 可能高估Q值，重复进入死胡同
- ❌ 需要更多探索才能发现死胡同

---

### 4.2 场景2：探索发现新Activity

**场景**：
- 当前状态：主页面
- 实际选择：随机点击了一个按钮（探索）
- 结果：发现了一个新的Activity（有价值）

#### On-Policy (SARSA)

```
更新Q值:
  Q(主页面, 随机按钮) = reward + gamma * Q(新Activity页面, 实际选择的动作)
  
如果实际选择的动作是探索（随机），Q值会反映：
  - 从新Activity页面可能继续探索
  - 探索的价值可能被低估（因为下一步也是探索）
  
结果: Q值可能不会充分反映发现新Activity的价值
```

**影响**：
- ❌ 可能低估探索的价值
- ❌ 如果探索后继续探索，Q值可能不高

#### Off-Policy (Q-Learning)

```
更新Q值:
  Q(主页面, 随机按钮) = reward + gamma * max Q(新Activity页面, 所有动作)
  
假设在新Activity页面会选择最优动作：
  - 假设会选择能到达更多新Activity的动作
  - Q值会充分反映发现新Activity的价值
  
结果: Q值会充分反映探索的价值
```

**影响**：
- ✅ 更充分地学习探索的价值
- ✅ 鼓励探索新路径
- ✅ 更快发现应用的功能

---

### 4.3 场景3：经验重用

**场景**：
- 收集了大量历史经验
- 想要重用这些经验来学习

#### On-Policy (SARSA)

```
问题: 历史经验是用旧策略收集的
- 如果策略改变了，旧经验可能不适用
- 不能直接重用历史经验

结果: 需要在线学习，不能重用历史经验
```

**影响**：
- ❌ 不能重用历史经验
- ❌ 每次测试都需要重新学习
- ❌ 样本效率较低

#### Off-Policy (Q-Learning)

```
优势: 可以重用历史经验
- 历史经验是用旧策略收集的
- 但学习的是最优策略，不依赖收集策略
- 可以使用经验回放（Experience Replay）

结果: 可以重用历史经验，提高样本效率
```

**影响**：
- ✅ 可以重用历史经验
- ✅ 可以使用经验回放（Prioritized Experience Replay）
- ✅ 样本效率更高
- ✅ 可以离线学习（从历史数据学习）

---

## 五、在GUI测试中的优缺点对比

### 5.1 On-Policy (SARSA) - 当前实现

#### 优点 ✅

1. **更保守，更安全**
   - 考虑探索的风险
   - 避免重复进入死胡同
   - 适合需要稳定测试的场景

2. **学习实际测试行为**
   - Q值反映实际测试策略
   - 更符合实际测试场景
   - 适合需要学习"测试策略"而非"最优策略"的场景

3. **实现简单**
   - 只需使用实际选择的动作
   - 不需要找最优动作
   - 代码更简单

4. **更稳定**
   - 不会高估Q值
   - 学习过程更稳定
   - 适合实时学习

#### 缺点 ❌

1. **可能过于保守**
   - 低估探索的价值
   - 可能错过一些有价值的路径
   - 探索效率可能较低

2. **不能重用历史经验**
   - 每次测试都需要重新学习
   - 样本效率较低
   - 不能离线学习

3. **学习次优策略**
   - 学习的是包含探索的策略
   - 不是最优策略
   - 可能不够高效

---

### 5.2 Off-Policy (Q-Learning)

#### 优点 ✅

1. **学习最优策略**
   - 学习最优测试策略
   - 不考虑探索的风险
   - 更高效

2. **可以重用历史经验**
   - 可以使用经验回放
   - 提高样本效率
   - 可以离线学习

3. **更充分地学习探索价值**
   - 鼓励探索新路径
   - 更快发现应用功能
   - 探索效率更高

4. **可以结合Prioritized Experience Replay**
   - 优先学习重要经验
   - 更快收敛
   - 样本效率极高

#### 缺点 ❌

1. **可能高估Q值**
   - 假设总是选择最优动作
   - 可能高估动作价值
   - 需要Double Q-Learning来缓解

2. **可能不稳定**
   - 在探索时可能不稳定
   - 需要更多调参
   - 可能重复进入死胡同

3. **实现稍复杂**
   - 需要找最优动作
   - 需要处理off-policy偏差
   - 代码稍复杂

---

## 六、在GUI测试场景下的选择建议

### 6.1 选择On-Policy (SARSA) 的场景

**适合场景**：
1. ✅ **需要稳定测试**
   - 不希望重复进入死胡同
   - 需要可预测的测试行为
   - 适合生产环境

2. ✅ **学习实际测试策略**
   - 需要学习"如何测试"而非"最优测试"
   - 测试策略包含探索
   - 适合需要人类可理解的测试策略

3. ✅ **实时学习**
   - 每次测试都重新学习
   - 不需要重用历史经验
   - 适合快速迭代

4. ✅ **简单实现**
   - 资源有限
   - 需要快速实现
   - 适合原型开发

**当前实现就是这种情况** ✅

---

### 6.2 选择Off-Policy (Q-Learning) 的场景

**适合场景**：
1. ✅ **需要高效探索**
   - 需要快速发现应用功能
   - 探索效率很重要
   - 适合大型应用测试

2. ✅ **可以重用历史经验**
   - 有大量历史测试数据
   - 可以使用经验回放
   - 适合长期测试项目

3. ✅ **需要学习最优策略**
   - 需要最优测试路径
   - 不考虑探索风险
   - 适合性能优化

4. ✅ **可以离线学习**
   - 可以从历史数据学习
   - 不需要实时学习
   - 适合批量处理

---

## 七、代码示例对比

### 7.1 当前实现（On-Policy SARSA）

```cpp
// ModelReusableAgent.cpp - updateQValues()
void ModelReusableAgent::updateQValues() {
    // 使用实际选择的动作（_newAction）
    double value = getQValue(_newAction);  // ← On-Policy: 使用实际动作
    
    // 从后往前更新
    for (int i = windowSize - 1; i >= 0; i--) {
        value = currentReward + DefaultGamma * value;  // value来自实际动作
        Q(_previousActions[i]) += alpha * (value - Q(_previousActions[i]));
    }
}
```

**关键点**：`_newAction`是实际选择的动作（可能是探索，也可能是利用）

---

### 7.2 Off-Policy实现（Q-Learning）

```cpp
// 假设的Off-Policy实现
void ModelReusableAgent::updateQValues() {
    // 找到最优动作（Q值最高的动作）
    ActionPtr bestAction = findBestAction(_newState);  // ← Off-Policy: 使用最优动作
    double value = getQValue(bestAction);  // ← 使用最优动作
    
    // 从后往前更新
    for (int i = windowSize - 1; i >= 0; i--) {
        value = currentReward + DefaultGamma * value;  // value来自最优动作
        Q(_previousActions[i]) += alpha * (value - Q(_previousActions[i]));
    }
}

// 辅助函数：找到最优动作
ActionPtr ModelReusableAgent::findBestAction(const StatePtr& state) {
    ActionPtr bestAction = nullptr;
    double maxQ = -std::numeric_limits<double>::max();
    
    for (const auto& action : state->getActions()) {
        double qValue = getQValue(action);
        if (qValue > maxQ) {
            maxQ = qValue;
            bestAction = action;
        }
    }
    
    return bestAction;
}
```

**关键点**：`bestAction`是最优动作（Q值最高的动作），即使实际没有选择它

---

## 八、总结

### 8.1 核心区别

| 特性 | On-Policy (SARSA) | Off-Policy (Q-Learning) |
|------|-------------------|------------------------|
| **学习目标** | 当前执行的策略 | 最优策略 |
| **更新使用** | 实际选择的动作 | 最优动作（Q值最高） |
| **探索风险** | 考虑探索风险 | 不考虑探索风险 |
| **经验重用** | 不能重用 | 可以重用（经验回放） |
| **稳定性** | 更稳定 | 可能不稳定 |
| **探索效率** | 可能较低 | 更高 |
| **实现复杂度** | 简单 | 稍复杂 |

### 8.2 在GUI测试中的建议

**当前实现（On-Policy SARSA）适合**：
- ✅ 需要稳定、可预测的测试
- ✅ 实时学习，不需要重用历史经验
- ✅ 简单实现，快速迭代

**如果改为Off-Policy (Q-Learning)**：
- ✅ 可以重用历史经验（经验回放）
- ✅ 更高效的探索
- ✅ 可以离线学习
- ⚠️ 需要处理off-policy偏差
- ⚠️ 可能高估Q值（需要Double Q-Learning）

### 8.3 最终建议

**保持On-Policy (SARSA)**，除非：
1. 有大量历史测试数据可以重用
2. 需要更高效的探索
3. 可以接受更高的实现复杂度

**如果改为Off-Policy**，建议同时实现：
- Double Q-Learning（减少过估计）
- Prioritized Experience Replay（提高样本效率）

---
