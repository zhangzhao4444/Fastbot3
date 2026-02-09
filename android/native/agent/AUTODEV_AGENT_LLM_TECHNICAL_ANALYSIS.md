/**
 * @authors Zhao Zhang
 */

# Fastbot3 AutodevAgent 基于 LLM 的 task 自动驱动技术

---

## 1. 概述

**AutodevAgent** 是一个基于大语言模型（LLM）的 GUI 测试智能体，在满足“检查点”条件时接管动作选择，执行预定义任务（如登录流程、特定业务步骤），并将 LLM 输出转换为与现有 Fastbot 一致的 `ActionPtr`，由 Model 转成 `Operate` 在 Java 层执行。Agent 本身不直接执行任何操作，只负责“选动作”。

核心特性：

- **检查点驱动**：通过外部配置文件（如 `/sdcard/max.llm.tasks`）按 Activity + XPath 检查点匹配任务，匹配成功则创建/进入会话。
- **可选 Planner/Executor 分层**：Planner 输出语义步骤，Executor 根据当前 UI 执行具体动作。
- **单 LLM 双角色**：同一 LLM 依次担任 Planner 与 Executor（若启用 Planner 层），每次请求为单轮调用。
- **会话与安全**：会话有步数、时长、连续失败上限；支持 safe_mode 与 forbidden_texts 避免误点敏感控件。

---

## 2. 整体架构

```
                    +------------------+
                    |   Java / 应用层   |
                    |  AiClient JNI    |
                    +--------+---------+
                             |
         getActionFromBuffer(activity, buffer)
         → getActionFromBufferNativeStructured(activity, buffer, byteLength)
                             |
                             v
    +------------------------------------------------------------------------+
    |                         Native (C++)                                    |
    |  +-------------------+     +------------------+     +----------------+  |
    |  | fastbot_native.cpp| --> | Model            | --> | AutodevAgent   |  |
    |  | parseTreeFromBuffer     | getOperateOpt()  |     | selectNextAction  |
    |  +-------------------+     +------------------+     +--------+--------+ |
    |                              |         ^                     |          |
    |                              |         | llmAction           |          |
    |                              v         |                     v          |
    |  +------------------+   selectAction   |   +--------------------------------+
    |  | StateFactory     |   (RL / custom)  |   | LlmClient (e.g. HttpLlmClient) |
    |  | Graph / Agent    |                  |   | predict(prompt, images, out)   |
    |  +------------------+                  |   | 无 libcurl 时经 JNI 回调 Java    |
    |                                        |   | 发 HTTP，图在 Java 按需截屏       |
    |  convertActionToOperate(action, state) |   +----------------+---------------+
    |                                        |                    |
    |  +------------------+                  |                    | HTTP
    |  | Operate          | <----------------+                    v
    |  | (CLICK/BACK/...) |                  |   +--------------------------------+
    |  +------------------+                  |   |  OpenAI-compatible API         |
    |                                        |   |  /chat/completions             |
    +-----------------------------------------------------------------------------+   
                             |
                             v
                    OperateResult / Operate --> 返回 Java 执行
```

**数据流简要**：

1. Java 传入当前 Activity、UI 树（Buffer）；**不传截图**（图在 C++ 回调 Java 发 LLM 请求时由 Java 按需截屏）。JNI 调用 `Model::getOperateOpt(elem, activity, deviceID, &requestScreenshotRetry)`。
2. Model 先建状态、再问 **AutodevAgent** 是否在本步给出动作：`selectNextAction(element, activity, deviceID, preMatchedLlmTask)`（无 screenshot 参数）。
3. 若 AutodevAgent 返回非空 `ActionPtr`，本步用该动作，跳过 RL；否则走原有 `selectAction(state, agent, ...)`。
4. 最终 `convertActionToOperate(action, state)` 得到 `Operate`，经 JNI 转成 `OperateResult` 返回 Java 执行。

**AutodevAgent 内部分工**：内部维护 `_preference`、`_llmClient`、`_session`（LlmSessionState）。`selectNextAction` 依次：判断是否在会话、是否过期 → 可选 Planner 出一步子任务 → Executor 根据当前 UI 出动作 → 解析 JSON → `convertToAction` → 记录历史并返回。详细分支与条件见**第 3 节**，算法细节见**第 4 节**。

