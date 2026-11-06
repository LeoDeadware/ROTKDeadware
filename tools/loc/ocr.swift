// Text on a screenshot via the Vision framework (Ukrainian + English), one line per text box.
//
//   swiftc -O -o ocr tools/loc/ocr.swift && ./ocr frame.png [frame2.png ...]
//
// Output: "<file>\t<text>" per recognized line. Used by tools/loc/tests/ua_menu.sh to check
// that the menus show the Ukrainian strings (and not the English ones).
import AppKit
import Foundation
import Vision

let args = CommandLine.arguments.dropFirst()
guard !args.isEmpty else {
  FileHandle.standardError.write("usage: ocr image.png...\n".data(using: .utf8)!)
  exit(2)
}
var bad = 0
for path in args {
  guard let img = NSImage(contentsOfFile: path),
        let cg = img.cgImage(forProposedRect: nil, context: nil, hints: nil) else {
    print("\(path)\t<unreadable>"); bad += 1; continue
  }
  // The game draws at 640x480: upscale so small menu text is large enough for Vision.
  let scale = 3
  let w = cg.width * scale, h = cg.height * scale
  guard let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0,
                            space: CGColorSpaceCreateDeviceRGB(),
                            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { exit(1) }
  ctx.interpolationQuality = .high
  ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
  let big = ctx.makeImage()!
  let req = VNRecognizeTextRequest()
  req.recognitionLevel = .accurate
  req.recognitionLanguages = ["uk-UA", "en-US"]
  req.usesLanguageCorrection = false
  do { try VNImageRequestHandler(cgImage: big, options: [:]).perform([req]) } catch {
    print("\(path)\t<error \(error)>"); bad += 1; continue
  }
  for o in req.results ?? [] {
    // Third column: the line's top and bottom, 0..1 from the top of the image.
    if let t = o.topCandidates(1).first {
      let b = o.boundingBox
      print("\(path)\t\(t.string)\t\(String(format: "%.3f-%.3f", 1 - b.maxY, 1 - b.minY))")
    }
  }
}
exit(bad == 0 ? 0 : 1)
