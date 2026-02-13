package com.android.commands.monkey.source;

import static com.android.commands.monkey.fastbot.client.ActionType.SCROLL_BOTTOM_UP;
import static com.android.commands.monkey.fastbot.client.ActionType.SCROLL_TOP_DOWN;
import static com.android.commands.monkey.framework.AndroidDevice.stopPackage;
import static com.android.commands.monkey.utils.Config.bytestStatusBarHeight;
import static com.android.commands.monkey.utils.Config.defaultGUIThrottle;
import static com.android.commands.monkey.utils.Config.doHistoryRestart;
import static com.android.commands.monkey.utils.Config.doHoming;
import static com.android.commands.monkey.utils.Config.execPreShell;
import static com.android.commands.monkey.utils.Config.execPreShellEveryStartup;
import static com.android.commands.monkey.utils.Config.execSchema;
import static com.android.commands.monkey.utils.Config.execSchemaEveryStartup;
import static com.android.commands.monkey.utils.Config.historyRestartRate;
import static com.android.commands.monkey.utils.Config.homeAfterNSecondsofsleep;
import static com.android.commands.monkey.utils.Config.homingRate;
import static com.android.commands.monkey.utils.Config.refetchInfoCount;
import static com.android.commands.monkey.utils.Config.refetchInfoWaitingInterval;
import static com.android.commands.monkey.utils.Config.saveGUITreeToXmlEveryStep;
import static com.android.commands.monkey.utils.Config.schemaTraversalMode;
import static com.android.commands.monkey.utils.Config.scrollAfterNSecondsofsleep;
import static com.android.commands.monkey.utils.Config.startAfterDoScrollAction;
import static com.android.commands.monkey.utils.Config.startAfterDoScrollActionTimes;
import static com.android.commands.monkey.utils.Config.startAfterDoScrollBottomAction;
import static com.android.commands.monkey.utils.Config.startAfterDoScrollBottomActionTimes;
import static com.android.commands.monkey.utils.Config.startAfterNSecondsofsleep;
import static com.android.commands.monkey.utils.Config.swipeDuration;
import static com.android.commands.monkey.utils.Config.throttleForExecPreSchema;
import static com.android.commands.monkey.utils.Config.throttleForExecPreShell;
import static com.android.commands.monkey.utils.Config.useRandomClick;

import android.app.IActivityManager;
import android.app.UiAutomation;
import android.content.ComponentName;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.graphics.PointF;
import android.graphics.Rect;
import android.hardware.display.DisplayManagerGlobal;
import android.os.Build;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.util.DisplayMetrics;
import android.view.Display;
import android.view.IWindowManager;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;

import com.android.commands.monkey.Monkey;
import com.android.commands.monkey.action.Action;
import com.android.commands.monkey.action.FuzzAction;
import com.android.commands.monkey.action.ModelAction;
import com.android.commands.monkey.events.CustomEvent;
import com.android.commands.monkey.events.CustomEventFuzzer;
import com.android.commands.monkey.events.MonkeyEvent;
import com.android.commands.monkey.events.MonkeyEventQueue;
import com.android.commands.monkey.events.MonkeyEventSource;
import com.android.commands.monkey.events.base.MonkeyActivityEvent;
import com.android.commands.monkey.events.base.MonkeyCommandEvent;
import com.android.commands.monkey.events.base.MonkeyDataActivityEvent;
import com.android.commands.monkey.events.base.MonkeyIMEEvent;
import com.android.commands.monkey.events.base.MonkeyKeyEvent;
import com.android.commands.monkey.events.base.MonkeyRotationEvent;
import com.android.commands.monkey.events.base.MonkeySchemaEvent;
import com.android.commands.monkey.events.base.MonkeyThrottleEvent;
import com.android.commands.monkey.events.base.MonkeyTouchEvent;
import com.android.commands.monkey.events.base.mutation.MutationAirplaneEvent;
import com.android.commands.monkey.events.base.mutation.MutationAlwaysFinishActivityEvent;
import com.android.commands.monkey.events.base.mutation.MutationWifiEvent;
import com.android.commands.monkey.events.customize.ClickEvent;
import com.android.commands.monkey.events.customize.ShellEvent;
import com.android.commands.monkey.fastbot.client.ActionType;
import com.android.commands.monkey.fastbot.client.Operate;
import com.android.commands.monkey.framework.AndroidDevice;
import com.android.commands.monkey.provider.SchemaProvider;
import com.android.commands.monkey.provider.ShellProvider;
import com.android.commands.monkey.tree.TreeBuilder;
import com.android.commands.monkey.utils.Config;
import com.android.commands.monkey.utils.JsonRPCResponse;
import com.android.commands.monkey.utils.Logger;
import com.android.commands.monkey.utils.MonkeySemaphore;
import com.android.commands.monkey.utils.MonkeyUtils;
import com.android.commands.monkey.utils.OkHttpClient;
import com.android.commands.monkey.utils.ProxyServer;
import com.android.commands.monkey.utils.RandomHelper;
import com.android.commands.monkey.utils.U2Client;
import com.android.commands.monkey.utils.UUIDHelper;
import com.bytedance.fastbot.AiClient;
import com.google.gson.Gson;

