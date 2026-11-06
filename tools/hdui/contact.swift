// contact: a contact sheet of screenshots (one look per hypothesis).
//   swiftc -O -o contact tools/hdui/contact.swift
//   contact out.png [--cols N] [--width W] [--crop x0,y0,x1,y1] img.png[=label] ...
// Each image is cropped (fractions 0..1 of its size, optional), scaled to
// W pixels wide (default 480) and placed in a grid of N columns (default 3)
// with its label (default: the file name) under it.
import CoreGraphics
import CoreText
import Foundation
import ImageIO

var cols = 3, width = 480
var crop = CGRect(x: 0, y: 0, width: 1, height: 1)
var items: [(String, String)] = []
var args = Array(CommandLine.arguments.dropFirst())
guard !args.isEmpty else {
  FileHandle.standardError.write("usage: contact out.png [--cols N] [--width W] [--crop x0,y0,x1,y1] img[=label]...\n".data(using: .utf8)!)
  exit(2)
}
let out = args.removeFirst()
while !args.isEmpty {
  let a = args.removeFirst()
  if a == "--cols" { cols = Int(args.removeFirst())! }
  else if a == "--width" { width = Int(args.removeFirst())! }
  else if a == "--crop" {
    let v = args.removeFirst().split(separator: ",").map { CGFloat(Double($0)!) }
    crop = CGRect(x: v[0], y: v[1], width: v[2] - v[0], height: v[3] - v[1])
  } else if let eq = a.firstIndex(of: "=") {
    items.append((String(a[..<eq]), String(a[a.index(after: eq)...])))
  } else {
    items.append((a, (a as NSString).lastPathComponent))
  }
}

func load(_ p: String) -> CGImage? {
  guard let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: p) as CFURL, nil) else { return nil }
  return CGImageSourceCreateImageAtIndex(src, 0, nil)
}

let imgs: [(CGImage, String)] = items.compactMap { p, l in
  guard let i = load(p) else { FileHandle.standardError.write("cannot read \(p)\n".data(using: .utf8)!); return nil }
  let r = CGRect(x: crop.minX * CGFloat(i.width), y: crop.minY * CGFloat(i.height),
                 width: crop.width * CGFloat(i.width), height: crop.height * CGFloat(i.height)).integral
  return (i.cropping(to: r) ?? i, l)
}
guard let first = imgs.first else { exit(1) }
let cellH = Int(Double(width) * Double(first.0.height) / Double(first.0.width))
let label = 22, rows = (imgs.count + cols - 1) / cols
let W = cols * width, H = rows * (cellH + label)
let cs = CGColorSpace(name: CGColorSpace.sRGB)!
let ctx = CGContext(data: nil, width: W, height: H, bitsPerComponent: 8, bytesPerRow: 0, space: cs,
                    bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
ctx.setFillColor(CGColor(red: 0.1, green: 0.1, blue: 0.1, alpha: 1))
ctx.fill(CGRect(x: 0, y: 0, width: W, height: H))
ctx.interpolationQuality = .high
let font = CTFontCreateWithName("Menlo" as CFString, 14, nil)
for (n, (img, text)) in imgs.enumerated() {
  let x = (n % cols) * width, top = (n / cols) * (cellH + label)
  let y = H - top - cellH
  ctx.draw(img, in: CGRect(x: x, y: y, width: width, height: cellH))
  let attr = NSAttributedString(string: text, attributes: [
    NSAttributedString.Key(kCTFontAttributeName as String): font,
    NSAttributedString.Key(kCTForegroundColorAttributeName as String): CGColor(red: 1, green: 1, blue: 1, alpha: 1)])
  let line = CTLineCreateWithAttributedString(attr)
  ctx.textPosition = CGPoint(x: x + 4, y: y - 16)
  CTLineDraw(line, ctx)
}
let dst = CGImageDestinationCreateWithURL(URL(fileURLWithPath: out) as CFURL, "public.png" as CFString, 1, nil)!
CGImageDestinationAddImage(dst, ctx.makeImage()!, nil)
exit(CGImageDestinationFinalize(dst) ? 0 : 1)
