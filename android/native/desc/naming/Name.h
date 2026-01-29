/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef FASTBOTX_NAME_H_
#define FASTBOTX_NAME_H_

#include "../../Base.h"
#include "Namer.h"
#include <string>
#include <vector>

namespace fastbotx {

    // Represents a canonical, hashable name for a widget under a specific
    // Namer configuration. The name is encoded as a normalized key string
    // over selected widget attributes and used for state keys and sets.
    class Name : public HashNode {
    public:
        // Construct a Name from a Namer and its attributes. The attributes
        // are copied and normalized into a unique key string which also
        // determines the hash value.
        Name(const NamerPtr &namer, std::vector<std::pair<NamerType, std::string>> attrs);

        // Convenience overload to allow construction from shared_ptr<const Namer>
        // (e.g., when calling shared_from_this() in const contexts).
        Name(const std::shared_ptr<const Namer> &namer,
             std::vector<std::pair<NamerType, std::string>> attrs);

        // HashNode interface: returns the precomputed hash of the normalized key.
        uintptr_t hash() const override { return _hash; }

        // Structural equality based on the normalized key string.
        bool operator==(const Name &other) const;

        // The Namer configuration that produced this Name.
        const NamerPtr &getNamer() const { return _namer; }

        // String representation used for hashing and equality.
        const std::string &toString() const { return _key; }

        // Returns true if this Name is at least as specific as `other`:
        //   - this.namer refines other.namer, and
        //   - this contains all attributes present in `other` with the same values.
        bool refinesTo(const std::shared_ptr<Name> &other) const;

    private:
        // Build the normalized key string and corresponding hash from
        // the current Namer and attribute map.
        void buildKey();

        NamerPtr _namer;
        // Compact attribute storage; entries are ordered according to the
        // Namer's type set so that key building is stable.
        std::vector<std::pair<NamerType, std::string>> _attrs;
        std::string _key;
        uintptr_t _hash{};
    };

    typedef std::shared_ptr<Name> NamePtr;
    typedef std::vector<NamePtr> NamePtrVec;

} // namespace fastbotx

#endif // FASTBOTX_NAME_H_
