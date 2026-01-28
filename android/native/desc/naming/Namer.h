/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef FASTBOTX_NAMER_H_
#define FASTBOTX_NAMER_H_

#include "../../Base.h"
#include "../Widget.h"
#include <set>
#include <map>
#include <memory>
#include <vector>

namespace fastbotx {

    // Attributes that can participate in naming a widget. Different
    // combinations correspond to different "granularity" levels.
    enum class NamerType {
        TYPE = 0,
        RESOURCE_ID,
        TEXT,
        CONTENT_DESC,
        INDEX,
        PARENT
    };

    class Namer;
    typedef std::shared_ptr<Namer> NamerPtr;

    class Name;
    typedef std::shared_ptr<Name> NamePtr;

    // A Namer describes how to build a Name from a widget by selecting
    // a subset of attributes (NamerType). It can be partially ordered by
    // refinement: one Namer refines another if it uses a superset of types.
    class Namer : public HashNode, public std::enable_shared_from_this<Namer> {
    public:
        // Create a Namer that uses the given set of attribute types.
        explicit Namer(const std::set<NamerType> &types);

        const std::set<NamerType> &getNamerTypes() const { return _types; }

        // Returns true if this Namer is at least as specific as `other`,
        // i.e. its type set is a superset of other's type set.
        bool refinesTo(const NamerPtr &other) const;

        // HashNode interface: hash over the bitmask of enabled types.
        uintptr_t hash() const override;

        // Build a Name instance for a widget according to this Namer's types.
        NamePtr naming(const WidgetPtr &widget) const;

    private:
        std::set<NamerType> _types;
    };

    // Factory and cache for Namer instances built from type sets. Ensures
    // reuse of Namer objects and provides utilities for computing refinements.
    class NamerFactory {
    public:
        // Get (or create) a Namer for the given type set. Instances are
        // cached by bitmask so that identical configurations are shared.
        static NamerPtr getNamer(const std::set<NamerType> &types);
        // The coarsest Namer: uses no attributes (all widgets share a Name).
        static NamerPtr topNamer();
        // The finest Namer: uses all attributes in usedTypes().
        static NamerPtr bottomNamer();
        // Return all Namers strictly above `current` in the refinement order,
        // sorted by increasing number of types.
        static std::vector<NamerPtr> getSortedAbove(const NamerPtr &current);

        // All attribute types that can be used in this environment.
        static const std::vector<NamerType> &usedTypes();

    private:
        static uint32_t toMask(const std::set<NamerType> &types);
        static void collectSupersets(std::vector<NamerPtr> &out,
                                     const std::vector<NamerType> &types,
                                     size_t index,
                                     std::set<NamerType> &current,
                                     const std::set<NamerType> &base);
    };

} // namespace fastbotx

#endif // FASTBOTX_NAMER_H_