import org.w3c.dom.Document;
import org.w3c.dom.Element;
import org.w3c.dom.NamedNodeMap;
import org.w3c.dom.Node;
import org.w3c.dom.NodeList;
import org.xml.sax.InputSource;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.io.StringReader;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Random;
import java.util.Stack;

import javax.xml.parsers.DocumentBuilder;
import javax.xml.parsers.DocumentBuilderFactory;
import javax.xml.parsers.ParserConfigurationException;
import javax.xml.xpath.XPath;
import javax.xml.xpath.XPathConstants;
import javax.xml.xpath.XPathExpressionException;
import javax.xml.xpath.XPathFactory;

import fi.iki.elonen.NanoHTTPD;
import okhttp3.Response;

public class MonkeySourceApeU2 extends MonkeySourceApeBase implements MonkeyEventSource {

    /**
     * UiAutomation client and connection
     */
    protected UiAutomation mUiAutomation;
    public Monkey monkey = null;

    /** total number of events generated so far */
    private long mEventCount = 0;
    /** The period of profiling coverage and other statistics. */
    private long mProfilePeriod;
    private int lastProfileStepsCount = 0;
    private boolean fuzzingStarted = false;
    /** U2-specific: activity visit count for coverage stats */
    private HashMap<String, Integer> activityCountHistory = new HashMap<>();

    private String stringOfGuiTree;
    protected final HandlerThread mHandlerThread = new HandlerThread("MonkeySourceApeU2");
    private static final Gson gson = new Gson();
    private OkHttpClient client;
    private Element hierarchy;
    private DocumentBuilder documentBuilder;
    private final ProxyServer server;
    private final U2Client u2Client;

    public MonkeySourceApeU2(Random random, List<ComponentName> MainApps,
                                 long throttle, boolean randomizeThrottle, boolean permissionTargetSystem,
                                 File outputDirectory, long profilePeriod) {
        super(random, MainApps, throttle, randomizeThrottle, outputDirectory);
        mProfilePeriod = profilePeriod;
        Logger.println("[MonkeySourceApeU2] ProfilePeriod: " + mProfilePeriod);
        connect();
        Logger.println("// device uuid is " + did);
        this.u2Client = U2Client.getInstance();
        this.server = new ProxyServer(8090, u2Client, this);
        try {
            server.start(NanoHTTPD.SOCKET_READ_TIMEOUT, false);
            Logger.println("[MonkeySourceApeU2] proxyServer started. Listening tcp:8090");
        } catch (IOException e) {
            Logger.println("[MonkeySourceApeU2] Error when trying to start the proxy server: " + e.getMessage());
            e.printStackTrace();
            throw new RuntimeException(e);
        }
    }

