package dev.r0hook.lab;

import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/** Offline behavior tests: no Android runtime, JNI or device access. */
public final class LabCommandRunnerTest {
    private static final class PendingExecutor implements Executor {
        final BlockingQueue<Runnable> tasks = new LinkedBlockingQueue<>();

        @Override
        public void execute(Runnable task) {
            tasks.add(task);
        }

        void runNext() throws Exception {
            Runnable task = tasks.poll(2, TimeUnit.SECONDS);
            check(task != null, "expected queued task");
            task.run();
        }
    }

    private static void check(boolean condition, String message) {
        if (!condition) {
            throw new AssertionError(message);
        }
    }

    private static void deferredAndSingleFlight() throws Exception {
        PendingExecutor worker = new PendingExecutor();
        PendingExecutor callbacks = new PendingExecutor();
        AtomicInteger operations = new AtomicInteger();
        AtomicInteger completions = new AtomicInteger();
        LabCommandRunner runner = new LabCommandRunner(worker, callbacks, command -> {
            operations.incrementAndGet();
            return "reply:" + command;
        });
        LabCommandRunner.Completion complete = (command, reply, error) -> {
            check(!runner.isRunning(), "completion must release busy state");
            check("reply:first".equals(reply), "reply changed");
            check("first".equals(command) && error == null, "completion changed");
            completions.incrementAndGet();
        };
        check(runner.submit("first", complete), "first rejected");
        check(operations.get() == 0, "operation ran inline");
        check(!runner.submit("duplicate", complete), "duplicate queued");
        worker.runNext();
        check(operations.get() == 1 && completions.get() == 0, "completion ran inline");
        check(!runner.submit("before-callback", complete), "accepted before completion");
        callbacks.runNext();
        check(completions.get() == 1, "missing completion");
        check(worker.tasks.isEmpty(), "rejected command still queued");
        check(runner.submit("first", complete), "next command rejected");
        worker.runNext();
        callbacks.runNext();
    }

    private static void operationFailure(Throwable failure) throws Exception {
        PendingExecutor worker = new PendingExecutor();
        PendingExecutor callbacks = new PendingExecutor();
        AtomicReference<Throwable> observed = new AtomicReference<>();
        LabCommandRunner runner = new LabCommandRunner(worker, callbacks, command -> {
            if (failure instanceof Exception) {
                throw (Exception) failure;
            }
            throw (LinkageError) failure;
        });
        check(runner.submit("failure", (command, reply, error) -> {
            check(reply == null, "failed operation returned a reply");
            observed.set(error);
        }), "failure command rejected");
        worker.runNext();
        check(runner.isRunning(), "busy released before error delivery");
        callbacks.runNext();
        check(observed.get() == failure, "failure was lost");
        check(!runner.isRunning(), "failure left runner busy");
    }

    private static void rejectedExecutors() throws Exception {
        Executor rejected = task -> { throw new RejectedExecutionException("closed"); };
        LabCommandRunner rejectedWorker = new LabCommandRunner(rejected, Runnable::run, command -> command);
        try {
            rejectedWorker.submit("first", (command, reply, error) -> {});
            throw new AssertionError("worker rejection swallowed");
        } catch (RejectedExecutionException expected) {
            check(!rejectedWorker.isRunning(), "worker rejection left runner busy");
        }
        PendingExecutor worker = new PendingExecutor();
        LabCommandRunner rejectedCallback = new LabCommandRunner(worker, rejected, command -> command);
        rejectedCallback.submit("first", (command, reply, error) -> {});
        try {
            worker.runNext();
            throw new AssertionError("callback rejection swallowed");
        } catch (RejectedExecutionException expected) {
            check(!rejectedCallback.isRunning(), "callback rejection left runner busy");
        }
    }

    private static void completionFailure() throws Exception {
        PendingExecutor worker = new PendingExecutor();
        PendingExecutor callbacks = new PendingExecutor();
        LabCommandRunner runner = new LabCommandRunner(worker, callbacks, command -> command);
        runner.submit("first", (command, reply, error) -> { throw new IllegalStateException("UI"); });
        worker.runNext();
        try {
            callbacks.runNext();
            throw new AssertionError("completion failure swallowed");
        } catch (IllegalStateException expected) {
            check(!runner.isRunning(), "completion failure left runner busy");
        }
    }

    private static void simultaneousSubmissions() throws Exception {
        PendingExecutor worker = new PendingExecutor();
        PendingExecutor callbacks = new PendingExecutor();
        LabCommandRunner runner = new LabCommandRunner(worker, callbacks, command -> command);
        CountDownLatch start = new CountDownLatch(1);
        AtomicInteger accepted = new AtomicInteger();
        AtomicInteger interrupted = new AtomicInteger();
        Thread[] contenders = new Thread[12];
        for (int i = 0; i < contenders.length; i++) {
            contenders[i] = new Thread(() -> {
                try {
                    start.await();
                    if (runner.submit("race", (command, reply, error) -> {})) {
                        accepted.incrementAndGet();
                    }
                } catch (InterruptedException error) {
                    interrupted.incrementAndGet();
                    Thread.currentThread().interrupt();
                }
            });
            contenders[i].start();
        }
        start.countDown();
        for (Thread contender : contenders) {
            contender.join(2000);
            check(!contender.isAlive(), "submitter did not finish");
        }
        check(interrupted.get() == 0 && accepted.get() == 1, "single-flight admission failed");
        check(worker.tasks.size() == 1, "more than one command queued");
        worker.runNext();
        callbacks.runNext();
    }

    private static void workerThreadAffinity() throws Exception {
        ExecutorService worker = Executors.newSingleThreadExecutor();
        PendingExecutor callbacks = new PendingExecutor();
        AtomicReference<Thread> previous = new AtomicReference<>();
        Thread caller = Thread.currentThread();
        AtomicReference<Throwable> failure = new AtomicReference<>();
        LabCommandRunner runner = new LabCommandRunner(worker, callbacks, command -> {
            Thread current = Thread.currentThread();
            if (current == caller || (previous.get() != null && previous.get() != current)) {
                throw new IllegalStateException("worker thread changed or ran on caller");
            }
            previous.set(current);
            return command;
        });
        try {
            for (int i = 0; i < 3; i++) {
                check(runner.submit("sequential", (command, reply, error) -> failure.set(error)),
                        "sequential command rejected");
                callbacks.runNext();
                check(failure.get() == null, "thread affinity failed");
            }
        } finally {
            worker.shutdown();
            check(worker.awaitTermination(2, TimeUnit.SECONDS), "worker did not stop");
        }
    }

    public static void main(String[] args) throws Exception {
        deferredAndSingleFlight();
        operationFailure(new IllegalStateException("operation"));
        operationFailure(new UnsatisfiedLinkError("native library"));
        rejectedExecutors();
        completionFailure();
        simultaneousSubmissions();
        workerThreadAffinity();
        System.out.println("lab_command_runner_host cases=7 result=pass");
    }
}