---

## 3. 程序流程图

### 3.1 主流程：selectNextAction

```
                    +--------+
                    |  START |
                    +---+----+
                        |
                        v
              +---------------------+
              | _llmClient == null? |--Yes--> return nullptr
              +----------+----------+
                         | No
                         v
              +---------------------+
              | inSession()?        |--No--> maybeStartSession()
              +----------+----------+           |
                         | No/Yes               v
                         |            matchLlmTask(activity, rootXml)
                         |                     |
                         |            +--------+--------+
                         |            | cfg != null?    |--No--> return nullptr
                         |            +--------+--------+
                         |                     | Yes
                         |                     v
                         |            创建 _session (LlmSessionState)
                         v
              +---------------------+
              | isSessionExpired()? |--Yes--> abort; resetSession(); return nullptr
              +----------+----------+
                         | No
                         v
        +--------------------------------+
        | usePlannerLayer &&             |
        | currentPlannerStep.tool empty? |
        +--------+-----------------------+
                 | Yes
                 v
        buildPlannerPrompt() --> predict() --> parsePlannerResponse()
                 |
                 +-- finish_task? --> completed; resetSession(); return nullptr
                 +-- 否则保存 currentPlannerStep
                 v
              buildPrompt() --> 可选注入 EXECUTOR SUB-TASK
                         |
                         v
              _llmClient->predict(prompt, images, rawResponse)
                         |
                    +----+----+
                    | ok?     |--No--> 连续失败+1; Planner 步失败+1; 可能 resetSession; return nullptr
                    +----+----+
                         | Yes
                         v
              parseLlmResponseFromJson(); todo/scratchpad 更新
                         |
                         v
              task_status COMPLETED/ABORT? --> resetSession(); return nullptr
                         | No (ONGOING)
                         v
              convertToAction(spec, rootXml, activity)
                         |
                    +----+----+
                    | action? |--No--> 连续失败+1; 若 Forbidden 安全终止; return nullptr
                    +----+----+
                         | Yes
                         v
              记录 history/summaries; 清空 currentPlannerStep(若 Planner 模式)
                         |
                         v
                    return action
```

### 3.2 Executor 动作转换：convertToAction

将 `LlmActionSpec` 转为 `ActionPtr`：STATUS→NOP；WAIT→CustomAction(NOP+waitTime)；BACK→Action(BACK)；SCROLL→CustomAction(scrollType)；CLICK/INPUT→`findTargetElement`→safe_mode 检查（见**第 7 节**）→CustomAction。分支与 fallbacks 详见**4.4**。

---

## 4. 算法

### 4.1 任务匹配与会话启动（Checkpoint Match）

- **配置来源**：`Preference::loadLlmTasks()` 从 `/sdcard/max.llm.tasks` 读取 JSON 数组，每项为 `LlmTaskConfig`（activity、checkpoint_xpath、task_description、max_steps、max_duration_ms、safe_mode、forbidden_texts、use_planner、use_llm_step_summary 等）。
- **匹配逻辑**：`Preference::matchLlmTask(activity, rootXML)`  
  - 过滤：`cfg->activity` 非空则需与当前 activity 一致。  
  - 用 `cfg->checkpointXpath` 在当前 `rootXML` 上做 `findFirstMatchedElement`，匹配则加入候选。  
  - 从候选中**随机**选一个返回（便于多任务配置时随机选一个执行）。
- **会话创建**：`maybeStartSession()` 在未入会话且匹配到配置时，创建 `LlmSessionState`，写入 taskConfig、activity、deviceId、步数/时间戳/完成与终止标志、todos、scratchpad、recentScreenHashes、currentPlannerStep 等。

### 4.2 Planner 层

