/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */

package com.android.commands.monkey.tree;

import android.graphics.Rect;
import android.util.Xml;
import android.view.accessibility.AccessibilityNodeInfo;

import com.android.commands.monkey.utils.Logger;

import org.w3c.dom.Element;
import org.w3c.dom.Node;
import org.w3c.dom.NodeList;
import org.xmlpull.v1.XmlSerializer;

import java.io.IOException;
import java.io.StringWriter;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;

/**
 * @author Jianqiang Guo, Zhao Zhang
 */

/**
 * guitree builder utils
 * AccessibilityNodeInfo object -> xml string
 */
public class TreeBuilder {

    /** PERFORMANCE_OPTIMIZATION_ITEMS §8.8: delegate to 2-arg with ThreadLocal; returns copy for API safety. */
    static Rect getVisibleBoundsInScreen(AccessibilityNodeInfo node) {
        if (node == null) return null;
        Rect reuse = sXmlDumpRect.get();
        getVisibleBoundsInScreen(node, reuse);
        return new Rect(reuse);
    }

    /** Fills reuseRect with node bounds; use in recursion to avoid per-node allocation (PERFORMANCE_OPTIMIZATION_ITEMS §2.2). */
    private static void getVisibleBoundsInScreen(AccessibilityNodeInfo node, Rect reuseRect) {
        if (node != null) {
            node.getBoundsInScreen(reuseRect);
        }
    }

    private static final ThreadLocal<Rect> sBinaryDumpRect = new ThreadLocal<Rect>() {
        @Override
        protected Rect initialValue() {
            return new Rect();
        }
    };
    private static final ThreadLocal<Rect> sXmlDumpRect = new ThreadLocal<Rect>() {
        @Override
        protected Rect initialValue() {
            return new Rect();
        }
    };  

    /** Returns EMPTY_BYTES for empty string to avoid per-node small allocation (PERFORMANCE_OPTIMIZATION_ITEMS §3.5). */
    private static byte[] toUtf8Bytes(String s) {
        return (s == null || s.isEmpty()) ? EMPTY_BYTES : s.getBytes(StandardCharsets.UTF_8);
    }

    // copy from AccessibilityNodeInfoDumper
    private static String safeCharSeqToString(CharSequence cs) {
        if (cs == null)
            return "";
        return stripInvalidXMLChars(cs);
    }

    private static boolean isInvalidXmlChar(char ch) {
        return (ch >= 0x1 && ch <= 0x8) || (ch >= 0xB && ch <= 0xC) || (ch >= 0xE && ch <= 0x1F)
                || (ch >= 0x7F && ch <= 0x84) || (ch >= 0x86 && ch <= 0x9f)
                || (ch >= 0xFDD0 && ch <= 0xFDDF) || (ch >= 0x1FFFE && ch <= 0x1FFFF)
                || (ch >= 0x2FFFE && ch <= 0x2FFFF) || (ch >= 0x3FFFE && ch <= 0x3FFFF)
                || (ch >= 0x4FFFE && ch <= 0x4FFFF) || (ch >= 0x5FFFE && ch <= 0x5FFFF)
                || (ch >= 0x6FFFE && ch <= 0x6FFFF) || (ch >= 0x7FFFE && ch <= 0x7FFFF)
                || (ch >= 0x8FFFE && ch <= 0x8FFFF) || (ch >= 0x9FFFE && ch <= 0x9FFFF)
                || (ch >= 0xAFFFE && ch <= 0xAFFFF) || (ch >= 0xBFFFE && ch <= 0xBFFFF)
                || (ch >= 0xCFFFE && ch <= 0xCFFFF) || (ch >= 0xDFFFE && ch <= 0xDFFFF)
                || (ch >= 0xEFFFE && ch <= 0xEFFFF) || (ch >= 0xFFFFE && ch <= 0xFFFFF)
                || (ch >= 0x10FFFE && ch <= 0x10FFFF);
    }

    // PERFORMANCE_OPTIMIZATION_ITEMS §2.3: fast path when no invalid chars (cs.toString()); else StringBuilder(capacity).
    private static String stripInvalidXMLChars(CharSequence cs) {
        int len = cs.length();
        for (int i = 0; i < len; i++) {
            if (isInvalidXmlChar(cs.charAt(i))) {
                StringBuilder ret = new StringBuilder(len);
                try {
                    for (int j = 0; j < len; j++) {
                        char ch = cs.charAt(j);
                        ret.append(isInvalidXmlChar(ch) ? '.' : ch);
                    }
                } catch (Exception ex) {
                    Logger.println("strip invalid xml failed, replace invalid chars: " + ex.toString());
                    return cs.toString().replaceAll("\"", "").replaceAll("&#", "");
                }
                return ret.toString();
            }
        }
        return cs.toString();
    }

