package com.dexlink.app

import android.app.Activity
import android.app.ActivityOptions
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ResolveInfo
import android.content.res.ColorStateList
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.ColorFilter
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PixelFormat
import android.graphics.drawable.BitmapDrawable
import android.graphics.drawable.Drawable
import android.graphics.drawable.RippleDrawable
import android.graphics.drawable.ShapeDrawable
import android.graphics.drawable.StateListDrawable
import android.graphics.drawable.shapes.RoundRectShape
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.TextUtils
import android.util.TypedValue
import android.view.Display
import android.view.Gravity
import android.view.KeyEvent
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.widget.FrameLayout
import android.widget.GridLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import java.util.Locale
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors

class DexDesktopActivity : Activity() {

    private data class DesktopShortcut(
        val label: String,
        val packageName: String,
        val className: String
    )

    companion object {
        private val BG_BASE = Color.parseColor("#0B0D12")
        private val PLANE_DARK = Color.parseColor("#10141D")
        private val PLANE_MID = Color.parseColor("#141925")
        private val PLANE_LIGHT = Color.parseColor("#192030")
        private val HAIRLINE = Color.parseColor("#232C3F")
        private val TEXT_PRIMARY = Color.parseColor("#F0F0F0")
        private val HOVER_FILL = Color.argb(38, 255, 255, 255)
        private val PRESS_FILL = Color.argb(64, 255, 255, 255)

        private const val MAX_DESKTOP_SHORTCUTS = 8
        private val PINNED_ORDER = listOf(
            "com.android.chrome",
            "com.android.vending",
            "com.android.settings",
            "com.google.android.apps.nbu.files",
            "com.android.documentsui",
            "com.google.android.apps.photos",
            "com.google.android.youtube",
            "com.discord",
            "com.tencent.ig",
            "com.mobilelegends.mi",
            "com.miHoYo.GenshinImpact"
        )
    }

    private val iconCache = ConcurrentHashMap<String, Bitmap>()
    private val bgExecutor = Executors.newSingleThreadExecutor()
    private val handler = Handler(Looper.getMainLooper())