    public File getDeviceOutputDir(){
        return server.getDeviceOutputDir();
    }

    public void processFailureNScreenshots() {
        server.processFailureNScreenshots();
    }

    public String peekImageQueue() {
        return server.peekImageQueue();
    }

    /**
     * If this activity could be interacted with. Should be in white list or not in blacklist or
     * not specified.
     * @param cn Component Name of this activity
     * @return If could be interacted, return true
     */
    private boolean checkAppActivity(ComponentName cn) {
        return cn == null || MonkeyUtils.getPackageFilter().checkEnteringPackage(cn.getPackageName());
    }

    public void setVerbose(int verbose) {
        mVerbose = verbose;
    }

    public void checkAppActivity() {
        ComponentName cn = getTopActivityComponentName();
        if (cn == null) {
            Logger.println("// get activity api error");
            clearEvent();
            startRandomMainApp();
            return;
        }
        String className = cn.getClassName();
        String pkg = cn.getPackageName();
        boolean allow = MonkeyUtils.getPackageFilter().checkEnteringPackage(pkg);

        if (allow) {
            if (!this.currentActivity.equals(className)) {
                this.currentActivity = className;
                activityHistory.add(this.currentActivity);
                Integer count = activityCountHistory.get(this.currentActivity);
                activityCountHistory.put(currentActivity, (count != null ? count : 0) + 1);
                Logger.println("// [Monkey] current activity is " + this.currentActivity);
                timestamp++;
            }
        }else
            dealWithBlockedActivity(cn);
    }

    public void updateActivityHistory() {
        ComponentName cn = getTopActivityComponentName();
        if (cn == null) {
            Logger.println("// get activity api error");
            return;
        }
        String className = cn.getClassName();
        if (!this.currentActivity.equals(className)) {
            this.currentActivity = className;
            activityHistory.add(this.currentActivity);
            Integer count = activityCountHistory.get(this.currentActivity);
            activityCountHistory.put(currentActivity, (count != null ? count : 0) + 1);
            Logger.println("// [Script] current activity is " + this.currentActivity);
            timestamp++;
        }
    }

    /**
     * If the given component is not allowed to interact with, start a random app or
     * generating a fuzzing action
     * @param cn Component that is not allowed to interact with
     */
    private void dealWithBlockedActivity(ComponentName cn) {
        String className = cn.getClassName();
        if (!hasEvent()) {
            if (appRestarted == 0) {
                Logger.println("// the top activity is " + className + ", not testing app, need inject restart app");
                startRandomMainApp();
                appRestarted = 1;
            } else {
                if (!AndroidDevice.isAtPhoneLauncher(className)) {
                    Logger.println("// the top activity is " + className + ", not testing app, need inject fuzz event");
                    Action fuzzingAction = generateFuzzingAction(true);
                    generateEventsForAction(fuzzingAction);
                } else {
                    fullFuzzing = false;
                }
                appRestarted = 0;
            }
        }
    }

    protected void startRandomMainApp() {
        generateActivityEvents(randomlyPickMainApp(), false, false);
    }

    /**
     * if the queue is empty, we generate events first
     *
     * @return the first event in the queue
     */
    public MonkeyEvent getNextEvent() {
        checkAppActivity();
        if (checkMonkeyStepDone()){
            if (shouldProfile()){
                Logger.println("[MonkeySourceApeU2] Profiling coverage...");
                profileCoverage();
            }
            MonkeySemaphore.doneMonkey.release();
            if (mVerbose > 3){
                Logger.println("[MonkeySourceApeU2] release semaphore： doneMonkey");
            }
        }
        if (!hasEvent()) {
            try {
                if (mVerbose > 3){
                    Logger.println("[MonkeySourceApeU2] wait semaphore: stepMonkey");
                }
                MonkeySemaphore.stepMonkey.acquire();
                if (mVerbose > 3){
                    Logger.println("[MonkeySourceApeU2] acquired semaphore: stepMonkey");
                }
                Logger.println("[MonkeySourceApeU2] stepsCount: " + server.stepsCount);
                if (server.monkeyIsOver) {
                    Logger.println("[MonkeySourceApeU2] received signal: MonkeyIsOver");
                    return null;
                }
                generateEvents();
                fuzzingStarted = true;
            } catch (RuntimeException e) {
                Logger.errorPrintln(e.getMessage());
                e.printStackTrace();
                clearEvent();
                return null;
            } catch (InterruptedException e) {
                throw new RuntimeException(e);
            }
        }
        mEventCount++;
        return popEvent();
    }

