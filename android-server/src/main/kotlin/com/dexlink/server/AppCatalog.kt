package com.dexlink.server

import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.Canvas
import android.util.Base64
import java.io.BufferedOutputStream
import java.io.OutputStreamWriter
import java.nio.charset.StandardCharsets
import java.util.Locale

object AppCatalog {

    private const val ICON_SIZE = 48

    fun dumpToStdout(context: Context) {
        val pm = context.packageManager
        val mainIntent = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
        val resolved = runCatching {
            pm.queryIntentActivities(mainIntent, 0)
        }.getOrDefault(emptyList())

        val seen = HashSet<String>()
        val out = OutputStreamWriter(BufferedOutputStream(System.out, 65536), StandardCharsets.UTF_8)

        data class Entry(
            val pkg: String,
            val label: String,
            val ri: android.content.pm.ResolveInfo
        )

        val entries = mutableListOf<Entry>()
        for (ri in resolved) {
            val ai = ri.activityInfo ?: continue
            val pkg = ai.packageName ?: continue
            if (!seen.add(pkg)) continue

            val rawLabel = runCatching { ri.loadLabel(pm).toString() }.getOrNull().orEmpty()
            val cleanLabel = sanitizeField(rawLabel).ifEmpty { fallbackName(pkg) }
            entries.add(Entry(pkg, cleanLabel, ri))
        }

        // Ensure Android Settings is always available if present
        if (!seen.contains("com.android.settings")) {
            runCatching {
                val ai = pm.getApplicationInfo("com.android.settings", 0)
                val lbl = sanitizeField(pm.getApplicationLabel(ai).toString()).ifEmpty { "Settings" }
                val bmp = renderIconRgba(context, "com.android.settings", pm, null)
                out.write("DXAPP\tcom.android.settings\t$lbl\t$bmp\n")
            }
        }

        entries.sortBy { it.label.lowercase(Locale.US) }

        val limit = entries.size.coerceAtMost(140)
        for (i in 0 until limit) {
            val e = entries[i]
            val b64 = renderIconRgba(context, e.pkg, pm, e.ri)
            out.write("DXAPP\t${e.pkg}\t${e.label}\t$b64\n")
        }
        out.flush()
    }

