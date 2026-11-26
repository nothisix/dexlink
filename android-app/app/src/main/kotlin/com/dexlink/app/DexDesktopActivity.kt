package com.dexlink.app

import android.app.Activity
import android.app.ActivityOptions
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.BitmapDrawable
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.Drawable
import android.graphics.drawable.GradientDrawable
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.Editable
import android.text.TextUtils
import android.text.TextWatcher
import android.util.DisplayMetrics
import android.util.TypedValue
import android.view.Display
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.GridView
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import java.util.Locale
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors

class DexDesktopActivity : Activity() {

    private data class AppEntry(
        val label: String,
        val packageName: String,
        val className: String
    )

    companion object {
        private val BG_COLOR = Color.parseColor("#0D0D0D")
        private val SURFACE_COLOR = Color.parseColor("#161616")
        private val BORDER_COLOR = Color.parseColor("#2A2A2A")
        private val TEXT_PRIMARY = Color.parseColor("#F0F0F0")
        private val TEXT_MUTED = Color.parseColor("#7A7A7A")
        private val ACCENT_CONNECTED = Color.parseColor("#00E676")
        private val ACCENT_ERR = Color.parseColor("#FF3B30")
    }

    @Volatile
    private var allApps: List<AppEntry> = emptyList()
    private val filteredApps = mutableListOf<AppEntry>()
    private val iconCache = ConcurrentHashMap<String, Bitmap>()
    private val iconLoading = ConcurrentHashMap.newKeySet<String>()
    private val bgExecutor = Executors.newFixedThreadPool(2)

    private lateinit var gridAdapter: AppGridAdapter
    private lateinit var metaText: TextView
    private lateinit var searchInput: EditText

    private val handler = Handler(Looper.getMainLooper())
    private val statusTick = object : Runnable {
        override fun run() {
            updateMeta()
            handler.postDelayed(this, 1000L)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        window.setBackgroundDrawable(ColorDrawable(BG_COLOR))
        setContentView(buildUi())
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            onBackInvokedDispatcher.registerOnBackInvokedCallback(
                android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT
            ) {
                if (searchInput.text?.isNotEmpty() == true) {
                    searchInput.setText("")
                }
            }
        }
        updateMeta()
        loadInstalledAppsAsync()
    }

    override fun onResume() {
        super.onResume()
        handler.removeCallbacks(statusTick)
        handler.post(statusTick)
        updateMeta()
        if (allApps.isEmpty()) {
            loadInstalledAppsAsync()
        }
    }

    override fun onPause() {
        handler.removeCallbacks(statusTick)
        super.onPause()
    }

    override fun onDestroy() {
        bgExecutor.shutdownNow()
        super.onDestroy()
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        if (searchInput.text?.isNotEmpty() == true) {
            searchInput.setText("")
            return
        }
        // Keep the DeX desktop alive so pressing Back at root never leaves a blank VirtualDisplay
    }

    override fun onKeyDown(keyCode: Int, event: android.view.KeyEvent?): Boolean {
        if (keyCode == android.view.KeyEvent.KEYCODE_BACK) {
            if (searchInput.text?.isNotEmpty() == true) {
                searchInput.setText("")
            }
            return true
        }
        return super.onKeyDown(keyCode, event)
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
        }

