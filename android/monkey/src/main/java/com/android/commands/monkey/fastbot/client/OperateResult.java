/**
 * @author Zhao Zhang
 */

package com.android.commands.monkey.fastbot.client;

/**
 * Native-friendly result for getAction. JNI fills these fields; Java builds Operate from it.
 */
public class OperateResult {
    public int actOrdinal;
    public short[] pos;
    public int throttle;
    public long waitTime;
    public String text;
    public boolean clear;
    public boolean rawInput;
    public boolean allowFuzzing;
    public boolean editable;
    public String sid;
    public String aid;
    public String jAction;
    public String widget;
}