    // Performance: short attribute names (SECURITY_AND_OPTIMIZATION §7) - C++ maps rid/cd/pkg etc.
    private static final String ATTR_INDEX = "idx";
    private static final String ATTR_TEXT = "t";
    private static final String ATTR_RESOURCE_ID = "rid";
    private static final String ATTR_CLASS = "class";
    private static final String ATTR_PACKAGE = "pkg";
    private static final String ATTR_CONTENT_DESC = "cd";
    private static final String ATTR_BOUNDS = "bnd";
    // bools kept short: checkable→ck, checked→cked, clickable→clk, enabled→en, focusable→foc, focused→fcd, scrollable→scl, long-clickable→lclk, password→pwd, selected→sel
    private static final String ATTR_CHECKABLE = "ck";
    private static final String ATTR_CHECKED = "cked";
    private static final String ATTR_CLICKABLE = "clk";
    private static final String ATTR_ENABLED = "en";
    private static final String ATTR_FOCUSABLE = "foc";
    private static final String ATTR_FOCUSED = "fcd";
    private static final String ATTR_SCROLLABLE = "scl";
    private static final String ATTR_LONG_CLICKABLE = "lclk";
    private static final String ATTR_PASSWORD = "pwd";
    private static final String ATTR_SELECTED = "sel";

    /** Max depth for XML/binary dump to avoid stack overflow and huge output. */
    private static final int MAX_TREE_DEPTH = 25;

    /** Attribute names to strip in filterTree (static to avoid array alloc per call). */
    private static final String[] ATTRIBUTES_TO_REMOVE = {"drawing-order", "hint", "display-id"};

    /** Avoid per-node Boolean.toString() allocation in XML dump. */
    private static final String VAL_TRUE = "true";
    private static final String VAL_FALSE = "false";

    /** Cache for small indices to avoid Integer.toString() per node in hot path. */
    private static final int INDEX_CACHE_SIZE = 256;
    private static final String[] INDEX_STRINGS = new String[INDEX_CACHE_SIZE];
    static {
        for (int i = 0; i < INDEX_CACHE_SIZE; i++) {
            INDEX_STRINGS[i] = String.valueOf(i);
        }
    }

    private static String indexToString(int index) {
        return (index >= 0 && index < INDEX_CACHE_SIZE) ? INDEX_STRINGS[index] : String.valueOf(index);
    }

    /** Reusable buffer for bounds string to avoid Rect.toShortString() intermediate allocations. */
    private static final ThreadLocal<StringBuilder> sBoundsBuffer = new ThreadLocal<StringBuilder>() {
        @Override
        protected StringBuilder initialValue() {
            return new StringBuilder(32);
        }
    };

    private static String boundsToShortString(Rect r) {
        StringBuilder sb = sBoundsBuffer.get();
        sb.setLength(0);
        sb.append('[').append(r.left).append(',').append(r.top).append("][")
                .append(r.right).append(',').append(r.bottom).append(']');
        return sb.toString();
    }

