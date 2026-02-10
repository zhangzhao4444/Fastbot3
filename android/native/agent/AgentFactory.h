/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */

#ifndef AgentFactory_H_
#define AgentFactory_H_

#include "AbstractAgent.h"
#include "Base.h"

namespace fastbotx {

    /**
     * @brief Device type enumeration
     * 
     * Used to distinguish different device types. Currently only Normal type is supported.
     * Reserved interface for future extension to support other device types.
     */
    enum DeviceType {
        Normal  ///< Normal device type
    };

    class Model;

    class AbstractAgent;

    typedef std::shared_ptr<Model> ModelPtr;
    typedef std::shared_ptr<AbstractAgent> AbstractAgentPtr;

    /**
     * @brief Factory class for creating different types of agents
     * 
     * Uses factory pattern to create different types of Agent instances.
     * 
     * Note: In current implementation, regardless of algorithm type (agentT) passed,
     * DoubleSarsaAgent instance is always created (Double SARSA reinforcement learning).
     * 
     * Advantages of factory pattern:
     * 1. Encapsulates object creation logic
     * 2. Unified creation interface
     * 3. Easy to extend with new agent types
     */
    class AgentFactory {
    public:

        /**
         * @brief Create Agent instance
         * 
         * Creates corresponding Agent instance based on algorithm type and device type.
         * 
         * Current implementation:
         * - Always creates DoubleSarsaAgent regardless of algorithm type
         * - After creating Agent, starts a background thread to periodically save model
         * 
         * @param agentT Algorithm type (currently unused, reserved interface)
         * @param model Model pointer, Agent needs access to model to get state graph info
         * @param deviceType Device type (currently unused, reserved interface, defaults to Normal)
         * @return Created Agent smart pointer
         * 
         * @note After creating Agent, model storage thread is automatically started,
         *       saving model every 10 minutes
         */
        static AbstractAgentPtr create(AlgorithmType agentT, const ModelPtr &model,
                                       DeviceType deviceType = DeviceType::Normal);
    };
}
#endif /* AgentFactory_H_ */
