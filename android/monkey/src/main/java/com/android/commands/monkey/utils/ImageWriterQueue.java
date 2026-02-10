/*
 * Copyright 2020 Advanced Software Technologies Lab at ETH Zurich, Switzerland
 *
 * Modified - Copyright (c) 2020 Bytedance Inc.
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
/*
 * @author Zhao Zhang
 */

package com.android.commands.monkey.utils;

import android.graphics.Bitmap;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.LinkedList;


public class ImageWriterQueue implements Runnable {

    // Visible to subclasses (e.g. CacheImageWriterQueue) for direct queue manipulation.
    protected final LinkedList<Req> requestQueue = new LinkedList<Req>();
    // Last enqueued image file name, accessed from ProxyServer and subclasses.
    public volatile String lastImage = "";

    @Override
    public void run() {
        try {
            while (!Thread.currentThread().isInterrupted()) {
                Req req = read();
                if (req != null) {
                    writePNG(req);
                }
            }
        } catch (InterruptedException e) {
            // Restore interrupt status and fall through to final flush.
            Thread.currentThread().interrupt();
        } finally {
            // Best-effort flush of remaining queued requests on shutdown.
            flush();
        }
    }

    protected void writePNG(Req req) {
        Bitmap map = req.map;
        File dst = req.dst;
        if (map == null) {
            Logger.format("No screen shot for %s", dst.getAbsolutePath());
            return;
        }
        try (FileOutputStream fos = new FileOutputStream(dst)) {
            map.compress(Bitmap.CompressFormat.PNG, 75, fos);
        } catch (IOException e) {
            Logger.errorPrintln("Fail to save screen shot to " + dst.getAbsolutePath() + ": " + e.getMessage());
        } finally {
            map.recycle();
        }
    }

    public synchronized void add(Bitmap map, File dst) {
        lastImage = (dst != null ? dst.getName() : "");
        requestQueue.addLast(new Req(map, dst));

        int maxSize = Config.flushImagesThreshold;
        if (maxSize > 0 && requestQueue.size() > maxSize) {
            // When the queue is too large, drop the oldest screenshots instead of blocking the producer with a synchronous flush.
            Logger.format("ImageQueue is too full (%d > %d), dropping oldest screenshots.", requestQueue.size(), maxSize);
            while (requestQueue.size() > maxSize) {
                Req dropped = requestQueue.removeFirst();
                if (dropped.map != null && !dropped.map.isRecycled()) {
                    dropped.map.recycle();
                }
            }
        }
        notifyAll();
    }

    public synchronized void flush() {
        while (!requestQueue.isEmpty()) {
            Req req = requestQueue.removeFirst();
            writePNG(req);
        }
    }

    private synchronized Req read() throws InterruptedException {
        while (requestQueue.isEmpty()) {
            wait();
        }
        Req req = requestQueue.removeFirst();
        return req;
    }

    public synchronized void tearDown() {
        flush();
    }

    public String getLastImage() {
        return lastImage;
    }

    static class Req {
        final Bitmap map;
        final File dst;

        public Req(Bitmap map, File dst) {
            this.map = map;
            this.dst = dst;
        }
    }
}
