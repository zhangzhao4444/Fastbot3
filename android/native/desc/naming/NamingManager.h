/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef FASTBOTX_NAMING_MANAGER_H_
#define FASTBOTX_NAMING_MANAGER_H_

#include "../../Base.h"
#include "NamingFactory.h"
#include "../State.h"
#include "Graph.h"
#include "StateKey.h"

namespace fastbotx {

    // NamingManager tracks the current Naming in use and its evolution
    // over time. It also maintains per‑tree edges describing how Naming
    // transitions as states are refined or abstracted.
    class NamingManager {
    public:
        explicit NamingManager(std::shared_ptr<NamingFactory> factory);

        NamingPtr getNaming() const { return _currentNaming; }

        // Resolve the effective Naming for a given GUI tree and activity by
        // following the recorded refinement/abstraction edges from the base.
        NamingPtr getNamingForTree(const ElementPtr &tree, const stringPtr &activity) const;

        // Evolve the global naming based on a newly explored state and graph
        // statistics (aliased actions, state counts, etc.).
        NamingPtr evolveNaming(const StatePtr &state, const GraphPtr &graph);

        // Refine the naming to resolve nondeterminism between two transitions.
        NamingPtr resolveNonDeterminism(const NamingPtr &current,
                                        const StateTransitionPtr &st1,
                                        const StateTransitionPtr &st2,
                                        const GraphPtr &graph);

        // Update internal edges for the given trees when the naming changes
        // from oldOne to newOne (refinement, abstraction, or replacement).
        void updateNamingForTrees(const std::vector<ElementPtr> &trees,
                                  const NamingPtr &oldOne,
                                  const NamingPtr &newOne,
                                  const stringPtr &activity);

        // True if the given naming has no outgoing refinement edges.
        bool isLeaf(const NamingPtr &naming) const;

        int getVersion() const { return _version; }

        NamingPtr getBaseNaming() const { return _factory->getBaseNaming(); }
        NamingPtr getTopNaming() const { return _factory->getTopNaming(); }
        NamingPtr getBottomNaming() const { return _factory->getBottomNaming(); }

        NamingPtr refineForAliasedAction(const ActivityStateActionPtr &action);

        NamingPtr batchAbstract(const NamingPtr &targetNaming,
                               const StatePtr &initialState,
                               const NamingPtr &targetParentNaming,
                               const StatePtrSet &targetStates,
                               const GraphPtr &graph);

        void setCurrentNaming(const NamingPtr &naming) { _currentNaming = naming; }

    private:
        std::shared_ptr<NamingFactory> _factory;
        NamingPtr _currentNaming;
        // APE alignment: Use StateKeyPtr instead of uintptr_t hash to avoid collisions
        std::map<NamingPtr, std::map<StateKeyPtr, NamingPtr, Comparator<StateKey>>, Comparator<Naming>> _namingEdges;
        int _version{0};
    };

    typedef std::shared_ptr<NamingManager> NamingManagerPtr;

} // namespace fastbotx

#endif // FASTBOTX_NAMING_MANAGER_H_
