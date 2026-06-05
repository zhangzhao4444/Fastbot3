/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 *
 * @file Model.cpp
 * @brief Core exploration model: state capture, dynamic abstraction / naming refinement, transitions, and scheduling.
 */
#ifndef Model_CPP_
#define Model_CPP_

#include "Model.h"
#include "StateFactory.h"
#include "../desc/reuse/ReuseState.h"
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
#include "../desc/gui_tree/GUITreeNode.h"
#include "../desc/reuse/ActivityNameAction.h"
#include "../desc/naming/NamerFactory.h"
#include "../desc/naming/NamerType.h"
#include "../desc/naming/ActionPatchNamer.h"
#endif
#ifndef NDEBUG
#include <cassert>
#include <thread>
#endif
#include <atomic>
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
#include "../desc/gui_tree/GUITreeFactory.h"
#include "../desc/gui_tree/GUITree.h"
#include "../desc/naming/NamingFactory.h"
#include "../desc/xpath/XPathNodeMapper.h"
#include "../desc/Element.h"
#include <functional>
#include <sstream>
namespace {
using namespace fastbotx;
/** @brief 64-bit hash of a string for log correlation and lightweight signatures. */
uint64_t hashStringForLog(const std::string &value) {
    return static_cast<uint64_t>(std::hash<std::string>{}(value));
}

/** @brief Truncates a string to a maximum length, appending an ellipsis when needed (compact logs). */
std::string abbreviateTextForLog(const std::string &value, size_t limit = 24) {
    if (value.size() <= limit) {
        return value;
    }
    return value.substr(0, limit) + "...";
}

/** @brief Formats a one-line summary of an Element (class, resource id, text, bounds, first children). */
std::string summarizeElementForLog(const fastbotx::ElementPtr &element, size_t childLimit = 3) {
    if (!element) {
        return std::string("(null)");
    }
    std::ostringstream oss;
    const RectPtr bounds = element->getBounds();
    oss << "root=" << element->getClassname()
        << ":" << element->getResourceID()
        << " text=" << abbreviateTextForLog(element->getText());
    if (bounds) {
        oss << " bounds=[" << bounds->left << "," << bounds->top
            << "]-[" << bounds->right << "," << bounds->bottom << "]";
    } else {
        oss << " bounds=(null)";
    }
    const auto &children = element->getChildren();
    oss << " children=" << children.size();
    size_t n = 0;
    for (const auto &child : children) {
        if (!child) {
            continue;
        }
        oss << (n == 0 ? " childSummary=" : " | ");
        oss << "#" << n << "=" << child->getClassname() << ":" << child->getResourceID();
        ++n;
        if (n >= childLimit) {
            break;
        }
    }
    if (children.size() > n) {
        oss << " | ... total=" << children.size();
    }
    return oss.str();
}

/** @brief Formats a one-line summary of a GUITree (root, children, sample xpaths). */
std::string summarizeGUITreeForLog(const fastbotx::gui_tree::GUITreePtr &tree, size_t childLimit = 3,
                                   size_t xpathLimit = 3) {
    if (!tree) {
        return std::string("(null)");
    }
    std::ostringstream oss;
    const auto &root = tree->getRootNodePtr();
    if (!root) {
        return std::string("root=(null)");
    }
    oss << "root=" << root->getClassName() << ":" << root->getResourceId()
        << " text=" << abbreviateTextForLog(root->getText())
        << " bounds=" << root->getBounds().toString()
        << " children=" << root->getChildren().size();
    size_t n = 0;
    for (const auto &child : root->getChildren()) {
        if (!child) {
            continue;
        }
        oss << (n == 0 ? " childSummary=" : " | ");
        oss << "#" << n << "=" << child->getClassName() << ":" << child->getResourceId();
        ++n;
        if (n >= childLimit) {
            break;
        }
    }
    if (root->getChildren().size() > n) {
        oss << " | ... total=" << root->getChildren().size();
    }
    const auto &xpaths = tree->getCurrentXPaths();
    oss << " xpaths=" << xpaths.size();
    const size_t lim = std::min<size_t>(xpaths.size(), xpathLimit);
    for (size_t i = 0; i < lim; ++i) {
        oss << (i == 0 ? " xpathSummary=" : " | ");
        oss << "#" << i << "=" << xpaths[i];
    }
    if (xpaths.size() > lim) {
        oss << " | ... total=" << xpaths.size();
    }
    return oss.str();
}

/** @brief Summarizes up to `limit` widgets from a State for logging. */
std::string summarizeStateWidgetsForLog(const fastbotx::StatePtr &state, size_t limit = 3) {
    if (!state) {
        return std::string("(null)");
    }
    std::ostringstream oss;
    size_t n = 0;
    for (const auto &w : state->getWidgets()) {
        if (!w) {
            continue;
        }
        if (n != 0) {
            oss << " | ";
        }
        oss << "#" << n << "=" << w->getClass() << ":" << w->getResourceID();
        ++n;
        if (n >= limit) {
            break;
        }
    }
    if (n == 0) {
        return std::string("(empty)");
    }
    if (state->getWidgets().size() > n) {
        oss << " | ... total=" << state->getWidgets().size();
    }
    return oss.str();
}

/** @brief Summarizes up to `limit` actions from a State for logging. */
std::string summarizeStateActionsForLog(const fastbotx::StatePtr &state, size_t limit = 3) {
    if (!state) {
        return std::string("(null)");
    }
    std::ostringstream oss;
    size_t n = 0;
    for (const auto &a : state->getActions()) {
        if (!a) {
            continue;
        }
        if (n != 0) {
            oss << " | ";
        }
        const auto tgt = a->getTarget();
        oss << "#" << n << "=" << static_cast<int>(a->getActionType()) << ":";
        if (tgt) {
            oss << tgt->getClass() << ":" << tgt->getResourceID();
        } else {
            oss << "(no-target)";
        }
        ++n;
        if (n >= limit) {
            break;
        }
    }
    if (n == 0) {
        return std::string("(empty)");
    }
    if (state->getActions().size() > n) {
        oss << " | ... total=" << state->getActions().size();
    }
    return oss.str();
}
/** Match buildApeStateKeyFromElementTree: parse XML to Element then buildFromElement (exclusion + attrs), else pugixml. */
fastbotx::gui_tree::GUITreeBuildResult buildGuitreeFromCachedXmlPreferElement(const std::string &xml,
                                                                              const std::string &pkg,
                                                                              const std::string &cls) {
    using namespace fastbotx;
    ElementPtr elem = Element::createFromXml(xml);
    if (elem) {
        gui_tree::GUITreeBuildResult built = gui_tree::GUITreeFactory::buildFromElement(elem, pkg, cls);
        if (built.tree && built.dom) {
            return built;
        }
    }
    return gui_tree::GUITreeFactory::buildFromXml(xml, pkg, cls);
}

/** @brief Builds a pugixml DOM and registers GUITree nodes for XPath evaluation over a GUI tree. */
static std::shared_ptr<fastbotx::gui_tree::XPathNodeMapper> buildXPathDomBridgeForGUITree(
    const fastbotx::gui_tree::GUITreePtr &tree) {
    using namespace fastbotx;
    using namespace fastbotx::gui_tree;
    if (!tree) {
        return nullptr;
    }
    const GUITreeNodePtr &root = tree->getRootNodePtr();
    if (!root) {
        return nullptr;
    }
    auto bridge = std::make_shared<XPathNodeMapper>();
    pugi::xml_node xmlRoot = bridge->initEmptyDocumentWithRoot("node");
    if (!xmlRoot) {
        return nullptr;
    }

    auto setBool = [](pugi::xml_node xn, const char *k, bool v) {
        if (!xn || !k || !*k) {
            return;
        }
        xn.append_attribute(k) = (v ? "true" : "false");
    };

    std::function<void(const GUITreeNodePtr &, pugi::xml_node, bool)> emit;
    emit = [&](const GUITreeNodePtr &n, pugi::xml_node xn, bool isOuterRoot) {
        if (!n || !xn) {
            return;
        }
        // Mirror the attributes that naming XPath expressions rely on.
        if (!n->getClassName().empty()) {
            xn.append_attribute("class") = n->getClassName().c_str();
        }
        if (!n->getResourceId().empty()) {
            xn.append_attribute("resource-id") = n->getResourceId().c_str();
        }
        if (!n->getPackageName().empty()) {
            xn.append_attribute("package") = n->getPackageName().c_str();
        }
        if (!n->getText().empty()) {
            xn.append_attribute("text") = n->getText().c_str();
        }
        if (!n->getContentDesc().empty()) {
            xn.append_attribute("content-desc") = n->getContentDesc().c_str();
        }
        xn.append_attribute("index") = n->getIndex();
        {
            const std::string b = n->getBounds().toString();
            if (!b.empty()) {
                xn.append_attribute("bounds") = b.c_str();
            }
        }
        setBool(xn, "enabled", n->isEnabled());
        setBool(xn, "clickable", n->isClickable());
        setBool(xn, "long-clickable", n->isLongClickable());
        setBool(xn, "checkable", n->isCheckable());
        setBool(xn, "checked", n->isChecked());
        setBool(xn, "focusable", n->isFocusable());
        setBool(xn, "focused", n->isFocused());
        setBool(xn, "password", n->isPassword());
        setBool(xn, "scrollable", n->getScrollable() != 0);

        bridge->registerNode(xn, n);

        // Children.
        for (const auto &ch : n->getChildren()) {
            if (!ch) {
                continue;
            }

            pugi::xml_node xc = xn.append_child("node");
            emit(ch, xc, false);
        }
        (void)isOuterRoot;
    };

    emit(root, xmlRoot, true);
    return bridge;
}

/**
 * DOM from XML (XPath bridge); optional GUITree snapshot from the same transition edge for tree body
 */
static fastbotx::gui_tree::GUITreeBuildResult buildGuitreePreferApeSnapshotAndDomXml(
    const std::string &xml, const std::string &pkg, const std::string &cls,
    const fastbotx::gui_tree::GUITreePtr &sourceSnapshot) {
    using namespace fastbotx;
    gui_tree::GUITreeBuildResult built = buildGuitreeFromCachedXmlPreferElement(xml, pkg, cls);
    // Prefer XML-space rebuild first (closer to per-tree document semantics).
    // Only fall back to snapshot if XML rebuild failed.
    if (built.tree && built.dom) {
        return built;
    }
    if (!sourceSnapshot) {
        return built;
    }
    // A single DOM+tree instance is expected per evaluation. If we prefer snapshot's tree body, we must
    // also build a DOM bridge that maps XPath results onto the *same* GUITreeNode instances.
    gui_tree::GUITreePtr treeCopy = gui_tree::GUITree::cloneDeep(*sourceSnapshot);
    if (!treeCopy) {
        return built;
    }
    if (auto dom = buildXPathDomBridgeForGUITree(treeCopy)) {
        built.tree = std::move(treeCopy);
        built.dom = std::move(dom);
    }
    return built;
}

/**
 * Strict source-tree mode for ND refine:
 * - If transition source snapshot exists, use that tree body directly (clone + DOM bridge),
 *   and DO NOT rebuild tree body from cached XML first.
 * - Only when snapshot is absent, fall back to XML rebuild.
 */
static fastbotx::gui_tree::GUITreeBuildResult buildGuitreeFromTransitionSourcePreferSnapshot(
    const std::string &xml, const std::string &pkg, const std::string &cls,
    const fastbotx::gui_tree::GUITreePtr &sourceSnapshot) {
    using namespace fastbotx;
    if (sourceSnapshot) {
        gui_tree::GUITreeBuildResult built;
        gui_tree::GUITreePtr treeCopy = gui_tree::GUITree::cloneDeep(*sourceSnapshot);
        if (!treeCopy) {
            return buildGuitreeFromCachedXmlPreferElement(xml, pkg, cls);
        }
        if (auto dom = buildXPathDomBridgeForGUITree(treeCopy)) {
            built.tree = std::move(treeCopy);
            built.dom = std::move(dom);
            return built;
        }
        return buildGuitreeFromCachedXmlPreferElement(xml, pkg, cls);
    }
    return buildGuitreeFromCachedXmlPreferElement(xml, pkg, cls);
}

/**
 * Predicate-eval object mode (checkPredicate affected set):
 * reuse the exact GUITree object instance when available so affected-membership stays object-level.
 */
static fastbotx::gui_tree::GUITreeBuildResult buildGuitreeFromSnapshotObjectOrCachedXml(
    const std::string &xml, const std::string &pkg, const std::string &cls,
    const fastbotx::gui_tree::GUITreePtr &sourceSnapshot) {
    using namespace fastbotx;
    if (sourceSnapshot) {
        gui_tree::GUITreeBuildResult built;
        if (auto dom = buildXPathDomBridgeForGUITree(sourceSnapshot)) {
            built.tree = sourceSnapshot;
            built.dom = std::move(dom);
            return built;
        }
    }
    return buildGuitreeFromCachedXmlPreferElement(xml, pkg, cls);
}

/**
 * Widget -> GUITreeNode property matching (bounds + class + resource-id).
 *
 * ReuseState::getStableElementIdForWidget produces a preorder index over the *unfiltered*
 * Element tree. The live GUITree is built with pruning rules (invisible / empty-slot nodes
 * skipped), so Element-preorder index != GUITree-preorder index.
 * Using the Element stableId as a GUITree preorder index can land on an unrelated node and cause
 * `target_xpath_name_null` / `target_name_unresolved` refine failures.
 *
 * This helper matches Widget against GUITreeNode by screen-visible identity (bounds + class +
 * resource-id), which is unique within a single UI snapshot.
 */
/**
 * Strict match (bounds + class + resource-id). Caller-owned diagnostic logging.
 * When used on hot paths (e.g. per-candidate evaluation), we cannot afford a per-miss log here.
 */
int apeFindGuiTreeNodePreorderIndexForWidget(const std::vector<gui_tree::GUITreeNode *> &preorder,
                                             const WidgetPtr &widget) {
    if (!widget) {
        return -1;
    }
    std::shared_ptr<Rect> wb = widget->getBounds();
    if (!wb) {
        return -1;
    }
    const std::string &wClass = widget->getClassname();
    const std::string &wRid = widget->getResourceID();
    for (size_t i = 0; i < preorder.size(); ++i) {
        gui_tree::GUITreeNode *n = preorder[i];
        if (!n) {
            continue;
        }
        const Rect &nb = n->getBounds();
        if (nb.left != wb->left || nb.top != wb->top || nb.right != wb->right || nb.bottom != wb->bottom) {
            continue;
        }
        if (n->getClassName() != wClass) {
            continue;
        }
        if (n->getResourceId() != wRid) {
            continue;
        }
        return static_cast<int>(i);
    }
    return -1;
}

/**
 * Diagnostic helper: classify *why* strict match failed on a given preorder/widget pair.
 * Returns the index of the first bounds-only / bounds+class soft match so the caller can
 * log attribution without doing two passes itself.
 */
struct ApeWidgetMatchMiss {
    size_t preorderN{0};
    int boundsMatchCount{0};
    int boundsClassMatchCount{0};
    int firstBoundsClassIdx{-1};
    int firstBoundsOnlyIdx{-1};
};

ApeWidgetMatchMiss apeClassifyWidgetMatchMiss(const std::vector<gui_tree::GUITreeNode *> &preorder,
                                             const WidgetPtr &widget) {
    ApeWidgetMatchMiss out;
    out.preorderN = preorder.size();
    if (!widget) {
        return out;
    }
    std::shared_ptr<Rect> wb = widget->getBounds();
    if (!wb) {
        return out;
    }
    const std::string &wClass = widget->getClassname();
    for (size_t i = 0; i < preorder.size(); ++i) {
        gui_tree::GUITreeNode *n = preorder[i];
        if (!n) {
            continue;
        }
        const Rect &nb = n->getBounds();
        if (nb.left != wb->left || nb.top != wb->top || nb.right != wb->right || nb.bottom != wb->bottom) {
            continue;
        }
        ++out.boundsMatchCount;
        if (out.firstBoundsOnlyIdx < 0) {
            out.firstBoundsOnlyIdx = static_cast<int>(i);
        }
        if (n->getClassName() == wClass) {
            ++out.boundsClassMatchCount;
            if (out.firstBoundsClassIdx < 0) {
                out.firstBoundsClassIdx = static_cast<int>(i);
            }
        }
    }
    return out;
}

/** @brief Formats a list of preorder stable IDs as a compact bracketed string for logs. */
std::string apeJoinStableIds(const std::vector<int> &ids) {
    if (ids.empty()) {
        return "[]";
    }
    std::string out = "[";
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) {
            out.append(",");
        }
        out.append(std::to_string(ids[i]));
    }
    out.push_back(']');
    return out;
}

/** @brief Stable string key for a Name: namer semantic key plus XPath or cache key (used in refinement comparisons). */
std::string nameIdentityKey(const naming::NamePtr &nm) {
    if (!nm) {
        return std::string();
    }
    const naming::NamerPtr nmr = nm->getNamer();
    const std::string nk = nmr ? naming::namerSemanticKey(*nmr) : std::string("0:");
    const std::string key = nm->cacheKeyString().empty() ? nm->toXPath() : nm->cacheKeyString();
    return nk + "|" + key;
}

/** @brief Short debug line for a GUITreeNode (index, class, resource id, bounds, xpath, namelet). */
std::string apeNodeDebugSummary(gui_tree::GUITreeNode *node) {
    if (!node) {
        return "node=null";
    }
    std::string summary = "idx=" + std::to_string(node->getIndex());
    summary.append(",class=");
    summary.append(node->getClassName());
    summary.append(",res=");
    summary.append(node->getResourceId());
    summary.append(",bounds=");
    summary.append(node->getBounds().toString());
    const naming::NamePtr nm = node->getXPathName();
    summary.append(",xpath=");
    summary.append(nm ? nm->toXPath() : "null");
    const naming::NameletPtr nl = node->getCurrentNamelet();
    summary.append(",nameletExpr=");
    summary.append(nl ? nl->getExprString() : "null");
    summary.append(",nameletMask=");
    if (nl && nl->getNamerPtr()) {
        summary.append(std::to_string(nl->getNamerPtr()->typeDimensionMask()));
    } else {
        summary.append("null");
    }
    return summary;
}

/** @brief One-line summary of a nondeterministic branch source transition for logging. */
std::string apeTransitionDebugSummary(const NondetTreeTransitionBranchPair::SourceTransition &t) {
    return std::string("seq=") + std::to_string(static_cast<unsigned long long>(t.transitionSeq)) +
           ",srcStateHash=" + std::to_string(static_cast<size_t>(t.sourceStateHash)) +
           ",targetStateHash=" + std::to_string(static_cast<size_t>(t.targetStateHash)) +
           ",stableIds=" + apeJoinStableIds(t.resolvedNodeStableIds) +
           ",xmlLen=" + std::to_string(t.sourceXml.size());
}

/** @brief Joins debug summaries for a vector of source transitions into one log string. */
std::string apeTransitionListDebugSummary(
    const std::vector<NondetTreeTransitionBranchPair::SourceTransition> &ts) {
    if (ts.empty()) {
        return "[]";
    }

    std::string out = "[";
    for (size_t i = 0; i < ts.size(); ++i) {
        if (i > 0) {
            out.append("; ");
        }

        out.append("#");
        out.append(std::to_string(i));
        out.append("{");
        out.append(apeTransitionDebugSummary(ts[i]));
        out.append("}");
    }
    out.push_back(']');
    return out;
}

} // namespace
#endif
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
namespace {
using namespace fastbotx;

struct ApeNonDetPairStat {
    std::string sourceActivity;
    uintptr_t sourceKeyHash{0};
    bool hasSourceStateKey{false};
    naming::StateKey sourceStateKey = naming::StateKey::fromParts("", nullptr, {});
    uintptr_t actionHash{0};
    std::unordered_set<uintptr_t> targetKeyHashes;
    size_t targetCount{0};
};

/** @brief Depth-first preorder traversal of a GUITree, appending each node pointer to `out`. */
void collectGUITreeNodesPreOrder(gui_tree::GUITreeNode *node, std::vector<gui_tree::GUITreeNode *> *out) {
    if (!node || !out) {
        return;
    }
    out->push_back(node);
    for (const auto &ch : node->getChildren()) {
        collectGUITreeNodesPreOrder(ch.get(), out);
    }
}

/** @brief Maps action targets to abstract RL identity hashes on ReuseState using GUI-tree xpaths (not raw stable IDs). */
void applyApeDynamicActionHashesToReuseState(const StatePtr &state,
                                             const std::vector<gui_tree::GUITreeNode *> &nodesPreOrder,
                                             const naming::StateKey &apeKey) {
    if (!state) {
        return;
    }
    uint32_t fullMask = 0;
    for (auto t : naming::namerTypesUsed()) {
        fullMask |= (1u << static_cast<unsigned>(t));
    }
    naming::NamerPtr fullNamer = naming::NamerFactory::current().getByMask(fullMask);
    const uintptr_t activityH = fastStringHash(apeKey.activity());
    auto reuseState = std::dynamic_pointer_cast<ReuseState>(state);
    auto nodeForWidget = [&](const WidgetPtr &widget) -> gui_tree::GUITreeNode * {
        if (!reuseState || !widget || nodesPreOrder.empty()) {
            return nullptr;
        }
        // Property match (bounds + class + resource-id) - GUITree pruning breaks any identity
        // that relies on Element-preorder stableId indexing into GUITree preorder.
        const int gidx = apeFindGuiTreeNodePreorderIndexForWidget(nodesPreOrder, widget);
        if (gidx < 0) {
            return nullptr;
        }
        return nodesPreOrder[static_cast<size_t>(gidx)];
    };
    auto stableTargetHashForWidget = [&](const WidgetPtr &widget) -> uintptr_t {
        if (!reuseState || !widget) {
            return 0x1;
        }
        const int stableId = reuseState->getStableElementIdForWidget(widget);
        if (stableId < 0) {
            return 0x1;
        }
        // Build deterministic per-element hash (stable across toolchains/runs).
        const uint64_t sid = static_cast<uint64_t>(static_cast<uint32_t>(stableId));
        const uint64_t mixed = sid * 11400714819323198485ull;
        return static_cast<uintptr_t>(mixed ^ (mixed >> 32));
    };
    const ActivityStateActionPtrVec &acts = state->getActions();
    const bool deriveActions = naming::useActionPatchDeriveActionsFromName();
    std::vector<uint8_t> keepMask;
    if (deriveActions) {
        keepMask.assign(acts.size(), 1);
    }
    size_t targetActions = 0;
    size_t noTargetActions = 0;
    size_t mappedXPath = 0;
    size_t mappedStableId = 0;
    size_t fallbackConst = 0;

    auto canonicalForCheck = [](ActionType t) -> ActionType {
        if (t == ActionType::SCROLL_BOTTOM_UP_N) {
            return ActionType::SCROLL_BOTTOM_UP;
        }
        return t;
    };

    auto kindBitForAction = [](ActionType t) -> uint32_t {
        switch (t) {
        case ActionType::CLICK:
            return naming::kDerivedKindClick;
        case ActionType::LONG_CLICK:
            return naming::kDerivedKindLongClick;
        case ActionType::SCROLL_TOP_DOWN:
        case ActionType::SCROLL_BOTTOM_UP:
        case ActionType::SCROLL_LEFT_RIGHT:
        case ActionType::SCROLL_RIGHT_LEFT:
        case ActionType::SCROLL_BOTTOM_UP_N:
            return naming::kDerivedKindScroll;
        default:
            return 0;
        }
    };

    for (size_t ai = 0; ai < acts.size(); ++ai) {
        const auto &a = acts[ai];
        auto ana = std::dynamic_pointer_cast<ActivityNameAction>(a);
        if (!ana) {
            continue;
        }
        naming::NamePtr nxp = nullptr;
        WidgetPtr w = ana->getTarget();
        if (!w) {
            noTargetActions++;
            ana->applyApeDynamicRlIdentity(activityH, 0x1);
            ana->setApeDynamicTargetFullPathHash(0);
            continue;
        }
        targetActions++;
        uintptr_t abstractTargetHash = stableTargetHashForWidget(w);
        uintptr_t fullTargetHash = 0;
        gui_tree::GUITreeNode *n = nodeForWidget(w);
        if (n) {
            nxp = n->getXPathName();
            if (nxp) {
                abstractTargetHash = fastStringHash(nxp->toXPath());
                mappedXPath++;
            } else if (abstractTargetHash != 0x1) {
                mappedStableId++;
            }
            if (fullNamer) {
                std::string fullKey = fullNamer->xpathKeyForNode(*n);
                if (fullKey.empty()) {
                    naming::NamePtr fullName = fullNamer->naming(*n);
                    if (fullName) {
                        fullKey = fullName->toXPath();
                    }
                }
                if (!fullKey.empty()) {
                    fullTargetHash = fastStringHash(fullKey);
                }
            }
        } else if (abstractTargetHash != 0x1) {
            mappedStableId++;
        }
        if (!nxp && abstractTargetHash == 0x1) {
            fallbackConst++;
        }
        ana->applyApeDynamicRlIdentity(activityH, abstractTargetHash);
        ana->setApeDynamicTargetFullPathHash(fullTargetHash);

        if (deriveActions && nxp && ai < keepMask.size()) {
            uint32_t allowedMask = 0;
            uint32_t knownKinds = 0;
            if (naming::decodeApeDerivedActionsFromName(nxp, &allowedMask, &knownKinds)) {
                const ActionType canonical = canonicalForCheck(ana->getActionType());
                const uint32_t kindBit = kindBitForAction(canonical);
                if (kindBit != 0 && (knownKinds & kindBit) != 0) {
                    const uint32_t actBit = 1u << static_cast<unsigned>(canonical);
                    if ((allowedMask & actBit) == 0) {
                        keepMask[ai] = 0;
                    }
                }
            }
        }
    }

    if (deriveActions && !keepMask.empty()) {
        state->filterActionsByKeepMask(keepMask);
    }
    if ((targetActions + noTargetActions) > 0) {
        BLOG("naming action hash mapping: activity=%s targetActions=%zu noTargetActions=%zu mappedXPath=%zu mappedStableId=%zu fallbackConst=%zu",
             apeKey.activity().c_str(), targetActions, noTargetActions, mappedXPath, mappedStableId,
             fallbackConst);
    }
}
} // namespace

#include "../desc/naming/NamingFactory.h"
#include "../desc/naming/NamerLattice.h"
#endif
#include "../Base.h"
namespace {
/**
 * @brief Counts graph states whose activity matches the canonical activity key.
 *
 * Used across abstraction-disabled and enabled builds because naming refinement always compiles.
 */
size_t apeGraphActivityStateCountLikeJavaActivityNode(const fastbotx::GraphPtr &graph,
                                                      const std::string &actKeyCanonical) {
    if (!graph || actKeyCanonical.empty()) {
        return 0;
    }
    size_t c = 0;
    for (const fastbotx::StatePtr &sp : graph->getStates()) {
        if (!sp) {
            continue;
        }
        auto ap = sp->getActivityString();
        const std::string ac =
            (ap && ap.get()) ? fastbotx::naming::StateKey::canonicalActivityString(*ap) : std::string();
        if (ac == actKeyCanonical) {
            ++c;
        }
    }
    return c;
}
} // namespace
#include "../utils.hpp"
#include "../thirdpart/json/json.hpp"
#include "../llm/HttpLlmClient.h"
#include <algorithm>
#include <ctime>
#include <inttypes.h>
#include <iostream>
#include <map>
#include <fstream>
#include <sstream>
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {
    /** @brief Converts WidgetKeyMask bits to a pipe-separated label string for logging. */
    std::string maskToDimensionString(fastbotx::WidgetKeyMask m) {
        std::ostringstream os;
        const char *sep = "";
        if (m & static_cast<fastbotx::WidgetKeyMask>(fastbotx::WidgetKeyAttr::Clazz)) { os << sep << "Clazz"; sep = "|"; }
        if (m & static_cast<fastbotx::WidgetKeyMask>(fastbotx::WidgetKeyAttr::ResourceID)) { os << sep << "ResourceID"; sep = "|"; }
        if (m & static_cast<fastbotx::WidgetKeyMask>(fastbotx::WidgetKeyAttr::OperateMask)) { os << sep << "OperateMask"; sep = "|"; }
        if (m & static_cast<fastbotx::WidgetKeyMask>(fastbotx::WidgetKeyAttr::ScrollType)) { os << sep << "ScrollType"; sep = "|"; }
        if (m & static_cast<fastbotx::WidgetKeyMask>(fastbotx::WidgetKeyAttr::Text)) { os << sep << "Text"; sep = "|"; }
        if (m & static_cast<fastbotx::WidgetKeyMask>(fastbotx::WidgetKeyAttr::ContentDesc)) { os << sep << "ContentDesc"; sep = "|"; }
        if (m & static_cast<fastbotx::WidgetKeyMask>(fastbotx::WidgetKeyAttr::Index)) { os << sep << "Index"; sep = "|"; }
        return os.str().empty() ? "(none)" : os.str();
    }

    constexpr int kNDActionBlacklistMinOutEdges = 3;

    /**
     * NamingFactory.sortRefinementResults / filterRefinementResult tie-break:
     * after primary keys (replay/score), prefer fewer induced partitions — proxy: smaller finenessGain;
     * then lexicographic namelets (expr, then compareNamer — reference comparator on updated expr).
     */
    int compareNamingLexicographicForApeFilter(const fastbotx::naming::NamingPtr &a,
                                               const fastbotx::naming::NamingPtr &b) {
        using namespace fastbotx::naming;
        if (!a && !b) {
            return 0;
        }
        if (!a) {
            return 1;
        }
        if (!b) {
            return -1;
        }
        const auto &va = a->getNamelets();
        const auto &vb = b->getNamelets();
        if (va.size() != vb.size()) {
            return va.size() < vb.size() ? -1 : 1;
        }
        for (size_t i = 0; i < va.size(); ++i) {
            const auto &nla = va[i];
            const auto &nlb = vb[i];
            if (!nla && !nlb) {
                continue;
            }
            if (!nla) {
                return 1;
            }
            if (!nlb) {
                return -1;
            }
            int e = nla->getExprString().compare(nlb->getExprString());
            if (e != 0) {
                return e;
            }
            int n = compareNamer(nla->getNamer(), nlb->getNamer());
            if (n != 0) {
                return n;
            }
        }
        return 0;
    }
}
#endif

#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
namespace {
using ApeHashCache = std::unordered_map<uintptr_t, uintptr_t>;
/** @brief Wraps NamingFactory::rebuildTree with optional stage-tagged logging for diagnostics. */
bool safeRebuildTree(const fastbotx::naming::NamingPtr &nm, fastbotx::gui_tree::GUITree &tree,
                     const std::shared_ptr<fastbotx::gui_tree::XPathNodeMapper> &dom,
                     const char *stage = nullptr) {
    static std::atomic<uint64_t> g_rebuild_tree_seq{0};
    const uint64_t seq = ++g_rebuild_tree_seq;
    const bool shouldLog = (seq <= 20 || (seq % 400) == 0);
    const char *stageTag = (stage && *stage) ? stage : "-";
    const fastbotx::gui_tree::GUITreePtr treeAlias(&tree, [](fastbotx::gui_tree::GUITree *) {});
    const std::string beforeSummary = shouldLog ? summarizeGUITreeForLog(treeAlias) : std::string();
    if (shouldLog) {
        BLOG("naming rebuild: enter seq=%" PRIu64 " stage=%s namingFp=%s treePtr=%p dom=%d %s",
             seq, stageTag, nm ? nm->fingerprintString().c_str() : "(null)", &tree, dom ? 1 : 0,
             beforeSummary.c_str());
    }
    fastbotx::naming::setRebuildLogStage(stageTag);
    const bool ok = fastbotx::naming::NamingFactory::rebuildTree(nm, tree, dom);
    fastbotx::naming::clearRebuildLogStage();
    if (shouldLog) {
        BLOG("naming rebuild: exit seq=%" PRIu64 " stage=%s ok=%d treePtr=%p %s",
             seq, stageTag, ok ? 1 : 0, &tree, summarizeGUITreeForLog(treeAlias).c_str());
    }
    return ok;
}

/**
 * Build GUITree from XML (or prefer a transition snapshot), then NamingFactory::rebuildTree for `naming`.
 * Shared by hash paths, isStateEquivalent / isTopNamingEquivalent (full StateKey equality, reference parity).
 */
bool apeGuitreeFromXmlWithNamingRebuilt(const std::string &activity, const std::string &xml,
                                        const fastbotx::naming::NamingPtr &naming,
                                        fastbotx::gui_tree::GUITreeBuildResult *outBuilt,
                                        const fastbotx::gui_tree::GUITreePtr *preferGuiSnapshot) {
    if (!naming || xml.empty() || !outBuilt) {
        return false;
    }
    std::string pkg;
    std::string cls;
    fastbotx::naming::StateKey::splitActivityPackageClass(activity, &pkg, &cls);
    const bool usePreferSnapshot = (preferGuiSnapshot && *preferGuiSnapshot);
    fastbotx::gui_tree::GUITreeBuildResult built =
        usePreferSnapshot
            ? buildGuitreeFromTransitionSourcePreferSnapshot(xml, pkg, cls, *preferGuiSnapshot)
            : buildGuitreeFromCachedXmlPreferElement(xml, pkg, cls);
    if (!built.tree || !built.dom) {
        return false;
    }
    if (!safeRebuildTree(naming, *built.tree, built.dom, "state_check")) {
        return false;
    }
    *outBuilt = std::move(built);
    return true;
}

/**
 * When branch SourceTransition did not copy sourceGuiSnapshot, recover the source GUITree from the
 * ring-buffer `TreeTransitionEntry` for the same `transitionSeq`.
 */
static fastbotx::gui_tree::GUITreePtr apeLookupApeSourceGuiTreeByTransitionSeq(
    uint64_t transitionSeq, const std::vector<fastbotx::TreeTransitionEntry> &treeLog) {
    for (const fastbotx::TreeTransitionEntry &te : treeLog) {
        if (te.valid && te.transitionSeq == transitionSeq && te.apeSourceGuiTree) {
            return te.apeSourceGuiTree;
        }
    }
    return nullptr;
}

/** Prefer `SourceTransition.sourceGuiSnapshot`; else `TreeTransitionEntry.apeSourceGuiTree` from the tree log row. */
static void apePreferSnapPtrFromSourceTransition(
    const fastbotx::NondetTreeTransitionBranchPair::SourceTransition &st,
    const std::vector<fastbotx::TreeTransitionEntry> &treeLog,
    fastbotx::gui_tree::GUITreePtr *outFallbackStorage,
    const fastbotx::gui_tree::GUITreePtr **outPreferSnapPtr) {
    if (outPreferSnapPtr) {
        *outPreferSnapPtr = nullptr;
    }
    if (outFallbackStorage) {
        outFallbackStorage->reset();
    }
    if (!outPreferSnapPtr) {
        return;
    }
    if (st.sourceGuiSnapshot) {
        *outPreferSnapPtr = &st.sourceGuiSnapshot;
        return;
    }
    fastbotx::gui_tree::GUITreePtr fromLog =
        apeLookupApeSourceGuiTreeByTransitionSeq(st.transitionSeq, treeLog);
    if (fromLog && outFallbackStorage) {
        *outFallbackStorage = std::move(fromLog);
        *outPreferSnapPtr = outFallbackStorage;
    }
}

/** @brief Computes abstract state-key hash from XML under `naming`, optionally reusing a GUI-tree snapshot and cache. */
bool apeStateHashFromXmlWithNaming(const std::string &activity, const std::string &xml,
                                    const fastbotx::naming::NamingPtr &naming,
                                    uintptr_t *outHash,
                                    uintptr_t cacheKey = 0, 
                                    ApeHashCache *cache = nullptr,
                                    const gui_tree::GUITreePtr *preferGuiSnapshot = nullptr) {
    using namespace fastbotx;
    if (!outHash || !naming || xml.empty()) {
        return false;
    }
    static std::atomic<uint64_t> g_xml_hash_build{0};
    const uint64_t seq = ++g_xml_hash_build;
    if (cache && cacheKey != 0) {
        auto itC = cache->find(cacheKey);
        if (itC != cache->end()) {
            *outHash = itC->second;
            return true;
        }
    }
    const bool usePreferSnapshot = (preferGuiSnapshot && *preferGuiSnapshot);
    if (seq <= 40 || (seq % 400) == 0) {
        BDLOG("naming xml hash: entry activity=%s seq=%" PRIu64
              " xmlSig=%" PRIu64 " xmlLen=%zu namingFp=%s preferSnapshot=%d snapPtr=%p snapSummary=%s",
              activity.c_str(), seq, hashStringForLog(xml), xml.size(),
              naming->fingerprintString().c_str(), usePreferSnapshot ? 1 : 0,
              usePreferSnapshot ? (*preferGuiSnapshot).get() : nullptr,
              usePreferSnapshot ? summarizeGUITreeForLog(*preferGuiSnapshot).c_str() : "(null)");
    }
    gui_tree::GUITreeBuildResult built;
    if (!apeGuitreeFromXmlWithNamingRebuilt(activity, xml, naming, &built, preferGuiSnapshot)) {
        return false;
    }
    if (seq <= 40 || (seq % 400) == 0) {
        BLOG("naming xml hash: tree after build+rebuild (state_check) activity=%s seq=%" PRIu64
             " source=%s treePtr=%p dom=%d namingFp=%s %s",
             activity.c_str(), seq, usePreferSnapshot ? "prefer_snapshot" : "xml_only", built.tree.get(),
             built.dom ? 1 : 0, naming->fingerprintString().c_str(),
             summarizeGUITreeForLog(built.tree).c_str());
    }
    *outHash = naming::StateKey::hashFromGUITree(*built.tree);
    if (seq <= 40 || (seq % 400) == 0) {
        BLOG("naming xml hash: exit activity=%s seq=%" PRIu64 " outHash=%" PRIuPTR,
             activity.c_str(), seq, static_cast<uintptr_t>(*outHash));
    }
    if (cache && cacheKey != 0) {
        (*cache)[cacheKey] = *outHash;
    }
    return true;
}

/** @brief Computes state hashes for the same XML under two different naming roots (single parse path where possible). */
bool apeStateHashFromXmlWithTwoNamings(const std::string &activity, const std::string &xml,
                                       const fastbotx::naming::NamingPtr &naming1, uintptr_t *outHash1,
                                       const fastbotx::naming::NamingPtr &naming2, uintptr_t *outHash2,
                                       const gui_tree::GUITreePtr *preferGuiSnapshot = nullptr) {
    if (!outHash1 || !outHash2 || !naming1 || !naming2 || xml.empty()) {
        return false;
    }
    *outHash1 = 0;
    *outHash2 = 0;
    const bool ok1 =
        apeStateHashFromXmlWithNaming(activity, xml, naming1, outHash1, 0, nullptr, preferGuiSnapshot);
    const bool ok2 =
        apeStateHashFromXmlWithNaming(activity, xml, naming2, outHash2, 0, nullptr, preferGuiSnapshot);
    return ok1 && ok2;
}

/**
 * Coarsen / blacklist / prune paths historically used two independent apeStateHashFromXmlWithNaming calls.
 * Prefer one XML parse + two rebuilds; if that fails (e.g. second rebuildTree fails), fall back to two full
 * parses so partial success and triggerSource==0 mapping fallback (prevKeyHash -> storedKey) match legacy.
 */
void apeStateKeyPairFromXmlCoarsenPath(const std::string &activity, const std::string &xml,
                                       const fastbotx::naming::NamingPtr &namingCur, uintptr_t *tgtKeyHash,
                                       const fastbotx::naming::NamingPtr &namingPrev, uintptr_t *prevKeyHash) {
    if (!tgtKeyHash || !prevKeyHash || xml.empty() || !namingCur || !namingPrev) {
        return;
    }
    *tgtKeyHash = 0;
    *prevKeyHash = 0;
    (void)apeStateHashFromXmlWithNaming(activity, xml, namingCur, tgtKeyHash);
    (void)apeStateHashFromXmlWithNaming(activity, xml, namingPrev, prevKeyHash);
}

/** @brief Upper bound on distinct state hashes allowed when evaluating a refinement candidate (depends on naming fineness). */
int apeMaxStatesForRefinementThreshold(const fastbotx::naming::NamingPtr &targetNaming) {
    if (!targetNaming) {
        return 8;
    }
    const int fineness = targetNaming->getFineness();
    const int total = static_cast<int>(fastbotx::naming::namerTypesUsed().size());
    int shift = total - fineness;
    if (shift < 0) {
        shift = 0;
    }
    return std::min(8, std::max(1, 2 << shift));
}

/** @brief DFS helper: collects fingerprint strings for `root` and each refinement child naming (cycle-safe). */
void collectNamingSubtreeFingerprintsImpl(
    const fastbotx::naming::NamingPtr &root, std::unordered_set<std::string> *out,
    std::unordered_set<const fastbotx::naming::Naming *> *visited) {
    if (!root || !out || !visited) {
        return;
    }

    const fastbotx::naming::Naming *const raw = root.get();
    if (visited->count(raw) != 0) {
        return;
    }

    visited->insert(raw);
    out->insert(root->fingerprintString());
    for (const auto &kv : root->getRefinementChildren()) {
        if (kv.second) {
            collectNamingSubtreeFingerprintsImpl(kv.second, out, visited);
        }
    }
}

/** Collect naming fingerprints for `root` and every descendant refinement naming (compare to graph “all states”). */
void collectNamingSubtreeFingerprints(const fastbotx::naming::NamingPtr &root,
                                      std::unordered_set<std::string> *out) {
    if (!root || !out) {
        return;
    }
    std::unordered_set<const fastbotx::naming::Naming *> visited;
    visited.reserve(64);
    collectNamingSubtreeFingerprintsImpl(root, out, &visited);
}

/** @brief State-split refinement check: two XML pools must yield disjoint state hashes within the fineness budget. */
bool apeCheckStateRefinementLikeJava(const std::string &activity, const fastbotx::naming::NamingPtr &newNaming,
                                     const fastbotx::naming::NamerPtr &newNamer,
                                     const std::vector<std::string> &tts1Sources,
                                     const std::vector<std::string> &tts2Sources,
                                     std::vector<fastbotx::naming::NamerPtr> *upperBounds, size_t *outPart1,
                                     size_t *outPart2,
                                     const std::vector<gui_tree::GUITreePtr> *tts1GuiSnapshots = nullptr,
                                     const std::vector<gui_tree::GUITreePtr> *tts2GuiSnapshots = nullptr) {
    if (!newNaming || !newNamer || !upperBounds || !outPart1 || !outPart2) {
        return false;
    }
    if (tts1Sources.empty() || tts2Sources.empty()) {
        return false;
    }
    auto snapFor = [&](size_t idx, bool side1) -> const gui_tree::GUITreePtr * {
        const std::vector<gui_tree::GUITreePtr> *v = side1 ? tts1GuiSnapshots : tts2GuiSnapshots;
        if (!v || idx >= v->size() || !(*v)[idx]) {
            return nullptr;
        }
        return &(*v)[idx];
    };
    uintptr_t hLast1 = 0;
    uintptr_t hLast2 = 0;
    if (!apeStateHashFromXmlWithNaming(activity, tts1Sources.back(), newNaming, &hLast1, 0, nullptr,
                                       snapFor(tts1Sources.size() - 1, true)) ||
        !apeStateHashFromXmlWithNaming(activity, tts2Sources.back(), newNaming, &hLast2, 0, nullptr,
                                       snapFor(tts2Sources.size() - 1, false))) {
        return false;
    }
    if (hLast1 == hLast2) {
        return false;
    }
    const int threshold = apeMaxStatesForRefinementThreshold(newNaming);
    std::unordered_set<uintptr_t> states1;
    std::unordered_set<uintptr_t> states2;
    for (size_t i = 0; i < tts1Sources.size(); ++i) {
        const std::string &xml = tts1Sources[i];
        uintptr_t h = 0;
        if (!apeStateHashFromXmlWithNaming(activity, xml, newNaming, &h, 0, nullptr, snapFor(i, true))) {
            return false;
        }
        if (!states1.insert(h).second) {
            continue;
        }
        if (static_cast<int>(states1.size()) > threshold) {
            upperBounds->push_back(newNamer);
            return false;
        }
    }
    for (size_t i = 0; i < tts2Sources.size(); ++i) {
        const std::string &xml = tts2Sources[i];
        uintptr_t h = 0;
        if (!apeStateHashFromXmlWithNaming(activity, xml, newNaming, &h, 0, nullptr, snapFor(i, false))) {
            return false;
        }
        if (states1.count(h) != 0) {
            return false;
        }
        if (!states2.insert(h).second) {
            continue;
        }
        if (static_cast<int>(states1.size() + states2.size()) > threshold) {
            upperBounds->push_back(newNamer);
            return false;
        }
    }
    upperBounds->push_back(newNamer);
    *outPart1 = states1.size();
    *outPart2 = states2.size();
    return true;
}

/** @brief Returns whether two namelets are equivalent (including both null). */
bool apeNameletMatches(const naming::NameletPtr &a, const naming::NameletPtr &b) {
    if (!a || !b) {
        return a.get() == b.get();
    }
    return *a == *b;
}

/** @brief Finds the index of `needle` in `naming`'s namelet list, or -1. */
ssize_t apeFindNameletIndex(const naming::NamingPtr &naming, const naming::NameletPtr &needle) {
    if (!naming || !needle) {
        return -1;
    }
    const auto &v = naming->getNamelets();
    for (size_t i = 0; i < v.size(); ++i) {
        if (apeNameletMatches(v[i], needle)) {
            return static_cast<ssize_t>(i);
        }
    }
    return -1;
}

/** @brief Builds the coarsest “top” naming: BASE namelet with the full namer lattice bitmask. */
naming::NamingPtr createTopNaming() {
    // Reference NamingFactory.createTopNaming: BASE namelet "//*" with lattice max namer
    // (NamerLattice typeToNamer.get(NamerType.allOf()) / bitmask with every namerTypesUsed bit).
    // Previously we used only the TYPE bit,
    // producing an almost-bottom naming: two XMLs with identical tree type-structure but
    // different text / resource-id / class / flags would hash equal, causing state
    // refinement to bail with reason=top_naming_equivalent even when the branches are
    // clearly distinguishable (observed ~50% false-equivalent rate: xmlsIdentical=0
    // with topH1==topH2 across 37/76 cases).
    uint32_t fullMask = 0;
    for (auto t : naming::namerTypesUsed()) {
        fullMask |= (1u << static_cast<unsigned>(t));
    }
    const naming::NamerPtr n = naming::NamerFactory::current().getByMask(fullMask);
    if (!n) {
        return nullptr;
    }
    std::vector<naming::NameletPtr> v;
    v.push_back(std::make_shared<naming::Namelet>(naming::Namelet::Type::BASE, "//*", n));
    return std::make_shared<naming::Naming>(std::move(v));
}

/**
 * XML-side port of NamingFactory.isStateEquivalent(Naming naming, GUITree t1, GUITree t2):
 * compare StateKey from two trees under the same `naming` via full StateKey::operator==
 * (activity + naming fingerprint + sorted widget xpaths), not hash-only equality.
 * Optional outputs: diagnostic StateKey::hash() per side; `outBothXmlPathsBuiltOk` iff both XML paths rebuilt.
 */
bool isStateEquivalent(const std::string &activity, const std::string &xmlA, const std::string &xmlB,
                       const naming::NamingPtr &naming, const gui_tree::GUITreePtr *preferSnapA = nullptr,
                       const gui_tree::GUITreePtr *preferSnapB = nullptr, uintptr_t *outHashA = nullptr,
                       uintptr_t *outHashB = nullptr, bool *outBothXmlPathsBuiltOk = nullptr) {
    if (outBothXmlPathsBuiltOk) {
        *outBothXmlPathsBuiltOk = false;
    }
    if (!naming || xmlA.empty() || xmlB.empty()) {
        return false;
    }
    fastbotx::gui_tree::GUITreeBuildResult builtA;
    fastbotx::gui_tree::GUITreeBuildResult builtB;
    if (!apeGuitreeFromXmlWithNamingRebuilt(activity, xmlA, naming, &builtA, preferSnapA)) {
        return false;
    }
    if (!apeGuitreeFromXmlWithNamingRebuilt(activity, xmlB, naming, &builtB, preferSnapB)) {
        return false;
    }
    if (outBothXmlPathsBuiltOk) {
        *outBothXmlPathsBuiltOk = true;
    }
    const naming::StateKey keyA = naming::StateKey::fromGUITree(*builtA.tree);
    const naming::StateKey keyB = naming::StateKey::fromGUITree(*builtB.tree);
    if (outHashA) {
        *outHashA = keyA.hash();
    }
    if (outHashB) {
        *outHashB = keyB.hash();
    }
    return keyA == keyB;
}

/** @brief True iff two XML snapshots are equivalent under the coarse top naming (same as reference top-naming check). */
bool isTopNamingEquivalent(const std::string &activity, const std::string &xmlA,
                           const std::string &xmlB, const gui_tree::GUITreePtr *preferSnapA = nullptr,
                           const gui_tree::GUITreePtr *preferSnapB = nullptr) {
    // Reference isTopNamingEquivalent -> isStateEquivalent(getTopNaming(), t1, t2).
    naming::NamingPtr top = createTopNaming();
    if (!top || xmlA.empty() || xmlB.empty()) {
        return false;
    }
    return isStateEquivalent(activity, xmlA, xmlB, top, preferSnapA, preferSnapB);
}

#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML

/**
 * Structural isomorphism on GUI tree nodes (NamingFactory.isIsomorphic node variant):
 * index, class, package, resourceId, enabled, text, contentDesc, clickable, checkable,
 * longClickable, scrollable; children same count and order, recursive.
 */
bool isIsomorphic(const gui_tree::GUITreeNode *t1, const gui_tree::GUITreeNode *t2) {
    if (!t1 || !t2) {
        return false;
    }
    if (t1->getIndex() != t2->getIndex()) {
        return false;
    }
    if (t1->getClassName() != t2->getClassName()) {
        return false;
    }
    if (t1->getPackageName() != t2->getPackageName()) {
        return false;
    }
    if (t1->getResourceId() != t2->getResourceId()) {
        return false;
    }
    if (t1->isEnabled() != t2->isEnabled()) {
        return false;
    }
    if (t1->getText() != t2->getText()) {
        return false;
    }
    if (t1->getContentDesc() != t2->getContentDesc()) {
        return false;
    }
    if (t1->isClickable() != t2->isClickable()) {
        return false;
    }
    if (t1->isCheckable() != t2->isCheckable()) {
        return false;
    }
    if (t1->isLongClickable() != t2->isLongClickable()) {
        return false;
    }
    if (t1->getScrollable() != t2->getScrollable()) {
        return false;
    }
    const auto &c1 = t1->getChildren();
    const auto &c2 = t2->getChildren();
    if (c1.size() != c2.size()) {
        return false;
    }
    for (size_t i = 0; i < c1.size(); ++i) {
        if (!isIsomorphic(c1[i].get(), c2[i].get())) {
            return false;
        }
    }
    return true;
}

/** Pairwise isomorphism on full `GUITree` wrappers (delegates to root nodes). */
bool isIsomorphic(const gui_tree::GUITree &t1, const gui_tree::GUITree &t2) {
    const gui_tree::GUITreeNode *r1 = t1.getRootNode();
    const gui_tree::GUITreeNode *r2 = t2.getRootNode();
    if (!r1 || !r2) {
        return false;
    }
    return isIsomorphic(r1, r2);
}

/**
 * Build two GUITrees from XML (optional transition snapshots), then isIsomorphic(tree, tree).
 * No Naming rebuild — raw widget-tree comparison only (stateRefinement uses this for logging).
 */
bool isIsomorphic(const std::string &activity, const std::string &xmlA, const std::string &xmlB,
                  const gui_tree::GUITreePtr *preferSnapA = nullptr,
                  const gui_tree::GUITreePtr *preferSnapB = nullptr) {
    if (xmlA.empty() || xmlB.empty()) {
        return false;
    }
    std::string pkg;
    std::string cls;
    naming::StateKey::splitActivityPackageClass(activity, &pkg, &cls);
    gui_tree::GUITreeBuildResult ba =
        (preferSnapA && *preferSnapA)
            ? buildGuitreeFromTransitionSourcePreferSnapshot(xmlA, pkg, cls, *preferSnapA)
            : buildGuitreeFromCachedXmlPreferElement(xmlA, pkg, cls);
    gui_tree::GUITreeBuildResult bb =
        (preferSnapB && *preferSnapB)
            ? buildGuitreeFromTransitionSourcePreferSnapshot(xmlB, pkg, cls, *preferSnapB)
            : buildGuitreeFromCachedXmlPreferElement(xmlB, pkg, cls);
    if (!ba.tree || !bb.tree) {
        return false;
    }
    return isIsomorphic(*ba.tree, *bb.tree);
}

#endif

/** @brief Locates the GUITree node for `targetWidget` using bounds/class/resource-id match on `preorder`. */
bool apeFindNodeForTargetWidget(const StatePtr &sourceState, const WidgetPtr &targetWidget,
                                const std::vector<gui_tree::GUITreeNode *> &preorder,
                                gui_tree::GUITreeNode **outNode,
                                std::string *outMissReason = nullptr) {
    if (!targetWidget || !outNode) {
        if (outMissReason) {
            *outMissReason = "invalid_target_or_output";
        }
        return false;
    }

    auto rs = std::dynamic_pointer_cast<ReuseState>(sourceState);
    if (!rs) {
        if (outMissReason) {
            *outMissReason = "source_state_not_reuse_state";
        }
        return false;
    }
    const int gidx = apeFindGuiTreeNodePreorderIndexForWidget(preorder, targetWidget);
    if (gidx < 0) {
        if (outMissReason) {
            *outMissReason = "widget_not_matched_in_gui_tree";
        }
        return false;
    }
    const size_t idx = static_cast<size_t>(gidx);
    if (idx >= preorder.size() || !preorder[idx]) {
        if (outMissReason) {
            *outMissReason = "preorder_node_null_at_matched_index";
        }
        return false;
    }
    *outNode = preorder[idx];
    return true;
}

/** @brief Legacy stable IDs from Element preorder for `targetWidget` (no live GUITree); prefer snapshot overload when available. */
std::vector<int> apeResolveStableIdsForTargetWidgetLikeJava(const StatePtr &sourceState,
                                                             const WidgetPtr &targetWidget) {
    std::vector<int> resolvedNodeStableIds;
    if (!sourceState || !targetWidget) {
        return resolvedNodeStableIds;
    }
    auto rs = std::dynamic_pointer_cast<ReuseState>(sourceState);
    if (!rs) {
        return resolvedNodeStableIds;
    }
    // Legacy no-snapshot overload: returns the Element-preorder stableId. Callers that need a
    // resolver-consumable index must use the snapshot-aware overload below, which matches the
    // widget against the live GUITree preorder by property (bounds + class + resource-id).
    const int sid = rs->getStableElementIdForWidget(targetWidget);
    if (sid >= 0) {
        resolvedNodeStableIds.push_back(sid);
    }
    return resolvedNodeStableIds;
}

/** @brief Finds the shared parent namelet index and widget xpath for `targetXPathName` consistent across two XML trees. */
bool apeResolveParentNameletAndWidgetXPath(const std::string &activity, const naming::NamingPtr &cur,
                                           const std::string &targetXPathName,
                                           const naming::NamerPtr &targetNameNamer,
                                           const std::string &xmlA, const std::string &xmlB,
                                           size_t *outParentIdx, std::string *outWidgetXPath,
                                           const gui_tree::GUITreePtr *preferSnapA = nullptr,
                                           const gui_tree::GUITreePtr *preferSnapB = nullptr) {
    if (!cur || targetXPathName.empty() || !targetNameNamer || !outParentIdx || !outWidgetXPath ||
        xmlA.empty() || xmlB.empty()) {
        return false;
    }
    std::string pkg;
    std::string cls;
    naming::StateKey::splitActivityPackageClass(activity, &pkg, &cls);

    auto evalOne = [&](const std::string &xml, naming::NameletPtr *outNl,
                       const gui_tree::GUITreePtr *preferSnap) -> bool {
        gui_tree::GUITreeBuildResult built =
            (preferSnap && *preferSnap)
                ? buildGuitreeFromTransitionSourcePreferSnapshot(xml, pkg, cls, *preferSnap)
                : buildGuitreeFromCachedXmlPreferElement(xml, pkg, cls);
        if (!built.tree || !built.dom) {
            return false;
        }
        if (!safeRebuildTree(cur, *built.tree, built.dom, "target_resolve")) {
            return false;
        }
        std::vector<gui_tree::GUITreeNode *> po;
        collectGUITreeNodesPreOrder(built.tree->getRootNode(), &po);
        naming::NameletPtr sharedNl;
        size_t matched = 0;
        for (gui_tree::GUITreeNode *node : po) {
            if (!node) {
                continue;
            }
            const naming::NamePtr nm = node->getXPathName();
            if (!nm || nm->toXPath() != targetXPathName) {
                continue;
            }
            ++matched;
            naming::NameletPtr nl = node->getCurrentNamelet();
            if (!nl) {
                return false;
            }
            if (!sharedNl) {
                sharedNl = nl;
            } else if (!apeNameletMatches(sharedNl, nl)) {
                return false;
            }
        }
        if (matched == 0 || !sharedNl) {
            return false;
        }
        if (!sharedNl->getNamerPtr() ||
            naming::compareNamer(*sharedNl->getNamerPtr(), *targetNameNamer) != 0) {
            return false;
        }
        if (outWidgetXPath->empty()) {
            *outWidgetXPath = targetXPathName;
        } else if (*outWidgetXPath != targetXPathName) {
            return false;
        }
        *outNl = std::move(sharedNl);
        return true;
    };

    naming::NameletPtr n1;
    naming::NameletPtr n2;
    if (!evalOne(xmlA, &n1, preferSnapA) || !evalOne(xmlB, &n2, preferSnapB)) {
        return false;
    }
    if (!apeNameletMatches(n1, n2)) {
        return false;
    }
    const ssize_t idx = apeFindNameletIndex(cur, n1);
    if (idx < 0) {
        return false;
    }
    *outParentIdx = static_cast<size_t>(idx);
    return !outWidgetXPath->empty();
}

/** @brief Resolves the target widget xpath (and optional namer) from stable IDs after rebuilding trees under `cur`. */
bool apeResolveTargetXPathNameLikeJava(const std::string &activity, const naming::NamingPtr &cur,
                                       const std::string &xml, const std::vector<int> &resolvedNodeStableIds,
                                       std::string *outTargetXPathName,
                                       naming::NamerPtr *outTargetNameNamer = nullptr,
                                       const gui_tree::GUITreePtr *preferSourceGuiSnapshot = nullptr) {
    if (!cur || !outTargetXPathName || xml.empty()) {
        return false;
    }
    std::string pkg;
    std::string cls;
    naming::StateKey::splitActivityPackageClass(activity, &pkg, &cls);
    gui_tree::GUITreeBuildResult built =
        (preferSourceGuiSnapshot && *preferSourceGuiSnapshot)
            ? buildGuitreeFromTransitionSourcePreferSnapshot(xml, pkg, cls, *preferSourceGuiSnapshot)
            : buildGuitreeFromCachedXmlPreferElement(xml, pkg, cls);
    if (!built.tree || !built.dom) {
        BDLOG("naming refine: target-name resolve build_failed activity=%s", activity.c_str());
        return false;
    }
    if (!safeRebuildTree(cur, *built.tree, built.dom, "target_resolve")) {
        BDLOG("naming refine: target-name resolve rebuild_failed activity=%s", activity.c_str());
        return false;
    }
    std::vector<gui_tree::GUITreeNode *> po;
    collectGUITreeNodesPreOrder(built.tree->getRootNode(), &po);
    if (resolvedNodeStableIds.empty()) {
        BDLOG("naming refine: target-name resolve node_miss activity=%s reason=resolved_nodes_missing",
              activity.c_str());
        return false;
    }
    std::unordered_set<int> sidSet;
    sidSet.reserve(resolvedNodeStableIds.size());
    for (int sid : resolvedNodeStableIds) {
        if (sid >= 0) {
            sidSet.insert(sid);
        }
    }
    if (sidSet.empty()) {
        BDLOG("naming refine: target-name resolve node_miss activity=%s reason=resolved_nodes_missing",
              activity.c_str());
        return false;
    }
    naming::NamePtr resolvedName;
    naming::NameletPtr resolvedNamelet;
    int firstHitStableId = -1;
    size_t validHits = 0;
    for (int stableId : sidSet) {
        if (stableId < 0 || static_cast<size_t>(stableId) >= po.size()) {
            BDLOG("naming refine: target-name resolve skip_oob_stable_id activity=%s stableId=%d preorder=%zu",
                  activity.c_str(), stableId, po.size());
            continue;
        }
        gui_tree::GUITreeNode *hit = po[static_cast<size_t>(stableId)];
        if (!hit) {
            BDLOG("naming refine: target-name resolve skip_null_node_at_stable_id activity=%s stableId=%d",
                  activity.c_str(), stableId);
            continue;
        }
        const naming::NamePtr nm = hit->getXPathName();
        const naming::NameletPtr nl = hit->getCurrentNamelet();
        if (!nm || !nm->getNamer() || !nl) {
            BDLOG("naming refine: target-name resolve skip_name_missing activity=%s stableId=%d node={%s}",
                  activity.c_str(), stableId, apeNodeDebugSummary(hit).c_str());
            continue;
        }
        if (!resolvedName) {
            resolvedName = nm;
            resolvedNamelet = nl;
            firstHitStableId = stableId;
            ++validHits;
            continue;
        }
        if (nameIdentityKey(resolvedName) != nameIdentityKey(nm) ||
            !apeNameletMatches(resolvedNamelet, nl)) {
            BDLOG("naming refine: target-name resolve node_miss activity=%s reason=resolved_nodes_inconsistent_name stableIds=%s",
                  activity.c_str(), apeJoinStableIds(resolvedNodeStableIds).c_str());
            return false;
        }
        ++validHits;
    }
    if (!resolvedName || validHits == 0) {
        BDLOG("naming refine: target-name resolve node_miss activity=%s reason=stable_id_out_of_preorder_range stableIds=%s preorder=%zu",
              activity.c_str(), apeJoinStableIds(resolvedNodeStableIds).c_str(), po.size());
        return false;
    }
    *outTargetXPathName = resolvedName->toXPath();
    if (outTargetNameNamer) {
        *outTargetNameNamer = resolvedName->getNamer();
    }
    BDLOG("naming refine: target-name resolved activity=%s targetName=%s stableId=%d stableCount=%zu",
          activity.c_str(), outTargetXPathName->c_str(), firstHitStableId, validHits);
    return !outTargetXPathName->empty();
}

/**
 * Shared-target check: whether any source GUI tree has more than one node matching
 * the same target Name.
 */
bool isSharedAction(
    const std::string &activity, const naming::NamingPtr &cur, const WidgetPtr &targetWidget,
    const std::vector<fastbotx::NondetTreeTransitionBranchPair::SourceTransition> &branchTransitions) {
    if (!cur || !targetWidget || branchTransitions.empty()) {
        return false;
    }
    std::string pkg;
    std::string cls;
    naming::StateKey::splitActivityPackageClass(activity, &pkg, &cls);
    for (size_t i = 0; i < branchTransitions.size(); ++i) {
        const auto &tt = branchTransitions[i];
        if (!tt.sourceGuiSnapshot) {
            BDLOG("naming refine: shared-check fail reason=cannot_align_missing_snapshot activity=%s branchIdx=%zu seq=%llu",
                  activity.c_str(), i, static_cast<unsigned long long>(tt.transitionSeq));
            continue;
        }
        if (tt.sourceXml.empty()) {
            BDLOG("naming refine: shared-check fail reason=transition_source_xml_missing activity=%s branchIdx=%zu seq=%llu",
                  activity.c_str(), i, static_cast<unsigned long long>(tt.transitionSeq));
            continue;
        }
        gui_tree::GUITreeBuildResult built =
            buildGuitreeFromTransitionSourcePreferSnapshot(tt.sourceXml, pkg, cls, tt.sourceGuiSnapshot);
        if (!built.tree || !built.dom) {
            BDLOG("naming refine: shared-check fail reason=build_failed activity=%s branchIdx=%zu seq=%llu",
                  activity.c_str(), i, static_cast<unsigned long long>(tt.transitionSeq));
            continue;
        }
        if (!safeRebuildTree(cur, *built.tree, built.dom, "shared_check")) {
            BDLOG("naming refine: shared-check fail reason=rebuild_failed activity=%s branchIdx=%zu seq=%llu",
                  activity.c_str(), i, static_cast<unsigned long long>(tt.transitionSeq));
            continue;
        }
        std::vector<gui_tree::GUITreeNode *> po;
        collectGUITreeNodesPreOrder(built.tree->getRootNode(), &po);
        gui_tree::GUITreeNode *hit = nullptr;
        const int hitIdx = apeFindGuiTreeNodePreorderIndexForWidget(po, targetWidget);
        if (hitIdx >= 0 && static_cast<size_t>(hitIdx) < po.size()) {
            hit = po[static_cast<size_t>(hitIdx)];
        }
        if (!hit) {
            BDLOG("naming refine: shared-check fail reason=target_node_unresolved activity=%s branchIdx=%zu seq=%llu",
                  activity.c_str(), i, static_cast<unsigned long long>(tt.transitionSeq));
            continue;
        }
        const naming::NamePtr targetName = hit->getXPathName();
        if (!targetName) {
            BDLOG("naming refine: shared-check fail reason=target_xpath_name_null activity=%s branchIdx=%zu seq=%llu node={%s}",
                  activity.c_str(), i, static_cast<unsigned long long>(tt.transitionSeq),
                  apeNodeDebugSummary(hit).c_str());
            continue;
        }
        const std::string targetNameKey = nameIdentityKey(targetName);
        size_t sameNameCount = 0;
        for (gui_tree::GUITreeNode *node : po) {
            if (!node) {
                continue;
            }
            const naming::NamePtr nm = node->getXPathName();
            if (!nm) {
                continue;
            }
            if (nameIdentityKey(nm) == targetNameKey) {
                ++sameNameCount;
                if (sameNameCount > 1) {
                    BDLOG("naming refine: shared-check hit activity=%s branchIdx=%zu seq=%llu name=%s sameNameCount=%zu",
                          activity.c_str(), i, static_cast<unsigned long long>(tt.transitionSeq),
                          targetName->toXPath().c_str(), sameNameCount);
                    return true;
                }
            }
        }
        BDLOG("naming refine: shared-check miss activity=%s branchIdx=%zu seq=%llu name=%s sameNameCount=%zu",
              activity.c_str(), i, static_cast<unsigned long long>(tt.transitionSeq),
              targetName->toXPath().c_str(), sameNameCount);
    }
    return false;
}

/** @brief Validates action refinement: distinct resolved target names per branch and state-count budget under `newNaming`. */
bool apeCheckActionRefinementLikeJava(const std::string &activity, const naming::NamingPtr &newNaming,
                                      const naming::NamerPtr &newNamer,
                                      const std::vector<fastbotx::NondetTreeTransitionBranchPair::SourceTransition>
                                          &tts1,
                                      const std::vector<fastbotx::NondetTreeTransitionBranchPair::SourceTransition>
                                          &tts2,
                                      std::vector<naming::NamerPtr> *upperBounds, size_t *outPart1 = nullptr,
                                      size_t *outPart2 = nullptr) {
    if (!newNaming || !newNamer || !upperBounds) {
        return false;
    }
    if (tts1.empty() || tts2.empty()) {
        return false;
    }
    std::string pkg;
    std::string cls;
    naming::StateKey::splitActivityPackageClass(activity, &pkg, &cls);

    auto resolveTransitionTargetName =
        [&](const char *branchTag, size_t branchIndex,
            const fastbotx::NondetTreeTransitionBranchPair::SourceTransition &tt,
            naming::NamePtr *outName) -> bool {
            if (tt.sourceXml.empty()) {
                BDLOG("naming refine: action-check fail reason=transition_source_xml_missing activity=%s seq=%llu",
                      activity.c_str(), static_cast<unsigned long long>(tt.transitionSeq));
                return false;
            }
            if (tt.resolvedNodeStableIds.empty()) {
                BDLOG("naming refine: action-check fail reason=resolved_nodes_missing activity=%s seq=%llu",
                      activity.c_str(), static_cast<unsigned long long>(tt.transitionSeq));
                return false;
            }
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
            if (!tt.sourceGuiSnapshot) {
                BDLOG("naming refine: action-check fail reason=cannot_align_missing_snapshot activity=%s branch=%s idx=%zu seq=%llu",
                      activity.c_str(), branchTag, branchIndex,
                      static_cast<unsigned long long>(tt.transitionSeq));
                return false;
            }
            gui_tree::GUITreeBuildResult built =
                buildGuitreeFromTransitionSourcePreferSnapshot(tt.sourceXml, pkg, cls, tt.sourceGuiSnapshot);
#else
            gui_tree::GUITreeBuildResult built = buildGuitreeFromCachedXmlPreferElement(tt.sourceXml, pkg, cls);
#endif
            if (!built.tree || !built.dom) {
                BDLOG("naming refine: action-check fail reason=source_tree_build_failed activity=%s branch=%s idx=%zu seq=%llu "
                      "srcStateHash=%zu targetStateHash=%zu stableIds=%s",
                      activity.c_str(), branchTag, branchIndex,
                      static_cast<unsigned long long>(tt.transitionSeq),
                      static_cast<size_t>(tt.sourceStateHash), static_cast<size_t>(tt.targetStateHash),
                      apeJoinStableIds(tt.resolvedNodeStableIds).c_str());
                return false;
            }
            if (!safeRebuildTree(newNaming, *built.tree, built.dom, "action_check")) {
                BDLOG("naming refine: action-check fail reason=source_tree_rebuild_failed activity=%s branch=%s idx=%zu seq=%llu "
                      "srcStateHash=%zu targetStateHash=%zu stableIds=%s",
                      activity.c_str(), branchTag, branchIndex,
                      static_cast<unsigned long long>(tt.transitionSeq),
                      static_cast<size_t>(tt.sourceStateHash), static_cast<size_t>(tt.targetStateHash),
                      apeJoinStableIds(tt.resolvedNodeStableIds).c_str());
                return false;
            }

            std::vector<gui_tree::GUITreeNode *> po;
            collectGUITreeNodesPreOrder(built.tree->getRootNode(), &po);
            naming::NamePtr nm;
            naming::NameletPtr nl;
            int firstHitStableId = -1;
            size_t validHits = 0;
            for (int stableId : tt.resolvedNodeStableIds) {
                if (stableId < 0 || static_cast<size_t>(stableId) >= po.size()) {
                    continue;
                }
                gui_tree::GUITreeNode *hit = po[static_cast<size_t>(stableId)];
                if (!hit) {
                    continue;
                }
                const naming::NamePtr curName = hit->getXPathName();
                const naming::NameletPtr curNl = hit->getCurrentNamelet();
                if (!curName || !curNl) {
                    continue;
                }
                if (!nm) {
                    nm = curName;
                    nl = curNl;
                    firstHitStableId = stableId;
                    ++validHits;
                    continue;
                }
                if (nameIdentityKey(nm) != nameIdentityKey(curName) ||
                    !apeNameletMatches(nl, curNl)) {
                    BDLOG("naming refine: action-check fail reason=resolved_nodes_inconsistent_name activity=%s branch=%s idx=%zu seq=%llu "
                          "srcStateHash=%zu targetStateHash=%zu stableIds=%s",
                          activity.c_str(), branchTag, branchIndex,
                          static_cast<unsigned long long>(tt.transitionSeq),
                          static_cast<size_t>(tt.sourceStateHash), static_cast<size_t>(tt.targetStateHash),
                          apeJoinStableIds(tt.resolvedNodeStableIds).c_str());
                    return false;
                }
                ++validHits;
            }
            if (!nm || validHits == 0) {
                BDLOG("naming refine: action-check fail reason=target_node_unresolved activity=%s branch=%s idx=%zu seq=%llu "
                      "srcStateHash=%zu targetStateHash=%zu stableIds=%s preorderSize=%zu",
                      activity.c_str(), branchTag, branchIndex,
                      static_cast<unsigned long long>(tt.transitionSeq),
                      static_cast<size_t>(tt.sourceStateHash), static_cast<size_t>(tt.targetStateHash),
                      apeJoinStableIds(tt.resolvedNodeStableIds).c_str(), po.size());
                return false;
            }
            const std::string xp = nm->toXPath();
            if (xp.empty()) {
                BDLOG("naming refine: action-check fail reason=target_xpath_name_empty activity=%s branch=%s idx=%zu seq=%llu "
                      "stableId=%d node={%s}",
                      activity.c_str(), branchTag, branchIndex,
                      static_cast<unsigned long long>(tt.transitionSeq), firstHitStableId, "-");
                return false;
            }
            BDLOG("naming refine: action-check target_name_resolved activity=%s branch=%s idx=%zu seq=%llu "
                  "stableId=%d stableCount=%zu sourceStateHash=%zu targetStateHash=%zu xpath=%s cacheKey=%s",
                  activity.c_str(), branchTag, branchIndex,
                  static_cast<unsigned long long>(tt.transitionSeq), firstHitStableId, validHits,
                  static_cast<size_t>(tt.sourceStateHash), static_cast<size_t>(tt.targetStateHash),
                  xp.c_str(), nm->cacheKeyString().c_str());
            *outName = nm;
            return true;
        };

    std::unordered_set<std::string> names;
    std::unordered_map<std::string, std::string> firstSeenByName;
    for (size_t i = 0; i < tts1.size(); ++i) {
        const auto &tt = tts1[i];
        naming::NamePtr name1;
        if (!resolveTransitionTargetName("A", i, tt, &name1)) {
            return false;
        }
        const std::string key1 = nameIdentityKey(name1);
        names.insert(key1);
        firstSeenByName.emplace(
            key1,
            std::string("branch=A idx=") + std::to_string(i) +
                " seq=" + std::to_string(static_cast<unsigned long long>(tt.transitionSeq)) +
                " srcStateHash=" + std::to_string(static_cast<size_t>(tt.sourceStateHash)) +
                " targetStateHash=" + std::to_string(static_cast<size_t>(tt.targetStateHash)) +
                " stableIds=" + apeJoinStableIds(tt.resolvedNodeStableIds));
    }
    for (size_t i = 0; i < tts2.size(); ++i) {
        const auto &tt = tts2[i];
        naming::NamePtr name2;
        if (!resolveTransitionTargetName("B", i, tt, &name2)) {
            return false;
        }
        const std::string key2 = nameIdentityKey(name2);
        if (!names.insert(key2).second) {
            std::string firstSeen = "unknown";
            const auto it = firstSeenByName.find(key2);
            if (it != firstSeenByName.end()) {
                firstSeen = it->second;
            }
            BDLOG("naming refine: action-check fail reason=name_collision activity=%s "
                  "collided=branch=B idx=%zu seq=%llu srcStateHash=%zu targetStateHash=%zu stableIds=%s "
                  "firstSeen={%s} xpath=%s cacheKey=%s",
                  activity.c_str(), i, static_cast<unsigned long long>(tt.transitionSeq),
                  static_cast<size_t>(tt.sourceStateHash), static_cast<size_t>(tt.targetStateHash),
                  apeJoinStableIds(tt.resolvedNodeStableIds).c_str(), firstSeen.c_str(),
                  name2 ? name2->toXPath().c_str() : "", name2 ? name2->cacheKeyString().c_str() : "");
            BDLOG("naming refine: action-check name_collision detail activity=%s newNamerMask=%u "
                  "newNamingFp=%s branchASize=%zu branchBSize=%zu uniqueNamesBeforeCollision=%zu",
                  activity.c_str(), newNamer ? newNamer->typeDimensionMask() : 0u,
                  newNaming ? newNaming->fingerprintString().c_str() : "-", tts1.size(), tts2.size(),
                  names.size());
            return false;
        }
    }
    if (names.empty()) {
        return false;
    }

    const int threshold = apeMaxStatesForRefinementThreshold(newNaming);
    std::unordered_set<uintptr_t> states;
    for (const auto &tt : tts1) {
        uintptr_t h = 0;
        if (!apeStateHashFromXmlWithNaming(activity, tt.sourceXml, newNaming, &h, 0, nullptr,
                                           &tt.sourceGuiSnapshot)) {
            return false;
        }
        states.insert(h);
        if (static_cast<int>(states.size()) > threshold) {
            upperBounds->push_back(newNamer);
            return false;
        }
    }
    for (const auto &tt : tts2) {
        uintptr_t h = 0;
        if (!apeStateHashFromXmlWithNaming(activity, tt.sourceXml, newNaming, &h, 0, nullptr,
                                           &tt.sourceGuiSnapshot)) {
            return false;
        }
        states.insert(h);
        if (static_cast<int>(states.size()) > threshold) {
            upperBounds->push_back(newNamer);
            return false;
        }
    }
    if (outPart1 && outPart2) {
        std::unordered_set<uintptr_t> h1;
        std::unordered_set<uintptr_t> h2;
        for (const auto &tt : tts1) {
            uintptr_t h = 0;
            if (!apeStateHashFromXmlWithNaming(activity, tt.sourceXml, newNaming, &h, 0, nullptr,
                                                &tt.sourceGuiSnapshot)) {
                return false;
            }
            h1.insert(h);
        }
        for (const auto &tt : tts2) {
            uintptr_t h = 0;
            if (!apeStateHashFromXmlWithNaming(activity, tt.sourceXml, newNaming, &h, 0, nullptr,
                                                &tt.sourceGuiSnapshot)) {
                return false;
            }
            h2.insert(h);
        }
        *outPart1 = h1.size();
        *outPart2 = h2.size();
    }
    upperBounds->push_back(newNamer);
    return true;
}

/** @brief Element-preorder stable IDs for the action target (legacy); not GUITree-preorder indices. */
std::vector<int> apeCollectResolvedNodeStableIds(const StatePtr &sourceState,
                                                 const ActivityStateActionPtr &action) {
    std::vector<int> ret;
    if (!sourceState || !action || !action->getTarget()) {
        return ret;
    }
    auto rs = std::dynamic_pointer_cast<ReuseState>(sourceState);
    if (!rs) {
        return ret;
    }
    // Legacy no-snapshot overload: returns the Element-preorder stableId only so callers that
    // use these ids purely for *ordering* (e.g. std::sort comparators) keep deterministic
    // behaviour. Any resolver-side indexing into a (pruned) GUITree must instead use the
    // snapshot-aware overload below, which produces GUITree-preorder indices.
    const int sid = rs->getStableElementIdForWidget(action->getTarget());
    if (sid >= 0) {
        ret.push_back(sid);
    }
    return ret;
}

#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
/** @brief GUITree-preorder index for the action target using property match against `liveSrcGuiTree`. */
std::vector<int> apeCollectResolvedNodeStableIds(const StatePtr &sourceState,
                                                 const ActivityStateActionPtr &action,
                                                 const gui_tree::GUITreePtr &liveSrcGuiTree) {
    std::vector<int> ret;
    if (!sourceState || !action || !action->getTarget() || !liveSrcGuiTree) {
        return ret;
    }
    if (!std::dynamic_pointer_cast<ReuseState>(sourceState)) {
        return ret;
    }
    std::vector<gui_tree::GUITreeNode *> preorder;
    collectGUITreeNodesPreOrder(liveSrcGuiTree->getRootNode(), &preorder);
    const WidgetPtr &widget = action->getTarget();
    int gidx = -1;
    {
        std::shared_ptr<Rect> wb = widget ? widget->getBounds() : nullptr;
        if (widget && wb) {
            const std::string &wClass = widget->getClassname();
            const std::string &wRid = widget->getResourceID();
            for (size_t i = 0; i < preorder.size(); ++i) {
                gui_tree::GUITreeNode *n = preorder[i];
                if (!n) {
                    continue;
                }
                const Rect &nb = n->getBounds();
                if (nb.left != wb->left || nb.top != wb->top ||
                    nb.right != wb->right || nb.bottom != wb->bottom) {
                    continue;
                }
                if (n->getClassName() != wClass) {
                    continue;
                }
                if (n->getResourceId() != wRid) {
                    continue;
                }
                gidx = static_cast<int>(i);
                break;
            }
        }
    }
    if (gidx >= 0) {
        ret.push_back(gidx);
        return ret;
    }
    // Emit a single attribution log for the transition-record miss so we can classify the
    // failure population. apeClassifyWidgetMatchMiss is a focused re-scan (no bound-stream
    // allocations beyond what the strict pass already touched).
    const ApeWidgetMatchMiss miss = apeClassifyWidgetMatchMiss(preorder, widget);
    std::shared_ptr<Rect> wbLog = widget ? widget->getBounds() : nullptr;
    const std::string wClassLog = widget ? widget->getClassname() : std::string();
    const std::string wRidLog = widget ? widget->getResourceID() : std::string();
    gui_tree::GUITreeNode *sampleBoundsOnly =
        (miss.firstBoundsOnlyIdx >= 0 && miss.firstBoundsOnlyIdx < static_cast<int>(preorder.size()))
            ? preorder[miss.firstBoundsOnlyIdx]
            : nullptr;
    gui_tree::GUITreeNode *sampleBoundsClass =
        (miss.firstBoundsClassIdx >= 0 && miss.firstBoundsClassIdx < static_cast<int>(preorder.size()))
            ? preorder[miss.firstBoundsClassIdx]
            : nullptr;
    BDLOG("naming resolved_nodes_miss preorderN=%zu bM=%d bcM=%d actionType=%d "
         "wBounds=[%d,%d] [%d,%d] wClass=%s wRid=%s "
         "sampleBoundsClassRid=%s sampleBoundsOnlyClass=%s sampleBoundsOnlyRid=%s",
         miss.preorderN, miss.boundsMatchCount, miss.boundsClassMatchCount,
         static_cast<int>(action->getActionType()),
         wbLog ? wbLog->left : -1, wbLog ? wbLog->top : -1,
         wbLog ? wbLog->right : -1, wbLog ? wbLog->bottom : -1,
         wClassLog.c_str(), wRidLog.c_str(),
         sampleBoundsClass ? sampleBoundsClass->getResourceId().c_str() : "-",
         sampleBoundsOnly ? sampleBoundsOnly->getClassName().c_str() : "-",
         sampleBoundsOnly ? sampleBoundsOnly->getResourceId().c_str() : "-");
    return ret;
}
#endif

/**
 * actionRefinement when actions are over-abstracted: one screen, multiple concrete widgets under the same
 * abstract target (native: _mergedWidgets). Requires distinct Name xpaths per concrete and bounds checks.
 */
bool apeCheckOverAbstractedActionRefinementLikeJava(const std::string &activity,
                                                    const naming::NamingPtr &newNaming,
                                                    const naming::NamerPtr &newNamer, const std::string &xml,
                                                    const StatePtr &sourceState,
                                                    const std::vector<WidgetPtr> &mergedConcretes,
                                                    int maxInitialNamesPerState,
                                                    std::vector<naming::NamerPtr> *upperBounds, size_t *outPart1,
                                                    size_t *outPart2,
                                                    const gui_tree::GUITreePtr *preferSourceGuiSnapshot = nullptr) {
    if (!newNaming || !newNamer || !upperBounds || xml.empty() || !sourceState) {
        return false;
    }
    if (mergedConcretes.size() < 2) {
        return false;
    }
    std::string pkg;
    std::string cls;
    naming::StateKey::splitActivityPackageClass(activity, &pkg, &cls);
    gui_tree::GUITreeBuildResult built =
        (preferSourceGuiSnapshot && *preferSourceGuiSnapshot)
            ? buildGuitreeFromTransitionSourcePreferSnapshot(xml, pkg, cls, *preferSourceGuiSnapshot)
            : buildGuitreeFromCachedXmlPreferElement(xml, pkg, cls);
    if (!built.tree || !built.dom) {
        return false;
    }
    if (!safeRebuildTree(newNaming, *built.tree, built.dom, "action_check")) {
        return false;
    }
    const naming::StateKey sk = naming::StateKey::fromGUITree(*built.tree);
    if (maxInitialNamesPerState > 0 &&
        static_cast<int>(sk.sortedXPaths().size()) > maxInitialNamesPerState) {
        return false;
    }
    const int stateBudget = apeMaxStatesForRefinementThreshold(newNaming);
    if (static_cast<int>(mergedConcretes.size()) > stateBudget) {
        upperBounds->push_back(newNamer);
        return false;
    }
    std::vector<gui_tree::GUITreeNode *> po;
    collectGUITreeNodesPreOrder(built.tree->getRootNode(), &po);
    std::unordered_set<std::string> seenNames;
    for (const WidgetPtr &w : mergedConcretes) {
        if (!w) {
            return false;
        }
        gui_tree::GUITreeNode *hit = nullptr;
        if (!apeFindNodeForTargetWidget(sourceState, w, po, &hit) || !hit) {
            return false;
        }
        const naming::NamePtr nm = hit->getXPathName();
        if (!nm) {
            return false;
        }
        const std::string xp = nm->toXPath();
        if (xp.empty() || !seenNames.insert(xp).second) {
            return false;
        }
    }
    if (outPart1) {
        *outPart1 = mergedConcretes.size();
    }
    if (outPart2) {
        *outPart2 = 0;
    }
    upperBounds->push_back(newNamer);
    return true;
}

} // namespace
#endif

namespace fastbotx {

namespace {
/** @brief Notifies the graph of a visit transition when the agent's current action is a model action with a target state. */
void fireGraphVisitStateTransitionIfModelAction(const GraphPtr &graph,
                                                const AbstractAgentPtr &agent,
                                                const StatePtr &targetState) {
    if (!graph || !agent || !targetState) {
        return;
    }
    StatePtr srcState = agent->getCurrentState();
    ActivityStateActionPtr act = agent->getCurrentAction();
    if (!srcState || !act || !act->isModelAct() || !act->requireTarget()) {
        return;
    }
    graph->notifyVisitStateTransition(srcState, act, targetState);
}
} // namespace

    /** @brief Returns the widget-key inclusion mask for this activity, or the default mask. */
    WidgetKeyMask Model::getActivityKeyMask(const std::string &activity) const {
        auto it = _activityKeyMask.find(activity);
        if (it != _activityKeyMask.end()) {
            return it->second;
        }
        return DefaultWidgetKeyMask;
    }

    /** @brief Returns the LLM client held by the task agent, or nullptr if none. */
    std::shared_ptr<LlmClient> Model::getLlmClient() const {
        return _llmTaskAgent ? _llmTaskAgent->getLlmClient() : nullptr;
    }

    /** @brief Stores a per-activity widget-key mask used when hashing widgets for state keys. */
    void Model::setActivityKeyMask(const std::string &activity, WidgetKeyMask mask) {
        _activityKeyMask[activity] = mask;
    }

    /**
     * @brief Log state information with each widget and action on a separate line
     * 
     * This helper function formats state information for debugging/logging purposes.
     * It prints the state hash, all widgets, and all actions in a readable format.
     * Long strings (>3000 chars) are split across multiple log lines.
     * 
     * @param state The state to log (nullptr is handled gracefully)
     */
    inline void logStatePerLine(const StatePtr &state) {
        if (state == nullptr) {
            BDLOGE("State is null, cannot log state information");
            return;
        }
        
        // Print state header with hash code
        BDLOG("{state: %lu", static_cast<unsigned long>(state->hash()));
        
        // Print each widget on a separate line for better readability; skip empty (e.g. toXPath returns "" when details cleared)
        BDLOG("widgets:");
        const auto &widgets = state->getWidgets();
        for (const auto &widget : widgets) {
            std::string widgetStr = widget->toString();
            if (widgetStr.empty()) continue;
            // If widget string is too long, split it across multiple log lines
            if (widgetStr.length() > 3000) {
                logLongStringInfo("   " + widgetStr);
            } else {
                BDLOG("   %s", widgetStr.c_str());
            }
        }
        
        // Print each action on a separate line for better readability
        BDLOG("action:");
        const auto &actions = state->getActions();
        for (const auto &action : actions) {
            std::string actionStr = action->toString();
            // If action string is too long, split it across multiple log lines
            if (actionStr.length() > 3000) {
                logLongStringInfo("   " + actionStr);
            } else {
                BDLOG("   %s", actionStr.c_str());
            }
        }
        
        BDLOG("}");
    }

    /**
     * @brief Factory method to create a new Model instance
     * 
     * Uses new + shared_ptr instead of make_shared because the constructor is protected
     * and make_shared cannot access protected constructors from outside the class.
     * 
     * @return Shared pointer to a new Model instance
     */
    std::shared_ptr<Model> Model::create() {
        return std::shared_ptr<Model>(new Model());
    }

    /**
     * @brief Constructor for Model class
     * 
     * Initializes the model with:
     * - A new Graph instance for state management
     * - Preference singleton instance
     * - Network action parameters set to default values
     */
    Model::Model() {
#ifndef FASTBOT_VERSION
    // Use build timestamp if available, otherwise use compile-time date/time
    #ifdef FASTBOT_BUILD_TIMESTAMP
        #define FASTBOT_VERSION FASTBOT_BUILD_TIMESTAMP
    #else
        // Fallback to compiler's __DATE__ and __TIME__ macros
        #define FASTBOT_VERSION __DATE__ " " __TIME__
    #endif
#endif
        BLOG("----Fastbot native code verison: 06052103, build version: " FASTBOT_VERSION "----\n");
        this->_graph = std::make_shared<Graph>();
        this->_preference = Preference::inst();
        this->_netActionParam.netActionTaskid = 0;

        // Initialize LLMTaskAgent with HTTP LLM client if LLM is enabled in config.
        LlmRuntimeConfig llmCfg;
        if (this->_preference) {
            llmCfg = this->_preference->getLlmRuntimeConfig();
        }
        std::shared_ptr<LlmClient> client = nullptr;
        if (llmCfg.enabled) {
            client = std::make_shared<HttpLlmClient>(llmCfg);
            BLOG("LLMTaskAgent: HTTP LLM client initialized with model %s", llmCfg.model.c_str());
        } else {
            BLOG("LLMTaskAgent: LLM is disabled in config");
        }
        this->_llmTaskAgent = std::make_shared<LLMTaskAgent>(this->_preference, client);
        this->_apeStateNamingManager = std::make_shared<naming::StateNamingManager>(nullptr);
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        this->_apeTransitionLog.resize(MaxTransitionLogSize);
        this->_apeTreeTransitionLog.resize(MaxTransitionLogSize);
        // TransitionLog upper bound implies a natural upper bound for distinct (srcKey,action) pairs.
        // Reserve once to reduce rehash/malloc on the hot path.
        this->_apePairAgg.reserve(MaxTransitionLogSize);
        this->_apeEvidencePools.reserve(MaxTransitionLogSize);
        // Fixed-size clock for bounded LRU-like eviction.
        constexpr size_t kEvidencePoolClockSize = MaxTransitionLogSize*4;
        this->_apeEvidencePoolClock.resize(kEvidencePoolClockSize);
        this->_apeEvidencePoolClockWriteIndex = 0;
        this->_apeEvidencePoolClockEvictIndex = 0;
        this->_apeEvidenceEpoch = 0;
        this->_apeTransitionSeq = 0;
        BLOG("state abstraction: enabled (reference-aligned event-driven refine/coarsen)");
#endif
    }


    /**
     * @brief General entry point for getting next operation step according to RL model
     * 
     * This is the main entry point that accepts XML content as a string.
     * It parses the XML string into an Element object and delegates to the
     * ElementPtr-based version of getOperate().
     * 
     * @param descContent XML content of the current page as a string
     * @param activity Activity name string
     * @param deviceID Device ID string (default: empty string uses default device)
     * @return Next operation step in JSON format, or empty string if parsing fails
     */
    std::string Model::getOperate(const std::string &descContent, const std::string &activity,
                                  const std::string &deviceID) {
        // Parse XML string into Element object using tinyxml2
        ElementPtr elem = Element::createFromXml(descContent);
        if (nullptr == elem) {
            return "";
        }
        // Delegate to ElementPtr-based version
        return this->getOperate(elem, activity, deviceID);
    }

    /**
     * @brief Create and add an agent to the model for a specific device
     * 
     * Creates a new agent using the AgentFactory, adds it to the device-agent map,
     * and registers it as a listener to the graph for state change notifications.
     * 
     * @param deviceIDString Device ID string (empty string uses default device ID)
     * @param agentType The type of algorithm/agent to create
     * @param deviceType The type of device (default: Normal)
     * @return Shared pointer to the newly created agent
     */
    AbstractAgentPtr Model::addAgent(const std::string &deviceIDString, AlgorithmType agentType,
                                     DeviceType deviceType) {
        // Create agent using factory pattern
        auto agent = AgentFactory::create(agentType, shared_from_this(), deviceType);
        
        // Use default device ID if empty string provided
        const std::string &deviceID = deviceIDString.empty() ? ModelConstants::DefaultDeviceID
                                                             : deviceIDString;
        
        // Add the device-agent pair to the map
        this->_deviceIDAgentMap.emplace(deviceID, agent);
        
        // Register agent as a listener to graph updates
        // This allows the agent to be notified when new states are added
        this->_graph->addListener(agent);
        
        return agent;
    }

    /** @brief Overload that ignores the legacy code-coverage flag and delegates to the primary addAgent(). */
    AbstractAgentPtr Model::addAgent(const std::string &deviceIDString, AlgorithmType agentType,
                                     bool useCodeCoverage, DeviceType deviceType) {
        (void) useCodeCoverage;
        return addAgent(deviceIDString, agentType, deviceType);
    }

    /**
     * @brief Get the agent for a specific device ID
     * 
     * @param deviceID Device ID string (empty string uses default device ID)
     * @return Shared pointer to the agent, or nullptr if not found
     */
    AbstractAgentPtr Model::getAgent(const std::string &deviceID) const {
        const std::string &d = deviceID.empty() ? ModelConstants::DefaultDeviceID : deviceID;
        auto iter = this->_deviceIDAgentMap.find(d);
        if (iter != this->_deviceIDAgentMap.end()) {
            return iter->second;
        }
        return nullptr;
    }


    /**
     * @brief Get next operation step from Element object, returning JSON string
     * 
     * This method wraps the core getOperateOpt() method and converts the result
     * to a JSON string format.
     * 
     * @param element XML Element object of the current page
     * @param activity Activity name string
     * @param deviceID Device ID string (default: empty string uses default device)
     * @return Next operation step in JSON format
     */
    std::string Model::getOperate(const ElementPtr &element, const std::string &activity,
                                  const std::string &deviceID) {
        OperatePtr operate = getOperateOpt(element, activity, deviceID);
        std::string operateString = operate->toString();
        return operateString;
    }


    /**
     * @brief Get custom action from preference if one exists for this page
     * 
     * Checks if the user has specified a custom action for this activity/page
     * in the preference settings. Returns nullptr if no custom action is defined.
     * 
     * @param activity Activity name string
     * @param element XML Element object of the current page
     * @return Custom action if exists, nullptr otherwise
     */
    /**
     * @brief Get or create an activity string pointer
     * 
     * This method optimizes memory usage by reusing existing activity string pointers
     * from the graph's visited activities set. If the activity already exists,
     * returns the cached shared pointer. Otherwise, creates a new one.
     * 
     * Performance optimization:
     * - Reuses existing string pointers to avoid duplicate string storage
     * - Uses hash-based set lookup for O(log n) complexity
     * 
     * @param activity The activity name string
     * @return Shared pointer to the activity string (cached or newly created)
     * 
     * @note The returned pointer may be from the cache or newly created.
     *       Newly created pointers will be added to the graph's visited activities
     *       when the state is added via createAndAddState().
     */
    stringPtr Model::getOrCreateActivityPtr(const std::string &activity) {
        // Get the set of visited activities (returns by value, but set is typically small)
        const stringPtrSet& activityStringPtrSet = this->_graph->getVisitedActivities();
        
        // Create temporary shared_ptr for lookup
        // Note: This creates a temporary object for comparison only
        // If not found, we'll return this pointer; if found, we'll return the cached one
        stringPtr tempActivityPtr = std::make_shared<std::string>(activity);
        
        // Try to find existing activity pointer in the set
        auto founded = activityStringPtrSet.find(tempActivityPtr);
        
        if (founded == activityStringPtrSet.end()) {
            // This is a new activity, return the newly created pointer
            return tempActivityPtr;
        } else {
            // Activity already exists, return the cached pointer to avoid duplication
            return *founded;
        }
    }

    /**
     * @brief Get or create an agent for the given device ID
     * 
     * This method retrieves an agent for the specified device ID. If no agent exists
     * for the device ID, returns the default agent. If no agents exist at all,
     * creates a default reuse agent.
     * 
     * Performance optimization:
     * - Uses find() instead of [] operator to avoid creating unnecessary map entries
     * - Falls back to default device ID if device ID is not found
     * 
     * @param deviceID The device ID string (empty string uses default device ID)
     * @return Shared pointer to the agent for the device
     * 
     * @note If the device ID is not found, returns the default agent instead of
     *       creating a new one. This ensures all devices have an agent to use.
     */
        AbstractAgentPtr Model::getOrCreateAgent(const std::string &deviceID) {
        // Create a default agent if map is empty
        if (this->_deviceIDAgentMap.empty()) {
            BLOG("%s", "use DoubleSarsaAgent as the default agent");
            this->addAgent(ModelConstants::DefaultDeviceID, AlgorithmType::DoubleSarsa);
        }
        
        // Use find() instead of [] to avoid creating unnecessary map entries
        // Performance: O(log n) lookup without side effects
        auto agentIterator = this->_deviceIDAgentMap.find(deviceID);
        
        if (agentIterator == this->_deviceIDAgentMap.end()) {
            // Device ID not found, return the default agent
            // Use find() again to avoid [] operator side effects
            auto defaultIterator = this->_deviceIDAgentMap.find(ModelConstants::DefaultDeviceID);
            if (defaultIterator != this->_deviceIDAgentMap.end()) {
                return defaultIterator->second;
            }
            // Should not reach here if addAgent worked correctly, but handle gracefully
            return nullptr;
        } else {
            // Found the agent for this device ID
            return agentIterator->second;
        }
    }

    /**
     * @brief Create a new state from element and add it to the graph
     * 
     * Creates a state object based on the agent's algorithm type, then adds it
     * to the graph. The graph will deduplicate if a similar state already exists.
     * Marks the state as visited with the current graph timestamp.
     * 
     * @param element XML Element object of the current page (must not be nullptr)
     * @param agent The agent to use for state creation (determines state type)
     * @param activityPtr Shared pointer to activity name string
     * @return Shared pointer to the created/existing state, or nullptr if element is null
     */
    StatePtr Model::buildStateOnly(const ElementPtr &element, const AbstractAgentPtr &agent,
                                   const stringPtr &activityPtr) {
        if (nullptr == element) {
            return nullptr;
        }
        std::string activityStr = activityPtr ? *activityPtr : "";
        WidgetKeyMask mask = getActivityKeyMask(activityStr);
        const std::string &xmlForLog = element->toXMLCached();
        const uint64_t xmlSigForLog = hashStringForLog(xmlForLog);
        StatePtr state = StateFactory::createState(agent->getAlgorithmType(), activityPtr, element, mask);
        static std::atomic<uint64_t> g_build_state_only{0};
        const uint64_t n = ++g_build_state_only;
        if (state && (n <= 20 || (n % 400) == 0)) {
            BLOG("naming state build: buildStateOnly source activity=%s seq=%" PRIu64
                 " elementPtr=%p xmlSig=%" PRIu64 " xmlLen=%zu %s",
                 activityStr.c_str(), n, element.get(), xmlSigForLog, xmlForLog.size(),
                 summarizeElementForLog(element).c_str());
            BLOG("naming state build: buildStateOnly activity=%s stateHash=%" PRIuPTR
                 " widgets=%zu actions=%zu widgetSummary=%s actionSummary=%s",
                 activityStr.c_str(), static_cast<uintptr_t>(state->hash()), state->getWidgetSize(),
                 state->getActions().size(), summarizeStateWidgetsForLog(state).c_str(),
                 summarizeStateActionsForLog(state).c_str());
        }
        return state;
    }

    /** @brief Builds a state from the UI element tree, inserts it into the graph, and returns it. */
    StatePtr Model::createAndAddState(const ElementPtr &element, const AbstractAgentPtr &agent,
                                      const stringPtr &activityPtr) {
        StatePtr state = buildStateOnly(element, agent, activityPtr);
        if (!state) return nullptr;
        state = this->_graph->addState(state);
        state->visit(this->_graph->getTimestamp());
        return state;
    }

    /**
     * @brief Select an action based on state, agent, and custom preferences
     * 
     * This method implements the action selection logic:
     * 1. Uses custom action from preference if available
     * 2. Checks for blocked state and returns RESTART if needed
     * 3. Otherwise, asks the agent to resolve a new action
     * 4. Updates agent strategy and marks action as visited if it's a model action
     * 
     * @param state The current state (may be modified)
     * @param agent The agent to use for action selection (may be modified)
     * @param customAction Custom action from preference, if any
     * @param actionCost Output parameter: time cost for action generation in seconds
     * @return Selected action, or nullptr if selection failed
     */
    ActionPtr Model::selectAction(StatePtr &state, AbstractAgentPtr &agent, ActionPtr customAction, double &actionCost) {
        double startGeneratingActionTimestamp = currentStamp();
        actionCost = 0.0;
        ActionPtr action = customAction; // Use custom action if provided

        const bool shouldSkipActionsFromModel =
            this->_preference ? this->_preference->skipAllActionsFromModel() : false;
        const char *activityLabel = "?";
        unsigned long long stateHashU = 0;
        size_t stateActionCount = 0;
        if (state) {
            stateHashU = static_cast<unsigned long long>(state->hash());
            stateActionCount = state->getActions().size();
            if (state->getActivityString() && state->getActivityString().get()) {
                activityLabel = state->getActivityString()->c_str();
            }
        }
        const int agentAlgo = agent ? static_cast<int>(agent->getAlgorithmType()) : -1;
        const int blockTimes = agent ? agent->getCurrentStateBlockTimes() : 0;
        BDLOG("selectAction: enter activity=%s stateHash=%llu graphStateId=%s stateActions=%zu "
              "agentAlgo=%d customXPath=%s listenSkipModel=%s blockTimes=%d",
              activityLabel,
              stateHashU,
              state ? state->getId().c_str() : "-",
              stateActionCount,
              agentAlgo,
              customAction ? "yes" : "no",
              shouldSkipActionsFromModel ? "yes" : "no",
              blockTimes);

        // Log state information for debugging
        logStatePerLine(state);

        if (shouldSkipActionsFromModel) {
            LOGI("listen mode skip get action from model");
        }

        // If no custom action specified and not in listen mode, get action from agent
        if (nullptr == customAction && !shouldSkipActionsFromModel) {
            // Check if we're in a blocked state and should restart
            if (-1 != BLOCK_STATE_TIME_RESTART &&
                -1 != Preference::inst()->getForceMaxBlockStateTimes() &&
                agent->getCurrentStateBlockTimes() > BLOCK_STATE_TIME_RESTART) {
                // Force restart action when stuck in blocked state
                action = Action::RESTART;
                BLOG("Ran into a block state %s blockTimes=%d restartThreshold=%d",
                     state ? state->getId().c_str() : "",
                     agent->getCurrentStateBlockTimes(),
                     BLOCK_STATE_TIME_RESTART);
            } else {
                // Ask agent to resolve a new action (this is the main RL model entry point)
                BDLOG("selectAction: calling agent->resolveNewAction() algo=%d", agentAlgo);
                auto resolvedAction = agent->resolveNewAction();
                action = std::dynamic_pointer_cast<Action>(resolvedAction);

                BDLOG("selectAction: resolveNewAction raw ptr=%s",
                      action ? action->toString().c_str() : "(null)");

                // Update agent's strategy based on the new action
                agent->updateStrategy();

                if (nullptr == action) {
                    BDLOGE("get null action!!!!");
                    return nullptr; // Handle null action gracefully
                }
            }

            // Calculate action generation time cost
            double endGeneratingActionTimestamp = currentStamp();
            actionCost = endGeneratingActionTimestamp - startGeneratingActionTimestamp;
        } else if (customAction) {
            BDLOG("selectAction: branch customXPath — skipping agent resolve/updateStrategy");
        } else if (shouldSkipActionsFromModel) {
            BDLOG("selectAction: branch listenSkipModel — agent not consulted (action may stay null)");
        }

        const char *how;
        if (customAction) {
            how = "custom_xpath";
        } else if (shouldSkipActionsFromModel) {
            how = "listen_skip_model";
        } else if (action && action->getActionType() == ActionType::RESTART) {
            how = "block_restart";
        } else {
            how = "agent";
        }
        BDLOG("selectAction: exit how=%s action=%s isModelAct=%d costMs=%.3f",
              how,
              action ? action->toString().c_str() : "(null)",
              (action && action->isModelAct()) ? 1 : 0,
              actionCost);

        return action;
    }

    /**
     * @brief Convert an action to an operate object and apply patches
     * 
     * Converts an Action object to a DeviceOperateWrapper (OperatePtr) that can be
     * executed. If the action requires a target widget, extracts widget information.
     * Applies preference patches and optionally clears state details for memory optimization.
     * 
     * @param action The action to convert (nullptr returns NOP operation)
     * @param state The current state (used for detail clearing optimization)
     * @return OperatePtr The operation object ready for execution
     */
    OperatePtr Model::convertActionToOperate(ActionPtr action, StatePtr state) {
        if (action == nullptr) {
            // Return no-operation if action is null
            return DeviceOperateWrapper::OperateNop;
        }

        BLOG("selected action %s", action->toString().c_str());
        
        // Convert action to operation object
        OperatePtr opt = action->toOperate();

        // If action requires a target widget, extract widget information
        if (action->requireTarget()) {
            if (auto stateAction = std::dynamic_pointer_cast<fastbotx::ActivityStateAction>(action)) {
                std::shared_ptr<Widget> widget = stateAction->getTarget();
                if (widget) {
                    // Serialize widget to JSON and attach to operation
                    std::string widget_str = widget->toJson();
                    opt->widget = widget_str;
                    BLOG("stateAction Widget: %s", widget_str.c_str());
                }
            }
        }

        // Apply preference patches to the operation (e.g., custom modifications)
        if (this->_preference) {
            this->_preference->patchOperate(opt);
        }

        // Memory optimization: clear state details after use if enabled
        // This reduces memory usage for states that are no longer needed in detail
        if (DROP_DETAIL_AFTER_SATE && state && !state->hasNoDetail()) {
            state->clearDetails();
        }

        return opt;
    }

    /**
     * @brief Core method for getting next operation step and updating RL model
     * 
     * This is the main orchestration method that:
     * 1. Gets custom action from preference if available
     * 2. Gets or creates activity pointer (memory optimization)
     * 3. Gets or creates agent for the device
     * 4. Creates and adds state to the graph
     * 5. Selects an action using the agent or custom action
     * 6. Converts action to operation object
     * 7. Logs performance metrics
     * 
     * @param element XML Element object of the current page
     * @param activity Activity name string
     * @param deviceID Device ID string (default: empty string uses default device)
     * @return DeviceOperateWrapper object containing the next operation to perform
     * 
     * @note This method updates the RL model by adding states and actions to the graph
     */
    OperatePtr Model::getOperateOpt(const ElementPtr &element, const std::string &activity,
                                    const std::string &deviceID) {
        // Record method start time for performance tracking
        double methodStartTimestamp = currentStamp();
        
        // Step 0: Match LLM task on raw tree (before resolvePage) so checkpoint matches unmodified UI.
        LlmTaskConfigPtr preMatchedLlmTask = nullptr;
        if (this->_preference && element) {
            preMatchedLlmTask = this->_preference->matchLlmTask(activity, element);
        }
        
        // Step 1: Resolve page (black widgets, tree pruning, valid texts) before using element.
        if (this->_preference && element) {
            this->_preference->resolvePage(activity, element);
        }
        // Step 2: Custom action from max.xpath.actions (if any) for this activity and page.
        ActionPtr customAction = (this->_preference && element)
            ? this->_preference->getCustomActionFromXpath(activity, element)
            : nullptr;
        
        // Step 3: Get or create activity pointer (reuses existing pointers for memory efficiency)
        stringPtr activityPtr = getOrCreateActivityPtr(activity);
        
        // Step 4: Get or create agent for this device (creates default if needed)
        AbstractAgentPtr agent = getOrCreateAgent(deviceID);
        if (agent) {
            // Default per-step state: not recovered; set true on graph-dedup recover-like paths.
            agent->setCurrentStateRecovered(false);
        }
        
        // Step 5: Build state, notify agent of transition (moveForward) before adding to graph, then add state
        // moveForward(currentState) must run before addState so agent still has previous _newState/_newAction
        // for (fromState, actionTaken, nextState) → updateKnowledge / AIG edges (see FIND_NAVIGATE_PATH_CODE_REVIEW §7).
        double buildStateStartTimestamp = currentStamp();
        StatePtr built = buildStateOnly(element, agent, activityPtr);
        StatePtr state = built;
        if (state) {
            // Cache XML string within this frame to avoid repeated Element::toXML() allocations.
            std::string xmlCache;
            auto getXml = [&]() -> const std::string & {
                if (xmlCache.empty()) {
                    xmlCache = element ? element->toXMLCached() : std::string();
                }
                return xmlCache;
            };
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
            naming::StateKey apeKey = naming::StateKey::fromParts(
                naming::StateKey::canonicalActivityString(activity), nullptr, {});
            bool haveApeKey = false;
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
            const bool wantApeRlIdentity = !_preference || !_preference->useStaticReuseAbstraction();
            const bool wantApeGraphDedup =
                _preference && _preference->useApeGraphDedupByStateKey();
            const bool wantApeStateKey = wantApeRlIdentity || wantApeGraphDedup;
            ApeStateKeyBuildFailReason buildFailReason = ApeStateKeyBuildFailReason::None;
            if (wantApeStateKey) {
                haveApeKey = buildApeStateKeyFromElementTree(
                    element, activity, &apeKey, &buildFailReason, wantApeRlIdentity ? built : StatePtr(), &xmlCache);
            } else {
                haveApeKey = false;
            }
            if (wantApeRlIdentity) {
                if (haveApeKey) {
                    built->applyDynamicAbstractionIdentityHash(apeKey.hash());
                } else {
                    BDLOG("naming statekey: skip identity update activity=%s reason=%d",
                          activity.c_str(), static_cast<int>(buildFailReason));
                }
            }
#else
            ApeStateKeyBuildFailReason buildFailReason = ApeStateKeyBuildFailReason::None;
            haveApeKey = buildApeStateKeyFromElementTree(element, activity, &apeKey, &buildFailReason);
#endif
            if (haveApeKey && _preference && _preference->useApeGraphDedupByStateKey()) {
                const uintptr_t kh = apeKey.hash();
                bool deduped = false;
                auto &bucket = _ape_graph_state_by_key[kh];
                for (const auto &entry : bucket) {
                    if (entry.key == apeKey) {
                        _ape_correctness_counters.graph_dedup_exact_hit++;
                        agent->setCurrentStateRecovered(true);
                        agent->moveForward(entry.state);
                        _graph->recordStateVisit(entry.state, built);
                        state = entry.state;
                        deduped = true;
                        break;
                    }
                }
                if (!deduped) {
                    if (!bucket.empty()) {
                        _ape_correctness_counters.graph_dedup_hash_collision++;
                        const uint64_t n = _ape_correctness_counters.graph_dedup_hash_collision;
                        if (n <= 3 || (n % 100) == 0) {
                            BLOG("naming graph dedup: hash collision activity=%s keyHash=%zu bucket=%zu",
                                 activity.c_str(), static_cast<size_t>(kh), bucket.size());
                        }
                    }
                    agent->moveForward(built);
                    state = _graph->addState(built);
                    bucket.push_back(ApeGraphStateKeyDedupEntry{apeKey, state});
                } else {
                    _ape_correctness_counters.graph_dedup_hash_hit++;
                }
            } else {
                agent->moveForward(built);
                state = _graph->addState(built);
            }
            if (haveApeKey) {
                recordApeStateKey(state, apeKey);
                logApeStateKeySnapshot(activity, state, apeKey, _graph);
            }
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
            if (element && _preference && !_preference->useStaticReuseAbstraction()) {
                const uintptr_t sh = state->hash();
                const std::string actKeyCanonical = naming::StateKey::canonicalActivityString(activity);
                _apeStateXmlByStateHash[sh] = getXml();
                if (element) {
                    _apeStateElementByStateHash[sh] = element;
                }
                apeMiniHistoryTouchState(actKeyCanonical, sh);
                constexpr size_t kMaxApeXmlCache = 2048;
                if (_apeStateXmlByStateHash.size() > kMaxApeXmlCache) {
                    // Q8 (local rebuild): protect mini-history referenced XML from cache eviction.
                    std::unordered_set<uintptr_t> protectedHashes;
                    protectedHashes.reserve(128);
                    for (const auto &kv : _apeMiniHistoryByActivity) {
                        const ApeMiniHistory &h = kv.second;
                        for (uintptr_t hsh : h.stateHashes) {
                            if (hsh != 0) {
                                protectedHashes.insert(hsh);
                            }
                        }
                        for (const auto &t : h.transitions) {
                            if (!t.valid) {
                                continue;
                            }
                            if (t.sourceStateHash != 0) {
                                protectedHashes.insert(t.sourceStateHash);
                            }
                            if (t.targetStateHash != 0) {
                                protectedHashes.insert(t.targetStateHash);
                            }
                        }
                    }
                    while (_apeStateXmlByStateHash.size() > kMaxApeXmlCache) {
                        auto itEvict = _apeStateXmlByStateHash.begin();
                        constexpr size_t kMaxProbe = 32;
                        size_t probe = 0;
                        while (itEvict != _apeStateXmlByStateHash.end() &&
                               protectedHashes.count(itEvict->first) != 0 &&
                               probe < kMaxProbe) {
                            ++itEvict;
                            ++probe;
                        }
                        if (itEvict == _apeStateXmlByStateHash.end()) {
                            itEvict = _apeStateXmlByStateHash.begin();
                        }
                        _apeStateElementByStateHash.erase(itEvict->first);
                        _apeStateXmlByStateHash.erase(itEvict);
                    }
                }
            }
#endif
#elif DYNAMIC_STATE_ABSTRACTION_ENABLED
            // No pugixml in this build: XPath/GUITree unavailable, skip dynamic StateKey identity update.
            naming::StateKey apeKey = naming::StateKey::fromParts(
                naming::StateKey::canonicalActivityString(activity), nullptr, {});
            bool haveApeKey = false;
            const bool wantApeRlIdentity = !_preference || !_preference->useStaticReuseAbstraction();
            std::vector<gui_tree::GUITreeNode *> guiPreOrder;
            if (wantApeRlIdentity) {
                (void)guiPreOrder;
                BDLOG("naming statekey: skip identity update activity=%s reason=no_pugixml", activity.c_str());
            }
            if (haveApeKey && _preference && _preference->useApeGraphDedupByStateKey()) {
                const uintptr_t kh = apeKey.hash();
                bool deduped = false;
                auto &bucket = _ape_graph_state_by_key[kh];
                for (const auto &entry : bucket) {
                    if (entry.key == apeKey) {
                        _ape_correctness_counters.graph_dedup_exact_hit++;
                        agent->setCurrentStateRecovered(true);
                        agent->moveForward(entry.state);
                        _graph->recordStateVisit(entry.state, built);
                        state = entry.state;
                        deduped = true;
                        break;
                    }
                }
                if (!deduped) {
                    if (!bucket.empty()) {
                        _ape_correctness_counters.graph_dedup_hash_collision++;
                        const uint64_t n = _ape_correctness_counters.graph_dedup_hash_collision;
                        if (n <= 3 || (n % 100) == 0) {
                            BLOG("naming graph dedup: hash collision activity=%s keyHash=%zu bucket=%zu",
                                 activity.c_str(), static_cast<size_t>(kh), bucket.size());
                        }
                    }
                    agent->moveForward(built);
                    state = _graph->addState(built);
                    bucket.push_back(ApeGraphStateKeyDedupEntry{apeKey, state});
                } else {
                    _ape_correctness_counters.graph_dedup_hash_hit++;
                }
            } else {
                agent->moveForward(built);
                state = _graph->addState(built);
            }
            if (haveApeKey) {
                recordApeStateKey(state, apeKey);
                logApeStateKeySnapshot(activity, state, apeKey, _graph);
            }
#else
            agent->moveForward(state);
            state = this->_graph->addState(state);
#endif
            if (state) {
                _apeLastScreenStateForValidate = state;
                if (agent) {
                    agent->validateAllNewActions(state);
                }
            }
        }
        // Count the previous step's model action only after we observe the next state (moveForward above).
        // Covers RL, xpath custom, and LLM-selected actions; keeps resolveAt's visitedCount % N aligned.
        if (state && agent) {
            const std::string devKey = deviceID.empty() ? ModelConstants::DefaultDeviceID : deviceID;
            auto pit = _pendingModelActionVisitByDevice.find(devKey);
            if (pit != _pendingModelActionVisitByDevice.end()) {
                ActionPtr pend = pit->second;
                if (pend && pend->isModelAct()) {
                    pend->visit(this->_graph->getTimestamp());
                }
                _pendingModelActionVisitByDevice.erase(pit);
            }
        }
        double buildStateEndTimestamp = currentStamp();
        bool fromLlm = (_llmTaskAgent && _llmTaskAgent->inSession());
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        auto runApeUnderAbstractedStateCheck = [&](const char *coarsenPhase) -> size_t {
            if (!_preference || !_preference->useApeEvolveModel()) {
                return 0;
            }

            const std::string actKeyCollected = naming::StateKey::canonicalActivityString(activity);
            auto itCtxCollected = _apeNamingContext.find(actKeyCollected);
            size_t collectedKeys = 0;
            size_t collectedObservations = 0;
            if (itCtxCollected != _apeNamingContext.end()) {
                collectedKeys = itCtxCollected->second.oldKeyHashToObservationCount.size();
                for (const auto &p : itCtxCollected->second.oldKeyHashToObservationCount) {
                    collectedObservations += p.second;
                }
            }

            if (_apeStateNamingManager) {
                naming::ActivityNamingManager &mgrGate = _apeStateNamingManager->activityManager();
                naming::NamingPtr curGate = mgrGate.getNaming(actKeyCollected);
                if (!curGate || !curGate->getParent()) {
                    BDLOG("naming: under-abstracted-check phase=%s activity=%s rollbacks=0 changed=0 "
                          "collected=%zu collectedObs=%zu skipped=root_no_parent fineness=%d",
                          coarsenPhase ? coarsenPhase : "?", activity.c_str(), collectedKeys,
                          collectedObservations, curGate ? curGate->getFineness() : -1);
                    return 0;
                }
            }

            bool changed = false;
            size_t rollbackCount = 0;
            while (true) {
                {
                    ApeNamingAbstractionContext &ctxProbe = _apeNamingContext[actKeyCollected];
                    static std::atomic<uint64_t> g_coarsen_callsite_probe{0};
                    const uint64_t cp = ++g_coarsen_callsite_probe;
                    if (cp <= 240 || (cp % 600) == 0) {
                        const uintptr_t triggerKeyHashFromStateKey =
                            ctxProbe.triggerSourceKeyExact ? ctxProbe.triggerSourceKey.hash() : 0;
                        BDLOG("naming BUG_PROBE [coarsen_callsite_ctx] phase=%s seq=%llu activity=%s "
                              "before_call=1 trigHashCtx=%lu trigExact=%d trigHashStateKey=%lu "
                              "equalCtxVsStateKey=%d old2newSize=%zu oldObsSize=%zu rollbackCount=%zu "
                              "lastRefineSeedSeq=%llu lastRefineSeedTrigHash=%lu",
                              coarsenPhase ? coarsenPhase : "?",
                              static_cast<unsigned long long>(cp), actKeyCollected.c_str(),
                              static_cast<unsigned long>(ctxProbe.triggerSourceKeyHash),
                              ctxProbe.triggerSourceKeyExact ? 1 : 0,
                              static_cast<unsigned long>(triggerKeyHashFromStateKey),
                              (ctxProbe.triggerSourceKeyExact &&
                               ctxProbe.triggerSourceKeyHash == triggerKeyHashFromStateKey) ? 1 : 0,
                              ctxProbe.oldKeyHashToNewKeyHashes.size(),
                              ctxProbe.oldKeyHashToObservationCount.size(),
                              rollbackCount,
                              static_cast<unsigned long long>(ctxProbe.lastRefineSeedSeq),
                              static_cast<unsigned long>(ctxProbe.lastRefineSeedTriggerHash));
                    }
                    if (ctxProbe.lastRefineSeedSeq == 0 || ctxProbe.triggerSourceKeyHash == 0) {
                        static std::atomic<uint64_t> g_coarsen_skip_unseeded_ctx{0};
                        const uint64_t su = ++g_coarsen_skip_unseeded_ctx;
                        const bool seq0 = ctxProbe.lastRefineSeedSeq == 0;
                        const bool trig0 = ctxProbe.triggerSourceKeyHash == 0;
                        const char *unseededDetail =
                            (seq0 && trig0)
                                ? "both_seq_and_trigger_zero"
                                : (seq0 ? "seq_zero_trigger_nonzero" : "trigger_zero_seq_nonzero");
                        if (su <= 200 || (su % 600) == 0) {
                            BDLOG("naming: coarsen skip activity=%s phase=%s reason=unseeded_context "
                                  "unseeded_detail=%s lastRefineSeedSeq=%llu trigHashCtx=%lu "
                                  "old2newSize=%zu oldObsSize=%zu note=ND_refine_seeds_ctx_after_recordTransition",
                                  actKeyCollected.c_str(), coarsenPhase ? coarsenPhase : "?",
                                  unseededDetail,
                                  static_cast<unsigned long long>(ctxProbe.lastRefineSeedSeq),
                                  static_cast<unsigned long>(ctxProbe.triggerSourceKeyHash),
                                  ctxProbe.oldKeyHashToNewKeyHashes.size(),
                                  ctxProbe.oldKeyHashToObservationCount.size());
                        }
                        break;
                    }
                }
                if (!coarsenActivityApeNamingIfNeeded(activity)) {
                    break;
                }
                _apeEventCoarsenRollbackCount++;
                rollbackCount++;
                changed = true;
            }

            if (changed) {
                pruneDivergentApeStatesForActivity(actKeyCollected);
                notifyAgentsOfApeNamingChange();
            }

            BDLOG("naming: under-abstracted-check phase=%s activity=%s rollbacks=%zu changed=%d "
                  "collected=%zu collectedObs=%zu",
                  coarsenPhase ? coarsenPhase : "?", activity.c_str(), rollbackCount, changed ? 1 : 0,
                  collectedKeys, collectedObservations);

            return rollbackCount;
        };

        const size_t underRollbacksPre = runApeUnderAbstractedStateCheck("pre_over_evolve");
        const size_t overRefinements =
            (_preference && _preference->useApeEvolveModel()) ? runApeOverAbstractedPreEvolvePhase(activity, state)
                                                             : static_cast<size_t>(0);
        const size_t underRollbacksPost = runApeUnderAbstractedStateCheck("post_over_evolve");

        if (state) {
            state->visit(this->_graph->getTimestamp());
        }
        recordTransition(agent, state);
        BDLOG("naming: evolve-step activity=%s underRollbacks_pre=%zu overRefine=%zu underRollbacks_post=%zu",
              activity.c_str(), underRollbacksPre, overRefinements, underRollbacksPost);
        if (state && state->getVisitedCount() == 0) {
            state->visit(this->_graph->getTimestamp());
            BDLOG("naming: state_visit_retry activity=%s hash=%zu",
                  activity.c_str(), static_cast<size_t>(state->hash()));
        }
#endif
#if !DYNAMIC_STATE_ABSTRACTION_ENABLED
        if (state) {
            state->visit(this->_graph->getTimestamp());
        }
        fireGraphVisitStateTransitionIfModelAction(this->_graph, agent, state);
#endif
        // Planner pipeline hook runs after addState + recordTransition. When DYNAMIC_STATE_ABSTRACTION_ENABLED,
        // state visit runs after preEvolve and before recordTransition (same ordering as reference tooling).
        // processState must not mutate StateKey or Graph dedup keys. Gated by max.llm.llmdroid at Model layer.
        {
            const PreferencePtr pref = _preference ? _preference : Preference::inst();
            if (pref && pref->isLlmdroidEnabled() && state && agent) {
                if (ReuseStatePtr reuseState = std::dynamic_pointer_cast<ReuseState>(state)) {
                    agent->processState(reuseState);
                }
            }
        }
        // Step 5b: Removed — screenshots are handled by the host/runtime pipeline when calling LLM helpers.
        // Native no longer returns NOP solely because screenshot bytes are empty when the host supplies them.
        // Step 6: Optionally delegate to LLMTaskAgent before RL (pass pre-matched task from raw tree).
        ActionPtr llmAction = nullptr;
        if (this->_llmTaskAgent) {
            llmAction = this->_llmTaskAgent->selectNextAction(element, activity, deviceID, preMatchedLlmTask);
        }

        // Step 7: Select action (either LLM, custom, restart, or from agent)
        double actionCost = 0.0;
        ActionPtr action;
        if (llmAction) {
            // When LLMTaskAgent returns an action, we bypass RL for this step.
            action = llmAction;
        } else {
            action = selectAction(state, agent, customAction, actionCost);
        }
        
        // Handle null action gracefully
        if (nullptr == action) {
            return DeviceOperateWrapper::OperateNop;
        }

        // Resolve merged widgets: when multiple concrete nodes share the same abstract widget,
        // set action target to the next concrete node (visitCount % total) so each selection hits a different node (e.g. 特价→首页→秒送→新品).
        if (state && action && action->requireTarget()) {
            if (auto stateAction = std::dynamic_pointer_cast<ActivityStateAction>(action)) {
                state->resolveAt(stateAction, _graph->getTimestamp());
            }
        }

        // Step 8: Convert action to operation object and apply patches
        OperatePtr opt = convertActionToOperate(action, state);
        if (llmAction) {
            opt->allowFuzzing = false;
        }
        const PreferencePtr prefLlmdroid = _preference ? _preference : Preference::inst();
        if (prefLlmdroid && prefLlmdroid->isLlmdroidEnabled() && state && action) {
            if (ReuseStatePtr rs = std::dynamic_pointer_cast<ReuseState>(state)) {
                rs->_actionToPerform = action;
            }
        }
        // Optional: agent-provided LLM-generated input text (e.g. LLMExplorerAgent content-aware input)
        if (agent) {
            std::string agentInputText = agent->getInputTextForAction(state, action);
            if (!agentInputText.empty()) {
                opt->setText(agentInputText);
            }
        }
        if (auto asa = std::dynamic_pointer_cast<ActivityStateAction>(action)) {
            if (!asa->getInputText().empty()) {
                opt->setText(asa->getInputText());
            }
        }

        // Record end time and log performance metrics (currentStamp returns ms, keep ms for log)
        double methodEndTimestamp = currentStamp();
        double buildStateCostMs = buildStateEndTimestamp - buildStateStartTimestamp;
        double actionCostMs = actionCost;
        double totalCostMs = methodEndTimestamp - methodStartTimestamp;
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        if (state && state->usesDynamicAbstractionIdentityHash()) {
            BLOG("build state cost: %.3fms action cost: %.3fms total cost: %.3fms dims=[Ape abstraction]",
                 buildStateCostMs,
                 actionCostMs,
                 totalCostMs);
        } else {
            BLOG("build state cost: %.3fms action cost: %.3fms total cost: %.3fms dims=[%s]",
                 buildStateCostMs,
                 actionCostMs,
                 totalCostMs,
                 maskToDimensionString(getActivityKeyMask(activity)).c_str());
        }
#else
        BLOG("build state cost: %.3fms action cost: %.3fms total cost: %.3fms",
             buildStateCostMs,
             actionCostMs,
             totalCostMs);
#endif
        if (action && action->isModelAct()) {
            const std::string devKey = deviceID.empty() ? ModelConstants::DefaultDeviceID : deviceID;
            _pendingModelActionVisitByDevice[devKey] = action;
        }
        return opt;
    }

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
    /** @brief Runs dynamic abstraction / naming refinement during the pre-evolve phase for one activity. */
    size_t Model::runApeOverAbstractedPreEvolvePhase(const std::string &activity, const StatePtr &state) {
        if (!_preference || !_preference->useApeEvolveModel()) {
            return 0;
        }
        if (!_apeStateNamingManager || !state) {
            return 0;
        }

        const std::string actKey = naming::StateKey::canonicalActivityString(activity);
        naming::ActivityNamingManager &mgr = _apeStateNamingManager->activityManager();

#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
        const auto rs = std::dynamic_pointer_cast<ReuseState>(state);
        const auto itXml = _apeStateXmlByStateHash.find(state->hash());
        if (rs && itXml != _apeStateXmlByStateHash.end() && !itXml->second.empty()) {
            const std::string &screenXml = itXml->second;
            naming::NamerLattice lat(naming::NamerFactory::current());
            std::set<std::string> blk;
            for (const auto &p : _apeNamingCoarseningBlacklist) {
                if (p.first == actKey) {
                    blk.insert(p.second);
                }
            }

            const int arThresh = _preference ? _preference->getApeActionRefinementThreshold() : 3;
            const int maxInitial =
                _preference ? _preference->getApeMaxInitialNamesPerState() : 20;
            const int apeMaxStatesPerActivity =
                _preference ? _preference->getApeMaxStatesPerActivity() : 10;
            const int apeMaxGUITreesPerState =
                _preference ? _preference->getApeMaxGuitreesPerState() : 20;
            const bool enableReplacingNameLet =
                _preference && _preference->useApeNamingEnableReplacingNamelet();

            bool activitySizeGateLogged = false;

            auto gateActivitySizes = [&]() -> bool {
                const size_t g = apeGraphActivityStateCountLikeJavaActivityNode(_graph, actKey);
                if (static_cast<int>(g) > apeMaxStatesPerActivity) {
                    if (!activitySizeGateLogged) {
                        const naming::NamingPtr cur = mgr.getNaming(actKey);
                        const size_t activityStateCount = getApeStateCountByActivityAndNamingFingerprint(
                            actKey, cur ? cur->fingerprintString() : std::string());
                        BDLOG("naming: skip pre-evolve refine activity=%s reason=maxStatesPerActivity graphStates=%zu max=%d",
                              activity.c_str(), g, apeMaxStatesPerActivity);
                        BDLOG("naming: maxStatesPerActivity detail activity=%s namingFingerprint=%s namingScopedStates=%zu",
                              activity.c_str(), cur ? cur->fingerprintString().c_str() : "null",
                              activityStateCount);
                        activitySizeGateLogged = true;
                    }
                    return false;
                }
                size_t guitreesForState = 0;
                auto itSn = _apeGuiTreeSnapshotsByStateHash.find(state->hash());
                if (itSn != _apeGuiTreeSnapshotsByStateHash.end()) {
                    guitreesForState = itSn->second.size();
                }
                if (guitreesForState == 0) {
                    size_t n = 0;
                    for (const auto &te : _apeTransitionLog) {
                        if (te.valid && te.sourceStateHash == state->hash()) {
                            ++n;
                        }
                    }
                    if (n > 0) {
                        guitreesForState = n;
                    }
                }
                if (static_cast<int>(guitreesForState) > apeMaxGUITreesPerState) {
                    if (!activitySizeGateLogged) {
                        const naming::NamingPtr cur = mgr.getNaming(actKey);
                        const size_t activityStateCount = getApeStateCountByActivityAndNamingFingerprint(
                            actKey, cur ? cur->fingerprintString() : std::string());
                        BDLOG("naming: skip pre-evolve refine activity=%s reason=maxGUITreesPerState guitreesInState=%zu max=%d",
                              activity.c_str(), guitreesForState, apeMaxGUITreesPerState);
                        BDLOG("naming: maxGUITreesPerState detail activity=%s namingFingerprint=%s namingScopedStates=%zu",
                              activity.c_str(), cur ? cur->fingerprintString().c_str() : "null",
                              activityStateCount);
                        activitySizeGateLogged = true;
                    }
                    return false;
                }
                return true;
            };

            auto tryRefineOneAction = [&](const ActivityStateActionPtr &asa) -> bool {
                if (!asa || !asa->requireTarget() || !asa->isModelAct() || !asa->getTarget()) {
                    return false;
                }
                const WidgetPtr tw = asa->getTarget();
                const uint64_t blkKey =
                    (static_cast<uint64_t>(state->hash()) << 32) ^
                    (static_cast<uint64_t>(static_cast<uint32_t>(tw->hash())) << 1);
                if (_apeOverAbstractedPreEvolveActionBlacklist.count(blkKey) != 0) {
                    return false;
                }
                if (!gateActivitySizes()) {
                    return false;
                }

                const WidgetPtrVec *mergedVec = state->getMergedTargetsIfAny(tw);
                if (!mergedVec || mergedVec->empty()) {
                    return false;
                }

                std::vector<WidgetPtr> mergedConcretes(mergedVec->begin(), mergedVec->end());
                if (static_cast<int>(mergedConcretes.size()) <= arThresh) {
                    return false;
                }

                std::string candidate_source = "unknown";
                naming::NamingPtr cur = mgr.getNaming(actKey);
                if (!cur) {
                    cur = naming::NamingFactory::defaultRootNaming();
                    if (!cur) {
                        return false;
                    }
                    candidate_source = "defaultRootNaming";
                    {
                        static std::atomic<uint64_t> g_updateNaming_defaultRoot_diag{0};
                        const uint64_t ud = ++g_updateNaming_defaultRoot_diag;
                        if (ud <= 20 || (ud % 200) == 0) {
                            const naming::NamingPtr oldN = nullptr;
                            const naming::NamingPtr newN = cur;
                            const naming::NamingPtr oldPar = nullptr;
                            const naming::NamingPtr newPar = newN ? newN->getParent() : nullptr;
                            const int direct_child = 0;
                            const int sibling_share_parent = 0;
                            const std::string oldFp = "";
                            const std::string newFp =
                                newN ? newN->fingerprintString() : std::string("-");
                            BDLOG(
                                "naming diag [Model.updateNaming-enter Refine] seq=%llu act=%s "
                                "old=%p new=%p oldPar=%p newPar=%p direct_child=%d sibling_share_parent=%d "
                                "candidate_source=%s oldFin=%d newFin=%d old_fp=%s new_fp=%s",
                                static_cast<unsigned long long>(ud), actKey.c_str(),
                                static_cast<const void *>(oldN.get()), static_cast<const void *>(newN.get()),
                                static_cast<const void *>(oldPar.get()), static_cast<const void *>(newPar.get()),
                                direct_child, sibling_share_parent, candidate_source.c_str(),
                                oldN ? oldN->getFineness() : -1, newN ? newN->getFineness() : -1,
                                oldFp.c_str(), newFp.c_str());
                        }
                    }
                    _apeStateNamingManager->updateNaming(actKey, naming::NamingUpdateKind::Refine, cur);
                    pruneDivergentApeStatesForActivity(actKey);
                }

                std::string pkg;
                std::string cls;
                naming::StateKey::splitActivityPackageClass(activity, &pkg, &cls);
                gui_tree::GUITreePtr apeVisitSnap = this->apeLatestGuiTreeSnapshot(state->hash());
                const gui_tree::GUITreePtr *apeVisitSnapPtr = apeVisitSnap ? &apeVisitSnap : nullptr;
                gui_tree::GUITreeBuildResult builtProbe =
                    (apeVisitSnapPtr && *apeVisitSnapPtr)
                        ? buildGuitreePreferApeSnapshotAndDomXml(screenXml, pkg, cls, *apeVisitSnapPtr)
                        : buildGuitreeFromCachedXmlPreferElement(screenXml, pkg, cls);
                if (!builtProbe.tree || !builtProbe.dom) {
                    return false;
                }
                if (!safeRebuildTree(cur, *builtProbe.tree, builtProbe.dom, "action_check")) {
                    return false;
                }

                const naming::StateKey skProbe = naming::StateKey::fromGUITree(*builtProbe.tree);
                if (maxInitial > 0 &&
                    static_cast<int>(skProbe.sortedXPaths().size()) > maxInitial) {
                    return false;
                }

                std::string targetXPathName;
                naming::NamerPtr targetNameNamer;
                const std::vector<int> targetStableIds =
                    apeResolveStableIdsForTargetWidgetLikeJava(state, tw);
                if (!apeResolveTargetXPathNameLikeJava(activity, cur, screenXml, targetStableIds,
                                                       &targetXPathName, &targetNameNamer,
                                                       apeVisitSnapPtr) ||
                    !targetNameNamer) {
                    return false;
                }

                size_t pIdx = 0;
                std::string wxp;
                if (!apeResolveParentNameletAndWidgetXPath(activity, cur, targetXPathName, targetNameNamer,
                                                           screenXml, screenXml, &pIdx, &wxp,
                                                           apeVisitSnapPtr, apeVisitSnapPtr)) {
                    return false;
                }

                if (pIdx >= cur->getNamelets().size()) {
                    return false;
                }

                naming::NameletPtr anchorNL = cur->getNamelets()[pIdx];
                naming::NamerPtr curNam = anchorNL ? anchorNL->getNamerPtr() : nullptr;
                if (!anchorNL || !curNam) {
                    return false;
                }

                std::vector<naming::NamerPtr> upperBounds;
                std::unordered_set<std::string> acceptedFp;
                const std::string fpCur = cur->fingerprintString();

                auto acceptChild = [&](naming::NamingPtr child,
                                       const naming::NamerPtr &refined) -> bool {
                    if (!child) {
                        return false;
                    }
                    if (blk.count(child->fingerprintString()) != 0) {
                        return false;
                    }
                    size_t p1 = 0;
                    size_t p2 = 0;
                    if (!apeCheckOverAbstractedActionRefinementLikeJava(
                            activity, child, refined, screenXml, state, mergedConcretes, maxInitial,
                            &upperBounds, &p1, &p2, apeVisitSnapPtr)) {
                        return false;
                    }
                    const std::string cfp = child->fingerprintString();
                    if (!acceptedFp.insert(cfp).second) {
                        return false;
                    }
                    if (cfp == fpCur) {
                        return false;
                    }

#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
                    {
                        std::vector<int> overAbsStableIds;
                        gui_tree::GUITreeBuildResult bPred =
                            (apeVisitSnapPtr && *apeVisitSnapPtr)
                                ? buildGuitreePreferApeSnapshotAndDomXml(screenXml, pkg, cls, *apeVisitSnapPtr)
                                : buildGuitreeFromCachedXmlPreferElement(screenXml, pkg, cls);
                        if (bPred.tree && bPred.dom &&
                            safeRebuildTree(child, *bPred.tree, bPred.dom)) {
                            std::vector<gui_tree::GUITreeNode *> poPred;
                            collectGUITreeNodesPreOrder(bPred.tree->getRootNode(), &poPred);
                            overAbsStableIds.reserve(mergedConcretes.size());
                            for (const WidgetPtr &w : mergedConcretes) {
                                if (!w) {
                                    continue;
                                }
                                gui_tree::GUITreeNode *hit = nullptr;
                                if (apeFindNodeForTargetWidget(state, w, poPred, &hit) && hit) {
                                    for (size_t i = 0; i < poPred.size(); ++i) {
                                        if (poPred[i] == hit) {
                                            overAbsStableIds.push_back(static_cast<int>(i));
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                        if (!overAbsStableIds.empty()) {
                            addApeActionRefinementPredicate(activity, state->hash(), screenXml,
                                                            overAbsStableIds, child);
                        }
                    }
#endif

                    bool rejectRefineCandidate = false;
                    {
                        const naming::NamingPtr oldN = cur;
                        const naming::NamingPtr newN = child;
                        const naming::NamingPtr oldPar = oldN ? oldN->getParent() : nullptr;
                        const naming::NamingPtr newPar = newN ? newN->getParent() : nullptr;
                        const int direct_child = (oldN && newPar && newPar == oldN) ? 1 : 0;
                        const int sibling_share_parent =
                            (oldPar && newPar && oldPar == newPar) ? 1 : 0;
                        static std::atomic<uint64_t> g_updateNaming_refine_entry_diag{0};
                        const uint64_t ud = ++g_updateNaming_refine_entry_diag;
                        if (ud <= 80 || direct_child == 0) {
                            const std::string oldFp = oldN ? oldN->fingerprintString() : std::string();
                            const std::string newFp = newN ? newN->fingerprintString() : std::string();
                            BLOG(
                                "naming diag [Model.updateNaming-enter Refine] seq=%llu act=%s "
                                "old=%p new=%p oldPar=%p newPar=%p direct_child=%d sibling_share_parent=%d "
                                "candidate_source=%s oldFin=%d newFin=%d old_fp=%s new_fp=%s",
                                static_cast<unsigned long long>(ud), actKey.c_str(),
                                static_cast<const void *>(oldN.get()), static_cast<const void *>(newN.get()),
                                static_cast<const void *>(oldPar.get()), static_cast<const void *>(newPar.get()),
                                direct_child, sibling_share_parent, candidate_source.c_str(),
                                oldN ? oldN->getFineness() : -1, newN ? newN->getFineness() : -1,
                                oldFp.c_str(), newFp.c_str());
                        }
                        if (direct_child == 0 && candidate_source == "extendUnderNamelet") {
                            static std::atomic<uint64_t> g_extend_parent_mismatch_diag{0};
                            const uint64_t md = ++g_extend_parent_mismatch_diag;
                            BLOG("naming diag [Model.extendUnderNamelet-parentMismatch] seq=%llu act=%s "
                                 "old=%p new=%p oldPar=%p newPar=%p sibling_share_parent=%d oldFin=%d newFin=%d",
                                 static_cast<unsigned long long>(md), actKey.c_str(),
                                 static_cast<const void *>(oldN.get()),
                                 static_cast<const void *>(newN.get()),
                                 static_cast<const void *>(oldPar.get()),
                                 static_cast<const void *>(newPar.get()),
                                 sibling_share_parent, oldN ? oldN->getFineness() : -1,
                                 newN ? newN->getFineness() : -1);
                            static std::atomic<uint64_t> g_extend_parent_mismatch_reject_diag{0};
                            const uint64_t rd = ++g_extend_parent_mismatch_reject_diag;
                            BLOG("naming diag [Model.extendUnderNamelet-parentMismatch-reject] seq=%llu "
                                 "act=%s old=%p new=%p",
                                 static_cast<unsigned long long>(rd), actKey.c_str(),
                                 static_cast<const void *>(oldN.get()),
                                 static_cast<const void *>(newN.get()));
                            rejectRefineCandidate = true;
                        }
                    }
                    if (rejectRefineCandidate) {
                        return false;
                    }
                    _apeStateNamingManager->updateNaming(actKey, naming::NamingUpdateKind::Refine,
                                                         std::move(child));
                    invalidateApeGraphStateKeyDedupMap();
                    pruneDivergentApeStatesForActivity(actKey);
                    notifyAgentsOfApeNamingChange();
                    return true;
                };

                if (enableReplacingNameLet && cur->hasChild()) {
                    const auto &cn = cur->getNamelets();
                    if (!cn.empty() && pIdx + 1 == cn.size() && cur->isReplaceable(anchorNL)) {
                        naming::NameletPtr parNL = anchorNL->getParent();
                        if (parNL && parNL->getNamerPtr()) {
                            std::vector<naming::NamerPtr> upperRepl;
                            for (const naming::NamerPtr &refined : lat.sortedAbove(parNL->getNamerPtr())) {
                                if (!refined) {
                                    continue;
                                }
                                if (anchorNL->getNamer().refinesTo(*refined)) {
                                    continue;
                                }

                                bool skipByUpper = false;
                                for (const naming::NamerPtr &ub : upperRepl) {
                                    if (ub && refined->refinesTo(*ub)) {
                                        skipByUpper = true;
                                        break;
                                    }
                                }
                                if (skipByUpper) {
                                    continue;
                                }

                                candidate_source = "replaceLast";
                                naming::NamingPtr child =
                                    naming::NamingFactory::replaceLast(cur, anchorNL, refined);
                                if (acceptChild(std::move(child), refined)) {
                                    return true;
                                }
                            }
                        }
                    }
                }

                for (const naming::NamerPtr &refined : lat.sortedAbove(curNam)) {
                    if (!refined) {
                        continue;
                    }

                    bool skipByUpper = false;
                    for (const naming::NamerPtr &ub : upperBounds) {
                        if (ub && refined->refinesTo(*ub)) {
                            skipByUpper = true;
                            break;
                        }
                    }
                    if (skipByUpper) {
                        continue;
                    }

                    candidate_source = "extendUnderNamelet";
                    naming::NamingPtr child =
                        naming::NamingFactory::extendUnderNamelet(cur, pIdx, wxp, refined);
                    if (acceptChild(std::move(child), refined)) {
                        return true;
                    }
                }

                _apeOverAbstractedPreEvolveActionBlacklist.insert(blkKey);
                return false;
            };

            auto innerPass = [&]() -> bool {
                ActivityStateActionPtrVec raw = state->targetActions();
                std::vector<ActivityStateActionPtr> acts;
                acts.reserve(raw.size());
                for (const auto &a : raw) {
                    auto asa = std::dynamic_pointer_cast<ActivityStateAction>(a);
                    if (!asa || !asa->requireTarget() || !asa->isModelAct()) {
                        continue;
                    }
                    acts.push_back(asa);
                }

                std::sort(acts.begin(), acts.end(),
                          [&](const ActivityStateActionPtr &a, const ActivityStateActionPtr &b) {
                              return apeCollectResolvedNodeStableIds(state, a) <
                                     apeCollectResolvedNodeStableIds(state, b);
                          });

                std::unordered_set<uintptr_t> seenTargetH;
                for (const auto &asa : acts) {
                    if (!asa->getTarget()) {
                        continue;
                    }
                    const uintptr_t th = asa->getTarget()->hash();
                    if (!seenTargetH.insert(th).second) {
                        continue;
                    }
                    if (tryRefineOneAction(asa)) {
                        return true;
                    }
                }
                return false;
            };

            size_t refinements = 0;
            for (int outer = 0; outer < 32; ++outer) {
                if (!innerPass()) {
                    break;
                }
                refinements++;
                BDLOG("naming: over-abstracted per-action refinement ok activity=%s outer=%d",
                      actKey.c_str(), outer);
            }

            return refinements;
        }
#endif

        naming::NamerLattice lat(naming::NamerFactory::current());
        const int hopsRaw = _preference ? _preference->getApeNamingActionRefineHops() : 8;
        const int hopsClamped = std::max(1, std::min(64, hopsRaw));
        size_t refinements = 0;
        for (int outer = 0; outer < 32; ++outer) {
            naming::NamingPtr cur = mgr.getNaming(actKey);
            if (!cur) {
                break;
            }

            const std::string fpBefore = cur->fingerprintString();
            naming::NamingPtr next = naming::NamingFactory::actionRefinement(cur, lat, hopsClamped);
            if (!next) {
                break;
            }
            if (next->fingerprintString() == fpBefore) {
                break;
            }
            {
                const naming::NamingPtr oldN = cur;
                const naming::NamingPtr newN = next;
                const naming::NamingPtr oldPar = oldN ? oldN->getParent() : nullptr;
                const naming::NamingPtr newPar = newN ? newN->getParent() : nullptr;
                const int direct_child = (oldN && newPar && newPar == oldN) ? 1 : 0;
                const int sibling_share_parent =
                    (oldPar && newPar && oldPar == newPar) ? 1 : 0;
                static std::atomic<uint64_t> g_updateNaming_batch_refine_entry_diag{0};
                const uint64_t ud = ++g_updateNaming_batch_refine_entry_diag;
                if (ud <= 80 || direct_child == 0) {
                    const std::string oldFp = oldN ? oldN->fingerprintString() : std::string();
                    const std::string newFp = newN ? newN->fingerprintString() : std::string();
                    const char *candidate_source = "actionRefinementBatch";
                    BLOG(
                        "naming diag [Model.batchUpdateNaming-enter Refine] seq=%llu act=%s outer=%d hops=%d "
                        "old=%p new=%p oldPar=%p newPar=%p direct_child=%d sibling_share_parent=%d "
                        "candidate_source=%s oldFin=%d newFin=%d old_fp=%s new_fp=%s",
                        static_cast<unsigned long long>(ud), actKey.c_str(), outer, hopsClamped,
                        static_cast<const void *>(oldN.get()), static_cast<const void *>(newN.get()),
                        static_cast<const void *>(oldPar.get()), static_cast<const void *>(newPar.get()),
                        direct_child, sibling_share_parent, candidate_source,
                        oldN ? oldN->getFineness() : -1, newN ? newN->getFineness() : -1,
                        oldFp.c_str(), newFp.c_str());
                }
            }
            _apeStateNamingManager->updateNaming(actKey, naming::NamingUpdateKind::Refine, next);
            invalidateApeGraphStateKeyDedupMap();
            pruneDivergentApeStatesForActivity(actKey);
            refinements++;
            notifyAgentsOfApeNamingChange();
            BDLOG("naming: over-abstracted batch actionRefinement activity=%s outer=%d hops=%d",
                  actKey.c_str(), outer, hopsClamped);
        }
        return refinements;
    }

#ifndef NDEBUG
    /** @brief Debug check that abstraction updates are not invoked concurrently from multiple threads. */
    void Model::assertApeSingleThreaded() const {
        const std::thread::id cur = std::this_thread::get_id();
        if (!_apeOwnerThreadSet) {
            _apeOwnerThread = cur;
            _apeOwnerThreadSet = true;
            return;
        }
        if (_apeOwnerThread != cur) {
            // Dynamic abstraction assumes Model is driven from a single thread.
            assert(false);
        }
    }
#endif
    /** @brief Records a graph transition after an agent moves to the target state from the current action. */
    void Model::recordTransition(const AbstractAgentPtr &agent, const StatePtr &targetState) {
        if (!agent || !targetState) return;
        const bool skipNd = agent->isCurrentStateRecovered();
        StatePtr srcState = agent->getCurrentState();
        ActivityStateActionPtr act = agent->getCurrentAction();
        if (!srcState || !act || !act->isModelAct() || !act->requireTarget()) return;
        {
            const int inLlm = (_llmTaskAgent && _llmTaskAgent->inSession()) ? 1 : 0;
            auto srcAp = srcState->getActivityString();
            const std::string srcAct = (srcAp && srcAp.get())
                                        ? naming::StateKey::canonicalActivityString(*srcAp)
                                        : std::string("<null>");
            auto tgtAp = targetState->getActivityString();
            const std::string tgtAct = (tgtAp && tgtAp.get())
                                        ? naming::StateKey::canonicalActivityString(*tgtAp)
                                        : std::string("<null>");
            BDLOG("naming: transition record enter inLlm=%d skipNdResolve=%d srcAct=%s tgtAct=%s actHash=%lu actId=%lu",
                inLlm, skipNd ? 1 : 0, srcAct.c_str(), tgtAct.c_str(), (unsigned long)act->hash(),
                (unsigned long)reinterpret_cast<uintptr_t>(act.get()));
        }
        fireGraphVisitStateTransitionIfModelAction(_graph, agent, targetState);
        recordApeTransitionForAbstraction(srcState, targetState, act, skipNd);
    }

    /** @brief Records an abstraction-layer transition between two states for refinement bookkeeping. */
    void Model::recordApeTransitionForAbstraction(const StatePtr &src, const StatePtr &tgt,
                                                  const ActivityStateActionPtr &act,
                                                  bool skipNonDeterministicResolve) {
        if (!src || !tgt || !act || _apeTransitionLog.empty() || _apeTreeTransitionLog.empty()) {
            return;
        }
#ifndef NDEBUG
        assertApeSingleThreaded();
#endif
        const int minTargets = (_preference ? _preference->getApeNamingMinNonDetTargets()
                                            : MinNonDeterminismCount);
        naming::StateKey srcKey = naming::StateKey::fromParts("", nullptr, {});
        naming::StateKey tgtKey = naming::StateKey::fromParts("", nullptr, {});
        auto srcAp = src->getActivityString();
        auto tgtAp = tgt->getActivityString();
        const std::string srcAct =
            (srcAp && srcAp.get()) ? naming::StateKey::canonicalActivityString(*srcAp) : std::string();
        const std::string tgtAct =
            (tgtAp && tgtAp.get()) ? naming::StateKey::canonicalActivityString(*tgtAp) : std::string();
        if (!tryGetApeStateKey(src->hash(), &srcKey, srcAct) ||
            !tryGetApeStateKey(tgt->hash(), &tgtKey, tgtAct)) {
            return;
        }
        if (src->getWidgetSize() != tgt->getWidgetSize()) {
            BDLOG("naming transition: state size delta srcHash=%" PRIuPTR " tgtHash=%" PRIuPTR
                  " srcWidgets=%zu tgtWidgets=%zu srcActions=%zu tgtActions=%zu "
                  "srcWidgetSummary=%s tgtWidgetSummary=%s srcActionSummary=%s tgtActionSummary=%s",
                  static_cast<uintptr_t>(src->hash()), static_cast<uintptr_t>(tgt->hash()),
                  src->getWidgetSize(), tgt->getWidgetSize(),
                  src->getActions().size(), tgt->getActions().size(),
                  summarizeStateWidgetsForLog(src).c_str(),
                  summarizeStateWidgetsForLog(tgt).c_str(),
                  summarizeStateActionsForLog(src).c_str(),
                  summarizeStateActionsForLog(tgt).c_str());
        }
        ApeTransitionEntry e;
        e.transitionSeq = ++_apeTransitionSeq;
        if (e.transitionSeq == 0) {
            e.transitionSeq = ++_apeTransitionSeq;
        }
        e.sourceKeyHash = srcKey.hash();
        e.hasSourceStateKey = true;
        e.sourceStateKey = srcKey;
        e.actionHash = act->hash();
        e.actionIdentity = reinterpret_cast<uintptr_t>(act.get());
        e.targetKeyHash = tgtKey.hash();
        e.hasTargetStateKey = true;
        e.targetStateKey = tgtKey;
        e.sourceStateHash = src->hash();
        e.targetStateHash = tgt->hash();
        {
            auto actPtr = src->getActivityString();
            e.sourceActivity = naming::StateKey::canonicalActivityString(
                (actPtr && actPtr.get()) ? *actPtr : "");
        }
        auto itSrcXmlSnapshot = _apeStateXmlByStateHash.find(e.sourceStateHash);
        if (itSrcXmlSnapshot != _apeStateXmlByStateHash.end()) {
            e.sourceXmlSnapshot = itSrcXmlSnapshot->second;
        }
        e.actionType = act->getActionType();
        e.hasTargetBounds = false;
        if (auto tw = act->getTarget()) {
            if (auto b = tw->getBounds()) {
                e.hasTargetBounds = true;
                e.targetBounds = *b;
                BDLOG("naming transition: record_target_bounds activity=%s srcKey=%zu act=%zu seq=%llu "
                      "widgetHash=%zu actionType=%d targetBounds=%s",
                      e.sourceActivity.c_str(), e.sourceKeyHash, e.actionHash,
                      static_cast<unsigned long long>(e.transitionSeq),
                      tw->hash(), static_cast<int>(act->getActionType()),
                      b->toString().c_str());
                if (e.targetBounds.left == 0 && e.targetBounds.top == 0 &&
                    e.targetBounds.right == 0 && e.targetBounds.bottom == 0) {
                    BDLOG("naming transition: zero_target_bounds_on_record activity=%s srcKey=%zu act=%zu "
                          "seq=%llu srcState=%zu tgtState=%zu",
                          e.sourceActivity.c_str(), e.sourceKeyHash, e.actionHash,
                          static_cast<unsigned long long>(e.transitionSeq),
                          e.sourceStateHash, e.targetStateHash);
                }
            } else {
                BDLOG("naming transition: record_target_bounds_null activity=%s srcKey=%zu act=%zu seq=%llu "
                      "widgetHash=%zu actionType=%d",
                      e.sourceActivity.c_str(), e.sourceKeyHash, e.actionHash,
                      static_cast<unsigned long long>(e.transitionSeq),
                      tw->hash(), static_cast<int>(act->getActionType()));
            }
        }
        e.hasTargetFullPath = false;
        e.targetFullPathHash = 0;
        if (auto ana = std::dynamic_pointer_cast<ActivityNameAction>(act)) {
            const uintptr_t h = ana->getApeDynamicTargetFullPathHash();
            if (h != 0) {
                e.hasTargetFullPath = true;
                e.targetFullPathHash = h;
            }
        }
        e.valid = true;
        BDLOG("naming: transition srcKey=%lu act=%lu tgtKey=%lu activity=%s",
              (unsigned long)e.sourceKeyHash, (unsigned long)e.actionHash, (unsigned long)e.targetKeyHash,
              e.sourceActivity.c_str());
        const ApePairKey pairKey{e.sourceKeyHash, e.actionHash};
        size_t prevPairTargetCount = 0;
        auto itPairBeforeInsert = _apePairAgg.find(pairKey);
        if (itPairBeforeInsert != _apePairAgg.end()) {
            prevPairTargetCount = itPairBeforeInsert->second.targetCounts.size();
        }
        ApeTransitionEntry &aSlot = _apeTransitionLog[_apeTransitionLogWriteIndex];
        aSlot = std::move(e);
        apePairAggAdd(aSlot);
        const std::string *srcXmlSnapshot = nullptr;
        auto itSrcXml = _apeStateXmlByStateHash.find(aSlot.sourceStateHash);
        if (itSrcXml != _apeStateXmlByStateHash.end() && !itSrcXml->second.empty()) {
            srcXmlSnapshot = &itSrcXml->second;
        }
        apeEvidencePoolAdd(pairKey, aSlot, srcXmlSnapshot);
        TreeTransitionEntry treeSlot;
        treeSlot.transitionSeq = aSlot.transitionSeq;
        treeSlot.sourceStateHash = aSlot.sourceStateHash;
        treeSlot.targetStateHash = aSlot.targetStateHash;
        treeSlot.actionHash = aSlot.actionHash;
        treeSlot.actionType = aSlot.actionType;
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
        gui_tree::GUITreePtr snapSrcLive = apeLatestGuiTreeSnapshot(src->hash());
        bool snapFallbackRebuilt = false;
        if (!snapFallbackRebuilt && !snapSrcLive) {
            // Lifecycle fallback: some transitions are recorded before buildApeRlStateKey has
            // published the source snapshot (fresh launch, recovery, coarsen-invalidated cache).
            // Without a snapshot, resolvedNodeStableIds stays empty (Plan B legacy path returns
            // empty), apeSourceGuiTree stays null, and downstream top-equivalence / target-name
            // resolvers degenerate. Rebuild from cached source XML and publish it so every later
            // call site (this one and future transitions) sees a valid object.
            auto itSrcXmlFallback = _apeStateXmlByStateHash.find(src->hash());
            if (itSrcXmlFallback != _apeStateXmlByStateHash.end() &&
                !itSrcXmlFallback->second.empty()) {
                std::string pkgFb;
                std::string clsFb;
                naming::StateKey::splitActivityPackageClass(aSlot.sourceActivity, &pkgFb, &clsFb);
                gui_tree::GUITreeBuildResult builtFb =
                    buildGuitreeFromCachedXmlPreferElement(itSrcXmlFallback->second, pkgFb, clsFb);
                if (builtFb.tree) {
                    apeRememberGuiTreeSnapshot(src->hash(), *builtFb.tree);
                    snapSrcLive = apeLatestGuiTreeSnapshot(src->hash());
                    snapFallbackRebuilt = (snapSrcLive != nullptr);
                    BDLOG("naming: transition record snap_fallback_rebuilt seq=%llu activity=%s "
                          "srcState=%lu rebuilt=%d xmlLen=%zu",
                          (unsigned long long)aSlot.transitionSeq, aSlot.sourceActivity.c_str(),
                          (unsigned long)src->hash(), snapFallbackRebuilt ? 1 : 0,
                          itSrcXmlFallback->second.size());
                }
            }
        }

        treeSlot.resolvedNodeStableIds = snapSrcLive
            ? apeCollectResolvedNodeStableIds(src, act, snapSrcLive)
            : apeCollectResolvedNodeStableIds(src, act);
#else
        treeSlot.resolvedNodeStableIds = apeCollectResolvedNodeStableIds(src, act);
#endif

        treeSlot.hasTargetBounds = aSlot.hasTargetBounds;
        treeSlot.targetBounds = aSlot.targetBounds;
        treeSlot.hasTargetFullPath = aSlot.hasTargetFullPath;
        treeSlot.targetFullPathHash = aSlot.targetFullPathHash;
        treeSlot.sourceActivity = aSlot.sourceActivity;
        treeSlot.valid = true;
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
        if (snapSrcLive) {
            treeSlot.apeSourceGuiTree = gui_tree::GUITree::cloneDeep(*snapSrcLive);
        }
        if (gui_tree::GUITreePtr snapTgt = apeLatestGuiTreeSnapshot(tgt->hash())) {
            treeSlot.apeTargetGuiTree = gui_tree::GUITree::cloneDeep(*snapTgt);
        }
#endif
        if (treeSlot.resolvedNodeStableIds.empty()) {
            BDLOG("naming: transition record resolvedNodes_empty seq=%llu activity=%s "
                  "hasLiveSnap=%d fallbackRebuilt=%d actionType=%d",
                  (unsigned long long)aSlot.transitionSeq, aSlot.sourceActivity.c_str(),
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
                  snapSrcLive ? 1 : 0, snapFallbackRebuilt ? 1 : 0,
#else
                  0, 0,
#endif
                  static_cast<int>(aSlot.actionType));
        }
        apeInsertTreeTransitionNoRefine(treeSlot);
        apeMiniHistoryRecordTransition(aSlot.sourceActivity, aSlot);
        size_t nowPairTargetCount = 0;
        auto itPairAfter = _apePairAgg.find(pairKey);
        if (itPairAfter != _apePairAgg.end()) {
            nowPairTargetCount = itPairAfter->second.targetCounts.size();
        }
        _apeTransitionLogWriteIndex = (_apeTransitionLogWriteIndex + 1) % _apeTransitionLog.size();

        const bool newActionTarget =
            prevPairTargetCount >= 1 && nowPairTargetCount > prevPairTargetCount;
        if (!newActionTarget) {
            const char *detail = "unknown";
            if (prevPairTargetCount == 0 && nowPairTargetCount == 1) {
                // Graph first edge for ModelAction is NEW_ACTION, not NEW_ACTION_TARGET.
                detail = "first_target_seen_NEW_ACTION";
            } else if (nowPairTargetCount <= prevPairTargetCount) {
                // Distinct target count did not grow for this (source, action) pair.
                detail = (nowPairTargetCount < prevPairTargetCount)
                         ? "target_count_decreased_no_growth"
                         : "target_count_unchanged_no_growth";
            } else if (prevPairTargetCount == 0 && nowPairTargetCount > 1) {
                // Rare when ring-buffer eviction and re-aggregation happened around first replay.
                detail = "unexpected_multi_target_with_zero_prev";
            }
            BDLOG("naming: skip event refine activity=%s reason=not_NEW_ACTION_TARGET "
                  "reason_detail=%s srcKey=%lu act=%lu prevTargets=%zu nowTargets=%zu "
                  "targetKey=%lu seq=%llu",
                  aSlot.sourceActivity.c_str(), detail, (unsigned long)pairKey.sourceKeyHash,
                  (unsigned long)pairKey.actionHash, static_cast<size_t>(prevPairTargetCount),
                  static_cast<size_t>(nowPairTargetCount), (unsigned long)aSlot.targetKeyHash,
                  static_cast<unsigned long long>(aSlot.transitionSeq));
            return;
        }
        if (skipNonDeterministicResolve) {
            BDLOG("naming: skip ND pipeline activity=%s reason=current_state_recovered "
                  "srcKey=%lu act=%lu tgtKey=%lu seq=%llu distinctTargets=%zu",
                  aSlot.sourceActivity.c_str(), (unsigned long)pairKey.sourceKeyHash,
                  (unsigned long)pairKey.actionHash, (unsigned long)aSlot.targetKeyHash,
                  static_cast<unsigned long long>(aSlot.transitionSeq), nowPairTargetCount);
            return;
        }
        if (!_preference || !_preference->useApeEvolveModel()) {
            BDLOG("naming: skip ND resolve activity=%s reason=evolveModel_off srcKey=%lu act=%lu",
                  aSlot.sourceActivity.c_str(), (unsigned long)pairKey.sourceKeyHash,
                  (unsigned long)pairKey.actionHash);
            return;
        }
        if (aSlot.actionType == ActionType::BACK) {
            BDLOG("naming: skip ND resolve activity=%s reason=back_action srcKey=%lu act=%lu targets=%zu",
                  aSlot.sourceActivity.c_str(), (unsigned long)pairKey.sourceKeyHash,
                  (unsigned long)pairKey.actionHash, nowPairTargetCount);
            return;
        }
        if (aSlot.actionIdentity != 0 &&
            _apeRefineActionIdentityBlacklist.count(aSlot.actionIdentity) != 0) {
            BDLOG("naming: skip ND resolve activity=%s reason=NDActionBlacklisted srcKey=%lu act=%lu actId=%lu",
                  aSlot.sourceActivity.c_str(), (unsigned long)pairKey.sourceKeyHash,
                  (unsigned long)pairKey.actionHash, (unsigned long)aSlot.actionIdentity);
            return;
        }
        auto countNonDetPairsByActivity = [&](const std::string &canonicalActivity) -> int {
            int c = 0;
            for (const auto &kv : _apePairAgg) {
                if (kv.second.sourceActivity == canonicalActivity &&
                    kv.second.targetCounts.size() >= static_cast<size_t>(minTargets)) {
                    ++c;
                }
            }
            return c;
        };
        const int nonDetPairs = countNonDetPairsByActivity(aSlot.sourceActivity);
        std::vector<uintptr_t> partnerTargets;
        partnerTargets.reserve(nowPairTargetCount);
        if (itPairAfter != _apePairAgg.end()) {
            itPairAfter->second.targetCounts.forEach([&](uintptr_t h, int /*count*/) {
                if (h != aSlot.targetKeyHash) {
                    partnerTargets.push_back(h);
                }
            });
        }
        std::sort(partnerTargets.begin(), partnerTargets.end());
        if (partnerTargets.empty()) {
            BDLOG("naming: skip ND partner loop activity=%s reason=no_peer_targets srcKey=%lu act=%lu "
                  "nstTarget=%lu distinctTargets=%zu",
                  aSlot.sourceActivity.c_str(), (unsigned long)pairKey.sourceKeyHash,
                  (unsigned long)pairKey.actionHash, (unsigned long)aSlot.targetKeyHash,
                  nowPairTargetCount);
            return;
        }

        BDLOG("naming: event refine-attempt (nondeterministic partner loop) activity=%s "
              "srcKey=%lu act=%lu nstTarget=%lu peerTargets=%zu nonDetPairs=%d",
              aSlot.sourceActivity.c_str(), (unsigned long)pairKey.sourceKeyHash,
              (unsigned long)pairKey.actionHash, (unsigned long)aSlot.targetKeyHash,
              partnerTargets.size(), nonDetPairs);
        ApeRefineFailReason failReason = ApeRefineFailReason::Other;
        auto failReasonStr = [&](ApeRefineFailReason r) -> const char * {
            switch (r) {
            case ApeRefineFailReason::None:
                return "none";
            case ApeRefineFailReason::ActionBlacklisted:
                return "action_blacklisted";
            case ApeRefineFailReason::NoDefaultRootNaming:
                return "no_default_root_naming";
            case ApeRefineFailReason::MaxStatesPerActivity:
                return "max_states_per_activity";
            case ApeRefineFailReason::MaxGuitreesPerState:
                return "max_guitrees_per_state";
            case ApeRefineFailReason::PairTargetsInsufficient:
                return "pair_targets_insufficient";
            case ApeRefineFailReason::UnsupportedRefineRelation:
                return "unsupported_refine_relation";
            case ApeRefineFailReason::NoAcceptedCandidates:
                return "no_accepted_candidates";
            case ApeRefineFailReason::BranchPairsUnavailable:
                return "branch_pairs_unavailable";
            case ApeRefineFailReason::Other:
            default: 
                return "other";
            }
        };

        bool namingChanged = false;
        for (uintptr_t peerTarget : partnerTargets) {
            ApeRefinePair rpPair;
            rpPair.sourceKeyHash = pairKey.sourceKeyHash;
            rpPair.nstTransitionSeq = aSlot.transitionSeq;
            rpPair.hasSourceStateKey = aSlot.hasSourceStateKey;
            rpPair.sourceStateKey = aSlot.sourceStateKey;
            rpPair.actionHash = pairKey.actionHash;
            rpPair.actionIdentity = aSlot.actionIdentity;
            rpPair.targetKeyHashes.clear();
            rpPair.targetKeyHashes.insert(peerTarget);
            rpPair.targetKeyHashes.insert(aSlot.targetKeyHash);
            rpPair.targetCount = 2;
            failReason = ApeRefineFailReason::Other;
            BDLOG("naming: ND refine try partner activity=%s srcKey=%lu act=%lu peerTarget=%lu nstTarget=%lu",
                  aSlot.sourceActivity.c_str(), (unsigned long)pairKey.sourceKeyHash,
                  (unsigned long)pairKey.actionHash, (unsigned long)peerTarget,
                  (unsigned long)aSlot.targetKeyHash);
            if (refineActivityApeNaming(aSlot.sourceActivity, &rpPair, nonDetPairs, &failReason)) {
                _apeEventRefineSuccessCount++;
                BDLOG("naming: event refine ok activity=%s srcKey=%lu act=%lu peerTarget=%lu nstTarget=%lu",
                      aSlot.sourceActivity.c_str(), (unsigned long)pairKey.sourceKeyHash,
                      (unsigned long)pairKey.actionHash, (unsigned long)peerTarget,
                      (unsigned long)aSlot.targetKeyHash);
                namingChanged = true;
                break;
            }
        }
        if (!namingChanged && failReason != ApeRefineFailReason::ActionBlacklisted &&
            pairKey.actionHash != 0) {
            size_t outStateTransitionsForAction = nowPairTargetCount;
            if (itPairAfter != _apePairAgg.end()) {
                outStateTransitionsForAction = itPairAfter->second.targetCounts.size();
            }
            const bool branchDataMissing =
                (failReason == ApeRefineFailReason::BranchPairsUnavailable);
            const bool ndBlacklistEligible =
                !branchDataMissing &&
                outStateTransitionsForAction >= static_cast<size_t>(kNDActionBlacklistMinOutEdges);
            const bool inserted =
                (ndBlacklistEligible && aSlot.actionIdentity != 0) &&
                _apeRefineActionIdentityBlacklist.insert(aSlot.actionIdentity).second;
            apeCapApeNamingCoarsenAndRefineBlacklists();
            if (inserted) {
                BLOG("naming: NDActionBlacklist add (out edges >= %d after failed resolve) activity=%s src=%lu act=%lu actId=%lu outTransitions=%zu failReason=%s",
                     kNDActionBlacklistMinOutEdges, aSlot.sourceActivity.c_str(),
                     (unsigned long)pairKey.sourceKeyHash, (unsigned long)pairKey.actionHash,
                     (unsigned long)aSlot.actionIdentity, outStateTransitionsForAction,
                     failReasonStr(failReason));
            }
            BDLOG("naming: event refine failed activity=%s srcKey=%lu act=%lu targets=%zu reason=%s "
                  "(ndBlacklistEligible=%d ndBlacklistInserted=%d actZero=%d outTransitions=%zu "
                  "outBelowNdBlacklistMin=%d branchDataMissing=%d)",
                  aSlot.sourceActivity.c_str(), (unsigned long)pairKey.sourceKeyHash,
                  (unsigned long)pairKey.actionHash, nowPairTargetCount,
                  failReasonStr(failReason), ndBlacklistEligible ? 1 : 0, inserted ? 1 : 0,
                  (pairKey.actionHash == 0) ? 1 : 0, outStateTransitionsForAction,
                  (outStateTransitionsForAction <
                   static_cast<size_t>(kNDActionBlacklistMinOutEdges)) ? 1 : 0,
                  branchDataMissing ? 1 : 0);
        } else if (!namingChanged) {
            BDLOG("naming: event refine failed activity=%s srcKey=%lu act=%lu targets=%zu reason=%s "
                  "(ndBlacklistEligible=0 actZero=%d)",
                  aSlot.sourceActivity.c_str(), (unsigned long)pairKey.sourceKeyHash,
                  (unsigned long)pairKey.actionHash, nowPairTargetCount,
                  failReasonStr(failReason), (pairKey.actionHash == 0) ? 1 : 0);
        }

        if (namingChanged) {
            notifyAgentsOfApeNamingChange();
        }
    }

    /** @brief Removes one aggregated transition sample for a (source,target) pair from internal counters. */
    void Model::apePairAggRemove(const ApeTransitionEntry &e) {
        if (!e.valid) {
            return;
        }
        ApePairKey pk{e.sourceKeyHash, e.actionHash};
        auto it = _apePairAgg.find(pk);
        if (it == _apePairAgg.end()) {
            return;
        }
        if (!it->second.targetCounts.decrement(e.targetKeyHash)) {
            return;
        }
        if (it->second.targetCounts.empty()) {
            _apePairAgg.erase(it);
        }
    }

    /** @brief Adds one aggregated transition sample for a (source,target) pair. */
    void Model::apePairAggAdd(const ApeTransitionEntry &e) {
        if (!e.valid) {
            return;
        }
        ApePairKey pk{e.sourceKeyHash, e.actionHash};
        auto &slot = _apePairAgg[pk];
        slot.targetCounts.increment(e.targetKeyHash);
        slot.sourceActivity = e.sourceActivity;
        if (e.hasSourceStateKey) {
            slot.hasSourceStateKey = true;
            slot.sourceStateKey = e.sourceStateKey;
        }
    }

    /** @brief Appends transition evidence for a pair key into the bounded evidence pool. */
    void Model::apeEvidencePoolAdd(const ApePairKey &pairKey, const ApeTransitionEntry &e,
                                   const std::string *sourceXmlSnapshot) {
        if (!e.valid || pairKey.sourceKeyHash == 0 || pairKey.actionHash == 0) {
            return;
        }
        _ape_correctness_counters.evidence_pool_sample_add++;

        ApeEvidenceSample s;
        s.sourceStateHash = e.sourceStateHash;
        if (sourceXmlSnapshot && !sourceXmlSnapshot->empty()) {
            s.sourceXml = *sourceXmlSnapshot;
        } else {
            auto itXml = _apeStateXmlByStateHash.find(e.sourceStateHash);
            if (itXml != _apeStateXmlByStateHash.end() && !itXml->second.empty()) {
                s.sourceXml = itXml->second;
                BDLOG("naming evidence: fallback_source_xml activity=%s srcState=%zu seq=%llu pair=(%zu,%zu)",
                      e.sourceActivity.c_str(), e.sourceStateHash,
                      static_cast<unsigned long long>(e.transitionSeq),
                      pairKey.sourceKeyHash, pairKey.actionHash);
            }
        }
        s.targetStateHash = e.targetStateHash;
        s.targetKeyHash = e.targetKeyHash;
        s.actionType = e.actionType;
        s.hasTargetBounds = e.hasTargetBounds;
        s.targetBounds = e.targetBounds;
        s.hasTargetFullPath = e.hasTargetFullPath;
        s.targetFullPathHash = e.targetFullPathHash;

        uint64_t epoch = ++_apeEvidenceEpoch;
        if (epoch == 0) {
            epoch = ++_apeEvidenceEpoch;
        }
        s.sourceTransitionSeq = e.transitionSeq;
        s.sourceTreeHash = s.sourceXml.empty() ? 0 : fastStringHash(s.sourceXml);

        auto it = _apeEvidencePools.find(pairKey);
        if (it == _apeEvidencePools.end()) {
            _ape_correctness_counters.evidence_pool_new_pair++;
            it = _apeEvidencePools.emplace(pairKey, ApeEvidencePool{}).first;
        }
        it->second.push(std::move(s), epoch);

        if (!_apeEvidencePoolClock.empty()) {
            ApeEvidencePoolClockEntry &ce = _apeEvidencePoolClock[_apeEvidencePoolClockWriteIndex];
            ce.key = pairKey;
            ce.epoch = epoch;
            _apeEvidencePoolClockWriteIndex =
                (_apeEvidencePoolClockWriteIndex + 1) % _apeEvidencePoolClock.size();
        }

        apeEvidencePoolClockEvict();
    }

    /** @brief Evicts oldest evidence entries when the shared evidence pool exceeds its cap. */
    void Model::apeEvidencePoolClockEvict() {
        constexpr size_t kMaxPools = MaxTransitionLogSize;
        if (_apeEvidencePoolClock.empty()) {
            return;
        }
        if (_apeEvidencePools.size() <= kMaxPools) {
            return;
        }

        size_t scanned = 0;
        const size_t clockSize = _apeEvidencePoolClock.size();
        while (_apeEvidencePools.size() > kMaxPools && scanned < clockSize) {
            ApeEvidencePoolClockEntry &victim = _apeEvidencePoolClock[_apeEvidencePoolClockEvictIndex];
            _apeEvidencePoolClockEvictIndex =
                (_apeEvidencePoolClockEvictIndex + 1) % _apeEvidencePoolClock.size();
            ++scanned;

            if (victim.epoch == 0) {
                continue;
            }

            auto itV = _apeEvidencePools.find(victim.key);
            if (itV == _apeEvidencePools.end()) {
                victim.epoch = 0;
                continue;
            }

            if (itV->second.lastTouchEpoch != victim.epoch) {
                continue;
            }

            _ape_correctness_counters.evidence_pool_evict++;
            _apeEvidencePools.erase(itV);
            victim.epoch = 0;
        }
    }

    /** @brief Clears transition aggregation structures for one canonical activity key. */
    void Model::apeClearTransitionAggregationForActivity(const std::string &actKeyCanonical) {
        for (auto &slot : _apeTransitionLog) {
            if (slot.valid && slot.sourceActivity == actKeyCanonical) {
                _apeEvidencePools.erase(ApePairKey{slot.sourceKeyHash, slot.actionHash});
                apePairAggRemove(slot);
                slot.valid = false;
            }
        }
        for (auto &slot : _apeTreeTransitionLog) {
            if (slot.valid && slot.sourceActivity == actKeyCanonical) {
                slot.valid = false;
            }
        }
        for (auto it = _apePairAgg.begin(); it != _apePairAgg.end();) {
            if (it->second.sourceActivity == actKeyCanonical) {
                it = _apePairAgg.erase(it);
            } else {
                ++it;
            }
        }
    }

    /** @brief Marks a state hash as recently visited in the per-activity mini history ring buffer. */
    void Model::apeMiniHistoryTouchState(const std::string &activityKeyCanonical, uintptr_t stateHash) {
        if (activityKeyCanonical.empty() || stateHash == 0) {
            return;
        }
        _apeMiniHistoryByActivity[activityKeyCanonical].touchState(stateHash);
    }

    /** @brief Appends a transition edge to mini history for incremental local rebuilds. */
    void Model::apeMiniHistoryRecordTransition(const std::string &activityKeyCanonical,
                                               const ApeTransitionEntry &e) {
        if (activityKeyCanonical.empty() || !e.valid) {
            return;
        }
        ApeMiniHistory &h = _apeMiniHistoryByActivity[activityKeyCanonical];
        h.touchState(e.sourceStateHash);
        h.touchState(e.targetStateHash);
        ApeMiniHistoryTransition t;
        t.sourceStateHash = e.sourceStateHash;
        t.targetStateHash = e.targetStateHash;
        t.actionHash = e.actionHash;
        t.actionType = e.actionType;
        t.hasTargetBounds = e.hasTargetBounds;
        t.targetBounds = e.targetBounds;
        t.hasTargetFullPath = e.hasTargetFullPath;
        t.targetFullPathHash = e.targetFullPathHash;
        t.valid = true;
        h.pushTransition(t);
    }

    /** @brief Inserts a raw transition log entry without running refinement passes. */
    void Model::apeInsertTransitionEntryNoRefine(const ApeTransitionEntry &e,
                                                 const TreeTransitionEntry *treeMeta) {
        if (!e.valid || _apeTransitionLog.empty()) {
            return;
        }
        ApeTransitionEntry &slot = _apeTransitionLog[_apeTransitionLogWriteIndex];
        if (slot.valid) {
            apePairAggRemove(slot);
        }
        slot = e;
        apePairAggAdd(slot);
        const std::string *slotSrcXmlSnapshot = slot.sourceXmlSnapshot.empty()
            ? nullptr
            : &slot.sourceXmlSnapshot;
        apeEvidencePoolAdd(ApePairKey{slot.sourceKeyHash, slot.actionHash}, slot,
                           slotSrcXmlSnapshot);
        TreeTransitionEntry treeSlot;
        treeSlot.transitionSeq = slot.transitionSeq;
        treeSlot.sourceStateHash = slot.sourceStateHash;
        treeSlot.targetStateHash = slot.targetStateHash;
        treeSlot.actionHash = slot.actionHash;
        treeSlot.actionType = slot.actionType;
        if (treeMeta) {
            treeSlot.resolvedNodeStableIds = treeMeta->resolvedNodeStableIds;
        }
        treeSlot.hasTargetBounds = slot.hasTargetBounds;
        treeSlot.targetBounds = slot.targetBounds;
        treeSlot.hasTargetFullPath = slot.hasTargetFullPath;
        treeSlot.targetFullPathHash = slot.targetFullPathHash;
        treeSlot.sourceActivity = slot.sourceActivity;
        treeSlot.valid = true;
        apeInsertTreeTransitionNoRefine(treeSlot);
        _apeTransitionLogWriteIndex = (_apeTransitionLogWriteIndex + 1) % _apeTransitionLog.size();
    }

    /** @brief Inserts a tree transition record without running refinement passes. */
    void Model::apeInsertTreeTransitionNoRefine(const TreeTransitionEntry &e) {
        if (!e.valid || _apeTreeTransitionLog.empty()) {
            return;
        }
        _apeTreeTransitionLog[_apeTreeTransitionLogWriteIndex] = e;
        _apeTreeTransitionLogWriteIndex =
            (_apeTreeTransitionLogWriteIndex + 1) % _apeTreeTransitionLog.size();
    }

    /** @brief Rebuilds local aggregation from mini history when drift thresholds require it. */
    bool Model::apeLocalRebuildFromHistoryIfNeeded(const std::string &activityKeyCanonical,
                                                   const char *reason) {
        if (!_graph || activityKeyCanonical.empty()) {
            return false;
        }
        ApeActivityRebuildStats &st = _apeRebuildStatsByActivity[activityKeyCanonical];
        const uint64_t now = static_cast<uint64_t>(_graph->getTimestamp());
        constexpr uint64_t kMinInterval = 500;
        if (st.lastRebuildTimestamp != 0 && now > st.lastRebuildTimestamp &&
            (now - st.lastRebuildTimestamp) < kMinInterval) {
            return false;
        }
        const double ratio =
            st.actionBlacklistChecks > 0
                ? (static_cast<double>(st.actionBlacklistHits) /
                   static_cast<double>(st.actionBlacklistChecks))
                : 0.0;
        constexpr int kMinConsecutiveRollbacks = 2;
        constexpr int kMinActionChecks = 20;
        constexpr double kMinActionRatio = 0.7;
        const bool triggerByRollback = st.consecutiveRollbacks >= kMinConsecutiveRollbacks;
        const bool triggerByActionBlk =
            (st.actionBlacklistChecks >= kMinActionChecks && ratio >= kMinActionRatio);
        if (!triggerByRollback && !triggerByActionBlk) {
            return false;
        }
        if (!apeLocalRebuildFromHistory(activityKeyCanonical)) {
            return false;
        }
        st.lastRebuildTimestamp = now;
        st.consecutiveRollbacks = 0;
        st.actionBlacklistChecks = 0;
        st.actionBlacklistHits = 0;
        BLOG("naming: local rebuild activity=%s reason=%s", activityKeyCanonical.c_str(),
             reason ? reason : "(unknown)");
        return true;
    }

    /** @brief Rebuilds transition aggregation from stored history for one activity. */
    bool Model::apeLocalRebuildFromHistory(const std::string &activityKeyCanonical) {
#if !defined(FASTBOT_HAS_PUGIXML) || !FASTBOT_HAS_PUGIXML
        (void)activityKeyCanonical;
        return false;
#else
        if (!_graph || activityKeyCanonical.empty()) {
            return false;
        }
        const bool wantApeRlIdentity = (_preference == nullptr) ||
                                       !_preference->useStaticReuseAbstraction();
        if (!wantApeRlIdentity) {
            return false;
        }

        // Snapshot stale-state-driven XML/transition history before clearing states.
        naming::NamingPtr curNaming = _apeStateNamingManager
            ? _apeStateNamingManager->activityManager().getNaming(activityKeyCanonical)
            : nullptr;
        const std::string currentFp = curNaming ? curNaming->fingerprintString() : std::string();
        if (currentFp.empty()) {
            return false;
        }

        std::unordered_set<uintptr_t> staleStateHashes;
        staleStateHashes.reserve(128);
        for (const auto &kv : _ape_state_keys_by_hash) {
            const uintptr_t sh = kv.first;
            for (const auto &k : kv.second) {
                if (k.activity() != activityKeyCanonical) {
                    continue;
                }
                if (k.namingFingerprint() != currentFp) {
                    staleStateHashes.insert(sh);
                }
                break;
            }
        }
        if (staleStateHashes.empty()) {
            return false;
        }

        // Snapshot minimal XML history before clearing states.
        std::unordered_map<uintptr_t, std::string> xmlByOldStateHash;
        xmlByOldStateHash.reserve(64);
        auto snapXml = [&](uintptr_t sh) {
            if (sh == 0) {
                return;
            }
            if (xmlByOldStateHash.find(sh) != xmlByOldStateHash.end()) {
                return;
            }
            auto itX = _apeStateXmlByStateHash.find(sh);
            if (itX == _apeStateXmlByStateHash.end() || itX->second.empty()) {
                return;
            }
            xmlByOldStateHash.emplace(sh, itX->second);
        };
        std::vector<TreeTransitionEntry> transitionsToReplay;
        transitionsToReplay.reserve(128);
        for (uintptr_t sh : staleStateHashes) {
            snapXml(sh);
        }
        for (const auto &e : _apeTreeTransitionLog) {
            if (!e.valid || e.sourceActivity != activityKeyCanonical) {
                continue;
            }
            if (staleStateHashes.count(e.sourceStateHash) == 0 &&
                staleStateHashes.count(e.targetStateHash) == 0) {
                continue;
            }
            transitionsToReplay.push_back(e);
            snapXml(e.sourceStateHash);
            snapXml(e.targetStateHash);
        }
        if (xmlByOldStateHash.empty()) {
            return false;
        }

        // Remove all states for this activity.
        AbstractAgentPtr agent = getOrCreateAgent(ModelConstants::DefaultDeviceID);
        if (!agent) {
            return false;
        }
        stringPtr activityPtr = getOrCreateActivityPtr(activityKeyCanonical);

        struct PreparedRebuildState {
            uintptr_t oldStateHash{0};
            std::string xml;
            ElementPtr elemSnapshot;
            StatePtr built;
            bool haveApeKey{false};
            naming::StateKey apeKey = naming::StateKey::fromParts("", nullptr, {});
        };

        std::vector<PreparedRebuildState> preparedStates;
        preparedStates.reserve(xmlByOldStateHash.size());
        for (const auto &kv : xmlByOldStateHash) {
            const uintptr_t oldStateHash = kv.first;
            const std::string &xml = kv.second;

            ElementPtr elem = Element::createFromXml(xml);
            if (!elem) {
                continue;
            }
            StatePtr built = buildStateOnly(elem, agent, activityPtr);
            if (!built) {
                continue;
            }

            PreparedRebuildState ps;
            ps.oldStateHash = oldStateHash;
            ps.xml = xml;
            ps.elemSnapshot = elem;
            ps.built = built;
            ps.apeKey = naming::StateKey::fromParts(activityKeyCanonical, nullptr, {});
            ps.haveApeKey = buildApeStateKeyFromElementTree(
                elem, activityKeyCanonical, &ps.apeKey, nullptr, built);
            if (ps.haveApeKey) {
                ps.built->applyDynamicAbstractionIdentityHash(ps.apeKey.hash());
            }
            preparedStates.push_back(std::move(ps));
        }
        if (preparedStates.empty()) {
            return false;
        }

        // Two-phase commit: prepare rebuilt states first, then replace stale states atomically.
        std::unordered_set<uintptr_t> toRemove;
        toRemove = staleStateHashes;
        std::unordered_map<uintptr_t, StatePtr> existingStateByHash;
        existingStateByHash.reserve(256);
        for (const auto &s : _graph->getStates()) {
            if (s) {
                existingStateByHash[s->hash()] = s;
            }
        }
        for (const auto &s : _graph->getStates()) {
            if (!s || toRemove.count(s->hash()) == 0) {
                continue;
            }
            for (const auto &a : s->getActions()) {
                if (a) {
                    _apeInvalidatedReuseActionHashes.insert(a->hash());
                }
            }
        }
        _graph->removeStatesByHash(toRemove);
        // Graph apeNaming index: already cleared in Graph::removeStatesByHash per removed StatePtr.
        for (uintptr_t sh : toRemove) {
            _ape_state_keys_by_hash.erase(sh);
            _apeGuiTreeNamingBlacklist.erase(sh);
            _apeStateElementByStateHash.erase(sh);
            _apeStateXmlByStateHash.erase(sh);
        }
        for (auto &slot : _apeTransitionLog) {
            if (!slot.valid) {
                continue;
            }
            if (toRemove.count(slot.sourceStateHash) == 0 &&
                toRemove.count(slot.targetStateHash) == 0) {
                continue;
            }
            _apeEvidencePools.erase(ApePairKey{slot.sourceKeyHash, slot.actionHash});
            apePairAggRemove(slot);
            slot.valid = false;
        }
        for (auto &slot : _apeTreeTransitionLog) {
            if (!slot.valid) {
                continue;
            }
            if (toRemove.count(slot.sourceStateHash) == 0 &&
                toRemove.count(slot.targetStateHash) == 0) {
                continue;
            }
            slot.valid = false;
        }
        

        // Reset history to track rebuilt state hashes.
        _apeMiniHistoryByActivity[activityKeyCanonical] = ApeMiniHistory{};
        ApeMiniHistory &newHist = _apeMiniHistoryByActivity[activityKeyCanonical];

        std::unordered_map<uintptr_t, StatePtr> rebuiltStateByOldHash;
        rebuiltStateByOldHash.reserve(preparedStates.size() * 2);
        for (const auto &ps : preparedStates) {
            StatePtr canonical = _graph->addState(ps.built);
            if (ps.haveApeKey) {
                recordApeStateKey(canonical, ps.apeKey);
            }
            _apeStateXmlByStateHash[canonical->hash()] = ps.xml;
            if (ps.elemSnapshot) {
                _apeStateElementByStateHash[canonical->hash()] = ps.elemSnapshot;
            }
            newHist.touchState(canonical->hash());
            rebuiltStateByOldHash[ps.oldStateHash] = canonical;
        }

        size_t replayTotal = 0;
        size_t replayMissingXml = 0;
        size_t replayMissingState = 0;
        size_t replayActionNoMatch = 0;
        size_t replayActionHashExact = 0;
        size_t replayInserted = 0;

        for (const auto &t : transitionsToReplay) {
            if (!t.valid) {
                continue;
            }
            replayTotal++;
            auto its = rebuiltStateByOldHash.find(t.sourceStateHash);
            auto itt = rebuiltStateByOldHash.find(t.targetStateHash);
            if (its == rebuiltStateByOldHash.end()) {
                auto itExisting = existingStateByHash.find(t.sourceStateHash);
                if (itExisting != existingStateByHash.end() && toRemove.count(t.sourceStateHash) == 0) {
                    its = rebuiltStateByOldHash.insert({t.sourceStateHash, itExisting->second}).first;
                }
            }
            if (itt == rebuiltStateByOldHash.end()) {
                auto itExisting = existingStateByHash.find(t.targetStateHash);
                if (itExisting != existingStateByHash.end() && toRemove.count(t.targetStateHash) == 0) {
                    itt = rebuiltStateByOldHash.insert({t.targetStateHash, itExisting->second}).first;
                }
            }
            if (its == rebuiltStateByOldHash.end() || itt == rebuiltStateByOldHash.end()) {
                replayMissingXml++;
                continue;
            }
            StatePtr srcState = its->second;
            StatePtr tgtState = itt->second;
            if (!srcState || !tgtState) {
                replayMissingState++;
                continue;
            }
            if (t.actionHash == 0) {
                replayActionNoMatch++;
                continue;
            }

            ActivityStateActionPtr matchedAction;
            for (const auto &a : srcState->getActions()) {
                auto asa = std::dynamic_pointer_cast<ActivityStateAction>(a);
                if (!asa) {
                    continue;
                }
                if (asa->hash() == t.actionHash) {
                    matchedAction = asa;
                    break;
                }
            }
            if (!matchedAction) {
                replayActionNoMatch++;
                continue;
            }
            if (matchedAction->hash() == t.actionHash) {
                replayActionHashExact++;
            }

            naming::StateKey srcKey = naming::StateKey::fromParts("", nullptr, {});
            naming::StateKey tgtKey = naming::StateKey::fromParts("", nullptr, {});
            auto srcAp = srcState->getActivityString();
            auto tgtAp = tgtState->getActivityString();
            const std::string srcAct =
                (srcAp && srcAp.get()) ? naming::StateKey::canonicalActivityString(*srcAp) : std::string();
            const std::string tgtAct =
                (tgtAp && tgtAp.get()) ? naming::StateKey::canonicalActivityString(*tgtAp) : std::string();
            if (!tryGetApeStateKey(srcState->hash(), &srcKey, srcAct) ||
                !tryGetApeStateKey(tgtState->hash(), &tgtKey, tgtAct)) {
                continue;
            }

            ApeTransitionEntry edge;
            edge.sourceKeyHash = srcKey.hash();
            edge.hasSourceStateKey = true;
            edge.sourceStateKey = srcKey;
            edge.actionHash = matchedAction->hash();
            edge.actionIdentity = reinterpret_cast<uintptr_t>(matchedAction.get());
            edge.targetKeyHash = tgtKey.hash();
            edge.hasTargetStateKey = true;
            edge.targetStateKey = tgtKey;
            edge.sourceStateHash = srcState->hash();
            edge.targetStateHash = tgtState->hash();
            edge.transitionSeq = t.transitionSeq;
            auto itSrcXml = xmlByOldStateHash.find(t.sourceStateHash);
            if (itSrcXml != xmlByOldStateHash.end()) {
                edge.sourceXmlSnapshot = itSrcXml->second;
            }
            edge.actionType = matchedAction->getActionType();
            edge.hasTargetBounds = t.hasTargetBounds;
            edge.targetBounds = t.targetBounds;
            edge.hasTargetFullPath = t.hasTargetFullPath;
            edge.targetFullPathHash = t.targetFullPathHash;
            edge.sourceActivity = activityKeyCanonical;
            edge.valid = true;
            apeInsertTransitionEntryNoRefine(edge, &t);
            replayInserted++;
        }

        BLOG("naming: local rebuild done activity=%s xmlSnapshots=%zu preparedStates=%zu "
             "removedStates=%zu replay(total=%zu inserted=%zu missingXml=%zu missingState=%zu "
             "actionNoMatch=%zu actionHashExact=%zu)",
             activityKeyCanonical.c_str(), xmlByOldStateHash.size(), preparedStates.size(),
             toRemove.size(), replayTotal, replayInserted, replayMissingXml, replayMissingState,
             replayActionNoMatch, replayActionHashExact);
        notifyAgentsOfApeNamingChange();
        return true;
#endif
    }

    /** @brief Notifies agents that naming or abstraction mappings changed so they can refresh. */
    void Model::notifyAgentsOfApeNamingChange() {
        ++_apeStructuralVersion;
        if (_graph) {
            _graph->syncApeStructuralEpoch(_apeStructuralVersion);
        }
        BDLOG("naming: structural_version=%llu graphId=%s (model version / graph structural id)",
              (unsigned long long)_apeStructuralVersion,
              _graph ? _graph->getStructureId().c_str() : "-");
        for (const auto &kv : _deviceIDAgentMap) {
            if (kv.second) {
                kv.second->onStateAbstractionChanged();
            }
        }
        _apeInvalidatedReuseActionHashes.clear();
        // Validate allNewActions again after model/naming rebuild (coarsen, over-refine, ND).
        if (_apeLastScreenStateForValidate) {
            const StatePtr s = _apeLastScreenStateForValidate;
            for (const auto &kv : _deviceIDAgentMap) {
                if (kv.second) {
                    kv.second->validateAllNewActions(s);
                }
            }
        }
    }

    /** @brief Counts abstract states for an activity and naming fingerprint. */
    size_t Model::getApeStateCountByActivityAndNamingFingerprint(
        const std::string &activityKeyCanonical, const std::string &namingFingerprint) const {
        if (activityKeyCanonical.empty() || namingFingerprint.empty()) {
            return 0;
        }
        size_t count = 0;
        for (const auto &kv : _ape_state_keys_by_hash) {
            const auto &bucket = kv.second;
            bool match = false;
            for (const auto &k : bucket) {
                if (k.activity() == activityKeyCanonical && k.namingFingerprint() == namingFingerprint) {
                    match = true;
                    break;
                }
            }
            if (match) {
                ++count;
            }
        }
        return count;
    }

#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
    /** @brief Caches the latest GUI tree snapshot for a state hash (debugging / refinement). */
    void Model::apeRememberGuiTreeSnapshot(uintptr_t stateHash, const gui_tree::GUITree &tree) {
        gui_tree::GUITreePtr copy = gui_tree::GUITree::cloneDeep(tree);
        if (!copy || stateHash == 0) {
            return;
        }

        auto &dq = _apeGuiTreeSnapshotsByStateHash[stateHash];
        dq.push_back(std::move(copy));
        while (dq.size() > kMaxApeGuiTreeSnapshotsPerState) {
            if (_apeStateNamingManager && dq.front()) {
                _apeStateNamingManager->releaseTreeCache(*dq.front());
            }
            dq.pop_front();
        }
    }

    /** @brief Returns the most recently cached GUI tree for a state hash, if any. */
    gui_tree::GUITreePtr Model::apeLatestGuiTreeSnapshot(uintptr_t stateHash) const {
        auto it = _apeGuiTreeSnapshotsByStateHash.find(stateHash);
        if (it == _apeGuiTreeSnapshotsByStateHash.end() || it->second.empty()) {
            return nullptr;
        }
        return it->second.back();
    }

    /** @brief Looks up a cached GUI tree whose serialized XML equals the given string. */
    gui_tree::GUITreePtr Model::apeGuiTreeSnapshotForExactCachedXml(const std::string &xml) const {
        if (xml.empty()) {
            return nullptr;
        }
        for (const auto &kv : _apeStateXmlByStateHash) {
            if (kv.second == xml) {
                return apeLatestGuiTreeSnapshot(kv.first);
            }
        }
        return nullptr;
    }
#endif

namespace {
    // Shared cleanup for any per-activity state prune: drops graph states + sidecar caches for the
    // given hash set, preserving mini-history-referenced XML for later local rebuild. Implemented as
    // a member-style helper via explicit parameters so it can be called from both the fingerprint-
}

    /** @brief Prunes abstract states in a set of hashes using the shared pruning policy. */
    void Model::pruneApeStatesByStateHashesCommon(const std::string &activityKeyCanonical,
                                                  std::unordered_set<uintptr_t> &staleStateHashes,
                                                  const char *reasonTag) {
        if (staleStateHashes.empty() || !_graph) {
            return;
        }

        // Collect action hashes from stale states (pre-prune) so agents can invalidate
        // hash-keyed caches without globally clearing all learned experience.
        for (const auto &s : _graph->getStates()) {
            if (!s || staleStateHashes.count(s->hash()) == 0) {
                continue;
            }
            for (const auto &a : s->getActions()) {
                if (a) {
                    _apeInvalidatedReuseActionHashes.insert(a->hash());
                }
            }
        }

        const size_t removedFromGraph = _graph->removeStatesByHash(staleStateHashes);
        (void)removedFromGraph;
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
        // Q8 (local rebuild): keep cached XML for states referenced by the per-activity mini-history,
        // even if the corresponding Graph states are pruned, so that later local rebuild can
        // still snapshot and replay these recent trees/transitions.
        const ApeMiniHistory *histKeep = nullptr;
        auto itHistKeep = _apeMiniHistoryByActivity.find(activityKeyCanonical);
        if (itHistKeep != _apeMiniHistoryByActivity.end()) {
            histKeep = &itHistKeep->second;
        }
        auto shouldKeepXml = [&](uintptr_t sh) -> bool {
            if (!histKeep) {
                return false;
            }
            for (uintptr_t h : histKeep->stateHashes) {
                if (h == sh) {
                    return true;
                }
            }
            for (const auto &t : histKeep->transitions) {
                if (!t.valid) {
                    continue;
                }
                if (t.sourceStateHash == sh || t.targetStateHash == sh) {
                    return true;
                }
            }
            return false;
        };
#endif

        for (uintptr_t sh : staleStateHashes) {
            _ape_state_keys_by_hash.erase(sh);
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
            if (!shouldKeepXml(sh)) {
                _apeStateElementByStateHash.erase(sh);
                _apeStateXmlByStateHash.erase(sh);
            }
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
            auto itSnap = _apeGuiTreeSnapshotsByStateHash.find(sh);
            if (itSnap != _apeGuiTreeSnapshotsByStateHash.end() && _apeStateNamingManager) {
                for (const auto &snap : itSnap->second) {
                    if (snap) {
                        _apeStateNamingManager->releaseTreeCache(*snap);
                    }
                }
            }
            _apeGuiTreeSnapshotsByStateHash.erase(sh);
#endif
#endif
            _apeGuiTreeNamingBlacklist.erase(sh);
        }
        BLOG("naming: %s pruned=%zu activity=%s",
             reasonTag ? reasonTag : "prune", staleStateHashes.size(), activityKeyCanonical.c_str());
    }

    /** @brief Removes stale abstract states for one activity to bound memory use. */
    void Model::pruneStaleApeStatesForActivity(const std::string &activityKeyCanonical,
                                               const std::string &staleNamingFingerprint,
                                               const std::unordered_set<uintptr_t> *affectedStateHashes) {
        if (!_graph || activityKeyCanonical.empty() || staleNamingFingerprint.empty()) {
            return;
        }
        (void)affectedStateHashes;
        std::unordered_set<uintptr_t> staleStateHashes;
        staleStateHashes.reserve(64);
        for (const auto &kv : _ape_state_keys_by_hash) {
            const uintptr_t stateHash = kv.first;
            const auto &bucket = kv.second;
            bool match = false;
            for (const auto &k : bucket) {
                if (k.activity() == activityKeyCanonical && k.namingFingerprint() == staleNamingFingerprint) {
                    match = true;
                    break;
                }
            }
            if (match) {
                staleStateHashes.insert(stateHash);
            }
        }
        if (staleStateHashes.empty()) {
            return;
        }
        pruneApeStatesByStateHashesCommon(activityKeyCanonical, staleStateHashes, "stale-fp-prune");
    }

    /** @brief Removes divergent abstract states for one activity. */
    void Model::pruneDivergentApeStatesForActivity(const std::string &activityKeyCanonical) {
        if (!_graph || activityKeyCanonical.empty() || !_apeStateNamingManager) {
            return;
        }
        naming::ActivityNamingManager &mgr = _apeStateNamingManager->activityManager();
        const naming::NamingPtr cur = mgr.getNaming(activityKeyCanonical);
        if (!cur) {
            return;
        }
        const std::string currentFp = cur->fingerprintString();
        if (currentFp.empty()) {
            return;
        }
        std::unordered_set<uintptr_t> staleStateHashes;
        staleStateHashes.reserve(64);
        for (const auto &kv : _ape_state_keys_by_hash) {
            const auto &bucket = kv.second;
            bool hasStaleEntryForActivity = false;
            for (const auto &k : bucket) {
                if (k.activity() != activityKeyCanonical) {
                    continue;
                }
                if (k.namingFingerprint() != currentFp) {
                    hasStaleEntryForActivity = true;
                    break;
                }
            }
            if (hasStaleEntryForActivity) {
                staleStateHashes.insert(kv.first);
            }
        }
        if (staleStateHashes.empty()) {
            return;
        }
        pruneApeStatesByStateHashesCommon(activityKeyCanonical, staleStateHashes, "divergent-fp-prune");
    }

    /** @brief Evaluates whether GUI-tree naming should be blacklisted for the given state hashes. */
    bool Model::evalApeGuiTreeNamingBlacklist(const std::vector<uintptr_t> &stateHashes,
                                               const naming::NamingPtr &naming) const {
        if (!naming || stateHashes.empty()) {
            return true;
        }
        const std::string &fp = naming->fingerprintString();
        for (uintptr_t sh : stateHashes) {
            auto it = _apeGuiTreeNamingBlacklist.find(sh);
            if (it != _apeGuiTreeNamingBlacklist.end() && it->second.count(fp) != 0) {
                return false;
            }
        }
        return true;
    }

    #if DYNAMIC_STATE_ABSTRACTION_ENABLED

    #if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
        /** @brief Resolves naming by rebuilding from cached XML / snapshots and walking the GUI tree when possible. */
        naming::NamingPtr Model::apeNamingResolvedViaTreeWalk(const std::string &activity, uintptr_t stateHash) {
            const std::string actKey = naming::StateKey::canonicalActivityString(activity);
            if (!_apeStateNamingManager) {
                return naming::NamingFactory::defaultRootNaming();
            }
            naming::ActivityNamingManager &mgr = _apeStateNamingManager->activityManager();
            if (stateHash == 0) {
                naming::NamingPtr cur = mgr.getNaming(actKey);
                return cur ? cur : naming::NamingFactory::defaultRootNaming();
            }
            auto itXml = _apeStateXmlByStateHash.find(stateHash);
            if (itXml == _apeStateXmlByStateHash.end() || itXml->second.empty()) {
                naming::NamingPtr cur = mgr.getNaming(actKey);
                return cur ? cur : naming::NamingFactory::defaultRootNaming();
            }
            std::string pkg;
            std::string cls;
            naming::StateKey::splitActivityPackageClass(activity, &pkg, &cls);
            gui_tree::GUITreeBuildResult built;
            if (gui_tree::GUITreePtr snap = apeLatestGuiTreeSnapshot(stateHash)) {
                built = buildGuitreePreferApeSnapshotAndDomXml(itXml->second, pkg, cls, snap);
            } else {
                built = buildGuitreeFromCachedXmlPreferElement(itXml->second, pkg, cls);
            }
            if (!built.tree) {
                naming::NamingPtr cur = mgr.getNaming(actKey);
                return cur ? cur : naming::NamingFactory::defaultRootNaming();
            }
            naming::NamingPtr resolved = built.dom
                ? _apeStateNamingManager->treeToNaming(*built.tree, built.dom)
                : _apeStateNamingManager->treeToNaming(*built.tree);
            if (resolved) {
                return resolved;
            }
            naming::NamingPtr cur = mgr.getNaming(actKey);
            return cur ? cur : naming::NamingFactory::defaultRootNaming();
        }
    #else
        /** @brief Returns current activity-level naming only (no GUI-tree rebuild without pugixml). */
        naming::NamingPtr Model::apeNamingResolvedViaTreeWalk(const std::string &activity, uintptr_t /*stateHash*/) {
            const std::string actKey = naming::StateKey::canonicalActivityString(activity);
            if (!_apeStateNamingManager) {
                return naming::NamingFactory::defaultRootNaming();
            }
            naming::NamingPtr cur = _apeStateNamingManager->activityManager().getNaming(actKey);
            return cur ? cur : naming::NamingFactory::defaultRootNaming();
        }
    #endif
    
    #endif


    /** @brief Evaluates action-side refinement predicates against the current naming and optional trees. */
    bool Model::evalApeActionRefinementPredicates(const std::string &activity, const naming::NamingPtr &naming,
                                                  const std::vector<gui_tree::GUITreePtr> *affectedSourceTrees) {
        if (!naming) {
            return false;
        }
        const std::string actKey = naming::StateKey::canonicalActivityString(activity);
        std::string pkg;
        std::string cls;
        naming::StateKey::splitActivityPackageClass(activity, &pkg, &cls);
        std::unordered_set<const gui_tree::GUITree *> affectedTreeObjects;
        if (affectedSourceTrees) {
            affectedTreeObjects.reserve(affectedSourceTrees->size());
            for (const auto &t : *affectedSourceTrees) {
                if (t) {
                    affectedTreeObjects.insert(t.get());
                }
            }
        }
        auto activityNamingOnly = [&]() -> naming::NamingPtr {
            if (_apeStateNamingManager) {
                naming::NamingPtr cur = _apeStateNamingManager->activityManager().getNaming(actKey);
                return cur ? cur : naming;
            }
            return naming;
        };
        auto namingForApePredicateEval = [&](const gui_tree::GUITreePtr &tree) -> naming::NamingPtr {
            if (tree && !affectedTreeObjects.empty() &&
                affectedTreeObjects.find(tree.get()) != affectedTreeObjects.end()) {
                return naming;
            }
            if (tree && _apeStateNamingManager) {
                naming::NamingPtr t = _apeStateNamingManager->treeToNaming(*tree);
                if (t) {
                    return t;
                }
            }
            return activityNamingOnly();
        };

        auto itStates = _apeStatesFewerThanPredicates.find(actKey);
        if (itStates != _apeStatesFewerThanPredicates.end()) {
            for (const ApeStatesFewerThanPredicate &pred : itStates->second) {
                if (pred.threshold <= 0) {
                    continue;
                }
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
                if (pred.stateHashes.empty() && pred.sourceTrees.empty()) {
#else
                if (pred.stateHashes.empty()) {
#endif
                    continue;
                }

                std::unordered_set<uintptr_t> uniqueStates;
                uniqueStates.reserve(pred.stateHashes.size());
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
                if (!pred.sourceTrees.empty() && pred.sourceTrees.size() == pred.stateHashes.size()) {
                    for (const auto &sourceTree : pred.sourceTrees) {
                        gui_tree::GUITreeBuildResult built =
                            buildGuitreeFromSnapshotObjectOrCachedXml("", pkg, cls, sourceTree);
                        if (!built.tree || !built.dom) {
                            continue;
                        }
                        const naming::NamingPtr nEval = namingForApePredicateEval(built.tree);
                        if (!safeRebuildTree(nEval, *built.tree, built.dom)) {
                            continue;
                        }
                        const uintptr_t h = naming::StateKey::hashFromGUITree(*built.tree);
                        if (h == 0) {
                            continue;
                        }
                        uniqueStates.insert(h);
                        if (uniqueStates.size() > static_cast<size_t>(pred.threshold)) {
                            return false;
                        }
                    }
                    continue;
                }
#endif
                for (uintptr_t sh : pred.stateHashes) {
                    auto itXml = _apeStateXmlByStateHash.find(sh);
                    if (itXml == _apeStateXmlByStateHash.end() || itXml->second.empty()) {
                        continue;
                    }
                    gui_tree::GUITreeBuildResult built;
#if defined(DYNAMIC_STATE_ABSTRACTION_ENABLED) && DYNAMIC_STATE_ABSTRACTION_ENABLED
                    built = buildGuitreeFromSnapshotObjectOrCachedXml(itXml->second, pkg, cls, 
                        apeLatestGuiTreeSnapshot(sh));
#else
                    built = buildGuitreeFromCachedXmlPreferElement(itXml->second, pkg, cls);
#endif
                    if (!built.tree || !built.dom) {
                        continue;
                    }
                    const naming::NamingPtr nEval = namingForApePredicateEval(built.tree);
                    if (!safeRebuildTree(nEval, *built.tree, built.dom)) {
                        continue;
                    }
                    const uintptr_t h = naming::StateKey::hashFromGUITree(*built.tree);
                    if (h == 0) {
                        continue;
                    }
                    uniqueStates.insert(h);
                    if (uniqueStates.size() > static_cast<size_t>(pred.threshold)) {
                        return false;
                    }
                }
            }
        }

        auto itSd = _apeSourceDivergentPredicates.find(actKey);
        if (itSd != _apeSourceDivergentPredicates.end() && !itSd->second.empty()) {
            for (const ApeSourceDivergentPredicate &pred : itSd->second) {
                const bool hasTrees = static_cast<bool>(pred.sourceTreeA) && static_cast<bool>(pred.sourceTreeB);
                if (!hasTrees && (pred.xmlA.empty() || pred.xmlB.empty())) {
                    continue;
                }
                gui_tree::GUITreeBuildResult builtA;
                gui_tree::GUITreeBuildResult builtB;
#if defined(DYNAMIC_STATE_ABSTRACTION_ENABLED) && DYNAMIC_STATE_ABSTRACTION_ENABLED
                builtA = buildGuitreeFromSnapshotObjectOrCachedXml(
                    pred.xmlA, pkg, cls, 
                    pred.sourceTreeA ? pred.sourceTreeA : apeGuiTreeSnapshotForExactCachedXml(pred.xmlA));
                builtB = buildGuitreeFromSnapshotObjectOrCachedXml(
                    pred.xmlB, pkg, cls, 
                    pred.sourceTreeB ? pred.sourceTreeB : apeGuiTreeSnapshotForExactCachedXml(pred.xmlB));
#else
                builtA = buildGuitreeFromCachedXmlPreferElement(pred.xmlA, pkg, cls);
                builtB = buildGuitreeFromCachedXmlPreferElement(pred.xmlB, pkg, cls);
#endif
                if (!builtA.tree || !builtA.dom || !builtB.tree || !builtB.dom) {
                    return false;
                }
                const naming::NamingPtr nA = namingForApePredicateEval(builtA.tree);
                const naming::NamingPtr nB = namingForApePredicateEval(builtB.tree);
                if (!safeRebuildTree(nA, *builtA.tree, builtA.dom) ||
                    !safeRebuildTree(nB, *builtB.tree, builtB.dom)) {
                    return false;
                }
                const uintptr_t hA = naming::StateKey::hashFromGUITree(*builtA.tree);
                const uintptr_t hB = naming::StateKey::hashFromGUITree(*builtB.tree);
                if (hA == 0 || hB == 0) {
                    return false;
                }
                if (hA == hB) {
                    return false;
                }
            }
        }
        auto it = _apeActionRefinementPredicates.find(actKey);
        if (it != _apeActionRefinementPredicates.end() && !it->second.empty()) {
            for (const ApeActionDivergentPredicate &pred : it->second) {
                if ((!pred.sourceTree && pred.sourceXml.empty()) || pred.partitionsStableIds.empty()) {
                    continue;
                }
                gui_tree::GUITreeBuildResult built;
#if defined(DYNAMIC_STATE_ABSTRACTION_ENABLED) && DYNAMIC_STATE_ABSTRACTION_ENABLED
                built = buildGuitreeFromSnapshotObjectOrCachedXml(
                    pred.sourceXml, pkg, cls,
                    pred.sourceTree ? pred.sourceTree : apeLatestGuiTreeSnapshot(pred.sourceStateHash));
#else
                built = buildGuitreeFromCachedXmlPreferElement(pred.sourceXml, pkg, cls);
#endif
                if (!built.tree || !built.dom) {
                    continue;
                }
                const naming::NamingPtr nEval = namingForApePredicateEval(built.tree);
                if (!safeRebuildTree(nEval, *built.tree, built.dom)) {
                    return false;
                }
                std::vector<gui_tree::GUITreeNode *> po;
                collectGUITreeNodesPreOrder(built.tree->getRootNode(), &po);
                if (po.empty()) {
                    continue;
                }
                std::unordered_set<std::string> actions;
                for (const auto &partition : pred.partitionsStableIds) {
                    std::unordered_set<std::string> temp;
                    for (int idx : partition) {
                        if (idx < 0) {
                            continue;
                        }
                        const size_t uidx = static_cast<size_t>(idx);
                        if (uidx >= po.size() || !po[uidx]) {
                            continue;
                        }
                        const naming::NamePtr name = po[uidx]->getXPathName();
                        if (!name) {
                            continue;
                        }
                        const std::string xp = name->toXPath();
                        if (xp.empty()) {
                            continue;
                        }
                        if (temp.insert(xp).second) {
                            if (!actions.insert(xp).second) {
                                return false;
                            }
                        }
                    }
                }
            }
        }
        return true;
    }

    /** @brief Adds a predicate that triggers when an activity has fewer than N abstract states. */
    void Model::addApeStatesFewerThanPredicate(const std::string &activity,
                                               const std::unordered_set<uintptr_t> &affectedStateHashes,
                                               int threshold) {
        if (threshold <= 0 || affectedStateHashes.empty()) {
            return;
        }
        ApeStatesFewerThanPredicate pred;
        pred.threshold = threshold;
        pred.stateHashes.reserve(affectedStateHashes.size());
        for (uintptr_t sh : affectedStateHashes) {
            pred.stateHashes.push_back(sh);
        }
        std::sort(pred.stateHashes.begin(), pred.stateHashes.end());
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
        pred.sourceTrees.reserve(pred.stateHashes.size());
        for (uintptr_t sh : pred.stateHashes) {
            if (gui_tree::GUITreePtr tree = apeLatestGuiTreeSnapshot(sh)) {
                pred.sourceTrees.push_back(tree);
            }
        }
#endif
        const std::string actKey = naming::StateKey::canonicalActivityString(activity);
        _apeStatesFewerThanPredicates[actKey].push_back(std::move(pred));
    }

    /** @brief Registers an action refinement predicate for one activity. */
    void Model::addApeActionRefinementPredicate(const std::string &activity,
                                                uintptr_t sourceStateHash,
                                                const std::string &sourceXml,
                                                const std::vector<int> &resolvedNodeStableIds,
                                                const naming::NamingPtr &updatedNaming) {
        if (!updatedNaming || sourceXml.empty() || resolvedNodeStableIds.empty()) {
            return;
        }
#if !defined(FASTBOT_HAS_PUGIXML) || !FASTBOT_HAS_PUGIXML
        (void)activity;
        (void)sourceXml;
        (void)resolvedNodeStableIds;
#else
        std::string pkg;
        std::string cls;
        naming::StateKey::splitActivityPackageClass(activity, &pkg, &cls);
        gui_tree::GUITreeBuildResult built;
#if defined(DYNAMIC_STATE_ABSTRACTION_ENABLED) && DYNAMIC_STATE_ABSTRACTION_ENABLED
        if (gui_tree::GUITreePtr snap = apeLatestGuiTreeSnapshot(sourceStateHash)) {
            built = buildGuitreePreferApeSnapshotAndDomXml(sourceXml, pkg, cls, snap);
        } else {
            built = buildGuitreeFromCachedXmlPreferElement(sourceXml, pkg, cls);
        }
#else
        built = buildGuitreeFromCachedXmlPreferElement(sourceXml, pkg, cls);
#endif
        if (!built.tree || !built.dom) {
            return;
        }
        if (!safeRebuildTree(updatedNaming, *built.tree, built.dom)) {
            return;
        }
        std::vector<gui_tree::GUITreeNode *> po;
        collectGUITreeNodesPreOrder(built.tree->getRootNode(), &po);
        if (po.empty()) {
            return;
        }
        std::unordered_map<std::string, std::vector<int>> partitions;
        for (int idx : resolvedNodeStableIds) {
            if (idx < 0) {
                continue;
            }
            const size_t uidx = static_cast<size_t>(idx);
            if (uidx >= po.size() || !po[uidx]) {
                continue;
            }
            const naming::NamePtr name = po[uidx]->getXPathName();
            if (!name) {
                continue;
            }
            const std::string xp = name->toXPath();
            if (xp.empty()) {
                continue;
            }
            partitions[xp].push_back(idx);
        }
        if (partitions.size() < 2) {
            return;
        }
        ApeActionDivergentPredicate pred;
        pred.sourceStateHash = sourceStateHash;
        pred.sourceXml = sourceXml;
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
        pred.sourceTree = apeLatestGuiTreeSnapshot(sourceStateHash);
#endif
        pred.partitionsStableIds.reserve(partitions.size());
        for (auto &kv : partitions) {
            if (!kv.second.empty()) {
                pred.partitionsStableIds.push_back(std::move(kv.second));
            }
        }
        if (pred.partitionsStableIds.size() < 2) {
            return;
        }
        const std::string actKey = naming::StateKey::canonicalActivityString(activity);
        _apeActionRefinementPredicates[actKey].push_back(std::move(pred));
#endif
    }

    /** @brief Removes refinement predicates that conflict with a newly added constraint. */
    void Model::removeConflictingApeActionRefinementPredicates(
        const std::string &activity, const naming::NamingPtr &naming,
        const std::unordered_set<uintptr_t> &affectedStateHashes) {
        if (!naming || affectedStateHashes.empty()) {
            return;
        }
        const std::string actKey = naming::StateKey::canonicalActivityString(activity);
        auto it = _apeActionRefinementPredicates.find(actKey);
        if (it == _apeActionRefinementPredicates.end() || it->second.empty()) {
            return;
        }
        std::vector<ApeActionDivergentPredicate> kept;
        kept.reserve(it->second.size());
        std::string pkg;
        std::string cls;
        naming::StateKey::splitActivityPackageClass(activity, &pkg, &cls);
        std::vector<gui_tree::GUITreePtr> affectedTreeKeepalive;
        std::unordered_set<const gui_tree::GUITree *> affectedTreeObjects;
#if defined(DYNAMIC_STATE_ABSTRACTION_ENABLED) && DYNAMIC_STATE_ABSTRACTION_ENABLED
        affectedTreeKeepalive.reserve(affectedStateHashes.size());
        affectedTreeObjects.reserve(affectedStateHashes.size());
        for (uintptr_t sh : affectedStateHashes) {
            if (gui_tree::GUITreePtr t = apeLatestGuiTreeSnapshot(sh)) {
                affectedTreeKeepalive.push_back(t);
                affectedTreeObjects.insert(t.get());
            }
        }
#endif

        auto namingForRollbackPredicateEval = [&](const gui_tree::GUITreePtr &tree) -> naming::NamingPtr {
            if (tree && !affectedTreeObjects.empty() &&
                affectedTreeObjects.find(tree.get()) != affectedTreeObjects.end()) {
                return naming;
            }
            if (tree && _apeStateNamingManager) {
                naming::NamingPtr t = _apeStateNamingManager->treeToNaming(*tree);
                if (t) {
                    return t;
                }
            }
            if (_apeStateNamingManager) {
                naming::NamingPtr cur = _apeStateNamingManager->activityManager().getNaming(actKey);
                return cur ? cur : naming;
            }
            return naming;
        };
        for (const auto &pred : it->second) {
            if (pred.sourceXml.empty() || pred.partitionsStableIds.empty()) {
                continue;
            }
            gui_tree::GUITreeBuildResult built;
#if defined(DYNAMIC_STATE_ABSTRACTION_ENABLED) && DYNAMIC_STATE_ABSTRACTION_ENABLED
            built = buildGuitreeFromSnapshotObjectOrCachedXml(
                pred.sourceXml, pkg, cls, apeLatestGuiTreeSnapshot(pred.sourceStateHash));
#else
            built = buildGuitreeFromCachedXmlPreferElement(pred.sourceXml, pkg, cls);
#endif
            if (!built.tree || !built.dom) {
                continue;
            }
            const naming::NamingPtr nPred = namingForRollbackPredicateEval(built.tree);
            if (!nPred || !safeRebuildTree(nPred, *built.tree, built.dom)) {
                continue;
            }
            std::vector<gui_tree::GUITreeNode *> po;
            collectGUITreeNodesPreOrder(built.tree->getRootNode(), &po);
            if (po.empty()) {
                continue;
            }
            bool valid = true;
            std::unordered_set<std::string> actions;
            for (const auto &partition : pred.partitionsStableIds) {
                std::unordered_set<std::string> temp;
                for (int idx : partition) {
                    if (idx < 0) {
                        continue;
                    }
                    const size_t uidx = static_cast<size_t>(idx);
                    if (uidx >= po.size() || !po[uidx]) {
                        continue;
                    }
                    const naming::NamePtr name = po[uidx]->getXPathName();
                    if (!name) {
                        continue;
                    }
                    const std::string xp = name->toXPath();
                    if (xp.empty()) {
                        continue;
                    }
                    if (temp.insert(xp).second) {
                        if (!actions.insert(xp).second) {
                            valid = false;
                            break;
                        }
                    }
                }
                if (!valid) {
                    break;
                }
            }
            if (valid) {
                kept.push_back(pred);
            }
        }
        it->second.swap(kept);
    }

    /** @brief Removes conflicting "fewer than N states" predicates. */
    void Model::removeConflictingApeStatesFewerThanPredicates(
        const std::string &activity, const naming::NamingPtr &naming,
        const std::unordered_set<uintptr_t> &affectedStateHashes) {
        if (!naming || affectedStateHashes.empty()) {
            return;
        }
        (void)affectedStateHashes;
        const std::string actKey = naming::StateKey::canonicalActivityString(activity);
        auto it = _apeStatesFewerThanPredicates.find(actKey);
        if (it == _apeStatesFewerThanPredicates.end() || it->second.empty()) {
            return;
        }
        std::vector<ApeStatesFewerThanPredicate> kept;
        kept.reserve(it->second.size());
        for (const auto &pred : it->second) {
            if (pred.threshold <= 0 || pred.stateHashes.empty()) {
                continue;
            }
            std::unordered_set<uintptr_t> uniqueStates;
            uniqueStates.reserve(pred.stateHashes.size());
            bool valid = true;
            for (uintptr_t sh : pred.stateHashes) {
                auto itXml = _apeStateXmlByStateHash.find(sh);
                if (itXml == _apeStateXmlByStateHash.end() || itXml->second.empty()) {
                    continue;
                }
                uintptr_t h = 0;
#if defined(DYNAMIC_STATE_ABSTRACTION_ENABLED) && DYNAMIC_STATE_ABSTRACTION_ENABLED
                gui_tree::GUITreePtr snapFew = apeLatestGuiTreeSnapshot(sh);
                const gui_tree::GUITreePtr *snapFewPtr = snapFew ? &snapFew : nullptr;
                if (!apeStateHashFromXmlWithNaming(activity, itXml->second, naming, &h, 0, nullptr, snapFewPtr) ||
                    h == 0) {
                    continue;
                }
#else
                if (!apeStateHashFromXmlWithNaming(activity, itXml->second, naming, &h) || h == 0) {
                    continue;
                }
#endif
                uniqueStates.insert(h);
                if (uniqueStates.size() > static_cast<size_t>(pred.threshold)) {
                    valid = false;
                    break;
                }
            }
            if (valid) {
                kept.push_back(pred);
            }
        }
        it->second.swap(kept);
    }

    /** @brief Registers a predicate for divergent source XML pairs within an activity. */
    void Model::addApeSourceDivergentPredicate(const std::string &activity, const std::string &xmlA,
                                               const std::string &xmlB, uintptr_t sharedSourceStateHash) {
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
        if (xmlA.empty() || xmlB.empty() || xmlA == xmlB) {
            return;
        }
        const std::string actKey = naming::StateKey::canonicalActivityString(activity);
        ApeSourceDivergentPredicate pred;
        pred.xmlA = xmlA;
        pred.xmlB = xmlB;
        pred.sharedSourceStateHash = sharedSourceStateHash;
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        pred.sourceTreeA = apeGuiTreeSnapshotForExactCachedXml(xmlA);
        pred.sourceTreeB = apeGuiTreeSnapshotForExactCachedXml(xmlB);
        if (!pred.sourceTreeA && sharedSourceStateHash != 0) {
            pred.sourceTreeA = apeLatestGuiTreeSnapshot(sharedSourceStateHash);
        }
        if (!pred.sourceTreeB && sharedSourceStateHash != 0) {
            pred.sourceTreeB = apeLatestGuiTreeSnapshot(sharedSourceStateHash);
        }
#endif
        _apeSourceDivergentPredicates[actKey].push_back(std::move(pred));
#else
        (void)activity;
        (void)xmlA;
        (void)xmlB;
        (void)sharedSourceStateHash;
#endif
    }

    /** @brief Removes conflicting source-divergence predicates. */
    void Model::removeConflictingApeSourceDivergentPredicates(
        const std::string &activity, const naming::NamingPtr &naming,
        const std::unordered_set<uintptr_t> &affectedStateHashes) {
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
        if (!naming || affectedStateHashes.empty()) {
            return;
        }
        (void)affectedStateHashes;
        const std::string actKey = naming::StateKey::canonicalActivityString(activity);
        auto it = _apeSourceDivergentPredicates.find(actKey);
        if (it == _apeSourceDivergentPredicates.end() || it->second.empty()) {
            return;
        }
        std::vector<ApeSourceDivergentPredicate> kept;
        kept.reserve(it->second.size());
        for (const auto &pred : it->second) {
            if (pred.xmlA.empty() || pred.xmlB.empty()) {
                continue;
            }
            uintptr_t ha = 0;
            uintptr_t hb = 0;
#if defined(DYNAMIC_STATE_ABSTRACTION_ENABLED) && DYNAMIC_STATE_ABSTRACTION_ENABLED
            gui_tree::GUITreePtr snapA = apeGuiTreeSnapshotForExactCachedXml(pred.xmlA);
            gui_tree::GUITreePtr snapB = apeGuiTreeSnapshotForExactCachedXml(pred.xmlB);
            const gui_tree::GUITreePtr *snapAPtr = snapA ? &snapA : nullptr;
            const gui_tree::GUITreePtr *snapBPtr = snapB ? &snapB : nullptr;
            if (!apeStateHashFromXmlWithNaming(activity, pred.xmlA, naming, &ha, 0, nullptr, snapAPtr) ||
                ha == 0 ||
                !apeStateHashFromXmlWithNaming(activity, pred.xmlB, naming, &hb, 0, nullptr, snapBPtr) ||
                hb == 0) {
                continue;
            }
#else
            if (!apeStateHashFromXmlWithNaming(activity, pred.xmlA, naming, &ha) || ha == 0 ||
                !apeStateHashFromXmlWithNaming(activity, pred.xmlB, naming, &hb) || hb == 0) {
                continue;
            }
#endif
            if (ha != hb) {
                kept.push_back(pred);
            }
        }
        it->second.swap(kept);
#else
        (void)activity;
        (void)naming;
        (void)affectedStateHashes;
#endif
    }

    /** @brief Blacklists finer naming choices when rolling back an unstable refinement step. */
    void Model::apeBlacklistFinerNamingOnRollback(
        const std::string &activity, const naming::NamingPtr &finerNaming,
        const ApeNamingAbstractionContext &ctx, const std::unordered_set<uintptr_t> &affectedStateHashesForBlacklist) {
        (void)ctx;
        if (!finerNaming || affectedStateHashesForBlacklist.empty()) {
            return;
        }
        const std::string fp = finerNaming->fingerprintString();
        // blacklistRefinement blacklists exactly the affected GUI trees.
        // Native uses state-hash keyed cache for GUI-tree naming blacklists, so we blacklist
        // by those affected state hashes directly.
        for (uintptr_t sh : affectedStateHashesForBlacklist) {
            _apeGuiTreeNamingBlacklist[sh].insert(fp);
        }
        apeCapGuiTreeNamingBlacklist();
        apeCapApeNamingCoarsenAndRefineBlacklists();
    }

    /** @brief Enforces maximum sizes on coarsen/refine blacklist tables. */
    void Model::apeCapApeNamingCoarsenAndRefineBlacklists() {
        // no-op: match unbounded blacklists (NDActionBlacklist/guiTreeNamingBlaclist).
    }

    /** @brief Returns human-readable hints where naming or transitions look non-deterministic. */
    std::vector<std::string> Model::detectNonDeterminismApe() const {
        const int minTargets = (_preference ? _preference->getApeNamingMinNonDetTargets()
                                            : MinNonDeterminismCount);
        std::set<std::string> activitiesSet;
        for (const auto &kv : _apePairAgg) {
            if (kv.second.targetCounts.size() >= static_cast<size_t>(minTargets)) {
                activitiesSet.insert(kv.second.sourceActivity);
            }
        }
        return std::vector<std::string>(activitiesSet.begin(), activitiesSet.end());
    }

#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
    /** @brief Resolves widget XPath-side expressions and parent namelets for split/refinement. */
    bool Model::resolveApeWidgetExprAndParentNamelet(uintptr_t stateHash, const std::string &activityForSplit,
                                                     const naming::NamingPtr &cur, const WidgetPtr &targetWidget,
                                                     std::string *outExpr, naming::NameletPtr *outParent) const {
        if (!outExpr || !outParent) {
            return false;
        }
        outExpr->clear();
        *outParent = nullptr;
        if (!targetWidget || !targetWidget->getBounds() || !cur) {
            return false;
        }

        std::string pkg;
        std::string cls;
        naming::StateKey::splitActivityPackageClass(activityForSplit, &pkg, &cls);
        gui_tree::GUITreeBuildResult built;
        auto itEl = _apeStateElementByStateHash.find(stateHash);
        if (itEl != _apeStateElementByStateHash.end() && itEl->second) {
            built = gui_tree::GUITreeFactory::buildFromElement(itEl->second, pkg, cls);
        }
        if (!built.tree || !built.dom) {
            auto itXml = _apeStateXmlByStateHash.find(stateHash);
            if (itXml == _apeStateXmlByStateHash.end() || itXml->second.empty()) {
                return false;
            }
            built = buildGuitreePreferApeSnapshotAndDomXml(itXml->second, pkg, cls, 
                apeLatestGuiTreeSnapshot(stateHash));
        }
        if (!built.tree || !built.dom) {
            return false;
        }
        if (!safeRebuildTree(cur, *built.tree, built.dom, "target_resolve")) {
            return false;
        }
        std::vector<gui_tree::GUITreeNode *> po;
        collectGUITreeNodesPreOrder(built.tree->getRootNode(), &po);
        if (po.empty()) {
            return false;
        }
        const Rect targetRect = *targetWidget->getBounds();
        for (gui_tree::GUITreeNode *node : po) {
            if (!node) {
                continue;
            }
            if (!(node->getBounds() == targetRect)) {
                continue;
            }
            const naming::NamePtr nm = node->getXPathName();
            if (nm) {
                *outExpr = nm->toXPath();
                if (!outExpr->empty()) {
                    break;
                }
            }
        }
        if (outExpr->empty()) {
            return false;
        }
        for (gui_tree::GUITreeNode *node : po) {
            if (!node) {
                continue;
            }
            const naming::NamePtr nm = node->getXPathName();
            if (!nm || nm->toXPath() != *outExpr) {
                continue;
            }
            naming::NameletPtr nl = node->getCurrentNamelet();
            if (!nl) {
                continue;
            }
            if (!*outParent) {
                *outParent = nl;
            } else if ((*outParent).get() != nl.get()) {
                outParent->reset();
                return false;
            }
        }
        return static_cast<bool>(*outParent);
    }
#endif

    /** @brief Overload that forwards to the full refine implementation (no failure-reason output). */
    bool Model::refineActivityApeNaming(const std::string &activity, const ApeRefinePair *pair,
                                        int precomputedActivityNonDetPairCount) {
        return refineActivityApeNaming(activity, pair, precomputedActivityNonDetPairCount, nullptr);
    }

    /** @brief Attempts naming refinement for one activity using logged transition evidence (full implementation). */
    bool Model::refineActivityApeNaming(const std::string &activity, const ApeRefinePair *pair,
                                        int precomputedActivityNonDetPairCount,
                                        ApeRefineFailReason *outFailReason) {
        if (outFailReason) {
            *outFailReason = ApeRefineFailReason::Other;
        }
        const std::string actKey = naming::StateKey::canonicalActivityString(activity);
        const int minTargets = (_preference ? _preference->getApeNamingMinNonDetTargets()
                                            : MinNonDeterminismCount);
        int nonDetPairs = 0;
        size_t dominantPairTargets = 0;
        uintptr_t dominantSourceKeyHash = 0;
        uintptr_t dominantActionHash = 0;
        uintptr_t dominantActionIdentity = 0;
        std::unordered_set<uintptr_t> dominantTargetKeyHashes;
        auto resolveActionIdentityForPair = [&](uintptr_t srcKeyHash, uintptr_t actionHash) -> uintptr_t {
            if (srcKeyHash == 0 || actionHash == 0) {
                return 0;
            }
            uintptr_t resolved = 0;
            uint64_t maxSeq = 0;
            for (const auto &te : _apeTransitionLog) {
                if (!te.valid || te.sourceActivity != actKey) {
                    continue;
                }
                if (te.sourceKeyHash != srcKeyHash || te.actionHash != actionHash || te.actionIdentity == 0) {
                    continue;
                }
                if (te.transitionSeq >= maxSeq) {
                    maxSeq = te.transitionSeq;
                    resolved = te.actionIdentity;
                }
            }
            return resolved;
        };
        auto isBackActionHashForActivity = [&](uintptr_t actionHash) -> bool {
            if (actionHash == 0) {
                return false;
            }
            for (const auto &te : _apeTransitionLog) {
                if (!te.valid || te.sourceActivity != actKey || te.actionHash != actionHash) {
                    continue;
                }
                if (te.actionType == ActionType::BACK) {
                    return true;
                }
            }
            return false;
        };
        const bool pairScopedCall = (pair != nullptr);
        if (pairScopedCall) {
            if (pair->sourceKeyHash == 0 || pair->actionHash == 0 || pair->targetCount < 2) {
                if (outFailReason) {
                    *outFailReason = ApeRefineFailReason::PairTargetsInsufficient;
                }
                return false;
            }
            if (isBackActionHashForActivity(pair->actionHash)) {
                BDLOG("naming: skip refine activity=%s reason=back_action srcKey=%lu act=%lu",
                      activity.c_str(), (unsigned long)pair->sourceKeyHash,
                      (unsigned long)pair->actionHash);
                if (outFailReason) {
                    *outFailReason = ApeRefineFailReason::Other;
                }
                return false;
            }
            nonDetPairs = (precomputedActivityNonDetPairCount >= 0)
                          ? precomputedActivityNonDetPairCount
                          : 1;
            dominantPairTargets = pair->targetCount;
            dominantSourceKeyHash = pair->sourceKeyHash;
            dominantActionHash = pair->actionHash;
            dominantActionIdentity = pair->actionIdentity;
            if (dominantActionIdentity == 0) {
                dominantActionIdentity = resolveActionIdentityForPair(pair->sourceKeyHash, pair->actionHash);
            }
            dominantTargetKeyHashes = pair->targetKeyHashes;
        } else {
            for (const auto &kv : _apePairAgg) {
                if (kv.second.sourceActivity != actKey) {
                    continue;
                }
                const auto &tm = kv.second.targetCounts;
                if (tm.size() < static_cast<size_t>(minTargets)) {
                    continue;
                }
                if (isBackActionHashForActivity(kv.first.actionHash)) {
                    continue;
                }
                nonDetPairs++;
                if (tm.size() > dominantPairTargets) {
                    dominantPairTargets = tm.size();
                    dominantSourceKeyHash = kv.first.sourceKeyHash;
                    dominantActionHash = kv.first.actionHash;
                    dominantActionIdentity =
                        resolveActionIdentityForPair(kv.first.sourceKeyHash, kv.first.actionHash);
                    dominantTargetKeyHashes.clear();
                    tm.forEach([&](uintptr_t h, int /*count*/) {
                        dominantTargetKeyHashes.insert(h);
                    });
                }
            }
        }
        if (dominantSourceKeyHash != 0 || dominantActionHash != 0) {
            ApeActivityRebuildStats &st = _apeRebuildStatsByActivity[actKey];
            if (dominantActionHash != 0) {
                ++st.actionBlacklistChecks;
            }
            if (dominantActionIdentity != 0 &&
                _apeRefineActionIdentityBlacklist.count(dominantActionIdentity) != 0) {
                if (dominantActionIdentity != 0) {
                    ++st.actionBlacklistHits;
                }
                BDLOG("naming: skip refine activity=%s reason=trigger action blacklisted srcKey=%lu act=%lu",
                      activity.c_str(), (unsigned long)dominantSourceKeyHash, (unsigned long)dominantActionHash);
                BDLOG("naming: action_blacklisted detail activity=%s srcKey=%lu act=%lu "
                      "actId=%lu blacklistSize=%zu checks=%d hits=%d nonDetPairs=%d dominantTargets=%zu",
                      activity.c_str(), (unsigned long)dominantSourceKeyHash,
                      (unsigned long)dominantActionHash, (unsigned long)dominantActionIdentity,
                      _apeRefineActionIdentityBlacklist.size(),
                      st.actionBlacklistChecks, st.actionBlacklistHits, nonDetPairs, dominantPairTargets);
                if (outFailReason) {
                    *outFailReason = ApeRefineFailReason::ActionBlacklisted;
                }
                return false;
            }
        }
        naming::ActivityNamingManager &mgr = _apeStateNamingManager->activityManager();
        naming::NamingPtr cur = mgr.getNaming(actKey);
        if (!cur) {
            cur = naming::NamingFactory::defaultRootNaming();
            if (!cur) {
                BDLOG("naming: skip refine activity=%s reason=no default root naming", activity.c_str());
                if (outFailReason) {
                    *outFailReason = ApeRefineFailReason::NoDefaultRootNaming;
                }
                return false;
            }
            _apeStateNamingManager->updateNaming(actKey, naming::NamingUpdateKind::Refine, cur);
            pruneDivergentApeStatesForActivity(actKey);
        }
        const size_t activityStateCount = getApeStateCountByActivityAndNamingFingerprint(
            actKey, cur ? cur->fingerprintString() : std::string());
        const size_t graphStatesInActivity =
            apeGraphActivityStateCountLikeJavaActivityNode(_graph, actKey);
        const int apeMaxStatesPerActivity =
            _preference ? _preference->getApeMaxStatesPerActivity() : 10;
        const int apeMaxGuitreesPerState = _preference ? _preference->getApeMaxGuitreesPerState() : 20;
        // Design intent: cap distinct states per activity and GUI trees per logical state.
        // Reference NamingFactory.refine historically used an.getStates().size() for the
        // second check; Fastbot uses rep-state guitrees (see log: maxGuitreesPerState).
        if (static_cast<int>(graphStatesInActivity) > apeMaxStatesPerActivity) {
            BDLOG("naming: skip refine activity=%s reason=maxStatesPerActivity graphStates=%zu max=%d",
                  activity.c_str(), graphStatesInActivity, apeMaxStatesPerActivity);
            BDLOG("naming: maxStatesPerActivity detail activity=%s namingFingerprint=%s namingScopedStates=%zu",
                  activity.c_str(), cur ? cur->fingerprintString().c_str() : "null", activityStateCount);
            if (outFailReason) {
                *outFailReason = ApeRefineFailReason::MaxStatesPerActivity;
            }
            return false;
        }
        // Second gate: GUI trees on the representative ND source state (NamingFactory.refine:
        // state.getGUITrees().size() > maxGUITreesPerState), not activity-wide graph state count.
        uintptr_t repStateHash = 0;
        if (dominantSourceKeyHash != 0 && _graph) {
            for (const StatePtr &sp : _graph->getStates()) {
                if (!sp) {
                    continue;
                }
                uintptr_t kH = 0;
                if (!tryGetApeStateKeyHash(sp->hash(), &kH, actKey, dominantSourceKeyHash) ||
                    kH != dominantSourceKeyHash) {
                    continue;
                }
                auto ap = sp->getActivityString();
                const std::string ac =
                    (ap && ap.get()) ? naming::StateKey::canonicalActivityString(*ap) : std::string();
                if (ac != actKey) {
                    continue;
                }
                repStateHash = sp->hash();
                break;
            }
        }
        size_t guitreesInRepState = 0;
        if (repStateHash != 0) {
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
            auto itSn = _apeGuiTreeSnapshotsByStateHash.find(repStateHash);
            if (itSn != _apeGuiTreeSnapshotsByStateHash.end()) {
                guitreesInRepState = itSn->second.size();
            }
#endif
            if (guitreesInRepState == 0) {
                size_t n = 0;
                for (const auto &te : _apeTransitionLog) {
                    if (te.valid && te.sourceStateHash == repStateHash) {
                        ++n;
                    }
                }
                if (n > 0) {
                    guitreesInRepState = n;
                }
            }
        }
        if (static_cast<int>(guitreesInRepState) > apeMaxGuitreesPerState) {
            BDLOG("naming: skip refine activity=%s reason=maxGuitreesPerState guitreesInRepState=%zu max=%d "
                  "(state.getGUITrees().size(); NamingFactory.refine)",
                  activity.c_str(), guitreesInRepState, apeMaxGuitreesPerState);
            if (outFailReason) {
                *outFailReason = ApeRefineFailReason::MaxGuitreesPerState;
            }
            return false;
        }
        naming::NamerLattice lat(naming::NamerFactory::current());
        std::set<std::string> blk;
        for (const auto &p : _apeNamingCoarseningBlacklist) {
            if (p.first == actKey) {
                blk.insert(p.second);
            }
        }
        // XML-space remapped trigger hashes (align Element->GUITree hash space to XML->GUITree space
        // so coarsen-gate hash comparisons are consistent with cached-XML rebuild via buildGuitreeFromCachedXmlPreferElement).
        uintptr_t xmlSpaceTriggerSourceKeyHash = dominantSourceKeyHash;
        std::unordered_set<uintptr_t> xmlSpaceTriggerTargetKeyHashes = dominantTargetKeyHashes;
        std::vector<naming::NamingPtr> candidates;
        struct CandidateEval {
            naming::NamingPtr naming;
            int finenessGain{0};
            bool strictFiner{false};
            bool fingerprintChanged{false};
            size_t partitionTotal{0};
            size_t anchorIdx{0};
            naming::NamerPtr refinedNamer{nullptr};
            /// RefinementResult.originalNamelet.getNamer() (comparator pass 2).
            naming::NamerPtr originalNameletNamer{nullptr};
            bool useApeJavaStyleComparator{false};
            /** Last appended namelet expr (RefinementResult.updatedNamelet.getExprString() tie-break). */
            std::string updatedNameletExpr;
            uintptr_t actionRefineSourceStateHash{0};
            std::string actionRefineSourceXml;
            std::vector<int> actionRefineResolvedNodeStableIds;
            bool registerApeSourceDivergentPredicate{false};
        };
        std::vector<uintptr_t> guiTreeBlacklistCheckHashes;
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
        // Build stateHash -> StatePtr and keyHash -> stateHash indices once
        // (avoids repeated O(N) graph scans throughout refine).
        std::unordered_map<uintptr_t, StatePtr> stateByHash;
        std::unordered_map<uintptr_t, std::vector<uintptr_t>> stateHashesByKeyHash;
        {
            const auto &allStates = getGraph()->getStates();
            stateByHash.reserve(allStates.size());
            stateHashesByKeyHash.reserve(64);
            for (const auto &sp : allStates) {
                if (!sp) {
                    continue;
                }
                stateByHash[sp->hash()] = sp;
                uintptr_t kH = 0;
                auto ap = sp->getActivityString();
                const std::string activityKey =
                    (ap && ap.get()) ? naming::StateKey::canonicalActivityString(*ap) : std::string();
                if (tryGetApeStateKeyHash(sp->hash(), &kH, activityKey)) {
                    stateHashesByKeyHash[kH].push_back(sp->hash());
                }
            }
        }
        std::vector<uintptr_t> triggerSourceStateHashesForReplay;
        if (dominantSourceKeyHash != 0) {
            auto itSrc = stateHashesByKeyHash.find(dominantSourceKeyHash);
            if (itSrc != stateHashesByKeyHash.end()) {
                triggerSourceStateHashesForReplay = itSrc->second;
            }
        }
        guiTreeBlacklistCheckHashes = triggerSourceStateHashesForReplay;
        bool replayActive = false;
        std::string replaySrcXml;
        uintptr_t replaySrcStateHash = 0;
        std::vector<uintptr_t> replayTgtStateHashes;
        if (dominantPairTargets >= static_cast<size_t>(minTargets) && dominantSourceKeyHash != 0 &&
            !dominantTargetKeyHashes.empty()) {
            auto findRepresentativeStateHashForKey = [&](uintptr_t keyH) -> uintptr_t {
                auto it = stateHashesByKeyHash.find(keyH);
                if (it == stateHashesByKeyHash.end()) {
                    return static_cast<uintptr_t>(0);
                }
                // Prefer one with cached XML (replay requires XML).
                for (uintptr_t sh : it->second) {
                    auto itXml = _apeStateXmlByStateHash.find(sh);
                    if (itXml != _apeStateXmlByStateHash.end() && !itXml->second.empty()) {
                        return sh;
                    }
                }
                return it->second.empty() ? static_cast<uintptr_t>(0) : it->second.front();
            };
            replaySrcStateHash = findRepresentativeStateHashForKey(dominantSourceKeyHash);
            if (replaySrcStateHash != 0) {
                auto itS = _apeStateXmlByStateHash.find(replaySrcStateHash);
                if (itS != _apeStateXmlByStateHash.end() && !itS->second.empty()) {
                    replaySrcXml = itS->second;
                }
            }
            replayTgtStateHashes.reserve(dominantTargetKeyHashes.size());
            for (uintptr_t th : dominantTargetKeyHashes) {
                const uintptr_t tsh = findRepresentativeStateHashForKey(th);
                if (tsh == 0) {
                    continue;
                }
                auto itT = _apeStateXmlByStateHash.find(tsh);
                if (itT != _apeStateXmlByStateHash.end() && !itT->second.empty()) {
                    replayTgtStateHashes.push_back(tsh);
                }
            }
            replayActive =
                !replaySrcXml.empty() &&
                replayTgtStateHashes.size() >= static_cast<size_t>(minTargets);
            if (!replayActive && dominantPairTargets >= static_cast<size_t>(minTargets)) {
                BDLOG("naming: replay skipped activity=%s haveSrcXml=%d tgtXml=%zu needTgt=%zu",
                      activity.c_str(), replaySrcStateHash != 0 ? 1 : 0, replayTgtStateHashes.size(),
                      dominantTargetKeyHashes.size());
            }
        }
        // Remap dominantSourceKeyHash / dominantTargetKeyHashes from Element->GUITree hash space
        // to XML->GUITree hash space. apeStateHashFromXmlWithNaming rebuilds from cached XML via
        // buildGuitreeFromCachedXmlPreferElement (Element parse + buildFromElement, else pugixml).
        // Residual divergence vs the live Element tree can remain (scrollable/edit-text normalization, etc.).
        {
            auto remapKeyHashToXmlSpace = [&](uintptr_t elementKeyHash) -> uintptr_t {
                if (elementKeyHash == 0) {
                    return 0;
                }
                auto itBucket = stateHashesByKeyHash.find(elementKeyHash);
                if (itBucket == stateHashesByKeyHash.end()) {
                    return elementKeyHash;
                }
                for (uintptr_t sh : itBucket->second) {
                    auto itXml = _apeStateXmlByStateHash.find(sh);
                    if (itXml == _apeStateXmlByStateHash.end() || itXml->second.empty()) {
                        continue;
                    }
                    uintptr_t xmH = 0;
                    gui_tree::GUITreePtr snapRm = this->apeLatestGuiTreeSnapshot(sh);
                    const gui_tree::GUITreePtr *snapPtr = snapRm ? &snapRm : nullptr;
                    if (apeStateHashFromXmlWithNaming(activity, itXml->second, cur, &xmH, 0, nullptr,
                                                      snapPtr) &&
                        xmH != 0) {
                        return xmH;
                    }
                }
                return elementKeyHash;
            };
            xmlSpaceTriggerSourceKeyHash = remapKeyHashToXmlSpace(dominantSourceKeyHash);
            if (xmlSpaceTriggerSourceKeyHash != dominantSourceKeyHash) {
                BDLOG("naming: remap triggerSource %lu -> %lu (xml-space)",
                      (unsigned long)dominantSourceKeyHash, (unsigned long)xmlSpaceTriggerSourceKeyHash);
            }
            xmlSpaceTriggerTargetKeyHashes.clear();
            for (uintptr_t tkh : dominantTargetKeyHashes) {
                xmlSpaceTriggerTargetKeyHashes.insert(remapKeyHashToXmlSpace(tkh));
            }
        }
#else
        const bool replayActive = false;
#endif
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        {
            if (guiTreeBlacklistCheckHashes.empty() && dominantSourceKeyHash != 0) {
                auto itSrcBlk = stateHashesByKeyHash.find(dominantSourceKeyHash);
                if (itSrcBlk != stateHashesByKeyHash.end()) {
                    guiTreeBlacklistCheckHashes = itSrcBlk->second;
                }
            }
        }
#endif
        std::vector<gui_tree::GUITreePtr> predicateAffectedSourceTrees;
        std::unordered_set<const gui_tree::GUITree *> predicateAffectedSeen;
        predicateAffectedSourceTrees.reserve(guiTreeBlacklistCheckHashes.size());
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        for (uintptr_t sh : guiTreeBlacklistCheckHashes) {
            gui_tree::GUITreePtr snap = apeLatestGuiTreeSnapshot(sh);
            if (!snap) {
                continue;
            }
            if (predicateAffectedSeen.insert(snap.get()).second) {
                predicateAffectedSourceTrees.push_back(std::move(snap));
            }
        }
#endif
        auto checkRefinementPredicate = [&](const naming::NamingPtr &cand) -> bool {
            if (!cand) {
                return false;
            }
            if (!evalApeGuiTreeNamingBlacklist(guiTreeBlacklistCheckHashes, cand)) {
                return false;
            }
            return this->evalApeActionRefinementPredicates(activity, cand, &predicateAffectedSourceTrees);
        };
        std::vector<CandidateEval> accepted;
        accepted.reserve(32);
        bool filledViaAcceptedCandidates = false;
        std::string apeRefineNdBranchAXml;
        std::string apeRefineNdBranchBXml;
        uintptr_t apeRefineNdSharedSrcStateHash = 0;
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
        std::vector<std::string> branchAXml;
        std::vector<std::string> branchBXml;
        std::vector<NondetTreeTransitionBranchPair::SourceTransition> branchATransitions;
        std::vector<NondetTreeTransitionBranchPair::SourceTransition> branchBTransitions;
        uintptr_t brSharedSrcStateHash = 0;
        std::vector<NondetTreeTransitionBranchPair> branchPairs;
        if (dominantSourceKeyHash != 0 && dominantActionHash != 0 &&
            dominantTargetKeyHashes.size() >= static_cast<size_t>(minTargets) &&
            !_apeTransitionLog.empty()) {
            const uint64_t nstSeq = (pair ? pair->nstTransitionSeq : 0);
            NondetBranchCollectionResult branchCollect = collectNondetBranchPairsForRefine(
                _apeTransitionLog, _apeTransitionLogWriteIndex, _apeTreeTransitionLog, actKey,
                dominantSourceKeyHash, dominantActionHash, dominantTargetKeyHashes,
                [&](uintptr_t sourceStateHash) {
                    if (!pair || !pair->hasSourceStateKey) {
                        return true;
                    }
                    auto itKeys = _ape_state_keys_by_hash.find(sourceStateHash);
                    if (itKeys == _ape_state_keys_by_hash.end()) {
                        return false;
                    }
                    for (const naming::StateKey &k : itKeys->second) {
                        if (k == pair->sourceStateKey) {
                            return true;
                        }
                    }
                    return false;
                },
                nstSeq);
            BDLOG("naming refine: branch_input_stats activity=%s srcKey=%zu act=%zu logN=%zu ordered=%zu "
                  "dropPair=%zu dropTarget=%zu dropSnapshot=%zu dropSrcStateKey=%zu",
                  actKey.c_str(), dominantSourceKeyHash, dominantActionHash, branchCollect.inputStats.logN,
                  branchCollect.inputStats.orderedCount, branchCollect.inputStats.filteredByActivityOrPair,
                  branchCollect.inputStats.filteredByTarget, branchCollect.inputStats.filteredBySnapshot,
                  branchCollect.inputStats.filteredBySourceStateKey);
            branchPairs = std::move(branchCollect.branchPairs);
            if (nstSeq == 0) {
                BDLOG("naming refine: skip pair reason=nst_transition_seq_not_found activity=%s "
                      "srcKeyHash=%zu actionHash=%zu nstSeq=%llu observed=%zu",
                      actKey.c_str(), dominantSourceKeyHash, dominantActionHash,
                      static_cast<unsigned long long>(nstSeq), branchCollect.orderedCount);
            } else if (!branchCollect.nstSeqFound) {
                BDLOG("naming refine: skip pair reason=nst_transition_seq_not_found activity=%s "
                      "srcKeyHash=%zu actionHash=%zu nstSeq=%llu observed=%zu",
                      actKey.c_str(), dominantSourceKeyHash, dominantActionHash,
                      static_cast<unsigned long long>(nstSeq), branchCollect.orderedCount);
            }
            BDLOG("naming refine: branch_pairs_stats activity=%s srcKey=%zu act=%zu pairs=%zu",
                  actKey.c_str(), dominantSourceKeyHash, dominantActionHash, branchPairs.size());
            for (size_t i = 0; i < branchPairs.size(); ++i) {
                const auto &bp = branchPairs[i];
                BDLOG("naming refine: branch_pair_detail activity=%s pairIdx=%zu srcStateHash=%zu "
                      "targetKeyA=%zu targetKeyB=%zu nstTargetStateHash=%zu firstSeenSeq=%llu "
                      "A.count=%zu B.count=%zu A.transitions=%s B.transitions=%s",
                      actKey.c_str(), i, static_cast<size_t>(bp.sourceStateHash),
                      static_cast<size_t>(bp.targetKeyA), static_cast<size_t>(bp.targetKeyB),
                      static_cast<size_t>(bp.nstTargetStateHash),
                      static_cast<unsigned long long>(bp.firstSeenSeq), bp.branchATransitions.size(),
                      bp.branchBTransitions.size(), apeTransitionListDebugSummary(bp.branchATransitions).c_str(),
                      apeTransitionListDebugSummary(bp.branchBTransitions).c_str());
                for (const auto &st : bp.branchATransitions) {
                    if (st.sourceGuiSnapshot &&
                        predicateAffectedSeen.insert(st.sourceGuiSnapshot.get()).second) {
                        predicateAffectedSourceTrees.push_back(st.sourceGuiSnapshot);
                    }
                }
                for (const auto &st : bp.branchBTransitions) {
                    if (st.sourceGuiSnapshot &&
                        predicateAffectedSeen.insert(st.sourceGuiSnapshot.get()).second) {
                        predicateAffectedSourceTrees.push_back(st.sourceGuiSnapshot);
                    }
                }
            }
        }
        bool haveXmlBranches = false;
        const bool arFirst = _preference && _preference->useApeNamingActionRefinementFirst();
        const bool enableReplacingNamelet =
            _preference && _preference->useApeNamingEnableReplacingNamelet();
        // refine() uses a single source State (st1.getSource() == st2.getSource()).
        StatePtr nondetSrcState;
        auto tryApeActionRefinement = [&]() {
            if (!haveXmlBranches || branchAXml.empty() || branchBXml.empty() || !nondetSrcState) {
                BDLOG("naming: skip action refinement activity=%s reason=missing_branches_or_source "
                      "haveBranches=%d aEmpty=%d bEmpty=%d hasSrc=%d srcKey=%lu act=%lu",
                      activity.c_str(), haveXmlBranches ? 1 : 0, branchAXml.empty() ? 1 : 0,
                      branchBXml.empty() ? 1 : 0, nondetSrcState ? 1 : 0,
                      (unsigned long)dominantSourceKeyHash, (unsigned long)dominantActionHash);
                return;
            }
            const std::string &xmlA = branchAXml.back();
            const std::string &xmlB = branchBXml.back();
            gui_tree::GUITreePtr snapFallbackXmlA;
            gui_tree::GUITreePtr snapFallbackXmlB;
            const gui_tree::GUITreePtr *snapXmlA = nullptr;
            const gui_tree::GUITreePtr *snapXmlB = nullptr;
            if (!branchATransitions.empty()) {
                apePreferSnapPtrFromSourceTransition(branchATransitions.back(), _apeTreeTransitionLog,
                                                     &snapFallbackXmlA, &snapXmlA);
            }
            if (!branchBTransitions.empty()) {
                apePreferSnapPtrFromSourceTransition(branchBTransitions.back(), _apeTreeTransitionLog,
                                                     &snapFallbackXmlB, &snapXmlB);
            }
            ActivityStateActionPtr edgeAct;
            for (const auto &action : nondetSrcState->getActions()) {
                auto asa = std::dynamic_pointer_cast<ActivityStateAction>(action);
                if (!asa || asa->hash() != dominantActionHash) {
                    continue;
                }
                edgeAct = asa;
                break;
            }
            if (!edgeAct || !edgeAct->requireTarget() || !edgeAct->getTarget()) {
                BDLOG("naming: skip action refinement activity=%s reason=missing_edge_action_or_target "
                      "hasEdge=%d requireTarget=%d hasTarget=%d srcKey=%lu act=%lu",
                      activity.c_str(), edgeAct ? 1 : 0, (edgeAct && edgeAct->requireTarget()) ? 1 : 0,
                      (edgeAct && edgeAct->getTarget()) ? 1 : 0, (unsigned long)dominantSourceKeyHash,
                      (unsigned long)dominantActionHash);
                return;
            }
            const WidgetPtr tw = edgeAct->getTarget();
            const std::shared_ptr<Rect> bnds = tw->getBounds();
            if (!bnds) {
                BDLOG("naming: skip action refinement activity=%s reason=target_bounds_missing srcKey=%lu act=%lu",
                      activity.c_str(), (unsigned long)dominantSourceKeyHash, (unsigned long)dominantActionHash);
                return;
            }
            const Rect tr = *bnds;
            if (tr.isEmpty()) {
                BDLOG("naming refine: zero_target_bounds_on_edge activity=%s srcKey=%lu act=%lu srcState=%zu",
                      activity.c_str(), (unsigned long)dominantSourceKeyHash, (unsigned long)dominantActionHash,
                      nondetSrcState ? nondetSrcState->hash() : 0);
            }
            const NondetActionRefineTransitionContext transCtx =
                buildNondetActionRefineTransitionContext(branchATransitions, branchBTransitions,
                                                         _apeTreeTransitionLog);
            const uintptr_t actionPredicateSourceStateHash = transCtx.actionPredicateSourceStateHash;
            const bool selectedTeHasBounds = transCtx.selectedTargetBoundsFound;
            const Rect selectedTeBounds = transCtx.selectedTargetBounds;
            std::string targetXPathName;
            naming::NamerPtr targetNameNamer;
            const auto missingSnapshotA =
                std::any_of(branchATransitions.begin(), branchATransitions.end(),
                            [this](const NondetTreeTransitionBranchPair::SourceTransition &st) {
                                return !st.sourceGuiSnapshot &&
                                       !apeLookupApeSourceGuiTreeByTransitionSeq(st.transitionSeq,
                                                                                 _apeTreeTransitionLog);
                            });
            const auto missingSnapshotB =
                std::any_of(branchBTransitions.begin(), branchBTransitions.end(),
                            [this](const NondetTreeTransitionBranchPair::SourceTransition &st) {
                                return !st.sourceGuiSnapshot &&
                                       !apeLookupApeSourceGuiTreeByTransitionSeq(st.transitionSeq,
                                                                                 _apeTreeTransitionLog);
                            });
            if (missingSnapshotA || missingSnapshotB) {
                BDLOG("naming: skip action refinement activity=%s reason=cannot_align_missing_snapshot "
                      "srcKey=%lu act=%lu missingA=%d missingB=%d",
                      activity.c_str(), (unsigned long)dominantSourceKeyHash,
                      (unsigned long)dominantActionHash, missingSnapshotA ? 1 : 0,
                      missingSnapshotB ? 1 : 0);
                return;
            }
            static const std::vector<int> kEmptyResolvedIds;
            const std::vector<int> &resolvedIdsA =
                branchATransitions.empty() ? kEmptyResolvedIds : branchATransitions.back().resolvedNodeStableIds;
            const std::vector<int> &resolvedIdsB =
                branchBTransitions.empty() ? kEmptyResolvedIds : branchBTransitions.back().resolvedNodeStableIds;
            if (!apeResolveTargetXPathNameLikeJava(activity, cur, xmlA, resolvedIdsA, &targetXPathName,
                                                   &targetNameNamer, snapXmlA) &&
                !apeResolveTargetXPathNameLikeJava(activity, cur, xmlB, resolvedIdsB, &targetXPathName,
                                                   &targetNameNamer, snapXmlB)) {
                BDLOG("naming: skip action refinement activity=%s reason=target_name_unresolved "
                      "srcKey=%lu act=%lu",
                      activity.c_str(), (unsigned long)dominantSourceKeyHash, (unsigned long)dominantActionHash);
                return;
            }
            const bool sharedA = isSharedAction(activity, cur, tw, branchATransitions);
            const bool sharedB = isSharedAction(activity, cur, tw, branchBTransitions);
            BDLOG("naming: shared_check_exact activity=%s srcKey=%lu act=%lu sharedA=%d sharedB=%d targetName=%s",
                  activity.c_str(), (unsigned long)dominantSourceKeyHash,
                  (unsigned long)dominantActionHash, sharedA ? 1 : 0, sharedB ? 1 : 0,
                  targetXPathName.c_str());
            if (!sharedA && !sharedB) {
                BDLOG("naming: skip action refinement activity=%s reason=target_not_shared srcKey=%lu act=%lu",
                      activity.c_str(), (unsigned long)dominantSourceKeyHash, (unsigned long)dominantActionHash);
                BDLOG("naming: target_not_shared details activity=%s srcKey=%lu act=%lu "
                      "targetBounds=%s teHasBounds=%d teBounds=%s teBoundsEqEdge=%d "
                      "branchA.n=%zu branchB.n=%zu",
                      activity.c_str(), (unsigned long)dominantSourceKeyHash, (unsigned long)dominantActionHash,
                      tr.toString().c_str(), selectedTeHasBounds ? 1 : 0,
                      selectedTeHasBounds ? selectedTeBounds.toString().c_str() : "N/A",
                      (selectedTeHasBounds && selectedTeBounds == tr) ? 1 : 0, branchATransitions.size(),
                      branchBTransitions.size());
                return;
            }
            size_t pIdx = 0;
            std::string wxp;
            if (!apeResolveParentNameletAndWidgetXPath(activity, cur, targetXPathName, targetNameNamer,
                                                       xmlA, xmlB, &pIdx, &wxp, snapXmlA, snapXmlB)) {
                BDLOG("naming: skip action refinement activity=%s reason=resolve_parent_namelet_failed "
                      "srcKey=%lu act=%lu",
                      activity.c_str(), (unsigned long)dominantSourceKeyHash, (unsigned long)dominantActionHash);
                return;
            }
            if (pIdx >= cur->getNamelets().size()) {
                BDLOG("naming: skip action refinement activity=%s reason=parent_index_out_of_range "
                      "pIdx=%zu namelets=%zu srcKey=%lu act=%lu",
                      activity.c_str(), pIdx, cur ? cur->getNamelets().size() : 0,
                      (unsigned long)dominantSourceKeyHash, (unsigned long)dominantActionHash);
                return;
            }
            std::unordered_set<std::string> acceptedFp;
            std::vector<naming::NamerPtr> upperBounds;
            size_t dropReplaceByNull = 0;
            size_t dropReplaceByBlacklist = 0;
            size_t dropReplaceByActionCheck = 0;
            size_t dropReplaceByDupFp = 0;
            size_t dropExtendByNull = 0;
            size_t dropExtendByBlacklist = 0;
            size_t dropExtendByActionCheck = 0;
            size_t dropExtendByDupFp = 0;
            // P0-2: aggregate type-dimension masks of refined namers the lattice expanded for us,
            // so `candCount=0` cases can be triaged by which dimensions ever got tried
            // (Type/Resource-id, Text/Content-desc, Index, ...) vs which were dropped.
            uint32_t extendDimTriedMask = 0;
            uint32_t extendDimDroppedMask = 0;
            size_t extendTried = 0;
            naming::NameletPtr anchorNl = cur->getNamelets()[pIdx];
            naming::NamerPtr curNam = anchorNl ? anchorNl->getNamerPtr() : nullptr;
            if (!curNam) {
                BDLOG("naming: skip action refinement activity=%s reason=anchor_namer_missing "
                      "srcKey=%lu act=%lu pIdx=%zu",
                      activity.c_str(), (unsigned long)dominantSourceKeyHash,
                      (unsigned long)dominantActionHash, pIdx);
                return;
            }
            BDLOG("naming: action_refinement_context activity=%s srcKey=%lu act=%lu "
                  "targetXPath=%s parentIdx=%zu anchorExpr=%s anchorMask=%u branchA.transitions=%s branchB.transitions=%s",
                  activity.c_str(), (unsigned long)dominantSourceKeyHash, (unsigned long)dominantActionHash,
                  targetXPathName.c_str(), pIdx, anchorNl ? anchorNl->getExprString().c_str() : "null",
                  (anchorNl && anchorNl->getNamerPtr()) ? anchorNl->getNamerPtr()->typeDimensionMask() : 0u,
                  apeTransitionListDebugSummary(branchATransitions).c_str(),
                  apeTransitionListDebugSummary(branchBTransitions).c_str());
            // actionRefinement: optional replaceLast(currentNamelet, …) using sortedAbove(parentNamer).
            if (enableReplacingNamelet && cur->hasChild()) {
                const auto &cn = cur->getNamelets();
                if (!cn.empty() && pIdx + 1 == cn.size() && anchorNl && cur->isReplaceable(anchorNl)) {
                    naming::NameletPtr parNl = anchorNl->getParent();
                    if (parNl && parNl->getNamerPtr()) {
                        std::vector<naming::NamerPtr> upperRepl;
                        for (const naming::NamerPtr &refined : lat.sortedAbove(parNl->getNamerPtr())) {
                            if (!refined) {
                                continue;
                            }
                            if (anchorNl->getNamer().refinesTo(*refined)) {
                                continue;
                            }
                            bool skipByUpper = false;
                            for (const naming::NamerPtr &ub : upperRepl) {
                                if (ub && refined->refinesTo(*ub)) {
                                    skipByUpper = true;
                                    break;
                                }
                            }
                            if (skipByUpper) {
                                continue;
                            }
                            naming::NamingPtr child =
                                naming::NamingFactory::replaceLast(cur, anchorNl, refined);
                            if (!child || !child->hasChild()) {
                                ++dropReplaceByNull;
                                continue;
                            }
                            if (blk.count(child->fingerprintString()) != 0) {
                                ++dropReplaceByBlacklist;
                                continue;
                            }
                            size_t p1 = 0;
                            size_t p2 = 0;
                            if (!apeCheckActionRefinementLikeJava(
                                    activity, child, refined, branchATransitions, branchBTransitions, &upperRepl,
                                    &p1, &p2)) {
                                ++dropReplaceByActionCheck;
                                continue;
                            }
                            if (!checkRefinementPredicate(child)) {
                                ++dropReplaceByActionCheck;
                                continue;
                            }
                            const std::string cfp = child->fingerprintString();
                            if (!acceptedFp.insert(cfp).second) {
                                ++dropReplaceByDupFp;
                                continue;
                            }
                            CandidateEval ev;
                            ev.naming = std::move(child);
                            ev.finenessGain = ev.naming->getFineness() - cur->getFineness();
                            ev.strictFiner = ev.finenessGain > 0;
                            ev.fingerprintChanged = ev.naming->fingerprintString() != cur->fingerprintString();
                            ev.partitionTotal = p1 + p2;
                            ev.anchorIdx = pIdx;
                            ev.refinedNamer = refined;
                            ev.originalNameletNamer = anchorNl ? anchorNl->getNamerPtr() : nullptr;
                            ev.useApeJavaStyleComparator = true;
                            ev.actionRefineSourceStateHash = actionPredicateSourceStateHash;
                            ev.actionRefineSourceXml = xmlA;
                            if (!branchATransitions.empty()) {
                                ev.actionRefineResolvedNodeStableIds =
                                    branchATransitions.back().resolvedNodeStableIds;
                            }
                            {
                                const auto &nmls = ev.naming->getNamelets();
                                if (!nmls.empty()) {
                                    ev.updatedNameletExpr = nmls.back()->getExprString();
                                }
                            }
                            accepted.push_back(std::move(ev));
                            break;
                        }
                    }
                }
            }
            for (const naming::NamerPtr &refined : lat.sortedAbove(curNam)) {
                if (!refined) {
                    continue;
                }
                bool skipByUpper = false;
                for (const naming::NamerPtr &ub : upperBounds) {
                    if (ub && refined->refinesTo(*ub)) {
                        skipByUpper = true;
                        break;
                    }
                }
                if (skipByUpper) {
                    continue;
                }
                const uint32_t curRefinedDimMask = refined->typeDimensionMask();
                ++extendTried;
                extendDimTriedMask |= curRefinedDimMask;
                naming::NamingPtr child = naming::NamingFactory::extendUnderNamelet(cur, pIdx, wxp, refined);
                if (!child) {
                    ++dropExtendByNull;
                    extendDimDroppedMask |= curRefinedDimMask;
                    continue;
                }
                if (blk.count(child->fingerprintString()) != 0) {
                    ++dropExtendByBlacklist;
                    extendDimDroppedMask |= curRefinedDimMask;
                    continue;
                }
                size_t p1 = 0;
                size_t p2 = 0;
                if (!apeCheckActionRefinementLikeJava(
                        activity, child, refined, branchATransitions, branchBTransitions,
                        &upperBounds, &p1, &p2)) {
                    ++dropExtendByActionCheck;
                    extendDimDroppedMask |= curRefinedDimMask;
                    continue;
                }
                if (!checkRefinementPredicate(child)) {
                    ++dropExtendByActionCheck;
                    extendDimDroppedMask |= curRefinedDimMask;
                    continue;
                }
                const std::string cfp = child->fingerprintString();
                if (!acceptedFp.insert(cfp).second) {
                    ++dropExtendByDupFp;
                    extendDimDroppedMask |= curRefinedDimMask;
                    continue;
                }
                CandidateEval ev;
                ev.naming = std::move(child);
                ev.finenessGain = ev.naming->getFineness() - cur->getFineness();
                ev.strictFiner = ev.finenessGain > 0;
                ev.fingerprintChanged = ev.naming->fingerprintString() != cur->fingerprintString();
                ev.partitionTotal = p1 + p2;
                ev.anchorIdx = pIdx;
                ev.refinedNamer = refined;
                ev.originalNameletNamer = anchorNl ? anchorNl->getNamerPtr() : nullptr;
                ev.useApeJavaStyleComparator = true;
                ev.actionRefineSourceStateHash = actionPredicateSourceStateHash;
                ev.actionRefineSourceXml = xmlA;
                if (!branchATransitions.empty()) {
                    ev.actionRefineResolvedNodeStableIds = branchATransitions.back().resolvedNodeStableIds;
                }
                {
                    const auto &nmls = ev.naming->getNamelets();
                    if (!nmls.empty()) {
                        ev.updatedNameletExpr = nmls.back()->getExprString();
                    }
                }
                accepted.push_back(std::move(ev));
                break;
            }
            BDLOG("naming: action_refinement_gate_stats activity=%s srcKey=%lu act=%lu acceptedNow=%zu "
                  "replace(null=%zu blk=%zu check=%zu dup=%zu) "
                  "extend(null=%zu blk=%zu check=%zu dup=%zu) "
                  "extend(tried=%zu triedDimMask=0x%x droppedDimMask=0x%x)",
                  activity.c_str(), (unsigned long)dominantSourceKeyHash,
                  (unsigned long)dominantActionHash, acceptedFp.size(), dropReplaceByNull,
                  dropReplaceByBlacklist, dropReplaceByActionCheck, dropReplaceByDupFp, dropExtendByNull,
                  dropExtendByBlacklist, dropExtendByActionCheck, dropExtendByDupFp, extendTried,
                  extendDimTriedMask, extendDimDroppedMask);
            BDLOG("naming: action_refinement_gate_detail activity=%s srcKey=%lu act=%lu "
                  "upperBounds.extend=%zu acceptedFp.size=%zu branchA.transitions=%zu branchB.transitions=%zu",
                  activity.c_str(), (unsigned long)dominantSourceKeyHash,
                  (unsigned long)dominantActionHash, upperBounds.size(), acceptedFp.size(),
                  branchATransitions.size(), branchBTransitions.size());
        };

        auto tryApeStateRefinement = [&]() {
            if (!haveXmlBranches || branchAXml.empty() || branchBXml.empty() || !nondetSrcState) {
                BDLOG("naming: skip state refinement activity=%s reason=missing_branches_or_source "
                      "haveBranches=%d aEmpty=%d bEmpty=%d hasSrc=%d srcKey=%lu act=%lu",
                      activity.c_str(), haveXmlBranches ? 1 : 0, branchAXml.empty() ? 1 : 0,
                      branchBXml.empty() ? 1 : 0, nondetSrcState ? 1 : 0,
                      (unsigned long)dominantSourceKeyHash, (unsigned long)dominantActionHash);
                return;
            }
            gui_tree::GUITreePtr snapFallbackBackA;
            gui_tree::GUITreePtr snapFallbackBackB;
            const gui_tree::GUITreePtr *snapBackA = nullptr;
            const gui_tree::GUITreePtr *snapBackB = nullptr;
            if (!branchATransitions.empty()) {
                apePreferSnapPtrFromSourceTransition(branchATransitions.back(), _apeTreeTransitionLog,
                                                     &snapFallbackBackA, &snapBackA);
            }
            if (!branchBTransitions.empty()) {
                apePreferSnapPtrFromSourceTransition(branchBTransitions.back(), _apeTreeTransitionLog,
                                                     &snapFallbackBackB, &snapBackB);
            }
            uintptr_t topH1 = 0;
            uintptr_t topH2 = 0;
            bool topHashReady = false;
            bool topEquiv = false;
            {
                naming::NamingPtr top = createTopNaming();
                if (top) {
                    topEquiv = isStateEquivalent(activity, branchAXml.back(), branchBXml.back(), top, snapBackA,
                                               snapBackB, &topH1, &topH2, &topHashReady);
                }
            }
            if (topEquiv) {
                // NamingFactory.stateRefinement: log top equivalence, then optional isomorphic (same early return).
                BDLOG("naming: two GUI trees are top naming equivalent (NamingFactory.stateRefinement) "
                      "activity=%s srcKey=%lu act=%lu",
                      activity.c_str(), (unsigned long)dominantSourceKeyHash,
                      (unsigned long)dominantActionHash);
                if (isIsomorphic(activity, branchAXml.back(), branchBXml.back(), snapBackA, snapBackB)) {
                    BDLOG("naming: two GUI trees are top naming equivalent and isomorphic "
                          "(NamingFactory.stateRefinement) activity=%s srcKey=%lu act=%lu",
                          activity.c_str(), (unsigned long)dominantSourceKeyHash,
                          (unsigned long)dominantActionHash);
                }
                BDLOG("naming: skip state refinement activity=%s reason=top_naming_equivalent "
                      "srcKey=%lu act=%lu",
                      activity.c_str(), (unsigned long)dominantSourceKeyHash,
                      (unsigned long)dominantActionHash);
                // Diagnostic: distinguish true top state-key equality from degenerate same-input
                // (identical XML / same snapshot object) when logs still show topEquiv.
                const bool xmlsIdentical = branchAXml.back() == branchBXml.back();
                const void *snapAPtr = snapBackA ? static_cast<const void *>(snapBackA->get()) : nullptr;
                const void *snapBPtr = snapBackB ? static_cast<const void *>(snapBackB->get()) : nullptr;
                BDLOG("naming: top_naming_equivalent details activity=%s srcKey=%lu act=%lu "
                      "topHashReady=%d topH1=%zu topH2=%zu xmlALen=%zu xmlBLen=%zu "
                      "xmlsIdentical=%d snapSameObj=%d snapA=%p snapB=%p",
                      activity.c_str(), (unsigned long)dominantSourceKeyHash,
                      (unsigned long)dominantActionHash, topHashReady ? 1 : 0,
                      topH1, topH2, branchAXml.back().size(), branchBXml.back().size(),
                      xmlsIdentical ? 1 : 0,
                      (snapAPtr && snapAPtr == snapBPtr) ? 1 : 0, snapAPtr, snapBPtr);
                return;
            }
            const std::string &xmlA = branchAXml.back();
            const std::string &xmlB = branchBXml.back();
            std::vector<gui_tree::GUITreePtr> stateRefineSnaps1;
            std::vector<gui_tree::GUITreePtr> stateRefineSnaps2;
            stateRefineSnaps1.reserve(branchATransitions.size());
            for (const auto &st : branchATransitions) {
                stateRefineSnaps1.push_back(st.sourceGuiSnapshot);
            }
            stateRefineSnaps2.reserve(branchBTransitions.size());
            for (const auto &st : branchBTransitions) {
                stateRefineSnaps2.push_back(st.sourceGuiSnapshot);
            }
            const std::vector<gui_tree::GUITreePtr> *stateSnaps1Ptr =
                (stateRefineSnaps1.size() == branchAXml.size()) ? &stateRefineSnaps1 : nullptr;
            const std::vector<gui_tree::GUITreePtr> *stateSnaps2Ptr =
                (stateRefineSnaps2.size() == branchBXml.size()) ? &stateRefineSnaps2 : nullptr;
            const NondetActionRefineTransitionContext stateTransCtx =
                buildNondetActionRefineTransitionContext(branchATransitions, branchBTransitions,
                                                         _apeTreeTransitionLog);
            const uintptr_t stateActionPredicateSourceStateHash = stateTransCtx.actionPredicateSourceStateHash;
            std::unordered_set<std::string> acceptedFp;
            size_t dropReplaceByNull = 0;
            size_t dropReplaceByBlacklist = 0;
            size_t dropReplaceByStateCheck = 0;
            size_t dropReplaceByDupFp = 0;
            size_t dropResolveParent = 0;
            size_t dropParentIdx = 0;
            size_t dropCurNamer = 0;
            size_t dropExtendByNull = 0;
            size_t dropExtendByBlacklist = 0;
            size_t dropExtendByStateCheck = 0;
            size_t dropExtendByDupFp = 0;
            // P0-2: type-dimension trace — OR of refined->typeDimensionMask() for every lattice
            // candidate tried vs every one dropped. Lets a 0-accepted outcome be classified by
            // dimension coverage (Type / Text / Index / …) without re-running refine.
            uint32_t extendDimTriedMask = 0;
            uint32_t extendDimDroppedMask = 0;
            size_t extendTried = 0;
            // stateRefinement: optional replaceLast(last, …) with sortedAbove(parent of last).
            if (enableReplacingNamelet && cur->hasChild()) {
                naming::NameletPtr lastNl = cur->getLastNamelet();
                if (lastNl && cur->isReplaceable(lastNl)) {
                    naming::NameletPtr pl = lastNl->getParent();
                    if (pl && pl->getNamerPtr() && lastNl->getNamerPtr()) {
                        std::vector<naming::NamerPtr> upperRepl;
                        for (const naming::NamerPtr &refined : lat.sortedAbove(pl->getNamerPtr())) {
                            if (!refined) {
                                continue;
                            }
                            if (lastNl->getNamer().refinesTo(*refined)) {
                                continue;
                            }
                            bool skipByUpper = false;
                            for (const naming::NamerPtr &ub : upperRepl) {
                                if (ub && refined->refinesTo(*ub)) {
                                    skipByUpper = true;
                                    break;
                                }
                            }
                            if (skipByUpper) {
                                continue;
                            }
                            naming::NamingPtr child = naming::NamingFactory::replaceLast(cur, lastNl, refined);
                            if (!child || !child->hasChild()) {
                                ++dropReplaceByNull;
                                continue;
                            }
                            if (blk.count(child->fingerprintString()) != 0) {
                                ++dropReplaceByBlacklist;
                                continue;
                            }
                            size_t p1 = 0;
                            size_t p2 = 0;
                            if (!apeCheckStateRefinementLikeJava(activity, child, refined, branchAXml, branchBXml,
                                                                 &upperRepl, &p1, &p2, stateSnaps1Ptr,
                                                                 stateSnaps2Ptr)) {
                                ++dropReplaceByStateCheck;
                                continue;
                            }
                            if (!checkRefinementPredicate(child)) {
                                ++dropReplaceByStateCheck;
                                continue;
                            }
                            const std::string cfp = child->fingerprintString();
                            if (!acceptedFp.insert(cfp).second) {
                                ++dropReplaceByDupFp;
                                continue;
                            }
                            const auto &cn = cur->getNamelets();
                            const size_t anchorIdx =
                                cn.empty() ? 0 : (cn.size() - 1);
                            CandidateEval ev;
                            ev.naming = std::move(child);
                            ev.finenessGain = ev.naming->getFineness() - cur->getFineness();
                            ev.strictFiner = ev.finenessGain > 0;
                            ev.fingerprintChanged = ev.naming->fingerprintString() != cur->fingerprintString();
                            ev.partitionTotal = p1 + p2;
                            ev.anchorIdx = anchorIdx;
                            ev.refinedNamer = refined;
                            ev.originalNameletNamer = lastNl ? lastNl->getNamerPtr() : nullptr;
                            ev.useApeJavaStyleComparator = true;
                            ev.actionRefineSourceStateHash = stateActionPredicateSourceStateHash;
                            ev.actionRefineSourceXml = xmlA;
                            if (!branchATransitions.empty()) {
                                ev.actionRefineResolvedNodeStableIds =
                                    branchATransitions.back().resolvedNodeStableIds;
                            }
                            {
                                const auto &nmls = ev.naming->getNamelets();
                                if (!nmls.empty()) {
                                    ev.updatedNameletExpr = nmls.back()->getExprString();
                                }
                            }
                            ev.registerApeSourceDivergentPredicate = true;
                            accepted.push_back(std::move(ev));
                            break;
                        }
                    }
                }
            }
            std::vector<WidgetPtr> nameCandidates;
            std::unordered_set<const Widget *> seenW;
            for (const auto &action : nondetSrcState->getActions()) {
                auto asa = std::dynamic_pointer_cast<ActivityStateAction>(action);
                if (!asa || !asa->requireTarget() || !asa->getTarget()) {
                    continue;
                }
                const WidgetPtr w = asa->getTarget();
                if (!seenW.insert(w.get()).second) {
                    continue;
                }
                // stateRefinement: HashSet<Name> iteration order; preserve getActions() insertion order +
                // first-seen dedup (LinkedHashSet analogue). Do not sort by widget hash — breaks parity with reference.
                nameCandidates.push_back(w);
            }
            for (const WidgetPtr &tw : nameCandidates) {
                if (!tw) {
                    continue;
                }
                std::string candidateTargetXPathName;
                naming::NamerPtr candidateTargetNamer;
                const std::vector<int> candidateStableIds =
                    apeResolveStableIdsForTargetWidgetLikeJava(nondetSrcState, tw);
                if (!apeResolveTargetXPathNameLikeJava(activity, cur, xmlA, candidateStableIds,
                                                       &candidateTargetXPathName, &candidateTargetNamer, snapBackA) ||
                    !apeResolveTargetXPathNameLikeJava(activity, cur, xmlB, candidateStableIds,
                                                       &candidateTargetXPathName, &candidateTargetNamer, snapBackB)) {
                    ++dropResolveParent;
                    continue;
                }
                size_t pIdx = 0;
                std::string wxp;
                if (!apeResolveParentNameletAndWidgetXPath(activity, cur, candidateTargetXPathName,
                                                           candidateTargetNamer, xmlA, xmlB, &pIdx, &wxp, snapBackA,
                                                           snapBackB)) {
                    ++dropResolveParent;
                    continue;
                }
                if (pIdx >= cur->getNamelets().size()) {
                    ++dropParentIdx;
                    continue;
                }
                std::vector<naming::NamerPtr> upperBounds;
                naming::NamerPtr curNam = cur->getNamelets()[pIdx]->getNamerPtr();
                if (!curNam) {
                    ++dropCurNamer;
                    continue;
                }
                for (const naming::NamerPtr &refined : lat.sortedAbove(curNam)) {
                    if (!refined) {
                        continue;
                    }
                    bool skipByUpper = false;
                    for (const naming::NamerPtr &ub : upperBounds) {
                        if (ub && refined->refinesTo(*ub)) {
                            skipByUpper = true;
                            break;
                        }
                    }
                    if (skipByUpper) {
                        continue;
                    }
                    const uint32_t curRefinedDimMask = refined->typeDimensionMask();
                    ++extendTried;
                    extendDimTriedMask |= curRefinedDimMask;
                    naming::NamingPtr child =
                        naming::NamingFactory::extendUnderNamelet(cur, pIdx, wxp, refined);
                    if (!child) {
                        ++dropExtendByNull;
                        extendDimDroppedMask |= curRefinedDimMask;
                        continue;
                    }
                    if (blk.count(child->fingerprintString()) != 0) {
                        ++dropExtendByBlacklist;
                        extendDimDroppedMask |= curRefinedDimMask;
                        continue;
                    }
                    size_t p1 = 0;
                    size_t p2 = 0;
                    if (!apeCheckStateRefinementLikeJava(activity, child, refined, branchAXml, branchBXml,
                                                         &upperBounds, &p1, &p2, stateSnaps1Ptr,
                                                         stateSnaps2Ptr)) {
                        ++dropExtendByStateCheck;
                        extendDimDroppedMask |= curRefinedDimMask;
                        continue;
                    }
                    if (!checkRefinementPredicate(child)) {
                        ++dropExtendByStateCheck;
                        extendDimDroppedMask |= curRefinedDimMask;
                        continue;
                    }
                    const std::string cfp = child->fingerprintString();
                    if (!acceptedFp.insert(cfp).second) {
                        ++dropExtendByDupFp;
                        extendDimDroppedMask |= curRefinedDimMask;
                        continue;
                    }
                    CandidateEval ev;
                    ev.naming = std::move(child);
                    ev.finenessGain = ev.naming->getFineness() - cur->getFineness();
                    ev.strictFiner = ev.finenessGain > 0;
                    ev.fingerprintChanged = ev.naming->fingerprintString() != cur->fingerprintString();
                    ev.partitionTotal = p1 + p2;
                    ev.anchorIdx = pIdx;
                    ev.refinedNamer = refined;
                    {
                        const auto &cnm = cur->getNamelets();
                        if (pIdx < cnm.size() && cnm[pIdx]) {
                            ev.originalNameletNamer = cnm[pIdx]->getNamerPtr();
                        }
                    }
                    ev.useApeJavaStyleComparator = true;
                    {
                        const auto &nmls = ev.naming->getNamelets();
                        if (!nmls.empty()) {
                            ev.updatedNameletExpr = nmls.back()->getExprString();
                        }
                    }
                    ev.registerApeSourceDivergentPredicate = true;
                    accepted.push_back(std::move(ev));
                    break;
                }
            }
            BDLOG("naming: state_refinement_gate_stats activity=%s srcKey=%lu act=%lu "
                "nameCandidates=%zu acceptedNow=%zu "
                "replace(null=%zu blk=%zu check=%zu dup=%zu) "
                "pre(resolveParent=%zu pIdx=%zu curNamer=%zu) "
                "extend(null=%zu blk=%zu check=%zu dup=%zu) "
                "extend(tried=%zu triedDimMask=0x%x droppedDimMask=0x%x)",
                activity.c_str(), (unsigned long)dominantSourceKeyHash,
                (unsigned long)dominantActionHash, nameCandidates.size(), acceptedFp.size(),
                dropReplaceByNull, dropReplaceByBlacklist, dropReplaceByStateCheck, dropReplaceByDupFp,
                dropResolveParent, dropParentIdx, dropCurNamer,
                dropExtendByNull, dropExtendByBlacklist, dropExtendByStateCheck, dropExtendByDupFp,
                extendTried, extendDimTriedMask, extendDimDroppedMask);
        };

        for (const auto &bp : branchPairs) {
            branchAXml = bp.branchA;
            branchBXml = bp.branchB;
            branchATransitions = bp.branchATransitions;
            branchBTransitions = bp.branchBTransitions;
            brSharedSrcStateHash = bp.sourceStateHash;
            haveXmlBranches = !branchAXml.empty() && !branchBXml.empty();
            nondetSrcState.reset();
            if (brSharedSrcStateHash != 0) {
                auto itShared = stateByHash.find(brSharedSrcStateHash);
                if (itShared != stateByHash.end() && itShared->second) {
                    nondetSrcState = itShared->second;
                }
            }
            if (pair && pair->hasSourceStateKey && nondetSrcState) {
                auto itB = _ape_state_keys_by_hash.find(nondetSrcState->hash());
                bool keyOk = false;
                if (itB != _ape_state_keys_by_hash.end()) {
                    for (const naming::StateKey &k : itB->second) {
                        if (k == pair->sourceStateKey) {
                            keyOk = true;
                            break;
                        }
                    }
                }
                if (!keyOk) {
                    nondetSrcState.reset();
                }
            }
            if (!nondetSrcState) {
                BDLOG("naming: skip refine pair activity=%s reason=pair_source_state_unavailable "
                      "srcState=%lu srcKey=%lu kA=%lu kB=%lu",
                      activity.c_str(), (unsigned long)bp.sourceStateHash,
                      (unsigned long)bp.sourceTransitionSeq, (unsigned long)bp.targetKeyA,
                      (unsigned long)bp.targetKeyB);
                continue;
            }
            guiTreeBlacklistCheckHashes.clear();
            guiTreeBlacklistCheckHashes.push_back(nondetSrcState->hash());
            if (arFirst) {
                tryApeActionRefinement();
                if (accepted.empty()) {
                    tryApeStateRefinement();
                }
            } else {
                tryApeStateRefinement();
                if (accepted.empty()) {
                    tryApeActionRefinement();
                }
            }
            if (!accepted.empty()) {
                if (!branchAXml.empty() && !branchBXml.empty()) {
                    apeRefineNdBranchAXml = branchAXml.back();
                    apeRefineNdBranchBXml = branchBXml.back();
                    apeRefineNdSharedSrcStateHash = brSharedSrcStateHash;
                }
                break;
            }
        }
        filledViaAcceptedCandidates = !accepted.empty();
#endif
        candidates.clear();
        candidates.reserve(accepted.size());
        for (const auto &ev : accepted) {
            candidates.push_back(ev.naming);
        }
        const int maxSteps = (_preference ? _preference->getApeNamingActionRefineHops() : 8);
        {
            const std::string curFpGather = cur ? cur->fingerprintString() : std::string("-");
            BDLOG(
                "naming: refine gather-cands activity=%s pair=%p useBatchNonDet=%d nonDetPairs=%d "
                "states=%zu srcKey=%lu act=%lu domTargets=%zu curFin=%d maxSteps=%d candCount=%zu cur_fp=%s "
                "compatRefine=%d",
                activity.c_str(), static_cast<const void *>(pair), pairScopedCall ? 1 : 0, nonDetPairs,
                activityStateCount, (unsigned long)dominantSourceKeyHash,
                (unsigned long)dominantActionHash, dominantPairTargets, cur ? cur->getFineness() : -1,
                maxSteps, candidates.size(), curFpGather.c_str(),
                filledViaAcceptedCandidates ? 1 : 0);
        }
        if (accepted.empty()) {
            // Distinguish "candidates enumerated but none accepted" from "branch data was never
            // usable so we couldn't enumerate". Only the former is a real no_accepted_candidates
            const bool branchDataUsable =
                !branchPairs.empty() &&
                (!branchATransitions.empty() || !branchBTransitions.empty());
            const ApeRefineFailReason reasonEmpty =
                branchDataUsable ? ApeRefineFailReason::NoAcceptedCandidates
                                 : ApeRefineFailReason::BranchPairsUnavailable;
            const char *reasonOnEmptyStr =
                (reasonEmpty == ApeRefineFailReason::BranchPairsUnavailable)
                    ? "branch_pairs_unavailable"
                    : "no_accepted_candidates";
            BDLOG("naming: skip refine activity=%s reason=%s "
                  "rawCandCount=%zu dominantPairTargets=%zu nonDetPairs=%d branchPairs=%zu "
                  "haveXmlBranches=%d replayActive=%d arFirst=%d branchDataUsable=%d",
                  activity.c_str(), reasonOnEmptyStr,
                  candidates.size(), dominantPairTargets, nonDetPairs,
                  branchPairs.size(), haveXmlBranches ? 1 : 0, replayActive ? 1 : 0, arFirst ? 1 : 0,
                  branchDataUsable ? 1 : 0);
            BDLOG("naming: no_accepted_candidates detail activity=%s srcKey=%lu act=%lu "
                  "activityStates=%zu graphStatesInActivity=%zu blkSize=%zu "
                  "branchATransitions=%zu branchBTransitions=%zu cur_fp=%s",
                  activity.c_str(), (unsigned long)dominantSourceKeyHash,
                  (unsigned long)dominantActionHash, activityStateCount, graphStatesInActivity,
                  blk.size(), branchATransitions.size(), branchBTransitions.size(),
                  cur ? cur->fingerprintString().c_str() : "-");
            if (outFailReason) {
                *outFailReason = reasonEmpty;
            }
            return false;
        }
        if (naming::NamingPtr mgrCurPreSort = _apeStateNamingManager->getNamingForActivity(actKey)) {
            cur = mgrCurPreSort;
        }
        // RefinementResult comparator: RefinementResult.states1/states2 are often unused in the reference impl;
        // so the size branch is a no-op at runtime; primary order matches NamerComparator + expr.
        std::stable_sort(accepted.begin(), accepted.end(), [&](const CandidateEval &a, const CandidateEval &b) {
            if (a.useApeJavaStyleComparator && b.useApeJavaStyleComparator) {
                {
                    // NamingFactory.comparator: originalNamelet namer, then updatedNamelet namer, then expr.
                    if (a.originalNameletNamer && b.originalNameletNamer) {
                        const int c0 = naming::compareNamer(*a.originalNameletNamer, *b.originalNameletNamer);
                        if (c0 != 0) {
                            return c0 < 0;
                        }
                    } else {
                        const auto &cn = cur->getNamelets();
                        if (a.anchorIdx < cn.size() && b.anchorIdx < cn.size() && cn[a.anchorIdx] &&
                            cn[b.anchorIdx]) {
                            const int c0 = naming::compareNamer(cn[a.anchorIdx]->getNamer(),
                                                                cn[b.anchorIdx]->getNamer());

                            if (c0 != 0) {
                                return c0 < 0;
                            }
                        }
                    }
                }
                if (a.refinedNamer && b.refinedNamer) {
                    const int c1 = naming::compareNamer(*a.refinedNamer, *b.refinedNamer);
                    if (c1 != 0) {
                        return c1 < 0;
                    }
                }
                if (a.updatedNameletExpr != b.updatedNameletExpr) {
                    return a.updatedNameletExpr < b.updatedNameletExpr;
                }
                if (a.partitionTotal != b.partitionTotal) {
                    return a.partitionTotal < b.partitionTotal;
                }
                return false;
            }
            if (a.finenessGain != b.finenessGain) {
                return a.finenessGain < b.finenessGain;
            }
            const int lex = compareNamingLexicographicForApeFilter(a.naming, b.naming);
            if (lex != 0) {
                return lex < 0;
            }
            return a.naming->fingerprintString() < b.naming->fingerprintString();
        });
        const naming::NamingPtr curPar = cur ? cur->getParent() : nullptr;
        auto isSupportedRefineRelation = [&](const naming::NamingPtr &cand, bool *outDirect,
                                             bool *outSibling) -> bool {
            const naming::NamingPtr candPar = cand ? cand->getParent() : nullptr;
            const bool direct = (cand && candPar.get() == cur.get());
            const bool sibling = (cand && cur && curPar && candPar && curPar.get() == candPar.get());
            if (outDirect) {
                *outDirect = direct;
            }
            if (outSibling) {
                *outSibling = sibling;
            }
            return direct || sibling;
        };
        // filterRefinementResult: sort then candidates.get(0) — always take sorted first (no skip scan).
        naming::NamingPtr next = accepted.front().naming;
        bool refineSiblingReplace = false;
        const size_t pickedEvalIndex = 0;
        {
            bool isDirect = false;
            bool isSibling = false;
            (void)isSupportedRefineRelation(next, &isDirect, &isSibling);
            refineSiblingReplace = isSibling;
        }
        if (!next) {
            BDLOG("naming: skip refine activity=%s reason=no_next_naming srcKey=%lu act=%lu accepted=%zu",
                  activity.c_str(), (unsigned long)dominantSourceKeyHash,
                  (unsigned long)dominantActionHash, accepted.size());
            if (outFailReason) {
                *outFailReason = ApeRefineFailReason::UnsupportedRefineRelation;
            }
            return false;
        }
        const naming::NamingPtr nextPar = next ? next->getParent() : nullptr;
        BLOG("naming: refine-candidates activity=%s total=%zu accepted=%zu pickedFineGain=%d",
             activity.c_str(), candidates.size(), accepted.size(),
             accepted[pickedEvalIndex].finenessGain);
        {
            static std::atomic<uint64_t> g_refine_pick_seq{0};
            const uint64_t pickN = ++g_refine_pick_seq;
            if (pickN <= 16 || (pickN % 512) == 0) {
                const naming::NamingPtr nextPar = next ? next->getParent() : nullptr;
                int top3Direct = 0;
                const size_t lim = std::min<size_t>(accepted.size(), 3);
                for (size_t i = 0; i < lim; ++i) {
                    const naming::NamingPtr c = accepted[i].naming;
                    const naming::NamingPtr p = c ? c->getParent() : nullptr;
                    if (p.get() == cur.get()) {
                        top3Direct++;
                    }
                }
                BDLOG(
                    "naming: chain picked Refine act=%s cur=%p next=%p next_par=%p par_eq_cur=%d "
                    "raw_cands=%zu accepted=%zu top3_direct_child_of_cur=%d curFin=%d nextFin=%d",
                    actKey.c_str(), static_cast<const void *>(cur.get()),
                    static_cast<const void *>(next.get()), static_cast<const void *>(nextPar.get()),
                    (nextPar.get() == cur.get()) ? 1 : 0, candidates.size(), accepted.size(), top3Direct,
                    cur ? cur->getFineness() : -1, next ? next->getFineness() : -1);
            }
        }
        ApeNamingAbstractionContext &ctx = _apeNamingContext[actKey];
        ctx.previousNamingBeforeRefine = cur;
        ctx.previousNamingFingerprintBeforeRefine = cur->fingerprintString();
        ctx.oldKeyHashToNewKeyHashes.clear();
        ctx.oldKeyHashToObservationCount.clear();
        ctx.stateCountAtLastNamingRefinement = activityStateCount;
        ctx.nonDetPairsAtLastNamingRefinement = nonDetPairs;
        ctx.triggerSourceKeyHash = xmlSpaceTriggerSourceKeyHash;
        ctx.triggerSourceKeyExact = false;
        ctx.triggerSourceKey = naming::StateKey::fromParts("", nullptr, {});
        {
            static std::atomic<uint64_t> g_refine_trigger_source_init_probe{0};
            const uint64_t rp = ++g_refine_trigger_source_init_probe;
            if (rp <= 160 || (rp % 500) == 0) {
                BDLOG("naming BUG_PROBE [refine_trigger_source_init] seq=%llu activity=%s "
                      "dominantSourceHash=%lu xmlSpaceSourceHash=%lu pair=%p pairHasSourceKey=%d "
                      "nondetSrcState=%p",
                      static_cast<unsigned long long>(rp), actKey.c_str(),
                      static_cast<unsigned long>(dominantSourceKeyHash),
                      static_cast<unsigned long>(xmlSpaceTriggerSourceKeyHash),
                      static_cast<const void *>(pair),
                      (pair && pair->hasSourceStateKey) ? 1 : 0,
                      static_cast<const void *>(nondetSrcState.get()));
            }
        }
        if (pair && pair->hasSourceStateKey) {
            ctx.triggerSourceKeyExact = true;
            ctx.triggerSourceKey = pair->sourceStateKey;
        } else if (nondetSrcState) {
            naming::StateKey recoveredSourceKey = naming::StateKey::fromParts("", nullptr, {});
            if (tryGetApeStateKey(nondetSrcState->hash(), &recoveredSourceKey, actKey,
                                  dominantSourceKeyHash) &&
                recoveredSourceKey.activity() == actKey &&
                recoveredSourceKey.hash() == dominantSourceKeyHash) {
                ctx.triggerSourceKeyExact = true;
                ctx.triggerSourceKey = recoveredSourceKey;
            }
        }
        if (!ctx.triggerSourceKeyExact) {
            for (auto it = _apeTransitionLog.rbegin(); it != _apeTransitionLog.rend(); ++it) {
                if (!it->valid || !it->hasSourceStateKey) {
                    continue;
                }
                if (it->sourceActivity != actKey || it->sourceKeyHash != dominantSourceKeyHash) {
                    continue;
                }
                ctx.triggerSourceKeyExact = true;
                ctx.triggerSourceKey = it->sourceStateKey;
                break;
            }
        }
        {
            static std::atomic<uint64_t> g_refine_trigger_source_resolve_probe{0};
            const uint64_t rp = ++g_refine_trigger_source_resolve_probe;
            if (rp <= 200 || (rp % 500) == 0) {
                const uintptr_t resolvedStateKeyHash =
                    ctx.triggerSourceKeyExact ? ctx.triggerSourceKey.hash() : 0;
                BDLOG("naming BUG_PROBE [refine_trigger_source_resolve] seq=%llu activity=%s "
                      "trigHashCtx=%lu trigExact=%d trigHashStateKey=%lu "
                      "equalCtxVsStateKey=%d old2newSize=%zu oldObsSize=%zu",
                      static_cast<unsigned long long>(rp), actKey.c_str(),
                      static_cast<unsigned long>(ctx.triggerSourceKeyHash),
                      ctx.triggerSourceKeyExact ? 1 : 0,
                      static_cast<unsigned long>(resolvedStateKeyHash),
                      (ctx.triggerSourceKeyExact &&
                       ctx.triggerSourceKeyHash == resolvedStateKeyHash) ? 1 : 0,
                      ctx.oldKeyHashToNewKeyHashes.size(),
                      ctx.oldKeyHashToObservationCount.size());
            }
        }
        if (!ctx.triggerSourceKeyExact) {
            BDLOG("naming: skip refine activity=%s reason=trigger source statekey unrecoverable srcKey=%lu act=%lu",
                  activity.c_str(), (unsigned long)dominantSourceKeyHash,
                  (unsigned long)dominantActionHash);
            if (outFailReason) {
                *outFailReason = ApeRefineFailReason::Other;
            }
            return false;
        }
        {
            static std::atomic<uint64_t> g_refine_seed_seq{0};
            const uint64_t s = ++g_refine_seed_seq;
            ctx.lastRefineSeedSeq = s;
            ctx.lastRefineSeedTriggerHash = ctx.triggerSourceKeyHash;
        }
        ctx.triggerActionHash = dominantActionHash;
        ctx.triggerTargetCountAtRefine = dominantPairTargets;
        ctx.triggerTargetKeyHashes = std::move(xmlSpaceTriggerTargetKeyHashes);
        {
            static std::atomic<uint64_t> g_refine_trigger_keyspace_diag{0};
            const uint64_t n = ++g_refine_trigger_keyspace_diag;
            if (n <= 120 || (n % 400) == 0) {
                const uintptr_t triggerKeyHashFromStateKey =
                    ctx.triggerSourceKeyExact ? ctx.triggerSourceKey.hash() : 0;
                BDLOG("naming: refine trigger keyspace seq=%" PRIu64
                      " activity=%s trigHash(xmlSpace)=%lu trigHash(stateKey)=%lu equal=%d "
                      "trigExact=%d srcHash(raw)=%lu srcHash(xmlspace)=%lu actHash=%lu",
                      n, actKey.c_str(),
                      static_cast<unsigned long>(ctx.triggerSourceKeyHash),
                      static_cast<unsigned long>(triggerKeyHashFromStateKey),
                      (ctx.triggerSourceKeyExact &&
                       ctx.triggerSourceKeyHash == triggerKeyHashFromStateKey) ? 1 : 0,
                      ctx.triggerSourceKeyExact ? 1 : 0,
                      static_cast<unsigned long>(dominantSourceKeyHash),
                      static_cast<unsigned long>(xmlSpaceTriggerSourceKeyHash),
                      static_cast<unsigned long>(dominantActionHash));
            }
        }
        if (pickedEvalIndex < accepted.size()) {
            const CandidateEval &picked = accepted[pickedEvalIndex];
            if (!picked.actionRefineSourceXml.empty() &&
                !picked.actionRefineResolvedNodeStableIds.empty()) {
                addApeActionRefinementPredicate(activity, picked.actionRefineSourceStateHash,
                                                picked.actionRefineSourceXml,
                                                picked.actionRefineResolvedNodeStableIds,
                                                picked.naming);
            }
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
            if (picked.registerApeSourceDivergentPredicate &&
                !apeRefineNdBranchAXml.empty() && !apeRefineNdBranchBXml.empty()) {
                addApeSourceDivergentPredicate(activity, apeRefineNdBranchAXml, apeRefineNdBranchBXml,
                                               apeRefineNdSharedSrcStateHash);
            }
#endif
        }
        {
            if (next && nextPar.get() != cur.get()) {
                auto apeSnipFp = [](const naming::NamingPtr &p) -> std::string {
                    if (!p) {
                        return std::string("-");
                    }
                    const std::string &s = p->fingerprintString();
                    return s.size() > 140 ? s.substr(0, 140) + std::string("...") : s;
                };
                const std::string sCur = apeSnipFp(cur);
                const std::string sNext = apeSnipFp(next);
                const std::string sPar = apeSnipFp(nextPar);
                int curStrictAncNext = 0;
                if (cur && next) {
                    for (naming::NamingPtr x = next->getParent(); x; x = x->getParent()) {
                        if (x == cur) {
                            curStrictAncNext = 1;
                            break;
                        }
                    }
                }
                BDLOG(
                    "naming: refine pre_update NOT direct child: act=%s cur=%p next=%p next_parent=%p "
                    "srcKeyH=%lu actH=%lu trigExact=%d curFin=%d nextFin=%d cur_strict_anc_next=%d "
                    "cur_fp=%s next_fp=%s par_fp=%s",
                    actKey.c_str(), static_cast<const void *>(cur.get()),
                    static_cast<const void *>(next.get()), static_cast<const void *>(nextPar.get()),
                    static_cast<unsigned long>(dominantSourceKeyHash),
                    static_cast<unsigned long>(dominantActionHash), ctx.triggerSourceKeyExact ? 1 : 0,
                    cur ? cur->getFineness() : -1, next ? next->getFineness() : -1, curStrictAncNext,
                    sCur.c_str(), sNext.c_str(), sPar.c_str());
            }
        }
        bool skipNamingEdgeUpdate = false;
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
        // StateNamingManager.updateNaming: when getNaming(tree,dom) already equals newOne, skip edge write.
        if (nondetSrcState && next && !branchAXml.empty()) {
            std::string pkgDom;
            std::string clsDom;
            naming::StateKey::splitActivityPackageClass(activity, &pkgDom, &clsDom);
            gui_tree::GUITreePtr noopSnap = apeLatestGuiTreeSnapshot(nondetSrcState->hash());
            gui_tree::GUITreeBuildResult noopBuilt =
                buildGuitreePreferApeSnapshotAndDomXml(branchAXml.back(), pkgDom, clsDom, noopSnap);
            if (noopBuilt.tree && noopBuilt.dom &&
                safeRebuildTree(cur, *noopBuilt.tree, noopBuilt.dom, "ape_noop_gate")) {
                naming::NamingPtr noopResolved =
                    _apeStateNamingManager->treeToNaming(*noopBuilt.tree, noopBuilt.dom);
                if (noopResolved && next &&
                    noopResolved->fingerprintString() == next->fingerprintString()) {
                    BDLOG("naming: refine noop gate (treeToNaming already target); skip "
                          "updateNamingWithStateKey only");
                    skipNamingEdgeUpdate = true;
                }
            }
        }
#endif
        const naming::NamingUpdateKind refineUpdateKind =
            refineSiblingReplace ? naming::NamingUpdateKind::Abstract : naming::NamingUpdateKind::Refine;
        {
            bool candDirect = false;
            bool candSibling = false;
            (void)isSupportedRefineRelation(next, &candDirect, &candSibling);
            static std::atomic<uint64_t> g_refine_updatekind_diag{0};
            const uint64_t ud = ++g_refine_updatekind_diag;
            const naming::NamingPtr candPar = next ? next->getParent() : nullptr;
            if (ud <= 30 || (ud % 300) == 0 ||
                (refineUpdateKind == naming::NamingUpdateKind::Refine && !candDirect) ||
                (refineUpdateKind == naming::NamingUpdateKind::Refine && candSibling)) {
                BLOG(
                    "naming diag [refineUpdateKind=%s] seq=%llu act=%s "
                    "cur=%p next=%p nextPar=%p refineSiblingReplace=%d "
                    "candDirect=%d candSibling=%d",
                    refineUpdateKind == naming::NamingUpdateKind::Refine ? "Refine" : "Abstract",
                    static_cast<unsigned long long>(ud), actKey.c_str(),
                    static_cast<const void *>(cur.get()),
                    static_cast<const void *>(next.get()),
                    static_cast<const void *>(candPar.get()),
                    refineSiblingReplace ? 1 : 0, candDirect ? 1 : 0, candSibling ? 1 : 0);
            }
        }
        if (!skipNamingEdgeUpdate) {
            _apeStateNamingManager->updateNamingWithStateKey(
                actKey, refineUpdateKind, cur, next, ctx.triggerSourceKey);
        } else if (next) {
            // Full updateNaming still ends in setNaming(activity, newOne); noop skipped edges only — sync pointer.
            naming::NamingPtr storedAct = _apeStateNamingManager->getNamingForActivity(actKey);
            if (!storedAct || storedAct.get() != next.get()) {
                _apeStateNamingManager->activityManager().setNaming(actKey, next);
            }
        }
        {
            naming::NamingPtr postApply = _apeStateNamingManager->getNamingForActivity(actKey);
            if (!postApply || !next ||
                postApply->fingerprintString() != next->fingerprintString()) {
                BDLOG("naming: refine failed activity naming did not reach picked target "
                      "(StateNamingManager rejected update or noop mismatch)");
                if (outFailReason) {
                    *outFailReason = ApeRefineFailReason::UnsupportedRefineRelation;
                }
                return false;
            }
        }
        invalidateApeGraphStateKeyDedupMap();
        bool rebuiltViaHistory = apeLocalRebuildFromHistory(actKey);
        std::vector<uintptr_t> repKeyHashes;
        std::unordered_set<uintptr_t> focusOldKeyHashes;
        std::unordered_set<uintptr_t> affectedTrees;
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
        if (ctx.triggerSourceKeyHash != 0) {
            uintptr_t triggerParentKeyHash = 0;
            if (refineSiblingReplace && curPar && nondetSrcState) {
                auto itSrcXml = _apeStateXmlByStateHash.find(nondetSrcState->hash());
                if (itSrcXml != _apeStateXmlByStateHash.end() && !itSrcXml->second.empty()) {
                    gui_tree::GUITreePtr trigSnap = this->apeLatestGuiTreeSnapshot(nondetSrcState->hash());
                    const gui_tree::GUITreePtr *trigSnapPtr = trigSnap ? &trigSnap : nullptr;
                    (void)apeStateHashFromXmlWithNaming(activity, itSrcXml->second, curPar, &triggerParentKeyHash,
                                                        0, nullptr, trigSnapPtr);
                }
            }
            auto isInAffectedSet = [&](const std::string &xml, uintptr_t oldKeyHash,
                                       uintptr_t stateHashForXml) -> bool {
                if (refineSiblingReplace && curPar && triggerParentKeyHash != 0) {
                    uintptr_t oldParentH = 0;
                    gui_tree::GUITreePtr affSnap = this->apeLatestGuiTreeSnapshot(stateHashForXml);
                    const gui_tree::GUITreePtr *affSnapPtr = affSnap ? &affSnap : nullptr;
                    return apeStateHashFromXmlWithNaming(activity, xml, curPar, &oldParentH, 0, nullptr,
                                                         affSnapPtr) &&
                           oldParentH == triggerParentKeyHash;
                }
                return oldKeyHash == ctx.triggerSourceKeyHash;
            };
            for (const auto &kv : _apeStateXmlByStateHash) {
                const uintptr_t sh = kv.first;
                const std::string &xml = kv.second;
                if (xml.empty()) {
                    continue;
                }
                auto itBucket = _ape_state_keys_by_hash.find(sh);
                if (itBucket == _ape_state_keys_by_hash.end() || itBucket->second.empty()) {
                    continue;
                }
                bool activityMatch = false;
                for (const auto &k : itBucket->second) {
                    if (k.activity() == actKey) {
                        activityMatch = true;
                        break;
                    }
                }
                if (!activityMatch) {
                    continue;
                }
                uintptr_t oldH = 0;
                uintptr_t newH = 0;
                gui_tree::GUITreePtr pairSnap = this->apeLatestGuiTreeSnapshot(sh);
                const gui_tree::GUITreePtr *pairSnapPtr = pairSnap ? &pairSnap : nullptr;
                if (!apeStateHashFromXmlWithTwoNamings(activity, xml, cur, &oldH, next, &newH, pairSnapPtr)) {
                    continue;
                }    
                const bool inAffected = isInAffectedSet(xml, oldH, sh);
                if (inAffected && oldH != newH) {
                    ctx.oldKeyHashToNewKeyHashes[oldH].insert(newH);
                    ctx.oldKeyHashToObservationCount[oldH]++;
                    focusOldKeyHashes.insert(oldH);
                    affectedTrees.insert(sh);
                }
            }
        }
#endif
        if (!rebuiltViaHistory) {
            repKeyHashes.reserve(8);
            if (!focusOldKeyHashes.empty()) {
                for (uintptr_t h : focusOldKeyHashes) {
                    repKeyHashes.push_back(h);
                    if (repKeyHashes.size() >= 8) {
                        break;
                    }
                }
            } else if (ctx.triggerSourceKeyHash != 0) {
                repKeyHashes.push_back(ctx.triggerSourceKeyHash);
            }
            if (!repKeyHashes.empty()) {
                rebuildApeStateRepresentativesForKeyHashes(activity, cur, repKeyHashes, 1);
            }
            if (!focusOldKeyHashes.empty()) {
                remapApeTransitionAggregationForActivity(activity, cur, next, &focusOldKeyHashes);
            }
            pruneStaleApeStatesForActivity(actKey, ctx.previousNamingFingerprintBeforeRefine, nullptr);
        }
        BLOG("naming: refine activity=%s", activity.c_str());
        {
            const std::string nextFpOk = next ? next->fingerprintString() : std::string("-");
            BDLOG("naming: refine success activity=%s nextFin=%d rebuilt=%d focusOldKeys=%zu "
                  "affectedTrees=%zu repKeys=%zu next_fp=%s",
                  activity.c_str(), next ? next->getFineness() : -1, rebuiltViaHistory ? 1 : 0,
                  focusOldKeyHashes.size(), affectedTrees.size(), repKeyHashes.size(), nextFpOk.c_str());
        }
        {
            const uintptr_t trigSk =
                ctx.triggerSourceKeyExact ? ctx.triggerSourceKey.hash() : static_cast<uintptr_t>(0);
            BDLOG(
                "naming: refineActivityApeNaming success_return activity=%s pairScoped=%d "
                "xmlSpaceTriggerSourceKeyHash=%lu ctx_triggerSourceKeyHash=%lu trigger_stateKeyHash=%lu "
                "lastRefineSeedSeq=%llu trigExact=%d dominantSrcKey=%lu dominantAct=%lu "
                "xmlSpaceTrigger_zero=%d ctxTrig_vs_sk_equal=%d",
                actKey.c_str(), pairScopedCall ? 1 : 0,
                static_cast<unsigned long>(xmlSpaceTriggerSourceKeyHash),
                static_cast<unsigned long>(ctx.triggerSourceKeyHash),
                static_cast<unsigned long>(trigSk),
                static_cast<unsigned long long>(ctx.lastRefineSeedSeq), ctx.triggerSourceKeyExact ? 1 : 0,
                static_cast<unsigned long>(dominantSourceKeyHash),
                static_cast<unsigned long>(dominantActionHash),
                xmlSpaceTriggerSourceKeyHash == 0 ? 1 : 0,
                (ctx.triggerSourceKeyHash == trigSk) ? 1 : 0);
        }
        if (outFailReason) {
            *outFailReason = ApeRefineFailReason::None;
        }
        return true;
    }

    /** @brief Clears cached state-key deduplication maps after abstraction changes. */
    void Model::invalidateApeGraphStateKeyDedupMap() {
        _ape_graph_state_by_key.clear();
    }

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
    /** @brief Rebuilds canonical state representatives for each abstract key hash. */
    void Model::rebuildApeStateRepresentativesForKeyHashes(
        const std::string &rawActivity,
        const naming::NamingPtr &oldNaming,
        const std::vector<uintptr_t> &oldKeyHashes,
        size_t maxStatesPerKeyHash) {
#if !defined(FASTBOT_HAS_PUGIXML) || !FASTBOT_HAS_PUGIXML
        (void)rawActivity;
        (void)oldNaming;
        (void)oldKeyHashes;
        (void)maxStatesPerKeyHash;
#else
        if (!_graph || !_preference || oldKeyHashes.empty() || !oldNaming || maxStatesPerKeyHash == 0) {
            return;
        }
        const std::string actKey = naming::StateKey::canonicalActivityString(rawActivity);
        const bool wantApeRlIdentity = !_preference->useStaticReuseAbstraction();
        if (!wantApeRlIdentity) {
            return;
        }
        AbstractAgentPtr agent = getOrCreateAgent(ModelConstants::DefaultDeviceID);
        if (!agent) {
            return;
        }
        stringPtr activityPtr = getOrCreateActivityPtr(rawActivity);

        auto rebuildOneXml = [&](const std::string &xml) {
            if (xml.empty()) {
                return;
            }
            ElementPtr elem = Element::createFromXml(xml);
            if (!elem) {
                return;
            }
            StatePtr built = buildStateOnly(elem, agent, activityPtr);
            if (!built) {
                return;
            }
            naming::StateKey apeKey = naming::StateKey::fromParts(
                naming::StateKey::canonicalActivityString(rawActivity), nullptr, {});
            ApeStateKeyBuildFailReason failReason = ApeStateKeyBuildFailReason::None;
            const bool haveApeKey = buildApeStateKeyFromElementTree(
                elem, rawActivity, &apeKey, &failReason, wantApeRlIdentity ? built : StatePtr());
            if (haveApeKey) {
                built->applyDynamicAbstractionIdentityHash(apeKey.hash());
            }
            StatePtr canonical = built;
            if (haveApeKey && _preference->useApeGraphDedupByStateKey()) {
                const uintptr_t kh = apeKey.hash();
                bool deduped = false;
                auto &bucket = _ape_graph_state_by_key[kh];
                for (const auto &entry : bucket) {
                    if (entry.key == apeKey) {
                        _ape_correctness_counters.graph_dedup_exact_hit++;
                        agent->setCurrentStateRecovered(true);
                        _graph->recordStateVisit(entry.state, built);
                        canonical = entry.state;
                        deduped = true;
                        break;
                    }
                }
                if (!deduped) {
                    if (!bucket.empty()) {
                        _ape_correctness_counters.graph_dedup_hash_collision++;
                    }
                    canonical = _graph->addState(built);
                    bucket.push_back(ApeGraphStateKeyDedupEntry{apeKey, canonical});
                } else {
                    _ape_correctness_counters.graph_dedup_hash_hit++;
                }
            } else {
                canonical = _graph->addState(built);
            }
            if (haveApeKey) {
                recordApeStateKey(canonical, apeKey);
            }
            _apeStateXmlByStateHash[canonical->hash()] = xml;
            if (elem) {
                _apeStateElementByStateHash[canonical->hash()] = elem;
            }
        };

        auto keyHashSet = std::unordered_set<uintptr_t>(oldKeyHashes.begin(), oldKeyHashes.end());
        std::unordered_map<uintptr_t, size_t> rebuiltPerKey;
        std::unordered_set<uintptr_t> satisfiedKeys;
        std::vector<uintptr_t> stateHashesToRebuild;
        stateHashesToRebuild.reserve(16);
        for (const auto &kv : _apeStateXmlByStateHash) {
            if (satisfiedKeys.size() >= keyHashSet.size()) {
                break;
            }
            const uintptr_t stateHash = kv.first;
            const std::string &xml = kv.second;
            if (xml.empty()) {
                continue;
            }
            naming::StateKey storedKey = naming::StateKey::fromParts("", nullptr, {});
            if (!tryGetApeStateKey(stateHash, &storedKey, actKey)) {
                continue;
            }
            if (storedKey.activity() != actKey) {
                continue;
            }
            uintptr_t oldH = 0;
#if defined(DYNAMIC_STATE_ABSTRACTION_ENABLED) && DYNAMIC_STATE_ABSTRACTION_ENABLED
            gui_tree::GUITreePtr rbSnap = apeLatestGuiTreeSnapshot(stateHash);
            const gui_tree::GUITreePtr *rbSnapPtr = rbSnap ? &rbSnap : nullptr;
            if (!apeStateHashFromXmlWithNaming(rawActivity, xml, oldNaming, &oldH, 0, nullptr, rbSnapPtr)) {
#else
            if (!apeStateHashFromXmlWithNaming(rawActivity, xml, oldNaming, &oldH)) {
#endif
                continue;
            }
            if (keyHashSet.count(oldH) == 0) {
                continue;
            }
            size_t &n = rebuiltPerKey[oldH];
            if (n >= maxStatesPerKeyHash) {
                satisfiedKeys.insert(oldH);
                continue;
            }
            stateHashesToRebuild.push_back(stateHash);
            ++n;
            if (n >= maxStatesPerKeyHash) {
                satisfiedKeys.insert(oldH);
            }
        }
        for (uintptr_t sh : stateHashesToRebuild) {
            auto it = _apeStateXmlByStateHash.find(sh);
            if (it == _apeStateXmlByStateHash.end() || it->second.empty()) {
                continue;
            }
            rebuildOneXml(it->second);
        }
#endif
    }

    /** @brief Remaps transition aggregation keys after pruning or renaming. */
    void Model::remapApeTransitionAggregationForActivity(
        const std::string &rawActivity,
        const naming::NamingPtr &fromNaming,
        const naming::NamingPtr &toNaming,
        const std::unordered_set<uintptr_t> *focusOldKeyHashes) {
#if !defined(FASTBOT_HAS_PUGIXML) || !FASTBOT_HAS_PUGIXML
        (void)rawActivity;
        (void)fromNaming;
        (void)toNaming;
#else
        if (rawActivity.empty() || !fromNaming || !toNaming) {
            return;
        }
        const std::string actKey = naming::StateKey::canonicalActivityString(rawActivity);
        const std::string &toFp = toNaming->fingerprintString();
        const bool hasFocus = (focusOldKeyHashes && !focusOldKeyHashes->empty());

        // Evidence pools are keyed by current (sourceKeyHash, actionSignature).
        // Since remap mutates hashes in-place, drop old-space pools for this activity and rebuild
        // from the remapped transition log.
        if (!_apeEvidencePools.empty()) {
            for (const auto &slot : _apeTransitionLog) {
                if (slot.valid && slot.sourceActivity == actKey) {
                    _apeEvidencePools.erase(ApePairKey{slot.sourceKeyHash, slot.actionHash});
                }
            }
        }

        uint32_t fullMask = 0;
        for (auto t : naming::namerTypesUsed()) {
            fullMask |= (1u << static_cast<unsigned>(t));
        }
        naming::NamerPtr fullNamer = naming::NamerFactory::current().getByMask(fullMask);

        // Build keyHash -> representative stateHash map once (avoid O(|states|*|transitions|) scan).
        std::unordered_map<uintptr_t, uintptr_t> representativeStateHashByKeyHash;
        representativeStateHashByKeyHash.reserve(256);
        for (const auto &kv : _ape_state_keys_by_hash) {
            const uintptr_t sh = kv.first;
            for (const auto &k : kv.second) {
                if (k.activity() == actKey && k.namingFingerprint() == toFp) {
                    representativeStateHashByKeyHash.emplace(k.hash(), sh);
                }
            }
        }
        auto findRepresentativeStateHash = [&](uintptr_t keyHash) -> uintptr_t {
            auto it = representativeStateHashByKeyHash.find(keyHash);
            return it == representativeStateHashByKeyHash.end() ? 0 : it->second;
        };

        std::string pkg;
        std::string cls;
        naming::StateKey::splitActivityPackageClass(rawActivity, &pkg, &cls);

        struct RemapTreeCacheEntry {
            gui_tree::GUITreeBuildResult built;
            std::vector<gui_tree::GUITreeNode *> preorder;
            std::vector<uintptr_t> fullPathHashes;
            std::vector<uintptr_t> xpathHashes;
            std::vector<Rect> bounds;
            bool ready{false};
            bool hasStateKey{false};
            naming::StateKey stateKey = naming::StateKey::fromParts("", nullptr, {});
        };

        std::unordered_map<uintptr_t, RemapTreeCacheEntry> treeCache;
        treeCache.reserve(64);
        std::unordered_map<uintptr_t, uintptr_t> keyHashCache;
        keyHashCache.reserve(128);

        auto getTreeEntry = [&](uintptr_t stateHash, const std::string &xml,
                                RemapTreeCacheEntry **out) -> bool {
            if (!out || stateHash == 0 || xml.empty()) {
                return false;
            }
            auto it = treeCache.find(stateHash);
            if (it != treeCache.end()) {
                *out = &it->second;
                return it->second.ready;
            }
            RemapTreeCacheEntry entry;
#if defined(DYNAMIC_STATE_ABSTRACTION_ENABLED) && DYNAMIC_STATE_ABSTRACTION_ENABLED
            entry.built =
                buildGuitreePreferApeSnapshotAndDomXml(xml, pkg, cls, apeLatestGuiTreeSnapshot(stateHash));
#else
            entry.built = buildGuitreeFromCachedXmlPreferElement(xml, pkg, cls);
#endif
            if (!entry.built.tree || !entry.built.dom) {
                treeCache.emplace(stateHash, std::move(entry));
                *out = &treeCache.find(stateHash)->second;
                return false;
            }
            if (!safeRebuildTree(toNaming, *entry.built.tree, entry.built.dom)) {
                treeCache.emplace(stateHash, std::move(entry));
                *out = &treeCache.find(stateHash)->second;
                return false;
            }
            collectGUITreeNodesPreOrder(entry.built.tree->getRootNode(), &entry.preorder);
            const size_t n = entry.preorder.size();
            entry.fullPathHashes.assign(n, 0);
            entry.xpathHashes.assign(n, 0);
            entry.bounds.resize(n);
            for (size_t i = 0; i < n; ++i) {
                auto *node = entry.preorder[i];
                if (!node) {
                    continue;
                }
                entry.bounds[i] = node->getBounds();
                if (auto nxp = node->getXPathName(); nxp) {
                    const std::string xp = nxp->toXPath();
                    if (!xp.empty()) {
                        entry.xpathHashes[i] = fastStringHash(xp);
                    }
                }
                if (fullNamer) {
                    std::string fullKey = fullNamer->xpathKeyForNode(*node);
                    if (fullKey.empty()) {
                        if (naming::NamePtr fullName = fullNamer->naming(*node); fullName) {
                            fullKey = fullName->toXPath();
                        }
                    }
                    if (!fullKey.empty()) {
                        entry.fullPathHashes[i] = fastStringHash(fullKey);
                    }
                }
            }
            entry.ready = true;
            treeCache.emplace(stateHash, std::move(entry));
            *out = &treeCache.find(stateHash)->second;
            return true;
        };

        auto getStateKeyHashUnderToNaming = [&](uintptr_t stateHash, const std::string &xml,
                                                uintptr_t *out) -> bool {
            if (!out || stateHash == 0) {
                return false;
            }
            auto it = keyHashCache.find(stateHash);
            if (it != keyHashCache.end()) {
                *out = it->second;
                return true;
            }
            RemapTreeCacheEntry *entry = nullptr;
            if (!getTreeEntry(stateHash, xml, &entry) || !entry || !entry->built.tree) {
                return false;
            }
            const uintptr_t h = naming::StateKey::hashFromGUITree(*entry->built.tree);
            keyHashCache.emplace(stateHash, h);
            *out = h;
            return true;
        };

        auto computeActionHash = [&](uintptr_t sourceStateHash, const std::string &xml,
                                     const ApeTransitionEntry &e, uintptr_t *out) -> bool {
            if (!out) {
                return false;
            }
            const uintptr_t activityH = fastStringHash(actKey);
            uintptr_t abstractTargetHash = 0x1;
            if (e.hasTargetFullPath || e.hasTargetBounds) {
                RemapTreeCacheEntry *entry = nullptr;
                if (getTreeEntry(sourceStateHash, xml, &entry) && entry && entry->ready) {
                    size_t matchedIdx = static_cast<size_t>(-1);
                    if (e.hasTargetFullPath && fullNamer) {
                        size_t bestIdx = static_cast<size_t>(-1);
                        long bestDist = 0;
                        bool haveBest = false;
                        for (size_t i = 0; i < entry->preorder.size(); ++i) {
                            if (entry->fullPathHashes[i] == 0 || entry->fullPathHashes[i] != e.targetFullPathHash) {
                                continue;
                            }
                            if (!e.hasTargetBounds) {
                                matchedIdx = i;
                                break;
                            }
                            const Rect &b = entry->bounds[i];
                            const long cx = static_cast<long>(b.left + (b.right - b.left) / 2);
                            const long cy = static_cast<long>(b.top + (b.bottom - b.top) / 2);
                            const long tx = static_cast<long>(
                                e.targetBounds.left + (e.targetBounds.right - e.targetBounds.left) / 2);
                            const long ty = static_cast<long>(
                                e.targetBounds.top + (e.targetBounds.bottom - e.targetBounds.top) / 2);
                            const long dx = (cx > tx) ? (cx - tx) : (tx - cx);
                            const long dy = (cy > ty) ? (cy - ty) : (ty - cy);
                            const long dist = dx + dy;
                            if (!haveBest || dist < bestDist) {
                                bestIdx = i;
                                bestDist = dist;
                                haveBest = true;
                            }
                        }
                        if (matchedIdx == static_cast<size_t>(-1) && haveBest) {
                            matchedIdx = bestIdx;
                        }
                    }
                    if (matchedIdx == static_cast<size_t>(-1) && e.hasTargetBounds) {
                        for (size_t i = 0; i < entry->preorder.size(); ++i) {
                            if (entry->bounds[i] == e.targetBounds) {
                                matchedIdx = i;
                                break;
                            }
                        }
                    }
                    if (matchedIdx != static_cast<size_t>(-1)) {
                        const uintptr_t xpH = entry->xpathHashes[matchedIdx];
                        if (xpH != 0) {
                            abstractTargetHash = xpH;
                        }
                    }
                }
            }
            const uintptr_t actionTypeH = std::hash<int>{}(static_cast<int>(e.actionType));
            *out = 0x9e3779b9 + (activityH << 2) ^ (((actionTypeH << 6) ^ (abstractTargetHash << 1)) << 1);
            return true;
        };

        // Pre-build stateHash -> xmlKeyHash(fromNaming) cache so the focus check can compare
        // XML-space hashes consistently, even when slot.sourceKeyHash is still Element-space.
        std::unordered_map<uintptr_t, uintptr_t> fromNamingKeyHashCache;
        if (hasFocus) {
            for (const auto &kv : _apeStateXmlByStateHash) {
                if (kv.second.empty()) {
                    continue;
                }
                uintptr_t h = 0;
                gui_tree::GUITreePtr snap = apeLatestGuiTreeSnapshot(kv.first);
                const gui_tree::GUITreePtr *fnSnapPtr = snap ? &snap : nullptr;
                if (apeStateHashFromXmlWithNaming(rawActivity, kv.second, fromNaming, &h, 0, nullptr, fnSnapPtr) && h != 0) {
                    fromNamingKeyHashCache.emplace(kv.first, h);
                }
            }
        }

        for (auto &slot : _apeTransitionLog) {
            if (!slot.valid || slot.sourceActivity != actKey) {
                continue;
            }
            // Q8 (affectedTrees): when focus is provided, only remap transition entries that
            // reference affected StateKey hashes. Keep other entries intact to avoid losing evidence.
            if (hasFocus) {
                auto xmlKeyForState = [&](uintptr_t stateHash, uintptr_t storedKeyHash) -> uintptr_t {
                    auto it = fromNamingKeyHashCache.find(stateHash);
                    return (it != fromNamingKeyHashCache.end()) ? it->second : storedKeyHash;
                };
                const uintptr_t srcXml = xmlKeyForState(slot.sourceStateHash, slot.sourceKeyHash);
                const bool inFocus = (focusOldKeyHashes->count(srcXml) != 0);
                if (!inFocus) {
                    const std::string *slotSrcXmlSnapshot = slot.sourceXmlSnapshot.empty()
                                                                ? nullptr
                                                                : &slot.sourceXmlSnapshot;
                    apeEvidencePoolAdd(ApePairKey{slot.sourceKeyHash, slot.actionHash}, slot,
                                       slotSrcXmlSnapshot);
                    continue;
                }
            }
            // Remove old-space aggregation first so that any remap failure won't leave stale counts.
            // Also avoid clearing pairAgg for other activities (Model.rebuild keeps unaffected evidence).
            apePairAggRemove(slot);
            const uintptr_t oldSrcStateHash = slot.sourceStateHash;
            const uintptr_t oldTgtStateHash = slot.targetStateHash;
            auto itSx = _apeStateXmlByStateHash.find(oldSrcStateHash);
            auto itTx = _apeStateXmlByStateHash.find(oldTgtStateHash);
            if (itSx == _apeStateXmlByStateHash.end() || itTx == _apeStateXmlByStateHash.end() ||
                itSx->second.empty() || itTx->second.empty()) {
                slot.valid = false;
                continue;
            }
            uintptr_t newSrcKeyHash = 0;
            uintptr_t newTgtKeyHash = 0;
            if (!getStateKeyHashUnderToNaming(oldSrcStateHash, itSx->second, &newSrcKeyHash) ||
                !getStateKeyHashUnderToNaming(oldTgtStateHash, itTx->second, &newTgtKeyHash)) {
                slot.valid = false;
                continue;
            }
            uintptr_t newActionHash = 0;
            if (!computeActionHash(oldSrcStateHash, itSx->second, slot, &newActionHash)) {
                slot.valid = false;
                continue;
            }
            slot.sourceKeyHash = newSrcKeyHash;
            slot.targetKeyHash = newTgtKeyHash;
            slot.actionHash = newActionHash;
            slot.actionIdentity = 0;
            // Try to preserve exact StateKey after naming remap (avoid hash-only refine/rollback).
            slot.hasSourceStateKey = false;
            slot.sourceStateKey = naming::StateKey::fromParts("", nullptr, {});
            slot.hasTargetStateKey = false;
            slot.targetStateKey = naming::StateKey::fromParts("", nullptr, {});
            {
                RemapTreeCacheEntry *entry = nullptr;
                if (getTreeEntry(oldSrcStateHash, itSx->second, &entry) && entry && entry->built.tree) {
                    if (!entry->hasStateKey) {
                        entry->stateKey = naming::StateKey::fromGUITree(*entry->built.tree);
                        entry->hasStateKey = true;
                    }
                    slot.sourceStateKey = entry->stateKey;
                    slot.hasSourceStateKey = true;
                }
            }
            {
                RemapTreeCacheEntry *entry = nullptr;
                if (getTreeEntry(oldTgtStateHash, itTx->second, &entry) && entry && entry->built.tree) {
                    if (!entry->hasStateKey) {
                        entry->stateKey = naming::StateKey::fromGUITree(*entry->built.tree);
                        entry->hasStateKey = true;
                    }
                    slot.targetStateKey = entry->stateKey;
                    slot.hasTargetStateKey = true;
                }
            }
            {
                const uintptr_t repSrc = findRepresentativeStateHash(newSrcKeyHash);
                slot.sourceStateHash = (repSrc != 0) ? repSrc : oldSrcStateHash;
            }
            {
                const uintptr_t repTgt = findRepresentativeStateHash(newTgtKeyHash);
                slot.targetStateHash = (repTgt != 0) ? repTgt : oldTgtStateHash;
            }
            // Re-add into aggregation under the new key space.
            apePairAggAdd(slot);
            if (slot.valid) {
                const std::string *slotSrcXmlSnapshot = slot.sourceXmlSnapshot.empty()
                                                            ? nullptr
                                                            : &slot.sourceXmlSnapshot;
                apeEvidencePoolAdd(ApePairKey{slot.sourceKeyHash, slot.actionHash}, slot,
                                   slotSrcXmlSnapshot);
            }
        }
#endif
    }

    /** @brief Enforces a cap on the GUI-tree naming blacklist size. */
    void Model::apeCapGuiTreeNamingBlacklist() {
        // no-op: match unbounded guiTreeNamingBlaclist.
    }
#endif

    /** @brief Coarsens naming for an activity when stability rules require it. */
    bool Model::coarsenActivityApeNamingIfNeeded(const std::string &activity) {
        const std::string actKey = naming::StateKey::canonicalActivityString(activity);
        ApeNamingAbstractionContext &ctx = _apeNamingContext[actKey];
        naming::ActivityNamingManager &mgr2 = _apeStateNamingManager->activityManager();
        naming::NamingPtr mgrCur = mgr2.getNaming(actKey);
        {
            static std::atomic<uint64_t> g_coarsen_entry_probe{0};
            const uint64_t ep = ++g_coarsen_entry_probe;
            if (ep <= 220 || (ep % 600) == 0) {
                const uintptr_t triggerKeyHashFromStateKey =
                    ctx.triggerSourceKeyExact ? ctx.triggerSourceKey.hash() : 0;
                BDLOG("naming BUG_PROBE [coarsen_entry_trigger_ctx] seq=%llu activity=%s mgrCur=%p "
                      "hasParent=%d trigHashCtx=%lu trigExact=%d trigHashStateKey=%lu "
                      "equalCtxVsStateKey=%d old2newSize=%zu oldObsSize=%zu",
                      static_cast<unsigned long long>(ep), actKey.c_str(),
                      static_cast<const void *>(mgrCur.get()),
                      (mgrCur && mgrCur->getParent()) ? 1 : 0,
                      static_cast<unsigned long>(ctx.triggerSourceKeyHash),
                      ctx.triggerSourceKeyExact ? 1 : 0,
                      static_cast<unsigned long>(triggerKeyHashFromStateKey),
                      (ctx.triggerSourceKeyExact &&
                       ctx.triggerSourceKeyHash == triggerKeyHashFromStateKey) ? 1 : 0,
                      ctx.oldKeyHashToNewKeyHashes.size(),
                      ctx.oldKeyHashToObservationCount.size());
            }
        }
        if (!mgrCur || !mgrCur->getParent()) {
            const char *detail = "missing_manager";
            int fineness = -1;
            const char *hasParent = "0";
            if (mgrCur) {
                detail = "at_root_no_parent";
                fineness = mgrCur->getFineness();
                hasParent = mgrCur->getParent() ? "1" : "0";
            }
            BDLOG("naming: coarsen skip activity=%s reason=missing_naming reason_detail=%s "
                  "mgrCur=%p hasParent=%s fineness=%d",
                  activity.c_str(), detail, static_cast<const void *>(mgrCur.get()), hasParent,
                  fineness);
            return false;
        }
        bool namingIndexWarmedThisCall = false;
        std::unordered_set<uintptr_t> totalNewKeys;
        for (const auto &p : ctx.oldKeyHashToNewKeyHashes) {
            totalNewKeys.insert(p.second.begin(), p.second.end());
        }
        size_t affectedStateObservations = 0;
        for (const auto &p : ctx.oldKeyHashToObservationCount) {
            affectedStateObservations += p.second;
        }
        
        constexpr int affectedThreshold = 8;
        int tnDepth = 0;
        unsigned coarsen_layers_walked = 0;
        unsigned coarsen_layers_skip_trig0 = 0;
        size_t coarsen_max_filtered_affected = 0;
        size_t coarsen_max_filtered_targets = 0;
        int coarsen_max_fa_depth = -1;
        int coarsen_max_ft_depth = -1;
        int coarsen_max_ft_thr_at_peak = -1;
        for (naming::NamingPtr tn = mgrCur; tn && tn->getParent(); tn = tn->getParent(), ++tnDepth) {
            coarsen_layers_walked++;
            const naming::NamingPtr targetParentNaming = tn->getParent();
            const int targetThreshold = apeMaxStatesForRefinementThreshold(tn);
            // Use the same trigger key for every ancestor layer; gating triggerSource on
            // tn->fingerprintString() == mgrCur->fingerprintString() skipped ancestor evaluation.
            const uintptr_t triggerSource = ctx.triggerSourceKeyHash;
            const bool sameFpAsMgrCur = (tn->fingerprintString() == mgrCur->fingerprintString());
            {
                static std::atomic<uint64_t> g_coarsen_loop_probe{0};
                const uint64_t lp = ++g_coarsen_loop_probe;
                if (lp <= 200 || (lp % 600) == 0) {
                    BDLOG("naming BUG_PROBE [coarsen_loop_probe] seq=%llu activity=%s depth=%d "
                          "tn=%p mgrCur=%p tnPar=%p trigHash=%lu triggerSource=%lu sameFpAsMgrCur=%d",
                          static_cast<unsigned long long>(lp), activity.c_str(), tnDepth,
                          static_cast<const void *>(tn.get()), static_cast<const void *>(mgrCur.get()),
                          static_cast<const void *>(targetParentNaming.get()),
                          static_cast<unsigned long>(ctx.triggerSourceKeyHash),
                          static_cast<unsigned long>(triggerSource), sameFpAsMgrCur ? 1 : 0);
                }
            }
            size_t filteredAffected = 0;
            size_t filteredTargets = 0;
            std::unordered_set<uintptr_t> filteredAffectedStateHashes;
            // optimization 4 (reference parity): recompute affectedStates/targets.size with the same
            // originState.equals(oldState) filtering semantics used by optimization 3.
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
            if (triggerSource == 0) {
                coarsen_layers_skip_trig0++;
                static std::atomic<uint64_t> g_coarsen_continue_trigger_zero{0};
                const uint64_t cz = ++g_coarsen_continue_trigger_zero;
                if (cz <= 160 || (cz % 500) == 0) {
                    BDLOG("naming BUG_PROBE [coarsen_continue_trigger_zero] seq=%llu activity=%s "
                          "depth=%d tn=%p mgrCur=%p trigHash=%lu sameFpAsMgrCur=%d",
                          static_cast<unsigned long long>(cz), activity.c_str(), tnDepth,
                          static_cast<const void *>(tn.get()), static_cast<const void *>(mgrCur.get()),
                          static_cast<unsigned long>(ctx.triggerSourceKeyHash),
                          sameFpAsMgrCur ? 1 : 0);
                }
                continue;
            }
            {
                std::unordered_set<std::string> subtreeFingerprints;
                collectNamingSubtreeFingerprints(tn, &subtreeFingerprints);
                std::vector<StatePtr> indexedCandidates;
                if (_graph) {
                    _graph->apeCollectStatesByNamingFingerprints(subtreeFingerprints,
                                                                 &indexedCandidates);
                    if (indexedCandidates.empty() && _graph->stateSize() > 0 &&
                        !namingIndexWarmedThisCall) {
                        warmApeNamingGraphIndex();
                        namingIndexWarmedThisCall = true;
                        _graph->apeCollectStatesByNamingFingerprints(subtreeFingerprints,
                                                                     &indexedCandidates);
                    }
                }
                const bool useNamingIndex = !indexedCandidates.empty();
                std::unordered_set<uintptr_t> distinctTargetKeys;
                auto considerState = [&](const StatePtr &sp) {
                    if (!sp) {
                        return;
                    }
                    auto apBa = sp->getActivityString();
                    const std::string aBa = (apBa && apBa.get())
                                                ? naming::StateKey::canonicalActivityString(*apBa)
                                                : std::string();
                    if (aBa != actKey) {
                        return;
                    }
                    const uintptr_t ghBa = sp->hash();
                    if (useNamingIndex) {
                        naming::StateKey stateSk = naming::StateKey::fromParts("", nullptr, {});
                        if (!tryGetApeStateKey(ghBa, &stateSk, actKey)) {
                            return;
                        }
                        if (subtreeFingerprints.find(stateSk.namingFingerprint()) ==
                            subtreeFingerprints.end()) {
                            return;
                        }
                    }
                    uintptr_t storedKeyH = 0;
                    const bool haveStoredApeKey = tryGetApeStateKeyHash(ghBa, &storedKeyH, actKey);
                    const uintptr_t khBa = haveStoredApeKey ? storedKeyH : ghBa;

                    auto itXml = _apeStateXmlByStateHash.find(ghBa);
                    const bool haveXml = (itXml != _apeStateXmlByStateHash.end() && !itXml->second.empty());
                    if (!haveXml) {
                        return;
                    }
                    uintptr_t tgtKeyHash = 0;
                    uintptr_t oldH = 0;
                    apeStateKeyPairFromXmlCoarsenPath(activity, itXml->second, tn,
                                                      &tgtKeyHash, targetParentNaming, &oldH);

                    const bool affectedBa = (oldH == triggerSource);

                    if (affectedBa) {
                        filteredAffected++;
                        filteredAffectedStateHashes.insert(ghBa);
                        if (tgtKeyHash != 0) {
                            distinctTargetKeys.insert(tgtKeyHash);
                        } else if (haveStoredApeKey) {
                            distinctTargetKeys.insert(khBa);
                        }
                    }
                };
                if (useNamingIndex) {
                    for (const auto &sp : indexedCandidates) {
                        considerState(sp);
                    }
                } else {
                    for (const auto &sp : getGraph()->getStates()) {
                        considerState(sp);
                    }
                }
                filteredTargets = distinctTargetKeys.size();
            }
#else
            if (triggerSource == 0) {
                coarsen_layers_skip_trig0++;
                static std::atomic<uint64_t> g_coarsen_continue_trigger_zero{0};
                const uint64_t cz = ++g_coarsen_continue_trigger_zero;
                if (cz <= 160 || (cz % 500) == 0) {
                    BDLOG("naming BUG_PROBE [coarsen_continue_trigger_zero] seq=%llu activity=%s "
                          "depth=%d tn=%p mgrCur=%p trigHash=%lu sameFpAsMgrCur=%d",
                          static_cast<unsigned long long>(cz), activity.c_str(), tnDepth,
                          static_cast<const void *>(tn.get()), static_cast<const void *>(mgrCur.get()),
                          static_cast<unsigned long>(ctx.triggerSourceKeyHash),
                          sameFpAsMgrCur ? 1 : 0);
                }
                continue;
            }
            {
                auto itFiltered = ctx.oldKeyHashToNewKeyHashes.find(triggerSource);
                if (itFiltered != ctx.oldKeyHashToNewKeyHashes.end()) {
                    auto itCnt = ctx.oldKeyHashToObservationCount.find(triggerSource);
                    filteredAffected = (itCnt == ctx.oldKeyHashToObservationCount.end()) ? 0 : itCnt->second;
                    filteredTargets = itFiltered->second.size();
                }
            }
        
#endif
            if (filteredAffected > coarsen_max_filtered_affected) {
                coarsen_max_filtered_affected = filteredAffected;
                coarsen_max_fa_depth = tnDepth;
            }
            if (filteredTargets > coarsen_max_filtered_targets) {
                coarsen_max_filtered_targets = filteredTargets;
                coarsen_max_ft_depth = tnDepth;
                coarsen_max_ft_thr_at_peak = targetThreshold;
            }
            const bool overFilteredAffected = filteredAffected > static_cast<size_t>(affectedThreshold);
            const bool overFilteredTargets = filteredTargets > static_cast<size_t>(targetThreshold);
            
            const bool shouldRollback = overFilteredAffected || overFilteredTargets;
            if (!shouldRollback) {
                continue;
            }
            naming::NamingPtr rollbackFrom = tn;
            naming::NamingPtr rollbackTo = targetParentNaming;
            std::string fpFiner = rollbackFrom->fingerprintString();
            std::unordered_set<uintptr_t> affectedStateHashesForBlacklist = filteredAffectedStateHashes;
            apeBlacklistFinerNamingOnRollback(activity, rollbackFrom, ctx, affectedStateHashesForBlacklist);
            {
                static std::atomic<uint64_t> g_coarsen_chain{0};
                const uint64_t cn = ++g_coarsen_chain;
                if (cn <= 10 || (cn % 128) == 0) {
                    const uintptr_t triggerKeyHashFromStateKey =
                        ctx.triggerSourceKeyExact ? ctx.triggerSourceKey.hash() : 0;
                    BDLOG(
                        "naming: chain coarsen rollback Abstract update act=%s rollbackFrom=%p rollbackTo=%p "
                        "trigExact=%d trigSrcH=%lu trigSrcH(stateKey)=%lu equal=%d",
                        actKey.c_str(), static_cast<const void *>(rollbackFrom.get()),
                        static_cast<const void *>(rollbackTo.get()),
                        ctx.triggerSourceKeyExact ? 1 : 0,
                        static_cast<unsigned long>(ctx.triggerSourceKeyHash),
                        static_cast<unsigned long>(triggerKeyHashFromStateKey),
                        (ctx.triggerSourceKeyExact &&
                         ctx.triggerSourceKeyHash == triggerKeyHashFromStateKey) ? 1 : 0);
                }
            }
            {
                static std::atomic<uint64_t> g_coarsen_trigger_keyspace_diag{0};
                const uint64_t n = ++g_coarsen_trigger_keyspace_diag;
                if (n <= 120 || (n % 400) == 0) {
                    const uintptr_t triggerKeyHashFromStateKey =
                        ctx.triggerSourceKeyExact ? ctx.triggerSourceKey.hash() : 0;
                    BLOG("naming: coarsen trigger keyspace seq=%" PRIu64
                         " activity=%s triggerSource=%lu trigHash(ctx)=%lu trigHash(stateKey)=%lu equal=%d "
                         "trigExact=%d filteredAffected=%zu filteredTargets=%zu thresholdA=%d thresholdT=%d",
                         n, actKey.c_str(), static_cast<unsigned long>(triggerSource),
                         static_cast<unsigned long>(ctx.triggerSourceKeyHash),
                         static_cast<unsigned long>(triggerKeyHashFromStateKey),
                         (ctx.triggerSourceKeyExact &&
                          ctx.triggerSourceKeyHash == triggerKeyHashFromStateKey) ? 1 : 0,
                         ctx.triggerSourceKeyExact ? 1 : 0,
                         filteredAffected, filteredTargets, affectedThreshold, targetThreshold);
                }
            }
            _apeStateNamingManager->updateNamingWithStateKey(
                actKey, naming::NamingUpdateKind::Abstract, rollbackFrom, rollbackTo,
                ctx.triggerSourceKey);
            invalidateApeGraphStateKeyDedupMap();
            std::unordered_set<uintptr_t> affectedStateHashsForPrune = filteredAffectedStateHashes;
            constexpr bool rebuiltViaHistory = false;
            std::vector<uintptr_t> repKeyHashes;
            std::unordered_set<uintptr_t> focusOldKeyHashes;
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
            if (!rebuiltViaHistory && !filteredAffectedStateHashes.empty()) {
                for (uintptr_t sh : affectedStateHashsForPrune) {
                    auto itXml = _apeStateXmlByStateHash.find(sh);
                    if (itXml == _apeStateXmlByStateHash.end() || itXml->second.empty()) {
                        continue;
                    }
                    uintptr_t oldH = 0;
                    uintptr_t newH = 0;
                    gui_tree::GUITreePtr rbSnap = this->apeLatestGuiTreeSnapshot(sh);
                    const gui_tree::GUITreePtr *rbSnapPtr = rbSnap ? &rbSnap : nullptr;
                    if (!apeStateHashFromXmlWithNaming(activity, itXml->second, rollbackFrom, &oldH, 0, nullptr,
                                                       rbSnapPtr) ||
                        !apeStateHashFromXmlWithNaming(activity, itXml->second, rollbackTo, &newH, 0, nullptr,
                                                       rbSnapPtr)) {
                        continue;
                    }
                    if (oldH != newH) {
                        focusOldKeyHashes.insert(oldH);
                    }
                }
            }
#endif
            if (!rebuiltViaHistory) {
                repKeyHashes.reserve(8);
                if (!focusOldKeyHashes.empty()) {
                    for (uintptr_t h : focusOldKeyHashes) {
                        repKeyHashes.push_back(h);
                        if (repKeyHashes.size() >= 8) {
                            break;
                        }
                    }
                } else if (ctx.triggerSourceKeyHash != 0) {
                    repKeyHashes.push_back(ctx.triggerSourceKeyHash);
                }
                if (!repKeyHashes.empty()) {
                    rebuildApeStateRepresentativesForKeyHashes(activity, rollbackFrom, repKeyHashes, 1);
                }
                remapApeTransitionAggregationForActivity(
                    activity, rollbackFrom, rollbackTo,
                    focusOldKeyHashes.empty() ? nullptr : &focusOldKeyHashes);
                pruneStaleApeStatesForActivity(actKey, fpFiner, nullptr);
            }
            if (!affectedStateHashsForPrune.empty()) {
                removeConflictingApeActionRefinementPredicates(activity, rollbackTo,
                                                               affectedStateHashsForPrune);
                removeConflictingApeSourceDivergentPredicates(activity, rollbackTo,
                                                              affectedStateHashsForPrune);
                removeConflictingApeStatesFewerThanPredicates(activity, rollbackTo,
                                                              affectedStateHashsForPrune);
                addApeStatesFewerThanPredicate(activity, affectedStateHashsForPrune, targetThreshold);
            }
            _apeNamingCoarseningBlacklist.insert(std::make_pair(actKey, fpFiner));
            apeCapApeNamingCoarsenAndRefineBlacklists();
            BDLOG("naming: coarsen activity=%s rollback overFilteredAffected=%d overFilteredTargets=%d "
                  "affectedStates=%zu totalNew=%zu filteredAffected=%zu filteredTargets=%zu rebuilt=%d "
                  "targetThreshold=%d triggerSource=%lu fp=%s",
                  activity.c_str(), overFilteredAffected ? 1 : 0, overFilteredTargets ? 1 : 0,
                  affectedStateObservations, totalNewKeys.size(), filteredAffected, filteredTargets,
                  rebuiltViaHistory ? 1 : 0, targetThreshold, (unsigned long)triggerSource,
                  fpFiner.c_str());
            ctx.oldKeyHashToNewKeyHashes.clear();
            ctx.oldKeyHashToObservationCount.clear();
            ctx.previousNamingBeforeRefine = nullptr;
            ctx.previousNamingFingerprintBeforeRefine.clear();
            ctx.triggerSourceKeyHash = 0;
            ctx.triggerSourceKeyExact = false;
            ctx.triggerSourceKey = naming::StateKey::fromParts("", nullptr, {});
            ctx.triggerActionHash = 0;
            ctx.triggerTargetKeyHashes.clear();
            ctx.triggerTargetCountAtRefine = 0;
            ctx.lastRefineSeedSeq = 0;
            ctx.lastRefineSeedTriggerHash = 0;
            ctx.stateCountAtLastNamingRefinement = getApeStateCountByActivityAndNamingFingerprint(
                actKey, rollbackTo ? rollbackTo->fingerprintString() : std::string());
            ApeActivityRebuildStats &st = _apeRebuildStatsByActivity[actKey];
            ++st.consecutiveRollbacks;
            return true;
        }
        _apeRebuildStatsByActivity[actKey].consecutiveRollbacks = 0;
        const bool coarsen_diag_verbose_activity =
            (actKey.find("CountryListActivity") != std::string::npos);
        static std::atomic<uint64_t> g_coarsen_keep_global_seq{0};
        const uint64_t keepSeq = ++g_coarsen_keep_global_seq;
        const bool emit_full_keep_diag =
            coarsen_diag_verbose_activity || keepSeq <= 120 || (keepSeq % 400) == 0;
        if (emit_full_keep_diag) {
            BDLOG(
                "naming: coarsen_decision outcome=keep reason=no_layer_hit_thresholds activity=%s "
                "actKey=%s layersWalked=%u layersSkipTrig0=%u maxFilteredAffected=%zu maxFilteredTargets=%zu "
                "maxFaDepth=%d maxFtDepth=%d thrAffected=%d targetThrAtMaxFt=%d "
                "lastRefineSeedSeq=%llu trigHashCtx=%lu old2newBuckets=%zu oldObsBuckets=%zu "
                "affectedObsTotal=%zu totalNewDist=%zu mgrFineness=%d",
                activity.c_str(), actKey.c_str(), coarsen_layers_walked, coarsen_layers_skip_trig0,
                coarsen_max_filtered_affected, coarsen_max_filtered_targets, coarsen_max_fa_depth,
                coarsen_max_ft_depth, affectedThreshold, coarsen_max_ft_thr_at_peak,
                static_cast<unsigned long long>(ctx.lastRefineSeedSeq),
                static_cast<unsigned long>(ctx.triggerSourceKeyHash),
                ctx.oldKeyHashToNewKeyHashes.size(), ctx.oldKeyHashToObservationCount.size(),
                affectedStateObservations, totalNewKeys.size(),
                mgrCur ? mgrCur->getFineness() : -1);
        } else {
            BDLOG("naming: coarsen keep refinement activity=%s rollback=0", activity.c_str());
        }
        return false;
    }
#endif

    /** @brief Records that an activity string was seen (coverage / bookkeeping). */
    void Model::reportActivity(const std::string &activity) {
        if (activity.empty()) return;
        std::lock_guard<std::mutex> lock(_coverageMutex);
        _visitedActivities.insert(activity);
        _coverageStepCount++;
    }

    /** @brief Returns exploration coverage metrics as a JSON string. */
    std::string Model::getCoverageJson() const {
        std::lock_guard<std::mutex> lock(_coverageMutex);
        nlohmann::json j;
        j["stepsCount"] = _coverageStepCount;
        nlohmann::json arr = nlohmann::json::array();
        for (const auto &a : _visitedActivities) {
            arr.push_back(a);
        }
        j["testedActivities"] = arr;
        return jsonDumpUtf8Safe(j);
    }

    /** @brief Returns the stagnation metric used by the exploration scheduler. */
    double Model::getLlmdroidStagnationMetric() const {
        std::lock_guard<std::mutex> lock(_coverageMutex);
        const size_t states = _graph ? _graph->stateSize() : 0;
        const size_t acts = _visitedActivities.size();
        const int steps = _coverageStepCount;
        return static_cast<double>(states) + 0.01 * static_cast<double>(acts) +
               1e-9 * static_cast<double>(steps);
    }

    /** @brief Loads persisted dynamic state-abstraction policy from storage. */
    void Model::loadStateAbstractionPolicy() {
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        auto pref = Preference::inst();
        // Only meaningful when dynamic abstraction is actually used and policy persistence is enabled.
        if (!pref || pref->useStaticReuseAbstraction() || !pref->isStateAbstractionPolicyEnabled()) {
            return;
        }

        const std::string &pkg = getPackageName();
        if (pkg.empty()) {
            return;
        }

        std::string path = "/sdcard/fastbot_" + pkg + ".statekey.json";
        BLOG("state abstraction: try load policy from %s", path.c_str());
        std::ifstream in(path);
        if (!in.is_open()) {
            return;
        }

        try {
            // Basic size guard to avoid attempting to parse extremely large / corrupted files.
            in.seekg(0, std::ios::end);
            std::streamoff sz = in.tellg();
            if (sz <= 0 || sz > static_cast<std::streamoff>(1024 * 1024)) { // 1MB hard upper bound
                BLOGE("state abstraction: skip loading %s (size=%lld bytes out of bounds)", path.c_str(),
                      static_cast<long long>(sz));
                return;
            }
            in.seekg(0, std::ios::beg);

            nlohmann::json j;
            in >> j;

            if (!j.is_object()) {
                BLOGE("state abstraction: policy file %s is not a JSON object", path.c_str());
                return;
            }

            // v1 files may contain widget-key masks and coarseningBlacklist (legacy); do not apply — dynamic
            // identity is StateKey-only; keeping old entries would confuse debugging.
            auto itActs = j.find("activities");
            if (itActs != j.end() && itActs->is_array() && !itActs->empty()) {
                BLOG("state abstraction: %s contains legacy activities[]; ignored", path.c_str());
            }
            auto itBlk = j.find("coarseningBlacklist");
            if (itBlk != j.end() && itBlk->is_array() && !itBlk->empty()) {
                BLOG("state abstraction: %s contains legacy coarseningBlacklist; ignored", path.c_str());
            }

            BLOG("state abstraction: loaded policy metadata from %s (no widget-mask state applied)", path.c_str());
        } catch (const std::exception &ex) {
            BLOGE("state abstraction: failed to load policy from %s: %s", path.c_str(), ex.what());
        }
#else
        (void)this;
#endif
    }

    /** @brief Persists dynamic state-abstraction policy to storage. */
    void Model::saveStateAbstractionPolicy() const {
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        auto pref = Preference::inst();
        // Only meaningful when dynamic abstraction is actually used and policy persistence is enabled.
        if (!pref || pref->useStaticReuseAbstraction() || !pref->isStateAbstractionPolicyEnabled()) {
            return;
        }

        const std::string &pkg = getPackageName();
        if (pkg.empty()) {
            return;
        }

        std::string path = "/sdcard/fastbot_" + pkg + ".statekey.json";
        std::string tmpPath = path + ".tmp";

        nlohmann::json j;
        j["version"] = 2;
        j["activities"] = nlohmann::json::array();

        try {
            std::ofstream out(tmpPath, std::ios::trunc);
            if (!out.is_open()) {
                BLOGE("state abstraction: cannot open temp policy file %s for writing", tmpPath.c_str());
                return;
            }
            out << jsonDumpUtf8Safe(j);
            out.close();
            if (std::rename(tmpPath.c_str(), path.c_str()) != 0) {
                BLOGE("state abstraction: failed to rename policy file %s -> %s", tmpPath.c_str(), path.c_str());
            } else {
                BLOG("state abstraction: policy saved to %s", path.c_str());
            }
        } catch (const std::exception &ex) {
            BLOGE("state abstraction: failed to save policy to %s: %s", path.c_str(), ex.what());
        }
#else
        (void)this;
#endif
    }

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
    /** @brief Logs abstract state key fields for one state (debug). */
    void Model::logApeStateKeySnapshot(const std::string &rawActivity, const StatePtr &state,
                                       const naming::StateKey &key, const GraphPtr &graph) {
        const auto &xs = key.sortedXPaths();
        std::ostringstream head;
        head << "state-key: activity=" << key.activity()
             << ";hash=" << static_cast<unsigned long>(key.hash())
             << ";naming=" << key.namingFingerprint()
             << ";widgets=" << xs.size();
        BDLOG("%s", head.str().c_str());
        for (size_t i = 0; i < xs.size(); ++i) {
            BDLOG("  %zu xpath=%s", static_cast<unsigned long>(i), xs[i].c_str());
        }
        (void)rawActivity;
        (void)state;
        (void)graph;
    }

#endif

#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML

    /** @brief Builds the abstract state key from an element tree and activity context. */
    bool Model::buildApeStateKeyFromElementTree(const ElementPtr &element, const std::string &activity,
                                               naming::StateKey *outKey,
                                               ApeStateKeyBuildFailReason *outFailReason,
                                               const StatePtr &stateForDynamicApply,
                                               std::string *ioXmlCache) {
        auto fail = [&](ApeStateKeyBuildFailReason reason) -> bool {
            if (outFailReason != nullptr) {
                *outFailReason = reason;
            }
            _ape_correctness_counters.statekey_build_fail++;
            switch (reason) {
                case ApeStateKeyBuildFailReason::NullInput:
                    _ape_correctness_counters.statekey_fail_null_input++;
                    break;
                case ApeStateKeyBuildFailReason::BuildTreeOrDomFailed:
                    _ape_correctness_counters.statekey_fail_build_tree_dom++;
                    break;
                case ApeStateKeyBuildFailReason::NoNaming:
                    _ape_correctness_counters.statekey_fail_no_naming++;
                    break;
                case ApeStateKeyBuildFailReason::RebuildTreeFailed:
                    _ape_correctness_counters.statekey_fail_rebuild_tree++;
                    break;
                case ApeStateKeyBuildFailReason::None:
                default:
                    break;
            }
            return false;
        };

        if (!element || !outKey) {
            return fail(ApeStateKeyBuildFailReason::NullInput);
        }
        // When static reuse abstraction is enabled, we must not sync naming fixed-point
        // refinement or update dynamic naming bookkeeping; we only need enough naming to
        // compute the StateKey identity.
        const bool wantApeRlIdentity =
            !_preference || !_preference->useStaticReuseAbstraction();
        std::string pkg;
        std::string cls;
        naming::StateKey::splitActivityPackageClass(activity, &pkg, &cls);
        const std::string actKey = naming::StateKey::canonicalActivityString(activity);
        // Lazily serialize once per call; share XML string across tree fallback + XML-space remap hashes.
        std::string xmlSnapshotMemo;
        bool xmlSnapshotMemoValid = false;
        auto xmlSnapshotRef = [&]() -> const std::string & {
            if (ioXmlCache && !ioXmlCache->empty()) {
                return *ioXmlCache;
            }
            if (xmlSnapshotMemoValid) {
                return xmlSnapshotMemo;
            }
            xmlSnapshotMemo = element->toXMLCached();
            xmlSnapshotMemoValid = true;
            if (ioXmlCache) {
                *ioXmlCache = xmlSnapshotMemo;
            }
            return xmlSnapshotMemo;
        };
        static std::atomic<uint64_t> g_build_ape_statekey{0};
        const uint64_t seq = ++g_build_ape_statekey;
        const std::string &xmlForEntryLog = xmlSnapshotRef();
        const uint64_t xmlSigForEntryLog = hashStringForLog(xmlForEntryLog);
        if (seq <= 20 || (seq % 400) == 0) {
            BDLOG("naming statekey: build source activity=%s seq=%" PRIu64
                  " elementPtr=%p statePtr=%p stateHash=%" PRIuPTR
                  " xmlSig=%" PRIu64 " xmlLen=%zu %s",
                  activity.c_str(), seq, element.get(), stateForDynamicApply.get(),
                  static_cast<uintptr_t>(stateForDynamicApply ? stateForDynamicApply->hash() : 0),
                  xmlSigForEntryLog, xmlForEntryLog.size(), summarizeElementForLog(element).c_str());
        }
        gui_tree::GUITreeBuildResult built = gui_tree::GUITreeFactory::buildFromElement(element, pkg, cls);
        if (seq <= 20 || (seq % 400) == 0) {
            BDLOG("naming statekey: tree stage=after_buildFromElement activity=%s seq=%" PRIu64
                  " treePtr=%p dom=%d %s",
                  activity.c_str(), seq, built.tree.get(), built.dom ? 1 : 0,
                  summarizeGUITreeForLog(built.tree).c_str());
        }
        if (!built.tree || !built.dom) {
#if defined(DYNAMIC_STATE_ABSTRACTION_ENABLED) && DYNAMIC_STATE_ABSTRACTION_ENABLED
            gui_tree::GUITreePtr fbSnap;
            if (stateForDynamicApply && stateForDynamicApply->hash() != 0) {
                fbSnap = apeLatestGuiTreeSnapshot(stateForDynamicApply->hash());
            }
            const std::string &xs = xmlSnapshotRef();
            built =
                fbSnap ? buildGuitreePreferApeSnapshotAndDomXml(xs, pkg, cls, fbSnap)
                       : buildGuitreeFromCachedXmlPreferElement(xs, pkg, cls);
#else
            built = buildGuitreeFromCachedXmlPreferElement(xmlSnapshotRef(), pkg, cls);
#endif
            if (seq <= 20 || (seq % 400) == 0) {
                BDLOG("naming statekey: tree stage=after_fallback_build activity=%s seq=%" PRIu64
                      " treePtr=%p dom=%d %s",
                      activity.c_str(), seq, built.tree.get(), built.dom ? 1 : 0,
                      summarizeGUITreeForLog(built.tree).c_str());
            }
        }
        if (!built.tree || !built.dom) {
            return fail(ApeStateKeyBuildFailReason::BuildTreeOrDomFailed);
        }
        naming::ActivityNamingManager &mgr = _apeStateNamingManager->activityManager();
        const int fpSteps = (_preference && wantApeRlIdentity)
                                ? _preference->getApeNamingFixedPointMaxIter()
                                : 0;
        naming::NamingPtr naming;
        if (fpSteps > 0) {
            naming::NamingPtr beforeNaming = mgr.getNaming(actKey);
            std::string fpBefore;
            if (beforeNaming) {
                fpBefore = beforeNaming->fingerprintString();
            }
            naming = _apeStateNamingManager->getNamingFixedPoint(actKey, *built.tree, built.dom, fpSteps);
            if (!naming) {
                return fail(ApeStateKeyBuildFailReason::NoNaming);
            }
            if (seq <= 20 || (seq % 400) == 0) {
                BDLOG("naming statekey: tree stage=after_getNamingFixedPoint activity=%s seq=%" PRIu64
                      " treePtr=%p namingFp=%s %s",
                      activity.c_str(), seq, built.tree.get(), naming->fingerprintString().c_str(),
                      summarizeGUITreeForLog(built.tree).c_str());
            }
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
            const std::string fpAfter = naming->fingerprintString();
            if (fpAfter != fpBefore) {
                // getNamingFixedPoint persists the new naming into ActivityNamingManager.
                // Align the rest of the runtime caches with the new key space.
                invalidateApeGraphStateKeyDedupMap();
                uintptr_t focusOldKeyHash = 0;
                uintptr_t focusOldKeyHashXml = 0;
                if (beforeNaming && built.tree && built.dom) {
                    if (safeRebuildTree(beforeNaming, *built.tree, built.dom)) {
                        focusOldKeyHash = naming::StateKey::hashFromGUITree(*built.tree);
                    }
                    if (!safeRebuildTree(naming, *built.tree, built.dom)) {
                        // Rebuild under current naming failed; tree may be stuck in beforeNaming state.
                        // Re-run getNamingFixedPoint to restore the tree to a consistent state.
                        naming = _apeStateNamingManager->getNamingFixedPoint(
                            actKey, *built.tree, built.dom, fpSteps);
                        if (!naming) {
                            return fail(ApeStateKeyBuildFailReason::NoNaming);
                        }
                    }
                }
                if (seq <= 20 || (seq % 400) == 0) {
                    BDLOG("naming statekey: tree stage=after_refine_rebuild activity=%s seq=%" PRIu64
                          " treePtr=%p namingFp=%s beforeFp=%s %s",
                          activity.c_str(), seq, built.tree.get(), naming->fingerprintString().c_str(),
                          fpBefore.c_str(), summarizeGUITreeForLog(built.tree).c_str());
                }
                // Recompute focusOldKeyHash in XML-space for affectedTrees comparison
                // (apeStateHashFromXmlWithNaming uses buildFromXml which can differ from buildFromElement).
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
                if (focusOldKeyHash != 0 && beforeNaming) {
                    const std::string &xmlForRemap = xmlSnapshotRef();
                    if (!xmlForRemap.empty()) {
                        uintptr_t xmh = 0;
#if defined(DYNAMIC_STATE_ABSTRACTION_ENABLED) && DYNAMIC_STATE_ABSTRACTION_ENABLED
                        gui_tree::GUITreePtr fpSnap;
                        const gui_tree::GUITreePtr *fpSnapPtr = nullptr;
                        if (stateForDynamicApply && stateForDynamicApply->hash() != 0) {
                            fpSnap = apeLatestGuiTreeSnapshot(stateForDynamicApply->hash());
                            fpSnapPtr = fpSnap ? &fpSnap : nullptr;
                        }
                        if (apeStateHashFromXmlWithNaming(activity, xmlForRemap, beforeNaming, &xmh, 0, nullptr,
                                                          fpSnapPtr) &&
#else
                        if (apeStateHashFromXmlWithNaming(activity, xmlForRemap, beforeNaming, &xmh) &&
#endif
                            xmh != 0) {
                            focusOldKeyHashXml = xmh;
                        }
                    }
                    if (focusOldKeyHashXml == 0) {
                        focusOldKeyHashXml = focusOldKeyHash;
                    }
                }
#else
                focusOldKeyHashXml = focusOldKeyHash;
#endif
                std::unordered_set<uintptr_t> focusOldKeyHashes;
                const uintptr_t focusForRemap = (focusOldKeyHashXml != 0) ? focusOldKeyHashXml : focusOldKeyHash;
                if (focusForRemap != 0) {
                    focusOldKeyHashes.insert(focusForRemap);
                }
                remapApeTransitionAggregationForActivity(
                    activity, beforeNaming, naming,
                    focusOldKeyHashes.empty() ? nullptr : &focusOldKeyHashes);
                std::unordered_set<uintptr_t> affectedTrees;
                if (focusOldKeyHashXml != 0 && beforeNaming) {
                    for (const auto &kv : _apeStateXmlByStateHash) {
                        const uintptr_t sh = kv.first;
                        const std::string &xml = kv.second;
                        if (xml.empty()) {
                            continue;
                        }
                        naming::StateKey storedKey = naming::StateKey::fromParts("", nullptr, {});
                        if (!tryGetApeStateKey(sh, &storedKey, actKey) || storedKey.activity() != actKey) {
                            continue;
                        }
                        uintptr_t oldH = 0;
#if defined(DYNAMIC_STATE_ABSTRACTION_ENABLED) && DYNAMIC_STATE_ABSTRACTION_ENABLED
                        gui_tree::GUITreePtr afSnap = apeLatestGuiTreeSnapshot(sh);
                        const gui_tree::GUITreePtr *afSnapPtr = afSnap ? &afSnap : nullptr;
                        if (apeStateHashFromXmlWithNaming(activity, xml, beforeNaming, &oldH, 0, nullptr,
                                                          afSnapPtr) &&
#else
                        if (apeStateHashFromXmlWithNaming(activity, xml, beforeNaming, &oldH) &&
#endif
                            oldH == focusOldKeyHashXml) {
                            affectedTrees.insert(sh);
                        }
                    }
                }
                pruneStaleApeStatesForActivity(actKey, fpBefore, nullptr);
                notifyAgentsOfApeNamingChange();
            }
#endif
        } else {
            naming = mgr.getNaming(actKey);
            if (!naming) {
                naming = naming::NamingFactory::defaultRootNaming();
                // In static reuse abstraction mode, avoid syncing a newly created naming into the manager.
                if (naming && wantApeRlIdentity) {
                    _apeStateNamingManager->updateNaming(
                        actKey, naming::NamingUpdateKind::Refine, naming);
                    pruneDivergentApeStatesForActivity(actKey);
                }
            }
            if (!naming) {
                return fail(ApeStateKeyBuildFailReason::NoNaming);
            }
            if (!safeRebuildTree(naming, *built.tree, built.dom)) {
                return fail(ApeStateKeyBuildFailReason::RebuildTreeFailed);
            }
            if (seq <= 20 || (seq % 400) == 0) {
                BDLOG("naming statekey: tree stage=after_safeRebuildTree activity=%s seq=%" PRIu64
                      " treePtr=%p namingFp=%s %s",
                      activity.c_str(), seq, built.tree.get(), naming->fingerprintString().c_str(),
                      summarizeGUITreeForLog(built.tree).c_str());
            }
        }
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        if (wantApeRlIdentity) {
            auto itCtx = _apeNamingContext.find(actKey);
            if (itCtx != _apeNamingContext.end() && itCtx->second.previousNamingBeforeRefine) {
                naming::NamingPtr prevN = itCtx->second.previousNamingBeforeRefine;
                uintptr_t oldH = 0;
                uintptr_t newH = 0;
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
                {
                    const std::string &xmlForRemap = xmlSnapshotRef();
                    if (!xmlForRemap.empty()) {
                        uintptr_t xmlOldH = 0;
                        uintptr_t xmlNewH = 0;
#if defined(DYNAMIC_STATE_ABSTRACTION_ENABLED) && DYNAMIC_STATE_ABSTRACTION_ENABLED
                        gui_tree::GUITreePtr ctxSnap;
                        const gui_tree::GUITreePtr *ctxSnapPtr = nullptr;
                        if (stateForDynamicApply && stateForDynamicApply->hash() != 0) {
                            ctxSnap = apeLatestGuiTreeSnapshot(stateForDynamicApply->hash());
                            ctxSnapPtr = ctxSnap ? &ctxSnap : nullptr;
                        }
                        if (apeStateHashFromXmlWithTwoNamings(activity, xmlForRemap, prevN, &xmlOldH, naming, &xmlNewH,
                                                              ctxSnapPtr)) {
#else
                        if (apeStateHashFromXmlWithTwoNamings(activity, xmlForRemap, prevN, &xmlOldH, naming, &xmlNewH)) {
#endif
                            oldH = xmlOldH;
                            newH = xmlNewH;
                        }
                    }
                }
#endif
                // Fallback to Element-space if XML remap unavailable.
                if (oldH == 0 && newH == 0) {
                    if (safeRebuildTree(prevN, *built.tree, built.dom)) {
                        oldH = naming::StateKey::hashFromGUITree(*built.tree);
                    }
                    if (!safeRebuildTree(naming, *built.tree, built.dom)) {
                        oldH = 0;
                        newH = 0;
                    } else {
                        newH = naming::StateKey::hashFromGUITree(*built.tree);
                    }
                }
                if (oldH != 0 && newH != 0 && oldH != newH) {
                    itCtx->second.oldKeyHashToNewKeyHashes[oldH].insert(newH);
                    itCtx->second.oldKeyHashToObservationCount[oldH]++;
                }
            }
        }
#endif
        if (naming && built.tree) {
            const std::string &xmlForCompare = xmlSnapshotRef();
            if (!xmlForCompare.empty()) {
                const uintptr_t elementPathHash = naming::StateKey::hashFromGUITree(*built.tree);
                uintptr_t xmlPathHash = 0;
                int hasCmpSnap = 0;
#if defined(DYNAMIC_STATE_ABSTRACTION_ENABLED) && DYNAMIC_STATE_ABSTRACTION_ENABLED
                gui_tree::GUITreePtr cmpSnap;
                const gui_tree::GUITreePtr *cmpSnapPtr = nullptr;
                if (stateForDynamicApply && stateForDynamicApply->hash() != 0) {
                    cmpSnap = apeLatestGuiTreeSnapshot(stateForDynamicApply->hash());
                    cmpSnapPtr = cmpSnap ? &cmpSnap : nullptr;
                    hasCmpSnap = cmpSnap ? 1 : 0;
                }
                const bool haveXmlPathHash = apeStateHashFromXmlWithNaming(
                    activity, xmlForCompare, naming, &xmlPathHash, 0, nullptr, cmpSnapPtr);
#else
                const bool haveXmlPathHash =
                    apeStateHashFromXmlWithNaming(activity, xmlForCompare, naming, &xmlPathHash);
#endif
                // Low-frequency alignment probe: report element/xml hash pair even when equal,
                // so we can compute mismatch rate from logs instead of only seeing failures.
                static std::atomic<uint64_t> g_ape_hash_align_seq{0};
                const uint64_t alignSeq = ++g_ape_hash_align_seq;
                if (haveXmlPathHash && elementPathHash != 0 && xmlPathHash != 0 &&
                    (alignSeq <= 120 || (alignSeq % 500) == 0)) {
                    const uintptr_t stateHashForAlign =
                        (stateForDynamicApply && stateForDynamicApply->hash() != 0)
                            ? stateForDynamicApply->hash()
                            : 0;
                    BDLOG("naming statekey: element/xml hash align seq=%" PRIu64
                          " activity=%s stateHash=%lu elementH=%lu xmlH=%lu equal=%d hasSnap=%d xmlLen=%zu",
                          alignSeq, activity.c_str(), static_cast<unsigned long>(stateHashForAlign),
                          static_cast<unsigned long>(elementPathHash),
                          static_cast<unsigned long>(xmlPathHash),
                          elementPathHash == xmlPathHash ? 1 : 0,
                          hasCmpSnap, xmlForCompare.size());
                }

                if (haveXmlPathHash && elementPathHash != 0 && xmlPathHash != 0 &&
                    elementPathHash != xmlPathHash) {
                    gui_tree::GUITreeBuildResult xmlBuilt;
#if defined(DYNAMIC_STATE_ABSTRACTION_ENABLED) && DYNAMIC_STATE_ABSTRACTION_ENABLED
                    xmlBuilt = (cmpSnapPtr && *cmpSnapPtr)
                        ? buildGuitreeFromTransitionSourcePreferSnapshot(
                            xmlForCompare, pkg, cls, *cmpSnapPtr)
                        : buildGuitreeFromCachedXmlPreferElement(xmlForCompare, pkg, cls);
#else
                    xmlBuilt = buildGuitreeFromCachedXmlPreferElement(xmlForCompare, pkg, cls);
#endif
                    bool haveXmlTreeForDiff =
                        xmlBuilt.tree && xmlBuilt.dom && safeRebuildTree(naming, *xmlBuilt.tree, xmlBuilt.dom);
                    const auto &elementXPaths = built.tree->getCurrentXPaths();
                    const std::vector<std::string> *xmlXPaths =
                        haveXmlTreeForDiff ? &xmlBuilt.tree->getCurrentXPaths() : nullptr;
                    const size_t elementXPathCount = elementXPaths.size();
                    const size_t xmlXPathCount = xmlXPaths ? xmlXPaths->size() : 0;
                    size_t diffIndex = 0;
                    bool foundDiff = false;
                    if (xmlXPaths) {
                        const size_t lim = std::min(elementXPathCount, xmlXPathCount);
                        for (; diffIndex < lim; ++diffIndex) {
                            if (elementXPaths[diffIndex] != (*xmlXPaths)[diffIndex]) {
                                foundDiff = true;
                                break;
                            }
                        }
                        if (!foundDiff && elementXPathCount != xmlXPathCount) {
                            foundDiff = true;
                        }
                    }
                    auto summarizeXPaths = [](const std::vector<std::string> &xpaths) -> std::string {
                        if (xpaths.empty()) {
                            return std::string("(empty)");
                        }
                        std::ostringstream oss;
                        const size_t lim = std::min<size_t>(xpaths.size(), 3);
                        for (size_t i = 0; i < lim; ++i) {
                            if (i != 0) {
                                oss << " | ";
                            }
                            oss << "#" << i << "=" << xpaths[i];
                        }
                        if (xpaths.size() > lim) {
                            oss << " | ... total=" << xpaths.size();
                        }
                        return oss.str();
                    };
                    const uintptr_t stateHashForLog =
                        (stateForDynamicApply && stateForDynamicApply->hash() != 0)
                            ? stateForDynamicApply->hash()
                            : 0;
                    const std::string &fp = naming->fingerprintString();
                    BDLOG("naming statekey: element/xml hash mismatch activity=%s stateHash=%lu "
                          "elementH=%lu xmlH=%lu hasSnap=%d xmlLen=%zu elementPtr=%p "
                          "statePtr=%p xmlSig=%" PRIu64 " %s namingFp=%s",
                          activity.c_str(), static_cast<unsigned long>(stateHashForLog),
                          static_cast<unsigned long>(elementPathHash),
                          static_cast<unsigned long>(xmlPathHash), hasCmpSnap,
                          xmlForCompare.size(), element.get(), stateForDynamicApply.get(),
                          hashStringForLog(xmlForCompare), summarizeElementForLog(element).c_str(),
                          fp.c_str());
                    BDLOG("naming statekey: tree stage=mismatch_element_tree activity=%s stateHash=%lu "
                          "treePtr=%p %s",
                          activity.c_str(), static_cast<unsigned long>(stateHashForLog), built.tree.get(),
                          summarizeGUITreeForLog(built.tree).c_str());
                    if (xmlXPaths) {
                        const std::string firstElementDiff =
                            (foundDiff && diffIndex < elementXPathCount) ? elementXPaths[diffIndex]
                                                                         : std::string("(none)");
                        const std::string firstXmlDiff =
                            (foundDiff && diffIndex < xmlXPathCount) ? (*xmlXPaths)[diffIndex]
                                                                     : std::string("(none)");
                        BDLOG("naming statekey: element/xml xpath diff activity=%s stateHash=%lu "
                              "elementCount=%zu xmlCount=%zu diffIndex=%zu elementXPath=%s xmlXPath=%s",
                              activity.c_str(), static_cast<unsigned long>(stateHashForLog),
                              elementXPathCount, xmlXPathCount, diffIndex,
                              firstElementDiff.c_str(), firstXmlDiff.c_str());
                        BDLOG("naming statekey: element/xml xpath summary activity=%s stateHash=%lu "
                              "element=%s xml=%s",
                              activity.c_str(), static_cast<unsigned long>(stateHashForLog),
                              summarizeXPaths(elementXPaths).c_str(),
                              summarizeXPaths(*xmlXPaths).c_str());
                    } else {
                        BDLOG("naming statekey: element/xml xpath diff unavailable activity=%s stateHash=%lu "
                              "xmlTreeReady=0",
                              activity.c_str(), static_cast<unsigned long>(stateHashForLog));
                    }
                }
            }
        }
        naming::StateKey kNew = naming::StateKey::fromGUITree(*built.tree);
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        if (stateForDynamicApply) {
            std::vector<gui_tree::GUITreeNode *> guiPreOrder;
            collectGUITreeNodesPreOrder(built.tree->getRootNode(), &guiPreOrder);
            applyApeDynamicActionHashesToReuseState(stateForDynamicApply, guiPreOrder, kNew);
        }
#endif
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
        if (wantApeRlIdentity && stateForDynamicApply && built.tree) {
            // Index snapshots by the identity hash (kNew.hash()) - NOT by the live
            // State::hash() at this point. stateForDynamicApply->hash() here is still the
            // pre-dynamic structural hash; the caller swaps it to apeKey.hash() via
            // State::applyDynamicAbstractionIdentityHash only AFTER this function returns.
            // Downstream consumers (recordApeTransitionForAbstraction, predicate eval,
            // refinement) always look up snapshots with src->hash() == identity hash,
            // so publishing under the pre-dynamic hash would orphan every future snapshot
            // in a bucket nobody reads - leaving apeLatestGuiTreeSnapshot stuck on the
            // one-shot fallback rebuild from the very first visit (stale coordinates),
            // which was observed to 100% miss widget bounds resolution for revisited
            // states (cf. snap_fallback_rebuilt=1 only for seq=12, then hasLiveSnap=1 with
            // bM=0 for all later visits of the same state).
            apeRememberGuiTreeSnapshot(kNew.hash(), *built.tree);
        }
#endif
        *outKey = std::move(kNew);
        if (outFailReason != nullptr) {
            *outFailReason = ApeStateKeyBuildFailReason::None;
        }
        _ape_correctness_counters.statekey_build_ok++;
        return true;
    }
#endif

    /** @brief Stores the mapping from a runtime state to its abstract key. */
    void Model::recordApeStateKey(const StatePtr &state, const naming::StateKey &key) {
        if (!state) {
            return;
        }
        const uintptr_t stateHash = state->hash();
        auto &bucket = _ape_state_keys_by_hash[stateHash];

        // In dynamic identity mode, State::hash() is overridden to StateKey::hash().
        // Only in this mode does it make sense to treat multiple different keys under the
        // same hash as a potential hash collision.
        const bool inApeHashSpace = (stateHash == key.hash());
        if (!inApeHashSpace) {
            bucket.clear();
            bucket.push_back(key);
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
            syncApeNamingGraphIndex(state);
#endif
            return;
        }

        for (const auto &existing : bucket) {
            if (existing == key) {
                return;
            }
        }
        if (!bucket.empty()) {
            _ape_correctness_counters.statekey_record_hash_collision++;
        }
        bucket.push_back(key);
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        syncApeNamingGraphIndex(state);
#endif
    }

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
    /** @brief Updates graph-side naming index entries when a state's abstraction changes. */
    void Model::syncApeNamingGraphIndex(const StatePtr &state) {
        if (!_graph || !state) {
            return;
        }
        auto ap = state->getActivityString();
        const std::string actKey =
            (ap && ap.get()) ? naming::StateKey::canonicalActivityString(*ap) : std::string();
        naming::StateKey k = naming::StateKey::fromParts("", nullptr, {});
        if (tryGetApeStateKey(state->hash(), &k, actKey)) {
            _graph->apeNamingIndexUpsert(state, k.namingFingerprint());
        } else {
            _graph->apeNamingIndexRemoveState(state);
        }
    }

    /** @brief Preloads naming-related graph indices to reduce lookup latency. */
    void Model::warmApeNamingGraphIndex() {
        if (!_graph) {
            return;
        }
        for (const StatePtr &s : _graph->getStates()) {
            if (!s) {
                continue;
            }
            auto ap = s->getActivityString();
            const std::string actKey =
                (ap && ap.get()) ? naming::StateKey::canonicalActivityString(*ap) : std::string();
            naming::StateKey k = naming::StateKey::fromParts("", nullptr, {});
            if (tryGetApeStateKey(s->hash(), &k, actKey)) {
                _graph->apeNamingIndexUpsert(s, k.namingFingerprint());
            }
        }
    }
#endif

    /** @brief Looks up the abstract state key for a state hash, optionally scoped by activity hint. */
    bool Model::tryGetApeStateKey(uintptr_t stateHash, naming::StateKey *out, const std::string &hintActivity,
                                  uintptr_t hintKeyHash) const {
        auto it = _ape_state_keys_by_hash.find(stateHash);
        if (it == _ape_state_keys_by_hash.end() || it->second.empty()) {
            return false;
        }
        const auto &bucket = it->second;
        const bool hasActivityHint = !hintActivity.empty();
        const bool hasKeyHint = (hintKeyHash != 0);
        auto matches = [&](const naming::StateKey &k) -> bool {
            if (hasActivityHint && k.activity() != hintActivity) {
                return false;
            }
            if (hasKeyHint && k.hash() != hintKeyHash) {
                return false;
            }
            return true;
        };
        const naming::StateKey *picked = nullptr;
        for (const auto &k : bucket) {
            if (!matches(k)) {
                continue;
            }
            if (picked != nullptr && *picked != k) {
                // Bucket remains ambiguous even with hints.
                return false;
            }
            picked = &k;
        }
        if (!picked) {
            return false;
        }
        if (out != nullptr) {
            *out = *picked;
        }
        return true;
    }

    /** @brief Looks up the abstract state key hash for a state hash. */
    bool Model::tryGetApeStateKeyHash(uintptr_t stateHash, uintptr_t *outKeyHash, const std::string &hintActivity,
                                      uintptr_t hintKeyHash) const {
        naming::StateKey k = naming::StateKey::fromParts("", nullptr, {});
        if (!tryGetApeStateKey(stateHash, &k, hintActivity, hintKeyHash)) {
            return false;
        }
        if (outKeyHash != nullptr) {
            *outKeyHash = k.hash();
        }
        return true;
    }

    /**
     * @brief Destructor for Model class
     * 
     * Clears the device-agent map to release all agent resources.
     * The graph and preference are shared pointers and will be automatically
     * cleaned up when the last reference is released.
     */
    Model::~Model() {
        this->_deviceIDAgentMap.clear();
    }

}  // namespace fastbotx

#endif  // Model_CPP_