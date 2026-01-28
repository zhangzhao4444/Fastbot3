/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef FASTBOTX_GUI_TREE_TRANSITION_CPP_
#define FASTBOTX_GUI_TREE_TRANSITION_CPP_

#include "GUITreeTransition.h"
#include "Graph.h"

namespace fastbotx {

    GUITreeTransition::GUITreeTransition(ElementPtr sourceTree,
                                         ActivityStateActionPtr action,
                                         ElementPtr targetTree)
            : _sourceTree(std::move(sourceTree)),
              _targetTree(std::move(targetTree)),
              _action(std::move(action)) {
    }

    int GUITreeTransition::getTimestamp() const {
        // APE alignment: getTimestamp from source tree snapshot
        if (_sourceTree) {
            return _sourceTree->getTimestamp();
        }
        return 0;
    }

} // namespace fastbotx

#endif // FASTBOTX_GUI_TREE_TRANSITION_CPP_
