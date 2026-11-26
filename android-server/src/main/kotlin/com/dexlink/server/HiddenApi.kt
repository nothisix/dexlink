package com.dexlink.server

import android.annotation.SuppressLint
import android.content.AttributionSource
import android.content.Context
import android.content.ContextWrapper
import android.content.pm.PackageManager
import android.os.Build
import android.os.IBinder
import android.os.Looper
import android.os.Process
import java.lang.reflect.Method

@SuppressLint("PrivateApi", "BlockedPrivateApi", "SoonBlockedPrivateApi", "DiscouragedPrivateApi")
object HiddenApi {
    private var prepared = false

    fun prepare() {
        if (prepared) return
        prepared = true
        bypassHiddenApiRestrictions()
        prepareLooper()
    }

    private fun bypassHiddenApiRestrictions() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) return
        runCatching {
            val forName = Class::class.java.getDeclaredMethod("forName", String::class.java)
            val classArrayType = emptyArray<Class<*>>()::class.java
            val stringArrayType = emptyArray<String>()::class.java
            val getDeclaredMethod = Class::class.java.getDeclaredMethod(
                "getDeclaredMethod",
                String::class.java,
                classArrayType
            )
            val vmRuntimeClass = forName.invoke(null, "dalvik.system.VMRuntime") as Class<*>
            val getRuntime = getDeclaredMethod.invoke(vmRuntimeClass, "getRuntime", emptyArray<Class<*>>()) as Method
            val setHiddenApiExemptions = getDeclaredMethod.invoke(
                vmRuntimeClass,
                "setHiddenApiExemptions",
                arrayOf(stringArrayType)
            ) as Method
            val runtime = getRuntime.invoke(null)
            setHiddenApiExemptions.invoke(runtime, arrayOf("L"))
        }
    }

    @Suppress("DEPRECATION")
    private fun prepareLooper() {
        runCatching {
            if (Looper.getMainLooper() == null) {
                Looper.prepareMainLooper()
            }
        }
    }

    fun getService(name: String): IBinder? = runCatching {
        val smClass = Class.forName("android.os.ServiceManager")
        val getServiceMethod = smClass.getDeclaredMethod("getService", String::class.java)
        getServiceMethod.invoke(null, name) as? IBinder
    }.getOrNull()

    fun createShellContext(): Context {
        System.err.println("[DEXLINK] createShellContext: ActivityThread()")
        val baseContext = runCatching {
            val atClass = Class.forName("android.app.ActivityThread")
            val ctor = atClass.getDeclaredConstructor()
            ctor.isAccessible = true
            val thread = ctor.newInstance()
            runCatching {
                val field = atClass.getDeclaredField("sCurrentActivityThread")
                field.isAccessible = true
                field.set(null, thread)
            }
            System.err.println("[DEXLINK] createShellContext: getSystemContext()")
            val systemCtxMethod = atClass.getDeclaredMethod("getSystemContext")
            systemCtxMethod.isAccessible = true
            systemCtxMethod.invoke(thread) as Context
        }.getOrElse { err ->
            System.err.println("[DEXLINK] createShellContext fallback: $err")
            runCatching {
                val atClass = Class.forName("android.app.ActivityThread")
                val systemMain = atClass.getDeclaredMethod("systemMain")
                systemMain.isAccessible = true
                val thread = systemMain.invoke(null)
                val systemCtxMethod = atClass.getDeclaredMethod("getSystemContext")
                systemCtxMethod.isAccessible = true
                systemCtxMethod.invoke(thread) as Context
            }.getOrThrow()
        }
        System.err.println("[DEXLINK] createShellContext: ok")
        val shellPkgCtx = runCatching {
            baseContext.createPackageContext("com.android.shell", Context.CONTEXT_IGNORE_SECURITY)
        }.getOrElse { baseContext }
        return ShellFakeContext(shellPkgCtx)
    }

    private class ShellFakeContext(base: Context) : ContextWrapper(base) {
        private val localDisplayManager: Any? by lazy {
            runCatching {
                val dmClass = Class.forName("android.hardware.display.DisplayManager")
                val ctor = dmClass.getDeclaredConstructor(Context::class.java)
                ctor.isAccessible = true
                ctor.newInstance(this)
            }.getOrNull()
        }

        override fun getPackageName(): String = "com.android.shell"

        override fun getOpPackageName(): String = "com.android.shell"

        @Suppress("unused")
        fun getBasePackageName(): String = "com.android.shell"

        override fun getApplicationContext(): Context = this

        override fun createPackageContext(packageName: String?, flags: Int): Context = this

        override fun getSystemService(name: String): Any? {
            if (name == Context.DISPLAY_SERVICE) {
                localDisplayManager?.let { return it }
            }
            return super.getSystemService(name)
        }

        override fun getAttributionSource(): AttributionSource {
            return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                AttributionSource.Builder(Process.myUid())
                    .setPackageName("com.android.shell")
                    .build()
            } else {
                super.getAttributionSource()
            }
        }

        override fun checkCallingOrSelfPermission(permission: String): Int =
            PackageManager.PERMISSION_GRANTED
    }
}
