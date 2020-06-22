import comdb2
import Foundation

import func Darwin.exit

// mutating func write(_ string: String)

struct StderrWriter: TextOutputStream {
    public func write(_ str: String) {
        if let data = str.data(using: String.Encoding.utf8) {
            FileHandle.standardError.write(data)
        }
    }
}
var stderr: StderrWriter = StderrWriter()

do {
    let args = CommandLine.arguments
    if args.count != 4 {
        print("Usage: dbname tier query", to: &stderr)
        exit(0)
    }
    let dbName = args[1]
    let tier = args[2]
    let sql = args[3]

    let db = try Comdb2(dbName: dbName, tier: tier)
    let rows = try db.run(sql: sql)
    for row in rows {
        if case let Comdb2Row.error(error: rc) = row {
            print("rc \(rc.errorCode) \(rc.errorMessage)", to: &stderr)
        }
        else if case let Comdb2Row.values(columnValues: row) = row {
            var sep = ""
            var columnNum = 0
            print("(", terminator: "")
            for col in row {
                print("\(sep)\(rows.columnNames[columnNum])=", terminator: "")
                print("\(col!)", terminator: "")
                sep = ", "
                columnNum += 1
            }
            print(")")
        }
    }
}
catch let error {
    print("error: \(error)")
}
