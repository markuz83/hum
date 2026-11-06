// Run with: node --test omarchy-plugin/tests
const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const path = require("node:path")

// Protocol.js starts with QML's ".pragma library", which isn't JavaScript, so
// the header is dropped before the file is loaded as a module.
const source = fs.readFileSync(path.join(__dirname, "../Protocol.js"), "utf8").replace(/^\.pragma library\n/, "")
const loaded = { exports: {} }
new Function("module", source)(loaded)
const { parse, resample, MAX_BARS, MAX_TEXT } = loaded.exports

test("levels are whole numbers clamped to 0..99", () => {
  assert.deepStrictEqual(parse("L 0 50 99 150 -4 abc"), { type: "levels", values: [0, 50, 99, 99, 0, 0] })
})

test("a flood of values is cut off", () => {
  const line = "L " + new Array(5000).fill(7).join(" ")
  assert.strictEqual(parse(line).values.length, MAX_BARS)
})

test("playback state falls back to Stopped for anything unknown", () => {
  assert.strictEqual(parse("S Playing").value, "Playing")
  assert.strictEqual(parse("S Paused").value, "Paused")
  assert.strictEqual(parse("S Exploding").value, "Stopped")
  assert.strictEqual(parse("S").value, "Stopped")
})

test("a track keeps markup-shaped text literally and bounds its length", () => {
  const evil = '<img src="https://example.invalid/tracker"><b>x</b>'
  const track = parse("N " + evil + "\t" + "a".repeat(1000))
  assert.strictEqual(track.title, evil)  // stored as text; the tooltip draws plain text
  assert.strictEqual(track.artist.length, MAX_TEXT)
})

test("a track without an artist, and no track at all", () => {
  assert.deepStrictEqual(parse("N Just A Title"), { type: "track", title: "Just A Title", artist: "" })
  assert.deepStrictEqual(parse("N"), { type: "track", title: "", artist: "" })
})

test("unknown lines are ignored", () => {
  assert.strictEqual(parse("hello"), null)
  assert.strictEqual(parse(""), null)
})

test("resample keeps the loudest band down and repeats up", () => {
  assert.deepStrictEqual(resample([10, 30, 50, 70], 2), [30, 70])
  assert.deepStrictEqual(resample([10, 20], 4), [10, 10, 20, 20])
  assert.deepStrictEqual(resample([], 4), [])
})