**Planner 是可选的** 任务配置项 `use_planner`（`LlmTaskConfig::usePlannerLayer`）默认 **true**（Planner 会多一轮 LLM，可能造成过多延迟）。若为 false，每步只调用一次 LLM（Executor）：prompt 中直接给「任务描述 + 当前 UI 摘要 + 历史」，由模型输出下一动作，适合简单、步骤少的任务，且省一次调用、延迟更低。若为 true，则启用「Planner → Executor」两层：先由 Planner 输出一个语义子任务（如 “tap 登录按钮”），再由 Executor 根据当前 UI 执行，适合多步、需要拆解的长任务。**Planner 在 C++ 侧传入 noImages**；在无 libcurl、走 Java HTTP 路径时，Java 在每次 `doLlmHttpPostFromPrompt` 内按需截屏并写入 body，故 Planner 与 Executor 的请求都会带图（Java 不区分调用方，统一在发请求时截一次）。

- **作用**：将高层任务拆成**单步语义子任务**，避免 Executor 一次收到整段任务导致目标不清。
- **触发条件**：`taskConfig->usePlannerLayer == true` 且当前 `currentPlannerStep.tool` 为空（即需要“下一步子任务”时）。
- **Prompt**：`buildPlannerPrompt()` 只给任务描述、当前 todos、scratchpad 键、已执行步骤摘要，**不给完整 UI 树**，要求输出**一个**语义步骤。
- **工具集（语义）**：`tap`、`scroll`、`type_text`、`answer`、`finish_task`、`go_back`。输出格式为单 JSON：`tool`、`intent`、`text`（type_text/answer 用），并可带 `todo_updates`。
- **解析**：`parsePlannerResponse()` 得到 `PlannerStep { tool, intent, text }`。若 `tool == "finish_task"` 则标记任务完成并 `resetSession()`。
- **与 Executor 的衔接**：Planner 得到的一步会写入 `currentPlannerStep`。`buildPrompt()` 中若存在 `currentPlannerStep`，会在 Executor 的 prompt 前插入 “EXECUTOR SUB-TASK” 段落（如 “Locate and tap on the …”、“Perform a scroll to …”、“Locate … then type: …”），使 Executor 只专注完成这一步。本步执行成功后清空 `currentPlannerStep`，下一轮再向 Planner 要下一步。

### 4.3 Executor 层（动作决策）

- **输入**：当前 Activity、由 `getScreenFingerprint(rootXml)` 得到的可交互元素轻量列表（index、class、resource-id、text、content-desc）、任务描述、近期步骤摘要、todos、scratchpad、以及（若启用 Planner）当前子任务描述。可选：当前屏 hash 用于防循环提示。
- **Prompt 设计**：  
  - 说明角色与 JSON 输出格式。  
  - 要求 `task_status` 为 ONGOING / COMPLETED / ABORT。  
  - 支持两种输出：`action` 对象（action_type、target、text、reason）或 `tool_calls`（click、input_text、scroll、back、wait、status）。  
  - 元素定位以 **INDEX** 为主（与 getScreenFingerprint 的遍历顺序一致），便于稳定解析。
- **解析**：`parseLlmResponseFromJson()` 统一为 `LlmActionSpec`（taskStatus、actionType、targetBy、targetValue、text、direction、reason、fallbacks）。同时从同一 JSON 中解析 `todo_updates`、`scratchpad_updates` 并写回 session。

### 4.4 动作转换与执行语义（convertToAction）

```
    LlmActionSpec (task_status, action_type, target, text, direction, ...)
                         |
         +---------------+---------------+---------------+---------------+
         v               v               v               v
      STATUS          WAIT            BACK           SCROLL
         |               |               |               |
         v               v               v               v
    Action(NOP)    CustomAction     Action(BACK)   CustomAction(scrollType)
                    (NOP+waitTime)                   bounds from target or viewport
         |               |               |               |
         +---------------+---------------+---------------+---------------+
         


         +---------------+---------------+
         v               v
      CLICK           INPUT
         |               |
         v               v
    findTargetElement(by, value, rootXml)  [fallbacks 可选]
         |
         +-- not found --> return nullptr (NotFound)
         |
         v
    safe_mode? 且 target 含 forbidden_texts --> return nullptr (Forbidden)，见第 7 节
         |
         v
    CustomAction(CLICK/INPUT) + bounds + text(若 INPUT)
```

