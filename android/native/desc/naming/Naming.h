/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef FASTBOTX_NAMING_H_
#define FASTBOTX_NAMING_H_

#include "../../Base.h"
#include "Namelet.h"
#include "Name.h"
#include <vector>

namespace fastbotx {

    // A Naming is a composition of one or more Namelets that together define
    // how widgets in a GUI tree are partitioned into equivalence classes.
    // It forms a refinement tree where parents are coarser partitions and
    // children are more fine‑grained.
    class Naming : public HashNode, public std::enable_shared_from_this<Naming> {
    public:
        struct NamingResult {
            // Names assigned to each accepted widget.
            NamePtrVec names;
            // Widgets that were successfully named (same order as names).
            WidgetPtrVec nodes;
        };

        // Construct a Naming from a sequence of namelets. The order matters
        // for resolution and for computing fineness.
        explicit Naming(const std::vector<NameletPtr> &namelets);

        // Name the given widgets using this Naming. Widgets that do not match
        // any namelet or fail filtering are skipped.
        NamingResult namingWidgets(const WidgetPtrVec &widgets, const RectPtr &rootBounds) const;

        // Resolve which namelet should be applied to a specific widget under
        // this Naming. Returns nullptr if none matches.
        NameletPtr resolveNamelet(const WidgetPtr &widget) const;

        // Extend this naming with a new namelet that refines `parent`.
        NamingPtr extend(const NameletPtr &parent, const NameletPtr &namelet) const;
        // Replace the last namelet (if it matches `replaced`) with `namelet`.
        NamingPtr replaceLast(const NameletPtr &replaced, const NameletPtr &namelet) const;

        // True if the given namelet is the last component and can be replaced.
        bool isReplaceable(const NameletPtr &namelet) const;

        // Last namelet in the chain, or nullptr if empty.
        NameletPtr getLastNamelet() const;
        NamingPtr getParent() const { return _parent.lock(); }

        // Returns true if this Naming is an ancestor in the refinement tree
        // of `other` (i.e. reachable by following parent links).
        bool isAncestor(const NamingPtr &other) const;

        // Fineness metric: maximum size of Namer type sets among namelets.
        // Higher fineness usually means more distinct states.
        int getFineness() const;

        // HashNode interface: combines hashes of all namelets.
        uintptr_t hash() const override;

    private:
        NameletPtr selectNamelet(const std::vector<NameletPtr> &matches) const;

        std::vector<NameletPtr> _namelets;
        std::weak_ptr<Naming> _parent;
    };

    typedef std::shared_ptr<Naming> NamingPtr;

} // namespace fastbotx

#endif // FASTBOTX_NAMING_H_
