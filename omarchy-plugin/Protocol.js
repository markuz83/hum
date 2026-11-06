.pragma library

// What Hum says over its bar socket, one line at a time (see src/barlink.h):
//   S Playing|Paused|Stopped      N <title> TAB <artist>      L <n> <n> ...
// The lines come from another program and the title from a file's tags, so
// everything is bounded and nothing is ever interpreted, only stored.

var MAX_BARS = 64
var MAX_TEXT = 200

function parse(line) {
  line = String(line)
  var kind = line.charAt(0)
  var rest = line.charAt(1) === " " ? line.slice(2) : ""

  if (kind === "L") {
    var fields = rest.split(" ", MAX_BARS)
    var values = []
    for (var i = 0; i < fields.length; i++) {
      var n = parseInt(fields[i], 10)
      values.push(isNaN(n) ? 0 : Math.max(0, Math.min(99, n)))
    }
    return { type: "levels", values: values }
  }
  if (kind === "S") {
    var state = rest === "Playing" || rest === "Paused" ? rest : "Stopped"
    return { type: "state", value: state }
  }
  if (kind === "N") {
    var parts = rest.split("\t")
    return { type: "track", title: parts[0].slice(0, MAX_TEXT), artist: (parts[1] || "").slice(0, MAX_TEXT) }
  }
  return null
}

// Pools the spectrum down (or repeats it up) to the number of bars drawn. A bar
// takes the loudest of its bands, not their average, so peaks stay visible.
function resample(values, count) {
  var out = []
  if (!values || values.length === 0 || count <= 0) return out
  for (var bar = 0; bar < count; bar++) {
    var from = Math.floor(bar * values.length / count)
    var to = Math.max(from + 1, Math.floor((bar + 1) * values.length / count))
    var peak = 0
    for (var i = from; i < to && i < values.length; i++) peak = Math.max(peak, values[i])
    out.push(peak)
  }
  return out
}

if (typeof module !== "undefined") module.exports = { parse: parse, resample: resample, MAX_BARS: MAX_BARS, MAX_TEXT: MAX_TEXT }
