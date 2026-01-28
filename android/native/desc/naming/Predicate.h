/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef FASTBOTX_PREDICATE_H_
#define FASTBOTX_PREDICATE_H_

#include "../../Base.h"
#include "Naming.h"
#include "../Element.h"

namespace fastbotx {

    // A Predicate encodes an invariant over a Naming and a collection of
    // GUI trees. It is used by NamingFactory to prevent regressions when
    // refining or abstracting naming schemes.
    class Predicate {
    public:
        // Evaluate the predicate under the given naming and set of trees.
        // When trees is empty, the predicate may fall back to its internal
        // reference set captured at construction time.
        virtual bool eval(const NamingPtr &naming, const std::vector<ElementPtr> &trees) const = 0;
        // Optionally return a Naming that should be used after this predicate
        // is applied (e.g., refined/abstracted variant).
        virtual NamingPtr getUpdatedNaming() const = 0;
        virtual ~Predicate() = default;
    };

    typedef std::shared_ptr<Predicate> PredicatePtr;

    // Ensures that, under the given naming, the number of distinct states
    // reachable from a group of trees does not exceed a threshold.
    class AssertStatesFewerThan : public Predicate {
    public:
        AssertStatesFewerThan(NamingPtr naming, std::vector<ElementPtr> trees, int threshold);

        bool eval(const NamingPtr &naming, const std::vector<ElementPtr> &trees) const override;
        NamingPtr getUpdatedNaming() const override { return _naming; }

    private:
        NamingPtr _naming;
        std::vector<ElementPtr> _trees;
        int _threshold;
    };

    // Ensures that two disjoint groups of trees map to disjoint state keys
    // under the given naming (i.e. they remain distinguishable).
    class AssertSourceDivergent : public Predicate {
    public:
        AssertSourceDivergent(NamingPtr naming,
                              std::vector<ElementPtr> group1,
                              std::vector<ElementPtr> group2);

        bool eval(const NamingPtr &naming, const std::vector<ElementPtr> &trees) const override;
        NamingPtr getUpdatedNaming() const override { return _naming; }

    private:
        NamingPtr _naming;
        std::vector<ElementPtr> _group1;
        std::vector<ElementPtr> _group2;
    };

    // Ensures that a particular widget target (identified by hash) receives
    // different Names across a collection of trees, i.e. that action sites
    // are still distinguishable after refinement.
    class AssertActionDivergent : public Predicate {
    public:
        AssertActionDivergent(NamingPtr naming,
                              std::vector<ElementPtr> trees,
                              uintptr_t targetWidgetHash);

        bool eval(const NamingPtr &naming, const std::vector<ElementPtr> &trees) const override;
        NamingPtr getUpdatedNaming() const override { return _naming; }

    private:
        NamingPtr _naming;
        std::vector<ElementPtr> _trees;
        uintptr_t _targetWidgetHash;
    };

} // namespace fastbotx

#endif // FASTBOTX_PREDICATE_H_