        val topBar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setBackgroundColor(SURFACE_COLOR)
            setPadding(dp(12), dp(8), dp(12), dp(8))
        }

        val dot = View(this).apply {
            layoutParams = LinearLayout.LayoutParams(dp(7), dp(7)).apply { marginEnd = dp(8) }
            background = GradientDrawable().apply {
                shape = GradientDrawable.OVAL
                setColor(ACCENT_CONNECTED)
            }
        }

        val brand = TextView(this).apply {
            text = "DEXLINK  DESKTOP"
            setTextColor(TEXT_PRIMARY)
            textSize = 13f
            typeface = Typeface.create("sans-serif-medium", Typeface.BOLD)
            letterSpacing = 0.08f
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            ).apply { marginEnd = dp(16) }
        }

        metaText = TextView(this).apply {
            text = "DISP #0  |  1920x1080"
            setTextColor(TEXT_MUTED)
            textSize = 11f
            typeface = Typeface.MONOSPACE
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
            layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f).apply {
                marginEnd = dp(10)
            }
        }

        searchInput = EditText(this).apply {
            hint = "FILTER"
            setHintTextColor(TEXT_MUTED)
            setTextColor(TEXT_PRIMARY)
            textSize = 12f
            typeface = Typeface.MONOSPACE
            isSingleLine = true
            setPadding(dp(10), dp(4), dp(10), dp(4))
            background = sharpBox(BG_COLOR, BORDER_COLOR)
            layoutParams = LinearLayout.LayoutParams(dp(150), dp(30)).apply { marginEnd = dp(8) }
            addTextChangedListener(object : TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
                override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {
                    applyFilter(s?.toString().orEmpty())
                }
                override fun afterTextChanged(s: Editable?) {}
            })
        }

        val audioBtn = makeBarButton("AUDIO") {
            launchComponent(packageName, MainActivity::class.java.name)
        }
        val settingsBtn = makeBarButton("SETTINGS") {
            launchPackage("com.android.settings")
        }
        val reloadBtn = makeBarButton("RELOAD") {
            loadInstalledAppsAsync()
        }

        topBar.addView(dot)
        topBar.addView(brand)
        topBar.addView(metaText)
        topBar.addView(searchInput)
        topBar.addView(audioBtn)
        topBar.addView(settingsBtn)
        topBar.addView(reloadBtn)

        val borderLine = View(this).apply {
            layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(1))
            setBackgroundColor(BORDER_COLOR)
        }

        gridAdapter = AppGridAdapter()
        val grid = GridView(this).apply {
            numColumns = GridView.AUTO_FIT
            columnWidth = dp(170)
            horizontalSpacing = dp(8)
            verticalSpacing = dp(8)
            stretchMode = GridView.STRETCH_COLUMN_WIDTH
            setPadding(dp(12), dp(12), dp(12), dp(12))
            clipToPadding = false
            adapter = gridAdapter
            layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f)
            setOnItemClickListener { _, _, position, _ ->
                val item = filteredApps.getOrNull(position) ?: return@setOnItemClickListener
                launchComponent(item.packageName, item.className)
            }
        }

        root.addView(topBar)
        root.addView(borderLine)
        root.addView(grid)
        return root
    }

    private fun makeBarButton(label: String, onClick: () -> Unit): Button = Button(this).apply {
        text = label
        setTextColor(TEXT_PRIMARY)
        textSize = 11f
        typeface = Typeface.MONOSPACE
        stateListAnimator = null
        background = sharpBox(BG_COLOR, BORDER_COLOR)
        setPadding(dp(10), 0, dp(10), 0)
        layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(30)).apply {
            marginStart = dp(6)
        }
        setOnClickListener { onClick() }
    }

    private fun currentDisplayIdCompat(): Int {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            runCatching { return display?.displayId ?: Display.DEFAULT_DISPLAY }
        }
        @Suppress("DEPRECATION")
        return windowManager.defaultDisplay?.displayId ?: Display.DEFAULT_DISPLAY
    }

    private fun updateMeta() {
        val id = currentDisplayIdCompat()
        val dm = DisplayMetrics()
        @Suppress("DEPRECATION")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            display?.getRealMetrics(dm)
        } else {
            windowManager.defaultDisplay?.getRealMetrics(dm)
        }
        val thermal = TelemetryHelper.read(this)
        val audio = if (AudioStreamService.getStatus().active) "PCM ON" else "PCM OFF"
        metaText.text = String.format(
            Locale.US,
            "DISP #%d  |  %dx%d  |  %.0fC  %d%%  |  %s",
            id,
            dm.widthPixels,
            dm.heightPixels,
            thermal.tempC,
            thermal.batteryPct,
            audio
        )
        metaText.setTextColor(if (thermal.thermalWarn) ACCENT_ERR else TEXT_MUTED)
    }

    private fun loadInstalledAppsAsync() {
        bgExecutor.execute {
            val pm = packageManager
            val mainIntent = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
            @Suppress("DEPRECATION")
            val resolved = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                pm.queryIntentActivities(mainIntent, PackageManager.ResolveInfoFlags.of(0))
            } else {
                pm.queryIntentActivities(mainIntent, 0)
            }

            // Stage 1 (<30ms): publish immediately using clean package fallback names, non-system apps first
            val items = resolved
                .filter { it.activityInfo != null && it.activityInfo.packageName != packageName }
                .distinctBy { "${it.activityInfo.packageName}/${it.activityInfo.name}" }
                .sortedBy { ri ->
                    val isSys = (ri.activityInfo.applicationInfo.flags and android.content.pm.ApplicationInfo.FLAG_SYSTEM) != 0
                    (if (isSys) "1_" else "0_") + ri.activityInfo.packageName
                }

            val fastList = items.map { ri ->
                val pkg = ri.activityInfo.packageName
                val shortName = pkg.substringAfterLast('.').replaceFirstChar {
                    if (it.isLowerCase()) it.titlecase(Locale.US) else it.toString()
                }
                AppEntry(label = shortName, packageName = pkg, className = ri.activityInfo.name)
            }
            handler.post {
                allApps = fastList
                applyFilter(searchInput.text?.toString().orEmpty())
            }

            // Stage 2: resolve friendly labels in background without blocking initial desktop render
            val labeledList = items.mapIndexed { idx, ri ->
                val rawLabel = runCatching { ri.loadLabel(pm).toString().trim() }.getOrNull()
                val label = if (!rawLabel.isNullOrEmpty()) rawLabel else fastList[idx].label
                AppEntry(label = label, packageName = ri.activityInfo.packageName, className = ri.activityInfo.name)
            }.sortedBy { it.label.lowercase(Locale.US) }

            handler.post {
                allApps = labeledList
                applyFilter(searchInput.text?.toString().orEmpty())
            }
        }
    }

    private fun applyFilter(rawQuery: String) {
        val q = rawQuery.trim().lowercase(Locale.US)
        filteredApps.clear()
        val src = allApps
        if (q.isEmpty()) {
            filteredApps.addAll(src)
        } else {
            filteredApps.addAll(src.filter {
                it.label.lowercase(Locale.US).contains(q) || it.packageName.lowercase(Locale.US).contains(q)
            })
        }
        gridAdapter.notifyDataSetChanged()
    }

    private fun requestIconAsync(pkg: String) {
        if (iconCache.containsKey(pkg) || !iconLoading.add(pkg)) return
        bgExecutor.execute {
            runCatching {
                val d = packageManager.getApplicationIcon(pkg)
                val size = dp(32).coerceAtLeast(32)
                val bmp = drawableToSmallBitmap(d, size)
                iconCache[pkg] = bmp
                handler.post { gridAdapter.notifyDataSetChanged() }
            }
            iconLoading.remove(pkg)
        }
    }

    private fun drawableToSmallBitmap(d: Drawable, size: Int): Bitmap {
        if (d is BitmapDrawable && d.bitmap != null && d.bitmap.width <= size * 2) {
            return d.bitmap
        }
        val bmp = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bmp)
        d.setBounds(0, 0, size, size)
        d.draw(canvas)
        return bmp
    }

    private fun launchPackage(pkg: String) {
        val intent = packageManager.getLaunchIntentForPackage(pkg) ?: return
        startOnCurrentDisplay(intent)
    }

    private fun launchComponent(pkg: String, cls: String) {
        val intent = Intent(Intent.ACTION_MAIN).apply {
            addCategory(Intent.CATEGORY_LAUNCHER)
            setClassName(pkg, cls)
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED)
        }
        startOnCurrentDisplay(intent)
    }

    private fun startOnCurrentDisplay(intent: Intent) {
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        val displayId = currentDisplayIdCompat()
        val opts = ActivityOptions.makeBasic()
        opts.launchDisplayId = displayId
        runCatching { startActivity(intent, opts.toBundle()) }
    }

    private inner class AppGridAdapter : BaseAdapter() {
        override fun getCount(): Int = filteredApps.size
        override fun getItem(position: Int): Any = filteredApps[position]
        override fun getItemId(position: Int): Long = position.toLong()

        override fun getView(position: Int, convertView: View?, parent: ViewGroup?): View {
            val item = filteredApps[position]
            val card = (convertView as? LinearLayout) ?: LinearLayout(this@DexDesktopActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setPadding(dp(10), dp(10), dp(10), dp(10))
                background = sharpBox(SURFACE_COLOR, BORDER_COLOR)

                val iconView = ImageView(this@DexDesktopActivity).apply {
                    tag = "icon"
                    scaleType = ImageView.ScaleType.FIT_CENTER
                    background = sharpBox(BG_COLOR, BORDER_COLOR)
                    setPadding(dp(2), dp(2), dp(2), dp(2))
                    layoutParams = LinearLayout.LayoutParams(dp(32), dp(32)).apply { marginEnd = dp(10) }
                }
                val textCol = LinearLayout(this@DexDesktopActivity).apply {
                    orientation = LinearLayout.VERTICAL
                    layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
                }
                val titleView = TextView(this@DexDesktopActivity).apply {
                    tag = "title"
                    setTextColor(TEXT_PRIMARY)
                    textSize = 12f
                    typeface = Typeface.create("sans-serif-medium", Typeface.BOLD)
                    maxLines = 1
                    ellipsize = TextUtils.TruncateAt.END
                }
                val pkgView = TextView(this@DexDesktopActivity).apply {
                    tag = "pkg"
                    setTextColor(TEXT_MUTED)
                    textSize = 10f
                    typeface = Typeface.MONOSPACE
                    maxLines = 1
                    ellipsize = TextUtils.TruncateAt.END
                }
                textCol.addView(titleView)
                textCol.addView(pkgView)
                addView(iconView)
                addView(textCol)
            }

            val iv = card.findViewWithTag<ImageView>("icon")
            val cached = iconCache[item.packageName]
            if (cached != null) {
                iv.setImageBitmap(cached)
            } else {
                iv.setImageDrawable(null)
                requestIconAsync(item.packageName)
            }
            card.findViewWithTag<TextView>("title").text = item.label
            card.findViewWithTag<TextView>("pkg").text = item.packageName
            return card
        }
    }
}
