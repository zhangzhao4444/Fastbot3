/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */

#ifndef Agent_Factory_CPP_
#define Agent_Factory_CPP_

#include "AgentFactory.h"
#include "utils.hpp"
#include "Model.h"
#include "DoubleSarsaAgent.h"
#include "DFSAgent.h"
#include "BFSAgent.h"
#include "FrontierAgent.h"
#include "GOExploreAgent.h"
#include "ICMAgent.h"
#include "json.hpp"
#include "Preference.h"

namespace fastbotx {

    /**
     * @brief Create Agent instance
     * 
     * Creates corresponding Agent instance based on algorithm type (agentT).
     * 
     * Supported algorithm types:
     * - All types: Creates DoubleSarsaAgent (Double SARSA)
     * 
     * Creation flow:
     * 1. Create Agent instance based on algorithm type
     * 2. Start background thread to periodically save model (starts after 3 second delay, saves every 10 minutes)
     * 3. Return Agent pointer
     * 
     * @param agentT Algorithm type, determines which Agent to create
     * @param model Model pointer, Agent needs access to model to get state graph info
     * @param deviceType Device type (currently unused, reserved interface)
     * @return Created Agent smart pointer
     * 
     * @note 
     * - Uses weak_ptr passed to background thread to avoid circular references
     * - Background thread automatically exits when Agent is destructed (weak_ptr becomes invalid)
     * - Model save thread starts after 3 second delay to avoid frequent saves during initialization
     */
    AbstractAgentPtr
    AgentFactory::create(AlgorithmType agentT, const ModelPtr &model, DeviceType /*deviceType*/) {
        AbstractAgentPtr agent = nullptr;

        // For AlgorithmType::DFS, use a simple DFS-based exploration agent.
        if (agentT == AlgorithmType::DFS) {
            DFSAgentPtr dfsAgent = std::make_shared<DFSAgent>(model);
            agent = dfsAgent;
            BLOG("Created DFSAgent (depth-first exploration)");
            return agent;
        }

        // For AlgorithmType::BFS, use BFS-based exploration agent (layer-by-layer).
        if (agentT == AlgorithmType::BFS) {
            BFSAgentPtr bfsAgent = std::make_shared<BFSAgent>(model);
            agent = bfsAgent;
            BLOG("Created BFSAgent (breadth-first exploration)");
            return agent;
        }

        // For AlgorithmType::Frontier, use frontier-based exploration agent.
        if (agentT == AlgorithmType::Frontier) {
            FrontierAgentPtr frontierAgent = std::make_shared<FrontierAgent>(model);
            agent = frontierAgent;
            BLOG("Created FrontierAgent (frontier-based exploration)");
            return agent;
        }

        // For AlgorithmType::ICM, use curiosity-driven agent (WebRLED-style dual novelty).
        if (agentT == AlgorithmType::ICM) {
            ICMAgentPtr icmAgent = std::make_shared<ICMAgent>(model);
            agent = icmAgent;
            BLOG("Created ICMAgent (curiosity-driven, WebRLED-style)");
            return agent;
        }

        // For AlgorithmType::GoExplore, use standalone Go-Explore style agent (archive + return + explore).
        if (agentT == AlgorithmType::GoExplore) {
            GOExploreAgentPtr goExploreAgent = std::make_shared<GOExploreAgent>(model);
            agent = goExploreAgent;
            BLOG("Created GOExploreAgent (standalone Go-Explore style)");
            return agent;
        }

        // Default / Reuse / DoubleSarsa: use DoubleSarsaAgent with periodic model saving.
        DoubleSarsaAgentPtr doubleSarsaAgent = std::make_shared<DoubleSarsaAgent>(model);

        // Start background thread to periodically save model
        // Parameter explanation:
        // - 3000: Start after 3 second delay (avoid frequent saves during initialization)
        // - false: Non-blocking execution
        // - &DoubleSarsaAgent::threadModelStorage: Thread execution function
        // - weak_ptr: Use weak_ptr to avoid circular references, thread exits when Agent is destructed
        threadDelayExec(3000, false, &DoubleSarsaAgent::threadModelStorage,
                        std::weak_ptr<fastbotx::DoubleSarsaAgent>(doubleSarsaAgent));

        agent = doubleSarsaAgent;
        BLOG("Created DoubleSarsaAgent");

        return agent;
    }

}

#endif