    // copy from AccessibilityNodeInfoDumper
    private static void dumpNodeRec(AccessibilityNodeInfo node, XmlSerializer serializer, int index,
                                    int depth)  throws IOException {
        serializer.startTag("", "node");
        serializer.attribute("", ATTR_INDEX, indexToString(index));
        serializer.attribute("", ATTR_TEXT, safeCharSeqToString(node.getText()));
        serializer.attribute("", ATTR_RESOURCE_ID, safeCharSeqToString(node.getViewIdResourceName()));
        serializer.attribute("", ATTR_CLASS, safeCharSeqToString(node.getClassName()));
        serializer.attribute("", ATTR_PACKAGE, safeCharSeqToString(node.getPackageName()));
        serializer.attribute("", ATTR_CONTENT_DESC, safeCharSeqToString(node.getContentDescription()));
        serializer.attribute("", ATTR_CHECKABLE, node.isCheckable() ? VAL_TRUE : VAL_FALSE);
        serializer.attribute("", ATTR_CHECKED, node.isChecked() ? VAL_TRUE : VAL_FALSE);
        serializer.attribute("", ATTR_CLICKABLE, node.isClickable() ? VAL_TRUE : VAL_FALSE);
        serializer.attribute("", ATTR_ENABLED, node.isEnabled() ? VAL_TRUE : VAL_FALSE);
        serializer.attribute("", ATTR_FOCUSABLE, node.isFocusable() ? VAL_TRUE : VAL_FALSE);
        serializer.attribute("", ATTR_FOCUSED, node.isFocused() ? VAL_TRUE : VAL_FALSE);
        serializer.attribute("", ATTR_SCROLLABLE, node.isScrollable() ? VAL_TRUE : VAL_FALSE);
        serializer.attribute("", ATTR_LONG_CLICKABLE, node.isLongClickable() ? VAL_TRUE : VAL_FALSE);
        serializer.attribute("", ATTR_PASSWORD, node.isPassword() ? VAL_TRUE : VAL_FALSE);
        serializer.attribute("", ATTR_SELECTED, node.isSelected() ? VAL_TRUE : VAL_FALSE);
        Rect xmlRect = sXmlDumpRect.get();
        getVisibleBoundsInScreen(node, xmlRect);
        serializer.attribute("", ATTR_BOUNDS, boundsToShortString(xmlRect));

        depth += 1;
        if (depth <= MAX_TREE_DEPTH) {
            int count = node.getChildCount();
            for (int i = 0; i < count; i++) {
                AccessibilityNodeInfo child = node.getChild(i);
                if (child != null && child.isVisibleToUser()) {
                    dumpNodeRec(child, serializer, i, depth);
                    child.recycle();
                }
            }
        }
        serializer.endTag("", "node");
    }

    private static String getAttr(org.w3c.dom.Element node, String shortName, String longName) {
        if (node.hasAttribute(shortName)) return node.getAttribute(shortName);
        return node.hasAttribute(longName) ? node.getAttribute(longName) : "";
    }

    private static String getIndexAttr(Element node, int fallbackIndex) {
        if (node.hasAttribute(ATTR_INDEX)) return node.getAttribute(ATTR_INDEX);
        if (node.hasAttribute("index")) return node.getAttribute("index");
        return String.valueOf(fallbackIndex);
    }

    private static void dumpNodeRec(Element node, XmlSerializer serializer, int index, int depth) throws IOException {
        serializer.startTag("", "node");
        serializer.attribute("", ATTR_INDEX, getIndexAttr(node, index));
        serializer.attribute("", ATTR_TEXT, getAttr(node, ATTR_TEXT, "text"));
        serializer.attribute("", ATTR_RESOURCE_ID, getAttr(node, ATTR_RESOURCE_ID, "resource-id"));
        serializer.attribute("", ATTR_CLASS, getAttr(node, ATTR_CLASS, "class"));
        serializer.attribute("", ATTR_PACKAGE, getAttr(node, ATTR_PACKAGE, "package"));
        serializer.attribute("", ATTR_CONTENT_DESC, getAttr(node, ATTR_CONTENT_DESC, "content-desc"));
        serializer.attribute("", ATTR_CHECKABLE, getAttr(node, ATTR_CHECKABLE, "checkable"));
        serializer.attribute("", ATTR_CHECKED, getAttr(node, ATTR_CHECKED, "checked"));
        serializer.attribute("", ATTR_CLICKABLE, getAttr(node, ATTR_CLICKABLE, "clickable"));
        serializer.attribute("", ATTR_ENABLED, getAttr(node, ATTR_ENABLED, "enabled"));
        serializer.attribute("", ATTR_FOCUSABLE, getAttr(node, ATTR_FOCUSABLE, "focusable"));
        serializer.attribute("", ATTR_FOCUSED, getAttr(node, ATTR_FOCUSED, "focused"));
        serializer.attribute("", ATTR_SCROLLABLE, getAttr(node, ATTR_SCROLLABLE, "scrollable"));
        serializer.attribute("", ATTR_LONG_CLICKABLE, getAttr(node, ATTR_LONG_CLICKABLE, "long-clickable"));
        serializer.attribute("", ATTR_PASSWORD, getAttr(node, ATTR_PASSWORD, "password"));
        serializer.attribute("", ATTR_SELECTED, getAttr(node, ATTR_SELECTED, "selected"));
        serializer.attribute("", ATTR_BOUNDS, getAttr(node, ATTR_BOUNDS, "bounds"));


        depth += 1;

        if (depth <= MAX_TREE_DEPTH) {
            NodeList childNodes = node.getChildNodes();
            int childElementIndex = 0;
            for (int i = 0; i < childNodes.getLength(); i++) {
                Node child = childNodes.item(i);
                if (child.getNodeType() == Node.ELEMENT_NODE) {
                    dumpNodeRec((Element) child, serializer, childElementIndex, depth);
                    childElementIndex++;
                }
            }
        }
        serializer.endTag("", "node");
    }

