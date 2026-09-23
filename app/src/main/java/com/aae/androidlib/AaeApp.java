package com.aae.androidlib;

import android.app.Application;
import android.content.Intent;
import android.os.Build;

/**
 * Installs the global crash handler: any uncaught exception (Java or a JNI
 * crash surfaced as an Error) is captured with device/engine context and
 * shown in CrashActivity instead of a silent death.
 */
public class AaeApp extends Application {

    private volatile boolean crashing;

    @Override
    public void onCreate() {
        super.onCreate();
        final Thread.UncaughtExceptionHandler previous =
                Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler(
                new Thread.UncaughtExceptionHandler() {
                    @Override
                    public void uncaughtException(Thread thread, Throwable e) {
                        if (!crashing) {
                            crashing = true;
                            try {
                                showCrash(thread, e);
                            } catch (Throwable ignored) {
                            }
                        }
                        if (previous != null) {
                            previous.uncaughtException(thread, e);
                        }
                        try {
                            Thread.sleep(400);
                        } catch (InterruptedException ignored) {
                        }
                        android.os.Process.killProcess(android.os.Process.myPid());
                        System.exit(10);
                    }
                });
    }

    private void showCrash(Thread thread, Throwable e) {
        StringBuilder sb = new StringBuilder();
        sb.append("Aae Tester crash\n");
        sb.append("thread: ").append(thread.getName()).append('\n');
        sb.append("device: ").append(Build.MANUFACTURER).append(' ')
                .append(Build.MODEL).append(" (Android ").append(Build.VERSION.RELEASE)
                .append(", API ").append(Build.VERSION.SDK_INT).append(")\n");
        try {
            sb.append("engine: ").append(bin.nt.aae.Aae.version()).append('\n');
        } catch (Throwable ignored) {
            sb.append("engine: (unavailable — crashed while loading)\n");
        }
        sb.append('\n');
        appendTrace(sb, e);
        String report = sb.toString();
        if (report.length() > 65536) {
            report = report.substring(0, 65536) + "\n…(truncated)";
        }
        Intent i = new Intent(this, CrashActivity.class);
        i.putExtra(CrashActivity.EXTRA_REPORT, report);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(i);
    }

    private static void appendTrace(StringBuilder sb, Throwable e) {
        java.io.StringWriter sw = new java.io.StringWriter();
        java.io.PrintWriter pw = new java.io.PrintWriter(sw);
        e.printStackTrace(pw);
        pw.flush();
        sb.append(sw.toString());
    }
}
