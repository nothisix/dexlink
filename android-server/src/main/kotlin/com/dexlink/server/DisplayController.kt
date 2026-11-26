package com.dexlink.server

import android.annotation.SuppressLint
import android.content.Context
import android.content.Intent
import android.graphics.Point
import android.graphics.Rect
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.hardware.display.VirtualDisplayConfig
import android.os.Build
import android.os.IBinder
import android.view.Display
import android.view.Surface
import com.dexlink.protocol.PacketProtocol
import dalvik.system.PathClassLoader
import java.lang.reflect.Method

@SuppressLint("PrivateApi", "WrongConstant")
class DisplayController(private val context: Context) {

    private var virtualDisplay: VirtualDisplay? = null
    private var surfaceControlDisplayToken: IBinder? = null
    private var mirrorDisplayLayerControl: Any? = null

    var activeDisplayId: Int = Display.DEFAULT_DISPLAY
        private set

    var activeWidth: Int = 1920
        private set

    var activeHeight: Int = 1080
        private set

    var isScreenOff: Boolean = false
        private set

    fun startCapture(
        mode: Int,
        width: Int,
        height: Int,
        dpi: Int,
        fps: Int,
        encoderSurface: Surface,
        launchPackage: String?
    ) {
        stopCapture()
        activeWidth = width
        activeHeight = height

        if (mode == PacketProtocol.MODE_DEX) {
            startDexVirtualDisplay(width, height, dpi, fps, encoderSurface, launchPackage)
        } else {
            startMirrorDisplay(width, height, dpi, fps, encoderSurface)
        }
    }

    private fun startDexVirtualDisplay(
        width: Int,
        height: Int,
        dpi: Int,
        fps: Int,
        encoderSurface: Surface,
        launchPackage: String?
    ) {
        val dm = context.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
        val flagPublic = 1 shl 0
        val flagPresentation = 1 shl 1
        val flagOwnContent = 1 shl 3
        val flagSupportsTouch = 1 shl 6
        val flagRotatesWithContent = 1 shl 7
        val flagDestroyContent = 1 shl 8
        val flagShowSystemDeco = 1 shl 9
        val flagTrusted = 1 shl 10

        val baseFlags = flagPublic or flagOwnContent or flagSupportsTouch or flagRotatesWithContent or flagDestroyContent
        val flagAttempts = intArrayOf(
            baseFlags or flagShowSystemDeco or flagTrusted,
            baseFlags or flagShowSystemDeco,
            baseFlags,
            flagPublic or flagPresentation or flagOwnContent
        )

        var created: VirtualDisplay? = null
        var lastError: Throwable? = null
        for (flags in flagAttempts) {
            try {
                created = createVirtualDisplayCompat(dm, "dexlink-dex", width, height, dpi, fps, encoderSurface, flags)
                if (created != null) break
            } catch (t: Throwable) {
                lastError = t
            }
        }

        if (created == null) {
            throw IllegalStateException("Failed to create DeX VirtualDisplay", lastError)
        }

        virtualDisplay = created
        activeDisplayId = created.display.displayId
        configureVirtualDisplayIme(activeDisplayId)

        if (!launchPackage.isNullOrBlank()) {
            launchPackageOnDisplay(launchPackage, activeDisplayId)
        } else {
            launchDefaultDesktopOrHome(activeDisplayId)
        }
    }

    private fun startMirrorDisplay(
        width: Int,
        height: Int,
        dpi: Int,
        fps: Int,
        encoderSurface: Surface
    ) {
        activeDisplayId = Display.DEFAULT_DISPLAY

        // Attempt SurfaceControl.createDisplay first (fastest zero-overhead mirror on API 30-33)
        if (trySurfaceControlMirror(width, height, encoderSurface)) {
            return
        }

        // Fallback for Android 14+ (API 34+): VirtualDisplay with AUTO_MIRROR
        val dm = context.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
        val flagPublic = 1 shl 0
        val flagAutoMirror = 1 shl 4
        val flagSupportsTouch = 1 shl 6

        val attempts = intArrayOf(
            flagPublic or flagAutoMirror or flagSupportsTouch,
            flagPublic or flagAutoMirror
        )

        var created: VirtualDisplay? = null
        var lastErr: Throwable? = null
        for (flags in attempts) {
            try {
                created = createVirtualDisplayCompat(dm, "dexlink-mirror", width, height, dpi, fps, encoderSurface, flags)
                if (created != null) break
            } catch (t: Throwable) {
                lastErr = t
            }
        }

        if (created == null) {
            // Last resort fallback on locked OEM Android 14+ devices: create DeX workspace
            System.err.println("[DisplayController] AUTO_MIRROR blocked ($lastErr); falling back to VirtualDisplay workspace")
            startDexVirtualDisplay(width, height, dpi, fps, encoderSurface, null)
            return
        }
        virtualDisplay = created
    }

