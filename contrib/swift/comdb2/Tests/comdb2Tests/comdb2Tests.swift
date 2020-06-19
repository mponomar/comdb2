import XCTest
@testable import comdb2

final class comdb2Tests: XCTestCase {
    func testExample() {
        // This is an example of a functional test case.
        // Use XCTAssert and related functions to verify your tests produce the correct
        // results.
        XCTAssertEqual(comdb2().text, "Hello, World!")
    }

    static var allTests = [
        ("testExample", testExample),
    ]
}
