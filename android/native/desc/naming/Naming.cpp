/*
 * Copyright 2020 Advanced Software Technologies Lab at ETH Zurich, Switzerland
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/**
 * @authors Tianxiao Gu, Zhao Zhang
 */
/**
 * Evaluates an ordered list of namelets against a `GUITree` DOM bridge: XPath matching, per-node namelet
 * selection, `Namer::naming`, grouping by semantic key, sorted output, and per-tree result caching.
 */

#include "Naming.h"
#include "BitmaskNamer.h"
#include "NamingRuntime.h"
#include "NamerType.h"
#include "../gui_tree/GUITree.h"
#include "../xpath/XPathNodeMapper.h"
#include "../../utils.hpp"

#include <algorithm>
#include <deque>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <string>

namespace fastbotx {
namespace naming {
namespace {
    /** Compact debug line for exception messages when naming fails on a widget node. */
    std::string describeNodeForNamingError(const gui_tree::GUITreeNode *n) {
        if (!n) {
            return "node=null";
        }
        std::ostringstream os;
        os << "class=" << n->getClassName()
           << ",res=" << n->getResourceId()
           << ",idx=" << n->getIndex()
           << ",bounds=" << n->getBounds().toString()
           << ",text=" << n->getText()
           << ",content-desc=" << n->getContentDesc();
        return os.str();
    }

    /** Serializes candidate namelets as `B:`/`R:` plus expression for diagnostics. */
    std::string describeNameletCandidates(const std::vector<NameletPtr> *candidates) {
        if (!candidates || candidates->empty()) {
            return "[]";
        }
        std::ostringstream os;
        os << "[";
        for (size_t i = 0; i < candidates->size(); ++i) {
            if (i > 0) {
                os << ";";
            }
            const NameletPtr &nl = (*candidates)[i];
            if (!nl) {
                os << "null";
                continue;
            }
            os << (nl->isBase() ? "B:" : "R:") << nl->getExprString();
        }
        os << "]";
        return os.str();
    }

    /** Writes the current DOM snapshot under `/sdcard/fastbot_naming_error_<id>.xml` when debugging failures. */
    std::string saveXmlOnError(const std::shared_ptr<gui_tree::XPathNodeMapper> &dom) {
        if (!dom) {
            return std::string();
        }
        const std::string xml = dom->dumpXmlString();
        if (xml.empty()) {
            return std::string();
        }
        static std::atomic<uint64_t> seq{0};
        const uint64_t id = ++seq;
        const std::string path = "/sdcard/fastbot_naming_error_" + std::to_string(id) + ".xml";
        std::ofstream ofs(path.c_str(), std::ios::out | std::ios::trunc);
        if (!ofs.is_open()) {
            return std::string();
        }
        ofs << xml;
        ofs.close();
        return path;
    }

    /** Population count of set bits in a namer mask (fineness heuristic). */
    int bitCount(uint32_t x) {
        return __builtin_popcount(x);
    }

