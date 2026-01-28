/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef FASTBOTX_NAMER_CPP_
#define FASTBOTX_NAMER_CPP_

#include "Namer.h"
#include "Name.h"
#include <algorithm>
#include <unordered_map>

namespace fastbotx {

    namespace {
        // Cache Namers by bitmask of enabled NamerType to avoid repeatedly
        // constructing identical instances and to keep pointer identity stable.
        std::unordered_map<uint32_t, std::weak_ptr<Namer>> g_namerCache;
        // Cache sorted refinement results for getSortedAbove().
        std::unordered_map<uint32_t, std::vector<NamerPtr>> g_sortedAboveCache;
        // Global list of all attribute types that can participate in naming.
        std::vector<NamerType> g_usedTypes = {
                NamerType::TYPE,
                NamerType::RESOURCE_ID,
                NamerType::TEXT,
                NamerType::CONTENT_DESC,
                NamerType::INDEX,
                NamerType::PARENT
        };
    }

    Namer::Namer(const std::set<NamerType> &types)
            : _types(types) {
    }

    bool Namer::refinesTo(const NamerPtr &other) const {
        if (!other) {
            return false;
        }
        const auto &base = other->getNamerTypes();
        return std::includes(_types.begin(), _types.end(), base.begin(), base.end());
    }

    uintptr_t Namer::hash() const {
        uint32_t mask = 0;
        for (auto t : _types) {
            mask |= (1U << static_cast<uint32_t>(t));
        }
        return std::hash<uint32_t>{}(mask);
    }

    NamePtr Namer::naming(const WidgetPtr &widget) const {
        if (!widget) {
            return nullptr;
        }
        std::vector<std::pair<NamerType, std::string>> attrs;
        attrs.reserve(_types.size());
        for (auto t : _types) {
            switch (t) {
                case NamerType::TYPE:
                    attrs.emplace_back(t, widget->getClassName());
                    break;
                case NamerType::RESOURCE_ID:
                    attrs.emplace_back(t, widget->getResourceID());
                    break;
                case NamerType::TEXT:
                    attrs.emplace_back(t, widget->getText());
                    break;
                case NamerType::CONTENT_DESC:
                    attrs.emplace_back(t, widget->getContentDesc());
                    break;
                case NamerType::INDEX:
                    attrs.emplace_back(t, std::to_string(widget->getIndex()));
                    break;
                case NamerType::PARENT: {
                    auto parent = widget->getParent();
                    if (parent) {
                        attrs.emplace_back(t, parent->getClassName() + "#" + parent->getResourceID());
                    } else {
                        attrs.emplace_back(t, "");
                    }
                    break;
                }
            }
        }
        return std::make_shared<Name>(shared_from_this(), std::move(attrs));
    }

    NamerPtr NamerFactory::getNamer(const std::set<NamerType> &types) {
        uint32_t mask = toMask(types);
        auto cached = g_namerCache.find(mask);
        if (cached != g_namerCache.end()) {
            if (auto locked = cached->second.lock()) {
                return locked;
            }
        }
        auto created = std::make_shared<Namer>(types);
        g_namerCache[mask] = created;
        return created;
    }

    NamerPtr NamerFactory::topNamer() {
        return getNamer({});
    }

    NamerPtr NamerFactory::bottomNamer() {
        return getNamer(std::set<NamerType>(g_usedTypes.begin(), g_usedTypes.end()));
    }

    const std::vector<NamerType> &NamerFactory::usedTypes() {
        return g_usedTypes;
    }

    uint32_t NamerFactory::toMask(const std::set<NamerType> &types) {
        uint32_t mask = 0;
        for (auto t : types) {
            mask |= (1U << static_cast<uint32_t>(t));
        }
        return mask;
    }

    void NamerFactory::collectSupersets(std::vector<NamerPtr> &out,
                                        const std::vector<NamerType> &types,
                                        size_t index,
                                        std::set<NamerType> &current,
                                        const std::set<NamerType> &base) {
        if (index == types.size()) {
            if (std::includes(current.begin(), current.end(), base.begin(), base.end())) {
                out.emplace_back(getNamer(current));
            }
            return;
        }
        // Skip this type
        collectSupersets(out, types, index + 1, current, base);
        // Include this type
        current.insert(types[index]);
        collectSupersets(out, types, index + 1, current, base);
        current.erase(types[index]);
    }

    std::vector<NamerPtr> NamerFactory::getSortedAbove(const NamerPtr &current) {
        if (!current) {
            return {};
        }
        const uint32_t baseMask = toMask(current->getNamerTypes());
        auto cached = g_sortedAboveCache.find(baseMask);
        if (cached != g_sortedAboveCache.end()) {
            return cached->second;
        }
        std::vector<NamerPtr> results;
        std::set<NamerType> base = current->getNamerTypes();
        std::set<NamerType> seed = base;
        collectSupersets(results, g_usedTypes, 0, seed, base);
        // Deduplicate by mask
        std::unordered_map<uint32_t, NamerPtr> unique;
        for (const auto &namer : results) {
            if (!namer) {
                continue;
            }
            unique[toMask(namer->getNamerTypes())] = namer;
        }
        results.clear();
        for (const auto &item : unique) {
            results.emplace_back(item.second);
        }
        std::sort(results.begin(), results.end(),
                  [](const NamerPtr &a, const NamerPtr &b) {
                      return a->getNamerTypes().size() < b->getNamerTypes().size();
                  });
        g_sortedAboveCache.emplace(baseMask, results);
        return results;
    }

} // namespace fastbotx

#endif // FASTBOTX_NAMER_CPP_
