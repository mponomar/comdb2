import XCTest

import test2Tests

var tests = [XCTestCaseEntry]()
tests += test2Tests.allTests()
XCTMain(tests)
