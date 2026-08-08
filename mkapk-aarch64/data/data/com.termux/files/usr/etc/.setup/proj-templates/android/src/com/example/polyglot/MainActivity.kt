package com.example.polyglot;

import android.app.Activity
import android.os.Bundle
import android.widget.TextView

class MainActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        // Instantiate the Java class to run cross-compilation validation checks
        val bridge = JavaBridge()
        val displayString = bridge.compiledMessageFromJava

        val textViewer = findViewById<TextView>(R.id.display_view)
        textViewer.text = displayString
    }
}