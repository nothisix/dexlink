package com.dexlink.app

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import com.dexlink.protocol.PacketProtocol
import java.util.Locale

class MainActivity : Activity() {

    companion object {
        private const val REQ_PERMS = 101
        private const val REQ_PROJECTION = 102

        private val BG_COLOR = Color.parseColor("#0D0D0D")
        private val SURFACE_COLOR = Color.parseColor("#161616")
        private val BORDER_COLOR = Color.parseColor("#2A2A2A")
        private val TEXT_PRIMARY = Color.parseColor("#F0F0F0")
        private val TEXT_MUTED = Color.parseColor("#7A7A7A")
        private val ACCENT_READY = Color.parseColor("#E5E5E5")
        private val ACCENT_CONNECTED = Color.parseColor("#00E676")
        private val ACCENT_ERR = Color.parseColor("#FF3B30")
    }

    private lateinit var statusDot: View
    private lateinit var statusLabel: TextView
    private lateinit var usbValue: TextView
    private lateinit var audioPermValue: TextView
    private lateinit var thermalValue: TextView
    private lateinit var actionButton: Button

    private val handler = Handler(Looper.getMainLooper())
    private val refreshTick = object : Runnable {
        override fun run() {
            updateUi()
            handler.postDelayed(this, 750L)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(buildUi())

        AudioStreamService.statusListener = {
            handler.post { updateUi() }
        }
        updateUi()
    }

    override fun onResume() {
        super.onResume()
        handler.removeCallbacks(refreshTick)
        handler.post(refreshTick)
    }

    override fun onPause() {
        handler.removeCallbacks(refreshTick)
        super.onPause()
    }

    override fun onDestroy() {
        AudioStreamService.statusListener = null
        super.onDestroy()
    }

    private fun dp(v: Int): Int =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v.toFloat(), resources.displayMetrics).toInt()

    private fun sharpBox(fill: Int, stroke: Int): GradientDrawable = GradientDrawable().apply {
        shape = GradientDrawable.RECTANGLE
        cornerRadius = dp(2).toFloat()
        setColor(fill)
        setStroke(dp(1), stroke)
    }

    private fun buildUi(): View {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(BG_COLOR)
            setPadding(dp(16), dp(20), dp(16), dp(20))
        }