    private boolean shouldProfile(){
        return mProfilePeriod > 0 && server.stepsCount != 0 && server.stepsCount % mProfilePeriod == 0;
    }

    /**
     * Check if the previous monkey step has been finished.
     * Algorithm: the event queue is empty and the length of event queue change from 1 to 0
     * This is for checking an edge case: As fastbot starts, the event queue is empty, but this
     * does not represent a monkey event was finished.
     * @return a monkey event was finished
     */
    private boolean checkMonkeyStepDone() {
        return (!hasEvent() && fuzzingStarted);
    }

    public int getStepsCount() {return server.stepsCount;}


    public void connect() {
        client = OkHttpClient.getInstance();
        for (int i = 0; i < 10; i++){
            sleep(2000);
            if (client.connect()) {
                return;
            }
        }
        throw new RuntimeException("Fail to connect to U2Server");
    }

    /**
     * Get the xml Document Builder
     * @return documentBuilder
     */
    public DocumentBuilder getDocumentBuilder() {
        if (documentBuilder == null)
        {
            DocumentBuilderFactory factory = DocumentBuilderFactory.newInstance();
            try {
                documentBuilder = factory.newDocumentBuilder();
            } catch (ParserConfigurationException e) {
                throw new RuntimeException(e);
            }
        }
        return documentBuilder;
    }


    /**
     * Dump hierarchy with u2.
     *  {
     *     "jsonrpc": "2.0",
     *     "id": 1,
     *     "method": "dumpWindowHierarchy",
     *     "params": [
     *         false,
     *         50
     *     ]
     * }
     */
    public void dumpHierarchy() {
        String res;

        if (server.useCache){
            // Use the cached hierarchy response in the server.
            Logger.println("[MonkeySourceApeU2] Latest event is MonkeyEvent. Use the cached hierarchy.");
            res = server.getHierarchyResponseCache();
        }
        else {
            try {
                Response hierarchyResponse = u2Client.dumpHierarchy();
                res = hierarchyResponse.body().string();
            } catch (IOException e)
            {
                throw new RuntimeException(e);
            }
        }

        JsonRPCResponse res_obj = gson.fromJson(res, JsonRPCResponse.class);
        String xmlString = res_obj.getResult();

        Logger.println("[MonkeySourceApeU2] Successfully Got hierarchy");
        if (mVerbose > 3) {
            Logger.println("[MonkeySourceApeU2] The full xmlString is:");
            Logger.println(xmlString);
        }

        Document document;

        try {
            // Use StringReader to transform the String into InputSource
            InputSource is = new InputSource(new StringReader(xmlString));
            // Parse InputSource to get the Document object
            document = getDocumentBuilder().parse(is);
            document.getDocumentElement().normalize();

            disableBlockWidgets(document);
            disableBlockTrees(document);

            hierarchy = getRootElement(document);
            TreeBuilder.filterTree(hierarchy);
            stringOfGuiTree = hierarchy != null ? TreeBuilder.dumpDocumentStrWithOutTree(hierarchy) : "";
        } catch (Exception e){
            e.printStackTrace();
            throw new RuntimeException(e);
        }
    }

