/**
 * @author Zhao Zhang
 */

package com.android.commands.monkey.fastbot.client;

import android.graphics.Point;
import android.graphics.Rect;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * @author Jianqiang Guo
 */

/**
 * Communication interface.
 * Performance (§8.7): pos kept as short[] until needed; setRectFromPos avoids List allocation in hot path.
 */
public class Operate extends GsonIface {
    public ActionType act;
    /** Set by fromJson; or lazily from posArray when getPos() called. Raw from native in fromOperateResult. */
    private List<Short> pos;
    /** Raw coords from native; avoids toShortList() in hot path (fromOperateResult). */
    private short[] posArray;
    // Text information to be inputed
    public String text;
    // Do you need to clear the original text before input text?
    public boolean clear;
    // Whether to use the original adb shell to perform input,
    // and raw input(adbkeyborad) choose one, raw input speed is faster,
    // adb compatibility is better, in some scenarios such as security keyboard may only use adb
    public boolean adbinput;
    public boolean rawinput;
    public boolean allowFuzzing;
    public boolean editable;
    public String sid;
    public String aid;
    // Event duration, such as long press 5 seconds, wait time is 5000
    public long waitTime;
    // Event interval time
    public int throttle;
    public String target;
    public String jAction;
    public String widget;
    /** When true, retry getActionFromBuffer with screenshot in the same frame (first-step image for AutodevAgent). */
    public boolean requestScreenshotRetry;

    public static Operate fromJson(String jsonStr) {
        return gson.fromJson(jsonStr, Operate.class);
    }

    /** Build Operate from native structured result (avoids full JSON parse, SECURITY_AND_OPTIMIZATION §7 opt4). §8.7: keep posArray, no toShortList. */
    public static Operate fromOperateResult(OperateResult r) {
        if (r == null) return null;
        Operate o = new Operate();
        ActionType[] values = ActionType.values();
        o.act = (r.actOrdinal >= 0 && r.actOrdinal < values.length) ? values[r.actOrdinal] : ActionType.NOP;
        o.posArray = r.pos;
        o.throttle = r.throttle;
        o.waitTime = r.waitTime;
        o.text = r.text != null ? r.text : "";
        o.clear = r.clear;
        o.adbinput = r.adbInput;
        o.rawinput = r.rawInput;
        o.allowFuzzing = r.allowFuzzing;
        o.editable = r.editable;
        o.sid = r.sid != null ? r.sid : "";
        o.aid = r.aid != null ? r.aid : "";
        o.jAction = r.jAction != null ? r.jAction : "";
        o.widget = r.widget != null ? r.widget : "";
        o.requestScreenshotRetry = r.requestScreenshotRetry;
        return o;
    }

    private static List<Short> toShortList(short[] arr) {
        List<Short> list = new ArrayList<>();
        if (arr != null) for (short s : arr) list.add(s);
        return list;
    }

    /** §8.7: Fill rect from posArray (or pos) without allocating List in hot path. Returns false if &lt; 4 coords. */
    public boolean setRectFromPos(Rect out) {
        if (posArray != null && posArray.length >= 4) {
            out.set(posArray[0], posArray[1], posArray[2], posArray[3]);
            return true;
        }
        List<Short> p = getPos();
        if (p.size() >= 4) {
            out.set(p.get(0), p.get(1), p.get(2), p.get(3));
            return true;
        }
        return false;
    }

    /** Lazy view of pos; for serialization and rare getPoints() callers. */
    public List<Short> getPos() {
        if (pos != null) return pos;
        if (posArray != null) { pos = toShortList(posArray); return pos; }
        return Collections.<Short>emptyList();
    }

    public List<Point> getPoints() {
        List<Point> points = new ArrayList<>();
        if (posArray != null) {
            for (int i = 0; i + 1 < posArray.length; i += 2)
                points.add(new Point(posArray[i], posArray[i + 1]));
        } else {
            List<Short> p = getPos();
            for (int i = 0; i + 1 < p.size(); i += 2)
                points.add(new Point(p.get(i), p.get(i + 1)));
        }
        return points;
    }

    public String toJson() {
        return gson.toJson(this);
    }

    @Override
    public String toString() {
        return toJson();
    }
}