    private fun createVirtualDisplayCompat(
        dm: DisplayManager,
        name: String,
        width: Int,
        height: Int,
        dpi: Int,
        fps: Int,
        surface: Surface,
        flags: Int
    ): VirtualDisplay? {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            val builder = VirtualDisplayConfig.Builder(name, width, height, dpi)
                .setFlags(flags)
                .setSurface(surface)
            runCatching {
                val setRefresh = builder.javaClass.getMethod("setRequestedRefreshRate", Float::class.javaPrimitiveType)
                setRefresh.invoke(builder, fps.toFloat())
            }
            return dm.createVirtualDisplay(builder.build())
        }
        @Suppress("DEPRECATION")
        return dm.createVirtualDisplay(name, width, height, dpi, surface, flags)
    }

    private fun trySurfaceControlMirror(width: Int, height: Int, surface: Surface): Boolean {
        return runCatching {
            val scClass = Class.forName("android.view.SurfaceControl")
            val createDisplay: Method = scClass.getDeclaredMethod(
                "createDisplay",
                String::class.java,
                Boolean::class.javaPrimitiveType
            )
            val token = createDisplay.invoke(null, "dexlink-mirror", false) as IBinder

            val (srcW, srcH) = getDefaultDisplaySize()
            val srcRect = Rect(0, 0, srcW, srcH)
            val dstRect = fitRectKeepAspect(srcW, srcH, width, height)

            val openTx = scClass.getDeclaredMethod("openTransaction")
            val setDisplaySurface = scClass.getDeclaredMethod(
                "setDisplaySurface",
                IBinder::class.java,
                Surface::class.java
            )
            val setDisplayLayerStack = scClass.getDeclaredMethod(
                "setDisplayLayerStack",
                IBinder::class.java,
                Int::class.javaPrimitiveType
            )
            val setDisplayProjection = scClass.getDeclaredMethod(
                "setDisplayProjection",
                IBinder::class.java,
                Int::class.javaPrimitiveType,
                Rect::class.java,
                Rect::class.java
            )
            val closeTx = scClass.getDeclaredMethod("closeTransaction")

            openTx.invoke(null)
            try {
                setDisplaySurface.invoke(null, token, surface)
                setDisplayLayerStack.invoke(null, token, 0)
                setDisplayProjection.invoke(null, token, 0, srcRect, dstRect)
            } finally {
                closeTx.invoke(null)
            }

            surfaceControlDisplayToken = token
            true
        }.getOrElse { false }
    }

    @Suppress("DEPRECATION")
    private fun getDefaultDisplaySize(): Pair<Int, Int> {
        val dm = context.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
        val display = dm.getDisplay(Display.DEFAULT_DISPLAY)
        val pt = Point()
        display?.getRealSize(pt)
        return if (pt.x > 0 && pt.y > 0) pt.x to pt.y else 1080 to 2400
    }

    private fun fitRectKeepAspect(srcW: Int, srcH: Int, dstW: Int, dstH: Int): Rect {
        val scale = minOf(dstW.toFloat() / srcW.toFloat(), dstH.toFloat() / srcH.toFloat())
        val outW = (srcW * scale).toInt().coerceIn(1, dstW)
        val outH = (srcH * scale).toInt().coerceIn(1, dstH)
        val left = (dstW - outW) / 2
        val top = (dstH - outH) / 2
        return Rect(left, top, left + outW, top + outH)
    }

    private fun configureVirtualDisplayIme(displayId: Int) {
        runCatching {
            val wmgClass = Class.forName("android.view.WindowManagerGlobal")
            val getWms = wmgClass.getDeclaredMethod("getWindowManagerService")
            val wms = getWms.invoke(null) ?: return
            val setImePolicy = wms.javaClass.getMethod(
                "setDisplayImePolicy",
                Int::class.javaPrimitiveType,
                Int::class.javaPrimitiveType
            )
            setImePolicy.invoke(wms, displayId, 0) // DISPLAY_IME_POLICY_LOCAL
        }
    }

    fun launchPackageOnDisplay(packageName: String, displayId: Int = activeDisplayId) {
        runCatching {
            val pm = context.packageManager
            val intent = pm.getLaunchIntentForPackage(packageName)
            if (intent != null) {
                val component = intent.component
                if (component != null) {
                    val cmd = arrayOf(
                        "am", "start",
                        "--display", displayId.toString(),
                        "-n", component.flattenToShortString(),
                        "--activity-single-top"
                    )
                    Runtime.getRuntime().exec(cmd)
                    return
                }
            }
            Runtime.getRuntime().exec(
                arrayOf(
                    "am", "start",
                    "--display", displayId.toString(),
                    "-a", Intent.ACTION_MAIN,
                    "-c", Intent.CATEGORY_LAUNCHER,
                    packageName
                )
            )
        }
    }

    private fun launchDefaultDesktopOrHome(displayId: Int) {
        runCatching {
            val secondaryHomeIntent = Intent(Intent.ACTION_MAIN).addCategory("android.intent.category.SECONDARY_HOME")
            val pm = context.packageManager
            val resolved = pm.resolveActivity(secondaryHomeIntent, 0)
            if (resolved?.activityInfo != null) {
                val comp = "${resolved.activityInfo.packageName}/${resolved.activityInfo.name}"
                Runtime.getRuntime().exec(arrayOf("am", "start", "--display", displayId.toString(), "-n", comp))
                return
            }
            Runtime.getRuntime().exec(
                arrayOf(
                    "am", "start",
                    "--display", displayId.toString(),
                    "-a", Intent.ACTION_MAIN,
                    "-c", Intent.CATEGORY_HOME
                )
            )
        }
    }

    fun setScreenPowerOff(off: Boolean): Boolean {
        val powerMode = if (off) 0 else 2 // POWER_MODE_OFF=0, POWER_MODE_NORMAL=2
        val scSuccess = runCatching {
            val scClass = Class.forName("android.view.SurfaceControl")
            val setPowerMode = scClass.getDeclaredMethod(
                "setDisplayPowerMode",
                IBinder::class.java,
                Int::class.javaPrimitiveType
            )
            val tokens = getPhysicalDisplayTokens(scClass)
            if (tokens.isEmpty()) return@runCatching false
            tokens.forEach { token -> setPowerMode.invoke(null, token, powerMode) }
            true
        }.getOrElse { false }

        if (scSuccess) {
            isScreenOff = off
            return true
        }

        // Android 14+ DisplayControl in services.jar
        val dcSuccess = runCatching {
            val loader = PathClassLoader("/system/framework/services.jar", ClassLoader.getSystemClassLoader())
            val dcClass = loader.loadClass("com.android.server.display.DisplayControl")
            val scClass = Class.forName("android.view.SurfaceControl")
            val setPowerMode = scClass.getDeclaredMethod(
                "setDisplayPowerMode",
                IBinder::class.java,
                Int::class.javaPrimitiveType
            )
            val getIds = dcClass.getDeclaredMethod("getPhysicalDisplayIds")
            val getToken = dcClass.getDeclaredMethod("getPhysicalDisplayToken", Long::class.javaPrimitiveType)
            val ids = getIds.invoke(null) as? LongArray ?: return@runCatching false
            if (ids.isEmpty()) return@runCatching false
            ids.forEach { id ->
                val token = getToken.invoke(null, id) as? IBinder
                if (token != null) setPowerMode.invoke(null, token, powerMode)
            }
            true
        }.getOrElse { false }

        if (dcSuccess) {
            isScreenOff = off
            return true
        }
        return false
    }

    private fun getPhysicalDisplayTokens(scClass: Class<*>): List<IBinder> {
        val tokens = mutableListOf<IBinder>()
        runCatching {
            val getIds = scClass.getDeclaredMethod("getPhysicalDisplayIds")
            val getToken = scClass.getDeclaredMethod("getPhysicalDisplayToken", Long::class.javaPrimitiveType)
            val ids = getIds.invoke(null) as? LongArray
            if (ids != null && ids.isNotEmpty()) {
                for (id in ids) {
                    (getToken.invoke(null, id) as? IBinder)?.let { tokens.add(it) }
                }
            }
        }
        if (tokens.isEmpty()) {
            runCatching {
                val getInternal = scClass.getDeclaredMethod("getInternalDisplayToken")
                (getInternal.invoke(null) as? IBinder)?.let { tokens.add(it) }
            }
        }
        return tokens
    }

    fun stopCapture() {
        runCatching { virtualDisplay?.release() }
        virtualDisplay = null

        val token = surfaceControlDisplayToken
        surfaceControlDisplayToken = null
        if (token != null) {
            runCatching {
                val scClass = Class.forName("android.view.SurfaceControl")
                val destroyDisplay = scClass.getDeclaredMethod("destroyDisplay", IBinder::class.java)
                destroyDisplay.invoke(null, token)
            }
        }
        mirrorDisplayLayerControl = null
    }
}
