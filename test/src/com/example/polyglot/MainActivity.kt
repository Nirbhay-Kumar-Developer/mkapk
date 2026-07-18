package com.example.polyglot

import android.app.Activity
import android.os.Bundle
import android.widget.TextView
// Added architectural component and UI component dependencies
import androidx.lifecycle.lifecycleScope
import com.google.android.material.snackbar.Snackbar
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

class MainActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.id.display_view) // Reference your layout element directly or via R.layout

        // Instantiate the Java class to run cross-compilation validation checks
        val bridge = JavaBridge()
        val displayString = bridge.compiledMessageFromJava

        val textViewer = findViewById<TextView>(R.id.display_view)
        textViewer.text = displayString

        // Utilizing added Coroutine and Lifecycle dependencies for asynchronous task tracking
        lifecycleScope.launch {
            delay(2000)
            
            // Utilizing Material Components dependency to push a UI Snackbar alert notification
            Snackbar.make(textViewer, "Dependencies resolved successfully!", Snackbar.LENGTH_LONG).show()
        }
    }
}