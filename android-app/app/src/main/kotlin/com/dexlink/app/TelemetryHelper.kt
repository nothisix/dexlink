package com.dexlink.app

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.os.Build
import android.os.PowerManager
import java.io.File

object TelemetryHelper {

    data class ThermalState(
        val tempC: Float,
        val batteryPct: Int,
        val charging: Boolean,
        val thermalWarn: Boolean
    )

    fun read(context: Context): ThermalState {
        var tempC = 0.0f
        var batteryPct = 100
        var charging = false

        val intent = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        if (intent != null) {
            val level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
            val scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, -1)
            if (level >= 0 && scale > 0) {
                batteryPct = ((level * 100f) / scale).toInt().coerceIn(0, 100)
            }
            val tempTenth = intent.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, -1)
            if (tempTenth > 0) {
                tempC = tempTenth / 10.0f
            }
            val status = intent.getIntExtra(BatteryManager.EXTRA_STATUS, -1)
            charging = status == BatteryManager.BATTERY_STATUS_CHARGING ||
                status == BatteryManager.BATTERY_STATUS_FULL
        }

        if (tempC <= 0.1f) {
            tempC = readSysfsThermal()
        }

        var thermalWarn = tempC >= 43.0f
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val pm = context.getSystemService(PowerManager::class.java)
            if (pm != null && pm.currentThermalStatus >= PowerManager.THERMAL_STATUS_SEVERE) {
                thermalWarn = true
            }
        }

        return ThermalState(tempC, batteryPct, charging, thermalWarn)
    }

    private fun readSysfsThermal(): Float {
        var maxC = 0.0f
        runCatching {
            File("/sys/class/thermal").listFiles { f -> f.name.startsWith("thermal_zone") }?.forEach { z ->
                val raw = File(z, "temp").takeIf { it.canRead() }?.readText()?.trim()?.toFloatOrNull()
                if (raw != null) {
                    val c = if (raw > 1000f) raw / 1000f else raw
                    if (c in 15f..110f && c > maxC) {
                        maxC = c
                    }
                }
            }
        }
        return maxC
    }
}