    /** Builds the `v4|…` content key from ordered namelets, parent links, and namer dimension masks. */
    std::string computeFingerprintString(
        const std::vector<std::shared_ptr<Namelet>> &namelets) {
        auto appendHex32 = [](std::string &dst, uint32_t v) {
            static const char kHex[] = "0123456789abcdef";
            for (int shift = 28; shift >= 0; shift -= 4) {
                dst.push_back(kHex[(v >> shift) & 0xF]);
            }
        };
        auto parentIndex = [&](const std::shared_ptr<Namelet> &parent) -> int {
            if (!parent) {
                return -1;
            }
            for (size_t i = 0; i < namelets.size(); ++i) {
                if (namelets[i].get() == parent.get()) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        };
        // Order-sensitive fingerprint for naming identity and blacklist keys—do not sort namelets.
        // Printable `v4|…` format for stable logging and persistence.
        std::string out;
        out.reserve(namelets.size() * 48);
        out.append("v4");
        for (size_t idx = 0; idx < namelets.size(); ++idx) {
            const auto &nl = namelets[idx];
            if (!nl) {
                continue;
            }
            out.push_back('|');
            // Prefix with positional index to make the fingerprint order-sensitive and easy to diff.
            out.append(std::to_string(idx));
            out.push_back(':');
            out.push_back(nl->isBase() ? 'B' : 'R');
            out.push_back(':');
            out.append(nl->getExprString());
            out.push_back('#');
            appendHex32(out, nl->getNamer().typeDimensionMask());
            out.append("@d");
            out.append(std::to_string(nl->getDepth()));
            out.append("p");
            out.append(std::to_string(parentIndex(nl->getParent())));
        }
        return out;
    }

    /** Tie-break for namelet selection: depth, then expression string, then pointer address. */
    bool nameletSelectLess(const NameletPtr &a, const NameletPtr &b) {
        if (!a || !b) return a.get() < b.get();
        if (a->getDepth() != b->getDepth()) return a->getDepth() < b->getDepth();
        const int c = a->getExprString().compare(b->getExprString());
        if (c != 0) return c < 0;
        return a.get() < b.get();
    }

    /**
     * Picks the applicable namelet for one DOM node: single base only, or the deepest candidate whose
     * ancestor chain stays within the sorted candidate set.
     */
    NameletPtr selectNameletForNode(const std::vector<NameletPtr> &candidates) {
        if (candidates.empty()) {
            throw std::invalid_argument("Empty namelet candidates.");
        }
        if (candidates.size() == 1) {
            if (!candidates[0] || !candidates[0]->isBase()) {
                throw std::invalid_argument("Missing base namelet.");
            }
            return candidates[0];
        }
        std::vector<NameletPtr> sorted = candidates;
        std::sort(sorted.begin(), sorted.end(), nameletSelectLess);
        auto comparatorContains = [&](const NameletPtr &target) -> bool {
            auto it = std::lower_bound(sorted.begin(), sorted.end(), target, nameletSelectLess);
            if (it == sorted.end()) {
                return false;
            }
            return !nameletSelectLess(target, *it) && !nameletSelectLess(*it, target);
        };
        for (size_t i = sorted.size(); i > 0; --i) {
            NameletPtr cur = sorted[i - 1];
            NameletPtr p = cur ? cur->getParent() : nullptr;
            while (p) {
                if (!comparatorContains(p)) {
                    break;
                }
                p = p->getParent();
            }
            if (!p) {
                return cur;
            }
        }
        throw std::runtime_error("A node has no namelet.");
    }

    /** RAII: publishes `node → namer` for ancestor bitmask evaluation, cleared on scope exit. */
    struct NamingEvalGuard {
        explicit NamingEvalGuard(const std::unordered_map<const gui_tree::GUITreeNode *, const Namer *> *m) {
            namingEvalSetNodeToNamer(m);
        }
        ~NamingEvalGuard() { namingEvalClear(); }
        NamingEvalGuard(const NamingEvalGuard &) = delete;
        NamingEvalGuard &operator=(const NamingEvalGuard &) = delete;
    };

    /** True if every node pointer in `result.node_groups` belongs to the widget tree rooted at `tree`. */
    bool namingResultBelongsToTree(const Naming::NamingResult &result, gui_tree::GUITree &tree,
                                   size_t *foreignCount = nullptr) {
        std::unordered_set<gui_tree::GUITreeNode *> owned;
        std::deque<gui_tree::GUITreeNode *> q;
        if (tree.getRootNode()) {
            q.push_back(tree.getRootNode());
        }
        while (!q.empty()) {
            gui_tree::GUITreeNode *cur = q.front();
            q.pop_front();
            if (!cur || !owned.insert(cur).second) {
                continue;
            }
            for (const auto &ch : cur->getChildren()) {
                if (ch) {
                    q.push_back(ch.get());
                }
            }
        }

        size_t foreign = 0;
        for (const auto &group : result.node_groups) {
            for (const auto &n : group) {
                if (!n) {
                    continue;
                }
                if (owned.count(n.get()) == 0) {
                    ++foreign;
                }
            }
        }

        if (foreignCount) {
            *foreignCount = foreign;
        }
        return foreign == 0;
    }

}

