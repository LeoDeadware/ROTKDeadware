// Presses a key or clicks in another app's window the way a user would (tests/scripts/launcher_play.sh):
//   press <pid> key <keycode>           CGEvent key down/up posted to that process (36 = Return)
//   press <pid> click <x> <y>           left click at a window point (points from the top-left; negative =
//                                       from the right / bottom edge)
//   press <pid> wait-window <seconds>   exit 0 once the process shows an on-screen window
import CoreGraphics
import Foundation

let a = CommandLine.arguments
guard a.count >= 3, let pid = pid_t(a[1]) else { print("usage: press <pid> key|click|wait-window ..."); exit(2) }

func window() -> CGRect? {
  let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly], kCGNullWindowID) as? [[String: Any]] ?? []
  for w in list where (w[kCGWindowOwnerPID as String] as? Int32) == pid && ((w[kCGWindowLayer as String] as? Int) ?? 0) < 100 {  // a modal window sits at layer 8
    if let b = w[kCGWindowBounds as String] as? [String: CGFloat], let r = CGRect(dictionaryRepresentation: b as CFDictionary),
       r.width > 100 { return r }
  }
  return nil
}

switch a[2] {
case "wait-window":
  let until = Date().addingTimeInterval(Double(a.count > 3 ? a[3] : "20") ?? 20)
  while Date() < until { if let r = window() { print("window \(Int(r.width))x\(Int(r.height))"); exit(0) }; usleep(200_000) }
  print("no window"); exit(1)
case "key":
  let code = CGKeyCode(a.count > 3 ? UInt16(a[3]) ?? 36 : 36)
  let src = CGEventSource(stateID: .hidSystemState)
  for down in [true, false] { CGEvent(keyboardEventSource: src, virtualKey: code, keyDown: down)?.postToPid(pid); usleep(50_000) }
case "click":
  guard let r = window(), a.count > 4, let x = Double(a[3]), let y = Double(a[4]) else { print("no window"); exit(1) }
  let p = CGPoint(x: x < 0 ? r.maxX + x : r.minX + x, y: y < 0 ? r.maxY + y : r.minY + y)
  for t in [CGEventType.leftMouseDown, .leftMouseUp] {
    CGEvent(mouseEventSource: nil, mouseType: t, mouseCursorPosition: p, mouseButton: .left)?.post(tap: .cghidEventTap)
    usleep(60_000)
  }
  print("clicked \(Int(p.x)),\(Int(p.y))")
default: print("unknown action"); exit(2)
}
