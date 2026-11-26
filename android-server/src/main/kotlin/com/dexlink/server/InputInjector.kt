package com.dexlink.server

import android.annotation.SuppressLint
import android.os.SystemClock
import android.view.InputDevice
import android.view.InputEvent
import android.view.KeyCharacterMap
import android.view.KeyEvent
import android.view.MotionEvent
import com.dexlink.protocol.PacketProtocol
import java.lang.reflect.Method

@SuppressLint("PrivateApi")
class InputInjector {

    companion object {
        private const val MAX_POINTERS = 16
        private const val INJECT_MODE_ASYNC = 0
    }

    private data class PointerState(
        var active: Boolean = false,
        var id: Int = 0,
        var x: Float = 0f,
        var y: Float = 0f,
        var pressure: Float = 1f
    )

    private val pointers = Array(MAX_POINTERS) { PointerState() }
    private var activeCount = 0
    private var touchDownTimeMs = 0L

    private val pointerProps = Array(MAX_POINTERS) { MotionEvent.PointerProperties() }
    private val pointerCoords = Array(MAX_POINTERS) { MotionEvent.PointerCoords() }

    private val inputManagerObj: Any
    private val injectMethod: Method
    private val setDisplayIdMethod: Method? = runCatching {
        InputEvent::class.java.getMethod("setDisplayId", Int::class.javaPrimitiveType)
    }.getOrNull()

    init {
        val (imObj, method) = resolveInputManager()
        inputManagerObj = imObj
        injectMethod = method
    }

    private fun resolveInputManager(): Pair<Any, Method> {
        runCatching {
            val imgClass = Class.forName("android.hardware.input.InputManagerGlobal")
            val getInst = imgClass.getDeclaredMethod("getInstance")
            val inst = getInst.invoke(null)!!
            val inject = imgClass.getMethod("injectInputEvent", InputEvent::class.java, Int::class.javaPrimitiveType)
            return inst to inject
        }
        val imClass = Class.forName("android.hardware.input.InputManager")
        val getInst = imClass.getDeclaredMethod("getInstance")
        val inst = getInst.invoke(null)!!
        val inject = imClass.getMethod("injectInputEvent", InputEvent::class.java, Int::class.javaPrimitiveType)
        return inst to inject
    }

    @Synchronized
    fun injectTouch(ev: PacketProtocol.TouchEvent, width: Int, height: Int, displayId: Int) {
        val now = SystemClock.uptimeMillis()
        val px = (ev.normX.coerceIn(0f, 1f) * width.coerceAtLeast(1))
        val py = (ev.normY.coerceIn(0f, 1f) * height.coerceAtLeast(1))
        val pressure = ev.pressure.coerceIn(0f, 1f)

        if (ev.action == PacketProtocol.TOUCH_HOVER_MOVE) {
            pointerProps[0].clear()
            pointerProps[0].id = 0
            pointerProps[0].toolType = MotionEvent.TOOL_TYPE_MOUSE
            pointerCoords[0].clear()
            pointerCoords[0].x = px
            pointerCoords[0].y = py
            pointerCoords[0].pressure = 0f
            pointerCoords[0].size = 1f

            val motion = obtainMotionEvent(
                downTime = now,
                eventTime = now,
                action = MotionEvent.ACTION_HOVER_MOVE,
                pointerCount = 1,
                source = InputDevice.SOURCE_MOUSE,
                displayId = displayId
            )
            injectEvent(motion, displayId)
            motion.recycle()
            return
        }

        val slotIndex = findOrAllocSlot(ev.pointerId, ev.action)
        if (slotIndex < 0) return

        var actionToInject = MotionEvent.ACTION_MOVE
        var removeAfterInject = false

        when (ev.action) {
            PacketProtocol.TOUCH_DOWN -> {
                if (!pointers[slotIndex].active) {
                    pointers[slotIndex].active = true
                    pointers[slotIndex].id = ev.pointerId
                    pointers[slotIndex].x = px
                    pointers[slotIndex].y = py
                    pointers[slotIndex].pressure = pressure
                    if (activeCount == 0) {
                        touchDownTimeMs = now
                        activeCount = 1
                        actionToInject = MotionEvent.ACTION_DOWN
                    } else {
                        activeCount++
                        val compactIdx = compactActiveIndex(slotIndex)
                        actionToInject = MotionEvent.ACTION_POINTER_DOWN or
                            (compactIdx shl MotionEvent.ACTION_POINTER_INDEX_SHIFT)
                    }
                } else {
                    pointers[slotIndex].x = px
                    pointers[slotIndex].y = py
                    pointers[slotIndex].pressure = pressure
                    actionToInject = MotionEvent.ACTION_MOVE
                }
            }

            PacketProtocol.TOUCH_MOVE -> {
                if (!pointers[slotIndex].active) return
                pointers[slotIndex].x = px
                pointers[slotIndex].y = py
                pointers[slotIndex].pressure = pressure
                actionToInject = MotionEvent.ACTION_MOVE
            }

            PacketProtocol.TOUCH_UP, PacketProtocol.TOUCH_CANCEL -> {
                if (!pointers[slotIndex].active) return
                pointers[slotIndex].x = px
                pointers[slotIndex].y = py
                pointers[slotIndex].pressure = 0f
                removeAfterInject = true
                if (activeCount <= 1) {
                    actionToInject = if (ev.action == PacketProtocol.TOUCH_CANCEL) {
                        MotionEvent.ACTION_CANCEL
                    } else {
                        MotionEvent.ACTION_UP
                    }
                } else {
                    val compactIdx = compactActiveIndex(slotIndex)
                    actionToInject = MotionEvent.ACTION_POINTER_UP or
                        (compactIdx shl MotionEvent.ACTION_POINTER_INDEX_SHIFT)
                }
            }
            else -> return
        }

        val count = fillPointerArrays()
        if (count > 0) {
            val motion = obtainMotionEvent(
                downTime = touchDownTimeMs,
                eventTime = now,
                action = actionToInject,
                pointerCount = count,
                source = InputDevice.SOURCE_TOUCHSCREEN,
                displayId = displayId
            )
            injectEvent(motion, displayId)
            motion.recycle()
        }

        if (removeAfterInject) {
            pointers[slotIndex].active = false
            activeCount = (activeCount - 1).coerceAtLeast(0)
        }
    }

