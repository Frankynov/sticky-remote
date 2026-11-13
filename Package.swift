// swift-tools-version: 6.0
// The firmware's UI code (firmware/remote_ui.h) as a library for desktop
// builds, behind a C interface (engine/core/include/sticky_core.h). Used by
// the Mac simulator; the command-line build is engine/build.sh.
import PackageDescription

let package = Package(
    name: "sticky-remote",
    platforms: [.macOS(.v15)],
    products: [
        .library(name: "StickyCore", targets: ["StickyCore"]),
    ],
    targets: [
        .target(
            name: "StickyCore",
            path: "engine/core",
            publicHeadersPath: "include",
            cxxSettings: [
                .headerSearchPath("../../firmware"),   // remote_ui.h
                .headerSearchPath("../stubs"),         // stand-ins for the ESPHome/ESP-IDF headers
            ]
        ),
    ],
    cxxLanguageStandard: .cxx17
)
