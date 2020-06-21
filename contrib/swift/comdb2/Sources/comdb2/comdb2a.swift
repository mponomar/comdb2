import cdb2api

public struct Comdb2aRow {
    public let columnValues: [Any?]
}

public struct Comdb2aError : Error {
    // todo enum?
    public let errorCode: Int
    public let errorMessage: String

    public init(errorCode: Int, errorMessage: String) {
        self.errorCode = errorCode
        self.errorMessage = errorMessage
    }
}

public class Comdb2aRows: IteratorProtocol, Sequence {
    private(set) var db: Comdb2a
    public private(set) var columnNames: [String]
    private var columnValues: [Any?] = []

    public func next() -> Comdb2aRow? {
        columnValues.removeAll(keepingCapacity: true)
        let rc = cdb2_next_record(db.db)
        if rc != 0 {
            return nil // TODO: throw?
        }
        else {
            // throw Comdb2aError(errorCode: Int(rc), errorMessage: String(cString: cdb2_errstr(db.db)))
            for i in 0 ..< cdb2_numcolumns(db.db) {
                let opaqueValue = cdb2_column_value(db.db, i)
                let type:UInt32 = UInt32(cdb2_column_type(db.db, i))
                if opaqueValue == nil {
                    columnValues.append(nil)
                    continue
                }
                switch type {
                    case CDB2_INTEGER.rawValue:
                        columnValues.append(opaqueValue!.load(fromByteOffset: 0, as: Int64.self))

                    case CDB2_CSTRING.rawValue:
                        let ptr: UnsafeMutablePointer<Int8> = opaqueValue!.assumingMemoryBound(to: Int8.self)
                        columnValues.append(String(cString: ptr))
                    default:
                            // TODO: throw?
                        return nil

                }
            }
            return Comdb2aRow(columnValues: columnValues)
        }
    }

    fileprivate init(db: Comdb2a, columnNames: [String]) {
        self.db = db
        self.columnNames = columnNames
    } 
}

public class Comdb2a {
    fileprivate var db: OpaquePointer
    public init(dbName: String, tier: String = "default") throws {
        var dbp: OpaquePointer?
        let rc = cdb2_open(&dbp, dbName, tier, 0)
        db = dbp!
        if rc != 0 {
            let err = Comdb2aError(errorCode: Int(rc), errorMessage: String(cString: cdb2_errstr(db)))
            cdb2_close(db)
            throw err
        }
    }

    public func run(sql: String, params: [String : Any]? = nil) throws -> Comdb2aRows {
        let rc = cdb2_run_statement(db, sql)
        if rc != 0 {
            throw Comdb2aError(errorCode: Int(rc), errorMessage: String(cString: cdb2_errstr(db)))
        }
        var colNames: [String] = []
        for columnNumber in 0 ..< cdb2_numcolumns(db) {
            colNames.append(String(cString: cdb2_column_name(db, columnNumber)))
        }
        return Comdb2aRows(db: self, columnNames: colNames)
    }
}