    /**
     * Set the block_widgets interaction attrs to false to disable it during fuzzing.
     * @param document The source xml document.
     * @throws XPathExpressionException .
     */
    private void disableBlockWidgets(Document document) throws XPathExpressionException {
        // filter the block widgets
        XPath xpath = XPathFactory.newInstance().newXPath();
        for (String expr : server.blockWidgets) {
            NodeList nodes = (NodeList) xpath.evaluate(expr, document, XPathConstants.NODESET);
            for (int i = 0; i < nodes.getLength(); i++) {
                Element e = (Element) nodes.item(i);
                setElementAttributes(e);
            }
        }
    }

    private void disableBlockTrees(Document document) throws XPathExpressionException {
        XPath xpath = XPathFactory.newInstance().newXPath();
        for (String expr : server.blockTrees) {
            NodeList nodes = (NodeList) xpath.evaluate(expr, document, XPathConstants.NODESET);
            for (int i = 0; i < nodes.getLength(); i++) {
                Node node = nodes.item(i);
                if (node.getNodeType() == Node.ELEMENT_NODE) {
                    disableElementAndDescendants((Element) node);
                }
            }
        }
    }

    private void disableElementAndDescendants(Element element) {
        setElementAttributes(element);
        // Recursively disable all child elements
        NodeList children = element.getChildNodes();
        for (int i = 0; i < children.getLength(); i++) {
            Node child = children.item(i);
            if (child.getNodeType() == Node.ELEMENT_NODE) {
                disableElementAndDescendants((Element) child);
            }
        }
    }

    public void setElementAttributes(Element element) {
        if (mVerbose > 3) {
            Logger.println("[MonkeySourceApeU2] Disable element: " + getElementAttributes(element));
        }
        // Disable the current element
        element.setAttribute("clickable", "false");
        element.setAttribute("long-clickable", "false");
        element.setAttribute("scrollable", "false");
        element.setAttribute("checkable", "false");
        element.setAttribute("enabled", "false");
        element.setAttribute("focusable", "false");

        // Log the disabled element
        if (mVerbose > 3) {
            Logger.println("[MonkeySourceApeU2] Disabled element: " + getElementAttributes(element));
        }
    }

    public Map<String, String> getElementAttributes(Element element) {
        NamedNodeMap attrs = element.getAttributes();
        Map<String, String> map = new HashMap<>();
        for (int i = 0; i < attrs.getLength(); i++) {
            Node attr = attrs.item(i);
            map.put(attr.getNodeName(), attr.getNodeValue());
        }
        return map;
    }


    /**
     * The response from u2 contains all the components on screen.
     * @param tree The root of tree return by u2.
     * @return The root of the current activate testing package.
     */
    public Element getRootElement(Document tree) {
        NodeList childNodes = tree.getDocumentElement().getChildNodes();
        // traverse the child list backwards to filter the input_method keyboard
        for (int i = 0; i < childNodes.getLength(); i++) {
            Node node = childNodes.item(i);
            String cur_package;
            if (node.getNodeType() == Node.ELEMENT_NODE) {
                cur_package = ((Element) node).getAttribute("package");
                if (!"com.android.systemui".equals(cur_package) && !cur_package.contains("inputmethod") && !"android".equals(cur_package)) {
                    if (mVerbose > 3){
                        Logger.println("[MonkeySourceApeU2] RootElement:"+cur_package);
                    }
                    return (Element) node;
                }
            }
        }
        return null;
    }

    public Element getHierarchy() {
        return hierarchy;
    }

    void resetRotation() {
        addEvent(new MonkeyRotationEvent(Surface.ROTATION_0, false));
    }

    public boolean dealWithSystemUI(Element info) {
        if (info == null || info.getAttribute("package") == null){
            Logger.println("get null accessibility node");
            return false;
        }
        String packageName = info.getAttribute("package");
        if(packageName.equals("com.android.systemui")) {
            Logger.println("get notification window or other system windows");
            Rect bounds = AndroidDevice.getDisplayBounds();
            // press home
            generateKeyEvent(KeyEvent.KEYCODE_HOME);
            //scroll up
            generateScrollEventAt(bounds, SCROLL_BOTTOM_UP);
            // launch app
            generateActivityEvents(randomlyPickMainApp(), false, false);
            generateThrottleEvent(1000);
            return true;
        }
        return false;
    }


