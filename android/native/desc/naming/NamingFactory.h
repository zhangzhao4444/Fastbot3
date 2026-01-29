/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef FASTBOTX_NAMING_FACTORY_H_
#define FASTBOTX_NAMING_FACTORY_H_

#include "../../Base.h"
#include "Naming.h"
#include "../Action.h"
#include "../State.h"
#include "Graph.h"
#include "Predicate.h"

namespace fastbotx {

    // NamingFactory encapsulates the policy for creating and evolving
    // Naming instances as exploration proceeds. It is responsible for
    // refining/abstracting naming granularity based on observed states,
    // nondeterminism, and user actions.
    class NamingFactory {
    public:
        NamingFactory();

        NamingPtr getBaseNaming() const { return _base; }
        NamingPtr getTopNaming() const { return _top; }
        NamingPtr getBottomNaming() const { return _bottom; }

        // Refine the current naming when an action is aliased (maps to too
        // many candidate widgets). Adds predicates to preserve divergence.
        NamingPtr refineForAliasedAction(const NamingPtr &current, const StatePtr &state);
        // Abstract the naming if it becomes too fine for the given number
        // of states (used to prevent state explosion).
        NamingPtr abstractIfTooFine(const NamingPtr &current, size_t stateCount) const;
        // APE alignment: batchAbstract signature matches APE's implementation
        // Batch abstraction over a group of states that share the same
        // parent naming. Potentially replaces targetNaming by its parent.
        NamingPtr batchAbstract(const NamingPtr &targetNaming,
                                const StatePtr &initialState,
                                const NamingPtr &targetParentNaming,
                                const StatePtrSet &targetStates,
                                const GraphPtr &graph);
        // Refine naming to resolve nondeterministic transitions between states.
        NamingPtr refineForNonDeterminism(const NamingPtr &current,
                                          const StateTransitionPtr &st1,
                                          const StateTransitionPtr &st2,
                                          const GraphPtr &graph,
                                          const std::function<bool(const NamingPtr &)> &isLeaf);

        // Compute the maximum acceptable number of states for a given
        // naming fineness before further refinement/abstraction is needed.
        int getMaxStatesForRefinementThreshold(const NamingPtr &naming) const;

    private:
        bool checkPredicate(const NamingPtr &naming,
                            const std::vector<ElementPtr> &affected) const;

        void addPredicate(const PredicatePtr &predicate);

        void removeConflictingPredicates(const NamingPtr &naming,
                                         const std::vector<ElementPtr> &affected);

        // Mark a naming as invalid for the given GUI trees so it will not
        // be selected again during future refinement.
        void blacklistNaming(const std::vector<ElementPtr> &trees,
                             const NamingPtr &naming);

        // Check whether a naming is blacklisted for a specific tree.
        bool isBlacklisted(const ElementPtr &tree, const NamingPtr &naming) const;

        // Factory helpers for initial namings with coarse/fine granularity.
        NamingPtr createBaseNaming() const;
        NamingPtr createTopNaming() const;
        NamingPtr createBottomNaming() const;

        // Build an attribute filter snapshot from a widget; used for
        // constructing refined Namelets that focus on a specific target.
        Namelet::AttributeFilter buildFilterFromWidget(const WidgetPtr &widget) const;

        NamingPtr _base;
        NamingPtr _top;
        NamingPtr _bottom;

        std::vector<PredicatePtr> _predicates;
        std::map<ElementPtr, std::set<NamingPtr>> _treeNamingBlacklist;
    };

} // namespace fastbotx

#endif // FASTBOTX_NAMING_FACTORY_H_