    private fun loadBestDrawable(
        context: Context,
        pkg: String,
        pm: PackageManager,
        ri: android.content.pm.ResolveInfo?
    ): android.graphics.drawable.Drawable? {
        val ai = runCatching {
            pm.getApplicationInfo(pkg, PackageManager.GET_META_DATA)
        }.getOrNull() ?: ri?.activityInfo?.applicationInfo
        val actIcon = ri?.activityInfo?.icon ?: 0
        val appIcon = ai?.icon ?: 0
        val resIds = listOf(appIcon, actIcon).filter { it != 0 }.distinct()

        // 1. Package context with loaded theme + split APKs (try appIcon first, then activity icon)
        for (resId in resIds) {
            runCatching {
                val pkgCtx = context.createPackageContext(pkg, Context.CONTEXT_IGNORE_SECURITY)
                val d = pkgCtx.resources.getDrawableForDensity(resId, 480, pkgCtx.theme)
                    ?: pkgCtx.resources.getDrawable(resId, pkgCtx.theme)
                if (d != null) return d
            }
        }

        // 2. ApplicationResources + custom AssetManager with framework + base + split APKs + fresh theme
        if (ai != null) {
            for (resId in resIds) {
                runCatching {
                    val res = pm.getResourcesForApplication(ai)
                    val d = res.getDrawableForDensity(resId, 480, null) ?: res.getDrawable(resId, null)
                    if (d != null) return d
                }
                runCatching {
                    val am = android.content.res.AssetManager::class.java.getDeclaredConstructor().newInstance()
                    val addAssetPath = android.content.res.AssetManager::class.java.getMethod("addAssetPath", String::class.java)
                    addAssetPath.invoke(am, "/system/framework/framework-res.apk")
                    if (!ai.sourceDir.isNullOrEmpty()) {
                        addAssetPath.invoke(am, ai.sourceDir)
                    }
                    ai.splitSourceDirs?.forEach { split ->
                        if (!split.isNullOrEmpty()) addAssetPath.invoke(am, split)
                    }
                    val dm = android.util.DisplayMetrics().apply {
                        setToDefaults()
                        densityDpi = 480
                        density = 3.0f
                        scaledDensity = 3.0f
                    }
                    @Suppress("DEPRECATION")
                    val pkgRes = android.content.res.Resources(am, dm, android.content.res.Configuration())
                    val theme = pkgRes.newTheme()
                    val d = runCatching { pkgRes.getDrawableForDensity(resId, 480, theme) }.getOrNull()
                        ?: runCatching { pkgRes.getDrawable(resId, theme) }.getOrNull()
                        ?: runCatching { @Suppress("DEPRECATION") pkgRes.getDrawable(resId) }.getOrNull()
                    if (d != null) return d
                }
            }
        }

        // 3. Direct APK/split Zip fallback for PNG/WebP launcher icons (handles missing split registration)
        if (ai != null) {
            val apks = mutableListOf<String>()
            if (!ai.sourceDir.isNullOrEmpty()) apks.add(ai.sourceDir)
            ai.splitSourceDirs?.forEach { if (!it.isNullOrEmpty()) apks.add(it) }
            val candidates = listOf(
                "ic_launcher_foreground.png",
                "ic_launcher_foreground.webp",
                "ic_launcher.png",
                "ic_launcher.webp",
                "ic_launcher_round.png",
                "ic_launcher_round.webp"
            )
            var bestEntry: java.util.zip.ZipEntry? = null
            var bestApk: String? = null
            var bestScore = -1
            for (apk in apks) {
                runCatching {
                    java.util.zip.ZipFile(apk).use { zip ->
                        val entries = zip.entries()
                        while (entries.hasMoreElements()) {
                            val e = entries.nextElement()
                            val n = e.name
                            if (!n.startsWith("res/") || (!n.endsWith(".png") && !n.endsWith(".webp"))) continue
                            if (!n.contains("launcher") && !n.contains("app_icon")) continue
                            var score = 0
                            if (n.contains("xxhdpi")) score += 50
                            else if (n.contains("xhdpi")) score += 40
                            else if (n.contains("xxxhdpi")) score += 45
                            else if (n.contains("hdpi")) score += 30
                            if (n.contains("ic_launcher.")) score += 20
                            else if (n.contains("ic_launcher_round")) score += 15
                            else if (n.contains("foreground")) score += 10
                            if (candidates.any { n.endsWith(it) }) score += 10
                            if (score > bestScore) {
                                bestScore = score
                                bestEntry = e
                                bestApk = apk
                            }
                        }
                    }
                }
            }
            if (bestApk != null && bestEntry != null) {
                runCatching {
                    java.util.zip.ZipFile(bestApk).use { zip ->
                        zip.getInputStream(bestEntry).use { stream ->
                            val bmp = android.graphics.BitmapFactory.decodeStream(stream)
                            if (bmp != null) {
                                return android.graphics.drawable.BitmapDrawable(context.resources, bmp)
                            }
                        }
                    }
                }
            }
        }

        return runCatching { ri?.loadIcon(pm) }.getOrNull()
            ?: runCatching { pm.getApplicationIcon(pkg) }.getOrNull()
    }

    private fun renderIconRgba(
        context: Context,
        pkg: String,
        pm: PackageManager,
        ri: android.content.pm.ResolveInfo?
    ): String {
        return runCatching {
            val drawable = loadBestDrawable(context, pkg, pm, ri) ?: return ""

            val bmp = Bitmap.createBitmap(ICON_SIZE, ICON_SIZE, Bitmap.Config.ARGB_8888)
            val canvas = Canvas(bmp)
            drawable.setBounds(0, 0, ICON_SIZE, ICON_SIZE)
            drawable.draw(canvas)

            val pixels = IntArray(ICON_SIZE * ICON_SIZE)
            bmp.getPixels(pixels, 0, ICON_SIZE, 0, 0, ICON_SIZE, ICON_SIZE)
            bmp.recycle()

            val rgba = ByteArray(pixels.size * 4)
            var p = 0
            for (c in pixels) {
                rgba[p++] = ((c ushr 16) and 0xFF).toByte() // R
                rgba[p++] = ((c ushr 8) and 0xFF).toByte()  // G
                rgba[p++] = (c and 0xFF).toByte()           // B
                rgba[p++] = ((c ushr 24) and 0xFF).toByte() // A
            }
            Base64.encodeToString(rgba, Base64.NO_WRAP)
        }.getOrDefault("")
    }

    private fun sanitizeField(s: String): String {
        return s.replace('\t', ' ')
            .replace('\n', ' ')
            .replace('\r', ' ')
            .trim()
    }

    private fun fallbackName(pkg: String): String {
        val tail = pkg.substringAfterLast('.')
        return tail.replaceFirstChar {
            if (it.isLowerCase()) it.titlecase(Locale.US) else it.toString()
        }
    }
}