- **STATUS**：仅更新任务状态，不执行 UI 操作，转为 `Action(NOP)`。
- **WAIT**：转为带 `waitTime` 的 `CustomAction(NOP)`（解析 text 中的 duration_ms，上限 10s）。
- **BACK**：转为 `Action(BACK)`。
- **SCROLL**：根据 direction 映射为 SCROLL_TOP_DOWN / SCROLL_BOTTOM_UP / SCROLL_LEFT_RIGHT / SCROLL_RIGHT_LEFT；若有 target 则用其 bounds，否则由 Java 层按默认视口滚动。
- **CLICK / INPUT**：  
  - 用 `findTargetElement(by, value, rootXml)` 解析目标，支持 **INDEX / TEXT / XPATH**；若主 selector 失败则尝试 `fallbacks`。**INDEX** 与 buildPrompt 中列表一致（可点击/长按/可滚元素 DFS 编号）。  
  - 若启用 `safe_mode`，会检查目标 text/content-desc 是否包含 `forbiddenTexts`，命中则返回 nullptr 并安全终止（**详见第 7 节**）。  
  - 最终用目标 bounds 构造 `CustomAction(CLICK)` 或带 `text` 的 INPUT。

### 4.5 会话生命周期与安全限制

- **过期条件**（`isSessionExpired()`）：  
  - `max_steps > 0` 且 `stepCount >= max_steps`；  
  - `max_duration_ms > 0` 且已超时；  
  - 连续失败次数 `consecutiveFailures >= kMaxConsecutiveFailures`（如 3）。  
  任一满足则设置 `abortReason` 并视为过期。
- **连续失败**：LLM 调用失败、解析失败、convertToAction 失败（非 Forbidden）都会增加 `consecutiveFailures`；成功执行一步后清零。若启用 Planner，当前 Planner 步多次失败（如 `plannerStepFailureCount >= kMaxPlannerStepFailures`）会清空 `currentPlannerStep`，下一轮重新向 Planner 要步。
- **防循环**：`recentScreenHashes` 保存最近若干屏的 fingerprint hash；若当前屏已在其中，在 prompt 中加入“此屏最近已见过，请避免重复滚动或换策略”的提示。
- **安全终止**：当 convertToAction 因 Forbidden 失败时，设置 `aborted` 与 `abortReason = "safety_forbidden"` 并 `resetSession()`。

### 4.6 历史与摘要

- **history**：每步成功执行后追加 `StepHistoryEntry`（stepIndex、actionType、targetBy、targetValue、actionReason、actionOutputJson），用于调试与可选摘要，长度有上限（如 kMaxHistoryEntries）。
- **historySummaries**：短句摘要列表，注入到下一次 `buildPrompt()` 的 “Recent steps summary”。若 `useLlmForStepSummary` 为 true，会调用 `requestStepSummaryFromLlm(entry)` 用 LLM 生成一句摘要；否则用 `appendLocalSummary(spec)` 本地拼接。长度有上限（如 kMaxHistory）。

### 4.7 Todo 与 Scratchpad

- **Todo**：Session 内维护 `todos`（id、content、status、priority）。Planner 或 Executor 的 JSON 可带 `todo_updates` 数组，通过 `applyTodoUpdatesFromJson()` 按 id 合并或追加，并做数量上限（如 kMaxTodos）。
- **Scratchpad**：Session 内 `scratchpad` 为 key → { title, text }。JSON 中 `scratchpad_updates` 可写入/覆盖，供后续步骤引用（如“把某信息存到 scratchpad 再在下一步使用”）。

---

## 5. 关键常量（代码内）

| 常量 | 典型值 | 含义 |
|------|--------|------|
| kMaxHistory | 10 | historySummaries 最大条数 |
| kMaxHistoryEntries | 20 | history 最大条数 |
| kMaxScreenHashes | 15 | recentScreenHashes 最大条数 |
| kMaxConsecutiveFailures | 3 | 连续失败上限，超则会话过期 |
| kMaxTodos | 32 | todos 数量上限 |
| kMaxScratchpadItems | 32 | scratchpad 条数上限 |
| kMaxPlannerStepFailures | 2 | 当前 Planner 步失败次数上限，超则清空步并重新要步 |
| kMaxSummaryLen | 200 | 单条 LLM 摘要最大长度 |

