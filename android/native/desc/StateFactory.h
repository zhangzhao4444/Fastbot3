/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef SateFactory_H_
#define SateFactory_H_

#include "State.h"
#include "../Base.h"
#include "Element.h"
#include "naming/Naming.h"

namespace fastbotx {

    /**
     * @brief Factory class for creating State objects
     * 
     * Provides a factory method to create State objects based on algorithm type.
     * Currently creates ReuseState instances, but can be extended to create
     * different state types based on algorithm requirements.
     */
    class StateFactory {
    public:
        /**
         * @brief Create a State object based on algorithm type
         * 
         * Creates an appropriate State subclass based on the algorithm type.
         * Currently all algorithms use ReuseState, but this can be extended.
         * 
         * @param agentT Algorithm type (currently unused, reserved for future use)
         * @param activity Activity name string pointer
         * @param element Root Element of the UI hierarchy
         * @return Shared pointer to created State
         */
        static StatePtr
        createState(AlgorithmType agentT, const stringPtr &activity, const ElementPtr &element,
                    const NamingPtr &naming);
    };
}
#endif /* SateFactory_H_ */
