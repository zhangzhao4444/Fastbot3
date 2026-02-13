/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef Operate_H_
#define Operate_H_

#include <string>
#include "../Base.h"

namespace fastbotx {

    /**
     * @brief Wrapper class for device operations
     * 
     * Converts model-generated operations into a format that the device
     * can understand and execute. Contains all information needed to
     * perform an action on the device (action type, position, text, etc.).
     * 
     * This class serves as the bridge between the RL model's action
     * selection and the actual device operation execution.
     */
    class DeviceOperateWrapper {
    public:
        /// Action type to perform
        ActionType act;
        
        /// Position/coordinates for the action (bounds rectangle)
        Rect pos;
        
        /// State ID (identifier of the state this operation belongs to)
        std::string sid;
        
        /// Action ID (identifier of the action being executed)
        std::string aid;
        
        /// Throttle/delay time in milliseconds before executing action
        float throttle;
        
        /// Wait time in milliseconds after executing action
        int waitTime;
        
        /// Whether the target element is editable (for text input)
        bool editable{};
        
        /// Whether fuzzing is allowed for this operation
        bool allowFuzzing{true};
        
        /// Whether to clear text before input
        bool clear{};
        
        /// Name/description of the operation
        std::string name;
        
        /// Widget information in JSON format
        std::string widget;

        DeviceOperateWrapper();

        DeviceOperateWrapper(const DeviceOperateWrapper &opt);

        explicit DeviceOperateWrapper(const std::string &optJsonStr);

        DeviceOperateWrapper &operator=(const DeviceOperateWrapper &node);

        std::string setText(const std::string &text);

        const std::string &getText() const { return this->_text; }

        bool getRawInput() const { return this->rawInput; }

        const std::string &getJAction() const { return this->jAction; }

        std::string toString() const;

        virtual ~DeviceOperateWrapper() = default;

        static std::shared_ptr<DeviceOperateWrapper> OperateNop;
    protected:
        bool rawInput{};
        std::string _text;
        std::string extra0;
        std::string jAction;
    };

    typedef std::shared_ptr<DeviceOperateWrapper> OperatePtr;

}

#endif
