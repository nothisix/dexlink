package com.dexlink.server

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import java.io.File

class TelemetryReader(private val context: Context) {

    data class Snapshot(
        val tempC: Float,
        val batteryPct: Int,
        val charging: Boolean
    )

    fun read(): Snapshot {
        var tempC = 0.0f
        var batteryPct = 100
        var charging = false

        runCatching {
            val batteryIntent = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
            if (batteryIntent != null) {
                val level = batteryIntent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
                val scale = batteryIntent.getIntExtra(BatteryManager.EXTRA_SCALE, -1)
                if (level >= 0 && scale > 0) {
                    batteryPct = ((level * 100f) / scale).toInt().coerceIn(0, 100)
                }
                val tempTenth = batteryIntent.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, -1)
                if (tempTenth > 0) {
                    tempC = tempTenth / 10.0f
                }
                val status = batteryIntent.getIntExtra(BatteryManager.EXTRA_STATUS, -1)
                charging = status == BatteryManager.BATTERY_STATUS_CHARGING ||
                    status == BatteryManager.BATTERY_STATUS_FULL
            }
        }

        if (tempC <= 0.1f) {
            tempC = readThermalSysfs()
        }

        return Snapshot(tempC, batteryPct, charging)
    }

    private fun readThermalSysfs(): Float {
        var maxTemp = 0.0f
        runCatching {
            val thermalDir = File("/sys/class/thermal")
            thermalDir.listFiles { f -> f.name.startsWith("thermal_zone") }?.forEach { zone ->
                val raw = File(zone, "temp").takeIf { it.canRead() }?.readText()?.trim()?.toFloatOrNull()
                if (raw != null) {
                    val c = if (raw > 1000f) raw / 1000f else raw
                    if (c in 15f..110f && c > maxTemp) {
                        maxTemp = c
                    }
                }
            }
        }
        return maxTemp
    }
}