---

## 6. 配置与调用链小结

- **LLM 开关与运行时**：`Preference::getLlmRuntimeConfig()`（max.llm.enabled、apiUrl、apiKey、model、maxTokens、timeoutMs 等）。Model 构造时若 enabled 则创建 `HttpLlmClient` 并传入 AutodevAgent。
- **任务配置**：路径与字段（activity、checkpoint_xpath、task_description、max_steps、safe_mode、forbidden_texts、use_planner 等）见 **4.1**。
- **调用链**：与**第 2 节**数据流一致；Java path 下完整 LLM 请求链路见 **8.1**。

---

## 7. safe_mode 与 forbidden_texts

用于在 AutodevAgent 执行 CLICK/INPUT 时**避免误点敏感控件**（如「注销」「删除账号」「确认退出」等），一旦命中则视为安全违规，**立即终止当前会话**并不再执行该步动作。

### 7.1 含义与配置

- **safe_mode**（`LlmTaskConfig::safeMode`）：是否启用本任务的「安全模式」。默认 **false**。在任务 JSON 里为 `"safe_mode": true/false`。
- **forbidden_texts**（`LlmTaskConfig::forbiddenTexts`）：不允许点击的**文案集合**。对目标元素的 `text` 或 `content-desc` 做**子串匹配**，命中任一即禁止点击。在任务 JSON 里为字符串数组，例如：
  ```json
  "forbidden_texts": ["注销", "删除", "退出登录", "Delete Account"]
  ```

配置来源：`Preference::loadLlmTasks()` 从 `/sdcard/max.llm.tasks` 读取每个任务的 `safe_mode`、`forbidden_texts`，写入 `LlmTaskConfig`（见 `Preference.cpp` 约 2086–2097 行）。

### 7.2 实现位置与逻辑

**触发时机**：仅在「把 LLM 输出转成具体动作」时做检查，即 **CLICK / INPUT** 在 `convertToAction()` 里已通过 `findTargetElement` 找到目标元素、且尚未根据 bounds 构造 `CustomAction` 之前。

**代码位置**：`AutodevAgent.cpp` 中 `convertToAction()`，约 665–680 行：

1. 若当前会话的 `taskConfig->safeMode` 为 false，则**不做**任何 forbidden 检查，直接继续用目标元素构造点击/输入动作。
2. 若 `safeMode == true`：
   - 取 `taskConfig->forbiddenTexts`（可为空）。
   - 取目标元素的 `targetElem->getText()` 和 `targetElem->getContentDesc()`。
   - 对 `forbiddenTexts` 中每个非空串 `f`，若 **`txt` 或 `desc` 包含 `f`**（`txt.find(f) != npos || desc.find(f) != npos`），则：
     - 打日志：`target element contains forbidden text '...' (safety abort)`；
     - 若调用方传入了 `outFailure`，则置 `*outFailure = ConvertFailureReason::Forbidden`；
     - **return nullptr**（不生成任何 Action）。

**上层对 Forbidden 的处理**：`selectNextAction()` 中在调用 `convertToAction(..., &convertFailure)` 后，若 `convertFailure == ConvertFailureReason::Forbidden`，则：

- 置 `_session->aborted = true`、`_session->abortReason = "safety_forbidden"`；
- 打日志并调用 `resetSession()`；
- 返回 `nullptr`（本步不执行任何动作，且会话结束）。

因此：**safe_mode + forbidden_texts 只在「已解析出目标元素」之后做一次子串检查；一旦命中即视为安全违规，终止会话并放弃本步点击/输入。**

### 7.3 小结

