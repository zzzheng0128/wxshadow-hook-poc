package dev.r0hook.lab;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

public final class MainActivity extends Activity {
    private static final String TAG = "R0Lab";

    static {
        System.loadLibrary("labprobe");
    }

    private TextView output;
    private EditText command;

    private native String nativeDescribe();
    private native String nativeControl(String args);

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
        command.setText(getIntent().getStringExtra("r0lab_command") == null
                ? "status" : getIntent().getStringExtra("r0lab_command"));
        root.addView(command);

        Button send = new Button(this);
        send.setText("Send Lab Control");
        root.addView(send);

        output = new TextView(this);
        root.addView(output);
        setContentView(root);

        send.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                runControl(command.getText().toString());
            }
        });

        if (getIntent().hasExtra("r0lab_command")) {
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

    private void runControl(String args) {
        try {
            String message = "command=" + args + "\n" + nativeControl(args);
            output.setText(message);
            Log.i(TAG, message);
        } catch (Exception error) {
            String message = "command=" + args + " error=" + error;
            output.setText(message);
            Log.e(TAG, message, error);
        }
    }
}