    private lateinit var shortcutGrid: GridLayout

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        window.setBackgroundDrawable(MinimalDexWallpaperDrawable())
        setContentView(buildOpenDesktop())
        applyImmersiveSystemBars()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            onBackInvokedDispatcher.registerOnBackInvokedCallback(
                android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT
            ) {
                // Keep open DeX desktop alive
            }
        }

        loadDesktopShortcutsAsync()
    }

    override fun onResume() {
        super.onResume()
        applyImmersiveSystemBars()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            applyImmersiveSystemBars()
        }
    }

    override fun onDestroy() {
        bgExecutor.shutdownNow()
        super.onDestroy()
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        // Keep the DeX desktop alive so pressing Back at root never leaves a blank VirtualDisplay
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    @Suppress("DEPRECATION")
    private fun applyImmersiveSystemBars() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false)
            window.insetsController?.let { controller ->
                controller.hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                controller.systemBarsBehavior =
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            window.decorView.systemUiVisibility = (
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                )
        }
    }

    private fun dp(v: Int): Int =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v.toFloat(), resources.displayMetrics).toInt()

    private fun buildOpenDesktop(): View {
        val canvas = FrameLayout(this).apply {
            background = MinimalDexWallpaperDrawable()
        }

        // Sparse column-major desktop shortcuts on the left edge, leaving 85%+ open canvas
        shortcutGrid = GridLayout(this).apply {
            orientation = GridLayout.VERTICAL
            rowCount = 4
            columnCount = 2
            val padH = dp(24)
            val padV = dp(22)
            setPadding(padH, padV, padH, padV)
            layoutParams = FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.START
            )
        }

        canvas.addView(shortcutGrid)
        return canvas
    }

    private fun loadDesktopShortcutsAsync() {
        bgExecutor.execute {
            val pm = packageManager
            val mainIntent = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
            @Suppress("DEPRECATION")
            val resolved = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                pm.queryIntentActivities(mainIntent, PackageManager.ResolveInfoFlags.of(0))
            } else {
                pm.queryIntentActivities(mainIntent, 0)
            }

            val byPackage = LinkedHashMap<String, ResolveInfo>()
            for (ri in resolved) {
                val ai = ri.activityInfo ?: continue
                if (ai.packageName == packageName) continue
                if (!byPackage.containsKey(ai.packageName)) {
                    byPackage[ai.packageName] = ri
                }
            }

            val chosen = mutableListOf<DesktopShortcut>()
            val usedPkgs = HashSet<String>()

            fun addResolveInfo(ri: ResolveInfo) {
                val ai = ri.activityInfo ?: return
                if (!usedPkgs.add(ai.packageName)) return
                val rawLabel = runCatching { ri.loadLabel(pm).toString().trim() }.getOrNull().orEmpty()
                val label = if (rawLabel.isNotEmpty()) {
                    rawLabel
                } else {
                    ai.packageName.substringAfterLast('.').replaceFirstChar {
                        if (it.isLowerCase()) it.titlecase(Locale.US) else it.toString()
                    }
                }
                chosen.add(DesktopShortcut(label = label, packageName = ai.packageName, className = ai.name))
                loadIconSync(ai.packageName, ri)
            }

            for (pkg in PINNED_ORDER) {
                if (chosen.size >= MAX_DESKTOP_SHORTCUTS) break
                byPackage[pkg]?.let { addResolveInfo(it) }
            }

            if (chosen.size < MAX_DESKTOP_SHORTCUTS) {
                val userApps = byPackage.values.filter { ri ->
                    val flags = ri.activityInfo.applicationInfo.flags
                    (flags and android.content.pm.ApplicationInfo.FLAG_SYSTEM) == 0
                }
                for (ri in userApps) {
                    if (chosen.size >= MAX_DESKTOP_SHORTCUTS) break
                    addResolveInfo(ri)
                }
            }

            // Include Audio Link shortcut if desktop still has room
            if (chosen.size < MAX_DESKTOP_SHORTCUTS) {
                runCatching {
                    val d = pm.getApplicationIcon(packageName)
                    iconCache[packageName] = drawableToBitmap(d, dp(48).coerceAtLeast(48))
                }
                chosen.add(DesktopShortcut("Audio Link", packageName, MainActivity::class.java.name))
            }

            handler.post {
                renderShortcuts(chosen)
            }
        }
    }

    private fun loadIconSync(pkg: String, ri: ResolveInfo) {
        if (iconCache.containsKey(pkg)) return
        runCatching {
            val d = runCatching { packageManager.getApplicationIcon(pkg) }
                .getOrElse { ri.loadIcon(packageManager) }
            val size = dp(48).coerceAtLeast(48)
            iconCache[pkg] = drawableToBitmap(d, size)
        }
    }

    private fun drawableToBitmap(d: Drawable, size: Int): Bitmap {
        if (d is BitmapDrawable && d.bitmap != null &&
            d.bitmap.width in (size / 2)..(size * 3) &&
            d.bitmap.height in (size / 2)..(size * 3)
        ) {
            return d.bitmap
        }
        val bmp = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bmp)
        d.setBounds(0, 0, size, size)
        d.draw(canvas)
        return bmp
    }

    private fun shortcutTouchFeedback(): Drawable {
        val r = dp(8).toFloat()
        val radii = floatArrayOf(r, r, r, r, r, r, r, r)
        val hoverShape = ShapeDrawable(RoundRectShape(radii, null, null)).apply {
            paint.color = HOVER_FILL
        }
        val stateBg = StateListDrawable().apply {
            addState(intArrayOf(android.R.attr.state_hovered), hoverShape)
            addState(intArrayOf(android.R.attr.state_focused), hoverShape)
        }
        val mask = ShapeDrawable(RoundRectShape(radii, null, null)).apply {
            paint.color = Color.WHITE
        }
        return RippleDrawable(ColorStateList.valueOf(PRESS_FILL), stateBg, mask)
    }

    private fun renderShortcuts(shortcuts: List<DesktopShortcut>) {
        shortcutGrid.removeAllViews()
        for (item in shortcuts) {
            val cell = LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                gravity = Gravity.CENTER_HORIZONTAL
                background = shortcutTouchFeedback()
                isClickable = true
                isFocusable = true
                setPadding(dp(8), dp(10), dp(8), dp(10))
                layoutParams = GridLayout.LayoutParams().apply {
                    width = dp(92)
                    height = dp(96)
                    setMargins(dp(4), dp(4), dp(12), dp(8))
                }
                setOnClickListener {
                    launchComponent(item.packageName, item.className)
                }
            }

            val iconView = ImageView(this).apply {
                scaleType = ImageView.ScaleType.FIT_CENTER
                layoutParams = LinearLayout.LayoutParams(dp(48), dp(48))
                iconCache[item.packageName]?.let { setImageBitmap(it) }
            }

            val labelView = TextView(this).apply {
                text = item.label
                setTextColor(TEXT_PRIMARY)
                textSize = 12f
                gravity = Gravity.CENTER
                isSingleLine = true
                maxLines = 1
                ellipsize = TextUtils.TruncateAt.END
                setShadowLayer(4f, 0f, 1f, Color.argb(210, 0, 0, 0))
                layoutParams = LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT
                ).apply {
                    topMargin = dp(6)
                }
            }

            cell.addView(iconView)
            cell.addView(labelView)
            shortcutGrid.addView(cell)
        }
    }

    private fun currentDisplayIdCompat(): Int {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            runCatching { return display?.displayId ?: Display.DEFAULT_DISPLAY }
        }
        @Suppress("DEPRECATION")
        return windowManager.defaultDisplay?.displayId ?: Display.DEFAULT_DISPLAY
    }

    private fun launchComponent(pkg: String, cls: String) {
        val intent = Intent(Intent.ACTION_MAIN).apply {
            addCategory(Intent.CATEGORY_LAUNCHER)
            setClassName(pkg, cls)
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED)
        }
        val opts = ActivityOptions.makeBasic()
        opts.launchDisplayId = currentDisplayIdCompat()
        runCatching { startActivity(intent, opts.toBundle()) }
    }

    private class MinimalDexWallpaperDrawable : Drawable() {
        private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
        private val path = Path()

        override fun draw(canvas: Canvas) {
            val b = bounds
            val w = b.width().toFloat()
            val h = b.height().toFloat()
            if (w <= 0f || h <= 0f) return

            paint.style = Paint.Style.FILL
            paint.color = BG_BASE
            canvas.drawRect(b, paint)

            // Subtle minimalist geometric obsidian facets (no gradients, no neon)
            path.reset()
            path.moveTo(w * 0.34f, 0f)
            path.lineTo(w, 0f)
            path.lineTo(w, h)
            path.lineTo(w * 0.58f, h)
            path.close()
            paint.color = PLANE_DARK
            canvas.drawPath(path, paint)

            path.reset()
            path.moveTo(w * 0.56f, 0f)
            path.lineTo(w, 0f)
            path.lineTo(w, h * 0.72f)
            path.lineTo(w * 0.42f, h * 0.46f)
            path.close()
            paint.color = PLANE_MID
            canvas.drawPath(path, paint)

            path.reset()
            path.moveTo(w * 0.64f, h * 0.22f)
            path.lineTo(w, h * 0.08f)
            path.lineTo(w, h * 0.52f)
            path.lineTo(w * 0.72f, h * 0.44f)
            path.close()
            paint.color = PLANE_LIGHT
            canvas.drawPath(path, paint)

            paint.style = Paint.Style.STROKE
            paint.strokeWidth = 1.2f
            paint.color = HAIRLINE
            canvas.drawLine(w * 0.34f, 0f, w * 0.58f, h, paint)
            canvas.drawLine(w * 0.42f, h * 0.46f, w, h * 0.72f, paint)
        }

        override fun setAlpha(alpha: Int) {}
        override fun setColorFilter(colorFilter: ColorFilter?) {}
        @Deprecated("Deprecated in Java")
        override fun getOpacity(): Int = PixelFormat.OPAQUE
    }
}
