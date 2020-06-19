// swift-tools-version:5.2
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "comdb2test",
    dependencies: [
        .package(path: "/Users/mike/comdb2/src/contrib/swift/comdb2"),
    ],
    targets: [
        .target(
            name: "comdb2test",
            dependencies: [ "comdb2" ])
    ]
)
