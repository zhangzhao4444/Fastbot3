/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef FASTBOTX_STATEKEY_H_
#define FASTBOTX_STATEKEY_H_

#include "../../Base.h"
#include "Naming.h"
#include "Name.h"

namespace fastbotx {

    // StateKey is a canonical identifier for a GUI state. It combines:
    //   - the current activity string
    //   - the Naming configuration
    //   - the multiset of widget Names in that state
    // and exposes a stable hash and ordering for map/set usage.
    class StateKey : public HashNode {
    public:
        // Construct a StateKey from activity, naming and the Names of widgets
        // in the state. The widgets vector will be sorted and hashed.
        StateKey(std::string activity, NamingPtr naming, NamePtrVec widgets);

        // HashNode interface: precomputed hash of (activity, naming, widgets).
        uintptr_t hash() const override { return _hash; }

        // Structural equality based on all components, not only hash.
        bool operator==(const StateKey &other) const;

        // Strict weak ordering for map/set; consistent with equality above.
        bool operator<(const StateKey &other) const;

        const NamingPtr &getNaming() const { return _naming; }
        const NamePtrVec &getWidgets() const { return _widgets; }
        const std::string &getActivity() const { return _activity; }

        // Heuristic: whether the state is considered "trivial" based on
        // number of widgets and actions, used to control exploration effort.
        bool isTrivialState() const;

        // Compact printable representation for logging and debugging.
        std::string toString() const;

    private:
        // Compute the combined hash after normalizing (sorting) widgets.
        void buildHash();

        std::string _activity;
        NamingPtr _naming;
        NamePtrVec _widgets;
        uintptr_t _hash{};
    };

    typedef std::shared_ptr<StateKey> StateKeyPtr;

} // namespace fastbotx

#endif // FASTBOTX_STATEKEY_H_