    /** Monotonic id suffix for debug names (`Naming[…]`). */
    std::atomic<int> Naming::naming_counter_{0};

    /** Root naming: delegates to the parent-taking constructor with no parent link. */
    Naming::Naming(std::vector<std::shared_ptr<Namelet>> namelets)
        : Naming(nullptr, std::move(namelets)) {}

    /** Builds a child refinement node in the lattice with optional parent back-pointer. */
    std::shared_ptr<Naming> Naming::createChild(std::shared_ptr<Naming> parent,
                                                 std::vector<std::shared_ptr<Namelet>> namelets) {
        return std::shared_ptr<Naming>(new Naming(std::move(parent), std::move(namelets)));
    }

    /** Registers `child` reachable via refinement edge `from → to` namelets. */
    void Naming::addRefinementChild(const NamingEdge &edge, std::shared_ptr<Naming> child) {
        if (!child) {
            return;
        }
        children_[edge] = std::move(child);
    }

    /** Lookup of the refinement child along edge `(from, to)`, if registered. */
    std::shared_ptr<Naming> Naming::getRefinementChild(const std::shared_ptr<Namelet> &from,
                                                       const std::shared_ptr<Namelet> &to) const {
        if (!from || !to) {
            return nullptr;
        }
        NamingEdge key{from, to};
        auto it = children_.find(key);
        if (it == children_.end()) {
            return nullptr;
        }
        return it->second;
    }

    /**
     * Stores namelet chain, assigns `naming_name_`, computes `fineness_` from bitmask width or type count,
     * and caches `fingerprint_cached_`.
     */
    Naming::Naming(std::shared_ptr<Naming> parent, std::vector<std::shared_ptr<Namelet>> namelets)
        : parent_(parent),
          namelets_(std::move(namelets)) {
        naming_name_ = "Naming[" + std::to_string(naming_counter_.fetch_add(1, std::memory_order_relaxed)) + "]";
        fineness_ = -1;
        for (const auto &nl : namelets_) {
            if (!nl) continue;
            int f = 0;
            const auto *bn = dynamic_cast<const BitmaskNamer *>(nl->getNamerPtr().get());
            if (bn) {
                f = bitCount(bn->getMask());
            } else {
                f = static_cast<int>(nl->getNamerPtr()->getNamerTypes().size());
            }
            if (fineness_ < 0 || f > fineness_) {
                fineness_ = f;
            }
        }
        if (fineness_ < 0) {
            fineness_ = 0;
        }
        fingerprint_cached_ = computeFingerprintString(namelets_);
    }

    /** Total widget nodes across all parallel groups. */
    size_t Naming::NamingResult::getNodeSize() const {
        size_t s = 0;
        for (const auto &g : node_groups) {
            s += g.size();
        }
        return s;
    }

    /** Writes each group’s `Name` and selected `Namelet` onto the corresponding `GUITreeNode` fields. */
    void Naming::NamingResult::updateNames() {
        for (size_t i = 0; i < names.size(); ++i) {
            if (i >= node_groups.size()) break;
            for (size_t j = 0; j < node_groups[i].size(); ++j) {
                gui_tree::GUITreeNodePtr &node = node_groups[i][j];
                if (!node) continue;
                node->setXPathName(names[i]);
                if (i < namelet_groups.size() && j < namelet_groups[i].size()) {
                    node->setCurrentNamelet(namelet_groups[i][j]);
                }
            }
        }
    }

    /** Stable content fingerprint derived from the ordered namelet list (see `computeFingerprintString`). */
    const std::string &Naming::fingerprintString() const { return fingerprint_cached_; }

