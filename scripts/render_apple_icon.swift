import AppKit
import ImageIO
import UniformTypeIdentifiers

// Rasterize the repository's vector icon onto an opaque background for Apple's asset catalog.
guard CommandLine.arguments.count == 3,
      let artwork = NSImage(contentsOfFile: CommandLine.arguments[1]),
      let context = CGContext(data: nil, width: 1024, height: 1024, bitsPerComponent: 8,
                              bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(),
                              bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else {
    fatalError("Usage: swift render_apple_icon.swift source.svg destination.png")
}
context.setFillColor(CGColor(colorSpace: CGColorSpaceCreateDeviceRGB(),
                            components: [20.0 / 255, 22.0 / 255, 28.0 / 255, 1])!)
context.fill(CGRect(x: 0, y: 0, width: 1024, height: 1024))
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(cgContext: context, flipped: false)
artwork.draw(in: NSRect(x: 0, y: 0, width: 1024, height: 1024))
NSGraphicsContext.restoreGraphicsState()
let output = URL(fileURLWithPath: CommandLine.arguments[2])
guard let image = context.makeImage(),
      let destination = CGImageDestinationCreateWithURL(output as CFURL, UTType.png.identifier as CFString, 1, nil) else {
    fatalError("Cannot create icon PNG")
}
CGImageDestinationAddImage(destination, image, nil)
guard CGImageDestinationFinalize(destination) else { fatalError("Cannot write icon PNG") }
