package dev.r0hook.lab;

import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicBoolean;

/** Runs one command at a time, including delivery of its completion. */
final class LabCommandRunner {
    interface Operation {
        String run(String command) throws Exception;
    }

    interface Completion {
        void complete(String command, String reply, Throwable error);
    }

    private final Executor worker;
    private final Executor callbacks;
    private final Operation operation;
    private final AtomicBoolean running = new AtomicBoolean();

    LabCommandRunner(Executor worker, Executor callbacks, Operation operation) {
        this.worker = worker;
        this.callbacks = callbacks;
        this.operation = operation;
    }

    boolean isRunning() {
        return running.get();
    }

    boolean submit(String command, Completion completion) {
        if (!running.compareAndSet(false, true)) {
            return false;
        }
        try {
            worker.execute(new Runnable() {
                @Override
                public void run() {
                    execute(command, completion);
                }
            });
        } catch (RuntimeException error) {
            running.set(false);
            throw error;
        }
        return true;
    }

    private void execute(String command, Completion completion) {
        String reply = null;
        Throwable failure = null;
        try {
            reply = operation.run(command);
        } catch (Exception | LinkageError error) {
            failure = error;
        }
        final String result = reply;
        final Throwable error = failure;
        try {
            callbacks.execute(new Runnable() {
                @Override
                public void run() {
                    running.set(false);
                    completion.complete(command, result, error);
                }
            });
        } catch (RuntimeException dispatchError) {
            running.set(false);
            throw dispatchError;
        }
    }
}
