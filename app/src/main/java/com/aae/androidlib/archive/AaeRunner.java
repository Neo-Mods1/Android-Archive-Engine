package com.aae.androidlib.archive;

import android.os.Handler;
import android.os.Looper;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Single background executor for all archive work. Archive sessions are
 * single-threaded and libzip is not thread-safe across sessions sharing a
 * file, so every operation funnels through one worker thread instead of
 * spawning a thread per button.
 *
 * <p>Progress/cancellation: long operations poll an {@link OpHandle}; the
 * UI calls {@link OpHandle#cancel()} (e.g. a Cancel button). Views are only
 * touched on the UI thread via {@link #postUi(Runnable)}.
 */
public final class AaeRunner {

    private static final ExecutorService EXEC = Executors.newSingleThreadExecutor();
    private static final Handler UI = new Handler(Looper.getMainLooper());

    private AaeRunner() {
    }

    /** Cancellation + progress token passed to worker code. */
    public static final class OpHandle {
        private final AtomicBoolean cancelled = new AtomicBoolean(false);
        private volatile Future<?> future;

        public boolean isCancelled() {
            return cancelled.get();
        }

        public void cancel() {
            cancelled.set(true);
            Future<?> f = future;
            if (f != null) {
                f.cancel(true);
            }
        }

        void attach(Future<?> f) {
            future = f;
        }
    }

    public interface Job {
        void run(OpHandle op) throws Exception;
    }

    public interface Done {
        void onDone(OpHandle op);
    }

    /** Submits work; {@code done} always runs on the UI thread. */
    public static OpHandle submit(final Job job, final Done done) {
        final OpHandle op = new OpHandle();
        Future<?> f = EXEC.submit(new Runnable() {
            @Override
            public void run() {
                try {
                    if (!op.isCancelled()) {
                        job.run(op);
                    }
                } catch (final Exception e) {
                    postUi(new Runnable() {
                        @Override
                        public void run() {
                            if (done instanceof FailAware) {
                                ((FailAware) done).onFail(op, e);
                            } else if (done != null) {
                                done.onDone(op);
                            }
                        }
                    });
                    return;
                }
                postUi(new Runnable() {
                    @Override
                    public void run() {
                        if (done != null) {
                            done.onDone(op);
                        }
                    }
                });
            }
        });
        op.attach(f);
        return op;
    }

    /** Done callback that also receives failures. */
    public interface FailAware extends Done {
        void onFail(OpHandle op, Exception e);
    }

    public static void postUi(Runnable r) {
        UI.post(r);
    }

    /** True when called on the main thread (asserts for view updates). */
    public static boolean isUiThread() {
        return Looper.myLooper() == Looper.getMainLooper();
    }
}