    public Element getRootInActiveWindow(){
        return hierarchy;
    }

    protected void generateEvents() {
        if (hasEvent()) {
            return;
        }

        resetRotation();
        ComponentName topActivityName = null;
        String stringOfGuiTree = "";
        Action fuzzingAction = null;
        Element info = null;

        int repeat = refetchInfoCount;

        int retry = 2;
        while ("".equals(stringOfGuiTree) && retry-- > 0){
            dumpHierarchy();
            stringOfGuiTree = this.stringOfGuiTree;
        }

        // try to get AccessibilityNodeInfo quickly for several times.
        while (repeat-- > 0) {
            topActivityName = this.getTopActivityComponentName();
            info = getRootInActiveWindow();
            // this two operations may not be the same
            if (info == null || topActivityName == null) {
                sleep(refetchInfoWaitingInterval);
                continue;
            }

            Logger.println("// Event id: " + mEventId);
            if (dealWithSystemUI(info))
                return;
            break;
        }

        // If node is null, try to get AccessibilityNodeInfo slow for only once
        if (info == null) {
            topActivityName = this.getTopActivityComponentName();
            info = getRootInActiveWindowSlow();
            if (info != null) {
                Logger.println("// Event id: " + mEventId);
                if (dealWithSystemUI(info))
                    return;
            }
        }

        // If node is not null, build tree and recycle this resource.
        if (info != null) {
            stringOfGuiTree = this.stringOfGuiTree;
            if (mVerbose > 3) {
                Logger.println("[MonkeySourceApeU2] StringOfGuiTree for agent in fastbot:");
                Logger.println(stringOfGuiTree);
            }
            // info.recycle();
        }

        // For user specified actions, during executing, fuzzing is not allowed.
        boolean allowFuzzing = true;

        Logger.println("topActivity Name: " + topActivityName);
//        Logger.println("GuiTree: " + stringOfGuiTree);

        if (topActivityName != null && !"".equals(stringOfGuiTree)) {
            try {
                long rpc_start = System.currentTimeMillis();

                Logger.println("// Dumped stringOfGuiTree");
                Logger.println("topActivityName: " + topActivityName.getClassName());
//                Logger.println(stringOfGuiTree);

                Operate operate = AiClient.getAction(topActivityName.getClassName(), stringOfGuiTree);

                // record the monkeyStep
                server.recordMonkeyStep(operate, topActivityName.getClassName());

                operate.throttle += (int) this.mThrottle;
                // For user specified actions, during executing, fuzzing is not allowed.
                allowFuzzing = operate.allowFuzzing;
                ActionType type = operate.act;
                Logger.println("action type: " + type.toString());
                Logger.println("rpc cost time: " + (System.currentTimeMillis() - rpc_start) + " ms");

                mReusableRect.set(0, 0, 0, 0);
                mReusablePointFloats.clear();

                if (type.requireTarget()) {
                    if (!operate.setRectFromPos(mReusableRect)) type = ActionType.NOP;
                }

                timeStep++;
                String sid = operate.sid;
                String aid = operate.aid;
                long timeMillis = System.currentTimeMillis();

                if (saveGUITreeToXmlEveryStep) {
                    checkOutputDir();
                    File xmlFile = new File(checkOutputDir(), String.format(stringFormatLocale,
                            "step-%d-%s-%s-%s.xml", timeStep, sid, aid, timeMillis));
                    Logger.infoFormat("Saving GUI tree to %s at step %d %s %s",
                            xmlFile, timeStep, sid, aid);

                    BufferedWriter out;
                    try {
                        out = new BufferedWriter(new OutputStreamWriter(new FileOutputStream(xmlFile, false)));
                        out.write(stringOfGuiTree);
                        out.flush();
                        out.close();
                    } catch (java.io.FileNotFoundException e) {
                    } catch (java.io.IOException e) {
                    }
                }


                ModelAction modelAction = new ModelAction(type, topActivityName, mReusablePointFloats, mReusableRect);
                modelAction.setThrottle(operate.throttle);

                // Complete the info for specific action type
                switch (type) {
                    case CLICK:
                        modelAction.setInputText(operate.text);
                        modelAction.setClearText(operate.clear);
                        modelAction.setEditText(operate.editable);
                        modelAction.setRawInput(operate.rawinput);
                        break;
                    case LONG_CLICK:
                        modelAction.setWaitTime(operate.waitTime);
                        break;
                    case SHELL_EVENT:
                        modelAction.setShellCommand(operate.text);
                        modelAction.setWaitTime(operate.waitTime);
                        break;
                    default:
                        break;
                }

                generateEventsForAction(modelAction);

                // check if could select next fuzz action from full fuzz-able action options.
                switch (type) {
                    case RESTART:
                    case CLEAN_RESTART:
                    case CRASH:
                        fullFuzzing = false;
                        break;
                    case BACK:
                        fullFuzzing = !AndroidDevice.isAtAppMain(topActivityName.getClassName(), topActivityName.getPackageName());
                        break;
                    default:
                        fullFuzzing = !AndroidDevice.isAtPhoneLauncher(topActivityName.getClassName());
                        break;
                }

            } catch (Exception e) {
                e.printStackTrace();
                generateThrottleEvent(mThrottle);
            }
        } else {
            Logger.println(
                    "// top activity is null or the corresponding tree is null, " +
                            "accessibility maybe error, fuzz needed."
            );
            fuzzingAction = generateFuzzingAction(fullFuzzing);
            Logger.println("// Fuzzing action: " + fuzzingAction.toString());
            server.recordMonkeyStep(fuzzingAction);
            generateEventsForAction(fuzzingAction);
        }
    }