        // Top header: DEXLINK + status dot (READY / ACTIVE)
        val header = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(2), 0, dp(2), dp(14))
        }

        val title = TextView(this).apply {
            text = "DEXLINK"
            setTextColor(TEXT_PRIMARY)
            textSize = 18f
            typeface = Typeface.create("sans-serif-medium", Typeface.BOLD)
            letterSpacing = 0.08f
            layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
        }

        statusDot = View(this).apply {
            layoutParams = LinearLayout.LayoutParams(dp(8), dp(8)).apply {
                marginEnd = dp(8)
            }
            background = GradientDrawable().apply {
                shape = GradientDrawable.OVAL
                setColor(ACCENT_READY)
            }
        }

        statusLabel = TextView(this).apply {
            text = "READY"
            setTextColor(TEXT_PRIMARY)
            textSize = 12f
            typeface = Typeface.MONOSPACE
        }

        header.addView(title)
        header.addView(statusDot)
        header.addView(statusLabel)
        root.addView(header)

        // Middle: 3 monospace data rows (USB LINK, AUDIO PERM, THERMAL)
        val panel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = sharpBox(SURFACE_COLOR, BORDER_COLOR)
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            )
        }

        val (row1, val1) = buildRow("USB LINK", "WAIT 127.0.0.1:${PacketProtocol.AUDIO_TELEMETRY_PORT}")
        val (row2, val2) = buildRow("AUDIO PERM", "NONE")
        val (row3, val3) = buildRow("THERMAL", "--.-C  --%")
        usbValue = val1
        audioPermValue = val2
        thermalValue = val3

        panel.addView(row1)
        panel.addView(divider())
        panel.addView(row2)
        panel.addView(divider())
        panel.addView(row3)
        root.addView(panel)

        // Spacer
        root.addView(View(this).apply {
            layoutParams = LinearLayout.LayoutParams(1, 0, 1f)
        })

        // Bottom: 1 flat rectangular button (GRANT AUDIO / STOP)
        actionButton = Button(this).apply {
            text = "GRANT AUDIO"
            setTextColor(TEXT_PRIMARY)
            textSize = 13f
            typeface = Typeface.create("sans-serif-medium", Typeface.BOLD)
            letterSpacing = 0.06f
            isAllCaps = true
            stateListAnimator = null
            background = sharpBox(SURFACE_COLOR, BORDER_COLOR)
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                dp(46)
            )
            setOnClickListener { onActionButtonClicked() }
        }
        root.addView(actionButton)

        return root
    }

    private fun buildRow(label: String, initialValue: String): Pair<LinearLayout, TextView> {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(12), dp(12), dp(12), dp(12))
        }
        val lbl = TextView(this).apply {
            text = label
            setTextColor(TEXT_MUTED)
            textSize = 12f
            typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
            letterSpacing = 0.05f
            layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
        }
        val valView = TextView(this).apply {
            text = initialValue
            setTextColor(TEXT_PRIMARY)
            textSize = 12f
            typeface = Typeface.MONOSPACE
            gravity = Gravity.END
        }
        row.addView(lbl)
        row.addView(valView)
        return row to valView
    }

    private fun divider(): View = View(this).apply {
        layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(1))
        setBackgroundColor(BORDER_COLOR)
    }

    private fun onActionButtonClicked() {
        val st = AudioStreamService.getStatus()
        if (st.active) {
            val stopIntent = Intent(this, AudioStreamService::class.java).apply {
                action = AudioStreamService.ACTION_STOP
            }
            startService(stopIntent)
            return
        }

        val needed = mutableListOf<String>()
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            needed.add(Manifest.permission.RECORD_AUDIO)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            needed.add(Manifest.permission.POST_NOTIFICATIONS)
        }
        if (needed.isNotEmpty()) {
            requestPermissions(needed.toTypedArray(), REQ_PERMS)
            return
        }
        requestProjection()
    }

    private fun requestProjection() {
        val mpm = getSystemService(MediaProjectionManager::class.java)
        @Suppress("DEPRECATION")
        startActivityForResult(mpm.createScreenCaptureIntent(), REQ_PROJECTION)
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQ_PERMS) {
            if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) {
                requestProjection()
            } else {
                updateUi()
            }
        }
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == REQ_PROJECTION) {
            if (resultCode == RESULT_OK && data != null) {
                val svcIntent = Intent(this, AudioStreamService::class.java).apply {
                    action = AudioStreamService.ACTION_START
                    putExtra(AudioStreamService.EXTRA_RESULT_CODE, resultCode)
                    putExtra(AudioStreamService.EXTRA_RESULT_DATA, data)
                }
                startForegroundService(svcIntent)
            }
            updateUi()
        }
    }

    private fun updateUi() {
        val st = AudioStreamService.getStatus()
        val thermal = TelemetryHelper.read(this)
        val hasMicPerm = checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED

        val dotColor = when {
            thermal.thermalWarn || st.linkState == AudioStreamService.LinkState.ERR -> ACCENT_ERR
            st.active && st.linkState == AudioStreamService.LinkState.CONNECTED -> ACCENT_CONNECTED
            st.active -> ACCENT_CONNECTED
            else -> ACCENT_READY
        }
        (statusDot.background as? GradientDrawable)?.setColor(dotColor)

        statusLabel.text = if (st.active) "ACTIVE" else "READY"
        statusLabel.setTextColor(if (st.active) ACCENT_CONNECTED else TEXT_PRIMARY)

        usbValue.text = when {
            !st.active -> "IDLE 127.0.0.1:${PacketProtocol.AUDIO_TELEMETRY_PORT}"
            st.linkState == AudioStreamService.LinkState.CONNECTED -> "CONNECTED 127.0.0.1:${PacketProtocol.AUDIO_TELEMETRY_PORT}"
            st.linkState == AudioStreamService.LinkState.ERR -> "ERR 127.0.0.1:${PacketProtocol.AUDIO_TELEMETRY_PORT}"
            else -> "WAIT 127.0.0.1:${PacketProtocol.AUDIO_TELEMETRY_PORT}"
        }
        usbValue.setTextColor(
            when (st.linkState) {
                AudioStreamService.LinkState.CONNECTED -> ACCENT_CONNECTED
                AudioStreamService.LinkState.ERR -> ACCENT_ERR
                AudioStreamService.LinkState.WAIT -> TEXT_PRIMARY
            }
        )

        audioPermValue.text = when {
            st.active && st.audioPermGranted -> "STREAMING 48KHZ"
            hasMicPerm -> "GRANTED"
            else -> "NONE"
        }

        thermalValue.text = String.format(Locale.US, "%.1fC  %d%%", thermal.tempC, thermal.batteryPct)
        thermalValue.setTextColor(if (thermal.thermalWarn) ACCENT_ERR else TEXT_PRIMARY)

        actionButton.text = if (st.active) "STOP" else "GRANT AUDIO"
        actionButton.background = sharpBox(
            SURFACE_COLOR,
            if (st.active) ACCENT_CONNECTED else BORDER_COLOR
        )
    }
}
