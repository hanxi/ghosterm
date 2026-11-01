// Manual macOS Kitty graphics probe; run via scripts/test-kitty-macos.py.
// Uses production TerminalSurface in an offscreen NSWindow, not the full app/PTY.
import AppKit
import GhostermTerminal
import Foundation

_ = NSApplication.shared
NSApplication.shared.setActivationPolicy(.accessory)
let directory = URL(fileURLWithPath: ProcessInfo.processInfo.environment["PROBE_OUT"]!)
let app = try TerminalApp.shared()
let surface = try TerminalSurface(app: app, frame: NSRect(x: 0, y: 0, width: 480, height: 280))
let window = NSWindow(contentRect: NSRect(x: 100, y: 100, width: 480, height: 280), styleMask: [.titled], backing: .buffered, defer: false)
window.contentView = surface.view
window.setFrameOrigin(NSPoint(x: -30000, y: -30000))
window.orderFrontRegardless()
surface.setSize(width: 960, height: 560)
let lock = NSLock()
var reply = Data()
surface.onInput = { bytes in lock.lock(); reply.append(bytes); lock.unlock() }
func replies() -> String { lock.lock(); defer { lock.unlock() }; return String(decoding: reply, as: UTF8.self) }
let io = DispatchQueue(label: "kitty-probe-output")
func feed(_ text: String) { io.sync { surface.feed(Data(text.utf8)) } }
func wait(_ seconds: Double = 0.6) {
    let end = Date().addingTimeInterval(seconds)
    while Date() < end { app.tick(); surface.draw(); RunLoop.main.run(until: Date().addingTimeInterval(0.02)) }
}
var failures = 0
func check(_ value: Bool, _ name: String) { print("\(value ? "PASS" : "FAIL") \(name)"); if !value { failures += 1 } }
func capture(_ name: String) throws -> (red: Int, green: Int) {
    guard let rep = surface.view.bitmapImageRepForCachingDisplay(in: surface.view.bounds) else { fatalError("capture unavailable") }
    surface.view.cacheDisplay(in: surface.view.bounds, to: rep)
    guard let png = rep.representation(using: .png, properties: [:]), let cg = rep.cgImage else { fatalError("empty capture") }
    try png.write(to: directory.appendingPathComponent(name + ".png"))
    let w = cg.width, h = cg.height
    var pixels = [UInt8](repeating: 0, count: w*h*4)
    pixels.withUnsafeMutableBytes { p in
        let ctx = CGContext(data: p.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w*4, space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
    }
    var red = 0, green = 0
    for i in stride(from: 0, to: pixels.count, by: 4) {
        if pixels[i] > 200 && pixels[i+1] < 70 && pixels[i+2] < 70 { red += 1 }
        if pixels[i+1] > 200 && pixels[i] < 70 && pixels[i+2] < 70 { green += 1 }
    }
    print("PIXELS \(name) size=\(w)x\(h) red=\(red) green=\(green)")
    return (red, green)
}
func command(_ control: String, _ data: String = "") -> String { "\u{1b}_G\(control);\(data)\u{1b}\\" }
func graphic(_ control: String, _ data: String = "") { feed(command(control, data)) }
feed("\u{1b}[2J\u{1b}[HKitty graphics / macOS Metal probe\r\n\r\n")
wait()
check(surface.readText()?.contains("Kitty graphics") == true, "independent text reaches grid")
graphic("a=q,i=41,f=24,s=1,v=1", "AAAA")
wait()
check(replies().contains("i=41;OK"), "Kitty graphics query acknowledged")
let baseline = try capture("00-before")
// Transmit PNG as protocol bytes; no file path access by the terminal.
let fixture = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 128, pixelsHigh: 80, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 512, bitsPerPixel: 32)!
for y in 0..<80 { for x in 0..<128 {
    let p = fixture.bitmapData!.advanced(by: y*512+x*4)
    p[0] = x < 64 ? 255 : 0; p[1] = x < 64 ? 0 : 255; p[2] = 0; p[3] = 255
} }
let png = fixture.representation(using: .png, properties: [:])!
let b64 = Array(png.base64EncodedString())
var offset = 0
while offset < b64.count {
    let end = min(offset + 128, b64.count)
    let more = end < b64.count ? 1 : 0
    graphic(offset == 0 ? "a=T,f=100,i=42,c=24,r=6,m=\(more)" : "m=\(more)", String(b64[offset..<end]))
    offset = end
}
wait(1)
check(replies().contains("i=42;OK"), "chunked PNG transmit/display acknowledged")
let displayed = try capture("01-png-displayed")
check(displayed.red > baseline.red + 1000 && displayed.green > baseline.green + 1000, "both PNG color regions rendered by Metal")
graphic("a=d,d=I,i=42")
wait()
let deleted = try capture("02-deleted")
check(deleted.red < displayed.red/10 && deleted.green < displayed.green/10 && displayed.red > 1000, "deleting image removes its pixels")
graphic("a=T,f=100,i=43", Data("not a PNG".utf8).base64EncodedString())
wait()
check(replies().contains("i=43;EINVAL"), "invalid PNG rejected explicitly")

feed("\u{1b}[3;1H")
graphic("a=T,f=24,i=44,s=1,v=1,c=24,r=6", "/wAA")
graphic("a=f,f=24,i=44,s=1,v=1,z=200", "AP8A")
wait()
check(replies().contains("i=44,r=2;OK"), "animation frame accepted")
graphic("a=a,i=44,s=1,c=1")
wait()
let firstFrame = try capture("03-animation-red")
graphic("a=a,i=44,s=1,c=2")
wait()
let secondFrame = try capture("04-animation-green")
check(firstFrame.red > 1000 && firstFrame.green == 0 && secondFrame.green > 1000 && secondFrame.red == 0, "animation frame selection changes rendered pixels")
graphic("a=a,i=44,r=1,z=200")
graphic("a=a,i=44,s=3,v=1")
var sawRed = false, sawGreen = false
for index in 0..<10 {
    wait(0.11)
    let frame = try capture("05-playback-\(index)")
    sawRed = sawRed || frame.red > 1000
    sawGreen = sawGreen || frame.green > 1000
}
check(sawRed && sawGreen, "automatic animation playback renders both frames")
graphic("a=a,i=44,s=1")

print("REPLIES \(replies().debugDescription)")
print("RESULT failures=\(failures)")
window.orderOut(nil)
exit(failures == 0 ? 0 : 1)