    // Binary format for C++ createFromBinary (SECURITY_AND_OPTIMIZATION §7 opt1). Little-endian.
    private static final byte[] BINARY_MAGIC = {'F', 'B', 0, 1};
    private static final int TAG_TEXT = 0, TAG_RID = 1, TAG_CLASS = 2, TAG_PKG = 3, TAG_CD = 4;
    private static final int MIN_NODE_BYTES = 4 + 16 + 2 + 2 + 1 + 2; // magic + bounds + index + flags + numStrings + numChildren
    private static final byte[] EMPTY_BYTES = new byte[0];  // PERFORMANCE_OPTIMIZATION_ITEMS §3.5: avoid "".getBytes() per node

    /**
     * Write tree to ByteBuffer in compact binary format (little-endian). C++ Element::createFromBinary reads this.
     * Single-pass over children: writes numChildren placeholder, dumps children while counting, then back-patches.
     *
     * @param rootInfo root node
     * @param buffer direct ByteBuffer, position advanced by bytes written
     * @return bytes written, or -1 if buffer too small
     */
    public static int dumpToBinary(AccessibilityNodeInfo rootInfo, ByteBuffer buffer) {
        if (rootInfo == null || buffer == null || !buffer.isDirect()) return -1;
        buffer = buffer.order(ByteOrder.LITTLE_ENDIAN);
        if (buffer.remaining() < BINARY_MAGIC.length) return -1;
        buffer.put(BINARY_MAGIC);
        if (dumpNodeRecBinary(rootInfo, buffer, 0, 1) < 0) return -1;
        return buffer.position();
    }

    private static int dumpNodeRecBinary(AccessibilityNodeInfo node, ByteBuffer buf, int index, int depth) {
        if (depth > MAX_TREE_DEPTH || buf.remaining() < MIN_NODE_BYTES) return -1;
        Rect r = sBinaryDumpRect.get();
        getVisibleBoundsInScreen(node, r);
        int left = (node != null) ? r.left : 0, top = (node != null) ? r.top : 0, right = (node != null) ? r.right : 0, bottom = (node != null) ? r.bottom : 0;
        buf.putInt(left).putInt(top).putInt(right).putInt(bottom);
        buf.putShort((short) index);
        int flags = 0;
        if (node.isCheckable()) flags |= 1;
        if (node.isChecked()) flags |= 2;
        if (node.isClickable()) flags |= 4;
        if (node.isEnabled()) flags |= 8;
        if (node.isFocusable()) flags |= 16;
        if (node.isFocused()) flags |= 32;
        if (node.isScrollable()) flags |= 64;
        if (node.isLongClickable()) flags |= 128;
        if (node.isPassword()) flags |= 256;
        if (node.isSelected()) flags |= 512;
        buf.putShort((short) flags);
        byte[] text = toUtf8Bytes(safeCharSeqToString(node.getText()));
        byte[] rid = toUtf8Bytes(safeCharSeqToString(node.getViewIdResourceName()));
        byte[] clazz = toUtf8Bytes(safeCharSeqToString(node.getClassName()));
        byte[] pkg = toUtf8Bytes(safeCharSeqToString(node.getPackageName()));
        byte[] cd = toUtf8Bytes(safeCharSeqToString(node.getContentDescription()));
        int numStrings = (text.length > 0 ? 1 : 0) + (rid.length > 0 ? 1 : 0) + (clazz.length > 0 ? 1 : 0) + (pkg.length > 0 ? 1 : 0) + (cd.length > 0 ? 1 : 0);
        int totalStringBytes = (text.length > 0 ? 3 + text.length : 0) + (rid.length > 0 ? 3 + rid.length : 0)
                + (clazz.length > 0 ? 3 + clazz.length : 0) + (pkg.length > 0 ? 3 + pkg.length : 0) + (cd.length > 0 ? 3 + cd.length : 0);
        if (buf.remaining() < totalStringBytes) return -1;  // require all strings to fit; avoid inconsistent binary
        buf.put((byte) numStrings);
        if (text.length > 0) { buf.put((byte) TAG_TEXT); buf.putShort((short) text.length); buf.put(text); }
        if (rid.length > 0) { buf.put((byte) TAG_RID); buf.putShort((short) rid.length); buf.put(rid); }
        if (clazz.length > 0) { buf.put((byte) TAG_CLASS); buf.putShort((short) clazz.length); buf.put(clazz); }
        if (pkg.length > 0) { buf.put((byte) TAG_PKG); buf.putShort((short) pkg.length); buf.put(pkg); }
        if (cd.length > 0) { buf.put((byte) TAG_CD); buf.putShort((short) cd.length); buf.put(cd); }
        if (buf.remaining() < 2) return -1;  // need 2 bytes for numChildren (placeholder; back-patch later)
        int posNumChildren = buf.position();
        buf.putShort((short) 0);  // placeholder; single-pass: count while dumping, then back-patch
        int childCount = node.getChildCount();
        int written = 0;
        for (int i = 0; i < childCount; i++) {
            AccessibilityNodeInfo child = node.getChild(i);
            if (child != null) {
                try {
                    if (child.isVisibleToUser()) {
                        if (dumpNodeRecBinary(child, buf, written, depth + 1) < 0) return -1;
                        written++;
                    }
                } finally {
                    child.recycle();
                }
            }
        }
        int savedPos = buf.position();
        buf.position(posNumChildren);
        buf.putShort((short) written);
        buf.position(savedPos);
        return savedPos;
    }

