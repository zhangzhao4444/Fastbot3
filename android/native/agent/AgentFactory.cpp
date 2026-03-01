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
#include "SarsaAgent.h"
#include "DFSAgent.h"
#include "BFSAgent.h"
#include "FrontierAgent.h"
#include "GOExploreAgent.h"
#include "CuriosityAgent.h"
#include "../desc/StateEncoder.h"
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

        // For AlgorithmType::Curiosity, use curiosity-driven agent (WebRLED-style dual novelty).
        if (agentT == AlgorithmType::Curiosity) {
            CuriosityAgentPtr curiosityAgent = std::make_shared<CuriosityAgent>(model);
#if !defined(FASTBOTX_CURIOSITY_DISABLE_DNN_ENCODER)
            // Default: use DnnStateEncoder (16->16->8); setStateEncoder sets _clusterDim from encoder->getOutputDim().
            // To compare with handcrafted-only clustering, build with FASTBOTX_CURIOSITY_DISABLE_DNN_ENCODER defined
            // so that CuriosityAgent uses 16-dim HandcraftedStateEncoder directly for clustering.
            curiosityAgent->setStateEncoder(std::make_shared<DnnStateEncoder>());
            agent = curiosityAgent;
            BLOG("Created CuriosityAgent (curiosity-driven, WebRLED-style, DNN encoder)");
#else
            // Handcrafted-only clustering: 16-dim HandcraftedStateEncoder, no DNN encoder.
            agent = curiosityAgent;
            BLOG("Created CuriosityAgent (curiosity-driven, WebRLED-style, handcrafted embedding)");
#endif
            return agent;
        }

        // For AlgorithmType::GoExplore, use standalone Go-Explore style agent (archive + return + explore).
        if (agentT == AlgorithmType::GoExplore) {
            GOExploreAgentPtr goExploreAgent = std::make_shared<GOExploreAgent>(model);
            agent = goExploreAgent;
            BLOG("Created GOExploreAgent (standalone Go-Explore style)");
            return agent;
        }

        // LLMExplorer removed (effect not ideal); AlgorithmType::LLMExplorer falls through to DoubleSarsa.

        // For AlgorithmType::Sarsa, use legacy-compatible SarsaAgent (single-Q SARSA + reuse model).
        if (agentT == AlgorithmType::Sarsa) {
            SarsaAgentPtr sarsaAgent = std::make_shared<SarsaAgent>(model);
            threadDelayExec(3000, false, &SarsaAgent::threadModelStorage,
                            std::weak_ptr<fastbotx::SarsaAgent>(sarsaAgent));
            agent = sarsaAgent;
            BLOG("Created SarsaAgent (legacy single-Q SARSA with reuse model)");
            return agent;
        }

        // Default / DoubleSarsa: use DoubleSarsaAgent with periodic model saving.
        DoubleSarsaAgentPtr doubleSarsaAgent = std::make_shared<DoubleSarsaAgent>(model);
        threadDelayExec(3000, false, &DoubleSarsaAgent::threadModelStorage,
                        std::weak_ptr<fastbotx::DoubleSarsaAgent>(doubleSarsaAgent));

        agent = doubleSarsaAgent;
        BLOG("Created DoubleSarsaAgent");

        return agent;
    }

}

#endif