    @Synchronized
    fun clearAllPointers(displayId: Int) {
        for (i in pointers.indices) {
            if (pointers[i].active) {
                injectTouch(
                    PacketProtocol.TouchEvent(pointers[i].id, PacketProtocol.TOUCH_CANCEL, 0.5f, 0.5f, 0f),
                    1920,
                    1080,
                    displayId
                )
            }
        }
    }

    fun injectKey(ev: PacketProtocol.KeyEventPacket, displayId: Int) {
        val now = SystemClock.uptimeMillis()
        val action = if (ev.action == PacketProtocol.KEY_DOWN) KeyEvent.ACTION_DOWN else KeyEvent.ACTION_UP
        val keyEvent = KeyEvent(
            now,
            now,
            action,
            ev.keycode,
            0,
            ev.metaState,
            KeyCharacterMap.VIRTUAL_KEYBOARD,
            0,
            KeyEvent.FLAG_FROM_SYSTEM,
            InputDevice.SOURCE_KEYBOARD
        )
        injectEvent(keyEvent, displayId)
    }

    fun injectNavKey(keycode: Int, displayId: Int) {
        val now = SystemClock.uptimeMillis()
        val down = KeyEvent(
            now, now, KeyEvent.ACTION_DOWN, keycode, 0, 0,
            KeyCharacterMap.VIRTUAL_KEYBOARD, 0, KeyEvent.FLAG_FROM_SYSTEM, InputDevice.SOURCE_KEYBOARD
        )
        val up = KeyEvent(
            now, now, KeyEvent.ACTION_UP, keycode, 0, 0,
            KeyCharacterMap.VIRTUAL_KEYBOARD, 0, KeyEvent.FLAG_FROM_SYSTEM, InputDevice.SOURCE_KEYBOARD
        )
        injectEvent(down, displayId)
        injectEvent(up, displayId)
    }

    private fun findOrAllocSlot(pointerId: Int, action: Int): Int {
        for (i in pointers.indices) {
            if (pointers[i].active && pointers[i].id == pointerId) {
                return i
            }
        }
        if (action == PacketProtocol.TOUCH_DOWN) {
            for (i in pointers.indices) {
                if (!pointers[i].active) return i
            }
        }
        return -1
    }

    private fun compactActiveIndex(slotIndex: Int): Int {
        var idx = 0
        for (i in 0 until slotIndex) {
            if (pointers[i].active) idx++
        }
        return idx
    }

    private fun fillPointerArrays(): Int {
        var count = 0
        for (i in pointers.indices) {
            val p = pointers[i]
            if (!p.active) continue
            pointerProps[count].clear()
            pointerProps[count].id = p.id
            pointerProps[count].toolType = MotionEvent.TOOL_TYPE_FINGER
            pointerCoords[count].clear()
            pointerCoords[count].x = p.x
            pointerCoords[count].y = p.y
            pointerCoords[count].pressure = p.pressure
            pointerCoords[count].size = 1f
            count++
        }
        return count
    }

    @Suppress("DEPRECATION")
    private fun obtainMotionEvent(
        downTime: Long,
        eventTime: Long,
        action: Int,
        pointerCount: Int,
        source: Int,
        displayId: Int
    ): MotionEvent {
        return MotionEvent.obtain(
            downTime,
            eventTime,
            action,
            pointerCount,
            pointerProps,
            pointerCoords,
            0,
            0,
            1.0f,
            1.0f,
            0,
            0,
            source,
            displayId,
            0,
            0
        )!!
    }

    private fun injectEvent(event: InputEvent, displayId: Int) {
        runCatching {
            setDisplayIdMethod?.invoke(event, displayId)
            injectMethod.invoke(inputManagerObj, event, INJECT_MODE_ASYNC)
        }
    }
}