    public Element getRootInActiveWindowSlow() {
        dumpHierarchy();
        sleep(1000);
        return getRootInActiveWindow();
    }

    /**
     * According to the action type of the action argument, generate its corresponding
     * event, and set throttle if necessary.
     * @param action generated action, could be action from native model, or generated fuzzing
     *               action from CustomEventFuzzer
     */
    protected void generateEventsForAction(Action action) {
        super.generateEventsForAction(action);
        long throttle = (action instanceof FuzzAction ? 0 : action.getThrottle());
        generateThrottleEvent(throttle);
    }

    public boolean validate() {
        client = OkHttpClient.getInstance();
        return client.connect();
    }


    @Override
    protected FuzzAction generateFuzzingAction(boolean sampleFromAllFuzzingActions) {
        List<CustomEvent> events = sampleFromAllFuzzingActions ?
                CustomEventFuzzer.generateFuzzingEvents() :
                CustomEventFuzzer.generateSimplifyFuzzingEvents();
        return new FuzzAction(events);
    }

    private void profileCoverage() {
        HashSet<String> set = mTotalActivities;
        String[] testedActivities = this.activityHistory.toArray(new String[0]);

        int j = 0;
        String activity = "";
        for (String testedActivity : testedActivities) {
            activity = testedActivity;
            if (set.contains(activity)) {
                j++;
            }
        }

        float f = 0;
        int s = set.size();
        if (s > 0) {
            f = 1.0f * j / s * 100;
        }

        String[] totalActivities = set.toArray(new String[0]);
        if (lastProfileStepsCount != server.stepsCount){
            lastProfileStepsCount = server.stepsCount;
            server.saveCoverageStatistics(
                    new CoverageData(server.stepsCount, f, totalActivities, testedActivities, activityCountHistory)
            );
        }
    }

    public void tearDown() {
        profileCoverage();
        server.tearDown();
        printCoverage();
    }

}