| 项目 | 说明 |
|------|------|
| 作用 | 避免点击文案或 content-desc 包含敏感关键词的控件 |
| 匹配方式 | 子串包含（text 或 content-desc 任一包含 forbidden 中任一项即命中） |
| 生效范围 | 仅 CLICK/INPUT 的目标元素；SCROLL/BACK/WAIT/STATUS 不检查 |
| 命中后 | 本步不执行动作，会话立即终止（abortReason = "safety_forbidden"） |
| 配置 | 每任务独立：`safe_mode` 默认 false；`forbidden_texts` 为字符串数组 |

---

## 8. LLM 请求与响应解析链路

**截图与传图策略**：图仅留在 Java，不经 JNI 传 C++；在 C++ 回调 Java 发 HTTP 时由 Java 按需截屏并写入 body，故每次 LLM 请求（含第一步）都带图。

### 8.1 整体链路（Java path，无 libcurl）

```
Java: getNextEvent
  → setLlmScreenshotProvider(this::captureScreenshotForLlmRequest)
  → getActionFromBuffer(activity, buffer)
    → JNI getActionFromBufferNativeStructured(activity, buffer, byteLength)
      → Model::getOperateOpt(elem, activity, "", &requestScreenshotRetry)
        → matchLlmTask(activity, element)  // raw tree, 得到 preMatchedLlmTask
        → resolvePage(activity, element)
        → AutodevAgent::selectNextAction(element, activity, deviceId, preMatchedLlmTask)
          → maybeStartSession(...)  // 若未在会话且 preMatchedLlmTask 非空则启动会话
          → [可选] Planner: buildPlannerPrompt() → _llmClient->predict(plannerPrompt, noImages, plannerResponse)
          → buildPrompt() → 构造 Executor prompt
          → images 恒为空；图由 Java 在 HTTP 回调时按需截屏
          → _llmClient->predict(prompt, images, rawResponse)
            → HttpLlmClient::predict(prompt, images, outResponse)
              #if !FASTBOTX_HAS_CURL:
              → llmHttpPostViaJavaWithPrompt(url, apiKey, prompt, model, maxTokens, &outResponse)
                → JNI CallObjectMethod(doLlmHttpPostFromPrompt)
                  → Java AiClient.doLlmHttpPostFromPrompt(url, apiKey, prompt, model, maxTokens)
                    → buildLlmRequestBody(): sLlmScreenshotProvider.captureForLlm() 按需截屏 → 拼 JSON body
                    → doLlmHttpPostBody(url, apiKey, body)  // HttpURLConnection POST
                    → return 完整 HTTP response body（OpenAI 格式）
              ← outResponse = 完整 response body
              → 解析 JSON：取 choices[0].message.content → outResponse 仅保留 content 字符串
          ← rawResponse = 仅 assistant message content（JSON 动作或 Planner JSON）
          → parsePlannerResponse(plannerResponse, step) 或 parseLlmResponseFromJson / parseLlmResponse(rawResponse, spec)
          → convertToAction(spec, rootXml, activity) → ActionPtr
        → Model::convertActionToOperate(action, state) → OperatePtr
      ← opt, requestScreenshotRetry
    ← OperateResult (act, pos, throttle, requestScreenshotRetry, ...)
  ← Operate
```

### 8.2 要点补充

- **入口与图**：`getOperateOpt` / `selectNextAction` 均无 screenshot 入参；C++ 侧 `images` 恒为空，图在 Java 回调 `doLlmHttpPostFromPrompt` 时由 `buildLlmRequestBody` 内 `captureForLlm()` 按需截屏并写入 body（见 8.1）。
- **API 响应**：Java 返回完整 HTTP body；C++ 取 `choices[0].message.content` 作为 `outResponse`，Planner/Executor 收到的即为该 content 字符串。Planner/Executor 的 JSON 格式与解析见 **4.2、4.3**；`convertToAction` 与 `convertActionToOperate` 见 **4.4**。JNI 产出 `OperateResult`，当前 `requestScreenshotRetry` 恒为 false。
- **容错**：`parsePlannerResponse` / `parseLlmResponse` 对「被 markdown 包裹的 JSON」做容错：若直接 `json::parse` 失败，用 `extractJsonObjectString` 取首 `{` 到末 `}` 再解析，支持 `` ```json\n{...}\n``` `` 等形式。