    /** Drops cached `NamingResult` for `tree` when the widget snapshot is rebuilt or invalidated. */
    void Naming::releaseTreeCache(const gui_tree::GUITree &tree) const {
        std::lock_guard<std::mutex> lk(naming_cache_mu_);
        tree_to_naming_result_.erase(tree.getId());
    }

    /** Final namelet in the refinement sequence (empty chain → null). */
    std::shared_ptr<Namelet> Naming::getLastNamelet() const {
        if (namelets_.empty()) {
            return nullptr;
        }
        return namelets_.back();
    }

    /** True when `namelet` is REFINE and matches the tail entry (eligible for in-place replacement). */
    bool Naming::isReplaceable(const std::shared_ptr<Namelet> &namelet) const {
        if (!namelet || namelets_.empty()) {
            return false;
        }
        if (!namelet->isRefine()) {
            return false;
        }
        return namelets_.back() == namelet;
    }

    /**
     * Core evaluation: optional memoized result per `tree` id; matches XPath per namelet; BFS assigns a
     * namelet and namer per node; groups nodes by `(namer key | name key)`; sorts groups by `Name::operator<`;
     * caches successful output. Throws with DOM dump paths when invariants break.
     */
    Naming::NamingResult Naming::namingInternal(
        gui_tree::GUITree &tree, const std::shared_ptr<gui_tree::XPathNodeMapper> &dom) const {
        static std::atomic<uint64_t> g_naming_internal_diag{0};
        const uint64_t diagSeq = ++g_naming_internal_diag;
#if defined(FASTBOT_NATIVE_VERBOSE_LOG) && FASTBOT_NATIVE_VERBOSE_LOG
        const bool shouldLog = (diagSeq <= 80 || (diagSeq % 400) == 0);
#else
        const bool shouldLog = false;
#endif
        const int treeId = tree.getId();
        {
            std::lock_guard<std::mutex> lk(naming_cache_mu_);
            auto itCached = tree_to_naming_result_.find(treeId);
            if (itCached != tree_to_naming_result_.end()) {
                size_t foreignCached = 0;
                if (namingResultBelongsToTree(itCached->second, tree, &foreignCached)) {
                    return itCached->second;
                }
                BDLOG("naming cache invalidated: foreign nodes in cached result namingFp=%s treeId=%d foreign=%zu",
                     fingerprint_cached_.c_str(), treeId, foreignCached);
                tree_to_naming_result_.erase(itCached);
            }
        }
        NamingResult out;
        if (!dom) {
            return out;
        }
        std::unordered_map<gui_tree::GUITreeNode *, std::vector<NameletPtr>> node_to_namelets;
        std::unordered_map<gui_tree::GUITreeNode *, gui_tree::GUITreeNodePtr> node_ref;
        node_to_namelets.reserve(256);
        node_ref.reserve(256);
        std::unordered_set<gui_tree::GUITreeNode *> treeOwnedNodes;
        std::deque<gui_tree::GUITreeNode *> ownedQ;
        if (tree.getRootNode()) {
            ownedQ.push_back(tree.getRootNode());
        }
        while (!ownedQ.empty()) {
            gui_tree::GUITreeNode *cur = ownedQ.front();
            ownedQ.pop_front();
            if (!cur || !treeOwnedNodes.insert(cur).second) {
                continue;
            }
            for (const auto &ch : cur->getChildren()) {
                if (ch) {
                    ownedQ.push_back(ch.get());
                }
            }
        }
        size_t foreignMatchedNodes = 0;
        std::ostringstream foreignMatchedSummary;

        for (const auto &nl : namelets_) {
            if (!nl) continue;
            std::vector<gui_tree::GUITreeNodePtr> matched = dom->nodesForXPath(nl->getExprString());
            for (const auto &nptr : matched) {
                if (!nptr) continue;
                gui_tree::GUITreeNode *raw = nptr.get();
                if (treeOwnedNodes.count(raw) == 0) {
                    if (foreignMatchedNodes < 3) {
                        if (foreignMatchedNodes != 0) {
                            foreignMatchedSummary << " | ";
                        }
                        foreignMatchedSummary << raw->getClassName() << ":" << raw->getResourceId()
                                              << " bounds=" << raw->getBounds().toString()
                                              << " expr=" << nl->getExprString();
                    }
                    ++foreignMatchedNodes;
                }
                node_ref.emplace(raw, nptr);
                node_to_namelets[raw].push_back(nl);
            }
        }
        if (shouldLog) {
            BDLOG("naming internal: matched seq=%llu namingFp=%s ownedNodes=%zu matchedNodes=%zu foreignMatched=%zu foreignSummary=%s",
                 static_cast<unsigned long long>(diagSeq), fingerprint_cached_.c_str(), treeOwnedNodes.size(),
                 node_ref.size(), foreignMatchedNodes,
                 foreignMatchedNodes ? foreignMatchedSummary.str().c_str() : "(none)");
        }

        std::vector<gui_tree::GUITreeNode *> bfs_nodes;
        bfs_nodes.reserve(256);
        std::deque<gui_tree::GUITreeNode *> q;
        if (tree.getRootNode()) q.push_back(tree.getRootNode());
        while (!q.empty()) {
            gui_tree::GUITreeNode *cur = q.front();
            q.pop_front();
            if (!cur) continue;
            bfs_nodes.push_back(cur);
            for (const auto &ch : cur->getChildren()) {
                if (ch) q.push_back(ch.get());
            }
        }

        std::unordered_map<const gui_tree::GUITreeNode *, const Namer *> node_to_namer;
        std::unordered_map<gui_tree::GUITreeNode *, NameletPtr> node_to_selected;
        node_to_namer.reserve(bfs_nodes.size());
        node_to_selected.reserve(bfs_nodes.size());
        for (gui_tree::GUITreeNode *n : bfs_nodes) {
            auto itNl = node_to_namelets.find(n);
            if (itNl == node_to_namelets.end()) {
                const std::string dumped = saveXmlOnError(dom);
                throw std::runtime_error(
                    "A node has no namelets. namingFp=" + fingerprint_cached_ + ";" +
                    describeNodeForNamingError(n) + "; bfsNodes=" + std::to_string(bfs_nodes.size()) +
                    (dumped.empty() ? std::string() : ("; xmlDump=" + dumped)));
            }
            NameletPtr sel = selectNameletForNode(itNl->second);
            node_to_selected[n] = sel;
            if (sel && sel->getNamerPtr()) {
                node_to_namer[n] = &sel->getNamer();
            }
        }
        if (shouldLog) {
            size_t foreignInNameletMap = 0;
            for (const auto &kv : node_to_namelets) {
                if (treeOwnedNodes.count(kv.first) == 0) {
                    ++foreignInNameletMap;
                }
            }
            BDLOG("naming internal: namelet-map seq=%llu bfsNodes=%zu mapNodes=%zu foreignMapNodes=%zu",
                 static_cast<unsigned long long>(diagSeq), bfs_nodes.size(), node_to_namelets.size(),
                 foreignInNameletMap);
        }
        NamingEvalGuard namingEvalGuard(&node_to_namer);

        struct Group {
            NamePtr name;
            std::vector<gui_tree::GUITreeNodePtr> nodes;
            std::vector<NameletPtr> namelets;
        };
        auto nameSemanticKey = [](const NamePtr &name) -> std::string {
            if (!name) {
                return std::string();
            }
            const NamerPtr nmr = name->getNamer();
            const std::string nk = nmr ? namerSemanticKey(*nmr) : std::string("0:");
            const std::string nv =
                name->cacheKeyString().empty() ? name->toXPath() : name->cacheKeyString();
            return nk + "|" + nv;
        };
        std::unordered_map<std::string, Group> groups;
        groups.reserve(bfs_nodes.empty() ? 64 : bfs_nodes.size());
        for (gui_tree::GUITreeNode *raw : bfs_nodes) {
            auto itNode = node_ref.find(raw);
            if (itNode == node_ref.end()) {
                const std::string dumped = saveXmlOnError(dom);
                throw std::runtime_error(
                    "GUITree node reference missing. namingFp=" + fingerprint_cached_ + ";" +
                    describeNodeForNamingError(raw) +
                    (dumped.empty() ? std::string() : ("; xmlDump=" + dumped)));
            }
            gui_tree::GUITreeNodePtr nptr = itNode->second;
            auto itSel = node_to_selected.find(raw);
            NameletPtr selected = (itSel != node_to_selected.end()) ? itSel->second : nullptr;
            if (!selected || !selected->getNamerPtr()) {
                std::string cands;
                auto itNl = node_to_namelets.find(raw);
                cands = describeNameletCandidates(itNl != node_to_namelets.end() ? &itNl->second : nullptr);
                const std::string dumped = saveXmlOnError(dom);
                throw std::runtime_error(
                    "A node has no namer. namingFp=" + fingerprint_cached_ + ";" +
                    describeNodeForNamingError(raw) + "; candidates=" + cands +
                    (dumped.empty() ? std::string() : ("; xmlDump=" + dumped)));
            }
            NamePtr name = selected->getNamer().naming(*nptr);
            if (!name) {
                const std::string dumped = saveXmlOnError(dom);
                throw std::runtime_error(
                    "A node has no name. namingFp=" + fingerprint_cached_ + ";" +
                    describeNodeForNamingError(raw) +
                    (dumped.empty() ? std::string() : ("; xmlDump=" + dumped)));
            }
            Group &g = groups[nameSemanticKey(name)];
            if (!g.name) g.name = name;
            g.nodes.push_back(nptr);
            g.namelets.push_back(selected);
        }
        std::vector<Group *> sorted_groups;
        sorted_groups.reserve(groups.size());
        for (auto &kv : groups) {
            sorted_groups.push_back(&kv.second);
        }
        std::sort(sorted_groups.begin(), sorted_groups.end(), [](const Group *ga, const Group *gb) {
            const NamePtr a = ga ? ga->name : nullptr;
            const NamePtr b = gb ? gb->name : nullptr;
            if (!a || !b) {
                return a.get() < b.get();
            }
            return *a < *b;
        });
        for (Group *g : sorted_groups) {
            if (!g || !g->name) {
                continue;
            }
            out.names.push_back(g->name);
            out.node_groups.push_back(std::move(g->nodes));
            out.namelet_groups.push_back(std::move(g->namelets));
        }
        if (shouldLog) {
            size_t foreignOut = 0;
            std::ostringstream foreignOutSummary;
            for (const auto &group : out.node_groups) {
                for (const auto &nptr : group) {
                    if (!nptr || treeOwnedNodes.count(nptr.get()) != 0) {
                        continue;
                    }
                    if (foreignOut < 3) {
                        if (foreignOut != 0) {
                            foreignOutSummary << " | ";
                        }
                        foreignOutSummary << nptr->getClassName() << ":" << nptr->getResourceId()
                                          << " bounds=" << nptr->getBounds().toString();
                    }
                    ++foreignOut;
                }
            }
            BDLOG("naming internal: output seq=%llu groups=%zu names=%zu foreignOut=%zu foreignSummary=%s",
                 static_cast<unsigned long long>(diagSeq), out.node_groups.size(), out.names.size(),
                 foreignOut, foreignOut ? foreignOutSummary.str().c_str() : "(none)");
        }
        {
            std::lock_guard<std::mutex> lk(naming_cache_mu_);
            tree_to_naming_result_[treeId] = out;
        }
        return out;
    }

} // namespace naming
} // namespace fastbotx
