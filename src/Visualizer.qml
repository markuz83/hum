import QtQuick

// Bars, a mirrored skyline, or a line, drawn from the analyzer's levels.
Canvas {
    id: canvas

    property var levels: []
    property int style: 0
    property color color: "#FFD60A"
    property int radius: 0

    onLevelsChanged: requestPaint()
    onStyleChanged: requestPaint()
    onColorChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()

    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        const n = levels.length
        if (n === 0)
            return
        const w = width, h = height
        const gap = Math.max(2, w / n * 0.25)
        const bar = (w - gap * (n - 1)) / n
        const r = Math.min(bar / 2, canvas.radius)
        ctx.fillStyle = color
        ctx.strokeStyle = color

        if (style === 2) {
            const step = w / (n - 1)
            ctx.beginPath()
            ctx.moveTo(0, h - levels[0] * h)
            for (let i = 1; i < n; ++i)
                ctx.lineTo(i * step, h - levels[i] * h)
            ctx.lineWidth = 3
            ctx.lineJoin = "round"
            ctx.stroke()
            ctx.lineTo(w, h)
            ctx.lineTo(0, h)
            ctx.closePath()
            ctx.globalAlpha = 0.15
            ctx.fill()
            return
        }

        for (let i = 0; i < n; ++i) {
            const level = levels[i]
            const barHeight = Math.max(3, level * h)
            const x = i * (bar + gap)
            const y = style === 1 ? (h - barHeight) / 2 : h - barHeight
            ctx.globalAlpha = 0.35 + 0.65 * level
            ctx.beginPath()
            ctx.roundedRect(x, y, bar, barHeight, r, r)
            ctx.fill()
        }
    }
}
