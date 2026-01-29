/**
 * @authors Zhao Zhang, Tianxiao Gu
 */
#ifndef FASTBOTX_NAME_CPP_
#define FASTBOTX_NAME_CPP_

#include "Name.h"

namespace fastbotx {

    Name::Name(const NamerPtr &namer, std::vector<std::pair<NamerType, std::string>> attrs)
            : _namer(namer), _attrs(std::move(attrs)) {
        buildKey();
    }

    Name::Name(const std::shared_ptr<const Namer> &namer,
               std::vector<std::pair<NamerType, std::string>> attrs)
            : _namer(std::const_pointer_cast<Namer>(namer)), _attrs(std::move(attrs)) {
        buildKey();
    }

    void Name::buildKey() {
        // Build key with a single std::string and reserved capacity to
        // reduce allocations compared to std::ostringstream.
        _key.clear();
        // Rough heuristic: each attribute contributes at least ~8 chars.
        _key.reserve(16 + _attrs.size() * 8);
        if (_namer) {
            _key.append("namer=");
            _key.append(std::to_string(_namer->hash()));
        }
        for (const auto &kv : _attrs) {
            _key.push_back('|');
            _key.append(std::to_string(static_cast<int>(kv.first)));
            _key.push_back('=');
            _key.append(kv.second);
        }
        _hash = std::hash<std::string>{}(_key);
    }

    bool Name::operator==(const Name &other) const {
        return _key == other._key;
    }

    bool Name::refinesTo(const std::shared_ptr<Name> &other) const {
        if (!other || !_namer || !other->_namer) {
            return false;
        }
        if (!_namer->refinesTo(other->_namer)) {
            return false;
        }
        // Ensure this name contains all attributes from `other` with
        // identical values. Attribute counts are small; linear search
        // keeps implementation simple and cache‑friendly.
        for (const auto &required : other->_attrs) {
            bool found = false;
            for (const auto &mine : _attrs) {
                if (mine.first == required.first && mine.second == required.second) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return false;
            }
        }
        return true;
    }

} // namespace fastbotx

#endif // FASTBOTX_NAME_CPP_