    /**
     * Using {@link AccessibilityNodeInfo} this method will walk the layout hierarchy
     * and generates an xml dump to the location specified by <code>dumpFile</code>
     */
    private static final int XML_WRITER_INITIAL_CAPACITY = 256 * 1024;  // PERFORMANCE_OPTIMIZATION_ITEMS §2.3: reduce resize

    public static String dumpDocumentStrWithOutTree(AccessibilityNodeInfo rootInfo) {
        String result = "";
        try {
            StringWriter textWriter = new StringWriter(XML_WRITER_INITIAL_CAPACITY);
            XmlSerializer serializer = Xml.newSerializer();
            serializer.setOutput(textWriter);
            serializer.startDocument("UTF-8", true);
            // Performance: disable indent to reduce XML size over JNI (PERFORMANCE_ANALYSIS §3.5)
            serializer.setFeature("http://xmlpull.org/v1/doc/features.html#indent-output", false);

            int depth = 1;
            dumpNodeRec(rootInfo, serializer, 0, depth);
            serializer.endDocument();
            result = textWriter.toString();
        } catch (IllegalArgumentException | IOException | IllegalStateException e) {
            Logger.println("failed to dump window to file: " + e.toString());
        }
        return result;
    }

    /**
     * Filter the attributes in U2 XML tree
     * @param root
     */
    public static void filterTree(Element root) {
        if (root == null) {
            return;
        }

        // filter the necessary attrs
        for (String attr : ATTRIBUTES_TO_REMOVE) {
            if (root.hasAttribute(attr)) {
                root.removeAttribute(attr);
            }
        }

        NodeList childNodes = root.getChildNodes();
        for (int i = 0; i < childNodes.getLength(); i++) {
            Node node = childNodes.item(i);
            if (node.getNodeType() == Node.ELEMENT_NODE) {
                Element child = (Element) node;
                filterTree(child);
            }
        }
    }

    /**
     * Using {@link AccessibilityNodeInfo} this method will walk the layout hierarchy
     * and generates an xml dump to the location specified by <code>dumpFile</code>
     */
    public static String dumpDocumentStrWithOutTree(Element rootInfo) {
        String result = "";
        try {
            StringWriter textWriter = new StringWriter(XML_WRITER_INITIAL_CAPACITY);
            XmlSerializer serializer = Xml.newSerializer();
            serializer.setOutput(textWriter);
            serializer.startDocument("UTF-8", true);
            // Performance: disable indent to reduce XML size (PERFORMANCE_ANALYSIS §3.5)
            serializer.setFeature("http://xmlpull.org/v1/doc/features.html#indent-output", false);

            int depth = 1;
            dumpNodeRec(rootInfo, serializer, 0, depth);
            serializer.endDocument();
            result = textWriter.toString();
        } catch (IllegalArgumentException | IOException | IllegalStateException e) {
            Logger.println("failed to dump window to file: " + e.toString());
        }
        return result;
    }
}
