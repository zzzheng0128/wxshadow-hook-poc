package dev.r0hook.lab;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.lang.ref.WeakReference;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ThreadFactory;

public final class MainActivity extends Activity {
    private static final String TAG = "R0Lab";

    static {
        System.loadLibrary("labprobe");
    }

    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    // A process-scoped worker preserves native command thread affinity across
    // Activity recreation. Never interrupt an in-flight native operation.
    private static final LabCommandRunner CONTROLS = new LabCommandRunner(
            Executors.newSingleThreadExecutor(new ThreadFactory() {
                @Override
                public Thread newThread(Runnable task) {
                    return new Thread(task, "R0LabControl");
                }
            }), new Executor() {
                @Override
                public void execute(Runnable task) {
                    if (!MAIN.post(task)) {
                        throw new RejectedExecutionException("Main looper is unavailable");
                    }
                }
            }, new LabCommandRunner.Operation() {
                @Override
                public String run(String args) {
                    return nativeControl(args);
                }
            });
    private static final LabCommandRunner.Completion COMPLETION =
            new LabCommandRunner.Completion() {
                @Override
                public void complete(String args, String reply, Throwable error) {
                    publishResult(args, reply, error);
                }
            };
    // These fields are read and written only on the main thread.
    private static WeakReference<MainActivity> currentActivity = new WeakReference<>(null);
    private static String lastOutput = "";

    private TextView output;
    private EditText command;
    private Button send;

    private static native String nativeDescribe();
    private static native String nativeControl(String args);

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int padding = (int) (16 * getResources().getDisplayMetrics().density);
        root.setPadding(padding, padding, padding, padding);

        TextView identity = new TextView(this);
        identity.setText(nativeDescribe() + " java_threads=" + Thread.activeCount());
        root.addView(identity);

        command = new EditText(this);
        command.setSingleLine(true);
        String initialCommand = getIntent().getStringExtra("r0lab_command");
        if (state != null) {
            initialCommand = state.getString("command", "status");
            if (lastOutput.isEmpty()) {
                lastOutput = state.getString("output", "");
            }
        }
        command.setText(initialCommand == null ? "status" : initialCommand);
        root.addView(command);

        send = new Button(this);
        send.setText("Send Lab Control");
        root.addView(send);

        output = new TextView(this);
        root.addView(output);
        setContentView(root);
        currentActivity = new WeakReference<>(this);
        renderState();

        send.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                runControl(command.getText().toString());
            }
        });

        // Intent commands are one-shot; recreation must not replay them.
        if (state == null && getIntent().hasExtra("r0lab_command")) {
            runControl(command.getText().toString());
        }
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        if (intent.hasExtra("r0lab_command")) {
            String args = intent.getStringExtra("r0lab_command");
            command.setText(args);
            runControl(args);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        currentActivity = new WeakReference<>(this);
        renderState();
    }

    @Override
    protected void onSaveInstanceState(Bundle state) {
        state.putString("command", command.getText().toString());
        state.putString("output", lastOutput);
        super.onSaveInstanceState(state);
    }

    @Override
    protected void onDestroy() {
        if (currentActivity.get() == this) {
            currentActivity.clear();
        }
        super.onDestroy();
    }

    private void renderState() {
        boolean running = CONTROLS.isRunning();
        command.setEnabled(!running);
        send.setEnabled(!running);
        output.setText(running ? "Running…" : lastOutput);
    }

    private void runControl(String args) {
        if (args == null || args.trim().isEmpty()) {
            publishResult("", null, new IllegalArgumentException("Missing command"));
            return;
        }
        try {
            if (!CONTROLS.submit(args.trim(), COMPLETION)) {
                Log.w(TAG, "command=" + args + " error=busy");
            }
            renderState();
        } catch (RuntimeException error) {
            publishResult(args, null, error);
        }
    }

    private static void publishResult(String args, String reply, Throwable error) {
        if (error == null) {
            lastOutput = "command=" + args + "\n" + reply;
            Log.i(TAG, lastOutput);
        } else {
            lastOutput = "command=" + args + " error=" + error;
            Log.e(TAG, lastOutput, error);
        }
        MainActivity activity = currentActivity.get();
        if (activity != null && !activity.isFinishing() && !activity.isDestroyed()) {
            activity.renderState();
        }
    }
}
