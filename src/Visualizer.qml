import QtQuick
import QtQuick.Shapes

// Bars, a mirrored skyline, or a line, drawn from the analyzer's levels. These
// are scene-graph items, so the GPU draws them; a Canvas would rasterize every
// frame on the CPU and upload it as a texture, which cannot keep up at 60 fps.
Item {
    id: root

    property var levels: []
    property int style: 0
    property color color: "#FFD60A"
    property int radius: 0

    readonly property int count: levels.length
    readonly property real gap: Math.max(2, width / Math.max(1, count) * 0.25)
    readonly property real barWidth: (width - gap * (count - 1)) / Math.max(1, count)

    Repeater {
        model: root.style === 2 ? 0 : root.count
        Rectangle {
            required property int index
            readonly property real level: root.levels[index] ?? 0
            readonly property real barHeight: Math.max(3, level * root.height)
            x: index * (root.barWidth + root.gap)
            width: root.barWidth
            height: barHeight
            y: root.style === 1 ? (root.height - barHeight) / 2 : root.height - barHeight
            radius: Math.min(root.barWidth / 2, root.radius)
            color: root.color
            opacity: 0.35 + 0.65 * level
        }
    }

    Shape {
        id: line
        anchors.fill: parent
        visible: root.style === 2 && root.count > 1
        preferredRendererType: Shape.CurveRenderer

        readonly property real step: root.width / Math.max(1, root.count - 1)
        // The outline and the faint fill below it share one set of points.
        readonly property var points: {
            const pts = []
            for (let i = 0; i < root.count; ++i)
                pts.push(Qt.point(i * step, root.height - root.levels[i] * root.height))
            return pts
        }

        ShapePath {
            fillColor: "transparent"
            strokeColor: root.color
            strokeWidth: 3
            joinStyle: ShapePath.RoundJoin
            PathPolyline { path: line.points }
        }
        ShapePath {
            fillColor: Qt.alpha(root.color, 0.15)
            strokeColor: "transparent"
            PathPolyline { path: line.points.concat([Qt.point(root.width, root.height), Qt.point(0, root.height)]) }
        }
    }
}
