package com.aerolon.ncv

import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import kotlin.math.roundToInt

internal object NcvUi {
    val BG = 0xFF080E1B.toInt()
    val SURFACE = 0xFF111B2E.toInt()
    val SURFACE_2 = 0xFF17243A.toInt()
    val BORDER = 0xFF263752.toInt()
    val TEXT = 0xFFF2F6FC.toInt()
    val MUTED = 0xFF9CAFC5.toInt()
    val CYAN = 0xFF7CDBE8.toInt()
    val BLUE = 0xFF748BFF.toInt()
    val GREEN = 0xFF75E0B0.toInt()
    val AMBER = 0xFFFFC879.toInt()
    val RED = 0xFFFF8C9B.toInt()

    fun dp(context: Context, value: Float): Int = (value * context.resources.displayMetrics.density).roundToInt()

    fun text(
        context: Context,
        value: CharSequence,
        sizeSp: Float = 14f,
        color: Int = TEXT,
        bold: Boolean = false,
        mono: Boolean = false
    ): TextView = TextView(context).apply {
        text = value
        textSize = sizeSp
        setTextColor(color)
        if (bold) setTypeface(typeface, Typeface.BOLD)
        if (mono) typeface = Typeface.create("monospace", Typeface.NORMAL)
        includeFontPadding = false
        gravity = Gravity.START or Gravity.CENTER_VERTICAL
        setLineSpacing(dp(context, 2f).toFloat(), 1.02f)
    }

    fun vertical(context: Context, spacingDp: Int = 0): LinearLayout = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        if (spacingDp > 0) showDividers = LinearLayout.SHOW_DIVIDER_NONE
    }

    fun horizontal(context: Context): LinearLayout = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
    }

    fun card(context: Context, paddingDp: Int = 18): LinearLayout = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(dp(context, paddingDp.toFloat()), dp(context, paddingDp.toFloat()), dp(context, paddingDp.toFloat()), dp(context, paddingDp.toFloat()))
        background = rounded(SURFACE, dp(context, 22f), BORDER, dp(context, 1f))
    }

    fun rounded(fill: Int, radiusPx: Int, stroke: Int? = null, strokePx: Int = 0): GradientDrawable = GradientDrawable().apply {
        shape = GradientDrawable.RECTANGLE
        setColor(fill)
        cornerRadius = radiusPx.toFloat()
        if (stroke != null && strokePx > 0) setStroke(strokePx, stroke)
    }

    fun gradient(context: Context, top: Int, bottom: Int, radiusDp: Float = 24f): GradientDrawable =
        GradientDrawable(GradientDrawable.Orientation.TL_BR, intArrayOf(top, bottom)).apply {
            cornerRadius = dp(context, radiusDp).toFloat()
        }

    fun pill(context: Context, value: String, color: Int = CYAN, fill: Int = 0x1D7CDBE8) : TextView = text(context, value.uppercase(), 10f, color, true).apply {
        setPadding(dp(context, 10f), dp(context, 7f), dp(context, 10f), dp(context, 7f))
        background = rounded(fill, dp(context, 40f))
        letterSpacing = 0.07f
    }

    fun button(context: Context, label: String, primary: Boolean = false, enabled: Boolean = true, onClick: () -> Unit): Button = Button(context).apply {
        text = label
        textSize = 14f
        isAllCaps = false
        setTextColor(if (primary) BG else TEXT)
        typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
        minHeight = dp(context, 52f)
        minimumHeight = dp(context, 52f)
        setPadding(dp(context, 18f), dp(context, 10f), dp(context, 18f), dp(context, 10f))
        background = if (primary) gradient(context, CYAN, 0xFF9AE8D3.toInt(), 16f) else rounded(SURFACE_2, dp(context, 16f), BORDER, dp(context, 1f))
        alpha = if (enabled) 1f else 0.42f
        isEnabled = enabled
        setOnClickListener { if (isEnabled) onClick() }
    }

    fun divider(context: Context, color: Int = BORDER): View = View(context).apply {
        setBackgroundColor(color)
        layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(context, 1f))
    }

    fun spacer(context: Context, heightDp: Int): View = View(context).apply {
        layoutParams = LinearLayout.LayoutParams(1, dp(context, heightDp.toFloat()))
    }

    fun rowSpacer(context: Context, widthDp: Int): View = View(context).apply {
        layoutParams = LinearLayout.LayoutParams(dp(context, widthDp.toFloat()), 1)
    }

    fun formatBytes(bytes: Long?): String = if (bytes == null || bytes < 0) "Size unavailable" else ImageInspector.formatBytes(bytes)
}
